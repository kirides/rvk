// Native opaque pass (src/enh/native_opaque.*, docs/research/native-renderer.md N3, 14): static-mesh parts drawn by one indirect call per
// class (opaque, alpha tested): the indices come from the native index arena (one bound u32 index buffer), the vertices are pulled from
// the engine's own buffers by device address (no vertex input state: one pipeline for every static layout), the per-part data is an item
// block in the frame's ring, indexed by gl_InstanceIndex (the indirect command's firstInstance).
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

// mirror of enh::NativeItemBlock (928 bytes)
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
    uvec4 addr;    // xy the part's stream 0 start (buffer address + stream offset)
};

// mirror of enh::NativeShadowBlock (272 bytes): the native sun cascades (enh/native_shadows)
layout(buffer_reference, std430, buffer_reference_align = 16) readonly buffer NativeShadow {
    mat4 viewProj[3];  // world -> cascade clip
    vec4 split;        // far end of cascades 0..2 (view depth, world units)
    uvec4 maps;        // bindless t2D slots; w = the point / clamp sampler
    vec4 params;       // x 1 / map size, y depth bias (texels), z normal offset (texels), w last cascade fade fraction
    vec4 texel;        // world units per texel of each cascade
    vec4 range;        // depth range of each cascade (world units)
};

layout(push_constant, std430) uniform NativePush {
    RVK_PASS_PUSH_HEADER  // frame, passId (the debug view), passFlags (16)
    uvec2 items;          // the NativeItem array (928 bytes each)
    uvec2 shadow;         // NativeShadow (0: no native cascades this frame: the engine's mask)
} pc;

const uint NATIVE_ITEM_SIZE = 928u;

// 64-bit address arithmetic on uvec2 (no shaderInt64)
uvec2 addrAdd(uvec2 a, uint bytes) {
    uint carry;
    uint lo = uaddCarry(a.x, bytes, carry);
    return uvec2(lo, a.y + carry);
}
NativeItem nativeItem(uint index) { return NativeItem(addrAdd(pc.items, index * NATIVE_ITEM_SIZE)); }

// word loads by address (the vertex stage cannot hold implicit-lod sampling, so these are not rvk_rt.glsl's)
layout(buffer_reference, std430, buffer_reference_align = 4) readonly buffer NativeWord {
    uint v;
};
uint loadWord(uvec2 a) { return NativeWord(a).v; }
float loadF(uvec2 base, uint byteOffset) { return uintBitsToFloat(loadWord(addrAdd(base, byteOffset))); }
vec3 loadV3(uvec2 base, uint byteOffset) { return vec3(loadF(base, byteOffset), loadF(base, byteOffset + 4u), loadF(base, byteOffset + 8u)); }
vec2 loadV2(uvec2 base, uint byteOffset) { return vec2(loadF(base, byteOffset), loadF(base, byteOffset + 4u)); }
vec4 loadColor(uvec2 base, uint byteOffset) {
    const uint c = loadWord(addrAdd(base, byteOffset));  // D3DCOLOR: 0xAARRGGBB
    return vec4(float((c >> 16) & 255u), float((c >> 8) & 255u), float(c & 255u), float(c >> 24)) * (1.0 / 255.0);
}

// The part's vertex position -> clip space, in the engine's convention (the same matrix, half-pixel fix, jitter and reversed depth as
// its own vertex shaders, so the depth lines up with the engine's remaining draws). Shared by the depth pre-pass and the colour pass:
// the colour pass tests depth EQUAL, so both must compute exactly this (gl_Position is `invariant` in both). `viewPos` = the clip
// position before the half-pixel fix (the engine's vViewPos).
vec4 nativeClip(NativeItem it, uvec2 vertex, out vec3 world, out vec4 viewPos) {
    // `precise`: no fused multiply-adds / reassociation, so this is the same sequence of IEEE operations in every shader program. Reversed
    // depth is near / w with the full float precision, so one ulp of difference in w between the pre-pass and the colour pass fails the
    // depth EQUAL test for a whole triangle (the D3D depth near 1 hid such differences)
    precise vec4 p = vec4(loadV3(vertex, 0u), 1.0);
    precise vec3 wpos = vec3(dot(it.row0, p), dot(it.row1, p), dot(it.row2, p));
    world = wpos;
    RvkFrame f = pc.frame;
    precise vec4 clip = f.viewProj * vec4(wpos, 1.0);
    viewPos = clip;
    clip.xy += (vec2(f.viewport.z, -f.viewport.w) + vec2(2.0 * f.jitter.x, -2.0 * f.jitter.y) * f.viewport.zw) * clip.w;
    // reversed depth (abi.h posFix.zw, rvk_common.glsl rvkHwDepthToD3D): the engine's remap z' = w - k z is mathematically the constant
    // `near` for this perspective projection (z' / w = near / view z); computing it as w - k z cancels to a value that differs by rounding
    // between two shader programs, and the colour pass tests depth EQUAL against the pre-pass, so use the constant itself
    if ((f.frameInfo.y & RVK_FRAME_REVERSE_Z) != 0u) clip.z = f.depthParams.x;
    return clip;
}
