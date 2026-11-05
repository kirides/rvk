// Ray-traced reflections of the material pack's smooth surfaces (setting materials.quality = high + materials.rt_reflections, enh/material_rt.*,
// docs/research/material-rt.md): a compute pass after the opaque scene reads the G-buffer (normal + roughness / metalness of the pack entries), and for
// every pixel whose surface is smooth enough traces one reflection ray, its direction drawn from the GGX lobe of the roughness (a mirror below 0.04),
// against the frame's TLAS. A hit is shaded from the screen where the scene shows it (the engine's lit colour: characters, point lights), else like the
// engine's material (albedo x pow(ambient + sun N.L x shadow ray, 0.45), the engine's fog); a miss reads the sky cube blurred by the roughness.
// The result is the light to ADD to the scene: radiance x the Fresnel term the raster's sky reflection used, capped by the surface's own lit brightness
// (a reflection cannot outshine the light that reaches the surface: the engine's colours stay), fogged like the surface. The filter pass accumulates it.
// Output: rgb = what to add, a = the factor the scene is multiplied by (1; puddles darken it).

#include "material_rt_common.hlsli"

[[vk::image_format("rgba16f")]] RWTexture2D<float4> outImage : register(u0, space1);

struct MaterialRtPush {
    RVK_PASS_PUSH_HEADER
    uint2 params;  // MaterialRtParams address
    uint2 pad;
};
RVK_PUSH(MaterialRtPush, pc);

bool mrtProject(RvkFrame f, float3 p, out float2 uv, out float viewZ) {
    float4 c = mul(float4(p, 1.0), f.viewProj);
    viewZ = c.w;
    uv = float2(c.x / c.w * 0.5 + 0.5, 0.5 - c.y / c.w * 0.5);
    return c.w > 1.0;
}

float3 mrtShadeHit(MaterialRtParams p, RvkFrame f, RvkRtHit h, float3 dir, float width0, float spread) {
    if ((p.trace.z & 8u) != 0u) return float3(0.15, 0.15, 0.15);
    RvkRtHitSurface s = rvkRtHitSurface(f, h, dir, width0, spread);
    float3 albedo = s.albedo.rgb;
    if ((s.uvSet & RVK_RT_UV_NO_TEXTURE) != 0u) albedo = (s.uvSet & RVK_RT_UV_HEURISTIC) != 0u ? float3(0.2, 0.2, 0.2) : min(albedo, float3(0.5, 0.5, 0.5));
    float3 N = normalize(s.normal), G = s.geometricNormal;
    float4 n1 = float4(N, 1.0);
    float4 n2 = N.yzzx * N.xyzz;
    float3 sh = float3(dot(p.sh[0], n1) + dot(p.sh[3], n2), dot(p.sh[1], n1) + dot(p.sh[4], n2), dot(p.sh[2], n1) + dot(p.sh[5], n2)) +
                p.sh[6].rgb * (N.x * N.x - N.y * N.y);
    float3 L = p.sunDir.xyz;
    float nl = clamp(dot(L, N), 0.0, 1.0) * clamp(dot(L, G) * 4.0, 0.0, 1.0);
    float shadow = 1.0;
    if (nl > 0.0 && p.trace.y != 0u && dot(p.sunColor.rgb, float3(1.0, 1.0, 1.0)) > 1e-3) {
        shadow = rvkRtVisibility(f, s.position + G * 4.0, L, 2.0, p.reach.z, 0.0, 0.004, p.trace.y == 1u ? RVK_RT_TRACE_OPAQUE : 0u);
    }
    // the hemisphere's sky ambient where the hit sees the sky (a ray straight up), the engine's indoor colour under a roof
    float skyVis = p.trace.y != 0u ? rvkRtVisibility(f, s.position + G * 4.0, float3(0.0, 1.0, 0.0), 2.0, 6000.0, 0.0, 0.004, RVK_RT_TRACE_OPAQUE) : 1.0;
    float3 l = max(lerp(p.indoor.rgb, sh, skyVis), float3(0.0, 0.0, 0.0)) + p.sunColor.rgb * (nl * shadow);
    float3 c = albedo * mrtDpow3(l, 0.45);
    if (s.emissive > 0.0) c += albedo * s.emissive;
    float fog = clamp(min(length(s.position - f.cameraPos.xyz) * p.fogParams.y + p.fogParams.x, p.fogParams.z), 0.0, 1.0);
    return lerp(c, p.fogColor.rgb, fog);
}

