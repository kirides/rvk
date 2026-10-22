#version 460
// Enhanced water shading (src/enh/water.cpp, docs/research/water.md 4), linear HDR into the engine's FP16 scene target,
// drawn opaque in the engine's Refracted pass (right after the refraction copy).
//
// "The same water, just better": the colour and brightness come from the engine's own material. Each Risen water material
// has its own ge_default pixel shader (ocean, lake, river, swamp); `engineSurface` evaluates the one of this draw, ported
// instruction by instruction from the D3D9 bytecode (docs/research/water.md 2.3): its normal maps, diffuse textures,
// constants (g_ShaderData c6..c13), its 9-coefficient ambient + sun lighting, Fresnel, specular, depth fade and refraction
// offset. The interpolators are the engine vertex shader's own (translated, it runs unchanged on the engine's meshes), so
// texture scrolling and river flow are the engine's; the sea's wave grid (water_ocean.vert) produces the same
// interpolators. What is better:
//   reflection  wherever the engine reflects its sky cube, the reflection ray is traced in screen space first (rocks, ships,
//               trees, cliffs are reflected; GD3D11's D3D12 Water.hlsl TraceWaterSSR ported, by kirides, the author of this
//               project); a miss keeps the engine's sky cube; the swamp, which has no reflection, gets a dim one
//   waves       the open sea: Gerstner waves + waves turning towards the coast (geometry and normal), the engine's normal
//               maps on top; a lower, moving waterline instead of the painted shore
//   volume      the sea's light path through the water tints what is seen through it (Beer-Lambert along the refracted
//               path), before the engine's own depth fade
//   foam        the sea only: surf lines, contact foam at the waterline and at walls, wave crests; never brighter than the
//               lit sand
//   fog         the engine's radial fog as the engine applies it

#include "water_common.glsl"

layout(location = 0) in vec4 tc0;   // TEXCOORD0..6: the engine water VS's outputs (see enh/water.cpp)
layout(location = 1) in vec4 tc1;
layout(location = 3) in vec4 tc3;   // world position, w global alpha (c32.w)
layout(location = 4) in vec4 tc4;   // world normal, w outdoor factor (COLOR1.w)
layout(location = 5) in vec4 tc5;   // tangent (grid: w wave amplitude)
layout(location = 6) in vec4 tc6;   // binormal (grid: w crest steepness)
layout(location = 7) in vec4 tc7;   // grid only: undisplaced xz, distance to the coast, shore-wave weight
layout(location = 16) in vec4 col0; // COLOR0 (river: alpha = the mesh's edge fade)
layout(location = 0) out vec4 outColor;

// ---- inputs ---------------------------------------------------------------------------------------------------------------

vec4 mtex(WaterBlock w, uint s, vec2 uv) { return texture(sampler2D(t2D[nonuniformEXT(w.tex[s])], samp[nonuniformEXT(w.smp[s])]), uv); }
vec4 mcube(WaterBlock w, uint s, vec3 d) {
    return texture(samplerCube(tCube[nonuniformEXT(w.tex[s])], samp[nonuniformEXT(w.smp[s])]), d);
}
float sceneViewDepth(WaterBlock w, RvkFrame f, vec2 uv) { return rvkSample2DLod(w.res2.w, w.smpOurs.z, uv, 0.0).r * f.cameraPos.w; }
vec3 refractionColor(WaterBlock w, vec2 uv) { return rvkSample2DLod(w.res2.z, w.smpOurs.x, uv, 0.0).rgb; }

bool projectToUv(RvkFrame f, vec3 p, out vec2 uv, out float viewZ) {
    vec4 c = f.viewProj * vec4(p, 1.0);
    viewZ = c.w;
    uv = vec2(c.x / c.w * 0.5 + 0.5, 0.5 - c.y / c.w * 0.5);
    return c.w > 1.0;
}

// D3D9 pow (log2 / exp2 of |x|)
float dpow(float x, float y) { return x == 0.0 ? 0.0 : exp2(y * log2(abs(x))); }

// ---- screen-space reflection; conf 0 = miss (the engine's sky cube stays) -------------------------------------------------

