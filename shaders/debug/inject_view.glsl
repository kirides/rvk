// The debug injection view (src/enh/debug_view.cpp, setting debug.inject_view): push block shared by its compute pass and
// its composite. Mirror: enh::DebugViewPush (static_assert'ed size; offsets checked by rvk_tests).

#define RVK_PASS_SHADER 1
#include "rvk_abi.glsl"
#include "rvk_common.glsl"

layout(push_constant, std430) uniform DebugViewPush {
    RVK_PASS_PUSH_HEADER
    uint depthTex;    // bindless t2D slot of the engine's depth map (0 = none)
    uint width;       // output size (the scene HDR target)
    uint height;
    uint srcTex;      // composite: bindless t2D slot of the transient factor image
    uint srcSampler;  // composite: bindless sampler index
    uint pad;
} pc;

// the view's settings come through the FrameBlock's feature parameters (abi.h kFrameParamDebugView):
// x mode (1 = depth: distance bands + world grid, 2 = tint: time-cycled colour), y strength 0..1, z grid spacing (world units)
const uint DEBUG_VIEW_PARAM = 0u;
const uint DEBUG_VIEW_DEPTH = 1u;
const uint DEBUG_VIEW_TINT = 2u;
