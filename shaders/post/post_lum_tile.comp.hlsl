// S-7: dispatch A of the luminance chain (see post_lum.comp.hlsl): 64 workgroups of 8x8, each takes an 8x8 tile of the 64x64 LogLuminance level straight
// from the scene (4 point taps per texel, the engine's order) and reduces it through levels 1..3 (2x2 boxes: the engine's DownSample taps are aligned
// blocks, checked per run by native::OwnPost) to one texel of the 8x8 level, written to `tileOut`.

#include "rvk_common.hlsli"

struct PostLumStage {
    float4 uv;
    float4 tap;
};
struct PostLumParams {
    uint4 tex;
    PostLumStage stage[7];
    float4 adapt;
};
struct PostLumPush {
    RVK_PASS_PUSH_HEADER
    uint2 params;
    uint2 pad;
};
RVK_PUSH(PostLumPush, pc);

[[vk::image_format("r32f")]] RWTexture2D<float> tileOut : register(u0, space1);

groupshared float tile[64 + 16 + 4];

int2 texel(float2 uv, int2 size) { return clamp(int2(floor(uv * float2(size))), int2(0, 0), size - 1); }

[numthreads(8, 8, 1)]
void CSMain(uint3 gid : SV_GroupID, uint3 tid : SV_GroupThreadID) {
    PostLumParams p = rvkPtr<PostLumParams, 16>(pc.params).Get();
    const uint t = tid.y * 8u + tid.x;
    const int2 sceneSize = rvkSize2D(p.tex.x);
    const uint2 px = gid.xy * 8u + tid.xy;
    const float2 uv = p.stage[0].uv.xy + p.stage[0].uv.zw * float2(px);
    const float2 c = p.stage[0].tap.xy;
    float3 a = rvkFetch2D(p.tex.x, texel(uv + float2(c.x, 0.0), sceneSize)).rgb * 0.25;
    a += rvkFetch2D(p.tex.x, texel(uv, sceneSize)).rgb * 0.25;
    a += rvkFetch2D(p.tex.x, texel(uv + c, sceneSize)).rgb * 0.25;
    a += rvkFetch2D(p.tex.x, texel(uv + float2(0.0, c.y), sceneSize)).rgb * 0.25;
    const float lum = dot(a, float3(0.300000012, 0.589999974, 0.109999999)) + 9.99999975e-06;
    tile[t] = log2(lum) * 0.693147182;
    GroupMemoryBarrierWithGroupSync();
    // 8x8 -> 4x4 -> 2x2 -> 1x1 (aligned 2x2 boxes)
    if (t < 16u) {
        const uint2 q = uint2(t & 3u, t >> 2u);
        const uint b = (q.y * 2u) * 8u + q.x * 2u;
        tile[64u + t] = (tile[b] + tile[b + 1u] + tile[b + 8u] + tile[b + 9u]) * 0.25;
    }
    GroupMemoryBarrierWithGroupSync();
    if (t < 4u) {
        const uint2 q = uint2(t & 1u, t >> 1u);
        const uint b = 64u + (q.y * 2u) * 4u + q.x * 2u;
        tile[80u + t] = (tile[b] + tile[b + 1u] + tile[b + 4u] + tile[b + 5u]) * 0.25;
    }
    GroupMemoryBarrierWithGroupSync();
    if (t == 0u) tileOut[int2(gid.xy)] = (tile[80] + tile[81] + tile[82] + tile[83]) * 0.25;
}
