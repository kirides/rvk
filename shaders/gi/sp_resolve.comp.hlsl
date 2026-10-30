// Screen probe resolve (M10 stage C P8, src/enh/gi.cpp, docs/research/rtgi.md 11, 20): per pixel the nearby tile probes,
// weighted by a Gaussian of the distance to each tile's centre x depth-plane x normal similarity (the probes' own surface
// from the geometry image), then temporal accumulation with the previous frame's result at the motion-reprojected position.
//
// The history is validated per tap (the stored view depth against the reprojected one: a disoccluded pixel starts again
// instead of blending in another surface), counted (the weight of a new frame is 1 / (count + 1) down to the history
// limit: a pixel that just appeared converges at once, a settled one averages many frames) and clamped to the probes'
// range. While the count is small the spatial filter widens to a 5 x 5 probes (the first frames of a pixel are the noisy
// ones). Outputs:
//   outResolved rgb = the GI ratio the material shaders of this frame read at their own pixel, a = the pixel's view depth
//               (0 = no probe covered it): a draw whose depth differs (not in the z pre-pass: characters, alpha) leaves it alone
//   outStats    rgb = the directional correction (gi.directional), a = frames accumulated
// Normals and motion from the depth (after the z pre-pass, before the G-buffer). HLSL port of sp_resolve.comp.

#include "rvk_common.hlsli"
#include "rvk_gi.hlsli"
#include "gi_common.hlsli"
#include "screen_common.hlsli"

[[vk::image_format("rgba16f")]] RWTexture2D<float4> outResolved : register(u0, space1);
[[vk::image_format("rgba16f")]] RWTexture2D<float4> outStats : register(u1, space1);

