#pragma once

// Ray-traced water reflections (setting rt.reflections, docs/research/rt-reflections.md 3.3): a compute pass at
// AfterRefractionCopy (before the water draws) traces, for every pixel of a reflection image, the mirror ray of the water
// surface's plane against the frame's TLAS (enh/rt_scene) and shades the hit (screen reuse or the engine's material
// lighting, shaders/water/water_rt.comp); the water's pixel shader (water.frag) samples that image like a planar reflection,
// its lookup bent by the waves and ripples, and falls back to its screen-space trace where it has nothing.
//
// Why not one ray per water pixel in the pixel shader (the first version): at lake_jungle the ray query inside the water's
// already large pixel shader (163 -> 234 VGPRs, 6 waves) cost +3.4 ms at 1080p on a 7900 XTX; the compute pass traces
// coherent mirror rays at half resolution in its own small shader.
//
// The surfaces are the water bodies drawn in the last frames (enh/water: a plane fitted to each body's vertices, its world
// box, its outdoor factor); bodies that are not planar (rivers with falls) are left out (their pixels keep the SSR).
// The tier holds the RT scene (RtScene::acquire) while on; off = nothing registered, no GPU memory, the water's SSR as before.

#include <array>
#include <cstdint>
#include <future>
#include <unordered_map>

#include "d3d9/device.h"
#include "shader/abi.h"
#include "vk/passes.h"

namespace rvk::enh {

class RtScene;

inline constexpr std::uint32_t kWaterRtMaxBodies = 16;

// mirror of shaders/water/water_rt.comp WaterRtParams (std430)
struct WaterRtParams {
    std::uint32_t tex[4];    // depth map, refraction copy, linear clamp sampler, point clamp sampler
    std::uint32_t info[4];   // output width, height, pixels per output pixel, bodies
    std::uint32_t trace[4];  // RVK_RT_TRACE_* of the reflection ray, shadow rays (0 / 1 cut-outs solid / 2 alpha-tested), tweaks, debug
    float reach[4];          // reach, extra cone spread (rad), shadow ray reach, unused
    float fogColor[4];
    float fogParams[4];
    float indoor[4];
    float sh[7][4];
    float sunColor[4];
    float sunDir[4];
    float plane[kWaterRtMaxBodies][4];  // y = a x + b z + c, w outdoor factor
    float box[kWaterRtMaxBodies][4];    // min x, min z, max x, max z
};
static_assert(sizeof(WaterRtParams) == 16 * (4 + 3 + 7 + 2 + 2 * kWaterRtMaxBodies));

// mirror of water_rt.comp WaterRtPush
struct WaterRtPush {
    shader::abi::PassPushHeader h;
    std::uint32_t params[2];
    std::uint32_t pad[2];
};
static_assert(sizeof(WaterRtPush) == 32 && sizeof(WaterRtPush) <= shader::abi::kPushConstantBudget);

class WaterRt {
public:
    // a water body as a plane (enh/water measures it once per vertex buffer)
    struct Body {
        float plane[3]{};  // y = plane[0] x + plane[1] z + plane[2]
        float box[4]{};    // min x, min z, max x, max z
        float outdoor = 1.0f;
    };
    // the light of the water draws (their pixel constants c3 / c4, c14..c22, c24)
    struct Light {
        float fogColor[4]{}, fogParams[4]{}, indoor[4]{}, sh[7][4]{}, sunColor[4]{}, sunDir[4]{};
    };

    WaterRt(d3d9::Device& device, RtScene* scene);
    ~WaterRt();
    WaterRt(const WaterRt&) = delete;
    WaterRt& operator=(const WaterRt&) = delete;

    // the ray tracing tier is on and the pass can run
    bool available() const;
    // tier 0 off, 1 low, 2 medium, 3 high (water.enabled + water.reflections = ssr + rt.reflections); tweaks = rt.reflection_tweaks;
    // debug: 0, 1 the source view (shaded hits marked), 2 every hit shaded
    void configure(int tier, int tweaks, int debug);
    int tier() const { return tier_; }
    // render thread, per replaced water draw of the main view
    void seeBody(std::uint64_t id, const Body& b, std::uint64_t frame);
    void setLight(const Light& l) { light_ = l; }
    // the reflection image for the water draws of this frame's first main view (0 = none: SSR); flags bit 0 = SSR fallback
    std::uint32_t imageSlot(std::uint64_t frame, std::uint32_t mainView) const {
        return frame == imageFrame_ && mainView == 1 ? imageSlot_ : 0u;
    }
    std::uint32_t flags() const { return tier_ >= 2 && (tweaks_ & 4) == 0 ? 1u : 0u; }

private:
    void record(d3d9::PassContext& ctx);
    bool pipelineReady();
    void logStats();

    d3d9::Device& dev_;
    RtScene* scene_ = nullptr;
    int tier_ = 0, tweaks_ = 0, debug_ = 0;
    bool held_ = false, failed_ = false;
    std::uint32_t passId_ = 0;
    vk::PassLayout layout_;
    VkPipeline pipeline_ = VK_NULL_HANDLE;
    std::future<VkPipeline> building_;  // the compute pipeline, built in the background (no hitch when the tier is turned on)
    struct Seen {
        Body body;
        std::uint64_t frame = 0;
    };
    std::unordered_map<std::uint64_t, Seen> bodies_;  // by vertex buffer id (the sea grid: its body's)
    Light light_;
    std::uint64_t imageFrame_ = ~0ull;
    std::uint32_t imageSlot_ = 0;
    std::uint64_t passes_ = 0, skipped_ = 0;
    std::uint32_t lastBodies_ = 0, lastWidth_ = 0, lastHeight_ = 0;
    double lastLog_ = -1e9;
};

}  // namespace rvk::enh
