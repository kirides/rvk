#version 460
// Native depth pre-pass, vertex stage (static_mesh.glsl): the position only. Same nativeClip as the colour pass (which tests depth EQUAL).
// The opaque class has no varyings but the view depth; the alpha-tested class (NATIVE_PREPASS_MASKED) adds the diffuse uv and the item.

#extension GL_EXT_buffer_reference_uvec2 : require
#include "static_mesh.glsl"

invariant gl_Position;

const float NATIVE_PREPASS_BIAS = 1e-5;

layout(location = 0) out float outViewZ;
#ifdef NATIVE_PREPASS_MASKED
layout(location = 1) out vec2 outUv;
layout(location = 2) flat out uint outItem;
#endif

void main() {
    const uint itemIndex = uint(gl_InstanceIndex);
    NativeItem it = nativeItem(itemIndex);
    const uvec2 v = addrAdd(it.addr.xy, uint(gl_VertexIndex) * it.tex2.z);
    vec3 world;
    vec4 viewPos;
    gl_Position = nativeClip(it, v, world, viewPos);
    // The pre-pass writes its depth a hair farther than the colour pass computes it (relative 1e-5, ~0.1 mm at 10 m): the colour pass tests
    // nearer-or-equal and must not depend on two shader programs rounding a position to the same bits (reversed depth, near / w, has the full
    // float precision: one ulp in w made whole triangles fail a depth EQUAL test)
    if ((pc.frame.frameInfo.y & RVK_FRAME_REVERSE_Z) != 0u) gl_Position.z *= 1.0 - NATIVE_PREPASS_BIAS;
    else gl_Position.z += NATIVE_PREPASS_BIAS * gl_Position.w;
    outViewZ = viewPos.w;
#ifdef NATIVE_PREPASS_MASKED
    outUv = it.off1.x != 0xFFFFu ? loadV2(v, it.off1.x) : vec2(0.0);
    outItem = itemIndex;
#endif
}
