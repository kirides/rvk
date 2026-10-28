#version 460
// Enhanced shadows (src/enh/shadows.cpp, docs/research/shadows.md): a triangle covering the shadow mask target at depth 1,
// drawn in place of the engine's last post_SSM.fx cascade draw. Its states stay the engine's (z GREATER: every pixel in
// front of the far plane, i.e. not the sky; stencil EQUAL 0 INCR; alpha writes only), so the engine's following passes see
// the stencil and the mask as after its own cascades.

#extension GL_EXT_buffer_reference_uvec2 : require

#include "rvk_abi.glsl"
#include "shadow_block.glsl"

void main() {
    vec2 uv = vec2((gl_VertexIndex << 1) & 2, gl_VertexIndex & 2);
    gl_Position = vec4(uv * 2.0 - 1.0, 1.0, 1.0);
    // pc.draw points at the ShadowBlock here, not at a DrawBlock (a replaced draw's block): reading pc.draw.posFix took
    // cascade rows as the depth remap and the triangle was clipped / failed the depth test (docs/research/engine-frame/shadow-bug.md).
    // Reversed depth: the engine's far plane (z 1) maps to near / far (abi.h posFix.zw, filled by enh::Shadows from Device::depthRemap)
    ShadowBlock b = ShadowBlock(uvec2(pc.draw));
    gl_Position.z += b.depthRemap.x + b.depthRemap.y;
}
