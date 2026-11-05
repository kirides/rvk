// The GI rays' hit shading (shared by gi_trace.comp: the DDGI probes, and sp_trace.comp: the screen probes; src/enh/gi.cpp,
// docs/research/rtgi.md 9, 11): a hit is lit like the engine's material shaders light a surface (rtgi.md 1), in acc units:
// albedo (linear) x (sun x N.L x a shadow ray + one of the frame's point lights with a shadow ray + the probes' own
// irradiance at the hit: multi-bounce). A miss sees the sky: the engine's outdoor ambient as radiance. A back face (the
// interpolated vertex normal facing away: the ray starts inside geometry) returns no light and a negative distance.
// HLSL port of gi_shade.glsl.
#pragma once

#include "rvk_rt_trace.hlsli"
#include "rvk_gi.hlsli"
#include "gi_common.hlsli"

// one of the frame's point lights at the hit, chosen in proportion to its unshadowed light, with a shadow ray (an unbiased
// one-sample estimate of the sum; the temporal accumulation averages the noise)
float3 giPointLights(GiPassParams p, RvkFrame f, float3 P, float3 N, float3 G, float u) {
    uint n = min(p.Get().flags.y, GI_MAX_LIGHTS);
    if (n == 0u) return float3(0.0, 0.0, 0.0);
    float total = 0.0;
    for (uint i = 0u; i < n; ++i) {
        float4 lp = giLight(i * 2u);
        float3 d = lp.xyz - P;
        float d2 = dot(d, d);
        float3 L = d * rsqrt(max(d2, 1e-4));
        float fall = 1.0 - clamp(d2 * lp.w, 0.0, 1.0);
        float k = clamp(dot(N, L), 0.0, 1.0) * clamp(4.0 * dot(G, L), 0.0, 1.0) * fall;
        total += k * dot(giLight(i * 2u + 1u).rgb, float3(0.333, 0.333, 0.333));
    }
    if (total <= 1e-6) return float3(0.0, 0.0, 0.0);
    float pick = u * total;
    for (uint i = 0u; i < n; ++i) {
        float4 lp = giLight(i * 2u);
        float3 d = lp.xyz - P;
        float d2 = dot(d, d);
        float dist = sqrt(max(d2, 1e-4));
        float3 L = d / dist;
        float fall = 1.0 - clamp(d2 * lp.w, 0.0, 1.0);
        float k = clamp(dot(N, L), 0.0, 1.0) * clamp(4.0 * dot(G, L), 0.0, 1.0) * fall;
        float3 col = giLight(i * 2u + 1u).rgb;
        float w = k * dot(col, float3(0.333, 0.333, 0.333));
        if (w <= 0.0) continue;
        pick -= w;
        if (pick > 0.0 && i + 1u < n) continue;
        float vis = rvkRtVisibility(f, P + G * 2.0, L, 1.0, max(dist - 8.0, 1.0), 0.0, 0.0, RVK_RT_TRACE_OPAQUE);
        // estimate = (the light's term / its probability) = its colour x k x total / w
        return col * k * (total / w) * vis;
    }
    return float3(0.0, 0.0, 0.0);
}

// set by the screen probes (rtgi.md 23): they start on visible surfaces, so a hit's back face is a surface seen from behind,
// not "the origin is inside geometry" (the DDGI probes' test): shade it like any two-sided surface instead of returning black
static bool gGiTwoSided = false;
// diagnostics (RVK_GI_DIAG, rtgi.md 23): the last giShadeRay's terms (already multiplied by the hit's albedo)
static float3 gDiagSun = float3(0.0, 0.0, 0.0), gDiagPoint = float3(0.0, 0.0, 0.0), gDiagBounce = float3(0.0, 0.0, 0.0),
              gDiagEmis = float3(0.0, 0.0, 0.0), gDiagLin = float3(0.0, 0.0, 0.0);

