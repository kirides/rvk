// The owned picture check (src/enh/picture_check.cpp, setting native.picture_check; docs/research/frame-ownership.md 7.47): push block of the compute pass
// that stores a composed frame's opaque scene as a reference or compares an owned frame's with it. Mirror: enh::PictureCheckPush.
#pragma once

#include "rvk_common.hlsli"

struct PictureCheckPush {
    RVK_PASS_PUSH_HEADER
    uint colorTex;     // bindless t2D slot of the scene HDR target (the opaque scene at AfterOpaque)
    uint depthTex;     // bindless t2D slot of the engine's linear depth map (R32F)
    uint width;
    uint height;
    uint mode;         // 0 = store the reference, 1 = compare with it
    float colorTol;    // a pixel differs in colour when max |a - b| / (0.05 + max(a, b)) exceeds this
    float depthTol;    // ... in depth when |a - b| / max(min(a, b), 1e-4) exceeds this
    uint pad;
    uint2 stats;       // the stats buffer (written: shaderRefRw pair)
    uint2 pad2;
};
RVK_PUSH(PictureCheckPush, pc);

// the stats buffer's words (uints; the floats are non-negative, so their bit patterns order like the values)
static const uint PC_COLOR_DIFF = 0u;   // pixels whose colour differs
static const uint PC_DEPTH_DIFF = 1u;   // pixels whose depth differs
static const uint PC_ANY_DIFF = 2u;     // either
static const uint PC_MAX_COLOR = 3u;    // max colour difference (float bits)
static const uint PC_MAX_DEPTH = 4u;    // max depth difference (float bits)
static const uint PC_MAX_X = 5u;        // bounding box of the differing pixels: max x, max y, 0xFFFF - min x, 0xFFFF - min y
static const uint PC_MAX_Y = 6u;
static const uint PC_NMIN_X = 7u;
static const uint PC_NMIN_Y = 8u;
static const uint PC_WORDS = 9u;
