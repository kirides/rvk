// Ray-traced water reflections (setting rt.reflections, src/enh/water.cpp, docs/research/rt-reflections.md 3.3): a compute
// pass at AfterRefractionCopy, before the water is drawn, traces the mirror ray of the water's surface plane for every
// pixel of a reflection image (half resolution at low / medium, full at high) against the frame's TLAS; water.frag
// samples that image like a planar reflection, its lookup bent by the waves and ripples (the same offset its screen-space
// trace takes: watery, broken up, no stochastic noise).
//
// The surface: the water bodies drawn in the last frames as planes (y = a x + b z + c, fitted to each body's vertices;
// the sea at its level), each within its world box; per pixel the nearest one in front of the scene (the engine's depth
// map). The hit: from the screen where it is visible there (the refraction copy = the engine's lit scene without the
// water: leaves, characters, point lights), else shaded like the engine's material (albedo x pow(ambient + sun N.L x
// shadow, 0.45), the water draw's own light constants c14..c24, the engine's fog over the camera's distance to the hit,
// rt-albedo-facts 1.3); the texture's mip from a ray cone (the footprint of an output pixel + a little roughness: sharp
// near contact, softer with distance). Coverage fades over the last 15 % of the reach.
//
// Ported from GD3D11's D3D12 WaterRT.hlsl (the user's own code): half-resolution rays, screen reuse with a depth match,
// lifted origin, a floor under dir.y, the coverage fade, Lambert sun x shadow ray + ambient, fog by distance.

#include "rvk_rt_trace.hlsli"

[[vk::image_format("rgba16f")]] RWTexture2D<float4> outImage : register(u0, space1);

static const uint WATER_RT_MAX_BODIES = 16u;

// mirror: enh::WaterRtParams (static_assert'ed size)
struct WaterRtParams {
    uint4 tex;        // x depth map (t2D), y refraction copy (t2D), z linear clamp sampler, w point clamp sampler
    uint4 info;       // x output width, y output height, z pixels per output pixel (1 / 2), w bodies
    uint4 trace;      // x RVK_RT_TRACE_* of the reflection ray, y shadow rays (0 none, 1 cut-outs solid, 2 alpha-tested), z tweaks
                      // (8 unshaded), w 1 = the source view (shaded hits: negative alpha), 2 = every hit shaded
    float4 reach;     // x reach (world units), y extra cone spread (rad), z shadow ray reach, w unused
    float4 fogColor;  // c3
    float4 fogParams; // c4 (x offset, y per unit, z maximum)
    float4 indoor;    // c14
    float4 sh[7];     // c15..c21
    float4 sunColor;  // c22
    float4 sunDir;    // c24 (towards the sun)
    float4 plane[WATER_RT_MAX_BODIES];  // y = x * a + z * b + c (xyz), w the body's outdoor factor
    float4 box[WATER_RT_MAX_BODIES];    // min x, min z, max x, max z (world)
};

struct WaterRtPush {
    RVK_PASS_PUSH_HEADER
    uint2 params;  // WaterRtParams address
    uint2 pad;
};
RVK_PUSH(WaterRtPush, pc);

float dpow(float x, float y) { return x == 0.0 ? 0.0 : exp2(y * log2(abs(x))); }
float3 dpow3(float3 v, float e) { return float3(dpow(v.r, e), dpow(v.g, e), dpow(v.b, e)); }

bool projectToUv(RvkFrame f, float3 p, out float2 uv, out float viewZ) {
    float4 c = mul(float4(p, 1.0), f.viewProj);
    viewZ = c.w;
    uv = float2(c.x / c.w * 0.5 + 0.5, 0.5 - c.y / c.w * 0.5);
    return c.w > 1.0;
}

float sceneViewDepth(WaterRtParams p, RvkFrame f, float2 uv) { return rvkSample2DLod(p.tex.x, p.tex.w, uv, 0.0).r * f.cameraPos.w; }

float3 shadeHit(WaterRtParams p, RvkFrame f, RvkRtHit h, float3 dir, float width0, float spread, float outdoor) {
    if ((p.trace.z & 8u) != 0u) return float3(0.15, 0.15, 0.15);  // tweak 8: unshaded (cost measurements)
    RvkRtHitSurface s = rvkRtHitSurface(f, h, dir, width0, spread);
    float3 albedo = s.albedo.rgb;
    if ((s.uvSet & RVK_RT_UV_NO_TEXTURE) != 0u) {
        // without a texture: the material's colour (constant diffuse), dimmed; no material at all: a dark neutral
        albedo = (s.uvSet & RVK_RT_UV_HEURISTIC) != 0u ? float3(0.2, 0.2, 0.2) : min(albedo, float3(0.5, 0.5, 0.5));
    }
    float3 N = normalize(s.normal), G = s.geometricNormal;
    float4 n1 = float4(N, 1.0);
    float4 n2 = N.yzzx * N.xyzz;
    float3 sh = float3(dot(p.sh[0], n1) + dot(p.sh[3], n2), dot(p.sh[1], n1) + dot(p.sh[4], n2), dot(p.sh[2], n1) + dot(p.sh[5], n2)) +
                p.sh[6].rgb * (N.x * N.x - N.y * N.y);
    float3 L = p.sunDir.xyz;
    float nl = clamp(dot(L, N), 0.0, 1.0) * clamp(dot(L, G) * 4.0, 0.0, 1.0);
    float shadow = 1.0;
    if (nl > 0.0 && p.trace.y != 0u && dot(p.sunColor.rgb, float3(1.0, 1.0, 1.0)) > 1e-3) {
        // towards the sun from just off the surface (cut-outs solid at medium, alpha-tested at high)
        shadow = rvkRtVisibilityOpaqueFirst(f, s.position + G * 4.0, L, 2.0, p.reach.z, 0.0, 0.004, p.trace.y == 1u ? RVK_RT_TRACE_OPAQUE : 0u);
    }
    float3 l = max(lerp(p.indoor.rgb, sh, outdoor), float3(0.0, 0.0, 0.0)) + p.sunColor.rgb * (nl * shadow);
    float3 c = albedo * dpow3(l, 0.45);
    float fog = clamp(min(length(s.position - f.cameraPos.xyz) * p.fogParams.y + p.fogParams.x, p.fogParams.z), 0.0, 1.0);
    return lerp(c, p.fogColor.rgb, fog);
}

