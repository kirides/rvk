// Ray-traced point-light shadows, pass 2: a depth-aware 5 x 5 blur of every entry's visibility (src/enh/light_table.cpp, docs/research/gpu-scene.md 9.10).
// The trace pass shoots one ray per light and pixel from a rotating disc point: the penumbra is a dither of lit / blocked pixels; the blur turns it into the
// soft edge of the engine's own shadows. Neighbours count when they lie on the pixel's surface (view depth within 2 % + 3 units) and hold the same light; a
// neighbour that holds the light but is not shadow-tested (an entry the trace did not pick there) counts as lit.

#include "light_shadow_common.hlsli"

[[vk::image_format("rgba16f")]] RWTexture2D<float4> outFinal : register(u0, space1);

[numthreads(8, 8, 1)]
void CSMain(uint3 id : SV_DispatchThreadID) {
    int2 q = int2(id.xy);
    if (q.x >= int(pc.tex.z) || q.y >= int(pc.tex.w)) return;
    float4 c = rvkFetch2D(pc.tex.y, q);
    float4 result = c;
    if (any(c > 0.0)) {
        RvkFrame f = rvkLoadFrame(pc.frame);
        float d = lsDepth(q);
        float vz = d * f.cameraPos.w;
        float tol = 0.02 * vz + 3.0;
        [unroll] for (uint e = 0; e < RVK_LS_ENTRIES; ++e) {
            if (c[e] <= 0.0) continue;
            uint sid = lsId(c[e]);
            float sum = 0.0, wsum = 0.0;
            for (int dy = -2; dy <= 2; ++dy) {
                for (int dx = -2; dx <= 2; ++dx) {
                    int2 t = clamp(q + int2(dx, dy), int2(0, 0), int2(pc.tex.zw) - 1);
                    float dn = lsDepth(t);
                    if (abs(dn - d) * f.cameraPos.w > tol) continue;
                    float4 n = rvkFetch2D(pc.tex.y, t);
                    float v = 1.0;
                    bool have = false;
                    [unroll] for (uint k = 0; k < RVK_LS_ENTRIES; ++k) {
                        if (n[k] > 0.0 && lsId(n[k]) == sid) {
                            v = lsVis(n[k]);
                            have = true;
                        }
                    }
                    // the light is not in the neighbour's list: it is lit there (not among its best), unless nothing is stored at all (sky / other cluster)
                    float w = have ? 1.0 : 0.5;
                    sum += v * w;
                    wsum += w;
                }
            }
            result[e] = lsEncode(sid, wsum > 0.0 ? sum / wsum : lsVis(c[e]));
        }
    }
    outFinal[q] = result;
}
