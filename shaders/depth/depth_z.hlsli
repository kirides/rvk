// S-6 (docs/research/frame-ownership.md 14.10, setting native.depth_z): the engine's linear depth map (R32F, view depth / engine far, cleared to 1) derived from the
// main view's hardware depth. Push block of the derive pass (depth_z.frag) and of the compare reduction (depth_z_compare.comp). Mirror: enh::DepthZPush.
#pragma once

#include "rvk_common.hlsli"

struct DepthZPush {
    RVK_PASS_PUSH_HEADER
    uint depthTex;  // the main view's hardware depth (bindless t2D)
    uint mapTex;    // compare: the engine's linear depth map (bindless t2D)
    uint width;
    uint height;
    uint2 stats;    // compare: the stats buffer (written: shaderRefRw pair)
    uint2 pad;
};
RVK_PUSH(DepthZPush, pc);

// What ge_default_ps.fx's z pass writes (viewPos.z = the vertex shader's view depth divided by the engine's far), from the hardware depth of the same triangles. The sky /
// the cleared buffer (view depth 0 = infinity) is the map's clear value 1. Geometry beyond the engine's far plane gives values above 1, as the z pass wrote them under reverse-z.
float rvkDepthZMap(RvkFrame f, float hw) {
    float z = rvkHwDepthToViewZ(f, hw);
    return z > 0.0 ? z * f.depthParams.w : 1.0;
}

// the stats buffer's words (uints; the float is non-negative, so its bit pattern orders like the value)
static const uint DZ_OVER_1E6 = 0u;   // pixels whose relative error exceeds 1e-6
static const uint DZ_OVER_1E5 = 1u;   // ... 1e-5
static const uint DZ_OVER_1E4 = 2u;   // ... 1e-4
static const uint DZ_OVER_1E3 = 3u;   // ... 1e-3
static const uint DZ_OVER_4E3 = 4u;   // ... 4e-3 (the picture check's depth tolerance)
static const uint DZ_MAX_REL = 5u;    // the largest relative error (float bits)
static const uint DZ_SKY_DIFF = 6u;   // one side is the sky / clear value (>= 0.9999), the other not
static const uint DZ_GEOMETRY = 7u;   // pixels with geometry in the engine's map
static const uint DZ_NEAR_MAX = 8u;   // the largest relative error among pixels whose map value is below 0.02 (float bits)
static const uint DZ_WORDS = 9u;