vec3 traceSSR(WaterBlock w, RvkFrame f, vec3 origin, vec3 dir, vec2 jitter, out float conf) {
    conf = 0.0;
    uint steps = w.res2.y;
    if (steps == 0u) return vec3(0.0);
    float maxDist = w.misc.z;
    float far = f.cameraPos.w;
    float startBias = 8.0;
    vec3 prev = origin + dir * startBias;
    vec2 prevUv;
    float prevZ;
    projectToUv(f, prev, prevUv, prevZ);
    for (uint i = 1u; i <= steps; ++i) {
        float s = float(i) / float(steps);
        float travelled = startBias + maxDist * s * s;
        vec3 cur = origin + dir * travelled;
        vec2 uv;
        float rayZ;
        if (!projectToUv(f, cur, uv, rayZ)) break;
        if (any(lessThan(uv, vec2(0.0))) || any(greaterThan(uv, vec2(1.0)))) break;
        float sceneZ = sceneViewDepth(w, f, uv);
        if (sceneZ >= far * 0.995) {  // over the sky: keep marching (something may still be in front further on)
            prev = cur;
            prevZ = rayZ;
            continue;
        }
        float thickness = 60.0 + travelled * 0.05;
        if (rayZ >= sceneZ && sceneZ >= prevZ - thickness) {
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
                // the detail normal's ripples break the reflected image up (the ray itself follows the calmer normal)
                vec2 juv = clamp(hitUv + jitter, vec2(0.0), vec2(1.0));
                if (sceneViewDepth(w, f, juv) < far * 0.995) hitUv = juv;
                vec3 c = refractionColor(w, hitUv);
                if (any(isnan(c)) || any(isinf(c))) {
                    conf = 0.0;
                    return vec3(0.0);
                }
                return c;
            }
        }
        prev = cur;
        prevZ = rayZ;
    }
    return vec3(0.0);  // a miss (off screen, through the sky): the engine's sky cube
}

// ---- the engine's lighting of its water (all four shaders): ambient (9 coefficients, indoor blend) + sun, then ^0.45 ------

// + the engine's point lights (shaders with g_PointLights: falloff 1 - saturate(d^2 / range^2), the point-light shadow mask
// s0 by channel), added before the curve as the engine does
vec3 engineLight(WaterBlock w, vec3 N, vec3 Ngeo, float outdoor, vec3 P, vec2 uv) {
    vec4 n1 = vec4(N, 1.0);
    vec4 n2 = N.yzzx * N.xyzz;
    vec3 sh = vec3(dot(w.sh[0], n1) + dot(w.sh[3], n2), dot(w.sh[1], n1) + dot(w.sh[4], n2), dot(w.sh[2], n1) + dot(w.sh[5], n2)) +
              w.sh[6].rgb * (N.x * N.x - N.y * N.y);
    vec3 amb = mix(w.indoor.rgb, sh, outdoor);
    vec3 L = w.sunDir.xyz;
    float nl = clamp(dot(L, N), 0.0, 1.0);
    float up = clamp(dot(L, Ngeo) * 4.0, 0.0, 1.0);
    vec3 l = amb + w.sunColor.rgb * nl * up;
    uint nLights = w.lightInfo.x;
    if (nLights > 0u) {
        vec4 mask = w.tex[0] != 0u ? rvkSample2DLod(w.tex[0], w.smpOurs.x, uv, 0.0) : vec4(0.0);
        for (uint i = 0u; i < nLights; ++i) {
            vec4 lp = w.lights[i * 3u];
            vec3 lv = lp.xyz - P;
            float d2 = dot(lv, lv);
            vec3 ld = lv * inversesqrt(max(d2, 1e-6));
            float att = (1.0 - clamp(d2 * lp.w, 0.0, 1.0)) * (1.0 - dot(w.lights[i * 3u + 2u], mask));
            float lup = clamp(dot(ld, Ngeo) * 4.0, 0.0, 1.0);
            l += w.lights[i * 3u + 1u].rgb * clamp(dot(ld, N), 0.0, 1.0) * lup * att;
        }
    }
    return vec3(dpow(l.r, 0.45), dpow(l.g, 0.45), dpow(l.b, 0.45));
}

vec2 nmap(vec4 t) { return t.wy; }  // DXT5 normal maps: x in alpha, y in green

// the engine's normal: tangent-space xy (0..1 per channel before the *2-1), the frame from the VS (not normalised, as the
// engine)
vec3 frameNormal(vec2 n01, vec3 T, vec3 B, vec3 Ngeo, out vec2 nxy) {
    nxy = n01 * 2.0 - 1.0;
    float z = sqrt(max(1.0 - dot(nxy, nxy), 0.0));
    return nxy.x * T + nxy.y * B + z * Ngeo;
}

