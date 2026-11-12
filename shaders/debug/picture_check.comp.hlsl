// The owned picture check, compute pass: mode 0 copies the opaque scene colour + the linear depth map into two reference images (a composed frame), mode 1 compares
// them with the current frame's (an owned frame, or the next composed frame as the noise control) and counts / bounds the differing pixels in a stats buffer.

#include "picture_check.hlsli"

[[vk::image_format("rgba16f")]] RWTexture2D<float4> refColor : register(u0, space1);
[[vk::image_format("r32f")]] RWTexture2D<float> refDepth : register(u1, space1);
// mode 2 (RVK_PICTURE_DUMP diagnostics): the compare of mode 1 + the current frame's colour / depth into these (read back and written to bin/rvk/dumps)
[[vk::image_format("rgba16f")]] RWTexture2D<float4> curColor : register(u2, space1);
[[vk::image_format("r32f")]] RWTexture2D<float> curDepth : register(u3, space1);

[numthreads(8, 8, 1)]
void CSMain(uint3 id : SV_DispatchThreadID) {
    uint2 p = id.xy;
    if (p.x >= pc.width || p.y >= pc.height) return;
    float3 c = rvkFetch2D(pc.colorTex, int2(p)).rgb;
    float d = rvkFetch2D(pc.depthTex, int2(p)).r;
    if (pc.mode == 0u) {
        refColor[p] = float4(c, 1.0);
        refDepth[p] = d;
        return;
    }
    if (pc.mode == 2u) {
        curColor[p] = float4(c, 1.0);
        curDepth[p] = d;
    }
    float3 rc = refColor[p].rgb;
    float rd = refDepth[p];
    float3 dc3 = abs(c - rc);
    float scale = 0.05 + max(max(max(c.r, c.g), c.b), max(max(rc.r, rc.g), rc.b));
    float dc = max(max(dc3.r, dc3.g), dc3.b) / scale;
    float dd = abs(d - rd) / max(min(d, rd), 1e-4);
    bool nan = any(isnan(c)) || any(isnan(rc)) || isnan(d) || isnan(rd);
    bool colorDiff = nan || dc > pc.colorTol;
    bool depthDiff = !nan && dd > pc.depthTol;
    if (nan) { dc = 1.0e6; }
    if (!colorDiff && !depthDiff) return;
    if (colorDiff) {
        rvkBufAdd(pc.stats, PC_COLOR_DIFF * 4u, 1u);
        rvkRwAtomicMax(rvkAddrAdd(rvkAddr(pc.stats), PC_MAX_COLOR * 4u), asuint(dc));
    }
    if (depthDiff) {
        rvkBufAdd(pc.stats, PC_DEPTH_DIFF * 4u, 1u);
        rvkRwAtomicMax(rvkAddrAdd(rvkAddr(pc.stats), PC_MAX_DEPTH * 4u), asuint(dd));
    }
    rvkBufAdd(pc.stats, PC_ANY_DIFF * 4u, 1u);
    rvkRwAtomicMax(rvkAddrAdd(rvkAddr(pc.stats), PC_MAX_X * 4u), p.x);
    rvkRwAtomicMax(rvkAddrAdd(rvkAddr(pc.stats), PC_MAX_Y * 4u), p.y);
    rvkRwAtomicMax(rvkAddrAdd(rvkAddr(pc.stats), PC_NMIN_X * 4u), 0xFFFFu - p.x);
    rvkRwAtomicMax(rvkAddrAdd(rvkAddr(pc.stats), PC_NMIN_Y * 4u), 0xFFFFu - p.y);
}
