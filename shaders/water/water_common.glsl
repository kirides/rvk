// Enhanced water (src/enh/water.cpp, docs/research/water.md): the block every water draw reads, waves, helpers.
// The draws go through the normal D3D9 draw path (bindless set 0, the 48-byte push block of rvk_abi.glsl); the push
// constant `draw` points at a WaterBlock instead of a DrawBlock. The block starts with a DrawBlock (RvkDraw: the engine
// draw's texture / sampler slots and the half-pixel fix), so the engine's own vertex shader (translated) runs unchanged on
// the engine's water meshes and the pixel shader samples the material's textures through the same slots.
// Mirror: enh::WaterBlock (static_assert'ed size).

#extension GL_EXT_buffer_reference_uvec2 : require

#include "rvk_abi.glsl"
#include "rvk_common.glsl"
#include "water_noise.glsl"
#include "ocean_shore.glsl"

layout(buffer_reference, std430, buffer_reference_align = 16) readonly buffer WaterBlock {
    // = RvkDraw (192 bytes): s0..s15 of the engine draw (bindless texture / sampler), posFix (read by the engine VS)
    uint tex[20];
    uint smp[20];
    vec4 posFix;
    uint alphaFunc;
    float alphaRef;
    uint tint;  // 0: the translated VS's tint path stays off
    uint pad1;
    // the engine draw's pixel constants
    vec4 camFar;      // c0 g_CamPosAndFar
    vec4 fogColor;    // c3 g_FogColor
    vec4 fogParams;   // c4 g_FogSettings (x offset, y per unit, z maximum)
    vec4 mat[8];      // c6..c13 g_ShaderData (the material: colour, fresnel, depth fade, specular, blends)
    vec4 indoor;      // c14 g_IndoorAmbientColor
    vec4 sh[7];       // c15..c21 g_AmbientLight (the engine's 9-coefficient ambient)
    vec4 sunColor;    // c22 g_DirLight.m_colDiffuse (HDR)
    vec4 sunSpec;     // c23 g_DirLight.m_colSpecular
    vec4 sunDir;      // c24 g_DirLight.m_vecDirection (towards the sun)
    // the sea grid: the engine ocean VS's texcoord transforms (vs c232..c237) and the sea mesh's texture mapping / frame
    vec4 vsTc[6];
    vec4 uvMapU;      // texcoord0.u = dot(vec3(x, z, 1), uvMapU.xyz)
    vec4 uvMapV;
    vec4 meshFrame;   // xyz the sea mesh's tangent (world), w its binormal sign
    vec4 meshExtra;   // x COLOR1.w (outdoor factor), y global alpha (c32.w)
    // ours
    vec4 ocean;       // x sea level, y wave height (world units), z lowering at the coast, w swash amplitude
    vec4 grid;        // x base spacing, y levels, z quads per side, w time (s)
    vec4 shoreMap;    // xy shore field corner (world xz), zw 1 / its extent
    vec4 look;        // x reflection strength (SSR, player), y foam strength, z extra reflection (models without one), w shore waves
    vec4 misc;        // x near-camera wave cap (world units), y coast band (world units), z SSR distance, w murkiness (the sea's tint)
    uvec4 res;        // x sky cube (tCube slot, 0 = none), y shore field (0 = none), z flags (WATER_*), w debug view
    uvec4 res2;       // x engine model (WATER_MODEL_*), y SSR steps (0 = cube only), z refraction copy slot, w depth map slot
    uvec4 smpOurs;    // bindless samplers: linear clamp, linear wrap, point clamp; w shadow mask slot (0 = none)
    vec4 falls[4];    // waterfalls near the camera: xz of the base, z radius (0 = none), w height of the base (whitewater)
    vec4 lights[12];  // the engine's point lights (g_PointLights c25 + 4 i, up to 4): position + 1 / range^2, colour, shadow-mask channel
    uvec4 lightInfo;  // x point lights (0..4)
    // the FFT ocean (src/enh/ocean.h) and the sea floor field (src/enh/sea_floor.h)
    vec4 fftLength;   // cascade tile sizes, world units (0 = none)
    uvec4 fftDisp;    // bindless slots: displacement (dx, dy, dz in m, foam)
    uvec4 fftDeriv;   // derivatives (d dy/dx, d dy/dz, d dx/dx, d dz/dz)
    vec4 fftSlopeVar; // slope variance of each cascade's band
    vec4 fftBand;     // representative wavelength of each cascade (world units)
    uvec4 fftInfo;    // x mode (0 Gerstner, 1 FFT), y cascades, z sampler slot, w N
    vec4 seaInfo;     // x Hs (world units), y wind-sea peak wavelength (world units), z wind angle, w sea-state level 0..3
    vec4 floorMap;    // floor field: xy corner (world xz), zw 1 / extent
    uvec4 floorTex;   // x field A, y field B (bindless t2D), z valid
    vec4 shoreWave;   // x period (s), y amplitude (world units), z breaker index, w unused
    vec4 player;      // xyz the player's position (eCWeatherAdmin), w 1 = known (else the near-water cap follows the camera)
    vec4 foamMap;     // the foam buffer (ocean_foam.comp): xy its first cell (world units), z its extent (world units), w 1 / extent
    uvec4 foamTex;    // x the foam buffer (bindless t2D, 0 = none), y its sampler (linear, wrap)
};

