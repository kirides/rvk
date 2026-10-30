// FSR input preparation (src/enh/upscale.cpp, docs/research/upscaling.md): the main view's hardware depth (D24S8 / D32S8, not
// accepted by the FidelityFX DLL as a 32-bit depth input) copied into an R32F image. Device depth as the engine's projection
// writes it (z / w, 0 near .. 1 far; the sky stays 1); a reversed buffer (reverse-z.md) is converted back to that.

#include "rvk_common.hlsli"

struct UpscalePush {
    RVK_PASS_PUSH_HEADER
    uint depthTex;  // the main view's hardware depth (bindless t2D)
    uint width;
    uint height;
    uint pad;
};
RVK_PUSH(UpscalePush, pc);

[[vk::image_format("r32f")]] RWTexture2D<float> outDepth : register(u0, space1);

[numthreads(8, 8, 1)]
void CSMain(uint3 id : SV_DispatchThreadID) {
    int2 p = int2(id.xy);
    if (p.x >= int(pc.width) || p.y >= int(pc.height)) return;
    outDepth[p] = rvkHwDepthToD3D(rvkLoadFrame(pc.frame), rvkFetch2D(pc.depthTex, p).r);
}
