// S-7c (frame-ownership.md 14.16): post_hdr.fx technique HDRI (PS_HDRI, listing shader #17: the Low preset's screen pass) on our own pixel shader: the ToneMap colour
// plus bloom x BloomIntensity, then the colour matrix. s0 the HDR scene (point), s1 the 1x1 adapted luminance, s2 the bloom image (linear); c0..c2 the colour matrix,
// c3.x ToneMapFactor, c4.x BloomIntensity, c5.x 1 / WhitePoint. v0 = scene uv, v1 = bloom uv.
#include "post_units.hlsli"

struct RvkPsIn {
    float4 rvk_fragCoord : SV_Position;
    [[vk::location(0)]] float4 v0 : VARY0;
    [[vk::location(1)]] float4 v1 : VARY1;
};
struct RvkPsOut {
    float4 oC0 : SV_Target0;
};

RvkPsOut PSMain(RvkPsIn i) {
    const RvkDrawData d = rvkLoadDraw();
    const float avg = rvkPostTap(d, 1u, float2(0.5, 0.5)).x;
    const float k = (1.0 / avg) * RVK_PSC(3).x;
    const float4 c = rvkPostTap(d, 0u, i.v0.xy);
    const float3 col = rvkPostToneMapColour(c.xyz, k, RVK_PSC(5).x);
    const float4 bloom = rvkPostTap(d, 2u, i.v1.xy);
    const float4 r = bloom * RVK_PSC(4).x + float4(col, c.w);
    RvkPsOut o;
    o.oC0 = float4(dot(r.xyz, RVK_PSC(0).xyz), dot(r.xyz, RVK_PSC(1).xyz), dot(r.xyz, RVK_PSC(2).xyz), r.w);
    return o;
}
