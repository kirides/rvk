#pragma once

// The Full-owned light table and its cluster grid (setting lighting.clustered, docs/research/gpu-scene.md 9.7).
//
// Every frame (main view, injection point AfterDepthPrepass) the world's lights (engine/lights: the cached-in static lights and the registered dynamic
// ones) are written into a CPU upload buffer, and one compute pass (shaders/lighting/light_cluster.comp.hlsl) copies them into the device buffer the
// material shaders read, builds a view-space cluster grid (16 x 9 x 24, exponential slices) and writes each cluster's light list. The material pixel
// shader variants (enh/material_variant RVK_MV_CLUSTER, shaders/include/rvk_light_model.hlsli) then fill the engine's point-light registers per PIXEL
// from the cluster instead of per object: lights beyond the engine's per-object reach (<= 2 static + 2 dynamic, 3 per pass) light the surfaces they
// reach, the engine's own shader evaluates them (falloff, N.L, colour, ambient SH, sun, encode: the engine's look). `lighting.style` picks the distance law
// (original = the engine's, physical = inverse square window); `lighting.debug` shows the grid. Nothing exists while the setting is off; the engine's
// per-draw lists and registers are never changed (the variant only reads them).
//
// Stage (b) (GPU-driven draws that consume the table instead of engine lists) and stage (c) (point-light shadow cubes) build on the same buffer.

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <future>
#include <memory>
#include <string>
#include <vector>

#include "d3d9/device.h"
#include "engine/lights.h"
#include "rhi/rhi.h"
#include "shader/abi.h"

