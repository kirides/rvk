// Ray-traced soft sun shadows, stage 2: temporal accumulation (src/enh/rt_shadows.cpp, docs/research/rt-shadows.md). The raw
// visibility (1 ray per pixel and frame: noisy in the penumbra only) is blended into the previous frame's result reprojected
// with the camera's motion (depth-checked bilinear taps of the history), the history clamped to the range of the raw 3x3
// neighbourhood (a shadow that moved leaves no ghost where the neighbourhood is uniform). The history holds
// x mean visibility, y view depth, z frames accumulated, w second moment (the spatial pass's variance estimate).

#include "rt_shadow_common.hlsli"

[[vk::image_format("rgba16f")]] RWTexture2D<float4> outHistory : register(u0, space1);

static const float RTS_MAX_FRAMES = 12.0;

[numthreads(8, 8, 1)]
void CSMain(uint3 id : SV_DispatchThreadID) {
    int2 q = int2(id.xy);
    RtShadowParams p = rtsParams();
    if (q.x >= int(p.info.x) || q.y >= int(p.info.y)) return;
    RvkFrame f = rvkLoadFrame(pc.frame);
    float d = rtsDepthMap(p, q);
    float vz = d * f.cameraPos.w;
    if (d >= RTS_SKY || vz >= p.range.x) {
        outHistory[q] = float4(1.0, vz, 0.0, 1.0);
        return;
    }
    float raw = rvkFetch2D(p.tex.y, q).r;
    float mn = raw, mx = raw;
    for (int j = -1; j <= 1; ++j) {
        for (int i = -1; i <= 1; ++i) {
            float s = rvkFetch2D(p.tex.y, rtsClamp(p, q + int2(i, j))).r;
            mn = min(mn, s);
            mx = max(mx, s);
        }
    }
    float hv = raw, hm2 = raw * raw, n = 0.0;
    {
        float4 h = rtsHistory(f, p, rtsPos(f, p, q, d));
        if (h.w > 0.05) {
            hv = clamp(h.x, mn, mx);
            n = h.y;
            hm2 = h.z;
        }
    }
    float count = min(n + 1.0, RTS_MAX_FRAMES);
    float a = 1.0 / count;
    float v = lerp(hv, raw, a);
    float m2 = lerp(hm2, raw * raw, a);
    outHistory[q] = float4(v, vz, count, m2);
}