[numthreads(8, 8, 1)]
void CSMain(uint3 id : SV_DispatchThreadID) {
    GiPassParams p = giParams();
    RvkFrame f = rvkLoadFrame(pc.frame);
    int2 px = int2(id.xy);
    int2 sizeI = int2(p.Get().screen2.zw);
    if (px.x >= sizeI.x || px.y >= sizeI.y) return;
    float2 size = float2(sizeI);
    float zpx = rvkHwDepthToD3D(f, rvkFetch2D(p.Get().screen.x, px).r);
    float viewZ = spViewDepth(f, zpx);
    if (viewZ <= 0.0) {
        outResolved[px] = float4(1.0, 1.0, 1.0, 0.0);
        outStats[px] = float4(0.0, 0.0, 0.0, 0.0);
        return;
    }
    float3 N = spDepthNormal(f, px, size, p.Get().screen.x);
    float3 P = spWorldPos(f, px, size, zpx);
    const float3 lw = float3(0.2126, 0.7152, 0.0722);

    // ---- the history: the 2 x 2 texels around the reprojected position, each valid when its stored view depth is the one
    // this surface had in the previous frame (the weights of the valid ones renormalised)
    float3 hist = float3(0.0, 0.0, 0.0), histG = float3(0.0, 0.0, 0.0);
    float histN = 0.0, histW = 0.0;
    if (p.Get().screen3.y != 0u) {
        float4 q = mul(float4(P, 1.0), f.prevViewProj);
        if (q.w > 1e-3) {
            float2 pprev = (q.xy / q.w) * float2(0.5, -0.5) + 0.5;
            pprev = pprev * size - 0.5;
            int2 i0 = int2(floor(pprev));
            float2 fr = pprev - float2(i0);
            float tol = max(q.w * 0.03, 6.0);
            for (int i = 0; i < 4; ++i) {
                int2 o = int2(i & 1, i >> 1);
                int2 t = i0 + o;
                if (any(t < int2(0, 0)) || any(t >= sizeI)) continue;
                float4 h = rvkFetch2D(p.Get().screen3.y, t);
                if (h.a <= 0.0 || abs(h.a - q.w) > tol) continue;
                float4 g = rvkFetch2D(p.Get().screen3.z, t);
                float w = (o.x == 0 ? 1.0 - fr.x : fr.x) * (o.y == 0 ? 1.0 - fr.y : fr.y);
                hist += h.rgb * w;
                histG += g.rgb * w;
                histN += g.a * w;
                histW += w;
            }
        }
    }
    bool haveHist = histW > 0.3;
    float count = 0.0;
    if (haveHist) {
        hist /= histW;
        histG /= histW;
        // partly covered taps (an edge): the count shrinks with the missing share, the next frames weigh the pixel in faster
        count = (histN / histW) * smoothstep(0.3, 0.9, histW);
    }

    // ---- the spatial filter over the tile probes: 3 x 3, widening to 5 x 5 (and a wider Gaussian) while the history is short
    float tsize = float(max(p.Get().screen2.x, 1u));
    int2 grid = int2(p.Get().screen.zw);
    int2 centre = clamp(int2((float2(px) + 0.5) / tsize), int2(0, 0), grid - 1);
    float ring = clamp((16.0 - count) / 12.0, 0.0, 1.0);  // 1 for a fresh pixel .. 0 once it has 16 frames
    float sigma = 1.0 + 0.6 * ring;
    float sum = 0.0, lo = 1e9, hi = -1e9, wsum = 0.0;
    float3 gsum = float3(0.0, 0.0, 0.0);
    float2 csum = float2(0.0, 0.0);  // the chroma ratio (colour bleeding): a wider Gaussian (sigma two tiles), always the 5 x 5 probes
    float cwsum = 0.0;
    for (int i = 0; i < 25; ++i) {
        int2 d = int2(i % 5 - 2, i / 5 - 2);
        bool outer = max(abs(d.x), abs(d.y)) == 2;
        int2 tile = clamp(centre + d, int2(0, 0), grid - 1);
        float4 pr = rvkFetch2D(p.Get().screen3.x, tile);
        if (pr.a <= 0.0) continue;
        float4 geo = rvkFetch2D(p.Get().screen.y, tile);
        // the distance to the tile's centre (stable from frame to frame; the probe's own pixel is jittered per frame: as the
        // weight it made the filter flicker, measured)
        float2 dpx = ((float2(centre + d) + 0.5) * tsize - (float2(px) + 0.5)) / tsize;
        float d2 = dot(dpx, dpx);
        // the probe's distance to this pixel's tangent plane (slanted surfaces keep their neighbours) and normal similarity
        float plane = abs(dot(geo.xyz - P, N));
        float surf = exp(-plane / max(viewZ * 0.01, 2.0)) * pow(max(dot(spUnpackNormal(geo.w), N), 0.0), 4.0);
        float wc = (exp(-0.125 * d2) + 1e-3) * surf;
        csum += pr.gb * wc;
        cwsum += wc;
        float w = (exp(-0.5 * d2 / (sigma * sigma)) + 1e-3) * surf;
        if (outer) w *= ring;
        if (w <= 0.0) continue;
        sum += pr.r * w;
        gsum += rvkFetch2D(p.Get().screen5.x, tile).rgb * w;
        wsum += w;
        lo = min(lo, pr.r);
        hi = max(hi, pr.r);
    }
    float3 chroma = float3(1.0, 1.0, 1.0);
    if (cwsum > 1e-4) {
        float2 c = csum / cwsum;
        chroma = float3(c.x, (1.0 - lw.r * c.x - lw.b * c.y) / lw.g, c.y);
    }
    float4 result = float4(1.0, 1.0, 1.0, 0.0);
    float4 stats = float4(0.0, 0.0, 0.0, 0.0);
    bool covered = wsum > 1e-4;
    if (covered) {
        float kNow = sum / wsum;
        float3 gNow = gsum / wsum * p.Get().screen6.x;
        float n1 = min(count + 1.0, 255.0);
        if (haveHist) {
            // the new frame's weight: 1 / (count + 1), not below the history limit (screen4.z); the chroma keeps a longer history
            // (colour noise is the visible kind)
            float limit = 1.0 - p.Get().screen4.z;
            float a = max(1.0 / n1, limit);
            float ac = max(1.0 / n1, limit * 0.3);
            float hl = dot(hist, lw);
            float3 hchroma = hl > 1e-3 ? clamp(hist / hl, float3(0.6, 0.6, 0.6), float3(1.6, 1.6, 1.6)) : float3(1.0, 1.0, 1.0);
            float hk = clamp(hl, lo - 0.25, hi + 0.25);
            float k = lerp(hk, kNow, a);
            float3 c = lerp(hchroma, chroma, ac);
            result = float4(float3(k, k, k) * c, viewZ);
            stats = float4(lerp(histG, gNow, a), n1);
        } else {
            result = float4(float3(kNow, kNow, kNow) * chroma, viewZ);
            stats = float4(gNow, 1.0);
        }
    } else if (haveHist) {
        result = float4(hist, viewZ);
        stats = float4(histG, min(count + 1.0, 255.0));
    }
    outResolved[px] = result;
    outStats[px] = stats;
}
