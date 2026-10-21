// Enhanced water (src/enh/water.cpp, docs/research/water.md): the block every water draw reads, waves, ripples, helpers.
// The draws go through the normal D3D9 draw path (bindless set 0, the 48-byte push block of rvk_abi.glsl); the push
// constant `draw` points at a WaterBlock instead of a DrawBlock. Mirror: enh::WaterBlock (static_assert'ed size).

#extension GL_EXT_buffer_reference_uvec2 : require

#include "rvk_abi.glsl"
#include "rvk_common.glsl"

layout(buffer_reference, std430, buffer_reference_align = 16) readonly buffer WaterBlock {
    vec4 posFix;      // xy: the half-pixel fix of the D3D9 path (NDC units of the viewport)
    vec4 sunColor;    // engine c22: g_DirLight.m_colDiffuse (HDR)
    vec4 sunSpec;     // engine c23: g_DirLight.m_colSpecular
    vec4 sunDir;      // engine c24: g_DirLight.m_vecDirection (towards the sun)
    vec4 fogColor;    // engine c3: g_FogColor
    vec4 fogParams;   // engine c4: g_FogSettings (x offset, y per unit, z maximum)
    vec4 ambient[3];  // engine c15..c17: the linear part of g_AmbientLight
    vec4 scatter;     // rgb in-water scattering colour (the material's c6), a refraction strength
    vec4 absorb;      // rgb extinction per world unit, a reflectivity
    vec4 surface;     // x roughness, y glint, z ripple strength, w ripple scale (world units)
    vec4 foam;        // rgb foam tint, a foam strength (0: no foam)
    vec4 ocean;       // x sea level, y wave height (world units), z lowering at the coast, w swash amplitude
    vec4 grid;        // x base spacing, y levels, z quads per side, w time (s)
    vec4 shoreMap;    // xy shore field corner (world xz), zw 1 / its extent
    vec4 misc;        // x sky cube scale, y near-camera cap (world units of wave height), z coast band (world units), w SSR distance
    uvec4 tex;        // bindless: refraction copy, depth map, sky cube (tCube), shore field
    uvec4 tex2;       // x shadow mask (0 = none), y SSR steps (0 = cube only), z flags (WATER_*), w debug view
    uvec4 smp;        // bindless samplers: linear clamp, linear wrap, point clamp
    vec4 falls[4];    // waterfalls near the camera: xz of the base, z radius (0 = none), w height of the base (whitewater)
    vec4 lights[12];  // up to 4 engine point lights (g_PointLights c25+): position + 1 / range^2 (w 0 = none), diffuse, specular
    vec4 look;        // x turbidity (the lit water colour over what is seen through it), yzw 0
};

const uint WATER_OCEAN_GRID = 1u;  // the camera-centred ocean grid (water_ocean.vert), shore + wave effects
const uint WATER_SEA = 2u;         // the open sea: shore foam, lowered coast
const uint WATER_INSTANCED = 4u;   // the engine mesh is instanced (world matrix from g_vInstanceData[i * 4 + 1])
const uint WATER_SHORE_FIELD = 8u; // tex.w holds the shore field
const uint WATER_UNDERSIDE = 16u;  // the camera is below this surface

WaterBlock waterBlock() { return WaterBlock(uvec2(pc.draw)); }

// ---- the open sea's waves (Gerstner sum; FFT cascades are M8) ------------------------------------------------------------

const int WAVE_COUNT = 8;
// wavelength (world units = cm), direction angle offset from the wind (rad), relative amplitude, phase
const vec4 WAVES[WAVE_COUNT] = vec4[](
    vec4(5200.0, 0.00, 1.00, 0.0), vec4(3300.0, -0.45, 0.70, 1.3), vec4(2100.0, 0.55, 0.50, 4.1), vec4(1350.0, -0.95, 0.34, 2.2),
    vec4(860.0, 0.30, 0.22, 5.0), vec4(560.0, 1.15, 0.15, 0.7), vec4(370.0, -0.25, 0.10, 3.6), vec4(240.0, 0.80, 0.06, 1.9));
const float WIND_ANGLE = 0.65;
const float GRAVITY = 981.0;  // cm / s^2