const uint WATER_OCEAN_GRID = 1u;  // the camera-centred ocean grid (water_ocean.vert), shore + wave effects
const uint WATER_SEA = 2u;         // the open sea: shore foam, lowered coast
const uint WATER_BLENDED = 4u;     // the engine draw blends (SRCALPHA / INVSRCALPHA): emulated over the refraction copy
const uint WATER_UNDERSIDE = 16u;  // the camera is below this surface

// the engine's water pixel shaders (ge_default variants, docs/research/water.md 2.3): one model per material shader
const uint WATER_MODEL_OCEAN = 1u;  // Special_Water_Ocean: 2 normal maps + blend mask, diffuse textures, sky cube, shadow mask
const uint WATER_MODEL_LAKE = 2u;   // Special_Water_Island: 3 normal maps, sky cube masked by two reflection textures
const uint WATER_MODEL_SWAMP = 3u;  // Special_Water_Swamp: 2 normal maps, colour + specular, no reflection
const uint WATER_MODEL_RIVER = 4u;  // Special_Water_River: 2 normal maps, whitewater diffuse blend, sky cube

WaterBlock waterBlock() { return WaterBlock(uvec2(pc.draw)); }

// ---- the open sea's waves (Gerstner sum; FFT cascades are M8) ------------------------------------------------------------

const int WAVE_COUNT = 8;
// wavelength (world units = cm), direction angle offset from the wind (rad), relative amplitude, phase
const vec4 WAVES[WAVE_COUNT] = vec4[](
    vec4(5200.0, 0.00, 1.00, 0.0), vec4(3300.0, -0.45, 0.70, 1.3), vec4(2100.0, 0.55, 0.50, 4.1), vec4(1350.0, -0.95, 0.34, 2.2),
    vec4(860.0, 0.30, 0.22, 5.0), vec4(560.0, 1.15, 0.15, 0.7), vec4(370.0, -0.25, 0.10, 3.6), vec4(240.0, 0.80, 0.06, 1.9));
const float WIND_ANGLE = 0.65;
const float GRAVITY = 981.0;  // cm / s^2

// displacement (xyz) of the sea surface at undisplaced xz, `amp` = height of the largest wave; slope = d height / d xz;
// crest = steepness 0..1
vec3 gerstner(vec2 xz, float t, float amp, out vec2 slope, out float crest) {
    vec3 d = vec3(0.0);
    vec2 s = vec2(0.0);
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
        float c = cos(th), sn = sin(th);
        d.xz += q * a * dir * c;
        d.y += a * sn;
        s += dir * (k * a * c);
        steep += q * k * a * sn;
    }
    slope = s;
    crest = clamp(steep, 0.0, 1.0);
    return d;
}