// the traced reflection follows a calmer normal than the shading (the detail maps' ripples would scatter the rays into noise;
// they still shape the sky cube's reflection and the specular)
vec3 ssrDir(vec3 N, vec3 Ngeo, vec3 V) {
    vec3 n = normalize(mix(normalize(Ngeo), normalize(N), 0.35));
    vec3 R = reflect(-V, n);
    R.y = max(R.y, 0.01);
    return normalize(R);
}

// the traced reflection over the engine's sky cube. Risen's water art relies on the cube's bright sky (also in caves and
// under trees, where it reads as the light's glitter on the water): a traced dark wall may dim it, never erase it (lakes
// and rivers keep more of it: their cube is masked into glitter by the material's textures)
vec3 keepSky(vec3 cube, vec3 ssr, float conf, float floorK) { return max(mix(cube, ssr, conf), cube * floorK); }

vec2 ssrJitter(vec3 N, vec3 Ngeo, float dist) {
    vec3 n = normalize(N), g = normalize(Ngeo);
    return (n.xz - g.xz) * vec2(0.12, -0.12) / (1.0 + dist / 4000.0);
}

struct Surface {
    vec3 colour;      // the engine's surface colour (before fog)
    float alpha;      // its opacity over what is seen through it
    vec2 distort;     // refraction offset scale (screen uv per unit of nxy, per axis)
    vec3 N;           // the shading normal
    vec2 nxy;         // tangent-space normal xy (refraction offset direction)
    vec3 refl;        // the reflection used (debug view)
};

// ---- main ---------------------------------------------------------------------------------------------------------------

