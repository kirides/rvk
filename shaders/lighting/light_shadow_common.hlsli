// Shared by the ray-traced point-light shadow passes (light_shadow_trace.comp.hlsl, light_shadow_filter.comp.hlsl; src/enh/light_table.cpp,
// docs/research/gpu-scene.md 9.10). The passes run at AfterDepthPrepass after the cluster build, on the engine's linear depth map (R32F, view depth /
// engine far, cleared to 1), and write an image of RVK_LS_ENTRIES floats per pixel: the visibility of the (up to four) shadow-casting table lights that
// matter most at the pixel. An entry is `shadow id + q / 64` (id 1..31, q 0..63 = the visibility in 1 / 63 steps; 0 = empty): exact in a half float,
// so the image is RGBA16F. The material variants (RVK_MV_CLUSTER) look up the shadow id of each table light they put into a register and scale its colour.
#pragma once

#include "rvk_common.hlsli"
#include "rvk_light_model.hlsli"

struct LightShadowPush {
    RVK_PASS_PUSH_HEADER
    uint4 tex;     // x depth map (t2D), y raw entries (t2D, filter pass), z width, w height
    float4 cfg;    // x light radius (units: the soft shadow's disc), y ray reach (units), z origin bias (units), w rays per light
    uint4 misc;    // x frame index, y RVK_RT_TRACE_* flags, z debug, w 0
    uint2 table;   // the light table device buffer's read reference (LAST: a ConstantBuffer packs a 4-component member to a 16-byte boundary, so a uint2 before
                   // the uint4s moved them by 8 bytes on D3D12: tex.x read the width as a heap index = GBV "uninitialized descriptor heap index 1600")
};
RVK_PUSH(LightShadowPush, pc);

static const float LS_SKY = 0.999;  // depth map values from here on are the cleared background
// A blocked lamp still lights its surroundings by bounce (the lit table top, walls, the room): a surface the table's plane shades from a candle ON the table
// (the whole rug: the shadow of the top reaches 2.5 m) is no black hole. The traced visibility is lerped this far towards 1; 0 = hard physical shade.
// (gpu-scene.md 9.13: pub_interior's rugs went near-black with the light shadows on, every ray to the candle hit the table top.)
static const float LS_FILL = 0.4;

float lsDepth(int2 q) { return rvkFetch2D(pc.tex.x, clamp(q, int2(0, 0), int2(pc.tex.zw) - 1)).r; }

float3 lsPos(RvkFrame f, int2 q, float d) {
    return rvkWorldFromViewDepth(f, float2(q) / float2(pc.tex.zw), d * f.cameraPos.w);
}

// the surface normal from the depth map: per axis the neighbour nearer in depth, turned towards the camera
float3 lsNormal(RvkFrame f, int2 q, float d, float3 P) {
    float dl = lsDepth(q + int2(-1, 0)), dr = lsDepth(q + int2(1, 0));
    float du = lsDepth(q + int2(0, -1)), dd = lsDepth(q + int2(0, 1));
    float3 dx = abs(dr - d) < abs(d - dl) ? lsPos(f, q + int2(1, 0), dr) - P : P - lsPos(f, q + int2(-1, 0), dl);
    float3 dy = abs(dd - d) < abs(d - du) ? lsPos(f, q + int2(0, 1), dd) - P : P - lsPos(f, q + int2(0, -1), du);
    float3 n = cross(dy, dx);
    float3 V = f.cameraPos.xyz - P;
    n = dot(n, n) > 1e-12 ? normalize(n) : normalize(V);
    return dot(n, V) < 0.0 ? -n : n;
}

float lsIgn(float2 p, uint frame) {
    p += 5.588238 * float(frame & 63u);
    return frac(52.9829189 * frac(0.06711056 * p.x + 0.00583715 * p.y));
}

float lsEncode(uint id, float vis) { return float(id) + floor(saturate(vis) * 63.0 + 0.5) / 64.0; }
uint lsId(float e) { return uint(floor(e + 0.0001)); }
float lsVis(float e) { return floor(frac(e + 0.0001) * 64.0) / 63.0; }
