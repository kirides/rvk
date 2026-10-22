// The RT scene's debug view (src/enh/rt_debug.cpp, setting rt.debug): push block shared by its compute pass and its
// composite. Mirror: enh::RtDebugPush (static_assert'ed size; offsets checked by rvk_tests abi_glsl_pass_push_matches_cpp).

#define RVK_PASS_SHADER 1
#include "rvk_abi.glsl"
#include "rvk_common.glsl"
#include "rvk_rt.glsl"

layout(push_constant, std430) uniform RtDebugPush {
    RVK_PASS_PUSH_HEADER
    uint depthTex;    // bindless t2D slot of the engine's depth map (0 = none)
    uint width;       // output size (the back buffer)
    uint height;
    uint srcTex;      // composite: bindless t2D slot of the transient colour image
    uint srcSampler;  // composite: bindless sampler index
    uint mode;        // RT_DEBUG_* | RT_DEBUG_LINEAR_DEPTH
    uvec2 probes;     // device address of the probe results (RtDebugProbes; 0 = none)
} pc;

// the probe pixels' results (enh::RtDebugProbe): a 5 x 5 grid over the picture
const uint RT_DEBUG_PROBES = 25u;
struct RtDebugProbe {
    uint record;   // record index + 1, 0 = miss
    float t;
    float rasterT;
    uint code;     // 1 match, 2 traced farther, 3 traced closer, 4 missed (raster has geometry), 5 hit (raster has none)
};
layout(buffer_reference, std430, buffer_reference_align = 16) writeonly buffer RtDebugProbes {
    RtDebugProbe p[];
};

const uint RT_DEBUG_INSTANCES = 1u;
const uint RT_DEBUG_DEPTH = 2u;
const uint RT_DEBUG_NORMALS = 3u;
const uint RT_DEBUG_ALBEDO = 4u;
const uint RT_DEBUG_UV = 5u;
const uint RT_DEBUG_LINEAR_DEPTH = 0x100u;  // depthTex holds view depth (the hardware depth capture), not view depth / far
const uint RT_DEBUG_OPAQUE = 0x200u;        // alpha-tested geometry traced as solid (RVK_RT_TRACE_OPAQUE)
const uint RT_DEBUG_SPLIT = 0x400u;         // albedo / uv: only the right half of the picture (the raster on the left)
