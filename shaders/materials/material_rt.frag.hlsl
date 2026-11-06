// The traced reflections' composite (enh/material_rt.*): the filtered reflection image onto the scene HDR target, blend result = src.rgb + dst * src.a
// (rgb = the light the reflection adds, a = the factor the scene is multiplied by: puddles darken the surface under them).

#include "rvk_common.hlsli"

struct MaterialRtCompositePush {
    RVK_PASS_PUSH_HEADER
    uint image;    // bindless t2D slot of the filtered reflection image (the scene's size)
    uint sampler;  // bindless sampler index (point)
    uint scale;    // 0: the image has the scene's size (point lookup); 2: half size (low bandwidth mode): the 2 x 2 texels around the pixel, depth-aware
    uint depth;    // bindless t2D slot of the depth map (view depth / far), read when scale > 0
};
RVK_PUSH(MaterialRtCompositePush, pc);

float4 PSMain(float4 position : SV_Position, float2 uv : TEXCOORD0) : SV_Target0 {
    if (pc.scale == 0u) return rvkSample2DLod(pc.image, pc.sampler, uv, 0.0);
    // the half-size image (texel i = the 2 x 2 pixels 2 i .. 2 i + 1; the filter wrote it at the trace texels' centres): a bilinear lookup whose taps
    // are weighted by their depth against this pixel's (a reflection stops at the silhouette of its surface)
    RvkFrame f = rvkLoadFrame(pc.frame);
    float far = f.cameraPos.w;
    float fs = float(pc.scale);
    int2 hs = rvkSize2D(pc.image);
    float2 tf = position.xy / fs - 0.5;
    int2 t0 = int2(floor(tf));
    float2 fr = tf - float2(t0);
    float z0 = rvkSample2DLod(pc.depth, pc.sampler, position.xy * f.viewport.zw, 0.0).r * far;
    float4 sum = float4(0.0, 0.0, 0.0, 0.0);
    float wsum = 0.0;
    for (int i = 0; i < 4; ++i) {
        int2 o = int2(i & 1, i >> 1);
        int2 tp = clamp(t0 + o, int2(0, 0), hs - 1);
        float zt = rvkSample2DLod(pc.depth, pc.sampler, (float2(tp) * fs + 0.5 * fs) * f.viewport.zw, 0.0).r * far;
        float w = (o.x == 0 ? 1.0 - fr.x : fr.x) * (o.y == 0 ? 1.0 - fr.y : fr.y) * exp(-abs(zt - z0) / max(0.03 * z0, 8.0));
        sum += rvkFetch2D(pc.image, tp) * w;
        wsum += w;
    }
    if (wsum > 1e-4) return sum / wsum;
    return rvkFetch2D(pc.image, clamp(int2(position.xy / fs), int2(0, 0), hs - 1));
}
