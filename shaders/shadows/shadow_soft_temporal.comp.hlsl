// Soft sun shadows, stage 2: temporal accumulation (src/enh/shadows.cpp, docs/research/shadows.md 8.10). The raw cascade term (a few
// rotated taps per frame: noisy in the penumbra only) is blended into the previous frame's result reprojected with the camera's
// motion (depth-checked bilinear taps, rt_shadow_common.hlsli rtsHistory), the history clamped to the range of the raw 3x3
// neighbourhood (a shadow that moved leaves no ghost where the neighbourhood is uniform). The history length follows the penumbra:
// a hard edge (filter radius under a texel: the engine's own crispness) takes no history at all, so a moving camera cannot blur it;
// a wide penumbra accumulates up to SOFT_MAX_FRAMES frames. History: x mean visibility, y view depth, z frames accumulated, w second moment.

#include "rt_shadow_common.hlsli"

[[vk::image_format("rgba16f")]] RWTexture2D<float4> outHistory : register(u0, space1);

static const float SOFT_MAX_FRAMES = 10.0;

[numthreads(8, 8, 1)]
void CSMain(uint3 id : SV_DispatchThreadID) {
    int2 q = int2(id.xy);
    RtShadowParams p = rtsParams();
    if (q.x >= int(p.info.x) || q.y >= int(p.info.y)) return;
    RvkFrame f = rvkLoadFrame(pc.frame);
    float d = rtsDepthMap(p, q);
    float vz = d * f.cameraPos.w;
    if (d >= RTS_SKY) {
        outHistory[q] = float4(1.0, vz, 0.0, 1.0);
        return;
    }
    float2 raw = rvkFetch2D(p.tex.y, q).rg;
    float mn = raw.x, mx = raw.x, pmax = raw.y;
    for (int j = -1; j <= 1; ++j) {
        for (int i = -1; i <= 1; ++i) {
            float2 s = rvkFetch2D(p.tex.y, rtsClamp(p, q + int2(i, j))).rg;
            mn = min(mn, s.x);
            mx = max(mx, s.x);
            pmax = max(pmax, s.y);
        }
    }
    float maxFrames = 1.0 + (SOFT_MAX_FRAMES - 1.0) * smoothstep(0.75, 4.0, pmax * 16.0);
    float hv = raw.x, hm2 = raw.x * raw.x, n = 0.0;
    if (maxFrames > 1.0) {
        float4 h = rtsHistory(f, p, rtsPos(f, p, q, d));
        if (h.w > 0.05) {
            hv = clamp(h.x, mn, mx);
            n = h.y;
            hm2 = h.z;
        }
    }
    float count = min(n + 1.0, maxFrames);
    float a = 1.0 / count;
    outHistory[q] = float4(lerp(hv, raw.x, a), vz, count, lerp(hm2, raw.x * raw.x, a));
}