// waves rolling in towards the coast: phase over the distance to the coast (the shore field), direction towards the coast;
// they take over from the open sea's waves inside the coast band, so the crests run parallel to the beach
vec3 shoreWaves(float coastDist, vec2 toCoast, float t, float amp, out vec2 slope, out float crest) {
    const float LEN = 1100.0;
    float k = 6.2831853 / LEN;
    float omega = sqrt(GRAVITY * k);
    float th = k * coastDist + omega * t;  // constant phase moves to smaller distances: towards the beach
    float c = cos(th), sn = sin(th);
    // a steeper front (sharper crest towards the beach): a second harmonic
    float h = amp * (sn + 0.25 * sin(2.0 * th - 0.8));
    float dh = amp * k * (c + 0.5 * cos(2.0 * th - 0.8));  // d h / d coastDist
    // d coastDist / d xz = -toCoast (toCoast points down the distance gradient)
    slope = -toCoast * dh;
    crest = smoothstep(0.3, 1.0, sn) * clamp(amp / 10.0, 0.0, 1.0);
    vec3 d = vec3(0.0, h, 0.0);
    d.xz = toCoast * (0.35 * amp * c);  // crests lean towards the beach
    return d;
}

// slope (d height / d x, d height / d z) of wind ripples at xz (world units), scale = world units per tile: three octaves
// drifting with time (the sea's chop between the waves, also in calm bays where the waves are damped)
vec2 rippleSlope(vec2 xz, float t, float scale, int octaves) {
    vec2 slope = vec2(0.0);
    float f = 1.0 / scale;
    float a = 1.0;
    vec2 drift = vec2(0.71, 0.43);
    for (int i = 0; i < octaves; ++i) {
        vec2 p = xz * f + drift * (t * (0.35 + 0.15 * float(i)));
        slope += a * waterNoised(p).yz;
        f *= 2.03;
        a *= 0.5;
        drift = vec2(-drift.y, drift.x * 1.1);
    }
    return slope;
}

// ---- shore field (enh/shore_field.h): distance to the engine's coast overlay, its height, direction to the coast ---------

vec4 shoreField(WaterBlock w, vec2 xz) {
    if (w.res.y == 0u) return vec4(30000.0, -30000.0, 0.0, 0.0);
    vec2 uv = (xz - w.shoreMap.xy) * w.shoreMap.zw;
    if (any(lessThan(uv, vec2(0.0))) || any(greaterThan(uv, vec2(1.0)))) return vec4(30000.0, -30000.0, 0.0, 0.0);
    return rvkSample2DLod(w.res.y, w.smpOurs.x, uv, 0.0);
}

// the sea surface's height offset at the coast: the engine's fake shore is replaced by a lower waterline that breathes
// (swash running up the actual beach and back), out to the coast band from the coast overlay
float coastOffset(WaterBlock w, float coastDist, float t, vec2 xz) {
    // full at the engine's coast overlay (the beach), gone a little further out (quays and cliffs keep the engine's level)
    float band = max(w.misc.y, 1.0);
    float near = 1.0 - smoothstep(0.2 * band, band, coastDist);
    float along = waterNoised(xz * 0.00035).x * 6.0;
    float swash = sin(t * 0.55 + coastDist * 0.0035 + along);
    return near * (-w.ocean.z + w.ocean.w * swash);
}

// wave height scale at xz: damped near the coast and near the camera (the player wades and swims in a near-flat sea)
float waveScale(WaterBlock w, float coastDist, vec2 xz, vec3 cameraPos) {
    float coast = smoothstep(300.0, 4500.0, coastDist);
    float cam = length(xz - cameraPos.xz);
    float capFrac = clamp(w.misc.x / max(w.ocean.y, 1.0), 0.0, 1.0);
    float nearCam = mix(capFrac, 1.0, smoothstep(700.0, 3000.0, cam));
    return w.ocean.y * coast * nearCam;
}

// the shore waves' height at xz: grow from the surf zone outwards, give way to the open sea's waves further out
float shoreWaveScale(WaterBlock w, float coastDist, vec2 toCoast, vec2 xz, vec3 cameraPos) {
    float zone = smoothstep(250.0, 900.0, coastDist) * (1.0 - smoothstep(2500.0, 5000.0, coastDist));
    float dirOk = smoothstep(0.1, 0.35, length(toCoast));  // ridges of the distance field (coves, points): no direction
    float cam = length(xz - cameraPos.xz);
    float nearCam = mix(0.6, 1.0, smoothstep(500.0, 2000.0, cam));
    return w.look.w * zone * dirOk * nearCam;
}

#include "ocean_surface.glsl"
