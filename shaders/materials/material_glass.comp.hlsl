// Glass panes with ray tracing (setting materials.glass, enh/material_rt.*, docs/research/material-rt.md 8), run after the alpha passes: the scene HDR holds
// the engine's blended glass over the opaque scene. For every pixel a primary ray against the glass instances only (TLAS mask kRtMaskGlass: windows, bottles of
// the pack entries with transmission > 0, in no other ray's way) finds a pane in front of the opaque scene (the depth map); there:
//   * Fresnel reflection (Schlick, F0 0.04): a reflection ray traced like the material reflections' (screen reuse, engine-style hit shading, sky cube),
//   * thin panes (the exit face of a 2 - 6 cm slab is parallel): nothing else, the engine's blend already shows what is behind them,
//   * thick / curved glass (bottles): the ray refracts in (eta 1/1.5), a ray inside finds the exit face, refracts out (total internal reflection reflects),
//     the transmitted ray's hit replaces most of the scene behind the glass (tinted by the glass' diffuse colour).
// Output as the material reflections' image: rgb = what to add, a = the factor the scene is multiplied by.

#include "material_rt_common.hlsli"

[[vk::image_format("rgba16f")]] RWTexture2D<float4> outImage : register(u0, space1);

struct MaterialRtPush {
    RVK_PASS_PUSH_HEADER
    uint2 params;
    uint2 pad;
};
RVK_PUSH(MaterialRtPush, pc);

float3 glassRadiance(MaterialRtParams p, RvkFrame f, float3 o, float3 d, float width0, float spread, float lit01, out float cover) {
    cover = 1.0;
    float maxD = p.reach.x;
    RvkRtHit h = rvkRtTraceClosest(f, o, d, 1.0, maxD, width0, spread, p.trace.x);
    if (h.hit) {
        float3 hp = o + d * h.t;
        cover = 1.0 - smoothstep(0.85 * maxD, maxD, h.t);
        float reuse = 0.0;
        float3 reused = float3(0.0, 0.0, 0.0);
        float2 huv;
        float hz;
        if (mrtProject(f, hp, huv, hz) && all(huv > float2(0.0, 0.0)) && all(huv < float2(1.0, 1.0))) {
            float2 edge = smoothstep(float2(0.0, 0.0), float2(0.03, 0.03), huv) * smoothstep(float2(0.0, 0.0), float2(0.03, 0.03), 1.0 - huv);
            float sz = rvkSample2DLod(p.tex.x, p.tex2.x, huv, 0.0).r * f.cameraPos.w;
            float tol = max(30.0, hz * 0.015);
            reuse = edge.x * edge.y * (1.0 - smoothstep(tol, 2.0 * tol, abs(sz - hz)));
            if (reuse > 0.0) reused = max(rvkSample2DLod(p.tex.z, p.tex2.x, huv, 0.0).rgb, float3(0.0, 0.0, 0.0));
        }
        float3 shaded = float3(0.0, 0.0, 0.0);
        if (reuse < 1.0) shaded = mrtShadeHit(p, f, h, d, width0, spread);
        return lerp(shaded, reused, reuse);
    }
    if (f.images.w == 0u) return float3(0.0, 0.0, 0.0);
    return RVK_TEXCUBE(f.images.w).SampleLevel(RVK_SAMP(p.tex.w), d, 0.0).rgb * lit01 * 0.7 * p.strength.x * smoothstep(-0.15, 0.25, d.y);
}

