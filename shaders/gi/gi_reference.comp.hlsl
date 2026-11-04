// Reference path tracer for the ray-traced GI (gi.reference, src/enh/gi.cpp, docs/research/path-tracing.md): the ground truth the
// DDGI probes + screen probes are measured against. Per pixel of the main view (primary hit from the z pre-pass depth, as the screen
// probes), progressively accumulated while the camera / light stand still:
//
//   * indirect: cosine-weighted paths with next-event estimation at every hit (the sun with a shadow ray, one of the frame's point
//     lights with a shadow ray, emissive surfaces), Russian roulette from the third bounce, the sky (the engine's outdoor ambient
//     SH) on a miss. Same inputs as the DDGI rays (gi_shade.hlsli): the RT scene's materials, the same lights, the same hit
//     shading (albedo^2.2 x light, gi.emissive) -- but no probes (the second bounce on is traced), no ray reach, no cap.
//   * direct: the sun + a point light with shadow rays at the primary hit (what the engine shows with shadow maps).
//   * the DDGI's own irradiance at the pixel (the volume lookup) and with the screen probes' factor, to compare in the same units.
//
// Units: "acc" -- the engine's lighting sum; its image is albedo x pow(acc, 0.45) (gi_shade.hlsli). The indirect value is the mean
// radiance of cosine-weighted rays = the irradiance / pi that the probes' registers evaluate to (rvkGiEvalRegs).
//
// Planes (plane-major float4 buffer, pixel = y * width + x): 0 reference indirect (rgb mean, a mean luma^2: the variance),
// 1 reference direct (rgb), 2 DDGI volume irradiance (a cascade), 3 DDGI + screen probes (a 1 = the probes' factor applied),
// 4 primary albedo (gamma encoded, as the engine's textures; a view depth), 5 normal (xyz, a 1 = valid pixel).
// Also writes the view image (gi.reference view: albedo x pow(acc, 0.45), or a heat map).

#include "rvk_common.hlsli"
#include "rvk_rt_trace.hlsli"
#include "rvk_gi.hlsli"
#include "gi_common.hlsli"
#include "gi_shade.hlsli"
#include "screen_common.hlsli"

// mirror: enh::GiRefParams (in the push block's pad address)
struct GiRefParamsData {
    uint4 buf;   // xy accumulation buffer (RW), z samples this frame, w samples accumulated before
    uint4 img;   // x resolved screen probe image (bindless t2D, 0 = none), y view mode, z max bounces, w seed
    uint4 size;  // x width, y height, z bit 0: the epoch's first frame (primary albedo), w 0
};
typedef RVK_REF(GiRefParamsData) GiRefParams;

[[vk::image_format("rgba16f")]] RWTexture2D<float4> outView : register(u0, space1);

static uint gRng;

float rnd() {
    gRng = gRng * 747796405u + 2891336453u;
    uint w = ((gRng >> ((gRng >> 28u) + 4u)) ^ gRng) * 277803737u;
    return float((w >> 22u) ^ w) * (1.0 / 4294967296.0);
}

float3 cosineDir(float3 n) {
    float3 t = normalize(abs(n.y) < 0.99 ? cross(n, float3(0.0, 1.0, 0.0)) : cross(n, float3(1.0, 0.0, 0.0)));
    float3 b = cross(n, t);
    float u1 = rnd(), u2 = rnd();
    float r = sqrt(u1), phi = 6.2831853 * u2;
    return normalize(t * (r * cos(phi)) + b * (r * sin(phi)) + n * sqrt(max(0.0, 1.0 - u1)));
}

float3 albedoLinear(RvkRtHitSurface s, float scale) {
    float3 albedo = s.albedo.rgb;
    if ((s.uvSet & RVK_RT_UV_NO_TEXTURE) != 0u) {
        albedo = (s.uvSet & RVK_RT_UV_HEURISTIC) != 0u ? float3(0.2, 0.2, 0.2) : min(albedo, float3(0.5, 0.5, 0.5));
    }
    return pow(clamp(albedo, float3(0.0, 0.0, 0.0), float3(1.0, 1.0, 1.0)), float3(2.2, 2.2, 2.2)) * scale;
}

// the light arriving at a surface point from the sun and one of the frame's point lights (shadow rays): acc units
float3 directLight(GiPassParams p, RvkFrame f, float3 P, float3 N, float3 G) {
    float3 E = float3(0.0, 0.0, 0.0);
    float3 L = p.Get().sunDir.xyz;
    float nl = clamp(dot(L, N), 0.0, 1.0) * clamp(dot(L, G) * 4.0, 0.0, 1.0);
    if (nl > 0.0 && dot(p.Get().sunColor.rgb, float3(1.0, 1.0, 1.0)) > 1e-3) {
        float vis = rvkRtVisibility(f, P + G * 2.0, L, 1.0, p.Get().shade.z, 0.0, 0.0, RVK_RT_TRACE_OPAQUE);
        E += p.Get().sunColor.rgb * (nl * vis);
    }
    E += giPointLights(p, f, P, N, G, rnd());
    return E;
}

