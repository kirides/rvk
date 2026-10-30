// Not used by the renderer: rvk_tests (shadows_block_glsl_matches_cpp) reads the member offsets of ShadowBlock
// (shaders/shadows/shadow_block.hlsli) from this module's SPIR-V and compares them with enh::ShadowBlock (src/enh/shadows.h).

#include "../shadows/shadow_block.hlsli"

[[vk::image_format("r32ui")]] RWTexture2D<uint> sink : register(u0, space1);

[numthreads(1, 1, 1)]
void CSMain(uint3 id : SV_DispatchThreadID) {
    ShadowBlock b = rvkShadowBlock().Get();
    uint v = asuint(b.cascade[0].x + b.split.x + b.texel[0].x + b.vsm[0].x + b.vsmParams.x + b.params.x + b.target.x +
                    b.rtParams.x + b.depthRemap.x) +
             b.maps.x + b.res.x + b.mode.x + b.rt.x;
    sink[int2(0, 0)] = v;
}