[numthreads(8, 8, 1)]
void CSMain(uint3 gid : SV_DispatchThreadID) {
    MaterialRtParams p = rvkPtr<MaterialRtParams, 16>(pc.params).Get();
    RvkFrame f = rvkLoadFrame(pc.frame);
    uint2 id = gid.xy;
    uint2 size = uint2(f.viewport.xy);
    if (id.x >= size.x || id.y >= size.y) return;
    float4 none = float4(0.0, 0.0, 0.0, 1.0);
    float2 uv = (float2(id) + 0.5) * f.viewport.zw;
    if (!rvkRtValid(f)) {
        outImage[int2(id)] = none;
        return;
    }
    float far = f.cameraPos.w;
    float3 cam = f.cameraPos.xyz;
    float sceneZ = rvkSample2DLod(p.tex.x, p.tex2.x, uv, 0.0).r * far;
    bool sky = sceneZ >= far * 0.995;
    float3 dir = normalize(rvkWorldFromViewDepth(f, uv, 1000.0) - cam);
    float tScene = sky ? 1e30 : length(rvkWorldFromViewDepth(f, uv, sceneZ) - cam);
    float pixelAngle = 2.0 / max(abs(f.proj[1][1]) * f.viewport.y, 1.0);
    RvkRtHit g = rvkRtTraceClosest(f, cam, dir, 1.0, min(tScene + 2.0, 30000.0), 0.0, pixelAngle, RVK_RT_TRACE_GLASS | RVK_RT_TRACE_OPAQUE);
    if (!g.hit) {
        outImage[int2(id)] = none;
        return;
    }
    RvkRtHitSurface s = rvkRtHitSurface(f, g, dir, 0.0, pixelAngle);
    float3 N = normalize(s.normal);
    float3 P = s.position;
    float NoV = saturate(dot(N, -dir));
    float F = 0.04 + 0.96 * pow(1.0 - NoV, 5.0);
    float3 sceneCol = max(rvkSample2DLod(p.tex.z, p.tex2.x, uv, 0.0).rgb, float3(0.0, 0.0, 0.0));
    float lumScene = mrtLum(sceneCol);
    float lit01 = max(saturate(lumScene * 3.0), 0.35);
    float width0 = pixelAngle * g.t;
    float spread = pixelAngle + p.reach.y;

    // the reflection
    float cover;
    float3 refl = glassRadiance(p, f, P + N * 2.0, reflect(dir, N), width0, spread, lit01, cover) * cover;

    // thick glass: through the pane
    float3 refr = float3(0.0, 0.0, 0.0);
    float transmit = 0.0;
    float3 T = refract(dir, N, 1.0 / 1.5);
    if (dot(T, T) > 0.5) {
        RvkRtHit e = rvkRtTraceClosest(f, P - N * 1.0, T, 0.5, 60.0, 0.0, 0.0, RVK_RT_TRACE_GLASS | RVK_RT_TRACE_OPAQUE);
        if (e.hit) {
            RvkRtHitSurface es = rvkRtHitSurface(f, e, T, 0.0, 0.0);
            float3 N2 = normalize(es.normal);  // towards the ray's origin: inside
            // a slab: the exit face is within a few cm and parallel to the entry face; anything else is thick / curved
            bool thin = e.t < 8.0 && dot(N2, -N) > 0.97;
            if (!thin) {
                float3 T2 = refract(T, N2, 1.5);
                float c2;
                if (dot(T2, T2) < 0.5) T2 = reflect(T, N2);  // total internal reflection
                refr = glassRadiance(p, f, es.position - N2 * 0.0 + T2 * 2.0, T2, width0, spread, lit01, c2);
                float3 tint = lerp(float3(1.0, 1.0, 1.0), saturate(s.albedo.rgb * 1.6), 0.6);
                refr *= tint * c2;
                transmit = 0.8;
            }
        }
    }
    float3 add = refl * F * p.strength.y + refr * (1.0 - F) * transmit;
    float lumAdd = mrtLum(add);
    float capLum = 2.0 * lumScene + 0.05;
    if (lumAdd > capLum) add *= capLum / lumAdd;
    if (any(isnan(add)) || any(isinf(add))) add = float3(0.0, 0.0, 0.0);
    float mul = 1.0 - transmit * (1.0 - F);
    outImage[int2(id)] = float4(add, mul);
}
