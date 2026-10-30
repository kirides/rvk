// Shared by the G-buffer passes (gtao.comp, gtao_filter.comp, gbuffer_debug.frag; src/enh/gbuffer.cpp, docs/research/rtgi.md
// 10): the push block (mirror: enh::GbufferPush) and the depth helpers.
#pragma once

#include "rvk_common.hlsli"
#include "rvk_oct.hlsli"  // rvkGiOctEncode / rvkGiOctDecode (the G-buffer normal)

struct GbufferPush {
    RVK_PASS_PUSH_HEADER
    uint depthTex;       // the main view's hardware depth (t2D)
    uint normalTex;      // G-buffer normal (A2B10G10R10: xy octahedral world normal, w id / 3: 0 = no material)
    uint motionTex;      // G-buffer motion (xy previous uv - uv)
    uint aoTex;          // raw AO (filter) / the AO history (debug)
    uint historyTex;     // the AO history of the previous frame (filter; 0 = none)
    uint linearSampler;  // linear clamp
    uint width;          // the scene target's size
    uint height;
    float radius;        // AO radius (world units)
    float falloff;       // fraction of the radius over which a sample's weight fades
    float strength;
    float blend;         // temporal: weight of the history
    uint frameIndex;     // the frame counter (noise)
    uint flags;
    uint mode;           // debug view: 1 normals, 2 motion, 3 AO
    uint pad;
};
RVK_PUSH(GbufferPush, pc);

static const float GB_PI = 3.14159265;
static const float GB_HALF_PI = 1.57079633;

// view-space position (D3D: x right, y up, z forward = view depth) of a pixel from the hardware depth (the engine's row-vector
// projection: z' = (z a + b) / z, x' = x P00 / z, y' = y P11 / z)
float3 gbViewPos(RvkFrame f, float2 uv, float z) {
    float a = f.proj[2][2], b = f.proj[3][2];
    float vz = abs(z - a) > 1e-9 ? b / (z - a) : 1e9;
    float2 ndc = rvkUvToNdc(uv) - rvkJitterNdc(f);
    return float3(ndc.x * vz / f.proj[0][0], ndc.y * vz / f.proj[1][1], vz);
}

// world position of a pixel from the hardware depth (the far plane's point for the sky)
float3 gbWorldPos(RvkFrame f, float2 uv, float z) {
    float4 w = mul(float4(rvkUvToNdc(uv) - rvkJitterNdc(f), min(z, 1.0), 1.0), f.invViewProj);
    return w.xyz / w.w;
}

// previous uv - this uv of a world point (the camera's motion)
float2 gbCameraMotion(RvkFrame f, float3 P) {
    float4 c = mul(float4(P, 1.0), f.viewProj);
    float4 p = mul(float4(P, 1.0), f.prevViewProj);
    if (c.w <= 1e-6 || p.w <= 1e-6) return float2(0.0, 0.0);
    float2 cuv = c.xy / c.w * float2(0.5, -0.5) + 0.5;
    float2 puv = p.xy / p.w * float2(0.5, -0.5) + 0.5;
    return puv - cuv;
}
