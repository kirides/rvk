#pragma once

// Native sun shadow cascades (docs/research/native-renderer.md N4, setting `native.shadows`, default off): the renderer's own sun shadow
// maps, drawn from the scene mirror (every cached-in static mesh part within range, also the ones off screen, alpha-tested leaf cards
// included) instead of the engine's cascade draws. Three sphere-fitted, texel-snapped cascades along the sun's direction, depth-only passes
// at AfterDepthPrepass into transient D32 images; the native opaque pass samples them (NativeShadowBlock) in place of the engine's shadow
// mask. Characters, trees and grass are not casters yet (they are not in the native parts), so the engine's cascades stay for them.

#include <cstdint>
#include <vector>

#include "d3d9/device.h"
#include "enh/native_parts.h"
#include "vk/passes.h"
#include "vk/vk.h"

namespace rvk::enh {

// mirror of shaders/native/static_mesh.glsl NativeShadow (std430, 272 bytes)
struct NativeShadowBlock {
    float viewProj[3][16]{};      // world -> cascade clip (GLSL column order)
    float split[4]{};             // far end of cascades 0..2 as view depth (world units); w = 1 when the block is valid
    std::uint32_t maps[4]{};      // bindless t2D slots of the cascade depth maps; w = the point / clamp sampler
    float params[4]{};            // x 1 / map size, y depth bias (texels of world size), z normal offset (texels), w fade fraction of the last cascade
    float texel[4]{};             // world units per texel of each cascade
    float range[4]{};             // depth range (world units) of each cascade
};
static_assert(sizeof(NativeShadowBlock) == 272);

class NativeShadows {
public:
    NativeShadows(d3d9::Device& device, NativeParts& parts);
    ~NativeShadows();
    NativeShadows(const NativeShadows&) = delete;
    NativeShadows& operator=(const NativeShadows&) = delete;

    void apply(bool enabled);
    bool enabled() const { return enabled_; }

    // the block of THIS frame in the frame's ring (0: no cascades this frame: sampling must fall back to the engine's mask)
    VkDeviceAddress block() const { return blockFrame_ == dev_.frameValue() ? block_ : 0; }

private:
    // parts with the same geometry and material are one instanced draw; the groups are cached while the mirror, the resolved parts and the camera stay
    // (rebuilt when any changes, at the latest every kRebuildFrames)
    struct Inst {
        float rows[3][4]{};
        float sphere[4]{};
        const void* entity = nullptr;  // its mirror entity (the rows follow it when it moves)
    };
    struct Group {
        NativePart part;
        std::vector<Inst> instances;
    };
    struct Cascade {
        double center[3]{};
        double radius = 0.0;
        double zRange = 0.0;
        float viewProj[16]{};  // column-major
    };
    void pass(d3d9::PassContext& ctx);
    bool ensurePipeline();
    void destroyPipeline();

    d3d9::Device& dev_;
    NativeParts& parts_;
    bool enabled_ = false;
    bool failed_ = false;
    std::uint32_t passId_ = 0;
    vk::PassLayout layout_;
    VkPipeline pipeline_ = VK_NULL_HANDLE;
    VkPipeline pipelineMasked_ = VK_NULL_HANDLE;
    std::vector<Group> groups_;
    std::uint64_t builtStructure_ = 0, builtTransform_ = 0, builtParts_ = 0, builtFrame_ = 0, refreshedFrame_ = 0;
    double builtCamera_[3]{};
    void rebuildGroups(const double camPos[3], double reach);
    void refreshTransforms();
    double rebuildMs_ = 0.0, refreshMs_ = 0.0;
    std::uint64_t refreshes_ = 0;
    std::uint64_t casterCount_ = 0, rebuilds_ = 0;
    VkDeviceAddress block_ = 0;
    std::uint64_t blockFrame_ = ~0ull;

    struct Stats {
        std::uint64_t frames = 0, casters = 0, draws = 0, instances = 0, groups = 0, skippedNoSun = 0, skippedNoCamera = 0, rebuilds = 0;
        double ms = 0.0;
    } stats_;
    double lastReport_ = 0.0;
};

}  // namespace rvk::enh
