// S-7c (frame-ownership.md 14.16): overlay_color.fx (the overlay effects: TOverlayColor, OverlayRadialColor, OverlayRadialBlur, OverlayImage, OverlayMovieStrips /
// OverlayMovieStripsAbs) on our own pixel shaders, ports of the ps_2_0 listings. The vertex shaders (vs_2_0) write oT0 (uv + half texel), oT1 (that - 0.5: centred),
// oT2 (the quad in -1..1, y scaled by the aspect or 1 / MovieStripsFactor); the blend states stay the draw's (SRCALPHA / INVSRCALPHA, the strips opaque).
#pragma once
#include "post_units.hlsli"

struct RvkPsInOverlay {
    float4 rvk_fragCoord : SV_Position;
    [[vk::location(0)]] float4 t0 : VARY0;
    [[vk::location(1)]] float4 t1 : VARY1;
    [[vk::location(2)]] float4 t2 : VARY2;
};
struct RvkPsOutOverlay {
    float4 oC0 : SV_Target0;
};

// TOverlayColor (shader #1): c0 = (OverlayColor.rgb, OverlayColor.a x BlendValue) (preshader)
float4 rvkOverlayColor() { return RVK_PSC(0); }

// OverlayRadialColor (shader #3): alpha = |p|^2 x RadialFactor (c1.x) x OverlayColor.a (c0.w) x BlendValue (c2.x), colour c3 = OverlayColor.rgb
float4 rvkOverlayRadialColor(RvkPsInOverlay i) {
    const float r = dot(i.t2.xy, i.t2.xy) + 0.0;
    return float4(RVK_PSC(3).xyz, ((r * RVK_PSC(1).x) * RVK_PSC(0).w) * RVK_PSC(2).x);
}

// OverlayRadialBlur (shader #5, OverlayHit): 8 taps of the frame copy (s0) scaled around the centre by 1 + |p|^2 BlendValue BlurScale k / 7, mean x OverlayColor.rgb;
// alpha = |p|^2 BlendValue x RadialFactor. c0 OverlayColor, c1.x RadialFactor, c2.x BlurScale, c3.x BlendValue.
float4 rvkOverlayRadialBlur(RvkPsInOverlay i) {
    const RvkDrawData d = rvkLoadDraw();
    const float2 t1 = i.t1.xy;
    const float z = (dot(i.t2.xy, i.t2.xy) + 0.0) * RVK_PSC(3).x;
    const float s = z * RVK_PSC(2).x;
    const float4 a0 = rvkPostTap(d, 0u, t1 + 0.5);
    const float4 a1 = rvkPostTap(d, 0u, t1 * (s * 0.142857149 + 1.0) + 0.5);
    const float4 a2 = rvkPostTap(d, 0u, t1 * (s * 0.285714298 + 1.0) + 0.5);
    const float4 a3 = rvkPostTap(d, 0u, t1 * (s * 0.428571433 + 1.0) + 0.5);
    const float4 a4 = rvkPostTap(d, 0u, t1 * (s * 0.571428597 + 1.0) + 0.5);
    const float4 a5 = rvkPostTap(d, 0u, t1 * (s * 0.857142866 + 1.0) + 0.5);
    const float4 a6 = rvkPostTap(d, 0u, t1 * (s * 0.714285731 + 1.0) + 0.5);
    const float4 a7 = rvkPostTap(d, 0u, t1 * (z * RVK_PSC(2).x + 1.0) + 0.5);
    float3 sum = a0.xyz + a1.xyz;
    sum = a2.xyz + sum;
    sum = a3.xyz + sum;
    sum = a4.xyz + sum;
    sum = a6.xyz + sum;
    sum = a5.xyz + sum;
    sum = a7.xyz + sum;
    sum = sum * RVK_PSC(0).xyz;
    return float4(sum * 0.125, z * RVK_PSC(1).x);
}

// OverlayImage (shader #7): the image (s0) x OverlayColor (c0), alpha x BlendValue (c1.x)
float4 rvkOverlayImage(RvkPsInOverlay i) {
    const RvkDrawData d = rvkLoadDraw();
    float4 r = rvkPostTap(d, 0u, i.t0.xy) * RVK_PSC(0);
    r.w = r.w * RVK_PSC(1).x;
    return r;
}

// OverlayMovieStrips / OverlayMovieStripsAbs (shaders #9 / #11): the bars where |y| >= 1 (texkill elsewhere), colour c0
float4 rvkOverlayStrips(RvkPsInOverlay i) {
    if (abs(i.t2.y) + -1.0 < 0.0) discard;
    return RVK_PSC(0);
}
