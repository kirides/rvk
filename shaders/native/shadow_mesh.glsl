// Native sun shadow cascades (src/enh/native_shadows.*, docs/research/native-renderer.md N4): depth-only draws of the scene mirror's static
// mesh parts from the sun, vertices pulled from the engine's buffers by device address (as static_mesh.*). Parts with the same geometry and
// material are one instanced draw: the per-instance transforms are read from a ring array by gl_InstanceIndex.
#define RVK_PASS_SHADER
#include "rvk_abi.glsl"

const uint SHADOW_INDEX32 = 1u;
const uint SHADOW_MASKED = 2u;

// per cascade (ring): the world -> clip matrix of the light (GLSL column order)
layout(buffer_reference, std430, buffer_reference_align = 16) readonly buffer ShadowCascade {
    mat4 viewProj;
};

// per group (ring): the alpha test's texture and the vertex layout
layout(buffer_reference, std430, buffer_reference_align = 16) readonly buffer ShadowItem {
    uvec4 tex;    // x albedo bindless slot, y sampler, z uv byte offset (0xFFFF = none), w flags
    uvec4 vtx;    // x stride
    vec4 params;  // x alpha reference 0..255
};

// per cascade (ring): the visible instances of all groups, 3 vec4 each (object -> world 3x4 rows)
layout(buffer_reference, std430, buffer_reference_align = 16) readonly buffer ShadowInstances {
    vec4 rows[];
};

layout(push_constant, std430) uniform ShadowPush {
    uvec2 cascade;
    uvec2 item;
    uvec2 instances;
    uvec2 vertices;
    uvec2 indices;
} pc;