[numthreads(8, 8, 1)]
void CSMain(uint3 gid : SV_DispatchThreadID) {
    MaterialRtParams p = rvkPtr<MaterialRtParams, 16>(pc.params).Get();
    RvkFrame f = rvkLoadFrame(pc.frame);
    uint2 id = gid.xy;
    if (id.x >= p.info.x || id.y >= p.info.y) return;
    float4 none = float4(0.0, 0.0, 0.0, 1.0);
    uint scale = p.info.z;
    uint2 off = scale > 1u ? uint2(p.info.w & 1u, (p.info.w >> 1) & 1u) : uint2(0, 0);
    float2 uv = (float2(id * scale + off) + 0.5) * f.viewport.zw;
    if (any(uv >= float2(1.0, 1.0)) || !rvkRtValid(f)) {
        outImage[int2(id)] = none;
        return;
    }
    float far = f.cameraPos.w;
    float sceneZ = rvkSample2DLod(p.tex.x, p.tex2.x, uv, 0.0).r * far;
    float4 gb = rvkSample2DLod(p.tex.y, p.tex2.x, uv, 0.0);
    float3 mat = rvkGbufferMaterialDecode(gb.z);
    if (sceneZ >= far * 0.995 || gb.w <= 0.0) {
        outImage[int2(id)] = none;
        return;
    }
    bool hasEntry = mat.z > 0.5;
    float rough = mat.x, metal = mat.y;
    float3 cam = f.cameraPos.xyz;
    float3 P = rvkWorldFromViewDepth(f, uv, sceneZ);
    float3 V = normalize(cam - P);
    float3 N = rvkGiOctDecode(gb.xy);
    if (dot(N, V) < 0.0) N = reflect(N, V);

    // rain (docs/research/material-rt.md 5): outdoors only (a ray straight up reaches the sky), the pack entries get smoother and darker, flat ground
    // gets puddles: a flat mirror with rings of drops
    float wetPix = 0.0, puddle = 0.0, darken = 1.0;
    if (p.strength.z > 0.02 && (hasEntry || N.y > 0.78)) {
        float pud = mrtPuddle(p, P, N);
        if (hasEntry || pud > 0.01) {
            // sheltered? a ray up and a little out of the surface (a rugged cliff face must not flicker between wet and dry on every ledge);
            // walls catch less rain than ground
            float3 rd = normalize(float3(0.0, 1.0, 0.0) + N * 0.6);
            float roof = rvkRtVisibility(f, P + N * 8.0, rd, 2.0, 4000.0, 0.0, 0.0, RVK_RT_TRACE_OPAQUE);
            roof *= lerp(0.3, 1.0, smoothstep(0.0, 0.7, N.y));
            wetPix = p.strength.z * roof;
            puddle = pud * roof;
        }
    }
    if (!hasEntry && puddle < 0.01) {
        outImage[int2(id)] = none;
        return;
    }
    if (hasEntry) {
        rough *= 1.0 - 0.7 * wetPix;
        darken *= 1.0 - 0.3 * wetPix;
        if (rough > p.reach.w && puddle < 0.01) {
            outImage[int2(id)] = float4(0.0, 0.0, 0.0, darken);  // too rough to mirror anything: only the wet darkening
            return;
        }
    }
    if (puddle > 0.0) {
        // a puddle is a flat water surface whatever the cobbles' normal map says, dark underneath, with rings where drops fall
        float2 rip = mrtRipple(P.xz, f.time.x);
        float3 Np = normalize(float3(rip.x, 1.0, rip.y));
        N = normalize(lerp(N, Np, saturate(puddle * 1.5)));
        rough = lerp(rough, 0.03, saturate(puddle * 1.6));
        metal = lerp(metal, 0.0, puddle);
        darken *= 1.0 - 0.45 * puddle;
    }
    float NoV = saturate(dot(N, V));

    float4 sceneSample = rvkSample2DLod(p.tex.z, p.tex2.x, uv, 0.0);
    float3 sceneCol = max(sceneSample.rgb, float3(0.0, 0.0, 0.0));
    float lumBase = mrtLum(sceneCol);
    // the surface's fog (undone for the metal tint, applied again to the reflection)
    float fogK = saturate(min(distance(cam, P) * p.fogParams.y + p.fogParams.x, p.fogParams.z));
    float3 lit = fogK < 0.999 ? (sceneCol - p.fogColor.rgb * fogK) / (1.0 - fogK) : sceneCol;
    lit = max(lit, float3(0.0, 0.0, 0.0));
    float3 tint = lit / max(max(lit.r, max(lit.g, lit.b)), 1e-3);
    float3 F0 = lerp(float3(0.04, 0.04, 0.04), saturate(tint * 0.92), metal);
    F0 = lerp(F0, float3(0.06, 0.06, 0.06), puddle);  // (water's 0.02, lifted so a puddle reads in the engine's flat light)
    float3 Fe = F0 + (max(float3(1.0 - rough, 1.0 - rough, 1.0 - rough), F0) - F0) * pow(1.0 - NoV, 5.0);

    // the reflection direction: the mirror one, or one drawn from the GGX lobe of the roughness (the filter pass accumulates the draws)
    float3 R = reflect(-V, N);
    float a = max(rough * rough, 0.0025);
    if (rough > 0.04) {
        float2 u = float2(mrtHash(float2(id) + float(p.info.w) * 17.13), mrtHash(float2(id).yx * 1.37 + float(p.info.w) * 5.71 + 3.0));
        float phi = 6.2831853 * u.x;
        float cosT = sqrt((1.0 - u.y) / (1.0 + (a * a - 1.0) * u.y));
        float sinT = sqrt(max(1.0 - cosT * cosT, 0.0));
        float3 up = abs(N.y) < 0.99 ? float3(0.0, 1.0, 0.0) : float3(1.0, 0.0, 0.0);
        float3 T = normalize(cross(up, N));
        float3 B = cross(N, T);
        float3 H = normalize(T * (sinT * cos(phi)) + B * (sinT * sin(phi)) + N * cosT);
        float3 Rl = reflect(-V, H);
        if (dot(Rl, N) > 0.02) R = Rl;
    }

    float3 refl;
    float cover = 1.0;
    float pixelAngle = 2.0 / max(abs(f.proj[1][1]) * f.viewport.y, 1.0) * float(scale);
    float dist = distance(cam, P);
    float width0 = pixelAngle * dist;
    float spread = pixelAngle + p.reach.y + rough * 0.02;
    float3 o = P + N * 3.0;
    float maxD = p.reach.x;
    RvkRtHit h = rvkRtTraceClosest(f, o, R, 1.0, maxD, width0, spread, p.trace.x);
    if (h.hit) {
        float3 hp = o + R * h.t;
        cover = 1.0 - smoothstep(0.85 * maxD, maxD, h.t);
        float reuse = 0.0;
        float3 reused = float3(0.0, 0.0, 0.0);
        float2 huv;
        float hz;
        if (mrtProject(f, hp, huv, hz) && all(huv > float2(0.0, 0.0)) && all(huv < float2(1.0, 1.0))) {
            float2 edge = smoothstep(float2(0.0, 0.0), float2(0.03, 0.03), huv) * smoothstep(float2(0.0, 0.0), float2(0.03, 0.03), 1.0 - huv);
            float sz = rvkSample2DLod(p.tex.x, p.tex2.x, huv, 0.0).r * far;
            float tol = max(30.0, hz * 0.015);
            reuse = edge.x * edge.y * (1.0 - smoothstep(tol, 2.0 * tol, abs(sz - hz)));
            // (only a surface seen from the front: the hit's normal faces the camera)
            if (reuse > 0.0) reused = max(rvkSample2DLod(p.tex.z, p.tex2.x, huv, 0.0).rgb, float3(0.0, 0.0, 0.0));
        }
        float3 shaded = float3(0.0, 0.0, 0.0);
        if (reuse < 1.0) shaded = mrtShadeHit(p, f, h, R, width0, spread);
        refl = lerp(shaded, reused, reuse);
    } else {
        // the sky: the engine's sky cube (display colours) blurred by the roughness, scaled by how much light reaches the surface (a night plaza must not
        // reflect a day sky); nothing below the horizon
        refl = float3(0.0, 0.0, 0.0);
        if (f.images.w != 0u) {
            float lit01 = saturate(mrtLum(lit) * 3.0);
            refl = RVK_TEXCUBE(f.images.w).SampleLevel(RVK_SAMP(p.tex.w), R, rough * 6.0).rgb * lit01 * 0.7 * p.strength.x * smoothstep(-0.15, 0.25, R.y);
        }
    }
    float3 add = refl * Fe * lerp(1.0, 0.25, rough) * p.strength.y * cover;
    // never more than 0.15 (rough) .. 0.55 (smooth) of the surface's own lit brightness (the raster's cap; metals and glass exempt)
    float lumAdd = mrtLum(add);
    float capLum = lerp(lerp(0.55, 0.15, saturate(rough * 2.0)) * lumBase + 0.0004, 1.0e3, max(metal, puddle));
    if (lumAdd > capLum) add *= capLum / lumAdd;
    add *= 1.0 - fogK;
    if (any(isnan(add)) || any(isinf(add))) add = float3(0.0, 0.0, 0.0);
    float4 result = float4(add, darken);
    if (p.trace.w == 1u) result = float4(h.hit ? float3(0.0, 0.5, 0.0) : float3(0.0, 0.0, 0.5), 1.0);
    outImage[int2(id)] = result;
}
