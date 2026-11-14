// Sample distribution shadow maps (docs/research/shadows.md 8.9, src/enh/sdsm.*): the push block of the depth reduction. Mirror: enh::SdsmPush.
#pragma once

#include "rvk_common.hlsli"

struct SdsmPush {
    RVK_PASS_PUSH_HEADER
    uint depthTex;  // the main view's hardware depth (bindless t2D)
    uint width;     // the scene target's size (the depth's valid part)
    uint height;
    uint stride;    // every stride-th pixel in x and y is sampled
    uint2 bins;     // the result buffer (written: shaderRefRw pair), SDSM_BINS x 4 words, cleared to 0 before the pass
    uint2 pad;
    float4 range;   // x: view depth of bin 0's lower edge, y: bins / ln(zMax / zMin), z: zMax (pixels beyond are left out), w: unused
    float4 lx;      // light-space x of a world point = dot(float4(world, 1), lx)
    float4 ly;
};
RVK_PUSH(SdsmPush, pc);

static const uint SDSM_BINS = 128;
