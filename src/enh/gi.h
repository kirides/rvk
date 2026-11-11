#pragma once

// Ray-traced global illumination, stage A "first light" (M10, docs/research/rtgi.md 5-6): DDGI probes whose irradiance the
// engine's own material shaders read as their ambient light.
//
// * P0 light capture: a d3d9::DrawObserver reads the main view's solid draws' light constants: the outdoor ambient SH
//   (g_AmbientLight c15..c21: the most frequent set of the frame), the indoor colour (c14), the sun (g_DirLight c22..c24, the
//   most frequent non-zero set) and the point lights (g_PointLights, deduplicated).
// * P1 probes: 3 camera-centred cascades of 32 x 16 x 32 probes (1.5 / 3 / 6 m), scrolling toroidally (a probe keeps its
//   absolute cell as a key; lookups ignore probes whose key is not the cell they expect). A budget of probes per frame
//   (new cells first, then round robin) is traced at AfterOpaque after the RT scene (shaders/gi/gi_trace.comp: 64 rays,
//   hits lit like the engine's materials: sun + a point light with shadow rays + the probes themselves = multi-bounce;
//   misses = the engine's outdoor ambient as radiance) and blended (gi_blend.comp: L2 SH in the engine's register layout,
//   octahedral depth moments, hysteresis, inside-geometry flag). Header per frame: Device::setGiHeader (FrameBlock::gi).
// * P2 materials: the pixel shaders' GI variant (d3d9::VariantSlot::Gi, built in the background): every read of c14..c21
//   becomes the pixel's probe lookup (shaders/include/rvk_gi.glsl, world position from gl_FragCoord + the FrameBlock's inverse
//   view-projection, geometric normal from its derivatives), mixed with the engine's by gi.strength; the engine's
//   CalcAmbientLightComplex then evaluates it with its normal-mapped normal. gi.lightmaps = replace: the draws' lightmap
//   samplers read the black texture. Only the main view's lit first passes (Device::giEligible).
// * P3 characters: RtScene::setDynamic (skinned draws captured into the RT scene each frame).
//
// Off: nothing registered, no GPU memory, the material shaders' own modules.

#include <array>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <future>
#include <random>
#include <vector>

#include "d3d9/device.h"
#include "engine/lights.h"
#include "rhi/rhi.h"
#include "shader/abi.h"

