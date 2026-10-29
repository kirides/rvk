// Shared by the GI probe passes (gi_trace.comp, gi_blend.comp; src/enh/gi.cpp, docs/research/rtgi.md 5): the pass
// parameters (mirror: enh::GiPassParams, static_assert'ed size), the probe list, the ray buffer and the ray directions.

#ifndef GI_COMMON_GLSL
#define GI_COMMON_GLSL

const uint GI_RAYS = 64u;        // rays per probe and update (= the workgroup size of both passes)
const uint GI_MAX_PROBE_ITEMS = 4096u;  // probes per update (the most gi.quality allows): the light visibility words follow the rays
const uint GI_MAX_LIGHTS = 128u;  // point lights of the frame: the world's most relevant (engine/lights), else the draws' g_PointLights

// mirror: enh::GiPassParams
layout(buffer_reference, std430, buffer_reference_align = 16) readonly buffer GiPassParams {
    uvec4 lists;        // xy probe list address (GiList), zw ray buffer address (GiRays)
    uvec4 volume;       // xy this frame's volume header (RvkGiVolume), z probes in the list, w frame
    uvec4 flags;        // x RVK_RT_TRACE_* of the probe rays, y lights, z bit 0: the probes take the lights' direct light (gi.lights world + replaced lightmaps), w the emissive gain (float bits)
    vec4 rot[3];        // the frame's random rotation of the ray set (rows)
    vec4 reach;         // ray length per cascade (world units)
    vec4 blend;         // x irradiance hysteresis, y depth hysteresis, z depth sharpness, w backface share that marks a probe inside
    vec4 shade;         // x bounce factor (the probes' own light at hits), y albedo scale, z sun shadow ray reach, w ray cone spread
    vec4 sunColor;      // c22 (rgb; 0 = no sun)
    vec4 sunDir;        // c24 (towards the sun)
    vec4 sky[7];        // the engine's outdoor ambient c15..c21 (rays that miss)
    vec4 lights[GI_MAX_LIGHTS * 2];  // position + 1 / R^2, colour
    // the screen probes (sp_trace.comp, sp_resolve.comp; rtgi.md 11)
    uvec4 screen;     // x the main view's hardware depth (t2D), y the probes' geometry image (t2D, rgba32f: position, packed octahedral normal), z probe grid width, w height
    uvec4 screen2;    // x tile size (pixels), y rays per probe, z scene width, w scene height
    uvec4 screen3;    // x the probes image (t2D, the resolve's input), y the previous frame's resolved image (t2D, 0 = none),
                      // z the previous frame's second resolved image (directional correction + history count), w linear clamp sampler
    vec4 screen4;     // x ray reach, y ratio cap, z history blend, w colour bleeding (0..1)
    uvec4 screen5;    // x the probes' second image (t2D: directional correction), y,z,w 0
    vec4 screen6;     // x directional correction strength (gi.directional, 0..1), y,z,w 0
};
// (cascade << 24) | slot of each probe to update
layout(buffer_reference, std430, buffer_reference_align = 4) readonly buffer GiList {
    uint e[];
};
// per probe x ray: radiance (rgb, acc units) + hit distance (w; negative = a back face, -0.2 t); after GI_MAX_PROBE_ITEMS x
// GI_RAYS entries one uvec4 (as float bits) per probe: the lights it sees (bit j = light j is visible from the probe)
layout(buffer_reference, std430, buffer_reference_align = 16) buffer GiRays {
    vec4 r[];
};

layout(push_constant, std430) uniform GiPush {
    RVK_PASS_PUSH_HEADER
    uvec2 params;  // GiPassParams address
    uvec2 pad;
} pc;

uint giHash(uint x) {
    x ^= x >> 16;
    x *= 0x7feb352du;
    x ^= x >> 15;
    x *= 0x846ca68bu;
    x ^= x >> 16;
    return x;
}

// spherical Fibonacci directions (uniform over the sphere), rotated by the frame's rotation
vec3 giRayDir(GiPassParams p, uint i) {
    const float golden = 2.39996323;  // pi * (3 - sqrt(5))
    float z = 1.0 - (2.0 * float(i) + 1.0) / float(GI_RAYS);
    float r = sqrt(max(0.0, 1.0 - z * z));
    float phi = float(i) * golden;
    vec3 d = vec3(cos(phi) * r, z, sin(phi) * r);
    return normalize(vec3(dot(p.rot[0].xyz, d), dot(p.rot[1].xyz, d), dot(p.rot[2].xyz, d)));
}

#endif
