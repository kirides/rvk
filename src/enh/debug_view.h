#pragma once

// The first (debug) enhancement, setting debug.inject_view (docs/research/injection.md 5): proof of the enhancement plumbing
// end to end. At the injection point AfterOpaque of the frame's first main view, a GLSL compute shader
// (shaders/debug/inject_view.comp, embedded at build time) writes a factor per pixel into a transient image: in "depth" mode
// distance bands + a world-space grid from the engine's depth map and the FrameBlock camera (the lines must stay put while the
// camera moves), in "tint" mode a colour cycled by the FrameBlock time. A fullscreen pass (inject_view.frag) multiplies the
// scene HDR target by it, before the refraction copy, water, alpha and the post chain. Its mode / strength travel in the
// FrameBlock's feature parameters (abi::kFrameParamDebugView). Off = the pass is not registered: the frame is untouched.

#include <cstdint>
#include <string_view>

#include "d3d9/device.h"
#include "shader/abi.h"
#include "vk/passes.h"

namespace rvk::enh {

// mirror of shaders/debug/inject_view.glsl DebugViewPush (offsets checked by rvk_tests abi_glsl_layout_matches_cpp)
struct DebugViewPush {
    shader::abi::PassPushHeader h;
    std::uint32_t depthTex;
    std::uint32_t width;
    std::uint32_t height;
    std::uint32_t srcTex;
    std::uint32_t srcSampler;
    std::uint32_t pad;
};
static_assert(sizeof(DebugViewPush) == 40 && sizeof(DebugViewPush) <= shader::abi::kPushConstantBudget);

class DebugView {
public:
    enum class Mode : std::uint8_t { Off, Depth, Tint };
    static Mode parse(std::string_view option);  // "off" / "depth" / "tint"

    explicit DebugView(d3d9::Device& device);
    ~DebugView();
    DebugView(const DebugView&) = delete;
    DebugView& operator=(const DebugView&) = delete;

    // render thread (settings callbacks run at the frame boundary)
    void setMode(Mode mode);
    Mode mode() const { return mode_; }

private:
    bool ensurePipelines(VkFormat target);
    void record(d3d9::PassContext& ctx);
    void destroyPipelines();

    d3d9::Device& dev_;
    Mode mode_ = Mode::Off;
    std::uint32_t passId_ = 0;
    vk::PassLayout layout_;
    VkPipeline compute_ = VK_NULL_HANDLE;
    VkPipeline composite_ = VK_NULL_HANDLE;
    VkFormat compositeFormat_ = VK_FORMAT_UNDEFINED;
    bool failed_ = false;
    std::uint64_t frames_ = 0;  // frames drawn (logged at the first)
};

}  // namespace rvk::enh