// displacement (xyz) of the sea surface at undisplaced xz, `amp` = height of the largest wave; dir = the coast direction
// blend (0 = wind only). foam: crest steepness 0..1
vec3 gerstner(vec2 xz, float t, float amp, out vec3 normal, out float crest) {
    vec3 d = vec3(0.0);
    vec3 n = vec3(0.0, 1.0, 0.0);
    float steep = 0.0;
    for (int i = 0; i < WAVE_COUNT; ++i) {
        vec4 w = WAVES[i];
        float k = 6.2831853 / w.x;
        float a = amp * w.z * 0.5;
        float ang = WIND_ANGLE + w.y;
        vec2 dir = vec2(cos(ang), sin(ang));
        float omega = sqrt(GRAVITY * k);
        float q = min(0.55 / (k * max(a, 1e-3) * float(WAVE_COUNT)), 1.0);
        float th = k * dot(dir, xz) - omega * t + w.w;
        float c = cos(th), s = sin(th);
        d.xz += q * a * dir * c;
        d.y += a * s;
        n.xz -= dir * (k * a * c);
        n.y -= q * k * a * s;
        steep += q * k * a * s;
    }
    normal = normalize(n);
    crest = clamp(steep, 0.0, 1.0);
    return d;
}

// ---- detail ripples: gradient noise with analytic derivatives (after Inigo Quilez, MIT) -----------------------------------

vec2 waterHash(vec2 p) {
    p = vec2(dot(p, vec2(127.1, 311.7)), dot(p, vec2(269.5, 183.3)));
    return -1.0 + 2.0 * fract(sin(p) * 43758.5453123);
}

// value, d/dx, d/dy
vec3 waterNoised(vec2 p) {
    vec2 i = floor(p);
    vec2 f = fract(p);
    vec2 u = f * f * f * (f * (f * 6.0 - 15.0) + 10.0);
    vec2 du = 30.0 * f * f * (f * (f - 2.0) + 1.0);
    vec2 ga = waterHash(i + vec2(0.0, 0.0));
    vec2 gb = waterHash(i + vec2(1.0, 0.0));
    vec2 gc = waterHash(i + vec2(0.0, 1.0));
    vec2 gd = waterHash(i + vec2(1.0, 1.0));
    float va = dot(ga, f - vec2(0.0, 0.0));
    float vb = dot(gb, f - vec2(1.0, 0.0));
    float vc = dot(gc, f - vec2(0.0, 1.0));
    float vd = dot(gd, f - vec2(1.0, 1.0));
    return vec3(va + u.x * (vb - va) + u.y * (vc - va) + u.x * u.y * (va - vb - vc + vd),
                ga + u.x * (gb - ga) + u.y * (gc - ga) + u.x * u.y * (ga - gb - gc + gd) +
                    du * (u.yx * (va - vb - vc + vd) + vec2(vb, vc) - va));
}

// slope (d height / d x, d height / d z) of the ripple field at xz (world units), scale = world units per tile
vec2 rippleSlope(vec2 xz, float t, float scale, int octaves) {
    vec2 slope = vec2(0.0);
    float f = 1.0 / scale;
    float a = 1.0;
    vec2 drift = vec2(0.71, 0.43);
    for (int i = 0; i < octaves; ++i) {
        vec2 p = xz * f + drift * (t * (0.35 + 0.15 * float(i)));
        vec3 n = waterNoised(p);
        slope += a * n.yz;
        f *= 2.03;
        a *= 0.5;
        drift = vec2(-drift.y, drift.x * 1.1);
    }
    return slope;
}

// ---- shore field (enh/shore_field.h): distance to the engine's coast overlay, its height, direction to the coast ---------

vec4 shoreField(WaterBlock w, vec2 xz) {
    if ((w.tex2.z & WATER_SHORE_FIELD) == 0u) return vec4(30000.0, -30000.0, 0.0, 0.0);
    vec2 uv = (xz - w.shoreMap.xy) * w.shoreMap.zw;
    if (any(lessThan(uv, vec2(0.0))) || any(greaterThan(uv, vec2(1.0)))) return vec4(30000.0, -30000.0, 0.0, 0.0);
    return rvkSample2DLod(w.tex.w, w.smp.x, uv, 0.0);
}

// the sea surface's height offset at the coast: the engine's fake shore is replaced by a lower waterline that breathes
// (swash running up the actual beach and back), out to `band` world units from the coast overlay
float coastOffset(WaterBlock w, float coastDist, float t, vec2 xz) {
    float band = max(w.misc.z, 1.0);
    float near = 1.0 - smoothstep(0.0, band, coastDist);
    float along = waterNoised(xz * 0.00035).x * 6.0;
    float swash = sin(t * 0.55 + coastDist * 0.0035 + along);
    return near * (-w.ocean.z + w.ocean.w * swash);
}

// wave height scale at xz: damped near the coast and near the camera (the player wades and swims in a near-flat sea)
float waveScale(WaterBlock w, float coastDist, vec2 xz, vec3 cameraPos) {
    float coast = smoothstep(300.0, 4500.0, coastDist);
    float cam = length(xz - cameraPos.xz);
    float capFrac = clamp(w.misc.y / max(w.ocean.y, 1.0), 0.0, 1.0);
    float nearCam = mix(capFrac, 1.0, smoothstep(700.0, 3000.0, cam));
    return w.ocean.y * coast * nearCam;
}
