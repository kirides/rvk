#version 460
// Native opaque pass, vertex stage (static_mesh.glsl): index -> vertex pulled by address -> the engine's clip space (the same matrix,
// half-pixel fix and jitter as the engine's own vertex shaders, so the depth lines up with the engine's remaining draws).

#extension GL_EXT_buffer_reference_uvec2 : require
#include "static_mesh.glsl"

// 64-bit address arithmetic on uvec2 (no shaderInt64), word loads by address (rvk_rt.glsl's, without its texture helpers: the
// vertex stage cannot hold implicit-lod sampling)
layout(buffer_reference, std430, buffer_reference_align = 4) readonly buffer NativeWord {
    uint v;
};
uvec2 addrAdd(uvec2 a, uint bytes) {
    uint carry;
    uint lo = uaddCarry(a.x, bytes, carry);
    return uvec2(lo, a.y + carry);
}
uint loadWord(uvec2 a) { return NativeWord(a).v; }
uint loadHalf(uvec2 a) {
    const uint w = loadWord(uvec2(a.x & ~3u, a.y));
    return (a.x & 2u) != 0u ? w >> 16 : w & 0xFFFFu;
}
float loadF(uvec2 base, uint byteOffset) { return uintBitsToFloat(loadWord(addrAdd(base, byteOffset))); }
vec3 loadV3(uvec2 base, uint byteOffset) { return vec3(loadF(base, byteOffset), loadF(base, byteOffset + 4u), loadF(base, byteOffset + 8u)); }
vec2 loadV2(uvec2 base, uint byteOffset) { return vec2(loadF(base, byteOffset), loadF(base, byteOffset + 4u)); }
vec4 loadColor(uvec2 base, uint byteOffset) {
    const uint c = loadWord(addrAdd(base, byteOffset));  // D3DCOLOR: 0xAARRGGBB
    return vec4(float((c >> 16) & 255u), float((c >> 8) & 255u), float(c & 255u), float(c >> 24)) * (1.0 / 255.0);
}

layout(location = 0) out vec3 outWorld;
layout(location = 1) out vec3 outNormal;
layout(location = 2) out vec3 outTangent;
layout(location = 3) out vec4 outColor0;
layout(location = 4) out vec4 outColor1;
layout(location = 5) out vec4 outUv;      // xy diffuse, zw normal map
layout(location = 6) out vec2 outLightmap;
layout(location = 7) out vec4 outViewPos;  // clip position before the half-pixel fix (the engine's vViewPos)

void main() {
    NativeItem it = NativeItem(pc.item);
    const uint flags = it.tex2.y;
    uint index;
    if ((flags & NATIVE_INDEX32) != 0u) {
        index = loadWord(addrAdd(pc.indices, uint(gl_VertexIndex) * 4u));
    } else {
        index = loadHalf(addrAdd(pc.indices, uint(gl_VertexIndex) * 2u));
    }
    const uvec2 v = addrAdd(pc.vertices, index * it.tex2.z);
    const vec4 p = vec4(loadV3(v, 0u), 1.0);
    const vec3 world = vec3(dot(it.row0, p), dot(it.row1, p), dot(it.row2, p));
    RvkFrame f = pc.frame;
    vec4 clip = f.viewProj * vec4(world, 1.0);
    outViewPos = clip;
    // the engine's half-pixel fix + camera jitter (abi.h posFix: 1 / w, -1 / h plus the jitter in NDC)
    clip.xy += (vec2(f.viewport.z, -f.viewport.w) + vec2(2.0 * f.jitter.x, -2.0 * f.jitter.y) * f.viewport.zw) * clip.w;
    gl_Position = clip;
    outWorld = world;

    // normal / tangent into world space (the rows' 3x3, as the engine's vertex shader)
    vec3 n = it.off0.x != 0xFFFFu ? loadV3(v, it.off0.x) : vec3(0.0, 1.0, 0.0);
    vec3 t = it.off0.y != 0xFFFFu ? loadV3(v, it.off0.y) : vec3(1.0, 0.0, 0.0);
    outNormal = normalize(vec3(dot(it.row0.xyz, n), dot(it.row1.xyz, n), dot(it.row2.xyz, n)));
    outTangent = normalize(vec3(dot(it.row0.xyz, t), dot(it.row1.xyz, t), dot(it.row2.xyz, t)));
    outColor0 = it.off0.z != 0xFFFFu ? loadColor(v, it.off0.z) : vec4(1.0);
    outColor1 = it.off0.w != 0xFFFFu ? loadColor(v, it.off0.w) : vec4(1.0);
    outUv = vec4(it.off1.x != 0xFFFFu ? loadV2(v, it.off1.x) : vec2(0.0), it.off1.y != 0xFFFFu ? loadV2(v, it.off1.y) : vec2(0.0));
    outLightmap = it.off1.z != 0xFFFFu ? loadV2(v, it.off1.z) : vec2(0.0);
}
