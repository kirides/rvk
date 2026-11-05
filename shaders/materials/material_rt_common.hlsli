// Ray-traced reflections of the material pack's smooth surfaces (enh/material_rt.*, docs/research/material-rt.md): the parameter block shared by the
// trace pass (material_rt.comp), the filter pass (material_rt_filter.comp) and the composite draw (material_rt.frag).
#pragma once

#include "rvk_rt_trace.hlsli"
#include "rvk_oct.hlsli"

// mirror: enh::MaterialRtParams (static_assert'ed size)
struct MaterialRtParams {
    uint4 tex;        // x depth map (t2D, view depth / engine far), y G-buffer normal, z scene HDR, w linear clamp sampler
    uint4 tex2;       // x point clamp sampler, y G-buffer motion, z history of the previous frame (0 = none), w the raw reflection image
    uint4 info;       // x trace image width, y height, z pixels per trace pixel (1 / 2), w frame index
    uint4 trace;      // x RVK_RT_TRACE_*, y shadow rays (0 none, 1 cut-outs solid, 2 alpha-tested), z tweaks, w debug (1 coverage, 2 raw)
    float4 reach;     // x reach (world units), y extra cone spread (rad), z shadow ray reach, w roughness limit (smoother surfaces trace)
    float4 strength;  // x sky reflection (materials.environment), y reflection strength, z rain wetness 0..1, w puddle strength 0..1
    float4 fogColor;  // c3
    float4 fogParams; // c4 (x offset, y per unit, z maximum)
    float4 indoor;    // c14
    float4 sh[7];     // c15..c21
    float4 sunColor;  // c22
    float4 sunDir;    // c24 (towards the sun)
};

float mrtDpow(float x, float y) { return x == 0.0 ? 0.0 : exp2(y * log2(abs(x))); }
float3 mrtDpow3(float3 v, float e) { return float3(mrtDpow(v.r, e), mrtDpow(v.g, e), mrtDpow(v.b, e)); }
float mrtLum(float3 c) { return dot(c, float3(0.3, 0.59, 0.11)); }

float mrtHash(float2 p) {
    float3 p3 = frac(float3(p.xyx) * 0.1031);
    p3 += dot(p3, p3.yzx + 33.33);
    return frac((p3.x + p3.y) * p3.z);
}

float mrtNoise(float2 p) {
    float2 i = floor(p), f = frac(p);
    float2 u = f * f * (3.0 - 2.0 * f);
    return lerp(lerp(mrtHash(i), mrtHash(i + float2(1.0, 0.0)), u.x), lerp(mrtHash(i + float2(0.0, 1.0)), mrtHash(i + float2(1.0, 1.0)), u.x), u.y);
}
float mrtFbm(float2 p) { return 0.55 * mrtNoise(p) + 0.3 * mrtNoise(p * 2.13 + 7.0) + 0.15 * mrtNoise(p * 4.37 + 3.0); }

// Rain puddles (docs/research/material-rt.md 5): the share of a pixel under a puddle. Flat surfaces (the G-buffer's normal) of a world position, where a value
// noise of about 2 m cells passes a threshold that drops while the wetness rises (the puddles spread and merge); params strength.z = the rain wetness
// (0 dry), strength.w = materials.puddles. Deterministic in the position: the filter pass evaluates it for its taps too. The caller multiplies by the
// pixel's roof test (rain does not reach it indoors / under a roof).
float mrtPuddle(MaterialRtParams p, float3 P, float3 N) {
    float wet = p.strength.z;
    if (wet <= 0.02 || p.strength.w <= 0.0 || N.y < 0.78) return 0.0;
    float n = mrtFbm(P.xz * (1.0 / 210.0));
    float thr = lerp(0.74, 0.46, saturate(wet));
    return smoothstep(thr, thr + 0.07, n) * p.strength.w * smoothstep(0.78, 0.93, N.y) * smoothstep(0.2, 0.6, wet);
}

// the normal's offset of the rain's drops in a puddle: rings that spread from random points of a 55 cm grid (xz), time in seconds
float2 mrtRipple(float2 xz, float t) {
    const float cell = 55.0;
    float2 g = xz / cell;
    float2 ci = floor(g);
    float2 f = g - ci;
    float2 acc = float2(0.0, 0.0);
    [unroll] for (int dy = -1; dy <= 1; ++dy) {
        [unroll] for (int dx = -1; dx <= 1; ++dx) {
            float2 c = ci + float2(float(dx), float(dy));
            float h = mrtHash(c), h2 = mrtHash(c + 7.3);
            float2 centre = float2(float(dx), float(dy)) + float2(h, h2) * 0.6 + 0.2;
            float phase = frac(t * 0.8 + h * 3.7);
            float2 d = (f - centre) * cell;
            float dist = max(length(d), 1e-3);
            float w = dist - phase * 34.0;
            float amp = (1.0 - phase) * (1.0 - phase) * exp(-w * w / 14.0) * cos(w * 0.55);
            acc += d / dist * amp;
        }
    }
    return acc * 0.22;
}

// ---- shared by the trace passes: the hit shading ----
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

