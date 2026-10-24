// The GI rays' hit shading (shared by gi_trace.comp: the DDGI probes, and sp_trace.comp: the screen probes; src/enh/gi.cpp,
// docs/research/rtgi.md 9, 11): a hit is lit like the engine's material shaders light a surface (rtgi.md 1), in acc units:
// albedo (linear) x (sun x N.L x a shadow ray + one of the frame's point lights with a shadow ray + the probes' own
// irradiance at the hit: multi-bounce). A miss sees the sky: the engine's outdoor ambient as radiance. A back face (the
// interpolated vertex normal facing away: the ray starts inside geometry) returns no light and a negative distance.
// Include after rvk_rt_trace.glsl, rvk_gi.glsl and gi_common.glsl.

#ifndef GI_SHADE_GLSL
#define GI_SHADE_GLSL

// one of the frame's point lights at the hit, chosen in proportion to its unshadowed light, with a shadow ray (an unbiased
// one-sample estimate of the sum; the temporal accumulation averages the noise)
vec3 giPointLights(GiPassParams p, RvkFrame f, vec3 P, vec3 N, vec3 G, float u) {
    uint n = min(p.flags.y, GI_MAX_LIGHTS);
    if (n == 0u) return vec3(0.0);
    float total = 0.0;
    for (uint i = 0u; i < n; ++i) {
        vec4 lp = p.lights[i * 2u];
        vec3 d = lp.xyz - P;
        float d2 = dot(d, d);
        vec3 L = d * inversesqrt(max(d2, 1e-4));
        float fall = 1.0 - clamp(d2 * lp.w, 0.0, 1.0);
        float k = clamp(dot(N, L), 0.0, 1.0) * clamp(4.0 * dot(G, L), 0.0, 1.0) * fall;
        total += k * dot(p.lights[i * 2u + 1u].rgb, vec3(0.333));
    }
    if (total <= 1e-6) return vec3(0.0);
    float pick = u * total;
    for (uint i = 0u; i < n; ++i) {
        vec4 lp = p.lights[i * 2u];
        vec3 d = lp.xyz - P;
        float d2 = dot(d, d);
        float dist = sqrt(max(d2, 1e-4));
        vec3 L = d / dist;
        float fall = 1.0 - clamp(d2 * lp.w, 0.0, 1.0);
        float k = clamp(dot(N, L), 0.0, 1.0) * clamp(4.0 * dot(G, L), 0.0, 1.0) * fall;
        vec3 col = p.lights[i * 2u + 1u].rgb;
        float w = k * dot(col, vec3(0.333));
        if (w <= 0.0) continue;
        pick -= w;
        if (pick > 0.0 && i + 1u < n) continue;
        float vis = rvkRtVisibility(f, P + G * 2.0, L, 1.0, max(dist - 8.0, 1.0), 0.0, 0.0, RVK_RT_TRACE_OPAQUE);
        // estimate = (the light's term / its probability) = its colour x k x total / w
        return col * k * (total / w) * vis;
    }
    return vec3(0.0);
}

// radiance (rgb, acc units) arriving at `origin` from `dir` and the hit distance (w; tMax for a miss, negative for a back face)
vec4 giShadeRay(GiPassParams p, RvkFrame f, RvkGiVolume v, vec3 origin, vec3 dir, float tMax, float u) {
    vec4 result = vec4(0.0, 0.0, 0.0, tMax);
    if (!rvkRtValid(f)) {
        result.rgb = rvkGiSkyRadiance(p.sky, dir);
        return result;
    }
    float spread = p.shade.w;
    RvkRtHit h = rvkRtTraceClosest(f, origin, dir, 1.0, tMax, 0.0, spread, p.flags.x);
    if (!h.hit) {
        result.rgb = rvkGiSkyRadiance(p.sky, dir);
        return result;
    }
    RvkRtRecord r = rvkRtRecord(f, h.record);
    // inside geometry: the hit's own (interpolated) normal faces away from the origin; foliage and characters are two-sided
    // / have no normals: never back faces
    if ((r.flags & (RVK_RT_ALPHA_TESTED | RVK_RT_SPEEDTREE | RVK_RT_DYNAMIC)) == 0u && r.normal != RVK_RT_ABSENT) {
        RvkRtSurface raw = rvkRtSurface(r, h.primitive, h.bary);
        if (dot(raw.normal, raw.normal) > 1e-12 && dot(rvkRtWorldNormal(h.worldToObject, raw.normal), dir) > 0.0) {
            return vec4(0.0, 0.0, 0.0, -0.2 * h.t);
        }
    }
    RvkRtHitSurface s = rvkRtHitSurface(f, h, dir, 0.0, spread);
    vec3 albedo = s.albedo.rgb;
    if ((s.uvSet & RVK_RT_UV_NO_TEXTURE) != 0u) {
        albedo = (s.uvSet & RVK_RT_UV_HEURISTIC) != 0u ? vec3(0.2) : min(albedo, vec3(0.5));
    }
    // the textures hold gamma-encoded colours: rgb = albedo x pow(acc, 0.45) = pow(albedo^2.2 x acc, 0.45)
    vec3 lin = pow(clamp(albedo, vec3(0.0), vec3(1.0)), vec3(2.2)) * p.shade.y;
    vec3 N = normalize(s.normal), G = s.geometricNormal;
    vec3 E = vec3(0.0);
    vec3 L = p.sunDir.xyz;
    float nl = clamp(dot(L, N), 0.0, 1.0) * clamp(dot(L, G) * 4.0, 0.0, 1.0);
    if (nl > 0.0 && dot(p.sunColor.rgb, vec3(1.0)) > 1e-3) {
        float vis = rvkRtVisibility(f, s.position + G * 2.0, L, 1.0, p.shade.z, 0.0, 0.0, RVK_RT_TRACE_OPAQUE);
        E += p.sunColor.rgb * (nl * vis);
    }
    E += giPointLights(p, f, s.position, N, G, u);
    // the probes' irradiance at the hit (last frames': multi-bounce); where no probe covers the hit, the engine's outdoor
    // ambient
    vec4 regs[7];
    if (rvkGiLookupHit(v, s.position + N * 4.0, N, regs)) {
        E += max(rvkGiEvalRegs(regs, N), vec3(0.0)) * p.shade.x;
    } else {
        E += max(rvkGiEvalRegs(p.sky, N), vec3(0.0)) * p.shade.x;
    }
    // self-illuminated surfaces (lava, lamp glass: rtgi.md 19) emit their diffuse colour; the engine adds the self-illumination
    // after the pow, so its acc-unit equivalent is the colour ^ 2.2; flags.w = the gain (gi.emissive)
    vec3 emission = vec3(0.0);
    float gain = uintBitsToFloat(p.flags.w);
    if ((s.uvSet & RVK_RT_UV_EMISSIVE) != 0u && gain > 0.0) emission = pow(clamp(albedo, vec3(0.0), vec3(1.0)), vec3(2.2)) * gain;
    result = vec4(lin * E + emission, h.t);
    if (any(isnan(result)) || any(isinf(result))) result = vec4(0.0, 0.0, 0.0, tMax);
    return result;
}

#endif
