// S-7b (docs/research/frame-ownership.md 14.8b): the bloom chain's first unit, post_hdr.fx "BrightPassAlphaMask", as a compute kernel. Faithful port of the
// translated pixel shader: four POINT taps of the scene at uv, uv + (cx, 0), uv + (cx, cy), uv + (0, cy) x 0.25 (summed in the shader's order), scaled by
// ToneMapFactor / avgLum (the 1x1 adapted luminance), c *= 1 + c / WhitePoint, max(c - threshold, 0), / (c + offset); alpha = 1 where the depth map (point tap at
// uv + half texel of the source) is 1 (the sky). The quad's uv at a pixel and the tap offsets come from the composed draw (native::OwnPost), the target is a
// storage image (RGBA8) the glow kernel reads.

#include "rvk_common.hlsli"

// mirror: native::PostBrightParams
struct PostBrightParams {
    uint4 tex;   // x scene (bindless t2D), y adapted luminance (1x1 R32F), z depth map, w target width
    float4 uv;   // x, y: the quad's uv at pixel (0, 0); z, w: its step per pixel
    float4 tap;  // x, y: (cx, cy), z, w: the depth tap's offset (half texel of the source)
    float4 ps;   // ToneMapFactor, threshold, offset, 1 / WhitePoint
    uint4 size;  // x, y: target size
};

struct PostBrightPush {
    RVK_PASS_PUSH_HEADER
    uint2 params;
    uint2 pad;
};
RVK_PUSH(PostBrightPush, pc);

[[vk::image_format("rgba8")]] RWTexture2D<float4> outBright : register(u0, space1);

// the point sampler's texel. A coordinate that is a whole texel up to float noise (the bloom's 309 x 210 grid over 1600 x 900: every 7th row) lands on either side in the
// engine's draw, by the rounding luck of the rasteriser's interpolation (measured in compare mode: a few texel flips per frame at the sharpest scene edges, which no
// kernel can replicate); this one picks the upper texel, the same way every frame
int2 texel(float2 uv, int2 size) { return clamp(int2(floor(uv * float2(size) + 0.001953125)), int2(0, 0), size - 1); }

[numthreads(8, 8, 1)]
void CSMain(uint3 id : SV_DispatchThreadID) {
    PostBrightParams p = rvkPtr<PostBrightParams, 16>(pc.params).Get();
    if (id.x >= p.size.x || id.y >= p.size.y) return;
    const float2 uv = p.uv.xy + p.uv.zw * float2(id.xy);
    const int2 sceneSize = rvkSize2D(p.tex.x);
    const float2 c = p.tap.xy;
    float3 a = rvkFetch2D(p.tex.x, texel(uv + float2(c.x, 0.0), sceneSize)).rgb * 0.25;
    a = rvkFetch2D(p.tex.x, texel(uv, sceneSize)).rgb * 0.25 + a;
    a = rvkFetch2D(p.tex.x, texel(uv + c, sceneSize)).rgb * 0.25 + a;
    a = rvkFetch2D(p.tex.x, texel(uv + float2(0.0, c.y), sceneSize)).rgb * 0.25 + a;
    const float avg = rvkFetch2D(p.tex.y, int2(0, 0)).x;
    const float w = (1.0 / avg) * p.ps.x;
    a *= w;
    const float3 r1 = a * p.ps.w + 1.0;
    const float3 m = max(a * r1 - p.ps.y, 0.0);
    const float3 rgb = m * (1.0 / (m + p.ps.z));
    const int2 depthSize = rvkSize2D(p.tex.z);
    const float d = rvkFetch2D(p.tex.z, texel(uv + p.tap.zw, depthSize)).x;
    outBright[int2(id.xy)] = float4(rgb, (d - 1.0) >= 0.0 ? 1.0 : 0.0);
}
