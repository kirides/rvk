#pragma once

// Enhanced water (settings category "Water", default off; docs/research/water.md). A draw replacer on d3d9::Device:
//  * the engine's water draws (the Refracted pass, right after the refraction copy) keep the engine's geometry and its own
//    vertex shader (translated) and get the GLSL water pixel shader (shaders/water/water.frag), which evaluates the
//    engine's water material (its pixel shader's model: normal maps, diffuse textures, constants, lighting, depth fade;
//    enh/water_profile.h picks the model from the shader's sampler layout, unknown shaders keep the engine's draw) and adds
//    traced reflections, foam and the sea's waves on top;
//  * the open sea (the engine's world-spanning sea plane, found by its geometry) is replaced by a camera-centred wave grid
//    (water.ocean_waves) that hands the same pixel shader the engine ocean VS's interpolators: waves offshore and rolling
//    in towards the coast, a lower waterline with swash, surf foam, the beach above the swash darkened as wet sand (an
//    AfterOpaque pass), using the shore field baked from the engine's own coast overlay (enh/shore_field.h); that overlay
//    (the "fake shore", Special_Water_Shore_*) is then not drawn;
//  * waterfalls, glass, crystals and everything else keep the engine's shading.
// Off: the replacer answers "engine" for every draw; nothing else changes.

#include <array>
#include <cstdint>
#include <future>
#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "d3d9/device.h"
#include "enh/ocean.h"
#include "enh/sea_floor.h"
#include "enh/shore_field.h"
#include "enh/water_profile.h"
#include "shader/abi.h"
#include "vk/passes.h"

namespace rvk::enh {

// mirror of shaders/water/water_common.glsl WaterBlock (std430: this layout); starts with a DrawBlock (shader/abi.h RvkDraw)
struct WaterBlock {
    // = DrawBlock: the engine draw's texture / sampler slots (bindless), the half-pixel fix its vertex shader reads
    std::uint32_t tex[20];
    std::uint32_t smp[20];
    float posFix[4];
    std::uint32_t alphaFunc;
    float alphaRef;
    std::uint32_t tint;
    std::uint32_t pad1;
    // the engine draw's pixel constants
    float camFar[4];      // c0
    float fogColor[4];    // c3
    float fogParams[4];   // c4
    float mat[8][4];      // c6..c13 g_ShaderData
    float indoor[4];      // c14
    float sh[7][4];       // c15..c21 g_AmbientLight
    float sunColor[4];    // c22..c24 g_DirLight
    float sunSpec[4];
    float sunDir[4];
    // the sea grid
    float vsTc[6][4];     // vs c232..c237 (the ocean VS's texcoord transforms)
    float uvMapU[4];      // texcoord0 = affine (x, z) of the sea mesh
    float uvMapV[4];
    float meshFrame[4];   // tangent, binormal sign
    float meshExtra[4];   // COLOR1.w, global alpha
    // ours
    float ocean[4];
    float grid[4];
    float shoreMap[4];
    float look[4];
    float misc[4];
    std::uint32_t res[4];
    std::uint32_t res2[4];
    std::uint32_t smpOurs[4];
    float falls[4][4];
    float lights[12][4];
    std::uint32_t lightInfo[4];
    // the FFT ocean (enh/ocean.h) and the sea floor field (enh/sea_floor.h)
    float fftLength[4];      // cascade tile sizes, world units (0 = none)
    std::uint32_t fftDisp[4];   // bindless slots: displacement (dx, dy, dz m, foam)
    std::uint32_t fftDeriv[4];  // derivatives (d dy/dx, d dy/dz, d dx/dx, d dz/dz)
    float fftSlopeVar[4];    // slope variance of each cascade's band
    float fftBand[4];        // representative wavelength of each cascade (world units: attenuation)
    std::uint32_t fftInfo[4];   // x mode (0 Gerstner, 1 FFT), y cascades, z sampler slot, w N
    float seaInfo[4];        // x Hs (world units), y wind-sea peak wavelength (world units), z wind angle, w sea-state level
    float floorMap[4];       // floor field: xy corner (world xz), zw 1 / extent
    std::uint32_t floorTex[4];  // x field A, y field B (bindless), z valid
    float shoreWave[4];      // x period (s), y amplitude (world units), z breaker index, w unused
    float player[4];         // the player's position (eCWeatherAdmin), w 1 = known
    float foamMap[4];        // the foam buffer: xy its first cell (world), z its extent, w 1 / extent
    std::uint32_t foamTex[4];   // x the foam buffer (bindless t2D, 0 = none), y its sampler (linear, wrap)
};
static_assert(sizeof(WaterBlock) == 192 + 70 * 16);

// mirror of shaders/water/wet_shore.frag WetPush
struct WetShorePush {
    shader::abi::PassPushHeader h;
    std::uint32_t depthTex, fieldTex, smpLinear, smpPoint;
    float shoreMap[4];
    float ocean[4];
    float params[4];         // time, wet height, darkening, swash period (0 = the M7 swash)
    std::uint32_t sky[4];    // the sky cube (tCube slot), its sampler
};
static_assert(sizeof(WetShorePush) == 96 && sizeof(WetShorePush) <= shader::abi::kPushConstantBudget);

class Water final : public d3d9::DrawReplacer {
public:
    struct Settings {
        bool enabled = false;
        int quality = 1;           // 0 low, 1 medium, 2 high
        bool ssr = true;           // water.reflections: ssr (else the sky cube only)
        bool oceanWaves = true;    // the wave grid for the open sea + lower coast (else the sea is shaded on its own mesh)
        int ocean = 2;             // water.ocean: 0 off (= !oceanWaves), 1 Gerstner (M7), 2 FFT
        int oceanQuality = 1;      // water.ocean_quality: 0 low, 1 medium, 2 high (FFT tiles)
        int seaState = -1;         // water.sea_state: -1 from the weather, 0 calm .. 3 storm
        int windDirection = -1;    // water.wind_direction: degrees, -1 = the prevailing wind
        float waveHeight = 1.0f;   // water.wave_scale: multiplier
        float murkiness = 1.0f;    // multiplier on the absorption
        float reflections = 1.0f;  // multiplier on reflectivity + glint
        int debug = 0;             // water.debug: 0 off, 1 shore field, 2 depth, 3 normals, 4 reflection, 5 foam, 6 sea floor, 7 waves
    };

