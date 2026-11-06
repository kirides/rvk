// Not used by the renderer: rvk_tests (abi_hlsl_scene_layout_matches_cpp) reads the member offsets of the scene variant's blocks (ScenePush, DrawRecord)
// from this module's SPIR-V and compares them with src/shader/abi.h.

#define RVK_SCENE_RECORD 1
#include "rvk_draw.hlsli"

[[vk::image_format("r32ui")]] RWTexture2D<uint> sink : register(u0, space1);

[numthreads(1, 1, 1)]
void CSMain(uint3 id : SV_DispatchThreadID) {
    rvkSceneLoad(pc.recordBase + id.x);
    const RvkDrawRecord r = rvkPtr<RvkDrawRecord, 16>(pc.records).Get();
    uint v = r.vsw01.x + r.vsw23.y + r.psw01.z + r.psw23.w + r.drawPrev.x + r.ids.y + r.slots.z + r.reserved.w + rvkVsWin(2u).x + rvkPsWin(3u).y + rvkDrawAddr().x +
             rvkDrawFlags() + rvkDrawId() + rvkDrawPrevVs().y + rvkLoadDraw().tex[0].x + asuint(rvkLoadFrame(pc.frame).params[0].x);
    sink[int2(0, 0)] = v;
}
