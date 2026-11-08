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
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

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

// ---- S-7b: the bloom chain (BrightPass, GlowH, Streak, GlowVAddBloom2) and the DoF middle (BlurH, BlurV, NearCoc, SmallBlur) ----
// mirrors of shaders/post/post_bright / post_glow / post_bloom / post_dofmid.comp.hlsl
struct PostBrightParams {
    std::uint32_t tex[4];  // scene, adapted luminance, depth map, -
    float uv[4];
    float tap[4];
    float ps[4];
    std::uint32_t size[4];
};
struct PostGlowParams {
    std::uint32_t tex[4];  // bright image, sampler, target size
    float uvH[4], centerH[4], offH[6][4];
    float uvS[4], centerS[4], light[4], sw[4];
};
struct PostBloomParams {
    std::uint32_t tex[4];  // glow image, streak image, sampler, previous bloom image (0: the output is read back in place)
    std::uint32_t size[4];
    float uv[4], center[4], off[6][4], blend[4];
};
struct PostDofMidParams {
    std::uint32_t tex[4];  // DoFDown image
    std::uint32_t size[4];
};
struct PostGroupPush {
    shader::abi::PassPushHeader h;
    std::uint32_t params[2];
    std::uint32_t pad[2];
};
static_assert(sizeof(PostGroupPush) == 32 && sizeof(PostGroupPush) <= shader::abi::kPushConstantBudget);

inline constexpr int kBloomUnits = 4, kDofMidUnits = 4, kMaxGroupUnits = 8;
// the groups of the scene graph's units (held and run as one kernel set when complete)
enum class OwnGroup : int { Lum = 0, Bloom = 1, DofMid = 2, Count = 3 };
inline constexpr int kGroupCount = static_cast<int>(OwnGroup::Count);

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
    // S-7b: the group (OwnGroup) and the index inside it of any group unit by its name; false for any other unit
    static bool groupUnit(std::string_view name, int& group, int& index);
    static int groupSize(int group);
    // the quad's uv at the centre of the pixel (0, 0) of a W x H target and its step per pixel, from a composed unit's vertices (the half-pixel conventions of the
    // renderer included); false + `why`: not a plain quad
    static bool quadUv(const fxc::EffectDraw& d, std::uint32_t w, std::uint32_t h, float out[4], std::string& why);
    // runs the group's kernels at its completion (`rec` = the group's composed draws in order, `write`: On = the engine's output images get the result and the units
    // are not drawn; else Compare: the result goes to scratch images, the intermediates are compared with the engine's units right now (the final image: finishCompare
    // after the last unit's engine draw)). False: nothing recorded (the caller draws the held units).
    bool runGroup(int group, const std::array<const fxc::EffectDraw*, kMaxGroupUnits>& rec, bool write);
    // Compare: the last unit of the group has been drawn by the engine: compare its target with ours
    void finishCompare(int group);
    bool groupReady(int group);
    // the pure part: the kernel's parameters from the eight composed draws (viewport sizes, quad data, VS / PS constants); false + `why`: not as
    // expected (the units then run as the engine's draws)
    // `sceneSubstituted`: the scene texture is replaced by the upscaler's output at draw time (its size is the taps' 1 / cx, 1 / cy, not the engine image's)
    static bool lumParams(const std::array<const fxc::EffectDraw*, kLumUnits>& rec, bool sceneSubstituted, PostLumParams& out, std::string& why);
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
    bool runBloom(const std::array<const fxc::EffectDraw*, kMaxGroupUnits>& rec, bool write);
    bool runDofMid(const std::array<const fxc::EffectDraw*, kMaxGroupUnits>& rec, bool write);
    // compare: copies `ours` (an RGBA8 scratch image) and the engine's target of the same size into this frame slot's readback buffer; evaluated when the slot is reused
    void recordCompare(rhi::CommandList& cmd, const char* name, rhi::Texture& ours, rhi::Texture& engine);
    void evalCompares(std::uint32_t slot);
    struct Pending {
        const char* name;
        std::uint32_t w, h, pitch, offOurs, offEngine;
        bool engineBgra;
        int group;
    };
    std::array<std::vector<Pending>, rhi::kFramesInFlight> cmpPending_;
    std::array<std::uint32_t, rhi::kFramesInFlight> cmpCursor_{};
    std::array<std::uint64_t, rhi::kFramesInFlight> cmpFrame_{};
    std::unique_ptr<rhi::Buffer> cmpBuf_;
    std::array<std::unique_ptr<rhi::Pipeline>, 4> kern_;  // bright, glow, bloom, dofmid
    std::array<std::future<std::unique_ptr<rhi::Pipeline>>, 4> kernBuilding_;
    bool kernFailed_ = false;
    // scratch of the Compare run of the bloom group, kept until the engine's last unit has been drawn
    rhi::Texture* bloomOurs_ = nullptr;
    d3d9::Texture* bloomEngine_ = nullptr;
    rhi::Texture* dofOursV_ = nullptr;
    rhi::Texture* dofOursS_ = nullptr;
    d3d9::Texture* dofEngineV_ = nullptr;
    d3d9::Texture* dofEngineS_ = nullptr;
    struct GroupStats {
        std::uint64_t runs = 0, refused = 0, compared = 0, differing = 0;
        double sumRel = 0.0, maxRel = 0.0;  // relative: sum |d| / sum engine, per image
        std::uint32_t maxStep = 0;           // the largest per-channel difference in 8-bit steps
        std::uint64_t pixels = 0, offPixels = 0;  // pixels compared, pixels differing by more than 1 step
    };
    std::array<GroupStats, kGroupCount> gstats_{};
    std::uint32_t dumped_ = 0;
    std::map<std::string, GroupStats> named_;  // per compared image
    std::string groupWhy_[kGroupCount];
    std::uint32_t groupWhyLogged_[kGroupCount]{};

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