namespace rvk::enh {

class RtScene;

inline constexpr std::uint32_t kGiCascades = 3;
inline constexpr std::uint32_t kGiMaxLights = 128;  // the most relevant lights of the frame (rtgi.md 19)
inline constexpr std::uint32_t kGiRays = 64;
inline constexpr std::uint32_t kGiDepthTexels = 64;  // 8 x 8 octahedral depth moments per probe

// mirror of shaders/include/rvk_gi.glsl RvkGiVolume (std430)
struct GiVolumeHeader {
    std::uint32_t dims[4];   // probes per cascade x, y, z; cascades
    std::uint32_t data[4];   // probes address, depth moments address
    float params[4];         // strength, normal bias, view bias (cells), cascade blend band (cells)
    std::uint32_t info[4];   // flags, debug view, probes per cascade, frame
    struct Cascade {
        std::int32_t origin[4];
        float spacing[4];    // spacing, 1 / spacing, depth cap (cells), 0
    } cascade[4];
    float sky[7][4];         // the outdoor ambient the probes' sky stands for (the draws' remainder over it is kept)
};
static_assert(sizeof(GiVolumeHeader) == 304);

// mirror of shaders/gi/gi_common.glsl GiPassParams (std430)
struct GiPassParams {
    std::uint32_t lists[4];   // probe list address, ray buffer address
    std::uint32_t volume[4];  // header address, probes in the list, frame
    std::uint32_t flags[4];   // trace flags, lights, tweaks, 0
    float rot[3][4];
    float reach[4];
    float blend[4];           // irradiance hysteresis, depth hysteresis, depth sharpness, back-face share
    float shade[4];           // bounce factor, albedo scale, sun shadow reach, ray cone spread
    float sunColor[4];
    float sunDir[4];
    float sky[7][4];
    float lights[kGiMaxLights * 2][4];
    std::uint32_t screen[4];   // screen probes: depth, probe geometry image, grid width, height
    std::uint32_t screen2[4];  // tile size, rays per probe, scene width, height
    std::uint32_t screen3[4];  // probes image, previous resolve, previous second resolve, linear sampler
    float screen4[4];          // ray reach, ratio cap, history blend, colour bleeding
    std::uint32_t screen5[4];  // probes' second image (directional correction), 0, 0, 0
    float screen6[4];          // directional correction strength, 0, 0, 0
};
static_assert(sizeof(GiPassParams) == 16 * (3 + 3 + 1 + 1 + 1 + 1 + 1 + 7 + kGiMaxLights * 2 + 6));
static_assert(offsetof(GiPassParams, lights) == 288);  // GI_LIGHTS_OFFSET of gi_common.hlsli (the shaders read the lights from memory by offset)

// mirror of gi_common.glsl GiPush
struct GiPush {
    shader::abi::PassPushHeader h;
    std::uint32_t params[2];
    std::uint32_t pad[2];
};
static_assert(sizeof(GiPush) == 32);

// mirror of shaders/gi/gi_reference.comp.hlsl GiRefParamsData (the reference path tracer, gi.reference; its address in GiPush::pad)
struct GiRefParams {
    std::uint32_t buf[4];   // accumulation buffer address, samples this frame, samples accumulated before
    std::uint32_t img[4];   // resolved screen probe image (bindless), view mode, max bounces, seed
    std::uint32_t size[4];  // width, height, bit 0: first frame of the epoch, 0
};
static_assert(sizeof(GiRefParams) == 48);

class Gi final : public d3d9::DrawObserver {
public:
    struct Settings {
        bool enabled = false;
        int quality = 1;          // 0 low, 1 medium, 2 high: probes traced per frame
        float strength = 1.0f;    // 0 = the engine's ambient .. 1 = GI
        bool replaceLightmaps = false;
        bool characters = true;   // skinned draws in the RT scene
        int debug = 0;            // 1 irradiance, 2 cascades, 3 vertex AO
        float bleed = 0.5f;       // colour bleeding of the screen probes (rtgi.md 15): 0 luminance only .. 1 full chroma ratio
        int foliage = 0;          // gi.foliage: leaf cards of the trees in the ray-traced scene: 0 off (default), 1 near (40 m), 2 far (80 m)
        float shadowFill = 0.5f;   // gi.shadow_fill: the floor of the ambient in shade as a share of the engine's (rtgi.md 21)
        bool cutoutsTested = true;   // gi.cutouts: the probe rays alpha-test foliage instead of treating it as solid (rtgi.md 21)
        float directional = 0.5f; // directional correction of the screen probes (gi.directional, rtgi.md 20): 0 off .. 1 full
        float emissive = 1.0f;    // gain of self-illuminated surfaces as light sources (gi.emissive, rtgi.md 19)
        int lights = 1;           // point lights the rays see (rtgi.md 19): 0 the drawn objects' (nearby), 1 the world's (engine/lights)
        int lightCount = 128;     // the most lights per frame (gi.light_count, debug)
        int screen = 2;           // screen probes (stage C, rtgi.md 11): 0 off, 1 low, 2 medium, 3 high
        // the reference path tracer (gi.reference*, docs/research/path-tracing.md; Debug): a view (0 off, 1 reference, 2 DDGI volume,
        // 3 DDGI + screen probes, 4 error of 3, 5 reference indirect, 6 screen indirect, 7 error of 2), samples per frame, samples
        // until the accumulation stops (and is dumped), the most bounces of a path
        int reference = 0;
        int referenceSpp = 4;
        int referenceMax = 1024;
        int referenceBounces = 6;
    };

    Gi(d3d9::Device& device, RtScene* scene);
    ~Gi() override;
    Gi(const Gi&) = delete;
    Gi& operator=(const Gi&) = delete;

    // the ray tracing tier is on and the passes can run
    bool available() const;
    void apply(const Settings& s);

    // the GLSL options of a material pixel shader's GI variant (d3d9::VariantSlot::Gi); false = not a lit material shader.
    // Public for rvk_shadertool corpus --gi
    static bool variantOptions(const d3d9::Shader& s, shader::TranslateOptions& o);

    // d3d9::DrawObserver: the frame's light constants (P0)
    bool observes(d3d9::draws::Kind kind, d3d9::scene::Phase phase) const override;
    bool ownedTreesOk() const override { return true; }  // (F-VAR: tallies registers only, never asks for a capture)
    bool ownedVegOk() const override { return true; }  // (F-VAR2: tallies registers only; the owned vegetation batches are shown through ownedVegObserve)
    const char* observerName() const override { return "gi"; }
    Registers registersRead() const override { return Registers::MaterialLighting; }  // F-VAR: the ambient / sun / point-light / indoor registers (PS window 3, composed from the illumination table for an owned draw)
    void observe(d3d9::Device& device, const d3d9::ReplaceQuery& query) override;

private:
    struct Cascade {
        std::array<std::int32_t, 3> origin{};
        bool valid = false;
        std::uint32_t cursor = 0;
        std::deque<std::uint32_t> priority;  // slots whose cell is new (scrolled in)
        std::vector<std::uint8_t> queued;
    };
    struct AmbientTally {
        std::uint64_t hash = 0;
        std::uint32_t count = 0;
        float regs[8][4]{};  // c14, c15..c21
    };
    struct SunTally {
        std::uint64_t hash = 0;
        std::uint32_t count = 0;
        float regs[3][4]{};  // c22..c24
    };
    struct Light {
        float pos[4]{};  // xyz, 1 / R^2
        float col[4]{};
    };