// radiance (rgb, acc units) arriving at `origin` from `dir` and the hit distance (w; tMax for a miss, negative for a back face)
float4 giShadeRay(GiPassParams p, RvkFrame f, RvkGiVolume v, float3 origin, float3 dir, float tMax, float u) {
    float4 result = float4(0.0, 0.0, 0.0, tMax);
    gDiagSun = float3(0.0, 0.0, 0.0);
    gDiagPoint = float3(0.0, 0.0, 0.0);
    gDiagBounce = float3(0.0, 0.0, 0.0);
    gDiagEmis = float3(0.0, 0.0, 0.0);
    gDiagLin = float3(0.0, 0.0, 0.0);
    if (!rvkRtValid(f)) {
        result.rgb = rvkGiSkyRadiance(p.Get().sky, dir);
        return result;
    }
    float spread = p.Get().shade.w;
    RvkRtHit h = rvkRtTraceClosest(f, origin, dir, 1.0, tMax, 0.0, spread, p.Get().flags.x);
    if (!h.hit) {
        result.rgb = (rvkGiExp(v) & 4u) != 0u ? float3(0.0, 0.0, 0.0) : rvkGiSkyRadiance(p.Get().sky, dir);
        return result;
    }
    RvkRtRecord r = rvkRtRecord(f, h.record);
    // inside geometry: the hit's own (interpolated) normal faces away from the origin; foliage and characters are two-sided
    // / have no normals: never back faces
    if (!gGiTwoSided && (r.flags & (RVK_RT_ALPHA_TESTED | RVK_RT_SPEEDTREE | RVK_RT_DYNAMIC)) == 0u && r.normal != RVK_RT_ABSENT) {
        RvkRtSurface raw = rvkRtSurface(r, h.primitive, h.bary);
        if (dot(raw.normal, raw.normal) > 1e-12 && dot(rvkRtWorldNormal(h.worldToObject, raw.normal), dir) > 0.0) {
            return float4(0.0, 0.0, 0.0, -0.2 * h.t);
        }
    }
    RvkRtHitSurface s = rvkRtHitSurface(f, h, dir, 0.0, spread);
    float3 albedo = s.albedo.rgb;
    if ((s.uvSet & RVK_RT_UV_NO_TEXTURE) != 0u) {
        albedo = (s.uvSet & RVK_RT_UV_HEURISTIC) != 0u ? float3(0.2, 0.2, 0.2) : min(albedo, float3(0.5, 0.5, 0.5));
    }
    // the textures hold gamma-encoded colours: rgb = albedo x pow(acc, 0.45) = pow(albedo^2.2 x acc, 0.45)
    float3 lin = pow(clamp(albedo, float3(0.0, 0.0, 0.0), float3(1.0, 1.0, 1.0)), float3(2.2, 2.2, 2.2)) * p.Get().shade.y;
    float3 N = normalize(s.normal), G = s.geometricNormal;
    float3 E = float3(0.0, 0.0, 0.0);
    float3 Esun = float3(0.0, 0.0, 0.0), Epoint = float3(0.0, 0.0, 0.0);
    float3 L = p.Get().sunDir.xyz;
    float nl = clamp(dot(L, N), 0.0, 1.0) * clamp(dot(L, G) * 4.0, 0.0, 1.0);
    if (nl > 0.0 && dot(p.Get().sunColor.rgb, float3(1.0, 1.0, 1.0)) > 1e-3) {
        float vis = rvkRtVisibility(f, s.position + G * 2.0, L, 1.0, p.Get().shade.z, 0.0, 0.0, RVK_RT_TRACE_OPAQUE);
        E += p.Get().sunColor.rgb * (nl * vis);
        Esun = p.Get().sunColor.rgb * (nl * vis);
    }
    const uint ex = rvkGiExp(v);
    // (EXP 2: the probes carry the point lights' direct light already: do not count it twice at the hit)
    if ((ex & 2u) == 0u || (p.Get().flags.z & 1u) == 0u) Epoint = giPointLights(p, f, s.position, N, G, u);
    E += Epoint;
    // the probes' irradiance at the hit (last frames': multi-bounce); where no probe covers the hit, the engine's outdoor
    // ambient
    float4 regs[7];
    float3 Ebounce;
    if (rvkGiLookupHit(v, s.position + N * 4.0, N, regs)) {
        Ebounce = max(rvkGiEvalRegs(regs, N), float3(0.0, 0.0, 0.0)) * p.Get().shade.x;
    } else {
        Ebounce = (ex & 1u) != 0u ? float3(0.0, 0.0, 0.0) : max(rvkGiEvalRegs(p.Get().sky, N), float3(0.0, 0.0, 0.0)) * p.Get().shade.x;
    }
    if ((ex & 8u) != 0u) Ebounce = float3(0.0, 0.0, 0.0);
    E += Ebounce;
    // self-illuminated surfaces (lava, lamp glass: rtgi.md 19) emit their diffuse colour; the engine adds the self-illumination
    // after the pow, so its acc-unit equivalent is the colour ^ 2.2; flags.w = the gain (gi.emissive)
    float3 emission = float3(0.0, 0.0, 0.0);
    float gain = asfloat(p.Get().flags.w);
    if ((s.uvSet & RVK_RT_UV_EMISSIVE) != 0u && gain > 0.0)
        emission = pow(clamp(albedo, float3(0.0, 0.0, 0.0), float3(1.0, 1.0, 1.0)), float3(2.2, 2.2, 2.2)) * gain;
    result = float4(lin * E + emission, h.t);
    gDiagSun = lin * Esun;
    gDiagPoint = lin * Epoint;
    gDiagBounce = lin * Ebounce;
    gDiagEmis = emission;
    gDiagLin = lin;
    if (any(isnan(result)) || any(isinf(result))) result = float4(0.0, 0.0, 0.0, tMax);
    return result;
}
