// Shared by the GI probe passes (gi_trace.comp, gi_blend.comp; src/enh/gi.cpp, docs/research/rtgi.md 5): the pass
// parameters (mirror: enh::GiPassParams, static_assert'ed size), the probe list, the ray buffer and the ray directions.
// HLSL port of gi_common.glsl.
#pragma once

#include "rvk_common.hlsli"

static const uint GI_RAYS = 64u;               // rays per probe and update (= the workgroup size of both passes)
static const uint GI_MAX_PROBE_ITEMS = 4096u;  // probes per update (the most gi.quality allows): the light visibility words follow the rays
static const uint GI_MAX_LIGHTS = 128u;        // point lights of the frame: the world's most relevant (engine/lights), else the draws' g_PointLights

// mirror: enh::GiPassParams
struct GiPassParamsData {
    uint4 lists;        // xy probe list address (GiList), zw ray buffer address (GiRays)
    uint4 volume;       // xy this frame's volume header (RvkGiVolume), z probes in the list, w frame
    uint4 flags;        // x RVK_RT_TRACE_* of the probe rays, y lights, z bit 0: the probes take the lights' direct light (gi.lights world + replaced lightmaps), w the emissive gain (float bits)
    float4 rot[3];      // the frame's random rotation of the ray set (rows)
    float4 reach;       // ray length per cascade (world units)
    float4 blend;       // x irradiance hysteresis, y depth hysteresis, z depth sharpness, w backface share that marks a probe inside
    float4 shade;       // x bounce factor (the probes' own light at hits), y albedo scale, z sun shadow ray reach, w ray cone spread
    float4 sunColor;    // c22 (rgb; 0 = no sun)
    float4 sunDir;      // c24 (towards the sun)
    float4 sky[7];      // the engine's outdoor ambient c15..c21 (rays that miss)
    float4 lights[GI_MAX_LIGHTS * 2];  // position + 1 / R^2, colour
    // the screen probes (sp_trace.comp, sp_resolve.comp; rtgi.md 11)
    uint4 screen;     // x the main view's hardware depth (t2D), y the probes' geometry image (t2D, rgba32f: position, packed octahedral normal), z probe grid width, w height
    uint4 screen2;    // x tile size (pixels), y rays per probe, z scene width, w scene height
    uint4 screen3;    // x the probes image (t2D, the resolve's input), y the previous frame's resolved image (t2D, 0 = none),
                      // z the previous frame's second resolved image (directional correction + history count), w linear clamp sampler
    float4 screen4;   // x ray reach, y ratio cap, z history blend, w colour bleeding (0..1)
    uint4 screen5;    // x the probes' second image (t2D: directional correction), y,z,w 0
    float4 screen6;   // x directional correction strength (gi.directional, 0..1), y,z,w 0
};
// a reference to the parameters in memory: members are read through `.Get()`
typedef vk::BufferPointer<GiPassParamsData, 16> GiPassParams;

// (cascade << 24) | slot of each probe to update: uint e[] at pc.params' lists.xy
uint giListEntry(GiPassParams p, uint item) { return vk::RawBufferLoad<uint>(rvkAddr(p.Get().lists.xy) + uint64_t(item) * 4ull, 4); }
// per probe x ray: radiance (rgb, acc units) + hit distance (w; negative = a back face, -0.2 t); after GI_MAX_PROBE_ITEMS x
// GI_RAYS entries one uint4 (as float bits) per probe: the lights it sees (bit j = light j is visible from the probe).
// float4 r[] at lists.zw, element i:
float4 giRayLoad(GiPassParams p, uint i) { return vk::RawBufferLoad<float4>(rvkAddr(p.Get().lists.zw) + uint64_t(i) * 16ull, 16); }
void giRayStore(GiPassParams p, uint i, float4 v) { vk::RawBufferStore<float4>(rvkAddr(p.Get().lists.zw) + uint64_t(i) * 16ull, v, 16); }

struct GiPush {
    RVK_PASS_PUSH_HEADER
    uint2 params;  // GiPassParams address
    uint2 pad;
};
RVK_PUSH(GiPush, pc);

GiPassParams giParams() { return rvkPtr<GiPassParamsData, 16>(pc.params); }

uint giHash(uint x) {
    x ^= x >> 16;
    x *= 0x7feb352du;
    x ^= x >> 15;
    x *= 0x846ca68bu;
    x ^= x >> 16;
    return x;
}

// spherical Fibonacci directions (uniform over the sphere), rotated by the frame's rotation
float3 giRayDir(GiPassParams p, uint i) {
    const float golden = 2.39996323;  // pi * (3 - sqrt(5))
    float z = 1.0 - (2.0 * float(i) + 1.0) / float(GI_RAYS);
    float r = sqrt(max(0.0, 1.0 - z * z));
    float phi = float(i) * golden;
    float3 d = float3(cos(phi) * r, z, sin(phi) * r);
    return normalize(float3(dot(p.Get().rot[0].xyz, d), dot(p.Get().rot[1].xyz, d), dot(p.Get().rot[2].xyz, d)));
}