namespace rvk::enh {

// registryGeneration's bookkeeping (pure state transitions; LightTable::record drives them, tests call them directly)
void registryNoteComplete(bool complete);             // bumps the generation when the flag changes
void registryNoteKnown(std::uint64_t positionsHash);  // bumps the generation when the light set's position hash differs from the previous build's
std::uint64_t registryPositionHash(const float (*positions)[4], std::uint32_t count);  // order-independent: a sum of a mix of each position's x / y / z (the 4th float is ignored)
void registryResetForTest();

inline constexpr std::uint32_t kLightMax = 1024;               // RVK_LIGHT_MAX
inline constexpr std::uint32_t kLightBytes = 64;               // RVK_LIGHT_BYTES
inline constexpr std::uint32_t kLightHeaderBytes = 128;        // RVK_LIGHT_HEADER_BYTES
inline constexpr std::uint32_t kLightClusterBytes = 256;       // RVK_LIGHT_CLUSTER_BYTES
inline constexpr std::uint32_t kLightMaxPerCluster = 63;       // RVK_LIGHT_MAX_PER_CLUSTER
inline constexpr std::uint32_t kLightGridX = 16, kLightGridY = 9, kLightGridZ = 24;
inline constexpr float kLightNear = 20.0f, kLightFar = 16000.0f;  // the grid's depth range (engine units)
// the far level (gpu-scene.md 9.10): beyond kLightFar a coarser-in-depth, finer-in-tiles grid up to kLightFar1; lights are collected over that range
inline constexpr float kLightFar1 = 96000.0f;
inline constexpr std::uint32_t kLightGrid1X = 32, kLightGrid1Y = 18, kLightGrid1Z = 8;

// mirror of shaders/include/rvk_light_model.hlsli RvkLight (64 bytes)
struct GpuLight {
    float posRcp[4];     // xyz, 1 / R^2
    float colRange[4];   // rgb, R
    float specFlags[4];  // rgb specular, flags (uint bits)
    float misc[4];       // id (uint bits), 0, 0, 0
};
static_assert(sizeof(GpuLight) == kLightBytes);

// mirror of light_cluster.comp.hlsl LightClusterPush
struct LightClusterPush {
    shader::abi::PassPushHeader h;
    std::uint32_t src[2];
    std::uint32_t dst[2];
    std::uint32_t grid[4];
    float zs[4];
    std::uint32_t offs[4];
    std::uint32_t shadow[4];
    std::uint32_t grid1[4];
    float zs1[4];
};
static_assert(sizeof(LightClusterPush) == 128 && sizeof(LightClusterPush) <= shader::abi::kPushConstantBudget);

// mirror of shaders/lighting/light_shadow_common.hlsli LightShadowPush (the ray-traced lamp shadow passes)
struct LightShadowPush {
    shader::abi::PassPushHeader h;
    std::uint32_t tex[4];   // depth map slot, raw entries slot, width, height
    float cfg[4];           // lamp radius (units), ray reach, origin bias, rays per light
    std::uint32_t misc[4];  // frame index, RVK_RT_TRACE_* flags, debug, 0
    std::uint32_t table[2];  // last: HLSL's constant buffer packing puts every 4-component member on a 16-byte boundary (D3D12 root constants)
};
static_assert(sizeof(LightShadowPush) <= shader::abi::kPushConstantBudget);
static_assert(offsetof(LightShadowPush, tex) % 16 == 0 && offsetof(LightShadowPush, cfg) % 16 == 0 && offsetof(LightShadowPush, misc) % 16 == 0,
              "a push block's 4-component members must sit on 16-byte boundaries (D3D12 root constants pack that way)");
inline constexpr std::uint32_t kLightShadowMax = 31;  // RVK_LIGHT_SHADOW_MAX

// pure cluster math (rvk_tests; the same formulas as the shader)
float lightSliceScale(float nearZ, float farZ, std::uint32_t slices);                    // slices / ln(far / near)
std::uint32_t lightSlice(float viewZ, float nearZ, float slicesPerLn, std::uint32_t slices);
float lightSliceNear(std::uint32_t slice, float nearZ, float slicesPerLn);               // the view depth where `slice` begins
// the physical distance law relative to the engine's (rvkLmPhysicalRatio)
float lightPhysicalRatio(float d2, float rcpSqrRange);
// the lamp shadow image's entries (light_shadow_common.hlsli lsEncode / lsId / lsVis): shadow id 1..31 + q / 64, q = visibility in 1 / 63 steps
float lightShadowEncode(std::uint32_t id, float vis);
std::uint32_t lightShadowId(float entry);
float lightShadowVis(float entry);
// the engine's falloff 1 - sat(d^2 / R^2)
float lightEngineFalloff(float d2, float rcpSqrRange);

// Full-owned lighting (setting lighting.full_lit, gpu-scene.md 9.8): lit draws are composed with one 3-light permutation and take ALL their point
// lights from the table. Set by HalDevice::applyLighting (needs the table on and no compare run); read by native::StaticOps.
bool fullLitActive();
// WS13 (frame-ownership.md 14.55): a build with the inputs of the last one keeps its table + upload; default on, `debug lightskip 0|1` (overlay command) switches it inside one run
// (perf-ab.ps1 -CommandTemplate), RVK_LIGHTS_SKIP_OFF=1 at start
std::atomic<bool>& lightBuildSkip();
void setFullLitActive(bool on);
// the two table lighting options (lighting.trust_table / lighting.tree_merge), meaningful while fullLitActive()
bool fullLitSkipsReads();   // lighting.trust_table: lit draws skip their own point-light reads when the table verifiably holds every light of the object
bool fullLitTreeMerge();
void setFullLitOptions(bool skipReads, bool treeMerge);
// WS22 (frame-ownership.md 16.8): the static half of the light collection on a worker thread; 0 off, 1 on, 2 auto (default: on, except on integrated GPUs); `debug lightworker 0|1|2`
// (overlay command, perf-ab.ps1 -CommandTemplate), RVK_LIGHT_WORKER=0|1 at start
void setLightCollectWorker(int mode);

class RtScene;

class LightTable {
public:
    struct ShadowSettings {
        int mode = 0;            // lighting.light_shadows: 0 off, 1 nearby (8 lights within 25 m), 2 many (31 within 60 m)
        int quality = 2;         // 1 low (cut-outs solid), 2 medium, 3 high (2 rays per light)
        float radius = 20.0f;    // the lamps' size (world units): the soft shadow's disc
        float baked = 0.4f;      // lighting.light_shadow_baked: the share of the baked light a traced shadow takes away (frame param z)
    };
    // A drawn object's light (its list entry: reg0 = position + 1 / R^2, reg1 = colour), handed over by native::StaticOps for lists it composed. The next
    // build adds those the registry walk did not return (within 8 units = the same light). Thread-safe.
    // true while the registry walk returns no static lights (nothing cached in): the drawn objects' static lights are then taken as a source too
    static bool wantStaticNotes();
    // true while the registry walk returned lights (static tree within reach + this frame's dynamic lights, no latency) and the table is built every frame:
    // the registry is trusted whole. With an EMPTY registry the objects' own lights are still read (a teleport: nothing cached in), objects without any skip
    static bool registryComplete();
    // true when a light of the last build (any level: the far lights too) sits within 8 units of `pos`: the table lights an object's own light there
    // per pixel, so the object's batch need not end on that light (native::planTreeBatches' table merge)
    static bool lightKnown(const float pos[3]);
    static std::uint64_t knownSerial();  // WS11: changes whenever the grid lightKnown reads is replaced
    // F2c-1: a counter bumped whenever what `lightKnown` / `registryComplete` depend on changes (the "registry complete" flag flips, or the set of the last build's
    // light positions differs from the previous build's): a retained record that took its `holdsAll` decision from them stays valid while the value is the same
    static std::uint64_t registryGeneration();
    // authoritative = the note comes from a fully composed frame (no owned view): only those update the colour a static registry light takes (frame-ownership.md 16.10)
    static void noteDrawnLight(const float pr[4], const float col[4], bool authoritative = true);

