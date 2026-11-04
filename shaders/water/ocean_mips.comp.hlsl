// FFT ocean: the mip chain of a displacement / derivative image in two dispatches instead of one draw (blit) per level.
// The structure follows AMD FidelityFX SPD (Single Pass Downsampler, MIT licence, (c) Advanced Micro Devices, Inc.): one
// workgroup reduces a tile through groupshared memory, several levels per dispatch. Differences: no global atomic counter /
// globallycoherent last-workgroup stage (the root signature has 8 storage images, u0..u7: a source level + 6 destination
// levels per dispatch), so the chain is cut into dispatch A (mip 0 -> 1..6, one workgroup per 64 x 64 texels, no
// cross-workgroup reads) and dispatch B (mip 6 -> 7..9, one workgroup; the source is at most 8 x 8). Both use this shader.
//
// Filter: the 2 x 2 box average, which is what the bilinear blit of a power-of-two level computes (the levels are rgba16f
// like the blit's; intermediate levels stay in float inside the group, the old chain rounded to half at every level).
// Source = u0 (a mip level of the image), destinations u1..u6 (the next levels). Group tile = 32 x 32 texels of level 1.
#include "rvk_common.hlsli"

// mirror: enh::OceanMipPush
struct MipPush {
    RVK_PASS_PUSH_HEADER
    uint4 info;  // x source size (texels per side), y levels to write (1..6)
};
RVK_PUSH(MipPush, pc);

[[vk::image_format("rgba16f")]] RWTexture2D<float4> srcImg : register(u0, space1);
[[vk::image_format("rgba16f")]] RWTexture2D<float4> dst1 : register(u1, space1);
[[vk::image_format("rgba16f")]] RWTexture2D<float4> dst2 : register(u2, space1);
[[vk::image_format("rgba16f")]] RWTexture2D<float4> dst3 : register(u3, space1);
[[vk::image_format("rgba16f")]] RWTexture2D<float4> dst4 : register(u4, space1);
[[vk::image_format("rgba16f")]] RWTexture2D<float4> dst5 : register(u5, space1);
[[vk::image_format("rgba16f")]] RWTexture2D<float4> dst6 : register(u6, space1);

groupshared float4 tile[32 * 32];

void storeLevel(uint level, uint2 p, float4 v) {
    if (level == 1) dst1[p] = v;
    else if (level == 2) dst2[p] = v;
    else if (level == 3) dst3[p] = v;
    else if (level == 4) dst4[p] = v;
    else if (level == 5) dst5[p] = v;
    else dst6[p] = v;
}

[numthreads(16, 16, 1)]
void CSMain(uint3 gid : SV_GroupID, uint3 tid : SV_GroupThreadID) {
    const uint srcSize = pc.info.x;
    const uint levels = pc.info.y;
    const uint o1 = min(32u, srcSize >> 1);  // the group's tile of level 1 (per side)
    const uint2 org = gid.xy * 32u;          // its first texel in level 1

    // level 1 from the source: each thread 2 x 2 texels of level 1
    [unroll] for (uint q = 0; q < 4; ++q) {
        uint2 lp = tid.xy * 2u + uint2(q & 1u, q >> 1);
        if (lp.x < o1 && lp.y < o1) {
            int2 s = int2((org + lp) * 2u);
            float4 v = 0.25 * (srcImg[s] + srcImg[s + int2(1, 0)] + srcImg[s + int2(0, 1)] + srcImg[s + int2(1, 1)]);
            tile[lp.y * 32u + lp.x] = v;
            dst1[org + lp] = v;
        }
    }
    GroupMemoryBarrierWithGroupSync();

    // levels 2..6 from groupshared memory (one texel per thread, the active square halves every level)
    [unroll] for (uint l = 2; l <= 6; ++l) {
        if (l <= levels) {
            const uint sz = o1 >> (l - 1);
            float4 v = 0.0;
            const bool active = tid.x < sz && tid.y < sz;
            if (active) {
                uint2 s = tid.xy * 2u;
                v = 0.25 * (tile[s.y * 32u + s.x] + tile[s.y * 32u + s.x + 1u] + tile[(s.y + 1u) * 32u + s.x] + tile[(s.y + 1u) * 32u + s.x + 1u]);
            }
            GroupMemoryBarrierWithGroupSync();  // (everyone has read level l - 1 before it is overwritten)
            if (active) {
                tile[tid.y * 32u + tid.x] = v;
                storeLevel(l, (gid.xy * (32u >> (l - 1))) + tid.xy, v);
            }
            GroupMemoryBarrierWithGroupSync();
        }
    }
}
