// S-7c (frame-ownership.md 14.16): post_dof.fx techniques DoFDown (listing shader #1) and DoF (shader #11, the post chain's back-buffer draw) on our own pixel
// shaders. RVK_POST_FUSED (post_dofdown_tm / post_dof_tm): the ToneMap image's taps are the HDR scene tone-mapped here (post_units.hlsli: rvkPostTmTexel /
// rvkPostTmCorner); the ToneMap unit is then not drawn. Without it the ports read the ToneMap image (s0) exactly as the translated shaders do.
#pragma once
#include "post_units.hlsli"

#ifndef RVK_POST_FUSED
#define RVK_POST_FUSED 0
#endif

// ---- DoFDown: colour = the mean of 4 bilinear taps of the ToneMap image (each at a texel corner: 16 texels), CoC = the max of 16 depth taps of
// sat(DoFEqWorld.x d + DoFEqWorld.y). s0 the ToneMap image (linear), s1 the depth map (linear); c0.xy = DoFEqWorld. v0, v1 the first row's colour taps, v2..v5 the
// first row's depth taps, v6 = (0, 1 / height) (the step to the next row).
struct RvkPsInDofDown {
    float4 rvk_fragCoord : SV_Position;
    [[vk::location(0)]] float4 v0 : VARY0;
    [[vk::location(1)]] float4 v1 : VARY1;
    [[vk::location(2)]] float4 v2 : VARY2;
    [[vk::location(3)]] float4 v3 : VARY3;
    [[vk::location(4)]] float4 v4 : VARY4;
    [[vk::location(5)]] float4 v5 : VARY5;
    [[vk::location(6)]] float4 v6 : VARY6;
};
struct RvkPsOutPost {
    float4 oC0 : SV_Target0;
};

RvkPsOutPost rvkDofDown(RvkPsInDofDown i) {
    const RvkDrawData d = rvkLoadDraw();
#if RVK_POST_FUSED
    const RvkPostTm tm = rvkPostTmLoad(d);
#define RVK_DOF_COLOUR(uv) rvkPostTmCorner(d, tm, uv)
#else
#define RVK_DOF_COLOUR(uv) rvkPostTap(d, 0u, uv)
#endif
    const float2 row = i.v6.xy;
    float3 c = RVK_DOF_COLOUR(i.v0.xy).xyz + RVK_DOF_COLOUR(i.v1.xy).xyz;
    c = c + RVK_DOF_COLOUR(row * 2.0 + i.v0.xy).xyz;
    c = c + RVK_DOF_COLOUR(row * 2.0 + i.v1.xy).xyz;
#undef RVK_DOF_COLOUR
    const float4 eq = RVK_PSC(0);
    float coc = 0.0;
    [unroll] for (int r = 0; r < 4; ++r) {
        const float2 o = row * float(r);
        const float4 dd = float4(rvkPostTap(d, 1u, o + i.v2.xy).x, rvkPostTap(d, 1u, o + i.v3.xy).x, rvkPostTap(d, 1u, o + i.v4.xy).x, rvkPostTap(d, 1u, o + i.v5.xy).x);
        const float4 cc = saturate(eq.x * dd + eq.y);
        coc = max(coc, max(max(cc.x, cc.y), max(cc.z, cc.w)));
    }
    RvkPsOutPost o;
    o.oC0 = float4(c * 0.25, coc);
    return o;
}

// ---- DoF: the in-focus colour src (v0), a 5-tap cross of it ("small"), SmallBlur ("med", s1) and BlurV ("large", s2) at v5, weighted by the circle of confusion;
// plus the bloom (s3) x BloomIntensity through the colour matrix. s4 the depth map; c0..c2 the colour matrix, c3.xy DoFEqFar, c4.x BloomIntensity, c5.x
// DoFNearFarRatio. v0 = the centre texel, v1..v4 the cross taps (texel corners), v5 the 400x225 images' uv.
struct RvkPsInDof {
    float4 rvk_fragCoord : SV_Position;
    [[vk::location(0)]] float4 v0 : VARY0;
    [[vk::location(1)]] float4 v1 : VARY1;
    [[vk::location(2)]] float4 v2 : VARY2;
    [[vk::location(3)]] float4 v3 : VARY3;
    [[vk::location(4)]] float4 v4 : VARY4;
    [[vk::location(5)]] float4 v5 : VARY5;
};

RvkPsOutPost rvkDof(RvkPsInDof i) {
    const RvkDrawData d = rvkLoadDraw();
#if RVK_POST_FUSED
    const RvkPostTm tm = rvkPostTmLoad(d);
    const float4 src = rvkPostTmTexel(d, tm, i.v0.xy);
#define RVK_DOF_CROSS(uv) rvkPostTmCorner(d, tm, uv)
#else
    const float4 src = rvkPostTap(d, 0u, i.v0.xy);
#define RVK_DOF_CROSS(uv) rvkPostTap(d, 0u, uv)
#endif
    float4 small = RVK_DOF_CROSS(i.v1.xy) + src;
    small = small + RVK_DOF_CROSS(i.v2.xy);
    small = small + RVK_DOF_CROSS(i.v3.xy);
    small = small + RVK_DOF_CROSS(i.v4.xy);
#undef RVK_DOF_CROSS
    small = small * 0.200000003;
    const float depth = rvkPostTap(d, 4u, i.v0.xy).x;
    const float4 c3 = RVK_PSC(3);
    const float farCoc = saturate(c3.x * depth + c3.y) * RVK_PSC(5).x;
    const float4 med = rvkPostTap(d, 1u, i.v5.xy);
    const float coc = (depth + -1.0) >= 0.0 ? med.w : max(med.w, farCoc);
    const float4 w = saturate(coc * float4(-3.00300312, -3.00300312, -3.00300312, 3.00300312) + float4(1.0, 2.00300312, 3.00300312, -2.00300312));
    const float2 wm = min(1.0 - w.xy, w.yz);
    float4 r = small * wm.x;
    r = w.x * src + r;
    r = wm.y * med + r;
    const float4 large = rvkPostTap(d, 2u, i.v5.xy);
    r = w.w * large + r;
    const float3 bloom = rvkPostTap(d, 3u, i.v5.xy).xyz * RVK_PSC(4).x;
    const float3 b = float3(dot(bloom, RVK_PSC(0).xyz), dot(bloom, RVK_PSC(1).xyz), dot(bloom, RVK_PSC(2).xyz));
    RvkPsOutPost o;
    o.oC0 = float4(r.xyz + b, r.w);
    return o;
}
