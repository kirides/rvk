// Shared by the ray-traced sun shadow passes (rt_shadow_trace.comp, rt_shadow_temporal.comp, rt_shadow_spatial.comp;
// src/enh/rt_shadows.cpp, docs/research/rt-shadows.md): the params block (mirror: enh::RtShadowParams) and the depth helpers.
//
// The passes run after the z pre-pass on the engine's linear depth map (R32F, view depth / engine far, cleared to 1) and
// reconstruct positions exactly as the enhanced shadow mask does (shadow_mask.frag `reconstruct`): the pixel x's ray is at
// NDC 2 x / size - 1 (the engine's D3D9 convention), minus this frame's camera jitter.
#pragma once

#include "rvk_common.hlsli"

struct RtShadowParams {
    uint4 tex;    // x depth map (t2D), y raw visibility (t2D), z history (t2D), w accumulated (t2D)
    uint4 info;   // x width, y height, z rays per pixel, w RVK_RT_TRACE_* flags
    uint4 mode;   // x frame index, y history valid (0 / 1), z debug, w 0
    float4 sun;   // xyz unit vector towards the sun, w tan of the sun cone's half angle
    float4 range; // x range (view depth, world units), y ray reach, z origin bias (units), w spatial filter strength
};

struct RtShadowPush {
    RVK_PASS_PUSH_HEADER
    uint2 params;  // RtShadowParams address
    uint2 pad;
};
RVK_PUSH(RtShadowPush, pc);

static const float RTS_SKY = 0.999;  // depth map values from here on are the cleared background

RtShadowParams rtsParams() { return rvkPtr<RtShadowParams, 16>(pc.params).Get(); }

int2 rtsClamp(RtShadowParams p, int2 q) { return clamp(q, int2(0, 0), int2(p.info.xy) - 1); }

float rtsDepthMap(RtShadowParams p, int2 q) { return rvkFetch2D(p.tex.x, rtsClamp(p, q)).r; }

float3 rtsPos(RvkFrame f, RtShadowParams p, int2 q, float depthMapValue) {
    return rvkWorldFromViewDepth(f, float2(q) / float2(p.info.xy), depthMapValue * f.cameraPos.w);
}

// the surface normal of the pixel from the depth map: per axis the neighbour nearer in depth (no smearing across edges),
// turned towards the camera
float3 rtsNormal(RvkFrame f, RtShadowParams p, int2 q, float d, float3 P) {
    float dl = rtsDepthMap(p, q + int2(-1, 0)), dr = rtsDepthMap(p, q + int2(1, 0));
    float du = rtsDepthMap(p, q + int2(0, -1)), dd = rtsDepthMap(p, q + int2(0, 1));
    float3 dx = abs(dr - d) < abs(d - dl) ? rtsPos(f, p, q + int2(1, 0), dr) - P : P - rtsPos(f, p, q + int2(-1, 0), dl);
    float3 dy = abs(dd - d) < abs(d - du) ? rtsPos(f, p, q + int2(0, 1), dd) - P : P - rtsPos(f, p, q + int2(0, -1), du);
    float3 n = cross(dy, dx);
    float3 V = f.cameraPos.xyz - P;
    n = dot(n, n) > 1e-12 ? normalize(n) : normalize(V);
    return dot(n, V) < 0.0 ? -n : n;
}

// interleaved gradient noise (Jimenez 2014), shifted per frame
float rtsIgn(float2 p, uint frame) {
    p += 5.588238 * float(frame & 63u);
    return frac(52.9829189 * frac(0.06711056 * p.x + 0.00583715 * p.y));
}

// last frame's accumulated result at the point P of pixel q: the history reprojected with the camera's motion (its jitter added
// back: the history is stored per rasterised pixel), 4 depth-checked bilinear taps. x mean visibility, y frames accumulated, z
// second moment, w the taps' weight sum (< 0.05 = no usable history: a new pixel, disoccluded, off screen)
float4 rtsHistory(RvkFrame f, RtShadowParams p, float3 P) {
    if (p.mode.y == 0u || (f.frameInfo.y & RVK_FRAME_PREV_VALID) == 0u) return float4(0.0, 0.0, 0.0, 0.0);
    float4 pc4 = mul(float4(P, 1.0), f.prevViewProj);
    if (pc4.w <= 1e-3) return float4(0.0, 0.0, 0.0, 0.0);
    float2 pndc = pc4.xy / pc4.w + float2(2.0 * f.jitter.z, -2.0 * f.jitter.w) * f.viewport.zw;
    float2 pp = float2(pndc.x * 0.5 + 0.5, 0.5 - pndc.y * 0.5) * float2(p.info.xy);
    int2 b = int2(floor(pp));
    float2 fr = pp - float2(b);
    float wsum = 0.0, vsum = 0.0, nsum = 0.0, msum = 0.0;
    for (int k = 0; k < 4; ++k) {
        int2 t = b + int2(k & 1, k >> 1);
        if (any(t < int2(0, 0)) || any(t >= int2(p.info.xy))) continue;
        float4 h = rvkFetch2D(p.tex.z, t);
        float w = (((k & 1) == 0) ? 1.0 - fr.x : fr.x) * (((k >> 1) == 0) ? 1.0 - fr.y : fr.y);
        if (abs(h.y - pc4.w) > 0.02 * pc4.w + 3.0 || h.z <= 0.0) w = 0.0;
        wsum += w;
        vsum += w * h.x;
        nsum += w * h.z;
        msum += w * h.w;
    }
    if (wsum <= 0.05) return float4(0.0, 0.0, 0.0, wsum);
    return float4(vsum / wsum, nsum / wsum, msum / wsum, wsum);
}
