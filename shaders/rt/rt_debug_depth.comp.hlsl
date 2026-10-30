// RT scene debug view, depth capture (rt.debug depth mode, src/enh/rt_debug.cpp): at AfterOpaque the main view's hardware
// depth buffer holds every opaque draw of the solid pass (the engine's linear depth map only the z-pass subset); this pass
// converts it to view depth (the FrameBlock projection: D3D z = A + B / viewZ) into an R32F image that the BeforeGui pass
// compares the traced distances with. 0 = nothing drawn (cleared to the far plane).

#include "rt_debug.hlsli"

[[vk::image_format("r32f")]] RWTexture2D<float> outDepth : register(u0, space1);

[numthreads(8, 8, 1)]
void CSMain(uint3 id : SV_DispatchThreadID) {
    uint2 p = id.xy;
    if (p.x >= pc.width || p.y >= pc.height) return;
    RvkFrame f = rvkLoadFrame(pc.frame);
    float viewZ = rvkHwDepthToViewZ(f, rvkFetch2D(pc.depthTex, int2(p)).r);
    outDepth[p] = max(viewZ, 0.0);
}
