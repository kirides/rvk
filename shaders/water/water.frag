#version 460
// Enhanced water shading (src/enh/water.cpp, docs/research/water.md 6), linear HDR into the engine's FP16 scene target,
// drawn opaque in the engine's Refracted pass (right after the refraction copy):
//   refraction  the refraction copy (the opaque scene) seen through the surface, distorted by the normal, rejected where
//               the distorted sample lies in front of the water (as ge_default.fxh Refract does)
//   volume      Beer-Lambert absorption along the refracted path (the engine's linear depth map gives the ground behind)
//               and in-scattering of the material's own colour lit by the engine's ambient + sun
//   reflection  screen-space reflections against the depth map / refraction copy (water.reflections = ssr), falling back
//               to the engine's sky environment cube (the actual sky of the moment); Fresnel (Schlick, water F0 0.02)
//   sun glint   GGX lobe of the engine's sun (g_DirLight), shadowed by the engine's shadow mask where the draw has one
//   foam        the sea only: the coast (shore field + depth), the swash line, wave crests; tinted by the water colour
//   fog         the engine's radial fog (ge_common.fxh RadialFog: c3 colour, c4 settings) on the surface layer; what is
//               seen through it is already fogged in the copy
// The SSR march follows GD3D11's D3D12 Water.hlsl TraceWaterSSR (quadratic steps, binary refinement, thickness test,
// foreground-occluder rule; by kirides, the author of this project), ported to GLSL against the engine's linear depth map.

#include "water_common.glsl"

layout(location = 0) in vec4 vWorld;
layout(location = 1) in vec4 vNormal;
layout(location = 2) in vec4 vExtra;
layout(location = 0) out vec4 outColor;

float sceneViewDepth(WaterBlock w, RvkFrame f, vec2 uv) {
    return rvkSample2DLod(w.tex.y, w.smp.z, uv, 0.0).r * f.cameraPos.w;
}

vec3 refractionColor(WaterBlock w, vec2 uv) { return rvkSample2DLod(w.tex.x, w.smp.x, uv, 0.0).rgb; }

bool projectToUv(RvkFrame f, vec3 p, out vec2 uv, out float viewZ) {
    vec4 c = f.viewProj * vec4(p, 1.0);
    viewZ = c.w;
    uv = vec2(c.x / c.w * 0.5 + 0.5, 0.5 - c.y / c.w * 0.5);
    return c.w > 1.0;
}

vec3 skyColor(WaterBlock w, vec3 dir) {
    if (w.tex.z == 0u) return w.fogColor.rgb;
    return textureLod(samplerCube(tCube[nonuniformEXT(w.tex.z)], samp[nonuniformEXT(w.smp.x)]), dir, 0.0).rgb * w.misc.x;
}

