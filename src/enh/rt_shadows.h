#pragma once

// Ray-traced soft sun shadows (settings shadows.rt*, docs/research/rt-shadows.md): near the camera the sun's shadow mask takes
// its value from shadow rays against the RT scene (enh/rt_scene) instead of the engine's cascade maps, so the nearest
// cascades' detail comes from real geometry: contact-hardening penumbrae (rays towards a cone around the sun), the casters'
// own alpha-tested leaves, no cascade resolution or bias limits.
//
// Three compute passes at AfterDepthPrepass (right after the TLAS is built, before the shadow mask is drawn):
//   trace    one (or two) shadow rays per pixel within the range, a random point of the sun's disc each frame
//   temporal accumulation with the camera's reprojection (depth-checked), the history clamped to the raw neighbourhood
//   spatial  a depth-plane bilateral blur where the temporal variance says the pixel is still noisy (the penumbra)
// The result image (R = visibility, 1 lit) is read by enh/shadows' mask shader inside the range (+ a blend band to the
// cascades); beyond it, and when this is off or the frame has no RT scene, the cascades give the mask as before.
// The tier holds the RT scene (RtScene::acquire) while on; off = nothing registered, no GPU memory.

#include <cstdint>
#include <future>

#include "d3d9/device.h"
#include "shader/abi.h"
#include "vk/passes.h"

namespace rvk::enh {

class RtScene;

// mirror of shaders/shadows/rt_shadow_common.glsl RtShadowParams (std430)
struct RtShadowParams {
    std::uint32_t tex[4];   // depth map, raw visibility, history, accumulated (bindless t2D)
    std::uint32_t info[4];  // width, height, rays per pixel, RVK_RT_TRACE_* flags
    std::uint32_t mode[4];  // frame index, history valid, debug, 0
    float sun[4];           // unit vector towards the sun, tan of the cone's half angle
    float range[4];         // range (view depth, world units), ray reach, origin bias, spatial filter strength
};
static_assert(sizeof(RtShadowParams) == 5 * 16);

// mirror of the passes' push block
struct RtShadowPush {
    shader::abi::PassPushHeader h;
    std::uint32_t params[2];
    std::uint32_t pad[2];
};
static_assert(sizeof(RtShadowPush) == 32 && sizeof(RtShadowPush) <= shader::abi::kPushConstantBudget);

class RtShadows {
public:
    struct Settings {
        int mode = 0;              // shadows.rt: 0 off, 1 near (cascade 0's range), 2 extended (cascades 0 + 1)
        float rangeMetres = 0.0f;  // shadows.rt_range: 0 = from the mode
        float softness = 1.0f;     // shadows.rt_softness: the sun cone's half angle in degrees
        int quality = 2;           // shadows.rt_quality: 1 low (1 ray, cut-outs solid), 2 medium (1 ray), 3 high (2 rays)
        bool wind = true;          // shadows.rt_wind: the trees near the camera in the wind (animated SpeedTrees)
        float filter = 1.0f;       // spatial filter strength (0 = temporal only)
        float bias = 1.5f;         // origin offset along the surface normal (world units)
        int debug = 0;             // shadows.debug rt_term / rt_coverage are handled by the mask shader
    };
    // what the shadow mask shader of this frame uses (slot 0 = none: the cascades alone)
    struct Frame {
        std::uint32_t slot = 0;
        std::uint32_t width = 0, height = 0;
        float range = 0.0f;  // world units
    };

    RtShadows(d3d9::Device& device, RtScene* scene);
    ~RtShadows();
    RtShadows(const RtShadows&) = delete;
    RtShadows& operator=(const RtShadows&) = delete;

    bool available() const;
    void apply(const Settings& s);
    const Settings& settings() const { return settings_; }
    Frame frame(std::uint64_t frameValue) const { return frameValue == frame_ ? info_ : Frame{}; }

    // the range of a mode in metres: near 15, extended 35 (the enhanced cascades' first two splits: 13.5 / 29.5 m)
    static float modeRangeMetres(int mode);

private:
    void startBuild();
    // stage 0 = trace (prepares the frame), 1 = temporal, 2 = spatial (publishes the image)
    void record(d3d9::PassContext& ctx, int stage);
    bool pipelinesReady();
    void logStats();

    d3d9::Device& dev_;
    RtScene* scene_ = nullptr;
    Settings settings_;
    bool held_ = false, failed_ = false, built_ = false;
    std::uint32_t passId_ = 0;
    vk::PassLayout layout_;
    VkPipeline pipeline_[3] = {VK_NULL_HANDLE, VK_NULL_HANDLE, VK_NULL_HANDLE};
    std::future<VkPipeline> building_[3];
    struct Pending {
        std::uint64_t frame = ~0ull;
        int next = 0;
        RtShadowPush push{};
        std::uint32_t gx = 0, gy = 0;
        vk::Transients::Entry* hist = nullptr;
        vk::Transients::Entry* fin = nullptr;
        std::uint32_t cur = 0, w = 0, h = 0;
        float range = 0.0f;
    };
    Pending pending_;
    std::uint32_t history_ = 0;
    bool historyValid_ = false;
    std::uint32_t lastWidth_ = 0, lastHeight_ = 0;
    std::uint64_t frame_ = ~0ull;
    Frame info_;
    std::uint64_t passes_ = 0, skipped_ = 0;
    float lastSun_[3]{};
    double lastLog_ = -1e9;
};

}  // namespace rvk::enh
