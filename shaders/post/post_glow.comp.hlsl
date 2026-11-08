// S-7b (docs/research/frame-ownership.md 14.8b): the bloom chain's "GlowH" (post_bloom.fx) and "Streak" (post_streaks.fx) units in one dispatch, both from the
// bright pass image (linear / clamp taps, as the engine's samplers). Faithful ports of the translated pixel shaders; every offset is a composed draw's vertex-shader
// constant (native::OwnPost checks their pattern), the kernels' weights are the shaders' `def` constants.
//   GlowH: 13 taps, centre uv + c0, offsets +/- v1..v6 (v7 = 0: the engine's vertex shader outputs a zero there) x weights (0.2516, 0.266, ...) summed in the
//          shader's order (+v2, +v1, +v3 .. +v7, then -v1 .. -v7).
//   Streak: 64 steps towards the sun's screen position, rgb x alpha, decaying weights.

#include "rvk_common.hlsli"

// mirror: native::PostGlowParams
struct PostGlowParams {
    uint4 tex;      // x bright image (bindless t2D), y linear / clamp sampler, z, w target size
    float4 uvH;     // quad uv at pixel (0, 0), step (GlowH)
    float4 centerH; // x, y: centre offset (vs c0) of GlowH
    float4 offH[6]; // vs c1..c6 .xy
    float4 uvS;     // Streak: quad uv at pixel (0, 0), step
    float4 centerS; // x, y: the vertex shader's half texel
    float4 light;   // x, y: sun screen position (ps c0.xy), z: density / 64 (c1.x), w: flare intensity (c2.x)
    float4 sw;      // x: weight (c3.x), y: decay (c4.x)
};

struct PostGlowPush {
    RVK_PASS_PUSH_HEADER
    uint2 params;
    uint2 pad;
};
RVK_PUSH(PostGlowPush, pc);

[[vk::image_format("rgba8")]] RWTexture2D<float4> outH : register(u0, space1);
[[vk::image_format("rgba8")]] RWTexture2D<float4> outS : register(u1, space1);

[numthreads(8, 8, 1)]
void CSMain(uint3 id : SV_DispatchThreadID) {
    PostGlowParams p = rvkPtr<PostGlowParams, 16>(pc.params).Get();
    if (id.x >= p.tex.z || id.y >= p.tex.w) return;
    // ---- GlowH ----
    {
        const float2 c = p.uvH.xy + p.uvH.zw * float2(id.xy) + p.centerH.xy;
        float2 v[7];
        [unroll] for (int i = 0; i < 6; ++i) v[i] = p.offH[i].xy;
        v[6] = float2(0.0, 0.0);
        const float w[7] = {0.265961498, 0.251588792, 0.21296531, 0.161313802, 0.109340027, 0.0663180873, 0.0359939747};
        // (v[k] = the engine's varying v(k + 1): the shader sums +v2 first, then +v1, +v3 .. +v7, then -v1 .. -v7)
        float4 r = rvkSample2DLod(p.tex.x, p.tex.y, c + v[1], 0.0) * 0.251588792;
        r = rvkSample2DLod(p.tex.x, p.tex.y, c + v[0], 0.0) * 0.265961498 + r;
        [unroll] for (int k = 2; k < 7; ++k) r = rvkSample2DLod(p.tex.x, p.tex.y, c + v[k], 0.0) * w[k] + r;
        [unroll] for (int m = 0; m < 7; ++m) r = rvkSample2DLod(p.tex.x, p.tex.y, c - v[m], 0.0) * w[m] + r;
        outH[int2(id.xy)] = saturate(r);
    }
    // ---- Streak ----
    {
        const float2 v0 = p.uvS.xy + p.uvS.zw * float2(id.xy) + p.centerS.xy;
        const float2 dir = v0 - p.light.xy;
        float4 s = rvkSample2DLod(p.tex.x, p.tex.y, v0, 0.0);
        s.rgb *= s.w;
        float4 acc = float4(s.rgb, s.w);
        float2 pos = v0;
        float atten = 1.0;
        for (int i = 0; i < 64; ++i) {
            pos = dir * (-p.light.z) + pos;
            float4 t = rvkSample2DLod(p.tex.x, p.tex.y, pos, 0.0);
            t.rgb *= t.w;
            t *= atten;
            acc = t * p.sw.x + acc;
            atten *= p.sw.y;
        }
        outS[int2(id.xy)] = saturate(float4(acc.rgb * p.light.w, acc.w));
    }
}