// screen-space reflection; conf 0 = miss. A ray that leaves the screen through the sky (depth map at the far plane) returns
// the scene's own sky there with a reduced confidence: the reflection of the sky actually on screen
vec3 traceSSR(WaterBlock w, RvkFrame f, vec3 origin, vec3 dir, out float conf) {
    conf = 0.0;
    uint steps = w.tex2.y;
    if (steps == 0u) return vec3(0.0);
    float maxDist = w.misc.w;
    float far = f.cameraPos.w;
    float startBias = 8.0;
    vec3 prev = origin + dir * startBias;
    vec2 prevUv;
    float prevZ;
    projectToUv(f, prev, prevUv, prevZ);
    vec2 skyUv = vec2(-1.0);
    vec2 behindUv = vec2(-1.0);
    for (uint i = 1u; i <= steps; ++i) {
        float s = float(i) / float(steps);
        float travelled = startBias + maxDist * s * s;
        vec3 cur = origin + dir * travelled;
        vec2 uv;
        float rayZ;
        if (!projectToUv(f, cur, uv, rayZ)) break;
        if (any(lessThan(uv, vec2(0.0))) || any(greaterThan(uv, vec2(1.0)))) break;
        float sceneZ = sceneViewDepth(w, f, uv);
        if (sceneZ >= far * 0.995) {
            skyUv = uv;  // over the sky: keep marching (something may still be in front further on)
            prev = cur;
            prevZ = rayZ;
            continue;
        }
        float thickness = 60.0 + travelled * 0.05;
        if (rayZ > sceneZ) behindUv = uv; else behindUv = vec2(-1.0);
        if (rayZ >= sceneZ && sceneZ >= prevZ - thickness) {
            // binary refinement
            vec3 lo = prev, hi = cur;
            vec2 hitUv = uv;
            float gap = rayZ - sceneZ;
            for (int j = 0; j < 4; ++j) {
                vec3 mid = 0.5 * (lo + hi);
                vec2 muv;
                float mz;
                if (!projectToUv(f, mid, muv, mz)) break;
                float ms = sceneViewDepth(w, f, muv);
                if (mz >= ms && ms >= prevZ - thickness) {
                    hi = mid;
                    hitUv = muv;
                    gap = mz - ms;
                } else {
                    lo = mid;
                }
            }
            if (gap < thickness) {
                vec2 edge = smoothstep(vec2(0.0), vec2(0.06), hitUv) * smoothstep(vec2(0.0), vec2(0.06), 1.0 - hitUv);
                conf = edge.x * edge.y * clamp(1.0 - travelled / maxDist, 0.0, 1.0);
                vec3 c = refractionColor(w, hitUv);
                if (any(isnan(c)) || any(isinf(c))) {
                    conf = 0.0;
                    return vec3(0.0);
                }
                return min(c, vec3(4096.0));
            }
        }
        prev = cur;
        prevZ = rayZ;
    }
    if (behindUv.x >= 0.0) {
        // left the screen behind geometry: that geometry covers this direction (dimmed: its far side is not seen)
        vec2 edge = smoothstep(vec2(0.0), vec2(0.06), behindUv) * smoothstep(vec2(0.0), vec2(0.06), 1.0 - behindUv);
        conf = 0.75 * max(edge.x * edge.y, 0.5);
        return min(refractionColor(w, behindUv) * 0.7, vec3(4096.0));
    }
    if (skyUv.x >= 0.0) {
        vec2 edge = smoothstep(vec2(0.0), vec2(0.06), skyUv) * smoothstep(vec2(0.0), vec2(0.06), 1.0 - skyUv);
        conf = 0.85 * edge.x * edge.y;
        return min(refractionColor(w, skyUv), vec3(4096.0));
    }
    return vec3(0.0);
}

vec3 ambientLight(WaterBlock w, vec3 n) {
    vec4 n1 = vec4(n, 1.0);
    return max(vec3(dot(w.ambient[0], n1), dot(w.ambient[1], n1), dot(w.ambient[2], n1)), vec3(0.0));
}

float ggx(float nh, float a) {
    float a2 = a * a;
    float d = nh * nh * (a2 - 1.0) + 1.0;
    return a2 / (3.14159265 * d * d);
}

