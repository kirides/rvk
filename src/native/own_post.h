#pragma once

// S-7 (docs/research/frame-ownership.md 14.8): the post chain on our own shaders, Full mode, `native.own_post` (Debug, default off).
// Group 1 = the luminance chain of the post graphs (eight draws: LogLuminance into 64x64 R32F, DownSample into 32 .. 2, AdaptiveLiminance and
// the AvgLuminance Copy into the 1x1 images) as ONE compute dispatch (shaders/post/post_lum.comp.hlsl). The units are still composed by
// native::EffectOps (effect_ops.cpp); this class turns their composed draws into the kernel's parameters, records the dispatch
// (Device::runPassNow) and, in compare mode, checks the kernel's result against the engine's own units.
//
// Parameters come from the composed draws (the quad's uv at the pixel centres, the taps' offsets, the adaptation constants), so the engine's
// half-texel conventions are inherited. Render thread.

#include <array>
#include <cstdint>
#include <future>
#include <memory>
#include <string>
#include <string_view>

#include "d3d9/device.h"
#include "native/effect_compose.h"
#include "rhi/game_device.h"
#include "rhi/rhi.h"
#include "shader/abi.h"

namespace rvk::native {

struct PostLumStage {
    float uv[4];   // the quad's uv at pixel (0, 0)'s centre (x, y) and its step per pixel (z, w)
    float tap[4];  // the taps' uv offsets (cx, cy)
};
// mirror of shaders/post/post_lum.comp.hlsl PostLumParams
struct PostLumParams {
    std::uint32_t tex[4];  // scene, previous adapted luminance (bindless slots)
    PostLumStage stage[7];
    float adapt[4];        // lerp factor, clamp min, clamp max
};
static_assert(sizeof(PostLumParams) == 16 + 7 * 32 + 16);
struct PostLumPush {
    shader::abi::PassPushHeader h;
    std::uint32_t params[2];
    std::uint32_t pad[2];
};
static_assert(sizeof(PostLumPush) == 32 && sizeof(PostLumPush) <= shader::abi::kPushConstantBudget);

inline constexpr int kLumUnits = 8;

class OwnPost {
public:
    enum class Mode : std::uint8_t { Off, On, Compare };
    explicit OwnPost(d3d9::Device& dev);
    ~OwnPost();
    OwnPost(const OwnPost&) = delete;
    OwnPost& operator=(const OwnPost&) = delete;

    void setMode(Mode m);
    Mode mode() const { return mode_; }

    // the index (0..7) of a luminance chain unit by its name ("<filter> <technique>"), -1 for any other unit
    static int lumUnit(std::string_view name);
    // the pure part: the kernel's parameters from the eight composed draws (viewport sizes, quad data, VS / PS constants); false + `why`: not as
    // expected (the units then run as the engine's draws)
    static bool lumParams(const std::array<const fxc::EffectDraw*, kLumUnits>& rec, PostLumParams& out, std::string& why);
    // the compute pipeline exists (built in the background after the mode is switched on)
    bool lumReady();
    // records the dispatch now (runPassNow). `write`: the engine's two persistent images get the result (the units are not drawn); else (compare) the
    // result goes to scratch images and is compared with the engine's Adaptive unit's target after the GPU is done. False: nothing recorded.
    bool runLum(const std::array<const fxc::EffectDraw*, kLumUnits>& rec, bool write);
    void report();

    // counters (rvk.log `native: own post`)
    struct Stats {
        std::uint64_t runs = 0, refused = 0, compared = 0, differing = 0;
        double maxRel = 0.0, sumRel = 0.0;
    };
    const Stats& stats() const { return stats_; }

private:
    void readCompare(std::uint32_t slot);

    d3d9::Device& dev_;
    Mode mode_ = Mode::Off;
    std::unique_ptr<rhi::Pipeline> lum_, tile_;
    std::future<std::unique_ptr<rhi::Pipeline>> building_, buildingTile_;
    bool failed_ = false;
    std::unique_ptr<rhi::Buffer> readback_;  // per frame slot: [ours, engine] 4 bytes each at 512-byte strides
    std::array<std::uint64_t, rhi::kFramesInFlight> pendingFrame_{};
    Stats stats_;
    double lastLog_ = 0.0;
    std::string lastWhy_;
    std::uint32_t whyLogged_ = 0;
};

}  // namespace rvk::native