    void enable();
    void disable();
    void record(d3d9::PassContext& ctx);
    bool pipelinesReady();
    bool ensureResources(rhi::CommandList& cmd);
    void destroyResources();
    void updateCascades(const float camera[3]);
    void queue(Cascade& c, std::uint32_t slot);
    void selectProbes(std::vector<std::uint32_t>& out, std::uint32_t budget);
    void pickLight();
    void logStats(std::uint32_t probes);
    // stage C: the screen probes after the z pre-pass (this frame's depth, the RT scene built there), with the last update's
    // light and volume
    void recordScreen(d3d9::PassContext& ctx);
    // gi.reference: the reference path tracer (right after the screen probes, same depth / RT scene / light) and its view
    void recordReference(d3d9::PassContext& ctx, rhi::CommandList& cl, GiPush push, std::uint32_t resolvedSlot, std::uint32_t w, std::uint32_t h);
    void recordReferenceView(d3d9::PassContext& ctx);
    void flushDumps();
    void scheduleDump(rhi::CommandList& cl, std::uint32_t planes, std::uint32_t spp, bool full);

    d3d9::Device& dev_;
    RtScene* scene_ = nullptr;
    Settings s_;
    bool on_ = false, failed_ = false;
    std::uint32_t passId_ = 0;
    using PipelineFuture = std::future<std::unique_ptr<rhi::Pipeline>>;  // (built on a worker: Device::createComputePipeline is thread-safe)
    std::unique_ptr<rhi::Pipeline> trace_, blend_;
    PipelineFuture traceBuild_, blendBuild_;
    // the screen probes (set 1: three storage images)
    std::unique_ptr<rhi::Pipeline> spTrace_, spResolve_;
    PipelineFuture spTraceBuild_, spResolveBuild_;
    std::uint32_t sgiHistory_ = 0;  // the resolved image written last (0 / 1)
    GiPassParams lastParams_{};
    GiVolumeHeader lastHeader_{};
    bool haveLast_ = false;
    bool sgiValid_ = false;
    bool sgiHalf_ = false;  // the resolved / stats images are half size (low bandwidth mode)
    std::uint64_t screenFrames_ = 0, screenSkipped_ = 0;
    std::unique_ptr<rhi::Buffer> probes_, depth_, rays_;
    // diagnostics (RVK_GI_DIAG, rtgi.md 23): the screen probes' light terms summed by the GPU (wrapping fixed-point counters,
    // two sun classes), read every frame as deltas into 64-bit sums, logged every 5 s
    std::unique_ptr<rhi::Buffer> diag_;
    std::uint32_t diagLast_[64]{};
    double diagSum_[64]{};
    void logDiag();
    bool cleared_ = false;
    // gi.reference
    std::unique_ptr<rhi::Pipeline> refTrace_, refViewPipe_;
    PipelineFuture refBuild_;
    std::uint32_t refViewPass_ = 0;
    std::uint32_t refViewFormat_ = 0;
    std::uint64_t refViewFrame_ = ~0ull;
    std::unique_ptr<rhi::Buffer> refBuf_;
    std::uint32_t refW_ = 0, refH_ = 0, refEpoch_ = 0, refAcc_ = 0, refNextDump_ = 0;
    bool refFirst_ = true, refFinalDumped_ = false;
    std::uint64_t refHash_[4]{};
    std::uint32_t refResets_[4]{}, refFrames_ = 0;
    struct PendingDump {
        std::unique_ptr<rhi::Buffer> buf;
        std::uint64_t frame = 0;
        std::uint32_t w = 0, h = 0, planes = 0, spp = 0, epoch = 0;
        bool full = false;
        float camera[3]{};
    };
    std::vector<PendingDump> refDumps_;
    std::array<Cascade, kGiCascades> cascades_;
    std::mt19937 rng_{0x9E3779B9u};

    // the frame's light (P0): tallies of this frame's draws, the light in use
    std::uint64_t tallyFrame_ = 0;
    std::vector<AmbientTally> ambient_;
    std::vector<SunTally> sun_;
    std::vector<Light> lights_;
    std::uint32_t ambientDraws_ = 0;
    float sky_[7][4]{}, indoor_[4]{}, sunColor_[4]{}, sunDir_[4]{0.0f, 1.0f, 0.0f, 0.0f};
    bool haveSky_ = false;
    bool trueSky_ = false;                // sky_ / indoor_ come from the engine's hemisphere light (engine/hemisphere)
    float drawSky_[7][4]{};               // the draws' most frequent set (the old inference; logged against the hemisphere's)
    float drawIndoor_[4]{};
    std::vector<Light> frameLights_;
    std::vector<engine::lights::WorldLight> worldScratch_;
    engine::lights::Stats lightStats_{};
    std::uint32_t worldLightsUsed_ = 0;
    double ratioSum_ = 0.0;
    std::uint64_t ratioCount_ = 0;

    // stats
    double lastLog_ = -1e9;
    std::uint64_t passes_ = 0, skipped_ = 0, probesTraced_ = 0, giDrawsBase_ = 0;
    std::uint64_t updatesTotal_ = 0;  // since enabled (the first update's log line)
};

}  // namespace rvk::enh
