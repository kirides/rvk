// S-7c (docs/research/frame-ownership.md 14.16): the post chain's draw units on our own pixel shaders. Each unit keeps its draw (the engine's vertex shader, the
// quad, the target, the blend / clip state, the constants c#, the textures s#: native::OwnPost copies the engine pixel shader's reflection) and swaps only the
// pixel shader for a port of post_hdr.fx / post_dof.fx / overlay_color.fx written against this ABI. The ports follow the D3D9 listings (rvk_fxtool dump --listing)
// instruction for instruction, so that `native.own_post=compare` measures them EQUAL to the translated shaders (the _pp hints are full precision there too).
// The fused variants (RVK_POST_TM_* below) also tone-map the HDR scene themselves: the ToneMap unit's 8-bit image is not drawn then.
#pragma once

#include "rvk_draw.hlsli"

// a bilinear / point tap of the draw's sampler stage s (D3D9 s#: the bindless texture and the sampler of the stage's states)
float4 rvkPostTap(RvkDrawData d, uint s, float2 uv) { return RVK_TEX2D_UNI(rvkDrawTex(d, s)).Sample(RVK_SAMP_UNI(rvkDrawSmp(d, s)), uv); }

static const float3 kRvkPostLumW = float3(0.300000012, 0.589999974, 0.109999999);

// post_hdr.fx PS_ToneMap's colour (shader #13 of the listing): k = ToneMapFactor / avgLum, L = dot(c, (0.3, 0.59, 0.11)), the blue shift lerp(L (0.8, 0.8, 1.4), c,
// sat(4 L)), times the compressed luminance kL (1 + kL / WhitePoint) / (1 + kL). `k` and `invWhite` are per draw; the colour matrix is applied by the caller.
float3 rvkPostToneMapColour(float3 c, float k, float invWhite) {
    const float L = dot(c, kRvkPostLumW);
    const float rL = k * L;
    const float d = 1.0 / (k * L + 1.0);
    const float q = rL * invWhite + 1.0;
    const float comp = d * (rL * q);
    const float3 shifted = L * -float3(0.800000012, 0.800000012, 1.39999998) + c;
    const float s = saturate(4.0 * L);
    const float3 col = s * shifted + L * float3(0.800000012, 0.800000012, 1.39999998);
    return comp * col;
}

// ---- the fused variants: the ToneMap unit's work done inside its consumers --------------------------------------------------------------------------------
// The ToneMap unit's inputs travel in two free sampler stages and free pixel registers of the consumer's draw (native::OwnPost::fuse): s5 the HDR scene (point,
// clamp: the engine samples it with POINT), s6 the 1x1 adapted luminance; c16..c18 the ToneMap draw's colour matrix rows, c19 = (ToneMapFactor, 1 / WhitePoint),
// c20.xy = half a texel of the ToneMap image (= the scene). The result is what the ToneMap image would hold, saturated like its UNORM target, without its 8-bit step.
#define RVK_POST_TM_SCENE 5u
#define RVK_POST_TM_AVG 6u

struct RvkPostTm {
    float k, invWhite;
    float3 m0, m1, m2;
    float2 ht;  // half a texel
};
RvkPostTm rvkPostTmLoad(RvkDrawData d) {
    RvkPostTm t;
    const float avg = rvkPostTap(d, RVK_POST_TM_AVG, float2(0.5, 0.5)).x;
    const float4 c19 = RVK_PSC(19);
    t.k = c19.x * (1.0 / avg);
    t.invWhite = c19.y;
    t.m0 = RVK_PSC(16).xyz;
    t.m1 = RVK_PSC(17).xyz;
    t.m2 = RVK_PSC(18).xyz;
    t.ht = RVK_PSC(20).xy;
    return t;
}
// one texel of the ToneMap image (the scene's texel at uv, point sampled)
float4 rvkPostTmTexel(RvkDrawData d, RvkPostTm t, float2 uv) {
    const float4 c = rvkPostTap(d, RVK_POST_TM_SCENE, uv);
    const float3 col = rvkPostToneMapColour(c.xyz, t.k, t.invWhite);
    return saturate(float4(dot(col, t.m0), dot(col, t.m1), dot(col, t.m2), c.w));
}
// a bilinear tap of the ToneMap image at a texel corner: the mean of the four texels around it
float4 rvkPostTmCorner(RvkDrawData d, RvkPostTm t, float2 uv) {
    const float4 a = rvkPostTmTexel(d, t, uv + float2(-t.ht.x, -t.ht.y));
    const float4 b = rvkPostTmTexel(d, t, uv + float2(t.ht.x, -t.ht.y));
    const float4 c = rvkPostTmTexel(d, t, uv + float2(-t.ht.x, t.ht.y));
    const float4 e = rvkPostTmTexel(d, t, uv + float2(t.ht.x, t.ht.y));
    return 0.25 * ((a + b) + (c + e));
}
