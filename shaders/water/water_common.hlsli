// Enhanced water (src/enh/water.cpp, docs/research/water.md): the block every water draw reads, waves, helpers.
// The draws go through the normal D3D9 draw path (bindless set 0, the push block of rvk_draw.hlsli); the push
// constant `draw` points at a WaterBlock instead of a DrawBlock. The block starts with a DrawBlock (RvkDraw: the engine
// draw's texture / sampler slots and the half-pixel fix), so the engine's own vertex shader (translated) runs unchanged on
// the engine's water meshes and the pixel shader samples the material's textures through the same slots.
// Mirror: enh::WaterBlock (static_assert'ed size). Port of water_common.glsl.
#pragma once

#include "rvk_draw.hlsli"
#include "water_noise.hlsli"
#include "ocean_shore.hlsli"

// All members are float4 / uint4 (arrays of them): the layout is the GLSL block's std430 layout.
struct WaterBlockData {
    // = RvkDraw (192 bytes): s0..s15 of the engine draw (bindless texture / sampler), posFix (read by the engine VS)
    uint4 tex[5];     // tex[s >> 2][s & 3] (GLSL: uint tex[20])
    uint4 smp[5];     // smp[s >> 2][s & 3] (GLSL: uint smp[20])
    float4 posFix;
    uint4 alphaTint;  // x alphaFunc, y alphaRef (float bits), z tint (0: the translated VS's tint path stays off), w pad1
    // the engine draw's pixel constants
    float4 camFar;      // c0 g_CamPosAndFar
    float4 fogColor;    // c3 g_FogColor
    float4 fogParams;   // c4 g_FogSettings (x offset, y per unit, z maximum)
    float4 mat[8];      // c6..c13 g_ShaderData (the material: colour, fresnel, depth fade, specular, blends)
    float4 indoor;      // c14 g_IndoorAmbientColor
    float4 sh[7];       // c15..c21 g_AmbientLight (the engine's 9-coefficient ambient)
    float4 sunColor;    // c22 g_DirLight.m_colDiffuse (HDR)
    float4 sunSpec;     // c23 g_DirLight.m_colSpecular
    float4 sunDir;      // c24 g_DirLight.m_vecDirection (towards the sun)
    // the sea grid: the engine ocean VS's texcoord transforms (vs c232..c237) and the sea mesh's texture mapping / frame
    float4 vsTc[6];
    float4 uvMapU;      // texcoord0.u = dot(vec3(x, z, 1), uvMapU.xyz)
    float4 uvMapV;
    float4 meshFrame;   // xyz the sea mesh's tangent (world), w its binormal sign
    float4 meshExtra;   // x COLOR1.w (outdoor factor), y global alpha (c32.w)
    // ours
    float4 ocean;       // x sea level, y wave height (world units), z lowering at the coast, w swash amplitude
    float4 grid;        // x base spacing, y levels, z quads per side, w time (s)
    float4 shoreMap;    // xy shore field corner (world xz), zw 1 / its extent
    float4 look;        // x reflection strength (SSR, player), y foam strength, z extra reflection (models without one), w shore waves
    float4 misc;        // x near-camera wave cap (world units), y coast band (world units), z SSR distance, w murkiness (the sea's tint)
    uint4 res;          // x sky cube (tCube slot, 0 = none), y shore field (0 = none), z flags (WATER_*), w debug view
    uint4 res2;         // x engine model (WATER_MODEL_*), y SSR steps (0 = cube only), z refraction copy slot, w depth map slot
    uint4 smpOurs;      // bindless samplers: linear clamp, linear wrap, point clamp; w shadow mask slot (0 = none)
    float4 falls[4];    // waterfalls near the camera: xz of the base, z radius (0 = none), w height of the base (whitewater)
    float4 lights[12];  // the engine's point lights (g_PointLights c25 + 4 i, up to 4): position + 1 / range^2, colour, shadow-mask channel
    uint4 lightInfo;    // x point lights (0..4)
    // the FFT ocean (src/enh/ocean.h) and the sea floor field (src/enh/sea_floor.h)
    float4 fftLength;   // cascade tile sizes, world units (0 = none)
    uint4 fftDisp;      // bindless slots: displacement (dx, dy, dz in m, foam)
    uint4 fftDeriv;     // derivatives (d dy/dx, d dy/dz, d dx/dx, d dz/dz)
    float4 fftSlopeVar; // slope variance of each cascade's band
    float4 fftBand;     // representative wavelength of each cascade (world units)
    uint4 fftInfo;      // x mode (0 Gerstner, 1 FFT), y cascades, z sampler slot, w N
    float4 seaInfo;     // x Hs (world units), y wind-sea peak wavelength (world units), z wind angle, w sea-state level 0..3
    float4 floorMap;    // floor field: xy corner (world xz), zw 1 / extent
    uint4 floorTex;     // x field A, y field B (bindless t2D), z valid
    float4 shoreWave;   // x period (s), y amplitude (world units), z breaker index, w unused
    float4 player;      // xyz the player's position (eCWeatherAdmin), w 1 = known (else the near-water cap follows the camera)
    float4 foamMap;     // the foam buffer (ocean_foam.comp): xy its first cell (world units), z its extent (world units), w 1 / extent
    uint4 foamTex;      // x the foam buffer (bindless t2D, 0 = none), y its sampler (linear, wrap)
    // ray-traced reflections (rt.reflections): x this frame's reflection image of water_rt.comp (bindless t2D, 0 = none: SSR),
    // y 1 = the screen-space trace where it has nothing
    uint4 rtRefl;
};

