// S-7b (docs/research/frame-ownership.md 14.8b): the bloom chain's last unit, post_bloom.fx "GlowVAddBloom2": the vertical 13-tap glow of the GlowH image + the streak
// image (a texel-centre tap) as rgb, alpha = the pass's blend weight a (= min(FrameRedundancy x FrameTime, MaxOpacity), a pixel-shader constant), blended onto the
// persistent bloom image with SRCALPHA / INVSRCALPHA on all four channels (read-modify-write of the storage image: the D3D9 render target is its own previous
// frame). Same tap table as post_glow.comp (+v2, +v1, +v3 .. +v7, -v1 .. -v7; v7 = 0), the vertical offsets are the composed draw's vertex constants.

#include "rvk_common.hlsli"

// mirror: native::PostBloomParams
struct PostBloomParams {
    uint4 tex;      // x GlowH image (bindless t2D), y streak image, z linear / clamp sampler, w previous bloom image (0: the output itself is read)
    uint4 size;     // x, y: target size
    float4 uv;      // quad uv at pixel (0, 0), step
    float4 center;  // x, y: centre offset (vs c0)
    float4 off[6];  // vs c1..c6 .xy
    float4 blend;   // x: the blend weight a
};

struct PostBloomPush {
    RVK_PASS_PUSH_HEADER
    uint2 params;
    uint2 pad;
};
RVK_PUSH(PostBloomPush, pc);

[[vk::image_format("rgba8")]] RWTexture2D<float4> bloom : register(u0, space1);

[numthreads(8, 8, 1)]
void CSMain(uint3 id : SV_DispatchThreadID) {
    PostBloomParams p = rvkPtr<PostBloomParams, 16>(pc.params).Get();
    if (id.x >= p.size.x || id.y >= p.size.y) return;
    const float2 c = p.uv.xy + p.uv.zw * float2(id.xy) + p.center.xy;
    float2 v[7];
    [unroll] for (int i = 0; i < 6; ++i) v[i] = p.off[i].xy;
    v[6] = float2(0.0, 0.0);
    const float w[7] = {0.265961498, 0.251588792, 0.21296531, 0.161313802, 0.109340027, 0.0663180873, 0.0359939747};
    float3 r = rvkSample2DLod(p.tex.x, p.tex.z, c + v[1], 0.0).rgb * 0.251588792;
    r = rvkSample2DLod(p.tex.x, p.tex.z, c + v[0], 0.0).rgb * 0.265961498 + r;
    [unroll] for (int k = 2; k < 7; ++k) r = rvkSample2DLod(p.tex.x, p.tex.z, c + v[k], 0.0).rgb * w[k] + r;
    [unroll] for (int m = 0; m < 6; ++m) r = rvkSample2DLod(p.tex.x, p.tex.z, c - v[m], 0.0).rgb * w[m] + r;
    r = rvkSample2DLod(p.tex.x, p.tex.z, c - v[6], 0.0).rgb * w[6] + r;
    const float3 s = rvkSample2DLod(p.tex.y, p.tex.z, c, 0.0).rgb;
    const float a = p.blend.x;
    const float4 src = saturate(float4(r + s, a));
    const float4 dst = p.tex.w != 0u ? rvkFetch2D(p.tex.w, int2(id.xy)) : bloom[int2(id.xy)];
    bloom[int2(id.xy)] = saturate(src * a + dst * (1.0 - a));
}
