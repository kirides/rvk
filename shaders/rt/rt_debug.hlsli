// The RT scene's debug view (src/enh/rt_debug.cpp, setting rt.debug): push block shared by its compute pass and its
// composite. Mirror: enh::RtDebugPush (static_assert'ed size; offsets checked by rvk_tests).
#pragma once

#include "rvk_rt.hlsli"

struct RtDebugPush {
    RVK_PASS_PUSH_HEADER
    uint depthTex;    // bindless t2D slot of the engine's depth map (0 = none)
    uint width;       // output size (the back buffer)
    uint height;
    uint srcTex;      // composite: bindless t2D slot of the transient colour image
    uint srcSampler;  // composite: bindless sampler index
    uint mode;        // RT_DEBUG_* | RT_DEBUG_LINEAR_DEPTH
    uint2 probes;     // device address of the probe results (RtDebugProbes; 0 = none)
};
RVK_PUSH(RtDebugPush, pc);

// the probe pixels' results (enh::RtDebugProbe, 16 bytes: record, t, rasterT, code), a 5 x 5 grid over the picture
static const uint RT_DEBUG_PROBES = 25u;

static const uint RT_DEBUG_INSTANCES = 1u;
static const uint RT_DEBUG_DEPTH = 2u;
static const uint RT_DEBUG_NORMALS = 3u;
static const uint RT_DEBUG_ALBEDO = 4u;
static const uint RT_DEBUG_UV = 5u;
static const uint RT_DEBUG_ROUGHNESS = 6u;  // material pack (enh/material_pack.h)
static const uint RT_DEBUG_METALNESS = 7u;
static const uint RT_DEBUG_LINEAR_DEPTH = 0x100u;  // depthTex holds view depth (the hardware depth capture), not view depth / far
static const uint RT_DEBUG_OPAQUE = 0x200u;        // alpha-tested geometry traced as solid (RVK_RT_TRACE_OPAQUE)
static const uint RT_DEBUG_SPLIT = 0x400u;         // albedo / uv: only the right half of the picture (the raster on the left)