void main() {
    WaterBlock w = waterBlock();
    RvkFrame f = pc.frame;
    uint flags = w.res.z;
    uint model = w.res2.x;
    bool grid = (flags & WATER_OCEAN_GRID) != 0u;
    bool sea = (flags & WATER_SEA) != 0u;
    vec2 uv = gl_FragCoord.xy * f.viewport.zw;
    vec3 P = tc3.xyz;
    vec3 toCam = f.cameraPos.xyz - P;
    float dist = length(toCam);
    vec3 V = toCam / max(dist, 1e-3);
    float t = w.grid.w;
    float far = f.cameraPos.w;
    float viewW = 1.0 / gl_FragCoord.w;  // the water point's view depth (the engine's v2.w)
    float sceneZ0 = sceneViewDepth(w, f, uv);
    float thick0 = sceneZ0 - viewW;  // the view ray's way through the water (the engine's depth fade input)

    vec3 Ngeo = tc4.xyz;
    vec3 T = tc5.xyz;
    vec3 B = tc6.xyz;
    if (grid) {
        // the waves' frame (the sea mesh's tangent, re-orthogonalised on the wave normal)
        Ngeo = normalize(Ngeo);
        // wind ripples between the waves (faded with distance against glitter), under the engine's normal maps
        // choppier inside the engine's coast overlay (harbours, coves: the water the engine painted moving)
        float chop = 1.0 + 1.5 * (1.0 - smoothstep(100.0, 600.0, tc7.z));
        vec2 rs = rippleSlope(P.xz, t, 420.0, 3) * (0.06 * chop / (1.0 + dist / 5000.0));
        Ngeo = normalize(Ngeo + vec3(-rs.x, 0.0, -rs.y));
        T = normalize(w.meshFrame.xyz - Ngeo * dot(Ngeo, w.meshFrame.xyz));
        B = cross(Ngeo, T) * w.meshFrame.w;
    }
    float outdoor = tc4.w;
    bool underside = (flags & WATER_UNDERSIDE) != 0u || (!grid && dot(V, Ngeo) < 0.0);

    // the engine's shadow mask (s0, screen space, the sea only)
    float shadow = 1.0;
    if (model == WATER_MODEL_OCEAN && w.tex[0] != 0u) shadow = rvkSample2DLod(w.tex[0], w.smpOurs.x, uv, 0.0).x;

    Surface s;
    s.refl = vec3(0.0);
    vec3 L = w.sunDir.xyz;
    vec4 c6 = w.mat[0], c7 = w.mat[1], c8 = w.mat[2], c9 = w.mat[3], c10 = w.mat[4], c11 = w.mat[5], c12 = w.mat[6];
    float reflK = w.look.x;  // water.reflection_strength

    if (model == WATER_MODEL_OCEAN) {
        // ps 6e22a01a6cf0ac0a: s14 / s13 normal maps blended by the mask s9, s11 * s10 diffuse, s12 sky cube
        vec4 mask = mtex(w, 9u, tc1.zw);
        vec2 n01 = mix(nmap(mtex(w, 13u, tc1.xy)), nmap(mtex(w, 14u, tc0.xy)), mask.wy);
        s.N = frameNormal(n01, T, B, Ngeo, s.nxy);
        vec3 lit = engineLight(w, s.N, Ngeo, outdoor, P, uv) * shadow;
        vec3 R = 2.0 * dot(V, s.N) * s.N - V;
        float nv = dot(s.N, V);
        float nl = clamp(dot(L, s.N), 0.0, 1.0);
        float up = clamp(dot(L, Ngeo) * 4.0, 0.0, 1.0);
        vec3 cube = mcube(w, 12u, R).rgb;
        float rl = clamp(dot(R, L), 0.0, 1.0);
        vec3 spec = clamp(dpow(rl, c8.w * 128.0) * nl / max(nv, 1e-5), 0.0, 1.0) * w.sunSpec.rgb * up * clamp(cube * c8.x, 0.0, 1.0);
        float fres = (c9.y * dpow(1.0 - nv, c9.z) + c9.x) * reflK;
        vec3 diffuse = mtex(w, 10u, tc0.zw).rgb * mtex(w, 11u, tc1.xy).rgb * c8.y;
        vec3 refl = cube;
        if (!underside) {
            float conf;
            vec3 ssr = traceSSR(w, f, P, ssrDir(s.N, Ngeo, V), ssrJitter(s.N, Ngeo, dist), conf);
            refl = keepSky(cube, min(ssr, vec3(1.5)), conf, 0.55);
        }
        s.refl = refl;
        s.colour = c6.rgb * lit + spec + clamp(mix(diffuse, refl, clamp(fres, 0.0, 1.0)), 0.0, 1.5);
        float fade = dpow(thick0 * c7.x + c7.y, c7.z);
        vec3 a3 = clamp(fade * max(vec3(mask.x, mask.x, mask.y), vec3(fade)), 0.0, 1.0);
        s.alpha = a3.x * tc3.w;
        s.distort = clamp(a3.yz * c6.w, 0.0, 1.0);
    } else if (model == WATER_MODEL_LAKE) {
        // ps 912d3cc09d934354: s12 / s13 / s14 normal maps, s9 sky cube masked by s10 / s11 (sampled by R.xy)
        vec2 n01 = mix(nmap(mtex(w, 12u, tc0.zw)), nmap(mtex(w, 13u, tc1.xy)), c11.w);
        n01 = clamp(mix(n01, nmap(mtex(w, 14u, tc0.xy)), c11.z), 0.0, 1.0);
        s.N = frameNormal(n01, T, B, Ngeo, s.nxy);
        vec3 lit = engineLight(w, s.N, Ngeo, outdoor, P, uv);
        vec3 R = 2.0 * dot(V, s.N) * s.N - V;
        float nv = dot(s.N, V);
        vec3 cube = clamp(mcube(w, 9u, R).rgb, 0.0, 1.0);
        vec3 refl = cube;
        if (!underside) {
            float conf;
            vec3 ssr = traceSSR(w, f, P, ssrDir(s.N, Ngeo, V), ssrJitter(s.N, Ngeo, dist), conf);
            refl = keepSky(cube, min(ssr, vec3(1.0)), conf, 0.85);
        }
        s.refl = refl;
        vec2 rm = R.xy * 0.5 + 0.5;
        float m10 = clamp(mtex(w, 10u, rm).y, 0.0, 1.0);
        float m11 = mtex(w, 11u, rm).y;
        float fresR = (c8.y * dpow(1.0 - nv, c8.z) + c8.x) * reflK;
        vec3 r = clamp(refl * m10 * fresR * c7.y * (m11 * c7.z) * c6.w, 0.0, 1.0);
        vec3 base = (r + c6.rgb) * c7.x;
        s.colour = base * lit + base;
        float fade = clamp(dpow(thick0 * c9.x + c9.y, c9.z), 0.0, 1.0);
        float alphaF = c10.y * dpow(1.0 - nv, c10.z) + c10.x;
        s.alpha = clamp(min(alphaF, fade), 0.0, 1.0) * tc3.w;
        s.distort = vec2(fade * c12.w);
    } else if (model == WATER_MODEL_SWAMP) {
        // ps 2cd5ceec7eb11593: s9 / s10 normal maps, colour + specular; the engine reflects nothing: a dim reflection here
        vec2 n01 = mix(nmap(mtex(w, 9u, tc1.xy)), nmap(mtex(w, 10u, tc0.xy)), c9.w);
        s.N = frameNormal(n01, T, B, Ngeo, s.nxy);
        vec3 lit = engineLight(w, s.N, Ngeo, outdoor, P, uv);
        vec3 R = 2.0 * dot(V, s.N) * s.N - V;
        float nv = dot(s.N, V);
        float nl = clamp(dot(L, s.N), 0.0, 1.0);
        float up = clamp(dot(L, Ngeo) * 4.0, 0.0, 1.0);
        float rl = clamp(dot(R, L), 0.0, 1.0);
        vec3 spec = clamp(dpow(rl, 128.0 * c10.w) * nl / max(nv, 1e-5), 0.0, 1.0) * w.sunSpec.rgb * up * c9.rgb;
        s.colour = c6.rgb * lit + spec;
        if (!underside && w.look.z > 0.0) {
            // still dark water mirrors the trees around it, a little: Schlick Fresnel x look.z over the murky colour
            float conf;
            vec3 ssr = traceSSR(w, f, P, ssrDir(s.N, Ngeo, V), ssrJitter(s.N, Ngeo, dist), conf);
            vec3 cube = w.res.x != 0u ? clamp(textureLod(samplerCube(tCube[nonuniformEXT(w.res.x)], samp[nonuniformEXT(w.smpOurs.x)]), R, 0.0).rgb, 0.0, 1.0)
                                      : s.colour;
            vec3 refl = mix(cube * 0.6, min(ssr, vec3(1.0)), conf);
            s.refl = refl;
            float F = (0.02 + 0.98 * dpow(1.0 - clamp(nv, 0.0, 1.0), 5.0)) * w.look.z * reflK;
            s.colour = mix(s.colour, refl, clamp(F, 0.0, 1.0));
        }
        float fade = dpow(thick0 * c7.x + c7.y, c7.z);
        float alphaF = c8.y * dpow(1.0 - nv, c8.z) + c8.x;
        s.alpha = clamp(min(alphaF, fade), 0.0, 1.0) * tc3.w;
        s.distort = vec2(fade * c6.w);
    } else {
        // WATER_MODEL_RIVER, ps 11b69f874dfbacdb: s14 / s15 normal maps, whitewater s10 / s11 blended by s12, s13 sky cube,
        // s9 (by R.xy) scales the whitewater
        vec2 n01 = mix(nmap(mtex(w, 14u, tc1.xy)), nmap(mtex(w, 15u, tc0.xy)), c7.x);
        s.N = frameNormal(n01, T, B, Ngeo, s.nxy);
        vec3 lit = engineLight(w, s.N, Ngeo, outdoor, P, uv);
        vec3 diff = mix(mtex(w, 10u, tc1.xy).rgb, mtex(w, 11u, tc0.xy).rgb, mtex(w, 12u, tc0.xy).rgb);
        vec3 R = 2.0 * dot(V, s.N) * s.N - V;
        float nv = dot(s.N, V);
        vec3 cube = clamp(mcube(w, 13u, R).rgb, 0.0, 1.0);
        vec3 refl = cube;
        if (!underside) {
            float conf;
            vec3 ssr = traceSSR(w, f, P, ssrDir(s.N, Ngeo, V), ssrJitter(s.N, Ngeo, dist), conf);
            refl = keepSky(cube, min(ssr, vec3(1.0)), conf, 0.85);
        }
        s.refl = refl;
        float k = mtex(w, 9u, R.xy * 0.5 + 0.5).y * c7.w;
        vec3 base = clamp(k * diff + c6.rgb, 0.0, 1.0);
        float fres = (c8.y * dpow(1.0 - nv, c8.z) + c8.x) * reflK;
        vec3 c = clamp(mix(base, refl, fres) * c6.w, 0.0, 1.0);
        s.colour = c * lit + c;
        float fade = dpow(thick0 * c9.x + c9.y, c9.z);
        s.alpha = clamp(min(fade, col0.w), 0.0, 1.0) * tc3.w;
        s.distort = vec2(clamp(fade * c7.y, 0.0, 1.0));
    }

    // ---- refraction (as the engine: offset by the normal map's xy, rejected where the offset sample lies in front) ----
    vec2 off = s.distort * s.nxy;
    vec2 ruv = clamp(uv + off, vec2(0.0), vec2(1.0));
    float sceneZ = sceneViewDepth(w, f, ruv);
    if (sceneZ - viewW < 0.0) {
        ruv = uv;
        sceneZ = sceneZ0;
    }
    vec3 refr = refractionColor(w, ruv);
    bool skyBehind = sceneZ >= far * 0.995;
    vec3 G = rvkWorldFromViewDepth(f, ruv, sceneZ);
    float vdepth = skyBehind ? 1e6 : max(P.y - G.y, 0.0);  // vertical depth over the ground
    // the same without the refraction offset (smooth: foam and the soft edge follow it)
    bool sky0 = sceneZ0 >= far * 0.995;
    float vdepth0 = sky0 ? 1e6 : max(P.y - rvkWorldFromViewDepth(f, uv, sceneZ0).y, 0.0);

    // ---- volume (the sea): what lies under the water loses red, then green, along the light's way through it ----
    if (sea && !skyBehind && !underside) {
        float nv = clamp(dot(normalize(Ngeo), V), 0.0, 1.0);
        float sinT = sqrt(max(1.0 - nv * nv, 0.0)) / 1.333;
        float cosT = sqrt(max(1.0 - sinT * sinT, 0.0));
        float path = vdepth / max(cosT, 0.25) + vdepth;  // down to the ground and back up to the eye
        refr *= exp(-vec3(0.0016, 0.0007, 0.0005) * w.misc.w * path);
    }

    // ---- foam (the sea only): surf lines, contact with the beach and walls, wave crests ----
    float foam = 0.0;
    if (sea && grid && w.look.y > 0.0 && !underside) {
        float coastDist = tc7.z;
        float fineFade = 1.0 / (1.0 + dist / 1500.0);
        float foamNoise = 0.5 + 0.5 * waterNoised(P.xz / 180.0 + vec2(t * 0.11, -t * 0.07)).x +
                          0.25 * fineFade * waterNoised(P.xz / 45.0 + vec2(-t * 0.05, t * 0.08)).x;
        // surf: breakers rolling in over the shallows near the coast. The phase runs over the distance to the coast and the
        // depth (both grow seaward; inside the engine's overlay strip the field distance is ~0), so the lines lie parallel
        // to the beach and move towards it; each line has a sharp front and a fading trail of foam behind it
        float surfCoord = coastDist + 6.0 * vdepth0;
        float arg = surfCoord / 650.0 + t * 0.16 + foamNoise * 0.35;
        float saw = fract(arg);
        float line = smoothstep(0.0, 0.1, saw) * exp(-saw * 2.5);
        float breakZone = (1.0 - smoothstep(40.0, 180.0, vdepth0)) * (1.0 - smoothstep(900.0, 1800.0, coastDist)) *
                          smoothstep(0.0, 8.0, vdepth0);
        float surf = breakZone * line * smoothstep(0.0, 0.7, foamNoise) * 0.9;
        // the broken water of the surf zone between the lines: streaky foam over the shallows next to the beach
        float shallow = smoothstep(3.0, 14.0, vdepth0) * (1.0 - smoothstep(35.0, 110.0, vdepth0)) * (1.0 - smoothstep(600.0, 1200.0, coastDist));
        float streak = 0.5 + 0.5 * waterNoised(P.xz / vec2(260.0, 90.0) + vec2(t * 0.05, -t * 0.12)).x;
        float lace = 0.5 + 0.5 * waterNoised(P.xz / vec2(55.0, 22.0) + vec2(t * 0.07, -t * 0.25)).x;
        surf = max(surf, shallow * smoothstep(0.5, 0.85, streak * 0.4 + lace * 0.35 + foamNoise * 0.25) * 0.45);
        // deeper water inside the engine's coast overlay (harbour basins, rocky coves): churned water, faint streaks where
        // the engine painted its moving foam
        float churn = (1.0 - smoothstep(60.0, 300.0, coastDist)) * smoothstep(60.0, 160.0, vdepth0);
        float fine = 0.5 + 0.5 * waterNoised(P.xz / vec2(70.0, 28.0) + vec2(-t * 0.09, t * 0.21)).x;
        surf = max(surf, churn * smoothstep(0.5, 0.85, streak * 0.45 + fine * 0.35 + foamNoise * 0.2) * 0.4 / (1.0 + dist / 3000.0));
        // contact: a lace where the water meets the beach (follows the swash) or a wall (short view-ray thickness)
        float contactD = min(vdepth0 * 1.5, thick0);
        float contact = (1.0 - smoothstep(1.0, 7.0, contactD)) * smoothstep(1.0, 3.0, contactD + 2.0) * smoothstep(0.35, 0.95, foamNoise) * 0.35;
        // crests of the larger waves offshore and of the waves rolling in
        float crest = smoothstep(0.55, 0.9, tc6.w) * smoothstep(0.4, 0.85, foamNoise) * 0.75;
        foam = clamp(max(max(surf, contact), crest) * w.look.y, 0.0, 1.0);
    }
    // whitewater below waterfalls (lakes: the river material has its own whitewater art)
    if (model == WATER_MODEL_LAKE && !underside) {
        float white = 0.0;
        for (int i = 0; i < 4; ++i) {
            vec4 fl = w.falls[i];
            if (fl.z <= 0.0) continue;
            float d = length(P.xz - fl.xy) / fl.z;
            float n = 0.5 + 0.5 * waterNoised(P.xz / 70.0 + vec2(0.0, -t * 0.9)).x + 0.35 * waterNoised(P.xz / 22.0 - t * 0.5).x;
            white = max(white, (1.0 - smoothstep(0.25, 1.0, d)) * step(abs(P.y - fl.w), 400.0) * smoothstep(0.4, 0.95, n));
        }
        foam = max(foam, white * 0.5);
    }

    // ---- compose as the engine does: lerp(refraction, lerp(surface, fog colour, fog), alpha) ----
    float fog = clamp(min(dist * w.fogParams.y + w.fogParams.x, w.fogParams.z), 0.0, 1.0);
    vec3 color = mix(refr, mix(s.colour, w.fogColor.rgb, fog), s.alpha);
    float alpha = s.alpha;
    if (foam > 0.0) {
        // foam lies on top of the water: white-ish, lit like the beach (the engine's diffuse lighting curve, albedo ~0.7),
        // fogged like the surface; never glowing
        vec3 lit = engineLight(w, Ngeo, Ngeo, outdoor, P, uv) * shadow;
        // white: the light's brightness, a little of the sky's tint (the sun's warm colour made it look like sand)
        float lum = dot(lit, vec3(0.2126, 0.7152, 0.0722));
        vec3 foamCol = 0.7 * mix(vec3(lum), lit, 0.25);
        color = mix(color, mix(foamCol, w.fogColor.rgb, fog), foam);
        alpha = max(alpha, foam);
    }
    // a soft edge where the water gets very thin (no seam on the beach)
    if (!sky0) color = mix(refractionColor(w, uv), color, smoothstep(0.0, 5.0, min(vdepth0 * 2.0, thick0)));
    if ((flags & WATER_BLENDED) != 0u) {
        // the engine draws this body blended (SRCALPHA / INVSRCALPHA) over the scene: the same, drawn opaque here
        color = mix(refractionColor(w, uv), color, alpha);
    }
    if (underside) {
        float k = 1.0 - exp(-dist / 1500.0);
        color = mix(refr, w.mat[0].rgb * engineLight(w, vec3(0.0, 1.0, 0.0), vec3(0.0, 1.0, 0.0), outdoor, P, uv), k);
    }

    // debug views (water.debug): 1 shore field distance, 2 depth, 3 normals, 4 reflection only, 5 foam / surf zone / depth
    uint dbg = w.res.w;
    if (dbg == 1u) color = vec3(fract(tc7.z / 1000.0), clamp(tc7.z / 30000.0, 0.0, 1.0), sea ? 1.0 : 0.0);
    else if (dbg == 2u) color = vec3(clamp(vdepth / 500.0, 0.0, 1.0), clamp(thick0 / 3000.0, 0.0, 1.0), s.alpha);
    else if (dbg == 3u) color = normalize(s.N) * 0.5 + 0.5;
    else if (dbg == 4u) color = s.refl;
    else if (dbg == 5u) color = vec3(foam, (1.0 - smoothstep(0.0, 450.0, tc7.z)), clamp(vdepth / 200.0, 0.0, 1.0));
    outColor = vec4(max(color, vec3(0.0)), 1.0);
}
