// Enhanced shadows (src/enh/shadows.cpp, docs/research/shadows.md): a triangle covering the shadow mask target at depth 1,
// drawn in place of the engine's last post_SSM.fx cascade draw. Its states stay the engine's (z GREATER: every pixel in
// front of the far plane, i.e. not the sky; stencil EQUAL 0 INCR; alpha writes only), so the engine's following passes see
// the stencil and the mask as after its own cascades.

#include "shadow_block.hlsli"

float4 VSMain(uint vertexId : SV_VertexID) : SV_Position {
    int vi = int(vertexId);
    float2 uv = float2(float((vi << 1) & 2), float(vi & 2));
    float4 position = float4(uv * 2.0 - 1.0, 1.0, 1.0);
    // pc.draw points at the ShadowBlock here, not at a DrawBlock (a replaced draw's block): reading pc.draw.posFix took
    // cascade rows as the depth remap and the triangle was clipped / failed the depth test (docs/research/engine-frame/shadow-bug.md).
    // Reversed depth: the engine's far plane (z 1) maps to near / far (abi.h posFix.zw, filled by enh::Shadows from Device::depthRemap)
    ShadowPtr b = rvkShadowBlock();
    position.z += b.Get().depthRemap.x + b.Get().depthRemap.y;
    return position;
}
