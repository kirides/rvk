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
#include <cstdint>
#include <deque>
#include <future>
#include <random>
#include <vector>

#include "d3d9/device.h"
#include "shader/abi.h"
#include "vk/passes.h"
#include "vk/raytracing.h"

namespace rvk::enh {

class RtScene;

inline constexpr std::uint32_t kGiCascades = 3;
inline constexpr std::uint32_t kGiMaxLights = 32;
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
    std::uint32_t screen[4];   // screen probes: depth, G-buffer normal, grid width, height
    std::uint32_t screen2[4];  // tile size, rays per probe, scene width, height
    std::uint32_t screen3[4];  // probes image, previous resolve, G-buffer motion, linear sampler
    float screen4[4];          // ray reach, ratio cap, history blend, colour bleeding
};
static_assert(sizeof(GiPassParams) == 16 * (3 + 3 + 1 + 1 + 1 + 1 + 1 + 7 + kGiMaxLights * 2 + 4));

// mirror of gi_common.glsl GiPush
struct GiPush {
    shader::abi::PassPushHeader h;
    std::uint32_t params[2];
    std::uint32_t pad[2];
};
static_assert(sizeof(GiPush) == 32);

class Gi final : public d3d9::DrawObserver {
public:
    struct Settings {
        bool enabled = false;
        int quality = 1;          // 0 low, 1 medium, 2 high: probes traced per frame
        float strength = 1.0f;    // 0 = the engine's ambient .. 1 = GI
        bool replaceLightmaps = false;
        bool characters = true;   // skinned draws in the RT scene
        int debug = 0;            // 1 irradiance, 2 cascades
        float bleed = 0.5f;       // colour bleeding of the screen probes (rtgi.md 15): 0 luminance only .. 1 full chroma ratio
        int screen = 2;           // screen probes (stage C, rtgi.md 11): 0 off, 1 low, 2 medium, 3 high
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
    static bool variantOptions(const d3d9::Shader& s, shader::GlslOptions& o);

    // d3d9::DrawObserver: the frame's light constants (P0)
    bool observes(d3d9::draws::Kind kind, d3d9::scene::Phase phase) const override;
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
    bool ensureResources(VkCommandBuffer cmd);
    void destroyResources();
    void updateCascades(const float camera[3]);
    void queue(Cascade& c, std::uint32_t slot);
    void selectProbes(std::vector<std::uint32_t>& out, std::uint32_t budget);
    void pickLight();
    void logStats(std::uint32_t probes);
    // stage C: the screen probes after the z pre-pass (this frame's depth, the RT scene built there), with the last update's
    // light and volume
    void recordScreen(d3d9::PassContext& ctx);

    d3d9::Device& dev_;
    RtScene* scene_ = nullptr;
    Settings s_;
    bool on_ = false, failed_ = false;
    std::uint32_t passId_ = 0;
    vk::PassLayout layout_;
    VkPipeline trace_ = VK_NULL_HANDLE, blend_ = VK_NULL_HANDLE;
    std::future<VkPipeline> traceBuild_, blendBuild_;
    // the screen probes (set 1: one storage image)
    vk::PassLayout screenLayout_;
    VkPipeline spTrace_ = VK_NULL_HANDLE, spResolve_ = VK_NULL_HANDLE;
    std::future<VkPipeline> spTraceBuild_, spResolveBuild_;
    std::uint32_t sgiHistory_ = 0;  // the resolved image written last (0 / 1)
    GiPassParams lastParams_{};
    GiVolumeHeader lastHeader_{};
    bool haveLast_ = false;
    bool sgiValid_ = false;
    std::uint64_t screenFrames_ = 0, screenSkipped_ = 0;
    vk::rt::AddressBuffer probes_, depth_, rays_;
    bool cleared_ = false;
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
    std::vector<Light> frameLights_;

    // stats
    double lastLog_ = -1e9;
    std::uint64_t passes_ = 0, skipped_ = 0, probesTraced_ = 0, giDrawsBase_ = 0;
    std::uint64_t updatesTotal_ = 0;  // since enabled (the first update's log line)
};

}  // namespace rvk::enh