[numthreads(8, 8, 1)]
void CSMain(uint3 gid : SV_DispatchThreadID) {
    WaterRtParams p = rvkPtr<WaterRtParams, 16>(pc.params).Get();
    RvkFrame f = rvkLoadFrame(pc.frame);
    uint2 id = gid.xy;
    if (id.x >= p.info.x || id.y >= p.info.y) return;
    float scale = float(p.info.z);
    float2 uv = (float2(id) + 0.5) * scale * f.viewport.zw;
    float3 cam = f.cameraPos.xyz;
    float3 d = normalize(rvkWorldFromViewDepth(f, uv, 1000.0) - cam);
    float sceneZ = sceneViewDepth(p, f, uv);
    bool sky = sceneZ >= f.cameraPos.w * 0.995;
    float tScene = sky ? 1e30 : length(rvkWorldFromViewDepth(f, uv, sceneZ) - cam);

    // the nearest water plane in front of the scene
    float tBest = 1e30;
    int best = -1;
    for (uint i = 0u; i < min(p.info.w, WATER_RT_MAX_BODIES); ++i) {
        float4 pl = p.plane[i];
        if (cam.y <= pl.x * cam.x + pl.y * cam.z + pl.z) continue;  // the camera under this surface: no reflection
        float den = d.y - pl.x * d.x - pl.y * d.z;
        if (den >= -1e-5) continue;
        float t = (pl.x * cam.x + pl.y * cam.z + pl.z - cam.y) / den;
        if (t <= 0.0 || t >= tBest || t > tScene + 30.0) continue;
        float3 q = cam + d * t;
        float4 b = p.box[i];
        if (q.x < b.x - 60.0 || q.z < b.y - 60.0 || q.x > b.z + 60.0 || q.z > b.w + 60.0) continue;
        tBest = t;
        best = int(i);
    }
    float4 result = float4(0.0, 0.0, 0.0, 0.0);
    if (best >= 0 && rvkRtValid(f)) {
        float4 pl = p.plane[best];
        float3 n = normalize(float3(-pl.x, 1.0, -pl.y));
        float3 P = cam + d * tBest;
        float3 dir = reflect(d, n);
        dir = normalize(float3(dir.x, max(dir.y, 0.02), dir.z));  // the ray never runs into the bed
        float3 o = P + n * 2.0;
        float pixelAngle = 2.0 / max(abs(f.proj[1][1]) * f.viewport.y, 1.0) * scale;  // one output pixel's angle
        float width0 = pixelAngle * tBest;
        float spread = pixelAngle + p.reach.y;
        float maxD = p.reach.x;
        RvkRtHit h = rvkRtTraceClosestOpaqueFirst(f, o, dir, 1.0, maxD, width0, spread, p.trace.x);
        if (h.hit) {
            float3 hp = o + dir * h.t;
            float cover = 1.0 - smoothstep(0.85 * maxD, maxD, h.t);
            // screen reuse: the hit is on screen and the depth map shows it there (not below the water's surface: the
            // refraction copy shows the scene under the water there)
            float reuse = 0.0;
            float3 reused = float3(0.0, 0.0, 0.0);
            float2 huv;
            float hz;
            float level = pl.x * hp.x + pl.y * hp.z + pl.z;
            if (hp.y > level - 3.0 && projectToUv(f, hp, huv, hz) && all(huv > float2(0.0, 0.0)) && all(huv < float2(1.0, 1.0))) {
                float2 edge = smoothstep(float2(0.0, 0.0), float2(0.03, 0.03), huv) * smoothstep(float2(0.0, 0.0), float2(0.03, 0.03), 1.0 - huv);
                float sz = sceneViewDepth(p, f, huv);
                float tol = max(30.0, hz * 0.015);
                reuse = edge.x * edge.y * (1.0 - smoothstep(tol, 2.0 * tol, abs(sz - hz)));
                if (reuse > 0.0) reused = rvkSample2DLod(p.tex.y, p.tex.z, huv, 0.0).rgb;
            }
            if (p.trace.w == 2u) reuse = 0.0;  // water.debug = reflection_shaded: every hit shaded (the exposure check)
            float3 shaded = float3(0.0, 0.0, 0.0);
            if (reuse < 1.0) shaded = shadeHit(p, f, h, dir, width0, spread, pl.w);
            float3 c = lerp(shaded, reused, reuse);
            if (!(any(isnan(c)) || any(isinf(c)))) {
                // alpha: the coverage (water.debug = reflection_source: negative where the colour was shaded)
                result = float4(c * cover, p.trace.w == 1u && reuse < 0.5 ? -cover : cover);  // premultiplied (bilinear lookups)
            }
        }
    }
    outImage[int2(id)] = result;
}