    explicit Water(d3d9::Device& device);
    ~Water() override;
    Water(const Water&) = delete;
    Water& operator=(const Water&) = delete;

    // render thread (settings callbacks run at the frame boundary)
    void apply(const Settings& s);
    const Settings& settings() const { return settings_; }

    void replace(d3d9::Device& device, const d3d9::ReplaceQuery& q, d3d9::DrawReplacement& out) override;
    // the sea floor field: the main view's static meshes are rasterised top-down (once per mesh and placement)
    bool wantsOpaque() const override;
    void observeOpaque(d3d9::Device& device, const d3d9::ReplaceQuery& q) override;

private:
    struct Body {
        bool sea = false;  // the world-spanning sea plane
        float minY = 0.0f, maxY = 0.0f, level = 0.0f;
        float extentX = 0.0f, extentZ = 0.0f;
        std::uint32_t vertices = 0;
        // the mesh's texture mapping and frame (the wave grid reproduces the sea mesh's interpolators)
        bool mapped = false;
        float uvU[3]{}, uvV[3]{};     // texcoord0 = affine (x, z)
        float tangent[3]{1.0f, 0.0f, 0.0f};
        float binormalSign = 1.0f;
        float outdoor = 1.0f;         // COLOR1.w
    };
    bool ensureShaders();
    void destroyShaders();
    // the wet beach (AfterOpaque pass, wet_shore.frag) while the wave grid lowers the waterline
    void updateWetPass();
    bool ensureWetPipeline(VkFormat target);
    void recordWet(d3d9::PassContext& ctx);
    void destroyWetPipeline();
    const Body& body(d3d9::Device& device, const d3d9::ReplaceQuery& q);
    void collectCoast(d3d9::Device& device, const d3d9::ReplaceQuery& q);
    void serviceBake(d3d9::Device& device);
    void prebuild();
    bool worldRows(d3d9::Device& device, const d3d9::ReplaceQuery& q, float rows[3][4]) const;
    VkDeviceAddress fillBlock(d3d9::Device& device, const d3d9::ReplaceQuery& q, d3d9::draws::Water kind, WaterModel model, bool grid,
                              const Body& b, std::array<d3d9::Texture*, 16>& sampled);
    void measureMapping(const d3d9::ReplaceQuery& q, const float rows[3][4], const std::uint8_t* data, std::uint32_t count, Body& b);
    // the FFT ocean: its simulation pass (AfterOpaque), the sea state (setting or weather), the sea floor field
    bool fftWanted() const { return settings_.enabled && settings_.ocean == 2 && !failed_; }
    void updateOceanPass();
    void recordOcean(d3d9::PassContext& ctx);
    void updateSea(double seconds);
    // the waves rolling in (WaterBlock::shoreWave xyz): period (s), amplitude (world units), breaker index
    std::array<float, 3> shoreWave() const;
    void serviceFloor(d3d9::Device& device);
    void saveFloor();

