// Native sun shadow cascades (src/enh/native_shadows.*, docs/research/native-renderer.md N4): depth-only draws of the scene mirror's static
// mesh parts from the sun, vertices pulled from the engine's buffers by device address (as static_mesh.*).
#define RVK_PASS_SHADER
#include "rvk_abi.glsl"

const uint SHADOW_INDEX32 = 1u;
const uint SHADOW_MASKED = 2u;

// per cascade (ring): the world -> clip matrix of the light (GLSL column order)
layout(buffer_reference, std430, buffer_reference_align = 16) readonly buffer ShadowCascade {
    mat4 viewProj;
};

// per part (ring): 3x4 object -> world rows and the alpha test's texture
layout(buffer_reference, std430, buffer_reference_align = 16) readonly buffer ShadowItem {
    vec4 row0;
    vec4 row1;
    vec4 row2;
    uvec4 tex;    // x albedo bindless slot, y sampler, z uv byte offset (0xFFFF = none), w flags
    uvec4 vtx;    // x stride
    vec4 params;  // x alpha reference 0..255
};

layout(push_constant, std430) uniform ShadowPush {
    uvec2 cascade;
    uvec2 item;
    uvec2 vertices;
    uvec2 indices;
} pc;
