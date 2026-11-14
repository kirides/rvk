// Soft sun shadows (shadows.soft = pcss; src/enh/shadows.cpp, docs/research/shadows.md 8.10), stage 1: the raw cascade term of the mask
// at every pixel of the engine's depth map: PCSS (a blocker search + a filter, each a few Vogel-disk taps rotated per pixel and
// frame; shadow_cascade.hlsli), the cascade blend band and the last cascade's fade included, with the ShadowBlock of this
// frame's mask draw (the same matrices, splits and receiver plane as the PCF grid). Output: R visibility (1 lit), G the filter
// radius (texels / 16: where the edge is soft, i.e. where the temporal / spatial passes may average). The mask draw reads the
// denoised result instead of evaluating the cascades (shadow_mask.frag, rt.y).

#define SHADOW_BLOCK_NO_DRAW 1
#define SHADOW_PCSS 1
#include "rt_shadow_common.hlsli"
#include "shadow_block.hlsli"
#include "shadow_cascade.hlsli"

// two channels: RG8 where the GPU has the extended storage formats (SOFT_RG8, enh/shadows.cpp), else RGBA8
#ifdef SOFT_RG8
[[vk::image_format("rg8")]] RWTexture2D<float4> outRaw : register(u0, space1);
#else
[[vk::image_format("rgba8")]] RWTexture2D<float4> outRaw : register(u0, space1);
#endif

[numthreads(8, 8, 1)]
void CSMain(uint3 id : SV_DispatchThreadID) {
    int2 q = int2(id.xy);
    RtShadowParams rp = rtsParams();
    if (q.x >= int(rp.info.x) || q.y >= int(rp.info.y)) return;
    ShadowPtr b = rvkPtr<ShadowBlock, 16>(rp.aux.xy);
    float2 fragCoord = float2(q) + 0.5;  // (SV_Position of the mask draw)
    DepthCross dc = depthCross(b, fragCoord);
    float d = dc.d;
    float vis = 1.0, pen = 0.0;
    if (d < RTS_SKY) {
        float4 p = reconstruct(b, fragCoord, d);
        float4 px = surfaceStep(b, fragCoord, d, p, float2(1.0, 0.0), dc.right, dc.left);
        float4 py = surfaceStep(b, fragCoord, d, p, float2(0.0, 1.0), dc.down, dc.up);
        float margin = 0.5 * float(clamp(b.Get().mode.x, 1u, 4u)) * max(b.Get().texel[0].z, 1.0) + 1.0;
        gSoftRot = rtsIgn(float2(q), rp.mode.x);
        float level;
        vis = cascadeMaskTerm(b, d, p, px, py, margin, level, pen);
    }
    outRaw[q] = float4(vis, saturate(pen / 16.0), 0.0, 1.0);
}
