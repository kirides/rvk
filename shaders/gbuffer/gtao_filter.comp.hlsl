// GTAO filter (M10 P7, src/enh/gbuffer.cpp): a depth-aware 3x3 blur of the raw AO, then temporal accumulation with the
// previous frame's history at the motion-reprojected position, clamped to the raw AO's 3x3 range (rejects history from
// disoccluded / moved surfaces without a depth history). Runs after the z pre-pass: r = the AO the material shaders of this
// frame read at their own pixel, g = its view depth (a draw at another depth: not in the z pre-pass, leaves it alone).

#include "gbuffer_common.hlsli"

[[vk::image_format("rg16f")]] RWTexture2D<float2> outAo : register(u0, space1);

float viewDepth(RvkFrame f, int2 p) {
    float z = rvkHwDepthToD3D(f, rvkFetch2D(pc.depthTex, p).r);
    float a = f.proj[2][2], b = f.proj[3][2];
    return z < 1.0 && abs(z - a) > 1e-9 ? b / (z - a) : 1e9;
}

[numthreads(8, 8, 1)]
void CSMain(uint3 id : SV_DispatchThreadID) {
    int2 p = int2(id.xy);
    if (p.x >= int(pc.width) || p.y >= int(pc.height)) return;
    RvkFrame f = rvkLoadFrame(pc.frame);
    int2 maxP = int2(int(pc.width) - 1, int(pc.height) - 1);
    float d0 = viewDepth(f, p);
    float sum = 0.0, wsum = 0.0, lo = 1.0, hi = 0.0;
    for (int y = -1; y <= 1; ++y) {
        for (int x = -1; x <= 1; ++x) {
            int2 q = clamp(p + int2(x, y), int2(0, 0), maxP);
            float a = rvkFetch2D(pc.aoTex, q).r;
            float d = viewDepth(f, q);
            float w = exp(-abs(d - d0) / max(d0 * 0.02, 1.0)) * (x == 0 && y == 0 ? 2.0 : 1.0);
            sum += a * w;
            wsum += w;
            lo = min(lo, a);
            hi = max(hi, a);
        }
    }
    float ao = sum / max(wsum, 1e-5);
    if (pc.historyTex != 0u) {
        float2 uv = (float2(p) + 0.5) / float2(pc.width, pc.height);
        float2 prev = uv + gbCameraMotion(f, gbWorldPos(f, uv, rvkHwDepthToD3D(f, rvkFetch2D(pc.depthTex, p).r)));
        if (all(prev >= float2(0.0, 0.0)) && all(prev <= float2(1.0, 1.0))) {
            float h = rvkSample2DLod(pc.historyTex, pc.linearSampler, prev, 0.0).r;
            h = clamp(h, lo - 0.1, hi + 0.1);
            ao = lerp(ao, h, pc.blend);
        }
    }
    outAo[p] = float2(clamp(ao, 0.0, 1.0), d0 < 1e8 ? d0 : 0.0);
}
