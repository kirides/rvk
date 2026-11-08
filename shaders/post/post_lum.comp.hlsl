// S-7 (docs/research/frame-ownership.md 14.8): the engine's luminance chain of the post graphs in TWO dispatches (post_lum_tile.comp: levels 0..3 over 64
// workgroups; this one: levels 4, 5 and the adaptation in one group). The chain is eight
// full-screen draws of post_hdr.fx (LogLuminance into 64x64 R32F, DownSample into 32 / 16 / 8 / 4 / 2, AdaptiveLiminance into 1x1,
// the "AvgLuminance" Copy into the persistent 1x1 image); here one workgroup keeps every level in groupshared memory and writes the two
// 1x1 images the later passes read (the adapted luminance the ToneMap / bright pass divide by, FSR's exposure source).
//
// Faithful port of the translated pixel shaders: every pass averages four POINT taps of the previous level (weights 0.25, summed in the
// shader's order), taps at uv + (0|cx, 0|cy) of the pass's quad: texel = floor(clamp(uv x size)) as the point sampler does. The quad's
// uv at a pixel centre (origin + step x pixel) and the tap offsets come from the composed draws (src/native/own_post.cpp), so the
// half-texel conventions of the engine's quad are inherited, not assumed.
//   level 0: L = ln(dot(avg4(scene rgb), (0.3, 0.59, 0.11)) + 1e-5)
//   level 1..5: avg4
//   level 6 (AdaptiveLiminance): g = exp2(avg4 x 1.44269502) (the geometric mean), a = lrp(f, g, previous), clamped to AvgLumClamp
// Both outputs get the same value (the Adaptive unit's target and the Copy unit's).

#include "rvk_common.hlsli"

struct PostLumStage {
    float4 uv;   // x, y: the quad's uv at pixel (0, 0)'s centre; z, w: its step per pixel
    float4 tap;  // x, y: the taps' uv offsets (cx, cy)
};

// mirror: native::PostLumParams (static_assert'ed size)
struct PostLumParams {
    uint4 tex;            // x scene (bindless t2D), y previous adapted luminance (bindless t2D, 1x1 R32F)
    PostLumStage stage[7];  // 0: 64x64 from the scene, 1..5: 32 .. 2, 6: the 2x2 level -> 1x1
    float4 adapt;         // x lerp factor, y clamp min, z clamp max
};

struct PostLumPush {
    RVK_PASS_PUSH_HEADER
    uint2 params;  // PostLumParams address
    uint2 pad;
};
RVK_PUSH(PostLumPush, pc);

[[vk::image_format("r32f")]] RWTexture2D<float> outAdapt : register(u0, space1);
[[vk::image_format("r32f")]] RWTexture2D<float> outAvg : register(u1, space1);

// levels 3..5 (8x8, 4x4, 2x2): 64 + 16 + 4 floats
groupshared float lvl[84];

static const uint kOffset[3] = {0u, 64u, 80u};

[[vk::image_format("r32f")]] RWTexture2D<float> tileIn : register(u2, space1);

int2 texel(float2 uv, int2 size) { return clamp(int2(floor(uv * float2(size))), int2(0, 0), size - 1); }

[numthreads(8, 8, 1)]
void CSMain(uint3 tid : SV_GroupThreadID) {
    PostLumParams p = rvkPtr<PostLumParams, 16>(pc.params).Get();
    const uint t = tid.y * 8u + tid.x;
    lvl[t] = tileIn[int2(tid.xy)];
    GroupMemoryBarrierWithGroupSync();

    // levels 4, 5: 4 .. 2 per side from the level before (the engine's tap order)
    [unroll] for (uint l = 4; l <= 5; ++l) {
        const uint dim = 64u >> l;
        if (t < dim * dim) {
            const uint2 px = uint2(t % dim, t / dim);
            const int2 sz = int2(dim * 2u, dim * 2u);
            const float2 uv = p.stage[l].uv.xy + p.stage[l].uv.zw * float2(px);
            const float2 c = p.stage[l].tap.xy;
            const uint src = kOffset[l - 4u];
            int2 t0 = texel(uv + float2(c.x, 0.0), sz), t1 = texel(uv, sz), t2 = texel(uv + c, sz), t3 = texel(uv + float2(0.0, c.y), sz);
            float a = lvl[src + uint(t0.y) * (dim * 2u) + uint(t0.x)] * 0.25;
            a += lvl[src + uint(t1.y) * (dim * 2u) + uint(t1.x)] * 0.25;
            a += lvl[src + uint(t2.y) * (dim * 2u) + uint(t2.x)] * 0.25;
            a += lvl[src + uint(t3.y) * (dim * 2u) + uint(t3.x)] * 0.25;
            lvl[kOffset[l - 3u] + t] = a;
        }
        GroupMemoryBarrierWithGroupSync();
    }

    // level 6: AdaptiveLiminance (1x1 from the 2x2 level), then the Copy
    if (t == 0u) {
        const int2 sz = int2(2, 2);
        const float2 uv = p.stage[6].uv.xy;
        const float2 c = p.stage[6].tap.xy;
        const uint src = kOffset[2];
        int2 t0 = texel(uv + float2(c.x, 0.0), sz), t1 = texel(uv, sz), t2 = texel(uv + c, sz), t3 = texel(uv + float2(0.0, c.y), sz);
        float a = lvl[src + uint(t0.y) * 2u + uint(t0.x)] * 0.25;
        a += lvl[src + uint(t1.y) * 2u + uint(t1.x)] * 0.25;
        a += lvl[src + uint(t2.y) * 2u + uint(t2.x)] * 0.25;
        a += lvl[src + uint(t3.y) * 2u + uint(t3.x)] * 0.25;
        const float geo = exp2(a * 1.44269502);
        const float prev = tileIn[int2(0, 8)];
        const float cur = p.adapt.x * (geo - prev) + prev;  // lrp
        const float v = min(p.adapt.z, max(cur, p.adapt.y));
        outAdapt[int2(0, 0)] = v;
        outAvg[int2(0, 0)] = v;
    }
}
