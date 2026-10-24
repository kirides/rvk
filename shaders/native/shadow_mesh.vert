#version 460
// Native shadow cascade, vertex stage (shadow_mesh.glsl)

#extension GL_EXT_buffer_reference_uvec2 : require
#include "shadow_mesh.glsl"

layout(buffer_reference, std430, buffer_reference_align = 4) readonly buffer ShadowWord {
    uint v;
};
uvec2 addrAdd(uvec2 a, uint bytes) {
    uint carry;
    uint lo = uaddCarry(a.x, bytes, carry);
    return uvec2(lo, a.y + carry);
}
uint loadWord(uvec2 a) { return ShadowWord(a).v; }
uint loadHalf(uvec2 a) {
    const uint w = loadWord(uvec2(a.x & ~3u, a.y));
    return (a.x & 2u) != 0u ? w >> 16 : w & 0xFFFFu;
}
float loadF(uvec2 base, uint byteOffset) { return uintBitsToFloat(loadWord(addrAdd(base, byteOffset))); }

layout(location = 0) out vec2 outUv;

void main() {
    ShadowItem it = ShadowItem(pc.item);
    ShadowCascade cs = ShadowCascade(pc.cascade);
    uint index;
    if ((it.tex.w & SHADOW_INDEX32) != 0u) {
        index = loadWord(addrAdd(pc.indices, uint(gl_VertexIndex) * 4u));
    } else {
        index = loadHalf(addrAdd(pc.indices, uint(gl_VertexIndex) * 2u));
    }
    const uvec2 v = addrAdd(pc.vertices, index * it.vtx.x);
    const vec4 p = vec4(loadF(v, 0u), loadF(v, 4u), loadF(v, 8u), 1.0);
    const vec3 world = vec3(dot(it.row0, p), dot(it.row1, p), dot(it.row2, p));
    gl_Position = cs.viewProj * vec4(world, 1.0);
    outUv = it.tex.z != 0xFFFFu ? vec2(loadF(v, it.tex.z), loadF(v, it.tex.z + 4u)) : vec2(0.0);
}
