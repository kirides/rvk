#pragma once

// The RT scene's debug view, setting rt.debug (docs/research/rt-reflections.md 3): the correctness proof of the RT scene
// (enh/rt_scene). At BeforeGui (the back buffer holds the post-processed frame, the depth map and the TLAS of this frame are
// still valid) a ray-query compute pass (shaders/rt/rt_debug.comp) traces one camera ray per pixel against the TLAS and a
// fullscreen pass blends the result over the frame: "instances" (a colour per instance), "depth" (the traced distance
// against the rendered one, from the main view's hardware depth buffer captured at AfterOpaque (every solid draw; the
// engine's linear depth map has only the z-pass subset): green match, red the TLAS lacks geometry, magenta the TLAS has
// extra / misplaced geometry, blue a miss where the raster has geometry), "normals" (interpolated vertex normals fetched from the geometry records). Holds
// the RT scene while on; off = nothing registered.

#include <chrono>
#include <cstdint>
#include <string_view>

#include "d3d9/device.h"
#include "shader/abi.h"
#include "vk/passes.h"
#include "vk/raytracing.h"

namespace rvk::enh {

class RtScene;

// mirror of shaders/rt/rt_debug.glsl RtDebugPush (offsets checked by rvk_tests abi_glsl_pass_push_matches_cpp)
struct RtDebugPush {
    shader::abi::PassPushHeader h;
    std::uint32_t depthTex;
    std::uint32_t width;
    std::uint32_t height;
    std::uint32_t srcTex;
    std::uint32_t srcSampler;
    std::uint32_t mode;  // RT_DEBUG_* | kRtDebugLinearDepth
    std::uint32_t probes[2];  // device address of the probe results (kRtDebugProbes x RtDebugProbe; 0 = none)
};
// what the compute pass found at the probe pixels (a 5 x 5 grid over the picture): rvk.log names the instances
struct RtDebugProbe {
    std::uint32_t record;  // record index + 1 (0 = the ray missed)
    float t;               // traced distance
    float rasterT;         // the raster's distance along the ray (0 = nothing)
    std::uint32_t code;    // 1 match, 2 traced farther, 3 traced closer, 4 missed where the raster has geometry, 5 hit where it has none
};
inline constexpr std::uint32_t kRtDebugProbes = 25;
inline constexpr std::uint32_t kRtDebugLinearDepth = 0x100u;  // depthTex holds view depth (the hardware depth capture)
static_assert(sizeof(RtDebugPush) == 48 && sizeof(RtDebugPush) <= shader::abi::kPushConstantBudget);

class RtDebug {
public:
    enum class Mode : std::uint8_t { Off, Instances, Depth, Normals };
    static Mode parse(std::string_view option);  // "off" / "instances" / "depth" / "normals"

    RtDebug(d3d9::Device& device, RtScene& scene);
    ~RtDebug();
    RtDebug(const RtDebug&) = delete;
    RtDebug& operator=(const RtDebug&) = delete;

    void setMode(Mode mode);  // render thread (settings callbacks)
    Mode mode() const { return mode_; }

private:
    bool ensurePipelines(VkFormat target);
    void record(d3d9::PassContext& ctx);
    void captureDepth(d3d9::PassContext& ctx);  // depth mode, AfterOpaque: the hardware depth -> view depth (R32F)
    void destroyPipelines();
    void readProbes(std::uint32_t slot);  // the slot's previous results (its frame completed) -> rvk.log

    d3d9::Device& dev_;
    RtScene& scene_;
    Mode mode_ = Mode::Off;
    std::uint32_t passId_ = 0;
    std::uint32_t depthPassId_ = 0;
    std::uint64_t depthFrame_ = 0;  // frame value of the last depth capture
    VkExtent2D depthExtent_{1, 1};  // its size
    std::uint64_t fallbacks_ = 0;   // frames without a capture
    vk::rt::AddressBuffer probes_[vk::Frames::kInFlight];  // host-visible probe results per frame slot
    std::uint64_t probeFrame_[vk::Frames::kInFlight]{};     // frame value whose results a slot holds (0 = none)
    std::chrono::steady_clock::time_point probeLogged_{};
    vk::PassLayout layout_;
    VkPipeline compute_ = VK_NULL_HANDLE;
    VkPipeline depthCompute_ = VK_NULL_HANDLE;
    VkPipeline composite_ = VK_NULL_HANDLE;
    VkFormat compositeFormat_ = VK_FORMAT_UNDEFINED;
    bool failed_ = false;
    std::uint64_t frames_ = 0;
};

}  // namespace rvk::enh
