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

#define WATER_FRAGMENT
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
// (wrap > 0: the sun's light wrapped around the normal, as through a scattering layer; ambK / sunK scale the ambient and the
// sun before the curve: foam)
vec3 engineLightEx(WaterBlock w, vec3 N, vec3 Ngeo, float outdoor, vec3 P, vec2 uv, float wrap, float ambK, float sunK) {
    vec4 n1 = vec4(N, 1.0);
    vec4 n2 = N.yzzx * N.xyzz;
    vec3 sh = vec3(dot(w.sh[0], n1) + dot(w.sh[3], n2), dot(w.sh[1], n1) + dot(w.sh[4], n2), dot(w.sh[2], n1) + dot(w.sh[5], n2)) +
              w.sh[6].rgb * (N.x * N.x - N.y * N.y);
    vec3 amb = mix(w.indoor.rgb, sh, outdoor) * ambK;
    vec3 L = w.sunDir.xyz;
    float nl = clamp((dot(L, N) + wrap) / (1.0 + wrap), 0.0, 1.0);
    float up = clamp(dot(L, Ngeo) * 4.0, 0.0, 1.0);
    vec3 l = amb + w.sunColor.rgb * nl * up * sunK;
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

vec3 engineLight(WaterBlock w, vec3 N, vec3 Ngeo, float outdoor, vec3 P, vec2 uv) { return engineLightEx(w, N, Ngeo, outdoor, P, uv, 0.0, 1.0, 1.0); }

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
    // the vertical depth over the ground without the refraction offset (smooth: foam, the thin water at the waterline and
    // the soft edge follow it)
    bool sky0 = sceneZ0 >= far * 0.995;
    float vdepth0 = sky0 ? 1e6 : max(P.y - rvkWorldFromViewDepth(f, uv, sceneZ0).y, 0.0);
    // the soft edge where the water meets the ground: at least a few pixels wide (no hard line, no stair steps)
    float edgeD = min(vdepth0 * 2.0, thick0);
    float softEdgeW = clamp(2.0 * fwidth(edgeD), 12.0, 30.0);

    vec3 Ngeo = tc4.xyz;
    vec3 T = tc5.xyz;
    vec3 B = tc6.xyz;
    bool fft = grid && fftMode(w);
    SeaFloor fl;
    ShoreTrain st;
    FftSample fs;
    float lostVar = 0.0;
    vec4 fftWt = vec4(0.0);
    vec3 Nmacro = vec3(0.0, 1.0, 0.0);
    vec3 Nfres = vec3(0.0, 1.0, 0.0);
    if (fft) {
        // per pixel: the FFT cascades' slopes (filtered by the screen footprint) with the same attenuation as the geometry,
        // + the waves rolling in; the slope variance the filtering removed becomes specular roughness
        fl = seaFloor(w, tc7.xy);
        float cap = nearCameraCap(w, tc7.xy, pc.vs.c[11].xyz);
        // the cap flattens the geometry near the camera; the short waves' normals stay (their displacement is small anyway)
        fftWt = cascadeWeights(w, fl) * vec4(cap, cap, 1.0, 1.0);
        vec4 macro;
        fs = fftSamplePixel(w, tc7.xy, fftWt, lostVar, macro);
        st = shoreTrain(w, fl, t);
        vec2 slope = fs.slope + st.slope * cap;
        Nmacro = normalize(vec3(-(macro.z + st.slope.x * cap), 1.0, -(macro.w + st.slope.y * cap)));
        // the Fresnel weight's surface: the long swell nearly at its full tilt (its slopes are gentle, and they make the swell
        // read as light and dark bands towards the horizon), the shorter macro waves and the train damped to 40 % (at grazing
        // views their faces turned away hide behind the crests: the eye sees the sky's reflection over most of the sea)
        vec2 fsl = macro.xy * 0.85 + (macro.zw - macro.xy) * 0.4 + st.slope * cap * 0.4;
        Nfres = normalize(vec3(-fsl.x, 1.0, -fsl.y));
        // sheltered water (harbour basins, coves) is calm, not dead: gusts ruffle it into short, drifting wind ripples
        // (the spectrum's short waves are weak on a calm day; the basin keeps them while the swell is gone)
        // (a little everywhere: the open sea's short chop between the spectrum's waves; they also break up the traced
        // reflections of walls and hulls, which follow the calmer macro surface)
        float shelteredN = 1.0 - smoothstep(0.2, 0.8, fl.exposure);
        float gust = 0.6 + 0.4 * smoothstep(-0.3, 0.5, waterNoise(tc7.xy / 2500.0 + vec2(t * 0.02, -t * 0.013)));
        slope += rippleSlope(tc7.xy, t, 170.0, 3) * ((0.04 + 0.14 * shelteredN) * gust / (1.0 + dist / 4000.0));
        Ngeo = normalize(vec3(-slope.x, 1.0, -slope.y));
        T = normalize(w.meshFrame.xyz - Ngeo * dot(Ngeo, w.meshFrame.xyz));
        B = cross(Ngeo, T) * w.meshFrame.w;
    } else if (grid) {
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
    float fresW = 0.0;              // the ocean model's Fresnel weight (FFT: the body / transmitted split)
    vec3 bodyD = vec3(0.0);         // the ocean model's body colour (its diffuse textures)
    vec3 seaTint = w.mat[0].rgb;  // the material's water colour (ocean: its diffuse textures)
    vec3 L = w.sunDir.xyz;
    vec4 c6 = w.mat[0], c7 = w.mat[1], c8 = w.mat[2], c9 = w.mat[3], c10 = w.mat[4], c11 = w.mat[5], c12 = w.mat[6];
    float reflK = w.look.x;  // water.reflection_strength

    if (model == WATER_MODEL_OCEAN) {
        // ps 6e22a01a6cf0ac0a: s14 / s13 normal maps blended by the mask s9, s11 * s10 diffuse, s12 sky cube
        vec4 mask = mtex(w, 9u, tc1.zw);
        vec2 n01 = mix(nmap(mtex(w, 13u, tc1.xy)), nmap(mtex(w, 14u, tc0.xy)), mask.wy);
        // the FFT's own small waves carry the detail: the material's normal maps weaker on top where they are (its look stays;
        // full strength in sheltered water)
        // (further away the FFT's short waves are filtered out, the material's maps carry the texture again)
        // and at grazing views, where the material's sparkle on the sky reflection is the engine's look)
        if (fft) {
            float damp = 0.3 * clamp(max(fftWt.y, fftWt.z), 0.0, 1.0) * (1.0 - smoothstep(1500.0, 6000.0, dist)) *
                         smoothstep(0.08, 0.35, dot(Ngeo, V));
            n01 = mix(vec2(0.5), n01, 1.0 - damp);
        }
        s.N = frameNormal(n01, T, B, Ngeo, s.nxy);
        vec3 lit = engineLight(w, s.N, Ngeo, outdoor, P, uv) * shadow;
        vec3 R = 2.0 * dot(V, s.N) * s.N - V;
        // on wave slopes facing away at grazing views the mirror direction dips below the horizon: it would meet the
        // next wave, which reflects the sky near the horizon, not the cube's dark lower half
        if (fft) R = normalize(vec3(R.x, max(R.y, 0.04), R.z));
        float nv = dot(s.N, V);
        float nl = clamp(dot(L, s.N), 0.0, 1.0);
        float up = clamp(dot(L, Ngeo) * 4.0, 0.0, 1.0);
        vec3 cube = mcube(w, 12u, R).rgb;
        float rl = clamp(dot(R, L), 0.0, 1.0);
        float specPow = c8.w * 128.0;
        float specNorm = 1.0;
        if (fft) {
            // the waves smaller than a pixel widen the highlight instead of sparkling (Toksvig-like: the filtered slope
            // variance added to the lobe's), energy kept
            float p2 = specPow / (1.0 + specPow * 2.0 * lostVar);
            specNorm = max((p2 + 2.0) / (specPow + 2.0), 0.6);
            specPow = p2;
        }
        vec3 spec = clamp(dpow(rl, specPow) * specNorm * nl / max(nv, 1e-5), 0.0, 1.0) * w.sunSpec.rgb * up * clamp(cube * c8.x, 0.0, 1.0);
        // FFT: the Fresnel weight of a calmer surface than the shading normal: at grazing views the wave faces turned away
        // are hidden behind the crests and the eye sees the sky's reflection over most of the sea (as the engine's flat
        // sea does); the short waves still bend the reflected ray and the specular
        float nvF = fft ? dot(Nfres, V) : nv;
        float fres = (c9.y * dpow(1.0 - nvF, c9.z) + c9.x) * reflK;
        if (fft) {
            // the thin water at the waterline (the swash's last centimetres over the sand) is a broken, rough film rather
            // than a mirror: less of the sky, the sand shows through (no grey band along the beach)
            float thinW = mix(0.3, 1.0, smoothstep(3.0, 35.0, vdepth0));
            fres *= thinW;
            spec *= thinW;
        }
        vec3 diffuse = mtex(w, 10u, tc0.zw).rgb * mtex(w, 11u, tc1.xy).rgb * c8.y;
        seaTint = diffuse;
        vec3 refl = cube;
        if (!underside) {
            // FFT: the traced ray follows the macro surface (the short waves would scatter it into the dark rocks / sea)
            vec3 Nssr = fft ? Nmacro : Ngeo;
            float conf;
            vec3 ssr = traceSSR(w, f, P, ssrDir(s.N, Nssr, V), ssrJitter(s.N, Nssr, dist), conf);
            refl = keepSky(cube, min(ssr, vec3(1.5)), conf, 0.55);
        }
        s.refl = refl;
        s.colour = c6.rgb * lit + spec + clamp(mix(diffuse, refl, clamp(fres, 0.0, 1.0)), 0.0, 1.5);
        fresW = clamp(fres, 0.0, 1.0);
        bodyD = diffuse;
        if (fft && !underside) {
            // light through the thin crests (subsurface scattering): backlit by the sun when looking towards it, in the
            // material's own turquoise; a little sky light in every crest
            float crestH = clamp(tc5.w, 0.0, 1.0);
            float back = dpow(clamp(dot(-V, L) * 0.5 + 0.5, 0.0, 1.0), 4.0);
            float thin = 1.0 - clamp(nv, 0.0, 1.0);
            vec3 sssCol = diffuse * 1.4;
            vec3 amb = engineLight(w, vec3(0.0, 1.0, 0.0), vec3(0.0, 1.0, 0.0), outdoor, P, uv);
            s.colour += sssCol * (w.sunColor.rgb * shadow * back * (0.35 + 0.65 * thin) * 0.22 + amb * 0.10) * crestH;
        }
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

    // ---- volume (the sea): what lies under the water loses red, then green, along the light's way through it ----
    float path = 1e6;
    if (sea && !skyBehind && !underside) {
        float nv = clamp(dot(normalize(Ngeo), V), 0.0, 1.0);
        float sinT = sqrt(max(1.0 - nv * nv, 0.0)) / 1.333;
        float cosT = sqrt(max(1.0 - sinT * sinT, 0.0));
        path = vdepth / max(cosT, 0.25) + vdepth;  // down to the ground and back up to the eye
        refr *= exp(-vec3(0.0016, 0.0007, 0.0005) * w.misc.w * path);
    }
    if (fft && model == WATER_MODEL_OCEAN && !underside) {
        // FFT ocean: the water's transparency follows the light's real way through it (refracted down to the floor and
        // back), not the view ray's length (the engine's depth fade, which turns even 1 m of water opaque at grazing views):
        // what is not reflected (1 - Fresnel) shows the lit sea floor through the water where it is shallow (turquoise over
        // the sand of the shelf) and the water's own body colour where it is deep. Deep water: the engine's model as is
        float T = exp(-0.0011 * w.misc.w * path);
        s.colour += (1.0 - fresW) * T * (refr - bodyD);
        s.alpha = tc3.w;
    }

    // ---- foam (the sea only): surf lines, contact with the beach and walls, wave crests ----
    float foam = 0.0;
    float milk = 0.0;  // aerated, sandy water of the surf zone (FFT ocean)
    vec4 foamTerms = vec4(0.0);
    float foamH = 0.0;          // FFT ocean: the foam's thickness (0..1), its gradient (world units^-1), freshness
    vec2 foamGrad = vec2(0.0);
    float foamFresh = 0.0;
    float foamFoot = 1.0;       // world units per pixel
    if (fft && w.look.y > 0.0 && !underside) {
        // FFT ocean foam (docs/research/ocean.md 4.4): coverages (0..1) turned into foam by one pattern attached to the water
        // (the undisplaced position): low coverage = lace with round holes, high = dense white water.
        //   whitecaps  where the simulated surface folds (Jacobian, accumulated over time in the tiles)
        //   white      a crest plunging where it breaks, the bores rolling on across the surf zone
        //   residual   what the white water leaves behind, drifting towards the beach and dissolving into lace
        //   contact    walls, rocks, hulls (sloshing), the swash's edge on the beach (a bright front running up, a thin lace
        //              as it drains back)
        // white / residual / contact come from the foam buffer (ocean_foam.comp: accumulated per cell over time, advected)
        // near the camera; further out from the same terms evaluated in place.
        vec2 xz0 = tc7.xy;
        float fineFade = 1.0 / (1.0 + dist / 3000.0);
        vec2 dX = dFdx(xz0), dY = dFdy(xz0);  // world units per pixel (the foam's soft edges follow the screen)
        float footprint = max(max(length(dX), length(dY)), 0.01);
        float fpMin = max(min(length(dX), length(dY)), 0.01);
        Surf su = surfAt(w.shoreWave, fl, st, xz0, t);
        float wetHere = smoothstep(3.0, 12.0, vdepth0);  // a rock or a swimmer just under the surface is not surf
        float cCap = clamp(fs.foam * 1.3, 0.0, 1.0) * 0.8;
        float cWhite = max(su.breakW * su.front, su.surfZone * su.front * mix(0.25, 0.6, su.patchy) * mix(0.5, 1.0, st.breaking));
        float cRes = su.surfZone * (0.06 + 0.2 * su.trail) * mix(0.4, 1.0, su.patchy);
        float cCon = 0.0;
        if (w.foamTex.x != 0u) {
            vec2 rel = (xz0 - w.foamMap.xy) * w.foamMap.w;
            float inBuf = smoothstep(0.0, 0.06, min(min(rel.x, rel.y), min(1.0 - rel.x, 1.0 - rel.y)));
            if (inBuf > 0.0) {
                vec4 fb = rvkSample2DLod(w.foamTex.x, w.foamTex.y, xz0 * w.foamMap.w, 0.0);
                cWhite = mix(cWhite, fb.r, inBuf);
                cRes = mix(cRes, fb.g, inBuf);
                cCon = fb.b * inBuf;
            }
        }
        cWhite *= wetHere;
        cRes *= wetHere;
        // the swash zone (the last metre of water before the beach): always broken water (by the sea floor's depth)
        float swashZone = wetHere * (1.0 - smoothstep(35.0, 110.0, max(fl.depth, 0.0))) * (1.0 - smoothstep(600.0, 1500.0, fl.overlay));
        cRes = max(cRes, swashZone * (0.12 + 0.18 * su.patchy));
        // sheltered water (harbour basins, coves, where the engine painted its moving foam): faint streaks of old foam and
        // scum drifting on the calm water, lines along the wind
        float sheltered = (1.0 - smoothstep(0.2, 0.7, fl.exposure)) * (1.0 - smoothstep(300.0, 1500.0, fl.overlay));
        float streak = smoothstep(0.3, 0.8, 0.5 + 0.5 * waterNoise(xz0 / vec2(620.0, 140.0) + vec2(t * 0.012, -t * 0.03)));
        float cStreak = sheltered * 0.3 * streak;
        // the swash's edge: the water's last centimetres on the beach (vertical depth) or at a wall (the view ray's short way
        // through the water); wider and denser while the swash runs up, a thin lace while it drains back
        float swPhase = 6.2831853 / max(w.shoreWave.x, 2.0) * t + waterNoised(xz0 * 0.00035).x * 0.8;
        float uprush = smoothstep(-0.2, 0.7, cos(swPhase));
        float contactD = min(vdepth0 * 1.5, thick0);
        float edgeW = mix(5.0, 15.0, uprush);
        float contactW = (1.0 - smoothstep(1.0, edgeW, contactD)) * smoothstep(1.0, 3.0, contactD + 2.0);
        cCon = max(cCon * 0.75, contactW * mix(0.4, 0.9, uprush));
        // one coverage (the union of the terms) and its freshness (dense white water and the swash's rim vs old lace)
        float cAll = 1.0 - (1.0 - cWhite * (0.55 + 0.45 * cWhite)) * (1.0 - cRes * 0.7) * (1.0 - cCap) * (1.0 - cCon * 0.85) * (1.0 - cStreak * 0.4);
        if (cAll > 0.004) {
            foamFresh = clamp((cWhite + 0.8 * cCon + 0.6 * cCap) / max(cAll, 1e-3), 0.0, 1.0);
            // the pattern drifts with the water as the foam buffer advects the coverage (towards the beach, faster in the
            // surf zone): two phases of a flow map, switched region by region at different moments (no global pulse)
            float coh = length(fl.dir);
            vec2 vel = (coh > 1e-4 ? fl.dir / coh : vec2(0.0)) * (smoothstep(0.05, 0.2, coh) * (20.0 + 90.0 * su.surfZone));
            const float FT = 6.0;
            float a0 = fract(t / FT);
            float m = smoothstep(0.42, 0.58, 1.0 - abs(2.0 * a0 - 1.0) + 0.4 * waterNoise(xz0 / 420.0 + vec2(4.1, 8.3)));
            vec2 g0 = vec2(0.0), g1 = vec2(0.0);
            float h0 = 0.0, h1 = 0.0;
            if (m > 0.0) {
                FoamField ff = foamField(xz0 - vel * ((a0 - 0.5) * FT), fpMin, footprint);
                h0 = foamDensity(ff, cAll, foamFresh, dX, dY, g0);
            }
            if (m < 1.0) {
                float a1 = fract(a0 + 0.5);
                FoamField ff = foamField(xz0 - vel * ((a1 - 0.5) * FT) + vec2(731.0, -419.0), fpMin, footprint);
                h1 = foamDensity(ff, cAll, foamFresh, dX, dY, g1);
            }
            foamH = clamp(mix(h1, h0, m) * w.look.y, 0.0, 1.0);
            foamGrad = mix(g1, g0, m) * w.look.y;
        }
        foamFoot = footprint;
        foamTerms = vec4(cCap, cWhite, cRes, cCon);
        // the broken water is full of bubbles and stirred-up sand: milky turquoise rather than clear
        // (most right behind the breaking crests, clearing between the bores)
        milk = clamp(su.surfZone * wetHere * (0.12 + 0.3 * su.trail) + 0.5 * cWhite + 0.3 * cRes, 0.0, 1.0) * fineFade;
        milk = max(milk, swashZone * 0.4 * fineFade);
    } else if (sea && grid && w.look.y > 0.0 && !underside) {
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
    if (milk > 0.0) {
        // the surf zone's aerated water: the light scattered back by bubbles and sand, in the sea's own tint
        vec3 lit = engineLight(w, Ngeo, Ngeo, outdoor, P, uv) * shadow;
        float lum = dot(lit, vec3(0.2126, 0.7152, 0.0722));
        vec3 milkCol = mix(vec3(0.55 * lum), clamp(seaTint, 0.0, 1.0) * lum * 1.3, 0.55);
        // it lightens what lies under the surface; it never darkens the sky's reflection on top of it
        color = max(color, mix(color, mix(milkCol, w.fogColor.rgb, fog), milk * 0.45));
    }
    if (foam > 0.0) {
        // foam lies on top of the water: white-ish, lit like the beach (the engine's diffuse lighting curve, albedo ~0.7),
        // fogged like the surface; never glowing
        vec3 lit = engineLight(w, Ngeo, Ngeo, outdoor, P, uv) * shadow;
        // white: the light's brightness, a little of the sky's tint (the sun's warm colour made it look like sand)
        float lum = dot(lit, vec3(0.2126, 0.7152, 0.0722));
        // a little mottling (bubbles of different sizes)
        float mottle = 0.88 + 0.12 * waterNoise(tc7.xy / 23.0 + vec2(t * 0.05, 0.0));
        vec3 foamCol = 0.58 * mottle * vec3(lum);
        color = mix(color, mix(foamCol, w.fogColor.rgb, fog), foam);
        alpha = max(alpha, foam);
    }
    if (foamH > 0.0) {
        // FFT ocean foam: a layer of bubbles on the water, lit by the sun and the sky (the engine's light and curve) on its
        // own relief (thick lumps brighter on the sun's side, their lee and the hollows between them darker), the light
        // scattered through it (wrapped), thin foam translucent and tinted by the water under it; never flat white
        float h = foamH;
        float reliefK = 9.0 / (1.0 + foamFoot / 4.0);  // the relief fades where the pattern gets small on screen
        vec2 relief = foamGrad * reliefK;
        relief *= min(1.0, 0.7 / max(length(relief), 1e-4));  // gentle lumps, never a face turned away from the light
        vec3 Nf = normalize(normalize(Ngeo) + vec3(-relief.x, 0.0, -relief.y));
        float ao = mix(0.72, 1.0, smoothstep(0.1, 0.8, h));
        vec3 lf = engineLightEx(w, Nf, Ngeo, outdoor, P, uv, 0.45, ao, mix(0.75, 1.0, h) * shadow);
        float lum = dot(lf, vec3(0.2126, 0.7152, 0.0722));
        vec3 base = vec3(lum);  // neutral (the sun's warm colour made it read as sand)
        float albedo = mix(0.5, 0.78, smoothstep(0.1, 0.9, h));
        float tl = dot(seaTint, vec3(0.2126, 0.7152, 0.0722));
        vec3 thinTint = mix(vec3(1.0), clamp(seaTint / max(tl, 1e-3), vec3(0.0), vec3(1.4)), 0.12);
        vec3 fc = base * albedo * mix(thinTint, vec3(1.0), smoothstep(0.2, 0.8, h));
        // matte foam is brighter than the water it lies on (at grazing views the water mirrors the bright sky, in a storm the
        // light is dim: foam darker than the water read as dirt)
        fc *= max(1.0, 1.3 * dot(color, vec3(0.2126, 0.7152, 0.0722)) / max(dot(fc, vec3(0.2126, 0.7152, 0.0722)), 1e-4));
        // a shoulder on the foam's brightness: under a strong sun the lit foam would clip to flat white (the scene target
        // is shown without a tone curve); its shading stays, compressed towards ~0.9
        float fl0 = dot(fc, vec3(0.2126, 0.7152, 0.0722));
        if (fl0 > 0.55) fc *= (0.55 + 0.35 * (1.0 - exp(-(fl0 - 0.55) / 0.35))) / fl0;
        // (dense where thick, a translucent veil where thin)
        float alphaF = 1.0 - exp(-3.2 * h);
        color = mix(color, mix(fc, w.fogColor.rgb, fog), alphaF);
        alpha = max(alpha, alphaF);
    }
    // a soft edge where the water gets very thin (no seam on the beach)
    // (FFT ocean: a wider fade, at least a few pixels: the swash spreads the waterline over the flat beaches)
    float edgeK = smoothstep(0.0, fft ? softEdgeW : 5.0, edgeD);
    if (!sky0) color = mix(refractionColor(w, uv), color, edgeK);
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
    else if (dbg == 5u && fft) color = foamTerms.xyz + vec3(foamTerms.w * 0.5) + (any(isnan(foamTerms)) ? vec3(1.0, 1.0, 0.0) : vec3(0.0));
    else if (dbg == 5u) color = vec3(foam, (1.0 - smoothstep(0.0, 450.0, tc7.z)), clamp(vdepth / 200.0, 0.0, 1.0));
    else if (dbg == 6u && fft) color = vec3(clamp(fl.depth / 1000.0, 0.0, 1.0), fl.exposure, (0.5 + 0.5 * sin(st.phase)) * st.weight);
    else if (dbg == 7u && fft) color = vec3(fftWt.x, fftWt.y, fftWt.z) * 0.8 + vec3(st.breaking * st.weight * 0.2);
    outColor = vec4(max(color, vec3(0.0)), 1.0);
}
