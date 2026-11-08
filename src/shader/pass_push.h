#pragma once

// Push blocks of passes that used to declare their C++ struct locally (in the .cpp that records the pass). They live here so that
// src/shader/abi_blocks.cpp can describe them: every block shared between C++ and HLSL has one field description there, which the probe
// test (tools/rhitest/abi_probe.cpp, docs/research/hlsl-rhi.md "ABI probe") reads back on the GPU on both backends.
#include <cstdint>

#include "shader/abi.h"

namespace rvk::shader::abi {

// shaders/blit/blit.vert|frag BlitPush (the D3D12 backend's scaled copy; root constants)
struct BlitPush {
    std::uint32_t srcTex, smp, pad0, pad1;
    float uvRect[4];  // u0, v0, du, dv
};
static_assert(sizeof(BlitPush) == 32);

// shaders/upscale/prep.comp UpscalePush, shaders/framegen/prep.comp (the same layout)
struct DepthPrepPush {
    PassPushHeader h;
    std::uint32_t depthTex, width, height, pad;
};
static_assert(sizeof(DepthPrepPush) == 32 && sizeof(DepthPrepPush) <= kPushConstantBudget);

// shaders/upscale/exposure.comp ExposurePush
struct ExposurePush {
    PassPushHeader h;
    std::uint32_t lumTex;
    float factor, lumMin, lumMax;
};
static_assert(sizeof(ExposurePush) == 32 && sizeof(ExposurePush) <= kPushConstantBudget);

// shaders/framegen/debug.frag FramegenPush
struct FramegenDebugPush {
    PassPushHeader h;
    std::uint32_t srcTex, sampler0, mode, pad;
    float nearZ, farZ, pad1, pad2;
};
static_assert(sizeof(FramegenDebugPush) == 48 && sizeof(FramegenDebugPush) <= kPushConstantBudget);

// shaders/rt/rt_skin.comp SkinPush
struct SkinPush {
    PassPushHeader h;
    std::uint32_t table[2];
    std::uint32_t pad[2];
};
static_assert(sizeof(SkinPush) == 32);

// shaders/gpuscene/lighting_compose.comp LightingComposePush
struct LightingComposePush {
    std::uint32_t illums[2], sets[2], jobs[2], images[2];
    std::uint32_t count, pad[3];
};
static_assert(sizeof(LightingComposePush) == 48);

// shaders/gpuscene/owned_compact.comp OwnedCompactPush
struct OwnedCompactPush {
    std::uint32_t records[2], list[2], jobs[2], out[2];
    std::uint32_t countsBase, argsBase, slotCount, viewSlots;
    std::uint32_t clusters[2], views[2];
    std::uint32_t clusterRows, pad[3];
};
static_assert(sizeof(OwnedCompactPush) == 80);

}  // namespace rvk::shader::abi