    explicit LightTable(d3d9::Device& device);
    ~LightTable();
    LightTable(const LightTable&) = delete;
    LightTable& operator=(const LightTable&) = delete;

    bool available() const { return !failed_; }
    // run time: on/off (the pass exists only while on), style 0 original / 1 physical, debug 0 off / 1 cluster counts / 2 slots filled
    void configure(bool enabled, int style, int debug, float defaultRoughness = 0.7f);
    // ray-traced lamp shadows (needs the RT scene; the table must be on): the nearest lights get a shadow id, the pass light.shadow traces them
    void setRtScene(RtScene* scene) { scene_ = scene; }
    void configureShadows(const ShadowSettings& s);
    void setCollectRadius(float r) { collectRadius_ = r; }
    // lighting.occupancy: 0 off (every pixel takes the full cluster path), 1 on (pixels in a cluster without lights take the short path), 2 compare (Vulkan:
    // the short path's state is checked against the full path for every empty-cluster pixel; mismatches are counted into the compare buffer, logged)
    void setOccupancy(int mode);
    // lighting.cluster_build (WS21, gpu-scene.md 9.16): 0 columns (one group per tile column, default), 1 threads (the legacy one thread per cluster kernel), 2 compare
    // (Vulkan: the columns' build, then the legacy kernel reads it and counts differing clusters / occupancy words; rvk.log "cluster build compare", must be 0)
    void setClusterBuild(int mode);

private:
    void record(d3d9::PassContext& ctx);
    void recordShadow(d3d9::PassContext& ctx);
    void recordShadowFilter(d3d9::PassContext& ctx);
    bool pipelineReady();
    bool shadowPipelinesReady();
    void applyShadowHold();
    void logStats();

