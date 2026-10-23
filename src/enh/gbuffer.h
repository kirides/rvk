#pragma once

// RTGI stage B "frame data" (M10 P5-P7, docs/research/rtgi.md 10): the thin G-buffer, motion vectors and GTAO.
//
// * P5 G-buffer: two extra colour attachments of the first main view's solid pass (d3d9::Device::setGbuffer): world normal
//   (normal-mapped: the operand the material shader evaluates its ambient SH with; else the interpolated normal) + id
//   (1 static material, 2 character) and motion, written by the material pixel shaders' Gbuffer variants
//   (enh/material_variant). The device arms the attachments once their pipelines are built (background).
// * P6 motion: xy = previous uv - this uv of the surface point: the camera's motion for every material pixel; characters
//   their own (the vertex capture's previous world positions, enh/rt_scene, while the RT scene is active); pixels without a
//   material draw (sky, decals only, ...) get the camera's motion from the hardware depth in the AO pass.
// * P7 GTAO (XeGTAO-style horizon search, 2 slices x 6 steps per side, MIT-licensed method; own implementation) at
//   AfterOpaque from the hardware depth + the G-buffer normal, a depth-aware 3x3 filter + temporal accumulation (motion
//   reprojection, neighbourhood clamp) into a history image; the material shaders reproject last frame's AO and scale the
//   ambient (and the GI) by it (FrameBlock::ao). ao.mode = replace (default): the game's SSAO (post_SSM.fx, the shadow
//   mask's red channel, which darkens the direct light too) is switched off by a draw replacer (its half-size SSAO / blur
//   draws write "unoccluded"); add: both act.
// * Debug view gbuffer.debug at BeforeGui: normals, motion or AO over the frame.
//
// On while AO or the debug view is on; off: nothing registered, the material shaders' plain (or GI) variants.

#include <cstdint>
#include <future>
#include <memory>

#include "d3d9/device.h"
#include "shader/abi.h"
#include "vk/passes.h"

namespace rvk::enh {

class RtScene;

// mirror of shaders/gbuffer/gbuffer_common.glsl GbufferPush
struct GbufferPush {
    shader::abi::PassPushHeader h;
    std::uint32_t depthTex, normalTex, motionTex, aoTex;  // bindless t2D slots (aoTex: raw AO / history / debug source)
    std::uint32_t historyTex, linearSampler, width, height;
    float radius, falloff, strength, blend;
    std::uint32_t frameIndex, flags, mode, pad;
};
static_assert(sizeof(GbufferPush) == 80 && sizeof(GbufferPush) <= shader::abi::kPushConstantBudget);

class Gbuffer final : public d3d9::DrawReplacer {
public:
    struct Settings {
        bool ao = false;
        bool replaceEngineSsao = true;  // ao.mode = replace
        float aoStrength = 1.0f;
        float aoRadius = 100.0f;  // world units (cm)
        int debug = 0;            // 1 normals, 2 motion, 3 AO
    };

    Gbuffer(d3d9::Device& device, RtScene* scene);
    ~Gbuffer() override;
    Gbuffer(const Gbuffer&) = delete;
    Gbuffer& operator=(const Gbuffer&) = delete;

    void apply(const Settings& s);

    // d3d9::DrawReplacer: the game's SSAO draws (ao.mode = replace)
    bool handles(d3d9::draws::Kind kind, d3d9::scene::Phase phase, const d3d9::draws::Tag& tag) const override;
    void replace(d3d9::Device& device, const d3d9::ReplaceQuery& query, d3d9::DrawReplacement& out) override;

private:
    void enable();
    void disable();
    bool ensureTargets(d3d9::PassContext& ctx);
    bool pipelinesReady();
    void recordAo(d3d9::PassContext& ctx);
    void recordDebug(d3d9::PassContext& ctx);

    d3d9::Device& dev_;
    RtScene* scene_ = nullptr;
    Settings s_;
    bool on_ = false, failed_ = false;
    vk::PassLayout layout_;  // set 1: two storage images
    VkPipeline gtao_ = VK_NULL_HANDLE, filter_ = VK_NULL_HANDLE, debug_ = VK_NULL_HANDLE;
    VkFormat debugFormat_ = VK_FORMAT_UNDEFINED;
    std::future<VkPipeline> gtaoBuild_, filterBuild_;
    std::shared_ptr<d3d9::Shader> whiteVs_, whitePs_;  // the SSAO replacement (fullscreen, writes 1)
    bool replacing_ = false;
    std::uint64_t ssaoReplaced_ = 0;
    std::uint32_t history_ = 0;          // the history image written last (0 / 1)
    bool historyValid_ = false;
    std::uint64_t aoFrames_ = 0, skipped_ = 0, debugFrames_ = 0;
    double lastLog_ = -1e9;
};

}  // namespace rvk::enh
