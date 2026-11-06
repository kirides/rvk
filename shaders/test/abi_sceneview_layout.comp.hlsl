// Not used by the renderer: rvk_tests (abi_hlsl_sceneview_layout_matches_cpp) reads the member offsets of the scene ABI v2's blocks (ScenePush with `views`,
// DrawRecord, ViewBlock2) from this module's SPIR-V and compares them with src/shader/abi.h.

#define RVK_SCENE_VIEW 1
#include "rvk_draw.hlsli"

[[vk::image_format("r32ui")]] RWTexture2D<uint> sink : register(u0, space1);

[numthreads(1, 1, 1)]
void CSMain(uint3 id : SV_DispatchThreadID) {
    rvk_viewSlot = id.y;
    rvkSceneLoad(pc.recordBase + id.x);
    const RvkViewBlock2 vb = rvkPtr<RvkViewBlock2, 16>(pc.views).Get();
    const float4 sum = vb.vs0[1] + vb.ps0[2] + vb.vs1[3] + vb.posFix + vb.c5;
    uint v = asuint(sum.x) + vb.flags.x + vb.reserved.y + rvkVsWin(1u).x + rvkPsWin(0u).y + rvkPsWin(1u).x + asuint(rvkSceneC5().w) + asuint(RVK_PSC(5).y) +
             rvkLoadDraw().tex[0].x + rvk_rec.slots.z + asuint(rvkLoadFrame(pc.frame).params[0].x);
    sink[int2(0, 0)] = v;
}
