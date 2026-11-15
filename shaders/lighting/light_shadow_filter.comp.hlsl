// Ray-traced point-light shadows, pass 2: a depth-aware 5 x 5 blur of every entry's visibility (src/enh/light_table.cpp, docs/research/gpu-scene.md 9.10).
// The trace pass shoots one ray per light and pixel from a rotating disc point: the penumbra is a dither of lit / blocked pixels; the blur turns it into the
// soft edge of the engine's own shadows. Neighbours count when they lie on the pixel's surface (view depth within 2 % + 3 units) and hold the same light; a
// neighbour that holds the light but is not shadow-tested (an entry the trace did not pick there) counts as lit.
// The taps are the OUTER loop (one depth + one raw fetch per tap, shared by all four entries): the same sums as the per-entry loops, a quarter of the fetches.
// With a half resolution raw image (misc.z bit 0, texel h = the full-resolution pixel 2h): the pixel takes its lights from the nearest raw texel that
// lies on its surface and averages the 5 x 5 raw texels around that one (the same depth test against the pixel's own depth).

#include "light_shadow_common.hlsli"

[[vk::image_format("rgba16f")]] RWTexture2D<float4> outFinal : register(u0, space1);

[numthreads(8, 8, 1)]
void CSMain(uint3 id : SV_DispatchThreadID) {
    int2 q = int2(id.xy);
    if (q.x >= int(pc.tex.z) || q.y >= int(pc.tex.w)) return;
    const bool halfRes = (pc.misc.z & 1u) != 0u;
    const int2 rext = halfRes ? (int2(pc.tex.zw) + 1) / 2 : int2(pc.tex.zw);
    float4 result = float4(0.0, 0.0, 0.0, 0.0);
    RvkFrame f = rvkLoadFrame(pc.frame);
    float d = lsDepth(q);
    float vz = d * f.cameraPos.w;
    float tol = 0.02 * vz + 3.0;
    int2 centre = q;       // the raw texel the lights are taken from
    int ext = 2;           // tap radius (raw texels)
    float4 c;
    if (halfRes) {
        ext = 2;
        // the four raw texels around q (texel h sits at full-resolution 2h): the one on q's surface with the smallest depth difference that holds an entry
        int2 h0 = q >> 1;
        int2 hs = int2((q.x & 1) != 0 ? 1 : 0, (q.y & 1) != 0 ? 1 : 0);
        float best = 1e30;
        c = float4(0.0, 0.0, 0.0, 0.0);
        centre = h0;
        [unroll] for (int k = 0; k < 4; ++k) {
            int2 h = min(h0 + int2(k & 1, k >> 1) * hs, rext - 1);
            float dh = lsDepth(h * 2);
            float4 e = rvkFetch2D(pc.tex.y, h);
            float diff = abs(dh - d) * f.cameraPos.w;
            if (any(e > 0.0) && diff <= tol && diff < best) {
                best = diff;
                c = e;
                centre = h;
            }
        }
    } else {
        c = rvkFetch2D(pc.tex.y, q);
    }
    result = halfRes ? float4(0.0, 0.0, 0.0, 0.0) : c;
    if (any(c > 0.0) && d < LS_SKY) {
        uint sid[RVK_LS_ENTRIES];
        float sum[RVK_LS_ENTRIES], wsum[RVK_LS_ENTRIES];
        [unroll] for (uint e = 0; e < RVK_LS_ENTRIES; ++e) {
            sid[e] = c[e] > 0.0 ? lsId(c[e]) : 0u;
            sum[e] = 0.0;
            wsum[e] = 0.0;
        }
        for (int dy = -ext; dy <= ext; ++dy) {
            for (int dx = -ext; dx <= ext; ++dx) {
                int2 t = clamp(centre + int2(dx, dy), int2(0, 0), rext - 1);
                float dn = lsDepth(halfRes ? min(t * 2, int2(pc.tex.zw) - 1) : t);
                if (abs(dn - d) * f.cameraPos.w > tol) continue;
                float4 n = rvkFetch2D(pc.tex.y, t);
                uint nid[RVK_LS_ENTRIES];
                [unroll] for (uint k = 0; k < RVK_LS_ENTRIES; ++k) nid[k] = n[k] > 0.0 ? lsId(n[k]) : 0u;
                [unroll] for (uint e = 0; e < RVK_LS_ENTRIES; ++e) {
                    if (sid[e] == 0u) continue;
                    float v = 1.0;
                    bool have = false;
                    [unroll] for (uint k = 0; k < RVK_LS_ENTRIES; ++k) {
                        if (nid[k] == sid[e]) {
                            v = lsVis(n[k]);
                            have = true;
                        }
                    }
                    // the light is not in the neighbour's list: it is lit there (not among its best), unless nothing is stored at all (sky / other cluster)
                    float w = have ? 1.0 : 0.5;
                    sum[e] += v * w;
                    wsum[e] += w;
                }
            }
        }
        [unroll] for (uint e = 0; e < RVK_LS_ENTRIES; ++e) {
            if (sid[e] == 0u) continue;
            result[e] = lsEncode(sid[e], wsum[e] > 0.0 ? sum[e] / wsum[e] : lsVis(c[e]));
        }
    }
    outFinal[q] = result;
}