    d3d9::Device& dev_;
    bool enabled_ = false, failed_ = false;
    int style_ = 0, debug_ = 0;
    float refA_ = 0.0f, refB_ = 0.0f;  // the light table reference words of frame param kFrameParamLights (x, y)
    float collectRadius_ = 96000.0f;
    double colourRatio_[3]{};
    std::uint64_t colourRatioN_ = 0;
    int occMode_ = 1;
    std::unique_ptr<rhi::Buffer> occCmp_;  // compare counters (uint words: 0 mismatches, 1 empty-cluster pixels checked, 2 occupancy bit wrong, 3 first mismatch kind bits)
    double occLog_ = -1e9;
    int clusterMode_ = 0;
    std::unique_ptr<rhi::Pipeline> legacyPipe_, comparePipe_;  // the legacy kernel and its compare variant (built when the mode asks)
    std::future<std::unique_ptr<rhi::Pipeline>> legacyBuilding_, compareBuilding_;
    std::unique_ptr<rhi::Buffer> clusterCmp_;  // compare counters (uint words: 0 clusters differing, 1 compared, 2 occupancy words differing, 3 builds compared, 4 highest differing cluster + 1)
    double clusterLog_ = -1e9;
    std::uint64_t clusterPasses_ = 0;
    float defaultRoughness_ = 0.7f;  // the physical style's roughness of surfaces without a pack entry
    std::uint32_t passId_ = 0;
    std::unique_ptr<rhi::Pipeline> pipeline_;
    std::future<std::unique_ptr<rhi::Pipeline>> building_;
    std::unique_ptr<rhi::Buffer> device_;                                          // header + lights + clusters
    std::array<std::unique_ptr<rhi::Buffer>, rhi::kFramesInFlight> upload_;        // the CPU's lights, per frame in flight
    std::vector<engine::lights::WorldLight> scratch_;
    std::vector<GpuLight> prev_, cur_;
    std::uint32_t lastDrawnAdded_ = 0;
    engine::lights::Stats stats_;
    // ray-traced lamp shadows
    RtScene* scene_ = nullptr;
    ShadowSettings shadow_;
    bool shadowHeld_ = false, shadowFailed_ = false;
    std::uint32_t shadowPassId_ = 0, shadowLights_ = 0;
    std::unique_ptr<rhi::Pipeline> shadowPipeline_[2];
    std::future<std::unique_ptr<rhi::Pipeline>> shadowBuilding_[2];
    struct PendingShadow {
        std::uint64_t frame = ~0ull;
        LightShadowPush push{};
        rhi::Texture* raw = nullptr;
        rhi::Texture* fin = nullptr;
        std::uint32_t gx = 0, gy = 0;    // the filter's dispatch (full resolution)
        std::uint32_t gxT = 0, gyT = 0;  // the trace's dispatch (the raw image: half resolution when the experiment bit is on)
    } pendingShadow_, pendingFilter_;
    std::uint64_t shadowPasses_ = 0, shadowSkipped_ = 0;
    std::uint64_t passes_ = 0, skipped_ = 0, changedTotal_ = 0;
    std::uint32_t lastCount_ = 0, lastChanged_ = 0, lastDropped_ = 0, lastNear_ = 0, lastNearStatic_ = 0, lastNearAny_ = 0;
    double lastLog_ = -1e9;
    void verifyReference(const float cam[3]);
    bool workerAllowed() const;
    std::vector<engine::lights::WorldLight> compareScratch_;  // the check builds' inline collection (WS22)
    engine::lights::Stats compareStats_;
    std::uint64_t workerBuilds_ = 0, workerCompared_ = 0, workerDiffering_ = 0;
    std::string workerFirst_;
    std::uint32_t backstopCounter_ = 0;                                  // builds seen: every 64th compares the statics with a fresh uncached walk
    std::uint64_t backstopChecks_ = 0, backstopMismatch_ = 0, backstopMismatchLogged_ = 0;
    double backstopUs_ = 0.0;
    std::uint64_t backstopWindowMismatch() { const std::uint64_t d = backstopMismatch_ - backstopMismatchLogged_; backstopMismatchLogged_ = backstopMismatch_; return d; }
    bool verify_ = false;  // RVK_LIGHTS_VERIFY=1
    std::vector<engine::lights::WorldLight> verifyDyn_, verifyDrawn_;
    std::uint64_t verifyBuilds_ = 0, verifyDiffering_ = 0;
    std::string verifyFirst_;
    std::uint64_t knownGridHash_ = 0;
    std::vector<std::uint32_t> shadowIdScratch_;
    std::vector<std::pair<float, std::uint32_t>> nearLightsScratch_;
    struct ReachKey {
        float reach;
        std::uint32_t index;
    };
    std::vector<ReachKey> reachKeys_;
    std::vector<engine::lights::WorldLight> truncated_;
    double cpuMs_[6]{};  // record()'s CPU per section, summed over cpuN_ builds (logged every 5 s)
    std::uint64_t cpuN_ = 0;
    // WS13: unchanged builds keep the table + upload (record())
    static constexpr std::uint32_t kFullEvery = 64;
    std::uint64_t buildSig_ = 0, buildSerial_ = 1;
    bool haveSig_ = false;
    std::uint32_t sinceFull_ = 0;
    std::array<std::uint64_t, rhi::kFramesInFlight> uploadSerial_{};
    std::uint64_t skipHits_ = 0, skipChecks_ = 0, skipDiffering_ = 0, uploadsSkipped_ = 0;
    std::string skipFirst_;
};

}  // namespace rvk::enh
