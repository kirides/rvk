// Native opaque pass (src/enh/native_opaque.*, docs/research/native-renderer.md N3): one static-mesh part per draw, vertices pulled
// from the engine's own buffers by device address (no vertex input state: one pipeline for every static layout). The per-draw data
// is an item block in the frame's ring.
#define RVK_PASS_SHADER
#include "rvk_abi.glsl"

const uint NATIVE_INDEX32 = 1u;     // 32-bit indices
const uint NATIVE_MASKED = 2u;      // alpha test (alphaFunc in off1.w bits 8..15, reference params.x, 0..255)
const uint NATIVE_NO_ALBEDO = 4u;   // no diffuse texture: grey
const uint NATIVE_LIGHTMAP = 8u;    // directional lightmaps (s4 / s5) at the lightmap uv
const uint NATIVE_AMB_COMPLEX = 16u;  // the 7-register SH ambient (else the 3-register one)
const uint NATIVE_NORMALMAP = 32u;
const uint NATIVE_DXT5NM = 64u;     // normal map: x in alpha, y in green
const uint NATIVE_NO_AMBIENT = 128u;
const uint NATIVE_NO_SUN = 256u;   // the permutation has no directional light
const uint NATIVE_NO_MASK = 512u;  // it does not sample the shadow mask

// mirror of enh::NativeItemBlock (912 bytes)
layout(buffer_reference, std430, buffer_reference_align = 16) readonly buffer NativeItem {
    vec4 ps[48];   // the engine's pixel shader constants c0..c47 of the draw (c0 camera, c1 view -> screen, c3 / c4 fog, c14 indoor
                   // ambient, c15..c21 SH ambient, c22..c24 sun (diffuse, specular, direction), c25.. point lights)
    vec4 row0;     // object -> world, 3x4 row-major
    vec4 row1;
    vec4 row2;
    uvec4 tex0;    // diffuse slot, diffuse sampler, normal slot, normal sampler
    uvec4 tex1;    // shadow mask slot, mask sampler, lightmap 1 slot, lightmap 2 slot
    uvec4 tex2;    // lightmap sampler, flags, stride, 0
    uvec4 off0;    // vertex byte offsets: normal, tangent, color0, color1 (0xFFFF = absent)
    uvec4 off1;    // uv (diffuse), uv (normal), lightmap uv, point light count | alpha func << 8
    vec4 params;   // x alpha reference 0..255
};

layout(push_constant, std430) uniform NativePush {
    RVK_PASS_PUSH_HEADER  // frame, passId, passFlags (16)
    uvec2 item;           // NativeItem
    uvec2 vertices;       // stream 0 start (buffer address + stream offset)
    uvec2 indices;        // index buffer start
} pc;
