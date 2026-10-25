#version 460
// Enhanced shadows (src/enh/shadows.cpp, docs/research/shadows.md): a triangle covering the shadow mask target at depth 1,
// drawn in place of the engine's last post_SSM.fx cascade draw. Its states stay the engine's (z GREATER: every pixel in
// front of the far plane, i.e. not the sky; stencil EQUAL 0 INCR; alpha writes only), so the engine's following passes see
// the stencil and the mask as after its own cascades.

#include "rvk_abi.glsl"

void main() {
    vec2 uv = vec2((gl_VertexIndex << 1) & 2, gl_VertexIndex & 2);
    gl_Position = vec4(uv * 2.0 - 1.0, 1.0, 1.0);
    // reversed depth: the engine's far plane (z 1) maps to near / far (abi.h posFix.zw)
    gl_Position.z += pc.draw.posFix.z + pc.draw.posFix.w;
}