    d3d9::Device& dev_;
    Settings settings_;
    bool failed_ = false;
    std::shared_ptr<d3d9::Shader> oceanVs_, ps_;
    std::unordered_set<std::uint64_t> meshVsRegistered_;  // engine water VS hashes whose pipelines with ps_ are prebuilt
    std::unordered_set<std::uint64_t> loggedModels_;      // engine PS hashes logged (model or "not a known water shader")
    std::unordered_map<std::uint64_t, Body> bodies_;  // by vertex buffer id
    float fallCentre_[2]{};                           // xz box centre of the body measured last (body())
    // the coast (the engine's shore overlay meshes): triangles in world space, baked into the shore field
    std::unordered_set<std::uint64_t> coastMeshes_;    // vertex buffer id ^ index buffer id << 1 seen
    std::vector<shore::Triangle> coast_;
    bool coastDirty_ = false;
    std::future<shore::Field> bake_;
    std::unique_ptr<d3d9::Texture> field_;
    std::array<float, 4> fieldMapping_{};
    // waterfalls seen (Alpha-pass water of kind Waterfall): base centre, radius, base height; by vertex buffer id
    struct Fall {
        float x, z, radius, baseY;
    };
    std::unordered_map<std::uint64_t, Fall> falls_;
    std::uint32_t wetPassId_ = 0;
    vk::PassLayout wetLayout_;
    VkPipeline wetPipeline_ = VK_NULL_HANDLE;
    VkFormat wetFormat_ = VK_FORMAT_UNDEFINED;
    bool wetFailed_ = false;
    float seaLevel_ = 0.0f;
    bool seaKnown_ = false;  // the sea plane was measured (seaLevel_ is its height)
    std::uint64_t lastGridFrame_ = 0;      // device frame of the last sea grid (0 = none yet)
    std::uint64_t wetFrames_ = 0;
    std::uint64_t gridFrame_ = ~0ull;      // device frame whose sea grid was drawn
    std::uint32_t gridMainView_ = 0;
    std::uint64_t replacedDraws_ = 0, skippedDraws_ = 0;
    // the FFT ocean
    std::unique_ptr<Ocean> ocean_;
    std::uint32_t oceanPassId_ = 0;
    float seaLevelState_ = 1.0f;     // smoothed sea-state level (0 calm .. 3 storm)
    float weatherLevel_ = 1.0f;
    bool haveSeaLevel_ = false, haveWeather_ = false, loggedWeather_ = false;
    double lastSeaSeconds_ = -1.0, lastWeatherRead_ = -1e9;
    std::uint64_t lastSimFrame_ = 0;
    float player_[4]{};              // the player's position (engine::playerPosition), [3] = 1 when known this frame
    bool loggedPlayer_ = false;
    shore::Field overlayField_;      // the M7 shore field (the coast overlay), kept for the floor bake
    // the sea floor field (enh/sea_floor.h): the static meshes seen (keys), triangles waiting for the worker, the heights
    // (owned by the worker while a bake runs), the textures
    struct FloorState {
        seafloor::Heights heights;
        bool loaded = false;
        std::size_t unsaved = 0;
        double lastSave = 0.0;
    };
    std::unordered_set<std::uint64_t> floorMeshes_;
    std::vector<shore::Triangle> floorPending_;
    std::shared_ptr<FloorState> floor_;
    std::future<seafloor::Field> floorBake_;
    std::unique_ptr<d3d9::Texture> floorA_, floorB_;
    std::array<float, 4> floorMapping_{};
    double lastFloorBake_ = -1e9;
    std::uint64_t floorFrame_ = 0, floorFrameBytes_ = 0, floorMeshCount_ = 0;
    bool floorDirty_ = false;
    bool loggedFirst_ = false, loggedNoInputs_ = false, prebuilt_ = false, pipelinesReady_ = false;
};

}  // namespace rvk::enh