static const uint WATER_OCEAN_GRID = 1u;  // the camera-centred ocean grid (water_ocean.vert), shore + wave effects
static const uint WATER_SEA = 2u;         // the open sea: shore foam, lowered coast
static const uint WATER_BLENDED = 4u;     // the engine draw blends (SRCALPHA / INVSRCALPHA): emulated over the refraction copy
static const uint WATER_UNDERSIDE = 16u;  // the camera is below this surface

// the engine's water pixel shaders (ge_default variants, docs/research/water.md 2.3): one model per material shader
static const uint WATER_MODEL_OCEAN = 1u;  // Special_Water_Ocean: 2 normal maps + blend mask, diffuse textures, sky cube, shadow mask
static const uint WATER_MODEL_LAKE = 2u;   // Special_Water_Island: 3 normal maps, sky cube masked by two reflection textures
static const uint WATER_MODEL_SWAMP = 3u;  // Special_Water_Swamp: 2 normal maps, colour + specular, no reflection
static const uint WATER_MODEL_RIVER = 4u;  // Special_Water_River: 2 normal maps, whitewater diffuse blend, sky cube

// a pointer to the block (GLSL: the WaterBlock buffer reference); read members through w.Get().member
typedef vk::BufferPointer<WaterBlockData, 16> WaterBlock;

WaterBlock waterBlock() { return rvkPtr<WaterBlockData, 16>(pc.draw); }

// bindless texture / sampler slot of the engine draw's sampler s (GLSL: w.tex[s], w.smp[s])
uint waterTexSlot(WaterBlock w, uint s) { return w.Get().tex[s >> 2][s & 3u]; }
uint waterSmpSlot(WaterBlock w, uint s) { return w.Get().smp[s >> 2][s & 3u]; }

// the cube map sampled through the bindless heap (GLSL: texture(samplerCube(tCube[nonuniformEXT(t)], samp[nonuniformEXT(s)]), d))
float4 waterSampleCube(uint tex, uint smp, float3 d) {
    return g_tCube[NonUniformResourceIndex(tex)].Sample(g_samp[NonUniformResourceIndex(smp)], d);
}
float4 waterSampleCubeLod(uint tex, uint smp, float3 d, float lod) {
    return g_tCube[NonUniformResourceIndex(tex)].SampleLevel(g_samp[NonUniformResourceIndex(smp)], d, lod);
}

// ---- the open sea's waves (Gerstner sum; FFT cascades are M8) ------------------------------------------------------------

static const int WAVE_COUNT = 8;
// wavelength (world units = cm), direction angle offset from the wind (rad), relative amplitude, phase
static const float4 WAVES[WAVE_COUNT] = {
    float4(5200.0, 0.00, 1.00, 0.0), float4(3300.0, -0.45, 0.70, 1.3), float4(2100.0, 0.55, 0.50, 4.1), float4(1350.0, -0.95, 0.34, 2.2),
    float4(860.0, 0.30, 0.22, 5.0), float4(560.0, 1.15, 0.15, 0.7), float4(370.0, -0.25, 0.10, 3.6), float4(240.0, 0.80, 0.06, 1.9)};
static const float WIND_ANGLE = 0.65;
static const float GRAVITY = 981.0;  // cm / s^2

