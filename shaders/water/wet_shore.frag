#version 460
// The sea's wet beach (src/enh/water.cpp, docs/research/water.md 4.6): with the wave grid the waterline sits lower than the
// engine's and swashes up and down the beach, so sand the engine always kept under its water (and its painted surf) is
// seen. That sand is wet: before the refraction copy (AfterOpaque) the scene is darkened where the ground lies in the
// coast band of the shore field and not higher than a little above the swash's reach (blend: dst * src). The water drawn
// after it shows the same darkened sand through its shallow edge: no seam at the waterline.

#define RVK_PASS_SHADER
#extension GL_EXT_buffer_reference_uvec2 : require
#include "rvk_abi.glsl"
#include "rvk_common.glsl"

layout(push_constant) uniform WetPush {
    RVK_PASS_PUSH_HEADER
    uint depthTex;
    uint fieldTex;
    uint smpLinear;
    uint smpPoint;
    vec4 shoreMap;  // xy shore field corner (world xz), zw 1 / its extent
    vec4 ocean;     // x sea level, y lowering at the coast, z swash amplitude, w coast band
    vec4 params;    // x time (s), y wet height above the swash's reach, z darkening (0..1), w unused
} pc;

layout(location = 0) in vec2 inUv;
layout(location = 0) out vec4 outColor;

void main() {
    RvkFrame f = pc.frame;
    float d = rvkSample2DLod(pc.depthTex, pc.smpPoint, inUv, 0.0).r;
    outColor = vec4(1.0);
    if (d >= 0.995) return;  // sky
    vec3 G = rvkWorldFromViewDepth(f, inUv, d * f.cameraPos.w);
    vec2 fuv = (G.xz - pc.shoreMap.xy) * pc.shoreMap.zw;
    if (any(lessThan(fuv, vec2(0.0))) || any(greaterThan(fuv, vec2(1.0)))) return;
    float coastDist = rvkSample2DLod(pc.fieldTex, pc.smpLinear, fuv, 0.0).x;
    // the beach only: in and next to the engine's coast overlay (quay walls further away stay as they are)
    float near = 1.0 - smoothstep(150.0, 500.0, coastDist);
    if (near <= 0.0) return;
    // the swash's highest reach (water_common.glsl coastOffset at its crest) + a damp margin above it
    float reach = pc.ocean.x - pc.ocean.y + pc.ocean.z;
    float h = G.y - reach;
    float wet = (1.0 - smoothstep(0.0, pc.params.y, h)) * smoothstep(-80.0, -30.0, h) * near;
    // freshly wet just above the water: darker; drying further up
    float k = pc.params.z * wet * mix(0.65, 1.0, 1.0 - smoothstep(0.0, pc.params.y * 0.6, h));
    outColor = vec4(vec3(1.0 - k), 1.0);
}
