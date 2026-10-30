// FSR exposure input (src/enh/upscale.cpp, docs/research/upscaling.md 18): the 1x1 R32F image FSR takes, from the engine's adapted
// luminance (the persistent image its ToneMap divides by, F-post-water-gui.md 2.4: the geometric mean scene luminance, smoothed over
// time). FSR's convention is exposure = 1 / (9.6 x Lavg) (its automatic exposure); `factor` = 1 / 9.6.

#include "rvk_common.hlsli"

struct ExposurePush {
    RVK_PASS_PUSH_HEADER
    uint lumTex;     // the engine's adapted luminance (bindless t2D, 1x1 R32F)
    float factor;    // 1 / 9.6 (engine/exposure.h kFsrExposureKey)
    float lumMin;    // sanity range of the luminance (the engine clamps it to AvgLumClamp itself)
    float lumMax;
};
RVK_PUSH(ExposurePush, pc);

[[vk::image_format("r32f")]] RWTexture2D<float> outExposure : register(u0, space1);

[numthreads(1, 1, 1)]
void CSMain(uint3 id : SV_DispatchThreadID) {
    float avg = rvkFetch2D(pc.lumTex, int2(0, 0)).r;
    // a fresh image holds zero (or NaN until the first adaptation step): the engine itself falls back to the clamp's lower bound
    if (!(avg > 0.0)) avg = pc.lumMin;
    avg = clamp(avg, pc.lumMin, pc.lumMax);
    outExposure[int2(0, 0)] = pc.factor / avg;
}
