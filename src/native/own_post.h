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
#include <atomic>
#include <cstdint>
#include <future>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
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
// the groups of the scene graph's units (held and run as one kernel set when complete); Units = S-7c's single draw units (their compare statistics only)
enum class OwnGroup : int { Lum = 0, Bloom = 1, DofMid = 2, Units = 3, Count = 4 };
inline constexpr int kGroupCount = static_cast<int>(OwnGroup::Count);

// ---- S-7c (frame-ownership.md 14.16): the post chain's single draw units on our own pixel shaders (own_post_units.cpp, shaders/post/post_*.frag.hlsl) ----
// A unit keeps its composed draw (the engine's vertex shader, quad, target, states, constants, textures) and gets our port of its pixel shader in place of the
// translated one. ToneMap can also be folded into its two consumers (DoFDown, DoF: "fused" ports that tone-map the HDR scene themselves): its 8-bit image is
// then not drawn. Opt-in (RVK_OWNPOST_FUSE=1): measured slower than the plain ports on the RX 7900 XTX (14.16).
enum class PostUnit : std::uint8_t { None, ToneMap, Hdri, DofDown, Dof, OvColor, OvRadialColor, OvRadialBlur, OvImage, OvStrips, Count };
inline constexpr int kPostUnitCount = static_cast<int>(PostUnit::Count);
// the pixel shader ports: one per unit (index = PostUnit - 1), then the fused DoFDown / DoF
inline constexpr int kPortDofDownTm = kPostUnitCount - 1, kPortDofTm = kPostUnitCount, kPortCount = kPostUnitCount + 1;

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

    // ---- S-7c: the single draw units ----
    // the unit of a post technique ("ToneMap", "DoFDown", "OverlayMovieStripsAbs", ...), None for any other
    static PostUnit unitOf(std::string_view technique);
    static const char* unitName(PostUnit u);
    // fusion of ToneMap into DoFDown / DoF wanted (RVK_OWNPOST_FUSE=1) and its two ports built
    bool fuseReady();
    // `tm` (a composed ToneMap draw) can be folded into its consumers: a full-target quad of point taps 1:1 on the scene. False + `why`
    static bool toneMapFusable(const fxc::EffectDraw& tm, std::string& why);
    // our version of unit `u`'s composed draw `d`: `out` = `d` with our pixel shader (fused when `tm` is given: DoFDown / DoF reading the HDR scene of that ToneMap
    // draw; the extra inputs in s5 / s6 and c16..c20). False: the unit stays the engine's (the port is not built yet, the engine's pixel shader is not the expected
    // one, a fused consumer's taps do not fit the ToneMap image)
    bool unitDraw(PostUnit u, const fxc::EffectDraw& d, const fxc::EffectDraw* tm, fxc::EffectDraw& out);
    // Compare: a scratch render target like `target` (size, format) for our version of a unit; `precopy`: it starts as a copy of the target's content (blended /
    // clipped draws; recorded now, before the engine's draw)
    d3d9::Surface* scratchFor(d3d9::Surface* target, bool precopy);
    // Compare: our scratch result against the engine's target, read back with the frame (`name` = the compared image; the per-image line of report())
    void compareUnit(const char* name, d3d9::Surface* ours, d3d9::Surface* engine);
    // counters: draws of a unit on our shader / kept on the engine's / ToneMap units folded into their consumers, drawn late, unread (not drawn)
    void countUnit(PostUnit u, bool own) {
        ++(own ? unitOwn_ : unitEngine_)[static_cast<std::size_t>(u)];
        if (own) ++acts_;
    }
    void countToneMap(int what) { ++tmCounts_[static_cast<std::size_t>(what)]; }  // 0 folded, 1 drawn late (a consumer could not fuse), 2 unread, 3 outside draw while held

    // ---- S-7d (frame-ownership.md 14.18): the effect ports (shaders/post/fx, generated by tools/post_port.py; table post_port_table.inl) ----
    // Every vertex / pixel shader of the effects the native frame composes (the shadow mask chain, the SSAO chain, the PSSM pass, the terrain map's blur,
    // the post chain's draw units and the rare image effects) has a port keyed by FNV-1a 64 of its D3D9 tokens. fxPort = our d3d9::Shader for the engine
    // shader `engine` (the engine's reflection: same registers, stages, outputs), null when it has no port or the port's module is not built yet (`why`).
    const d3d9::Shader* fxPort(const void* engine, const char** why = nullptr);
    // the port's name ("ssm_ssao.frag"; static storage) of an engine shader fxPort has seen, null: none
    const char* fxPortName(const void* engine) const;
    // which composed units the ports serve (EffectOps' unit kinds, for the counters): 0 mask, 1 SSAO chain, 2 PSSM, 3 terrain blur, 4 post, 5 other
    static constexpr int kFxKinds = 6;
    void countFx(int kind, bool vsOurs, bool psOurs, bool vsLeft, bool psLeft, const char* why);
    // drawn with a port this session (all kinds, + S-7c units + kernel runs): the stage line's "acted"
    std::uint64_t acts() const { return acts_ + stats_.runs + gstats_[1].runs + gstats_[2].runs; }
    void countAct() { ++acts_; }

    // ---- F12 / F13 (frame-ownership.md 14.24): the sky dome (main view + the sky cube's faces) and the GUI sprite batches on the effect ports ----
    // `native.own_sky` / `native.own_gui` (Full, Debug): On = the composed draws are drawn with the ports (shaders/post/fx/sky_dome.*, gui_main.*); Compare = they
    // are drawn a second time with the ports into a scratch copy of their target before the composed draw, and the two images are compared (`image 'sky_dome.frag'`,
    // `image 'gui_main.frag'`). The ports are built whenever one of own_post / own_sky / own_gui is not off.
    enum class Scene : int { Sky = 0, Gui = 1, Count = 2 };
    void setSceneMode(Scene which, Mode m);
    Mode sceneMode(Scene which) const { return sceneMode_[static_cast<std::size_t>(which)]; }
    // a composed draw of the class: drawn with our shaders (`ours`) or kept on the engine's (`why`)
    void countScene(Scene which, bool ours, const char* why);
    std::uint64_t sceneActs(Scene which) const { return sceneActs_[static_cast<std::size_t>(which)]; }
    // Compare: a 2D scratch image of `src`'s size and format (any face / level: the sky cube's faces), `tag` keeps two scratch images of one size apart; `copy` = it
    // starts as a copy of `src` (recorded now)
    d3d9::Surface* scratchOf(d3d9::Surface* src, int tag, bool copy);
    // draws skipped so far (a shader still compiling, a D3D12 PSO still building): a compare whose two draws ran while it moved is dropped (counted)
    std::uint64_t drawSkips();
    void countDroppedCompare() { ++droppedCompares_; }

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
    void recordCompare(rhi::CommandList& cmd, const char* name, rhi::Texture& ours, rhi::Texture& engine, int group = -1, int channels = 4);
    void evalCompares(std::uint32_t slot);
    struct Pending {
        const char* name;
        std::uint32_t w, h, pitch, offOurs, offEngine;
        bool engineBgra, oursBgra;
        int group, channels;
        int kind = 0;   // 0 8-bit unorm x4 (RGBA / BGRA), 1 float16 x comps, 2 float32 x comps (S-7d: the mask chain's float images)
        int comps = 4;
    };
    // ---- F12 / F13 ----
    std::array<Mode, 2> sceneMode_{};
    std::array<std::uint64_t, 2> sceneActs_{};
    struct SceneCounts {
        std::uint64_t draws = 0, ours = 0, left = 0;
        std::string why;
    };
    std::array<SceneCounts, 2> sceneW_{};
    std::unique_ptr<d3d9::Texture> sceneScratch_[8];
    int sceneScratchTag_[8]{};
    std::uint32_t sceneScratchNext_ = 0;
    std::uint64_t droppedCompares_ = 0;
    void reportScene();
    // ---- S-7d ----
    void buildFxPorts();
    void reportFx();
    void destroyFxPorts();
    struct FxPortShader {
        const void* engine = nullptr;
        std::shared_ptr<d3d9::Shader> ours;
    };
    std::vector<std::atomic<rhi::ShaderModule>> fxModules_;     // per table row (built by fxBuilding_)
    std::vector<std::atomic<std::uint8_t>> fxState_;           // per table row: 0 not built, 1 building, 2 ready, 3 failed
    std::vector<std::future<void>> fxBuilding_;
    std::unordered_map<const void*, int> fxRow_;               // engine shader -> table row (-1: no port)
    std::unordered_map<const void*, std::shared_ptr<d3d9::Shader>> fxOurs_;  // engine shader -> our shader
    struct FxCounts {
        std::uint64_t draws = 0, vsOurs = 0, psOurs = 0, vsLeft = 0, psLeft = 0;
        std::string why;
    };
    std::array<FxCounts, kFxKinds> fxW_{};  // the 5 s window
    std::uint64_t acts_ = 0;
    // ---- S-7c ----
    void buildPorts();
    struct PortShader {
        const d3d9::Shader* engine = nullptr;
        std::shared_ptr<d3d9::Shader> ours;
    };
    std::array<std::future<rhi::ShaderModule>, kPortCount> portBuilding_;
    std::array<rhi::ShaderModule, kPortCount> port_{};
    std::array<std::uint8_t, kPortCount> portState_{};  // 0 not built, 1 building, 2 ready, 3 failed
    std::array<std::vector<PortShader>, kPortCount> portShaders_;  // per engine pixel shader (a handful)
    bool portReady(int port);
    d3d9::Shader* portShader(int port, const d3d9::Shader* engine, bool fused);
    std::unique_ptr<d3d9::Texture> scratch_[24];  // compare scratch targets (by size + format: ToneMap, DoFDown, the back buffer, an overlay copy; S-7d: the mask chain's images)
    std::array<std::uint64_t, kPostUnitCount> unitOwn_{}, unitEngine_{};
    std::array<std::uint64_t, 4> tmCounts_{};
    std::array<std::string, kPostUnitCount> unitWhy_;
    std::uint64_t bigCompareFrame_ = 0;
    std::uint64_t bigFrame_ = ~0ull;  // (compareUnit: the frame of the full-resolution compares counted in bigInFrame_)
    std::uint32_t bigInFrame_ = 0;
    std::map<std::string, std::uint64_t> cmpFirst_;  // compared image -> the frame of its first compare (warm-up)
    std::uint64_t cmpWarmup_ = 0;
    bool noFuse_ = false;
    bool noUnits_ = false;  // RVK_OWNPOST_UNITS=0: every single unit stays the engine's
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
    std::uint32_t unsupportedLogged_ = 0;  // (S-7d: compares of formats the readback does not read)
    std::uint64_t aliasedInputs_ = 0;  // DoF middle runs whose DoFDown image was also a target (copied aside)
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

// the owned stages' state line (static_ops_stages.cpp): the device's OwnPost mode (0 off, 1 on, 2 compare) and its acted count (0 without one)
int ownPostMode();
std::uint64_t ownPostActs();
// F12 / F13: the device's OwnPost (null before the HAL device made it); the stage line's modes / acted counts of own_sky / own_gui
OwnPost* ownPost();
void registerOwnPost(OwnPost* p, OwnPost* only = nullptr);  // (OwnPost's constructor / destructor; `only`: cleared only while it is that one)
int ownSceneMode(OwnPost::Scene which);
std::uint64_t ownSceneActs(OwnPost::Scene which);

}  // namespace rvk::native