void main() {
    WaterBlock w = waterBlock();
    RvkFrame f = pc.frame;
    vec2 uv = gl_FragCoord.xy * f.viewport.zw;
    vec3 P = vWorld.xyz;
    vec3 toCam = f.cameraPos.xyz - P;
    float dist = length(toCam);
    vec3 V = toCam / max(dist, 1e-3);
    float t = w.grid.w;
    uint flags = w.tex2.z;
    bool sea = (flags & WATER_SEA) != 0u;

    // ---- normal: the geometric normal, the sea's waves, detail ripples (faded with distance against aliasing) ----
    vec3 N = normalize(vNormal.xyz);
    float crest = 0.0;
    if ((flags & WATER_OCEAN_GRID) != 0u) {
        vec3 wn;
        gerstner(vExtra.xy, t, vNormal.w, wn, crest);
        // far away the waves are smaller than a pixel: towards the mean normal (no glittering noise at the horizon)
        N = normalize(mix(vec3(0.0, 1.0, 0.0), wn, 1.0 / (1.0 + dist / 25000.0)));
    }
    float rippleFade = 1.0 / (1.0 + dist / 6000.0);
    // ripple slopes: the noise derivative (per noise unit, ~1) scaled to a slope of ~0.1 at ripple strength 1
    vec2 slope = rippleSlope(P.xz, t, w.surface.w, 3) * (0.1 * w.surface.z * rippleFade);
    // perturb in the surface's tangent frame (waterfalls and slopes keep their orientation)
    vec3 tx = normalize(cross(vec3(0.0, 0.0, 1.0), N) + vec3(1e-4, 0.0, 0.0));
    vec3 tz = cross(N, tx);
    N = normalize(N - tx * slope.x - tz * slope.y);
    bool underside = dot(V, vNormal.xyz) < 0.0 && (flags & WATER_OCEAN_GRID) == 0u;
    underside = underside || (flags & WATER_UNDERSIDE) != 0u;
    if (underside) N = -N;
    float nv = max(dot(N, V), 1e-3);

    // ---- what lies behind the surface ----
    float far = f.cameraPos.w;
    vec2 dummyUv;
    float surfZ;
    projectToUv(f, P, dummyUv, surfZ);
    float sceneZ0 = sceneViewDepth(w, f, uv);
    float thick0 = max(sceneZ0 - surfZ, 0.0);
    // refraction offset (screen space), stronger for deeper water, weaker far away
    vec2 off = (N.xz - vNormal.xz) * (0.035 * w.scatter.a) * clamp(thick0 / 150.0, 0.0, 1.0) / (1.0 + surfZ / 4000.0);
    vec2 ruv = clamp(uv + off, vec2(0.0), vec2(1.0));
    float sceneZ = sceneViewDepth(w, f, ruv);
    if (sceneZ < surfZ) {  // the distorted sample is in front of the water: no distortion (ge_default.fxh Refract)
        ruv = uv;
        sceneZ = sceneZ0;
    }
    vec3 refr = refractionColor(w, ruv);
    bool sky = sceneZ >= far * 0.995;
    vec3 G = rvkWorldFromViewDepth(f, ruv, sceneZ);
    float vdepth = sky ? 1e6 : max(P.y - G.y, 0.0);  // vertical water depth over the ground (flat surfaces)
    // the light from the ground travels the refracted direction inside the water (Snell, n = 1.33): at a grazing view the
    // path is ~1.5x the depth, not the long view-ray segment; slanted surfaces (falls, rapids) use the view-ray segment
    float sinT = sqrt(max(1.0 - nv * nv, 0.0)) / 1.333;
    float cosT = sqrt(max(1.0 - sinT * sinT, 0.0));
    float flatness = smoothstep(0.85, 0.97, vNormal.y);
    float path = sky ? 1e6 : min(mix(distance(P, G), vdepth / max(cosT, 0.2), flatness), 1e6);

    // ---- lighting of the in-water colour (the engine's ambient + sun, as its water shaders light c6) ----
    vec3 L = normalize(w.sunDir.xyz);
    float sunUp = clamp(L.y * 4.0, 0.0, 1.0);
    float shadow = 1.0;
    if (w.tex2.x != 0u) shadow = rvkSample2DLod(w.tex2.x, w.smp.x, uv, 0.0).a;
    vec3 light = ambientLight(w, vec3(0.0, 1.0, 0.0)) + w.sunColor.rgb * clamp(L.y, 0.0, 1.0) * sunUp * mix(0.35, 1.0, shadow);
    // the engine's point lights (its falloff: 1 - saturate(d^2 / range^2)): diffuse into the water colour, a glint each
    vec3 pointGlint = vec3(0.0);
    for (int i = 0; i < 4; ++i) {
        vec4 pr = w.lights[i * 3];
        if (pr.w <= 0.0) continue;
        vec3 lv = pr.xyz - P;
        float fall = 1.0 - clamp(dot(lv, lv) * pr.w, 0.0, 1.0);
        if (fall <= 0.0) continue;
        vec3 ld = normalize(lv);
        light += w.lights[i * 3 + 1].rgb * fall * clamp(ld.y, 0.0, 1.0);
        vec3 hp = normalize(V + ld);
        float fhp = 0.02 + 0.98 * pow(1.0 - max(dot(V, hp), 0.0), 5.0);
        pointGlint += w.lights[i * 3 + 2].rgb * fall * (ggx(max(dot(N, hp), 0.0), max(w.surface.x, 0.02)) * fhp * 0.25 / max(nv, 0.1)) *
                      clamp(dot(N, ld), 0.0, 1.0) * w.surface.y;
    }
    vec3 inscatter = w.scatter.rgb * pow(max(light, vec3(0.0)), vec3(0.45));
    // the ground is lit through the water too (downwelling along the vertical depth), then seen along the refracted path
    vec3 T = exp(-w.absorb.rgb * (path + (sky ? 0.0 : vdepth)));
    // turbidity: the lit water colour over the view through it (not at the very edge: no film on the sand)
    T *= 1.0 - w.look.x * smoothstep(0.0, 30.0, vdepth);
    vec3 under = refr * T + inscatter * (1.0 - T);

    // ---- reflection ----
    vec3 R = reflect(-V, N);
    R.y = max(R.y, 0.015);
    R = normalize(R);
    vec3 refl = skyColor(w, R);
    if (!underside) {
        float conf;
        vec3 ssr = traceSSR(w, f, P, R, conf);
        refl = mix(refl, ssr, conf);
    }
    float F = 0.02 + 0.98 * pow(1.0 - nv, 5.0);
    F = clamp(F * w.absorb.a, 0.0, 1.0) * mix(0.35, 1.0, smoothstep(0.0, 45.0, vdepth));
    if (underside) F = 0.0;

    // ---- sun glint ----
    vec3 H = normalize(V + L);
    float nh = max(dot(N, H), 0.0);
    float a = max(w.surface.x, 0.02);
    float Fh = 0.02 + 0.98 * pow(1.0 - max(dot(V, H), 0.0), 5.0);
    vec3 glint = w.sunSpec.rgb * (ggx(nh, a) * Fh * 0.25 / max(nv, 0.1)) * clamp(dot(N, L), 0.0, 1.0) * w.surface.y * shadow * sunUp;
    glint = min(glint + pointGlint, vec3(64.0));
    if (underside) glint = vec3(0.0);

    // ---- foam (the sea only): coast band, swash line over the ground, wave crests ----
    float foam = 0.0;
    if (sea && w.foam.a > 0.0) {
        float coastDist = vExtra.z;
        // foam texture: a broad pattern + finer detail that fades out with distance (no pixel noise far away)
        float fineFade = 1.0 / (1.0 + dist / 1500.0);
        float foamNoise = 0.5 + 0.5 * waterNoised(P.xz / 180.0 + vec2(t * 0.11, -t * 0.07)).x +
                          0.25 * fineFade * waterNoised(P.xz / 45.0 + vec2(-t * 0.05, t * 0.08)).x;
        // lines rolling in towards the coast (phase moves towards smaller distances)
        float band = 1.0 - smoothstep(0.0, max(w.misc.z, 1.0) * 1.4, coastDist);
        // surf: the engine's coast overlay marks the surf zone (field distance ~0 inside it); in there, breaker lines run
        // over the water depth towards the shallows (phase moves to smaller depths) and fade out on the way in
        float zone = (1.0 - smoothstep(0.0, max(w.misc.z, 1.0) * 0.5, coastDist)) * (1.0 - smoothstep(60.0, 260.0, vdepth));
        float lines = pow(0.5 + 0.5 * sin(vdepth * 0.07 + t * 1.3 + foamNoise * 2.0), 4.0);
        float coastFoam = zone * lines * smoothstep(0.2, 0.7, foamNoise) * smoothstep(0.0, 25.0, vdepth + 10.0);
        // contact foam where the water is shallow over the ground (follows the swash up and down the beach)
        float contact = (1.0 - smoothstep(0.0, 9.0, vdepth)) * smoothstep(0.45, 0.9, foamNoise) * 0.8;
        float crests = smoothstep(0.55, 0.95, crest) * smoothstep(0.4, 0.9, foamNoise);
        foam = clamp(max(max(coastFoam, contact), crests) * w.foam.a, 0.0, 1.0);
    }
    // foam: a rough white-ish layer lit like the engine's diffuse surfaces (albedo ~0.5, the sun's colour half desaturated)
    vec3 sunGrey = vec3(dot(w.sunColor.rgb, vec3(0.3, 0.59, 0.11)));
    float white = 0.0;
    for (int i = 0; i < 4; ++i) {
        vec4 fall = w.falls[i];
        if (fall.z <= 0.0) continue;
        float d = length(P.xz - fall.xy) / fall.z;
        float n = 0.5 + 0.5 * waterNoised(P.xz / 70.0 + vec2(0.0, -t * 0.9)).x + 0.35 * waterNoised(P.xz / 22.0 - t * 0.5).x;
        white = max(white, (1.0 - smoothstep(0.25, 1.0, d)) * step(abs(P.y - fall.w), 400.0) * smoothstep(0.35, 0.95, n));
    }
    foam = max(foam, white * 0.65);
    vec3 foamColor = 0.6 * w.foam.rgb * (ambientLight(w, vec3(0.0, 1.0, 0.0)) +
                                         mix(sunGrey, w.sunColor.rgb, 0.5) * clamp(L.y, 0.0, 1.0) * mix(0.35, 1.0, shadow));

    // ---- compose: the transmitted part (already fogged in the copy) + the surface layer (fogged here) ----
    float opacity = 1.0 - (1.0 - F) * (1.0 - foam);
    vec3 transmitted = refr * T * (1.0 - F) * (1.0 - foam);
    vec3 layer = inscatter * (1.0 - T) * (1.0 - F) * (1.0 - foam) + refl * F * (1.0 - foam) + foamColor * foam + glint;
    float layerAlpha = 1.0 - (1.0 - opacity) * (T.r + T.g + T.b) / 3.0;
    float fog = clamp(min(dist * w.fogParams.y + w.fogParams.x, w.fogParams.z), 0.0, 1.0);
    vec3 color = transmitted + mix(layer, w.fogColor.rgb * layerAlpha, fog);
    // a soft edge where the water gets very thin (no hard seam on the ground)
    float edge = sky ? 1.0 : smoothstep(0.0, 6.0, path);
    color = mix(refractionColor(w, uv), color, edge);
    if (underside) color = refr * exp(-w.absorb.rgb * dist) + inscatter * (1.0 - exp(-w.absorb.rgb * dist));

    // debug views (water.debug): 1 shore field distance, 2 vertical depth, 3 normal, 4 reflection only
    if (w.tex2.w == 1u) color = vec3(fract(vExtra.z / 1000.0), clamp(vExtra.z / 30000.0, 0.0, 1.0), sea ? 1.0 : 0.0);
    else if (w.tex2.w == 2u) color = vec3(clamp(vdepth / 500.0, 0.0, 1.0), clamp(path / 3000.0, 0.0, 1.0), 0.0);
    else if (w.tex2.w == 3u) color = N * 0.5 + 0.5;
    else if (w.tex2.w == 4u) color = refl;
    outColor = vec4(max(color, vec3(0.0)), 1.0);
}