// displacement (xyz) of the sea surface at undisplaced xz, `amp` = height of the largest wave; slope = d height / d xz;
// crest = steepness 0..1
float3 gerstner(float2 xz, float t, float amp, out float2 slope, out float crest) {
    float3 d = float3(0.0, 0.0, 0.0);
    float2 s = float2(0.0, 0.0);
    float steep = 0.0;
    for (int i = 0; i < WAVE_COUNT; ++i) {
        float4 w = WAVES[i];
        float k = 6.2831853 / w.x;
        float a = amp * w.z * 0.5;
        float ang = WIND_ANGLE + w.y;
        float2 dir = float2(cos(ang), sin(ang));
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
float3 shoreWaves(float coastDist, float2 toCoast, float t, float amp, out float2 slope, out float crest) {
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
    float3 d = float3(0.0, h, 0.0);
    d.xz = toCoast * (0.35 * amp * c);  // crests lean towards the beach
    return d;
}

// slope (d height / d x, d height / d z) of wind ripples at xz (world units), scale = world units per tile: three octaves
// drifting with time (the sea's chop between the waves, also in calm bays where the waves are damped)
float2 rippleSlope(float2 xz, float t, float scale, int octaves) {
    float2 slope = float2(0.0, 0.0);
    float f = 1.0 / scale;
    float a = 1.0;
    float2 drift = float2(0.71, 0.43);
    for (int i = 0; i < octaves; ++i) {
        float2 p = xz * f + drift * (t * (0.35 + 0.15 * float(i)));
        slope += a * waterNoised(p).yz;
        f *= 2.03;
        a *= 0.5;
        drift = float2(-drift.y, drift.x * 1.1);
    }
    return slope;
}

// ---- shore field (enh/shore_field.h): distance to the engine's coast overlay, its height, direction to the coast ---------

float4 shoreField(WaterBlock w, float2 xz) {
    if (w.Get().res.y == 0u) return float4(30000.0, -30000.0, 0.0, 0.0);
    float2 uv = (xz - w.Get().shoreMap.xy) * w.Get().shoreMap.zw;
    if (any(uv < float2(0.0, 0.0)) || any(uv > float2(1.0, 1.0))) return float4(30000.0, -30000.0, 0.0, 0.0);
    return rvkSample2DLod(w.Get().res.y, w.Get().smpOurs.x, uv, 0.0);
}

// the sea surface's height offset at the coast: the engine's fake shore is replaced by a lower waterline that breathes
// (swash running up the actual beach and back), out to the coast band from the coast overlay
float coastOffset(WaterBlock w, float coastDist, float t, float2 xz) {
    // full at the engine's coast overlay (the beach), gone a little further out (quays and cliffs keep the engine's level)
    float band = max(w.Get().misc.y, 1.0);
    float near = 1.0 - smoothstep(0.2 * band, band, coastDist);
    float along = waterNoised(xz * 0.00035).x * 6.0;
    float swash = sin(t * 0.55 + coastDist * 0.0035 + along);
    return near * (-w.Get().ocean.z + w.Get().ocean.w * swash);
}

// wave height scale at xz: damped near the coast and near the camera (the player wades and swims in a near-flat sea)
float waveScale(WaterBlock w, float coastDist, float2 xz, float3 cameraPos) {
    float coast = smoothstep(300.0, 4500.0, coastDist);
    float cam = length(xz - cameraPos.xz);
    float capFrac = clamp(w.Get().misc.x / max(w.Get().ocean.y, 1.0), 0.0, 1.0);
    float nearCam = lerp(capFrac, 1.0, smoothstep(700.0, 3000.0, cam));
    return w.Get().ocean.y * coast * nearCam;
}

// the shore waves' height at xz: grow from the surf zone outwards, give way to the open sea's waves further out
float shoreWaveScale(WaterBlock w, float coastDist, float2 toCoast, float2 xz, float3 cameraPos) {
    float zone = smoothstep(250.0, 900.0, coastDist) * (1.0 - smoothstep(2500.0, 5000.0, coastDist));
    float dirOk = smoothstep(0.1, 0.35, length(toCoast));  // ridges of the distance field (coves, points): no direction
    float cam = length(xz - cameraPos.xz);
    float nearCam = lerp(0.6, 1.0, smoothstep(500.0, 2000.0, cam));
    return w.Get().look.w * zone * dirOk * nearCam;
}

#include "ocean_surface.hlsli"
