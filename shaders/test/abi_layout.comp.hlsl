// Not used by the renderer: rvk_tests (abi_hlsl_draw_layout_matches_cpp) reads the member offsets of the draw ABI blocks from this module's
// SPIR-V and compares them with src/shader/abi.h.

#include "rvk_draw.hlsli"

[[vk::image_format("r32ui")]] RWTexture2D<uint> sink : register(u0, space1);

[numthreads(1, 1, 1)]
void CSMain(uint3 id : SV_DispatchThreadID) {
    RvkStageData vs = rvkPtr<RvkStageData, 16>(pc.vs).Get();
    RvkStageData ps = rvkPtr<RvkStageData, 16>(pc.ps).Get();
    RvkStageData prev = rvkPtr<RvkStageData, 16>(pc.prevVs).Get();
    RvkDrawData d = rvkLoadDraw();
    uint v = asuint(vs.c[0].x) + uint(ps.i[0].x) + prev.b[0].x + d.tex[0].x + d.alpha.w + pc.drawId + pc.flags + asuint(RVK_VSC(40).x) +
             asuint(RVK_PSC(20).x) + asuint(d.psC5.w) + asuint(d.posFix.x) + d.smp[1].y + d.alpha.x + asuint(rvkLoadFrame(pc.frame).params[0].x);
    sink[int2(0, 0)] = v;
}
