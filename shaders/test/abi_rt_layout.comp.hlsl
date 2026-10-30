// Not used by the renderer: rvk_tests (abi_hlsl_rt_layout_matches_cpp) reads the member offsets of the RT geometry record
// (shaders/include/rvk_rt.hlsli) and of the RT debug view's push block (shaders/rt/rt_debug.hlsli) from this module's SPIR-V.

#include "../rt/rt_debug.hlsli"

[[vk::image_format("r32ui")]] RWTexture2D<uint> sink : register(u0, space1);

[numthreads(1, 1, 1)]
void CSMain(uint3 id : SV_DispatchThreadID) {
    RvkFrame f = rvkLoadFrame(pc.frame);
    RvkRtRecordData d = rvkPtr<RvkRtRecordData, 16>(f.rtScene.zw).Get();
    RvkRtSegmentData s = rvkPtr<RvkRtSegmentData, 16>(d.w6.xy).Get();
    uint v = pc.passId + pc.passFlags + pc.depthTex + pc.width + pc.height + pc.srcTex + pc.srcSampler + pc.mode + pc.probes.x +
             d.w0.x + d.w1.x + d.w2.x + d.w3.x + d.uvMap0.x + d.uvMap1.w + d.w6.x +
             s.w0.x + s.w1.x + s.uvMap0.x + s.uvMap1.w + f.rtScene.x + f.rtInfo.y;
    sink[int2(0, 0)] = v;
}
