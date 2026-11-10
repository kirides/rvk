// S-7c (frame-ownership.md 14.16): post_hdr.fx technique ToneMap (PS_ToneMap, listing shader #13) on our own pixel shader. s0 the HDR scene (point), s1 the 1x1
// adapted luminance (point); c0..c2 the colour matrix rows, c3.x ToneMapFactor, c4.x 1 / WhitePoint (preshader). Target: the ToneMap image (A8R8G8B8).
#include "post_units.hlsli"

struct RvkPsIn {
    float4 rvk_fragCoord : SV_Position;
    [[vk::location(0)]] float4 v0 : VARY0;
};
struct RvkPsOut {
    float4 oC0 : SV_Target0;
};

RvkPsOut PSMain(RvkPsIn i) {
    const RvkDrawData d = rvkLoadDraw();
    const float avg = rvkPostTap(d, 1u, float2(0.5, 0.5)).x;
    const float k = (1.0 / avg) * RVK_PSC(3).x;
    const float4 c = rvkPostTap(d, 0u, i.v0.xy);
    const float3 col = rvkPostToneMapColour(c.xyz, k, RVK_PSC(4).x);
    RvkPsOut o;
    o.oC0 = float4(dot(col, RVK_PSC(0).xyz), dot(col, RVK_PSC(1).xyz), dot(col, RVK_PSC(2).xyz), c.w);
    return o;
}