[numthreads(8, 8, 1)]
void CSMain(uint3 id : SV_DispatchThreadID) {
    GiPassParams p = giParams();
    GiRefParams r = rvkPtr<GiRefParamsData, 16>(pc.pad);
    RvkFrame f = rvkLoadFrame(pc.frame);
    uint w = r.Get().size.x, h = r.Get().size.y;
    if (id.x >= w || id.y >= h) return;
    int2 px = int2(id.xy);
    uint pix = id.y * w + id.x;
    uint planeStride = w * h * 16u;
    RvkAddr base = rvkAddr(r.Get().buf.xy);
    // (a plane's pixel)
    #define PLANE(k) rvkAddrAdd(base, uint(k) * planeStride + pix * 16u)

    float zHw = rvkFetch2D(p.Get().screen.x, px).r;
    float z = rvkHwDepthToD3D(f, zHw);
    float viewZ = spViewDepth(f, z);
    if (viewZ <= 0.0) {
        for (uint k = 0u; k < 6u; ++k) rvkRwStore<float4, 16>(PLANE(k), float4(0.0, 0.0, 0.0, 0.0));
        outView[px] = float4(0.0, 0.0, 0.0, 1.0);
        return;
    }
    float2 size = float2(w, h);
    float3 P = spWorldPos(f, px, size, z);
    float3 N = spDepthNormal(f, px, size, p.Get().screen.x);
    float3 V = normalize(f.cameraPos.xyz - P);
    float spread = p.Get().shade.w;
    uint traceFlags = p.Get().flags.x;

    // ---- the DDGI's irradiance here
    RvkGiVolume v = rvkPtr<RvkGiVolumeData, 16>(p.Get().volume.xy);
    float4 regs[7];
    uint cascade = 0u;
    float3 ddgi = float3(0.0, 0.0, 0.0);
    float3 ddgiScr = float3(0.0, 0.0, 0.0);
    float scrFlag = 0.0;
    if (rvkGiLookupVolume(v, P, N, V, regs, cascade)) {
        ddgi = max(rvkGiEvalRegs(regs, N), float3(0.0, 0.0, 0.0));
        ddgiScr = ddgi;
        uint sg = r.Get().img.x;
        if (sg != 0u) {
            float4 s = rvkFetch2D(sg, px);
            if (s.a > 0.0 && abs(s.a - viewZ) < max(viewZ * 0.02, 4.0)) {
                ddgiScr = ddgi * clamp(s.rgb, float3(0.0, 0.0, 0.0), float3(3.2, 3.2, 3.2));
                scrFlag = 1.0;
            }
        }
    }
    rvkRwStore<float4, 16>(PLANE(2), float4(ddgi, float(cascade)));
    rvkRwStore<float4, 16>(PLANE(3), float4(ddgiScr, scrFlag));
    rvkRwStore<float4, 16>(PLANE(5), float4(N, 1.0));

    // ---- the primary albedo (a camera ray to the pixel's depth), once per epoch
    float3 alb = rvkRwLoad<float4, 16>(PLANE(4)).rgb;
    if ((r.Get().size.z & 1u) != 0u) {
        float3 d = P - f.cameraPos.xyz;
        float dist = length(d);
        float3 dir = d / max(dist, 1e-3);
        float cone = 2.0 / (abs(f.proj[1][1]) * size.y);
        RvkRtHit hp = rvkRtTraceClosest(f, f.cameraPos.xyz, dir, 1.0, dist * 1.05 + 20.0, 0.0, cone, 0u);
        alb = float3(0.5, 0.5, 0.5);
        if (hp.hit && abs(hp.t - dist) < max(dist * 0.03, 8.0)) {
            RvkRtHitSurface s = rvkRtHitSurface(f, hp, dir, 0.0, cone);
            alb = s.albedo.rgb;
            if ((s.uvSet & RVK_RT_UV_NO_TEXTURE) != 0u) {
                alb = (s.uvSet & RVK_RT_UV_HEURISTIC) != 0u ? float3(0.2, 0.2, 0.2) : min(alb, float3(0.5, 0.5, 0.5));
            }
        }
        rvkRwStore<float4, 16>(PLANE(4), float4(alb, viewZ));
    }

    // ---- the reference: this frame's samples, merged into the running means
    uint spp = r.Get().buf.z, before = r.Get().buf.w;
    float4 ind = rvkRwLoad<float4, 16>(PLANE(0));
    float4 dir0 = rvkRwLoad<float4, 16>(PLANE(1));
    if (spp > 0u) {
        gRng = giHash(pix * 2654435761u ^ r.Get().img.w);
        gRng = giHash(gRng + uint(id.x) * 9781u + uint(id.y) * 6271u);
        float3 sumInd = float3(0.0, 0.0, 0.0), sumDir = float3(0.0, 0.0, 0.0);
        float sumL2 = 0.0;
        const float3 lw = float3(0.2126, 0.7152, 0.0722);
        float3 origin0 = P + N * 3.0 + V * 2.0;
        for (uint s = 0u; s < spp; ++s) {
            sumDir += directLight(p, f, P + N * 1.0, N, N);
            // one indirect path
            float3 dir = cosineDir(N);
            float3 origin = origin0;
            float3 tp = float3(1.0, 1.0, 1.0), rad = float3(0.0, 0.0, 0.0);
            for (uint b = 0u; b < r.Get().img.z; ++b) {
                RvkRtHit h = rvkRtTraceClosest(f, origin, dir, 1.0, 1.0e6, 0.0, spread, traceFlags);
                if (!h.hit) {
                    rad += tp * rvkGiSkyRadiance(p.Get().sky, dir);
                    break;
                }
                RvkRtHitSurface hs = rvkRtHitSurface(f, h, dir, 0.0, spread);
                float3 lin = albedoLinear(hs, p.Get().shade.y);
                float3 Nh = normalize(hs.normal), Gh = hs.geometricNormal;
                float3 emission = float3(0.0, 0.0, 0.0);
                float gain = asfloat(p.Get().flags.w);
                if ((hs.uvSet & RVK_RT_UV_EMISSIVE) != 0u && gain > 0.0) {
                    emission = pow(clamp(hs.albedo.rgb, float3(0.0, 0.0, 0.0), float3(1.0, 1.0, 1.0)), float3(2.2, 2.2, 2.2)) * gain;
                }
                rad += tp * (lin * directLight(p, f, hs.position, Nh, Gh) + emission);
                tp *= lin;
                if (b >= 2u) {
                    float q = clamp(max(tp.r, max(tp.g, tp.b)), 0.1, 0.95);
                    if (rnd() > q) break;
                    tp /= q;
                }
                dir = cosineDir(Nh);
                if (dot(dir, Gh) <= 0.0) break;
                origin = hs.position + Gh * 2.0;
            }
            if (any(isnan(rad)) || any(isinf(rad))) rad = float3(0.0, 0.0, 0.0);
            sumInd += rad;
            float l = dot(rad, lw);
            sumL2 += l * l;
        }
        float n0 = float(before), n1 = float(before + spp);
        ind = float4((ind.rgb * n0 + sumInd) / n1, (ind.a * n0 + sumL2) / n1);
        dir0 = float4((dir0.rgb * n0 + sumDir) / n1, n1);
        rvkRwStore<float4, 16>(PLANE(0), ind);
        rvkRwStore<float4, 16>(PLANE(1), dir0);
    }

    // ---- the view
    uint mode = r.Get().img.y;
    float3 outc = float3(0.0, 0.0, 0.0);
    const float3 lw = float3(0.2126, 0.7152, 0.0722);
    float3 gam = float3(0.45, 0.45, 0.45);
    if (mode == 1u) outc = alb * pow(max(dir0.rgb + ind.rgb, 0.0), gam);
    else if (mode == 2u) outc = alb * pow(max(dir0.rgb + ddgi, 0.0), gam);
    else if (mode == 3u) outc = alb * pow(max(dir0.rgb + ddgiScr, 0.0), gam);
    else if (mode == 5u) outc = pow(max(ind.rgb, 0.0), gam) * 0.5;
    else if (mode == 6u) outc = pow(max(ddgiScr, 0.0), gam) * 0.5;
    else if (mode == 4u || mode == 7u) {
        float a = dot(max(ind.rgb, 0.0), lw), b = dot(mode == 4u ? ddgiScr : ddgi, lw);
        float rel = (b - a) / max(a, 0.02);
        float m = saturate(abs(rel) / 0.5);
        outc = lerp(float3(0.3, 0.3, 0.3) * sqrt(saturate(a)), rel > 0.0 ? float3(1.0, 0.1, 0.05) : float3(0.1, 0.3, 1.0), m);
    }
    outView[px] = float4(outc, 1.0);
}
