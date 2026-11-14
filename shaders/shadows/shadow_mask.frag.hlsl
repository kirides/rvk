// Enhanced shadows (src/enh/shadows.cpp, docs/research/shadows.md): the sun's screen-space shadow mask in one pass, in
// place of the engine's post_SSM.fx cascade draws (technique SSM: one draw per cascade, each 8 dithered point taps, the
// cascade picked by the stencil, a hard switch at each split and a short fade at the range's end).
//
// Per pixel: the engine's own reconstruction (p = depth-map value * the view ray, the matrices the engine set for its
// cascade draws: p -> cascade texture coordinates), a smooth PCF (a grid of bilinear taps: 4 compares from one gather each,
// with a receiver-plane depth bias), the cascades blended across a band before each split, a long fade at the end of the
// last cascade, times the engine's terrain variance map (VSM) exactly as SSM does. Output: the mask's alpha (the draw keeps
// the engine's colour write mask: alpha only).
// Mirror: enh::ShadowBlock (static_assert'ed size).

#include "shadow_block.hlsli"
#include "shadow_cascade.hlsli"

float4 PSMain(float4 position : SV_Position) : SV_Target0 {
    ShadowPtr b = rvkShadowBlock();
    float2 fragCoord = position.xy;
    DepthCross dc = depthCross(b, fragCoord);
    float d = dc.d;  // the engine samples the depth at t + half a pixel
    float4 p = reconstruct(b, fragCoord, d);
    // the surface's screen gradient from the neighbour on the same surface on each axis (the smaller depth step): screen
    // derivatives (dFdx) jump across every stone and leaf edge, and a wrong receiver plane shadows the surface itself
    float4 px = surfaceStep(b, fragCoord, d, p, float2(1.0, 0.0), dc.right, dc.left);
    float4 py = surfaceStep(b, fragCoord, d, p, float2(0.0, 1.0), dc.down, dc.up);

    uint dbg = b.Get().mode.y;
    float margin = 0.5 * float(clamp(b.Get().mode.x, 1u, 4u)) * max(b.Get().texel[0].z, 1.0) + 1.0;

    // the ray-traced shadow within its range (rt_shadow_*.comp: same depth map, same reconstruction), blended out to the
    // cascades over the last band; its rays see the terrain as well, so the variance map's term goes with it. Where it covers
    // the pixel completely the cascades and the terrain term are not evaluated at all (the debug views want them)
    float rtWeight = 0.0, rtVis = 1.0;
    if (b.Get().rt.x != 0u && d < b.Get().rtParams.x) {
        rtWeight = 1.0 - smoothstep(b.Get().rtParams.x * (1.0 - b.Get().rtParams.y), b.Get().rtParams.x, d);
        if (rtWeight > 0.0) rtVis = rvkFetch2D(b.Get().rt.x, int2(fragCoord)).r;
    }
    if (rtWeight >= 1.0 && dbg == 0u) return float4(rtVis, rtVis, rtVis, rtVis);

    // the cascades' term (shadow_cascade.hlsli). With the soft shadow trace of this frame (shadows.soft: PCSS, temporal + spatial
    // denoise, shadow_pcss.comp) it is that image's value, blend band and fade included; the debug views want the cascades' own
    float shadow, level, pen;
    if (b.Get().rt.y != 0u && dbg == 0u) {
        shadow = rvkFetch2D(b.Get().rt.y, int2(fragCoord)).r;
        level = 1.0;
    } else {
        shadow = cascadeMaskTerm(b, d, p, px, py, margin, level, pen);
    }
    // fully shadowed by the cascades: the product is 0 whatever the terrain term is (no variance map fetch)
    float terrain = (shadow == 0.0 && dbg != 3u) ? 1.0 : varianceShadow(b, p);
    float mask = shadow * terrain;
    mask = lerp(mask, rtVis, rtWeight);
    if (dbg == 1u) mask = level;
    else if (dbg == 2u) mask = shadow;
    else if (dbg == 3u) mask = terrain;
    else if (dbg == 4u) mask = lerp(1.0, rtVis, rtWeight);
    else if (dbg == 5u) mask = rtWeight;
    return float4(mask, mask, mask, mask);
}
