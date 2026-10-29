// Not used by the renderer: rvk_tests (abi_hlsl_layout_matches_cpp) reads the member offsets of RvkFrameData and of the debug
// view's push block from this module's SPIR-V and compares them with src/shader/abi.h / enh::DebugViewPush.

#include "debug/inject_view.hlsli"

[[vk::image_format("r32ui")]] RWTexture2D<uint> sink : register(u0, space1);

[numthreads(1, 1, 1)]
void CSMain(uint3 id : SV_DispatchThreadID) {
    RVK_REF(RvkFrameData) fp = pc.frame;
    RvkFrameData d = RVK_DEREF(fp);
    float acc = d.viewProj[1].x + d.invViewProj[1].x + d.view[1].x + d.proj[1].x + d.invView[1].x + d.invProj[1].x + d.prevViewProj[1].x +
                d.prevInvViewProj[1].x + d.cameraPos.x + d.prevCameraPos.x + d.depthParams.x + d.viewport.x + d.jitter.x + d.time.x +
                d.params[3].x;
    uint bits = asuint(acc) + d.frameInfo.x + d.images.x + d.rtScene.x + d.rtInfo.x + d.gi.x + d.ao.x + d.sgi.x + pc.passId + pc.passFlags +
                pc.depthTex + pc.width + pc.height + pc.srcTex + pc.srcSampler + pc.pad;
    sink[uint2(0, 0)] = bits;
}
