// Helpers for hand-written shaders: include after rvk_abi.hlsli (the port of rvk_common.glsl).
#pragma once

#include "rvk_abi.hlsli"

// bindless sampling
float4 rvkSample2D(uint tex, uint smp, float2 uv) { return RVK_TEX2D(tex).Sample(RVK_SAMP(smp), uv); }
float4 rvkSample2DLod(uint tex, uint smp, float2 uv, float lod) { return RVK_TEX2D(tex).SampleLevel(RVK_SAMP(smp), uv, lod); }
float4 rvkFetch2D(uint tex, int2 p) { return RVK_TEX2D(tex).Load(int3(p, 0)); }
// CAUTION (Vulkan, AMD RX 7900 XTX, measured 2026-11-11 by rvk_rhitest "opaque-first traces"): with an index that differs between the lanes of
// a wave the size is the FIRST lane's texture's for every lane (OpImageQuerySizeLod; DXC decorates index, access chain and load NonUniform; a
// WaveReadLaneFirst waterfall does not help either); D3D12 is right. Only for an index uniform across the wave; the RT scene's records carry
// their textures' sizes instead (rvkRtTexArea, rvkRtPackLodOffset; rt-reflections.md 3.3.1 / 3.3.2: the audit of every size query).
int2 rvkSize2D(uint tex) {
    uint w, h;
    RVK_TEX2D(tex).GetDimensions(w, h);
    return int2(w, h);
}

// GLSL's unpackUnorm4x8: byte 0 is x
float4 rvkUnpackUnorm4x8(uint w) { return float4(w & 0xFFu, (w >> 8) & 0xFFu, (w >> 16) & 0xFFu, w >> 24) / 255.0; }

// Texture coordinates (0,0 = top left, D3D9 / Vulkan image order) -> D3D NDC (y up)
float2 rvkUvToNdc(float2 uv) { return float2(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0); }

// This frame's camera jitter as an NDC offset (src/enh/upscale: the main view's geometry is shifted by it in clip space, the
// FrameBlock's matrices stay the unjittered camera's; zero without jitter). The surface rasterised at a pixel whose centre is
// at NDC c has the unjittered NDC c - J: every reconstruction of a position from the main view's depth (depth map / hardware
// depth) takes `rvkUvToNdc(uv) - rvkJitterNdc(f)`.
float2 rvkJitterNdc(RvkFrame f) { return float2(2.0 * f.jitter.x, -2.0 * f.jitter.y) * f.viewport.zw; }

// The main view's hardware depth (docs/research/reverse-z.md). With RVK_FRAME_REVERSE_Z the buffer holds near / view z (1 at the
// near plane, 0 at infinity and for the sky); the engine's projection wrote D3D depth z = f / (f - n) (1 - n / viewZ). This
// returns the D3D depth (0 near .. 1 far, the sky and everything beyond the far plane 1), so code written for the engine's
// convention keeps working; without the flag it is the identity.
float rvkHwDepthToD3D(RvkFrame f, float d) {
    if ((f.frameInfo.y & RVK_FRAME_REVERSE_Z) == 0u) return d;
    float k = 1.0 - f.depthParams.x / max(f.depthParams.y, 1e-3);
    return min((1.0 - d) / max(k, 1e-6), 1.0);
}

// View depth (distance along the view axis) of a hardware depth value; 0 for the sky / infinity
float rvkHwDepthToViewZ(RvkFrame f, float d) {
    if ((f.frameInfo.y & RVK_FRAME_REVERSE_Z) != 0u) return d > 0.0 ? f.depthParams.x / d : 0.0;
    float a = f.proj[2][2], b = f.proj[3][2];
    return (d < 1.0 && abs(d - a) > 1e-9) ? max(b / (d - a), 0.0) : 0.0;
}

// The engine's linear depth map (R32F, ge_default_ps.fx: view depth / far, cleared to 1) -> view depth
float rvkViewDepth(RvkFrame f, float depthMapValue) { return depthMapValue * f.cameraPos.w; }

// World position of a pixel from its view depth: the point of the pixel's ray at that depth (the far-plane point has the
// projection's far as its view depth; the view depth grows linearly along a ray from the camera)
float3 rvkWorldFromViewDepth(RvkFrame f, float2 uv, float viewDepth) {
    float4 farPoint = mul(float4(rvkUvToNdc(uv) - rvkJitterNdc(f), 1.0, 1.0), f.invViewProj);
    float3 p = farPoint.xyz / farPoint.w;
    float projFar = max(f.depthParams.y, 1e-3);
    return f.cameraPos.xyz + (p - f.cameraPos.xyz) * (viewDepth / projFar);
}
