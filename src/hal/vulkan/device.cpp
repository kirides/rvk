// eCAPIDevice on d3d9::Device (docs/research/hal-structs.md sections 2.x).

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>
#include <cstdlib>
#include <filesystem>
#include <format>
#include <string>

#include <spdlog/spdlog.h>

#include "api/capture.h"
#include "core/caps.h"
#include "core/crash_dump.h"
#include "core/exports.h"
#include "core/paths.h"
#include "core/settings.h"
#include "core/stall_sampler.h"
#include "d3d9/fvf.h"
#include "enh/ocean.h"
#include "engine/scene_hooks.h"
#include "engine/fast_sort.h"
#include "engine/load_accel.h"
#include "engine/lod.h"
#include "engine/scene_table.h"
#include "native/f10_collect.h"
#include "native/f10_request.h"
#include "native/f10_extract.h"
#include "gpuscene/instance_buffer.h"
#include "gpuscene/illumination_table.h"
#include "gpuscene/lighting_images.h"
#include "gpuscene/mesh_parts.h"
#include "gpuscene/window_tables.h"
#include "gpuscene/draw_record_table.h"
#include "gpuscene/owned_compact.h"
#include "gpuscene/cluster_table.h"
#include "gpuscene/view_table.h"
#include "gpuscene/billboard_table.h"
#include "gpuscene/object_table.h"
#include "gpuscene/row_table.h"
#include "gpuscene/material_table.h"
#include "gpuscene/shutdown.h"
#include "gpuscene/verify.h"
#include "engine/xinput_throttle.h"
#include "engine/memread.h"
#include "engine/material_source.h"
#include "engine/menu_state.h"
#include "core/perf_window.h"
#include "engine/shadow_maps.h"
#include "engine/vfs.h"
#include "engine/window_mode.h"
#include "enh/material_variant.h"
#include "hal/vulkan/hal_vulkan.h"
#include "res/dds.h"
#include "rhi/d3d_formats.h"

namespace rvk::hal::vulkan {

using namespace rvk::d3d9;

namespace {
// Values that used to be player / developer settings (docs/research/settings-audit.md, the clean-up of 2026-11): art coefficients
// frozen at their shipped defaults, and the finished A/B switches' outcome. Change the number here to tune.
namespace frozen {
constexpr float kWaterMurkiness = 1.0f, kWaterReflection = 1.0f;
constexpr float kShadowSoftness = 1.0f, kShadowBlend = 0.2f, kShadowFade = 0.35f, kShadowSplit = 0.4f;
constexpr float kRtShadowSoftness = 1.0f;
constexpr bool kRtShadowWind = true;
constexpr float kRtLodRange = 60.0f, kRtLodFarRange = 150.0f;
constexpr int kRtLodBias = 2, kRtLodBiasLow = 3;  // the ladder runs only with rt.lod_scale > 0 (the Low preset): that gets the stronger bias
constexpr float kGiBleed = 0.5f, kGiDirectional = 0.5f, kGiShadowFill = 0.5f, kGiEmissive = 1.0f;
constexpr int kGiLightCount = 128;
constexpr bool kGiCharacters = true, kGiCutoutsTested = true;
constexpr int kGiLightsWorld = 1;
constexpr float kAoRadiusMetres = 1.0f;
constexpr float kLightCollectRadius = 96000.0f, kLampRadiusCm = 20.0f, kLampShadowBaked = 0.4f;
constexpr bool kTrustTable = false, kTreeMerge = true;
constexpr float kMaterialSpecular = 1.0f, kMaterialEnvironment = 1.0f, kMaterialRtStrength = 1.0f, kMaterialRoughness = 0.7f;
constexpr float kDeriveMin = 0.25f, kDeriveMax = 0.9f, kPuddleShare = 0.7f;  // puddles = materials.rain_wetness x 0.7
constexpr float kReactiveCutoff = 0.08f, kReactiveScale = 1.0f;
// the Full-mode CPU optimisations (native.*): the finished ones always on, the dead ends off; the code stays behind these constants
constexpr int kMirrorMode = 0, kGpuScene = 0;
constexpr unsigned kCpuOpts = 255u;
constexpr bool kLeanRestore = true, kLeanEffects = true, kRetained = false, kRetainedCheck = false;
constexpr int kRetainedGroups = 0x2ff;
constexpr int kReadCacheKiB = 64, kAbsorbMode = 1;
}  // namespace frozen

std::atomic<bool> g_logFrame{false};
}  // namespace

bool logThisFrame() { return g_logFrame.load(std::memory_order_relaxed); }

// -LogFrame: every key HAL call of the frame, prefixed with the frame phase (view/pass, engine/scene_hooks.h)
#define HAL_LOG(fmt, ...)                                                                                     \
    do {                                                                                                      \
        if (logThisFrame()) spdlog::info("[{}] " fmt, engine::scene::currentLabel() __VA_OPT__(, ) __VA_ARGS__); \
    } while (0)

void HalDevice::updateLogFrame() {
    g_logFrame = options_.logFrame != 0 && dev_.stats().frames + 1 == options_.logFrame;
}

namespace {

// the engine's CPU-only DX9 functions (D3DX preprocessing / effect compilation / aux buffers) called on a stand-in `this`
template <class Fn>
Fn dx9(const char* name) {
    static Fn fn = nullptr;
    if (fn == nullptr) {
        fn = exports::resolveAs<Fn>(L"Engine.dll", name);
    }
    return fn;
}

}  // namespace

namespace {
std::mutex g_liveDevicesMutex;
std::vector<const HalDevice*> g_liveDevices;
}  // namespace

bool HalDevice::isLive(const HalDevice* device) {
    const std::lock_guard lock(g_liveDevicesMutex);
    return std::ranges::find(g_liveDevices, device) != g_liveDevices.end();
}

HalDevice::HalDevice() {
    {
        const std::lock_guard lock(g_liveDevicesMutex);
        g_liveDevices.push_back(this);
    }
    // eCDX9Device::CompileEffect* return 0 when the IDirect3DDevice9 at +0x10 is null; nothing else of `this` is used
    dx9Standin_[2] = 1;  // +0x10
}

void HalDevice::registerTexture(const void* api, d3d9::Texture* texture) {
    const std::lock_guard lock(apiTextureMutex_);
    apiTextures_[api] = texture;
    apiTextureGeneration_.fetch_add(1, std::memory_order_release);
}

void HalDevice::unregisterTexture(const void* api) {
    const std::lock_guard lock(apiTextureMutex_);
    apiTextures_.erase(api);
    apiTextureGeneration_.fetch_add(1, std::memory_order_release);
}

d3d9::Texture* HalDevice::textureOfApi(const void* api) {
    if (api == nullptr) return nullptr;
    const std::lock_guard lock(apiTextureMutex_);
    const auto it = apiTextures_.find(api);
    return it != apiTextures_.end() ? it->second : nullptr;
}

void HalDevice::refractionCopied(const void* api) {
    if (frameTap_) frameTap_->refractionCopy(api);
}

// The render mode of this Vulkan session (renderer.mode; core/config.h): Full = the native frame takes over every draw,
// Wrapper = the engine's draws through the HAL. "original" (requested for the next start) renders as Wrapper until then.
RenderMode HalDevice::renderMode() const {
    const auto& rs = settings::renderer();
    const auto requested = rs.rendererMode != nullptr ? static_cast<RenderMode>(rs.rendererMode->asInt()) : kDefaultRenderMode;
    return vulkanSessionMode(requested);
}

void HalDevice::applyRenderMode() {
    const auto& rs = settings::renderer();
    const auto requested = rs.rendererMode != nullptr ? static_cast<RenderMode>(rs.rendererMode->asInt()) : kDefaultRenderMode;
    const RenderMode mode = vulkanSessionMode(requested);
    if (mode != loggedMode_ || requested != loggedRequest_) {
        if (requested == RenderMode::Original) {
            spdlog::info("renderer: mode original requested (the game's D3D9 renderer after a restart); this session renders as {}",
                         toString(mode));
        } else {
            spdlog::info("renderer: mode {} ({})", toString(mode),
                         mode == RenderMode::Full ? "the native frame draws everything from engine data" : "the engine's draws through the D3D9 model on Vulkan");
        }
        loggedMode_ = mode;
        loggedRequest_ = requested;
    }
    applyStaticOps();
}

void HalDevice::applyStaticOps() {
    const auto& rs = settings::renderer();
    const auto on = [](const settings::Setting* s) { return s != nullptr && s->asBool(); };
    // Full turns the static and scene ops on; the Debug overrides native.static_ops / native.scene_ops do so in Wrapper too
    const NativePlan plan = nativePlan(renderMode(), on(rs.nativeStaticOps), on(rs.nativeStaticOpsCompare), on(rs.nativeSceneOps),
                                       on(rs.nativeSceneOpsCompare),
                                       rs.nativeSceneOpsGroups != nullptr ? rs.nativeSceneOpsGroups->asInt() : static_cast<int>(kAllSceneOpGroups));
    engine::scene::setSkyCubeFullMode(renderMode() == RenderMode::Full);
    // frame-ownership.md 7.47: native.clusters / cluster_cull / cluster_cull_shadows are RESTART settings: retained draws and op records keep the index buffers (the cluster arena's copies or
    // the engine's) they were made with, so a run-time toggle could not take effect (the stage line showed `armed, nothing owned`). The session runs with the start values
    const auto onStart = [](const settings::Setting* s) { return s != nullptr && s->startValue() != 0.0; };
    for (const settings::Setting* s : {rs.nativeClusters, rs.nativeClusterCull, rs.nativeClusterCullShadows}) {
        if (s != nullptr && s->value() != s->startValue()) {
            static std::unordered_map<const settings::Setting*, double> told;
            const auto it = told.find(s);
            if (it == told.end() || it->second != s->value()) {
                told[s] = s->value();
                spdlog::info("native: {} was changed to {} at run time: not applied, it takes effect at the next start (a restart setting: retained draws keep their index buffers); this session runs with {}",
                             s->spec().key, s->value() != 0.0 ? "true" : "false", s->startValue() != 0.0 ? "true" : "false");
            }
        }
    }
    dev_.setClusters(onStart(rs.nativeClusters));  // F5-1: static meshes clustered (any mode: the draws are the D3D9 model's)
    dev_.setClusterCull(onStart(rs.nativeClusterCull) && onStart(rs.nativeClusters));  // F5-2: the GPU cull of the owned draws of clustered meshes
    dev_.setClusterCullShadows(onStart(rs.nativeClusterCullShadows) && onStart(rs.nativeClusterCull) && onStart(rs.nativeClusters) && on(rs.nativeOwnedShadows));  // F3-2: ... also in the sun cascades
    const bool takeOver = plan.staticTake;
    const bool compare = plan.staticCompare;
    staticOps_->setMode(native::StaticOps::Group::Static, takeOver, compare);
    // N5.2 scene ops (native.scene_ops*): groups 1 alpha / refracted mesh ops, 2 entity overlay passes, 4 decals, 8 sky, 16 billboards,
    // 32 sky cube faces, 64 skinned actors, 128 SpeedTrees + tree billboards, 256 vegetation, 512 shadow views, 1024 effect draws
    const bool sceneTake = plan.sceneTake;
    const bool sceneCompare = plan.sceneCompare;
    const int groups = static_cast<int>(plan.groups);
    staticOps_->setMode(native::StaticOps::Group::Mesh, sceneTake && (groups & 1) != 0, sceneCompare && (groups & 1) != 0);
    staticOps_->setOverlays((sceneTake || sceneCompare) && (groups & 2) != 0);
    staticOps_->setMode(native::StaticOps::Group::Decals, sceneTake && (groups & 4) != 0, sceneCompare && (groups & 4) != 0);
    // row 13 (frame-ownership.md 14.26): the decal records' persistent geometry (Full, the decal group taken over)
    staticOps_->setOwnedDecals(rs.nativeOwnedDecals != nullptr && renderMode() == RenderMode::Full && sceneTake && (groups & 4) != 0 ? static_cast<int>(rs.nativeOwnedDecals->asInt()) : 0);
    // N5.2c: 8 the sky dome op, 16 the opaque billboards' queue
    staticOps_->setMode(native::StaticOps::Group::Sky, sceneTake && (groups & 8) != 0, sceneCompare && (groups & 8) != 0);
    staticOps_->setMode(native::StaticOps::Group::Billboards, sceneTake && (groups & 16) != 0, sceneCompare && (groups & 16) != 0);
    staticOps_->setMode(native::StaticOps::Group::SkyCube, sceneTake && (groups & 32) != 0, sceneCompare && (groups & 32) != 0);
    // N5.3: 64 the skinned actors' ops
    staticOps_->setMode(native::StaticOps::Group::Actors, sceneTake && (groups & 64) != 0, sceneCompare && (groups & 64) != 0);
    // N5.4: 128 the SpeedTree + tree billboard ops, 256 the vegetation ops
    staticOps_->setMode(native::StaticOps::Group::Trees, sceneTake && (groups & 128) != 0, sceneCompare && (groups & 128) != 0);
    staticOps_->setMode(native::StaticOps::Group::Vegetation, sceneTake && (groups & 256) != 0, sceneCompare && (groups & 256) != 0);
    // N5.5: 512 the shadow maps' z ops (the post_SSM shadow mask is a later stage)
    staticOps_->setMode(native::StaticOps::Group::ShadowViews, sceneTake && (groups & 512) != 0, sceneCompare && (groups & 512) != 0);
    // N5.5c: 1024 the shadow mask chain (post_SSM), its 1/4 depth copy and the terrain variance map's blur (native/effect_ops)
    effectOps_->setMode(sceneTake && (groups & 1024) != 0, sceneCompare && (groups & 1024) != 0);
    // 2048 the shadow views' setup: the cascade fit, the shadow camera, the maps' target / viewport / clear / reset
    effectOps_->setSetupMode(sceneTake && (groups & 2048) != 0, sceneCompare && (groups & 2048) != 0);
    // N5.6: 4096 the particle ops, 8192 the strip + light streak ops, 16384 the precipitation op (late list)
    staticOps_->setMode(native::StaticOps::Group::Particles, sceneTake && (groups & 4096) != 0, sceneCompare && (groups & 4096) != 0);
    staticOps_->setMode(native::StaticOps::Group::Strips, sceneTake && (groups & 8192) != 0, sceneCompare && (groups & 8192) != 0);
    staticOps_->setMode(native::StaticOps::Group::Precipitation, sceneTake && (groups & 16384) != 0, sceneCompare && (groups & 16384) != 0);
    // N5.7: 32768 the post chain (the image-effect graphs' effect filters), 65536 the sprite batcher (GUI, text, cursor, videos)
    effectOps_->setPostMode(sceneTake && (groups & 32768) != 0, sceneCompare && (groups & 32768) != 0);
    effectOps_->setGuiMode(sceneTake && (groups & 65536) != 0, sceneCompare && (groups & 65536) != 0);
    // S-7: the post chain's own shaders (Full only; On needs the take-over, Compare the composition)
    effectOps_->setOwnPost(rs.nativeOwnPost != nullptr && renderMode() == RenderMode::Full ? static_cast<native::OwnPost::Mode>(rs.nativeOwnPost->asInt()) : native::OwnPost::Mode::Off);
    // F12 / F13 (frame-ownership.md 14.24): the sky dome / the GUI sprite batches on our own shaders (Full; their groups taken over: 8 / 32 the sky op and cube faces,
    // 65536 the sprite batcher)
    {
        const auto sceneMode = [&](settings::Setting* st, bool groupsTaken, const char* what) {
            const auto m = st != nullptr && renderMode() == RenderMode::Full ? static_cast<native::OwnPost::Mode>(st->asInt()) : native::OwnPost::Mode::Off;
            if (m != native::OwnPost::Mode::Off && !groupsTaken) {
                spdlog::warn("native: own {}: needs native.scene_ops with its groups taken over: staying off", what);
                return native::OwnPost::Mode::Off;
            }
            return m;
        };
        effectOps_->setOwnScene(native::OwnPost::Scene::Sky, sceneMode(rs.nativeOwnSky, sceneTake && (groups & (8 | 32)) == (8 | 32), "sky"));
        effectOps_->setOwnScene(native::OwnPost::Scene::Gui, sceneMode(rs.nativeOwnGui, sceneTake && (groups & 65536) != 0, "gui"));
        // F11 (frame-ownership.md 14.26): the particle ops' quads expanded on the GPU (group 4096 taken over)
        effectOps_->setOwnScene(native::OwnPost::Scene::Particles, sceneMode(rs.nativeOwnParticles, sceneTake && (groups & 4096) != 0, "particles"));
    }
    frameTap_->setStaticOpsActive(staticOps_->active());
    frameTap_->setEffectOpsActive(effectOps_->active());
    // O1: the register file's source; the coverage counter only where it is read (a compare, the tap's own report, or asked for)
    staticOps_->setMirrorMode(frozen::kMirrorMode);
    // O2a / O2b: the engine's slot code skipped for the composed ops (per op: only where the group takes over; default on). A compare
    // needs the engine's code: without native.lean_check (its check frames) a compare run keeps lean off
    const int leanCheck = rs.nativeLeanCheck != nullptr ? rs.nativeLeanCheck->asInt() : 0;
    const bool leanWanted = on(rs.nativeLean);
    const bool leanOn = leanWanted && !((compare || sceneCompare) && leanCheck <= 0);
    if (leanWanted && !leanOn) spdlog::info("native: lean ops off: a compare is on and native.lean_check is 0 (the compare needs the engine's code)");
    staticOps_->setLean(leanOn, leanCheck);
    // O3: the effect units, sprite flushes and the PSSM pass
    effectOps_->setLean(leanOn && frozen::kLeanEffects, leanCheck);
    {
        const bool cacheBlocked = compare || sceneCompare || on(rs.nativeFrameTap) || on(rs.nativeCoverage) || !leanOn;
        const int pm = rs.nativePacketCache != nullptr && !cacheBlocked ? rs.nativePacketCache->asInt() : 0;
        staticOps_->setPacketCache(pm == 4 ? 1 : pm);
        dev_.setRetainVariants(pm != 4);  // 4 = the cache of before the variant packets (A/B)
    }
    staticOps_->setGpuScene(frozen::kGpuScene);
    staticOps_->setOpts(frozen::kCpuOpts);
    staticOps_->setLeanRestore(frozen::kLeanRestore);
    {
        // frame-ownership.md 7.8: owned register windows (Full + the material table, which is a restart setting)
        const bool owned = on(rs.nativeOwnedWindows) && renderMode() == RenderMode::Full && gpuscene::materialtable().active();
        if (owned != gpuscene::windowtables().active()) spdlog::info("native: owned windows {}", owned ? "on" : "off");
        gpuscene::windowtables().setActive(owned);
        staticOps_->setOwnedWindows(owned);
        // frame-ownership.md 7.13: F2c-1 owned objects (VS windows 1 / 2; needs the owned windows)
        const bool objects = on(rs.nativeOwnedObjects) && owned;
        if (objects != gpuscene::objecttable().active()) spdlog::info("native: owned objects {}", objects ? "on" : "off");
        gpuscene::objecttable().setActive(objects);
        // frame-ownership.md 14.15 b: rigid-object history motion (restart: the history doubles the object table's buffer, made at its first flush)
        gpuscene::objecttable().setHistory(objects && rs.nativeRigidMotion != nullptr && rs.nativeRigidMotion->startValue() != 0.0 && rs.nativeBoneMotion != nullptr &&
                                           rs.nativeBoneMotion->startValue() != 0.0);
        staticOps_->setOwnedObjects(objects);
        // frame-ownership.md 7.14: F2c-2 op records (compare only; needs the object table + the lighting images)
        staticOps_->setOwnedRecords(on(rs.nativeOwnedRecords) && objects && gpuscene::lightingimages().active());
    }
    {
        // frame-ownership.md 7.11: F2b-1 scene draws (Full only, needs the static ops to issue the draws)
        const bool scene = on(rs.nativeSceneDraws) && renderMode() == RenderMode::Full && staticOps_->active();
        dev_.setSceneDraws(scene, rs.nativeSceneDrawsCheck != nullptr ? static_cast<std::uint32_t>(std::max(0, rs.nativeSceneDrawsCheck->asInt())) : 0u);
        staticOps_->setSceneDraws(scene && dev_.sceneDrawsActive());
        // frame-ownership.md 7.16: F2c-3 owned static draws (needs the op records, scene draws and table lighting; Full)
        const bool ownedStatic = on(rs.nativeOwnedStatic) && renderMode() == RenderMode::Full && scene && dev_.sceneDrawsActive();
        // frame-ownership.md 7.19: the owned static draws use scene ABI v2 (the view table; v2 pipelines are built for the retained draws only while it is on)
        // (`-Environment 'RVK_OWNED_V1=1'` = the F2c-3 path on scene ABI v1, for A/B measurements)
        static const bool forceV1 = [] {
            char v[4] = {};
            return GetEnvironmentVariableA("RVK_OWNED_V1", v, sizeof(v)) != 0 && v[0] == '1';
        }();
        gpuscene::viewtable().setActive(ownedStatic && !forceV1);
        dev_.setSceneViews(ownedStatic && !forceV1);
        staticOps_->setOwnedStatic(ownedStatic, rs.nativeOwnedCheck != nullptr ? rs.nativeOwnedCheck->asInt() : 10);
        gpuscene::setVerifyInterval(rs.nativeOwnedCheck != nullptr ? rs.nativeOwnedCheck->asInt() : 10);  // 7.27: the tables' per-draw verification runs on the check frames
        // frame-ownership.md 7.21: F2c-4a persistent owned draws (scene ABI v2 + the view table)
        staticOps_->setOwnedPersistent(ownedStatic && !forceV1 && on(rs.nativeOwnedPersistent));
        {
            // frame-ownership.md 14.12: S-4 vertex pulling of the persistent owned draws (their slots get vertex fetch blocks, the draws' pulled vertex shaders are built)
            const bool pull = ownedStatic && !forceV1 && on(rs.nativeOwnedPersistent) && on(rs.nativeVertexPull);
            if (pull != dev_.vertexPull()) spdlog::info("native: vertex pull {}", pull ? "on" : "off");
            dev_.setVertexPull(pull);
        }
        {
            // frame-ownership.md 7.25: F4-2 / F4-3 owned SpeedTrees (the persistent blocks + scene ABI v2; the trees' v2 scene forms are built while it is on)
            const bool trees = on(rs.nativeOwnedTrees) && ownedStatic && !forceV1 && on(rs.nativeOwnedPersistent);
            dev_.setSceneTrees(trees);
            staticOps_->setOwnedTrees(trees);
        }
        {
            const bool shadows = on(rs.nativeOwnedShadows) && ownedStatic && !forceV1 && on(rs.nativeOwnedPersistent);
            dev_.setSceneShadows(shadows);  // (the cascades' z draws get their scene forms: the retained draws the owned path replays)
            staticOps_->setOwnedShadows(shadows);
            staticOps_->setOwnedShadowViews(shadows && on(rs.nativeOwnedShadowViews));  // F3-4 (frame-ownership.md 14.29): the terrain map + the point-light atlas cells too
        }
        staticOps_->setOwnedVegetation(on(rs.nativeOwnedVegetation) && renderMode() == RenderMode::Full && staticOps_->active());
        staticOps_->setOwnFoliage(on(rs.nativeOwnFoliage) && renderMode() == RenderMode::Full && staticOps_->active());  // S-5 (frame-ownership.md 14.14)
        // F4-5 (frame-ownership.md 14.17): the RT scene's animated SpeedTrees by compute (no capture: the owned trees stay owned with RT on; Full only)
        if (rtScene_) rtScene_->setTreeWind(rs.nativeTreeWind != nullptr && renderMode() == RenderMode::Full ? rs.nativeTreeWind->asInt() : 0);
        // F8-1 (frame-ownership.md 14.30): the RT scene from the owned record set (Full, the persistent records)
        if (rtScene_) rtScene_->setRecordReplay(on(rs.nativeRtRecords) && on(rs.nativeOwnedPersistent) && renderMode() == RenderMode::Full);
        {
            // frame-ownership.md 7.42: F7-2 owned vegetation draws (the persistent blocks + scene ABI v2; the grass draws' v2 scene forms are built while it is on)
            const bool vegOps = on(rs.nativeOwnedVegetationOps) && on(rs.nativeOwnedVegetation) && ownedStatic && !forceV1 && on(rs.nativeOwnedPersistent);
            dev_.setSceneVeg(vegOps);
            staticOps_->setOwnedVegetationOps(vegOps);
        }
        {
            // frame-ownership.md 7.41: F6-1 owned skinned actors (the persistent records + scene ABI v2, the bone table; the skinned draws' v2 scene forms are built while it is on)
            const bool actors = on(rs.nativeOwnedActors) && ownedStatic && !forceV1 && on(rs.nativeOwnedPersistent) && on(rs.nativeOwnedObjects);
            dev_.setSceneActors(actors);
            gpuscene::bonetable().setActive(actors);
            // frame-ownership.md 14.13: bone-history motion (restart setting: the history doubles the bone table's buffer, made at its first flush)
            const int bone = actors && rs.nativeBoneMotion != nullptr ? static_cast<int>(rs.nativeBoneMotion->startValue()) : 0;
            gpuscene::bonetable().setHistory(bone != 0);
            dev_.setBoneMotion(gpuscene::bonetable().history() ? bone : 0);
            dev_.setRigidMotion(bone != 0 && rs.nativeRigidMotion != nullptr && rs.nativeRigidMotion->startValue() != 0.0 && gpuscene::objecttable().active());  // 14.15 b
            staticOps_->setOwnedActors(actors);
        }
        {
            // frame-ownership.md 14.19: F2f owned instance lists (+ the hardware batches composed per instance) and F9 the blended terrain layers at their place
            const bool inst = on(rs.nativeOwnedInstances) && ownedStatic && !forceV1 && on(rs.nativeOwnedPersistent);
            dev_.setSceneInstances(inst);
            gpuscene::rowtable().setActive(inst);
            staticOps_->setOwnedInstances(inst);
        }
        {
            // frame-ownership.md 14.20: F9 + F14 owned ordered draws (the solid list's blended / no-z-write ops, the alpha / refracted lists' mesh ops at their place)
            const bool ordered = on(rs.nativeOwnedOrdered) && ownedStatic && !forceV1 && on(rs.nativeOwnedPersistent);
            dev_.setSceneOrdered(ordered);
            staticOps_->setOwnedOrdered(ordered);
        }
        staticOps_->setOwnedBillboards(on(rs.nativeOwnedBillboards) && scene && dev_.sceneDrawsActive() && gpuscene::lightingimages().active() && gpuscene::windowtables().active());
    }
    staticOps_->setRetained(frozen::kRetained, frozen::kRetainedGroups, frozen::kRetainedCheck);
    frameTap_->setCoverage(on(rs.nativeCoverage) || on(rs.nativeFrameTap) || compare || sceneCompare);
}

void HalDevice::registerCube(const void* api, d3d9::Texture* texture) {
    const std::lock_guard lock(apiTextureMutex_);
    apiCubes_[api] = texture;
    apiTextureGeneration_.fetch_add(1, std::memory_order_release);
}

void HalDevice::unregisterCube(const void* api) {
    const std::lock_guard lock(apiTextureMutex_);
    apiCubes_.erase(api);
    apiTextureGeneration_.fetch_add(1, std::memory_order_release);
}

d3d9::Texture* HalDevice::cubeOfApi(const void* api) {
    if (api == nullptr) return nullptr;
    const std::lock_guard lock(apiTextureMutex_);
    const auto it = apiCubes_.find(api);
    return it != apiCubes_.end() ? it->second : nullptr;
}

HalDevice::~HalDevice() {
    {
        const std::lock_guard lock(g_liveDevicesMutex);
        std::erase(g_liveDevices, this);
    }
    spdlog::info("hal: device released by the engine: shutting down");
    const auto shutdownT0 = std::chrono::steady_clock::now();
    // 14.26: a NtTerminateProcess of another thread waits for this shutdown (the game ends the process that way; it cut the shutdown half-way before)
    crashdump::deviceShutdownBegin();
    struct ShutdownMark {
        ~ShutdownMark() { crashdump::deviceShutdownEnd(); }
    } shutdownMark;
    overlay::setWindowCloseHandler(nullptr, nullptr);
    engine::scene::detach(&dev_);
    settings::registry().removeCallbacks(this);
    if (framegen_) framegen_->shutdown();  // AMD's swapchain calls into the overlay from its threads: it goes first
    if (overlay_) {
        dev_.setOverlay(nullptr, false);
        overlay_.reset();  // (the UI backend's shutdown waits for the device under the queue lock)
    }
    if (settings::registry().dirty() && !options_.settingsFile.empty()) {
        settings::registry().save(options_.settingsFile);
    }
    pictureCheck_.reset();
    depthZ_.reset();
    debugView_.reset();  // removes its pass, retires its pipelines
    // the RT scene's consumers (rt.debug, the water's traced reflections) release it in their destructors: before it
    rtDebug_.reset();    // releases the RT scene
    objectMotion_.reset();  // unregisters its observer
    framegen_.reset();   // removes its passes, retires the generator
    upscale_.reset();    // clears the device's jitter source
    materialRt_.reset(); // removes its pass, releases the RT scene
    gbuffer_.reset();    // removes its passes, the G-buffer attachments
    if (matPack_) {
        enh::matpack::setReloadHook({});
        dev_.setPackProvider({});
        dev_.setPackStamp({});
        if (rtScene_) rtScene_->setMaterialPack(nullptr);
    }
    matPack_.reset();    // joins its loader, frees the pack textures
    gi_.reset();         // unregisters its observer / pass, releases the RT scene
    water_.reset();      // unregisters its draw replacer, releases the RT scene
    shadows_.reset();
    rtShadows_.reset();  // removes its pass, releases the RT scene
    lightTable_.reset(); // its destructor releases the RT scene (lamp shadows hold it): before the scene goes
    rtScene_.reset();    // unregisters its observer / pass, retires its acceleration structures
    {
        const std::lock_guard lock(handleMutex_);
        for (auto& [h, d] : decls_) dev_.release(std::move(d));
        for (auto& [f, d] : fvfDecls_) dev_.release(std::move(d));
        for (auto& [h, t] : depthBuffers_) dev_.release(std::move(t));
        decls_.clear();
        fvfDecls_.clear();
        depthBuffers_.clear();
    }
    // the native frame's owners hold pipelines / images of the RHI device: they must go before dev_.destroy() frees it
    // (as members they were destroyed after it: OwnPost's pipeline destructor read the freed VkDeviceImpl)
    effectOps_.reset();
    staticOps_.reset();
    frameTap_.reset();
    // the process-wide GPU scene tables (static / leaked singletons) hold buffers and pipelines of the device: released before it (cluster store's worker included);
    // left alive they were leaked at vkDestroyDevice and the static ones' destructors at process exit retired them through the freed backend
    const auto enhT1 = std::chrono::steady_clock::now();
    gpuscene::releaseGpuObjects(dev_);
    dev_.destroy();
    const auto ms = [](auto a, auto b) { return std::chrono::duration<double, std::milli>(b - a).count(); };
    const auto shutdownT2 = std::chrono::steady_clock::now();
    spdlog::info("hal: device shutdown complete in {:.0f} ms (features + native frame {:.0f} ms, device {:.0f} ms; {} process terminations of other threads waited for it)",
                 ms(shutdownT0, shutdownT2), ms(shutdownT0, enhT1), ms(enhT1, shutdownT2), crashdump::terminateWaits());
}

bool HalDevice::create(const Options& options, const eCAPIDevice::eSAPIStartParameters& p) {
    options_ = options;
    DeviceOptions o;
    o.window = p.window;
    o.width = p.backBufferWidth;
    o.height = p.backBufferHeight;
    engineVsync_ = p.vsync;
    const auto& rs = settings::renderer();
    if (rs.vsync != nullptr) {
        // player settings (config.toml overrides are locks on them: bootstrap)
        const std::string_view v = rs.vsync->asEnum();
        o.vsync = v == "on" ? true : v == "off" ? false : p.vsync;
    } else {
        o.vsync = options.vsync < 0 ? p.vsync : options.vsync != 0;
    }
    o.perf = options.perf;
    o.pipelineLibraries = options.pipelineLibraries;
    o.perfFrame = options.perfFrame;
    o.depthFormat = static_cast<std::uint32_t>(p.depthFormat);
    o.validation = options.validation;
    o.syncValidation = options.syncValidation;
    o.adapter = options.adapter;
    // the graphics API of the session (vulkan.api is a restart setting: the value it started with decides); D3D12 needs SM 6.6 / binding tier 3
    // and falls back to Vulkan with a log line
    // the D3D12 backend is a debug-mode feature while it is not released: a stored d3d12 value is ignored outside debug mode
    // (a config.toml `[vulkan] api` lock counts as the developer's choice and sets the value through the same setting)
    if (rs.api != nullptr && rs.api->startValue() != 0.0 && !settings::registry().debugMode()) {
        spdlog::info("hal: vulkan.api = d3d12 ignored: the Direct3D 12 backend needs debug mode (overlay.debug_mode)");
    } else if (rs.api != nullptr && rs.api->startValue() != 0.0) {
        std::string reason;
        if (rhi::d3d12Usable(options.adapter, &reason)) {
            o.backend = rhi::Backend::D3D12;
            spdlog::info("hal: graphics API Direct3D 12 (vulkan.api = d3d12)");
        } else {
            spdlog::warn("hal: vulkan.api = d3d12 is not usable here ({}): Vulkan instead", reason);
        }
    }
    o.maxFps = rs.maxFps != nullptr ? rs.maxFps->value() : options.maxFps;
    o.cacheDir = options.cacheDir;
    o.logDraws = options.logDraws;
    // rt.allow is a restart setting: the value the session started with decides (its effective value waits for the report)
    o.d3d12Messages = options.d3d12Messages;
    o.d3d12Dred = options.d3d12Dred;
    o.allowRayTracing = rs.rtAllow == nullptr || rs.rtAllow->startValue() != 0.0;
    // the frame generation swapchain needs three more queues than the floor's one (vk/fg_swapchain.h): requested whenever the frame
    // generation DLL is installed, so that framegen.mode can be switched at run time (queues nobody uses cost nothing)
    {
        std::error_code ec;
        o.frameGenQueues = rs.framegenMode != nullptr &&
                           std::filesystem::exists(paths::dataDir() / "amd_fidelityfx_framegeneration_vk.dll", ec) &&
                           std::filesystem::exists(paths::dataDir() / "amd_fidelityfx_loader_vk.dll", ec);
    }
    // debug.names is a restart setting too: objects are named as they are created (vk/debug_names.h)
    rhi::setDebugNames(rs.debugNames == nullptr || rs.debugNames->startValue() != 0.0);
    if (!dev_.create(o)) {
        return false;
    }
    // fullscreen: a borderless window over the monitor, or exclusive fullscreen with vulkan.fullscreen = exclusive
    // (engine/window_mode.h)
    window_ = p.window;
    engine::window::onModeSet(p.window, p.windowed, o.width, o.height, p.refreshRate);
    spdlog::info("hal: Vulkan device for window {} ({}x{}, vsync {}, depth {})", static_cast<void*>(p.window), o.width, o.height,
                 o.vsync, rhi::d3dFormatName(o.depthFormat));
    engine::scene::attach(&dev_);  // the frame phase markers (no-ops when they are not installed)
    auto& reg = settings::registry();
    // frame generation providers (vk/framegen.h): which of FSR 3 / FSR 4 this device can run, for the capability report
    dev_.game().probeFrameGeneration(rhi::fidelityFxLoader(paths::dataDir(), o.backend));
    // capability report (core/caps.h, docs/research/raytracing.md 3): logged once, the overlay's "Capabilities", and the
    // resolution of settings with requirements (greyed + parity fallback when unmet)
    {
        caps::Report report = dev_.game().capabilityReport(options.pipelineLibraries);
        // test knob: RVK_FAKE_INTEGRATED=1 makes the device count as integrated (low bandwidth auto, the Integrated preset's first-run defaults)
        if (char fake[8] = {}; GetEnvironmentVariableA("RVK_FAKE_INTEGRATED", fake, sizeof(fake)) > 0 && fake[0] == '1' && !report.integratedGpu) {
            report.integratedGpu = true;
            spdlog::info("caps: RVK_FAKE_INTEGRATED=1: the device counts as an integrated GPU");
        }
        // the session's render mode (renderer.mode): the overlay's System tab "Graphics card" and the caps lines
        report.properties.emplace_back("render mode", std::format("{} at start (Vulkan; Wrapper / Full switch at run time)", toString(renderMode())));
        for (const auto& line : report.lines()) spdlog::info("caps: {}", line);
        caps::setCurrent(report);
        reg.setCapabilities(report);
        // integrated GPU: the first run picks the Integrated preset for every setting still at its default (docs/research/frame-ownership.md 14.11)
        if (report.known && report.integratedGpu) {
            const auto changes = settings::applyIntegratedDefaults(reg);
            if (changes.empty()) {
                spdlog::info("settings: integrated GPU: Integrated preset already applied, or nothing left at its default");
            } else {
                spdlog::info("settings: integrated GPU: Integrated preset chosen by default, {} values set", changes.size());
                for (const auto& c : changes) spdlog::info("settings: integrated preset: {}", c);
            }
        }
        // Low bandwidth mode (docs/research/low-bandwidth.md): auto = on for integrated GPUs; the decision is read by the passes that
        // create the affected images (caps::lowBandwidth()), a change re-creates them (transients are keyed by description)
        if (rs.lowBandwidth != nullptr) {
            const auto applyLowBandwidth = [report](const settings::Setting& s) {
                const caps::LowBandwidth d = caps::resolveLowBandwidth(s.asEnum(), report);
                if (caps::setLowBandwidth(d)) {
                    spdlog::info("vulkan: low bandwidth: {} ({})", d.on ? "on" : "off", d.automatic ? "auto: " + d.reason : d.reason);
                }
            };
            applyLowBandwidth(*rs.lowBandwidth);
            reg.onChange(*rs.lowBandwidth, applyLowBandwidth, this);
        }
    }
    if (options.rtSelfTest) dev_.game().runRaytracingSelfTest();
    if (rs.vsync != nullptr) reg.onChange(*rs.vsync, [this](const settings::Setting&) { applyVsync(); }, this);
    if (rs.fullscreen != nullptr) {
        // exclusive fullscreen (docs/research/window-mode.md 8): the effective value (borderless without the capability)
        reg.onChange(*rs.fullscreen, [this](const settings::Setting&) { applyFullscreen(); }, this);
        applyFullscreen();
    }
    if (rs.maxFps != nullptr) {
        reg.onChange(*rs.maxFps, [this](const settings::Setting& s) {
            dev_.setMaxFps(s.value());
            spdlog::info("hal: settings: frame rate limit {}", s.value() > 0.0 ? std::format("{} fps", s.value()) : std::string("none"));
        }, this);
    }
    // debug views and draw records (d3d9/draws.h, docs/research/draw-tags.md)
    if (rs.tintDraws != nullptr) {
        const auto applyTint = [this](const settings::Setting& s) {
            const std::string_view v = s.asEnum();
            dev_.setTintMode(v == "kind" ? draws::TintMode::Kind : v == "pass" ? draws::TintMode::Pass : draws::TintMode::Off);
            spdlog::info("hal: settings: tint draws {}", v);
        };
        if (!rs.tintDraws->isDefault()) applyTint(*rs.tintDraws);
        reg.onChange(*rs.tintDraws, applyTint, this);
    }
    // regions first: the crash markers follow them (applied the other way round, the markers warned "debug.regions is off" with regions on)
    if (rs.debugRegions != nullptr) {
        dev_.setRegionsEnabled(rs.debugRegions->asBool());
        reg.onChange(*rs.debugRegions, [this](const settings::Setting& s) { dev_.setRegionsEnabled(s.asBool()); }, this);
    }
    if (rs.debugCrashMarkers != nullptr) {
        dev_.setCrashMarkers(rs.debugCrashMarkers->asBool());
        reg.onChange(*rs.debugCrashMarkers, [this](const settings::Setting& s) { dev_.setCrashMarkers(s.asBool()); }, this);
    }
    if (rs.recordThreads != nullptr) {
        dev_.setRecordThreads(rs.recordThreads->asBool());
        reg.onChange(*rs.recordThreads, [this](const settings::Setting& s) {
            dev_.setRecordThreads(s.asBool());
            spdlog::info("hal: settings: record threads {}", s.asBool() ? "on" : "off");
        }, this);
    }
    if (rs.reverseZ != nullptr) {
        dev_.setReverseZ(rs.reverseZ->asBool());
        reg.onChange(*rs.reverseZ, [this](const settings::Setting& s) {
            dev_.setReverseZ(s.asBool());
            spdlog::info("hal: settings: reversed depth {}", s.asBool() ? "on" : "off");
        }, this);
    }
    if (rs.batchDraws != nullptr) {
        dev_.setBatching(rs.batchDraws->asBool());
        reg.onChange(*rs.batchDraws, [this](const settings::Setting& s) {
            dev_.setBatching(s.asBool());
            spdlog::info("hal: settings: batch opaque draws {}", s.asBool() ? "on" : "off");
        }, this);
    }
    if (rs.splitVerify != nullptr) {
        dev_.setSplitVerify(rs.splitVerify->asBool());
        gpuscene::setWindowMasksWanted(rs.splitVerify->asBool());
        reg.onChange(*rs.splitVerify, [this](const settings::Setting& s) { dev_.setSplitVerify(s.asBool()); gpuscene::setWindowMasksWanted(s.asBool()); }, this);
    }
    // frame-ownership.md 7.47: the owned picture check (Full; the pass is registered while the setting is on)
    pictureCheck_ = std::make_unique<enh::PictureCheck>(dev_);
    if (rs.nativePictureCheck != nullptr) {
        const auto applyPicture = [this](const settings::Setting& s) { pictureCheck_->setEnabled(s.asBool() && renderMode() == RenderMode::Full); };
        reg.onChange(*rs.nativePictureCheck, applyPicture, this);
        if (rs.rendererMode != nullptr) reg.onChange(*rs.rendererMode, applyPicture, this);
        applyPicture(*rs.nativePictureCheck);
    }
    // S-6 (frame-ownership.md 14.10): depth-only z passes (Full)
    depthZ_ = std::make_unique<enh::DepthZ>(dev_);
    if (rs.nativeDepthZ != nullptr) {
        const auto applyDepthZ = [this](const settings::Setting& s) {
            const int v = renderMode() == RenderMode::Full ? s.asInt() : 0;
            depthZ_->setMode(v == 1 ? enh::DepthZ::Mode::On : v == 2 ? enh::DepthZ::Mode::Compare : enh::DepthZ::Mode::Off);
        };
        reg.onChange(*rs.nativeDepthZ, applyDepthZ, this);
        if (rs.rendererMode != nullptr) reg.onChange(*rs.rendererMode, applyDepthZ, this);
        applyDepthZ(*rs.nativeDepthZ);
    }
    // enhancements (docs/research/injection.md): the debug injection view
    debugView_ = std::make_unique<enh::DebugView>(dev_);
    if (rs.injectView != nullptr) {
        const auto applyInject = [this](const settings::Setting& s) {
            debugView_->setMode(enh::DebugView::parse(s.asEnum()));
            spdlog::info("hal: settings: injection test view {}", s.asEnum());
        };
        if (!rs.injectView->isDefault()) applyInject(*rs.injectView);
        reg.onChange(*rs.injectView, applyInject, this);
    }
    // the ray tracing scene (enh/rt_scene.h, docs/research/rt-reflections.md 3) and its debug view rt.debug
    dev_.setEngineTextureResolver([this](const void* api) { return textureOfApi(api); });
    rtScene_ = std::make_unique<enh::RtScene>(dev_);
    rtDebug_ = std::make_unique<enh::RtDebug>(dev_, *rtScene_);
    if (rs.rtDebug != nullptr) {
        const auto applyRtDebug = [this](const settings::Setting& s) {
            rtDebug_->setMode(enh::RtDebug::parse(s.asEnum()), enh::RtDebug::optionFlags(s.asEnum()));
            spdlog::info("hal: settings: ray tracing scene view {}", s.asEnum());
        };
        if (rs.rtDebug->asEnum() != "off") applyRtDebug(*rs.rtDebug);
        reg.onChange(*rs.rtDebug, applyRtDebug, this);
    }
    if (rs.rtSceneViews != nullptr) {
        const auto applyViews = [this](const settings::Setting& s) {
            const std::string_view v = s.asEnum();
            rtScene_->setViews(v == "main" ? 1 : v == "shadow" ? 2 : 3);
        };
        if (!rs.rtSceneViews->isDefault()) applyViews(*rs.rtSceneViews);
        reg.onChange(*rs.rtSceneViews, applyViews, this);
    }
    // F8-0 persistent RT scene entries (frame-ownership.md 7.20): Full only, run time
    if (rs.rtTableEntries != nullptr) {
        const auto applyTable = [this](const settings::Setting&) {
            const auto& r2 = settings::renderer();
            rtScene_->setTableEntries(r2.rtTableEntries->asBool() && renderMode() == RenderMode::Full, r2.rtTableCompare != nullptr && r2.rtTableCompare->asBool());
        };
        applyTable(*rs.rtTableEntries);
        reg.onChange(*rs.rtTableEntries, applyTable, this);
        if (rs.rtTableCompare != nullptr) reg.onChange(*rs.rtTableCompare, applyTable, this);
        if (rs.rendererMode != nullptr) reg.onChange(*rs.rendererMode, applyTable, this);
    }
    // the RT scene's far level of detail + memory limit (docs/research/rt-lod.md) and the RT quality preset (rt.preset)
    if (rs.rtLodScale != nullptr && rs.rtMemoryBudget != nullptr) {
        const auto applyLod = [this](const settings::Setting&) {
            const float scale = settings::renderer().rtLodScale->asFloat();
            rtScene_->setLod(frozen::kRtLodRange, scale > 0.0f ? frozen::kRtLodBiasLow : frozen::kRtLodBias, frozen::kRtLodFarRange, scale);
        };
        const auto applyBudget = [this](const settings::Setting&) {
            rtScene_->setMemoryBudget(static_cast<std::uint32_t>(settings::renderer().rtMemoryBudget->asInt()));
        };
        applyLod(*rs.rtLodScale);
        if (rs.rtMemoryBudget->asInt() != 0) applyBudget(*rs.rtMemoryBudget);
        reg.onChange(*rs.rtLodScale, applyLod, this);
        reg.onChange(*rs.rtMemoryBudget, applyBudget, this);
    }
    if (rs.rtPreset != nullptr) {
        // Low / Medium / High set the cost knobs of everything that traces; a hand change of one of them makes the preset Custom
        // (callbacks run in the registry's dispatch: a preset's own values arrive together, so they match it by then)
        settings::Registry* regp = &reg;
        reg.onChange(*rs.rtPreset, [regp](const settings::Setting& s) {
            const std::string_view v = s.asEnum();
            if (v == "custom") return;
            const auto& presets = settings::rtPresets();
            for (const auto& p : presets) {
                if (p.name == v) settings::applyPreset(*regp, p);
            }
        }, this);
        for (const auto& kv : settings::rtPresets().front().values) {
            if (settings::Setting* member = reg.find(kv.first); member != nullptr) {
                reg.onChange(*member, [regp](const settings::Setting&) { settings::syncRtPreset(*regp); }, this);
            }
        }
        // the preset says what the members say, also for a settings.toml / config.toml loaded before this point
        if (settings::syncRtPreset(reg)) spdlog::info("hal: settings: rt.preset set to {} (the files' ray tracing values)", rs.rtPreset->asEnum());
    }
    // characters skinned by compute for the RT scene (docs/research/rt-shadows.md 10)
    if (rs.rtSkin != nullptr && rs.rtSkinVerify != nullptr) {
        const auto applySkin = [this](const settings::Setting&) {
            const auto& rs2 = settings::renderer();
            rtScene_->setGpuSkinning(rs2.rtSkin->asBool(), rs2.rtSkinVerify->asBool());
        };
        applySkin(*rs.rtSkin);
        reg.onChange(*rs.rtSkin, applySkin, this);
        reg.onChange(*rs.rtSkinVerify, applySkin, this);
    }
    // enhanced water (enh/water.h, docs/research/water.md): a draw replacer, live settings
    water_ = std::make_unique<enh::Water>(dev_, rtScene_.get());
    if (rs.waterEnabled != nullptr) {
        for (settings::Setting* s : {rs.waterEnabled, rs.waterQuality, rs.waterReflections, rs.waterOcean, rs.waterOceanQuality,
                                     rs.waterSeaState, rs.waterWindDirection, rs.waterWaveScale,
                                     rs.waterDebug, rs.rtReflections, rs.rtReflectionTweaks}) {
            if (s != nullptr) reg.onChange(*s, [this](const settings::Setting&) { applyWater(); }, this);
        }
        if (rs.waterEnabled->asBool()) applyWater();
    }
    // enhanced sun shadows (enh/shadows.h, docs/research/shadows.md): a draw replacer for the shadow mask, live settings
    shadows_ = std::make_unique<enh::Shadows>(dev_);
    rtShadows_ = std::make_unique<enh::RtShadows>(dev_, rtScene_.get());
    shadows_->setRtShadows(rtShadows_.get());
    if (rs.shadowsEnabled != nullptr) {
        for (settings::Setting* s : {rs.shadowsRt, rs.shadowsRtQuality, rs.shadowsRtRange, rs.rtShadowSync, rs.shadowsEnabled, rs.shadowsFilter,
                                     rs.shadowsBias, rs.shadowsDebug, rs.shadowsMapSize, rs.shadowsRange, rs.rtReflectionTweaks}) {
            if (s != nullptr) reg.onChange(*s, [this](const settings::Setting&) { applyShadows(); }, this);
        }
        applyShadows();  // also the engine's cascade overrides (shadows.map_size / range: "auto" follows shadows.enabled)
    }
    // the native frame's op tap (native/frame_tap.h, docs/research/engine-frame/README.md N5.0)
    frameTap_ = std::make_unique<native::FrameTap>(dev_);
    if (rs.nativeFrameTap != nullptr) {
        reg.onChange(*rs.nativeFrameTap, [this](const settings::Setting& s) { frameTap_->apply(s.asBool()); }, this);
        frameTap_->apply(rs.nativeFrameTap->asBool());
    }
    // frame ownership F0 (native/owned_observe.h): the tap runs for it and reports each view's start
    if (rs.nativeOwnedObserve != nullptr) {
        const auto applyObserve = [this](const settings::Setting& s) {
            native::owned_observe::setEnabled(s.asBool());
            frameTap_->setObserve(s.asBool() && native::owned_observe::installed());
        };
        reg.onChange(*rs.nativeOwnedObserve, applyObserve, this);
        applyObserve(*rs.nativeOwnedObserve);
    }
    // frame ownership F0 (gpuscene/mesh_parts.h): the part + layout tables, fed by the static composition's draws (restart setting)
    gpuscene::meshparts().setActive(rs.nativeMeshParts != nullptr && rs.nativeMeshParts->asBool());
    // N5.1 static ops (native/static_ops.h): driven by the frame tap
    staticOps_ = std::make_unique<native::StaticOps>(*this);
    frameTap_->setStaticOps(staticOps_.get());
    effectOps_ = std::make_unique<native::EffectOps>(*this);
    frameTap_->setEffectOps(effectOps_.get());
    for (settings::Setting* s : {rs.nativeStaticOps, rs.nativeStaticOpsCompare, rs.nativeSceneOps, rs.nativeSceneOpsCompare, rs.nativeSceneOpsGroups,
                                 rs.nativeCoverage, rs.nativeFrameTap, rs.nativeLean, rs.nativeLeanCheck,
                                 rs.nativePacketCache, rs.nativeOwnPost, rs.nativeOwnSky, rs.nativeOwnGui, rs.nativeOwnParticles, rs.nativeOwnedDecals, rs.nativeOwnedWindows, rs.nativeOwnedObjects, rs.nativeOwnedRecords, rs.nativeOwnedBillboards, rs.nativeOwnedVegetation, rs.nativeOwnedVegetationOps, rs.nativeSceneDraws, rs.nativeSceneDrawsCheck, rs.nativeOwnedStatic, rs.nativeOwnedPersistent, rs.nativeOwnedShadows, rs.nativeOwnedShadowViews, rs.nativeOwnedActors, rs.nativeOwnedInstances, rs.nativeOwnedOrdered, rs.nativeOwnedCheck, rs.nativeOwnedTrees, rs.nativeClusters, rs.nativeClusterCull, rs.nativeClusterCullShadows, rs.nativeVertexPull, rs.nativeBoneMotion, rs.nativeOwnFoliage, rs.nativeTreeWind, rs.nativeRtRecords}) {
        if (s != nullptr) reg.onChange(*s, [this](const settings::Setting&) { applyStaticOps(); }, this);
    }
    // renderer.mode (Wrapper / Full): like the native.* settings applied by the registry's dispatch before a present, i.e. at a
    // frame boundary (no frame starts with one mode and ends with the other)
    if (rs.rendererMode != nullptr) reg.onChange(*rs.rendererMode, [this](const settings::Setting&) { applyRenderMode(); }, this);
    applyRenderMode();
    // material pack (enh/material_pack.h, docs/research/material-pack.md): decided before any material variant is built (variants are cached
    // per shader, the PBR code is part of them or not): the setting is on and the pack folder has something in it
    std::vector<std::filesystem::path> packRoots;
    if (rs.materialsPack != nullptr && rs.materialsPack->asBool()) {
        packRoots.push_back(paths::dataDir() / "materialpack");
        // (the Win32 environment, not the CRT's getenv snapshot: run.ps1 -Environment / [debug] environment set the variable from DllMain with SetEnvironmentVariableW)
        char envBuf[4096];
        const DWORD envLen = GetEnvironmentVariableA("RVK_MATPACK", envBuf, sizeof(envBuf));
        if (envLen != 0 && envLen < sizeof(envBuf)) {
            std::string list(envBuf, envLen);
            for (std::size_t at = 0; at <= list.size();) {
                const std::size_t semi = list.find(';', at);
                const std::string part = list.substr(at, semi == std::string::npos ? std::string::npos : semi - at);
                if (!part.empty()) packRoots.emplace_back(part);
                if (semi == std::string::npos) break;
                at = semi + 1;
            }
        }
        const auto hasFiles = [](const std::filesystem::path& root) {
            std::error_code ec;
            if (!std::filesystem::is_directory(root, ec)) return false;
            for (std::filesystem::recursive_directory_iterator it(root, ec), end; it != end && !ec; it.increment(ec)) {
                if (it->is_regular_file(ec)) return true;
            }
            return false;
        };
        std::erase_if(packRoots, [&](const std::filesystem::path& p) { return !hasFiles(p); });
    }
    // Full-owned clustered lights (enh/light_table.h): decided here for the same reason (variants are cached per shader)
    const bool clusterLights = rs.lightingClustered != nullptr && rs.lightingClustered->asBool() && renderMode() == RenderMode::Full;
    // (the physical light style evaluates the PBR code of the pack's variants on every lit draw: a session that STARTS with lighting.style = physical carries it in the
    // cluster variants, original <-> physical then switch at run time; the code costs ~0.4 ms GPU in the jungle save, so it is not compiled in otherwise)
    enh::setPbrVariants(!packRoots.empty() || (clusterLights && rs.lightingStyle != nullptr && rs.lightingStyle->asEnum() == "physical"));
    if (rs.lightingClustered != nullptr && rs.lightingClustered->asBool() && !clusterLights) spdlog::info("enh: lights: lighting.clustered applies in Full mode only");
    enh::setClusterVariants(clusterLights);
    // ray-traced global illumination (enh/gi.h, docs/research/rtgi.md): probes + the material shaders' GI variants
    gi_ = std::make_unique<enh::Gi>(dev_, rtScene_.get());
    if (!packRoots.empty()) {
        // the pack rides on the GI variant slot (the Gi class registers it only with a ray tracing scene)
        dev_.setVariantProvider(d3d9::VariantSlot::Gi, {enh::materialVariantTag("gi"), [](const d3d9::Shader& s, shader::TranslateOptions& o) {
                                                            return enh::materialVariantOptions(s, enh::MaterialVariant::Gi, o);
                                                        }});
        matPack_ = std::make_unique<enh::matpack::MaterialPack>(dev_);
        matPack_->configure(true, packRoots, static_cast<std::uint64_t>(rs.materialsBudget->asInt()) << 20, static_cast<float>(frozen::kMaterialRoughness));
        dev_.setPackProvider([this](const d3d9::draws::Tag& tag) {
            const auto h = matPack_->lookupMaterial(tag.material);
            static auto last = std::chrono::steady_clock::now();
            static unsigned calls = 0;
            const auto now = (++calls & 255u) == 0 ? std::chrono::steady_clock::now() : last;
            if (now - last >= std::chrono::seconds(5)) {
                last = now;
                const auto c = matPack_->takeCounters();
                if (c.lookups != 0) spdlog::info("enh: materials: {} draws looked up, {} matched ({} distinct entries seen), {} derived from the specular data", c.lookups, c.matched, matPack_->matchedNames(), c.derived);
            }
            return d3d9::Device::PackHit{h.word, h.texture};
        });
        dev_.setPackStamp([this]() -> std::uint32_t { return matPack_->generation(); });
        materialRt_ = std::make_unique<enh::MaterialRt>(dev_, rtScene_.get());
        for (settings::Setting* s : {rs.materialsQuality, rs.materialsDebug, rs.materialsAlternate,
                                     rs.materialsRtReflections, rs.materialsRtRange, rs.materialsRainWetness, rs.materialsRainForce, rs.materialsGlass,
                                     rs.materialsDerive}) {
            if (s != nullptr) reg.onChange(*s, [this](const settings::Setting&) {
                applyMaterials();
                applyGbuffer();  // the traced reflections read the G-buffer
            }, this);
        }
        applyMaterials();
        rtScene_->setMaterialPack(matPack_.get());
        enh::matpack::setReloadHook([this, packRoots]() {
            // (the overlay's command thread is the render thread's: the commands run at presents)
            const auto& rs2 = settings::renderer();
            matPack_->configure(true, packRoots, static_cast<std::uint64_t>(rs2.materialsBudget->asInt()) << 20, static_cast<float>(frozen::kMaterialRoughness));
            return true;
        });
        spdlog::info("enh: materials: pack on ({} folder(s)), PBR variants enabled", packRoots.size());
    }
    if (clusterLights) {
        // the variants ride on the Gi slot and the pack's gate (lit material draws of the main view); a pack provider that finds nothing (word 0) opens the gate
        dev_.setVariantProvider(d3d9::VariantSlot::Gi, {enh::materialVariantTag("gi"), [](const d3d9::Shader& s, shader::TranslateOptions& o) {
                                                            return enh::materialVariantOptions(s, enh::MaterialVariant::Gi, o);
                                                        }});
        if (!matPack_) dev_.setPackProvider([](const d3d9::draws::Tag&) { return d3d9::Device::PackHit{}; });
        lightTable_ = std::make_unique<enh::LightTable>(dev_);
        lightTable_->setRtScene(rtScene_.get());
        for (settings::Setting* s : {rs.lightingEnabled, rs.lightingStyle, rs.lightingDebug, rs.lightingFullLit, rs.lightingShadows, rs.lightingShadowQuality, rs.nativeStaticOpsCompare, rs.nativeSceneOpsCompare}) {
            if (s != nullptr) reg.onChange(*s, [this](const settings::Setting&) { applyLighting(); }, this);
        }
        applyLighting();
    }
    if (rs.giEnabled != nullptr) {
        for (settings::Setting* s : {rs.giEnabled, rs.giQuality, rs.giStrength, rs.giLightmaps, rs.giScreen, rs.giFoliage, rs.giDebug, rs.giReference, rs.giReferenceSpp, rs.giReferenceMax, rs.giReferenceBounces}) {
            if (s != nullptr) reg.onChange(*s, [this](const settings::Setting&) {
                applyGi();
                applyGbuffer();  // the screen probes need the G-buffer
            }, this);
        }
        if (rs.giEnabled->asBool()) applyGi();
    }
    // the thin G-buffer, motion vectors and GTAO (enh/gbuffer.h, docs/research/rtgi.md 10)
    gbuffer_ = std::make_unique<enh::Gbuffer>(dev_, rtScene_.get());
    if (rs.aoEnabled != nullptr) {
        for (settings::Setting* s : {rs.aoEnabled, rs.aoMode, rs.aoStrength, rs.gbufferDebug}) {
            if (s != nullptr) reg.onChange(*s, [this](const settings::Setting&) { applyGbuffer(); }, this);
        }
        applyGbuffer();
    }
    // camera jitter / FSR (enh/upscale.h, docs/research/upscaling.md)
    upscale_ = std::make_unique<enh::Upscale>(dev_);
    objectMotion_ = std::make_unique<enh::ObjectMotion>(dev_, rtScene_.get());
    if (rs.upscaleMode != nullptr) {
        for (settings::Setting* s : {rs.upscaleMode, rs.upscaleProvider, rs.upscaleObjectMotion, rs.upscaleReactive, rs.upscaleSharpness, rs.upscaleExposure,
                                     rs.upscaleJitterFlip, rs.upscaleAlphaDither, rs.upscaleCutoutThicken}) {
            if (s != nullptr) reg.onChange(*s, [this](const settings::Setting&) { applyUpscale(); }, this);
        }
        applyUpscale();
    }
    // frame generation (enh/framegen.h, docs/research/frame-generation.md)
    framegen_ = std::make_unique<enh::FrameGeneration>(dev_);
    if (rs.framegenMode != nullptr) {
        for (settings::Setting* s : {rs.framegenMode, rs.framegenManual, rs.framegenDebug}) {
            if (s != nullptr) reg.onChange(*s, [this](const settings::Setting&) { applyFramegen(); }, this);
        }
        applyFramegen();
    }
    // detail distances (engine/lod.h, docs/research/lod.md)
    for (settings::Setting* s : {rs.lodTrees, rs.lodMeshes, rs.lodView}) {
        if (s != nullptr) reg.onChange(*s, [this](const settings::Setting&) { applyLod(); }, this);
    }
    applyLod();
    if (rs.materialSources != nullptr && rs.ubershader != nullptr && rs.ubershaderSkip != nullptr) {
        engine::matsrc::setResolver(&HalHLSLShader::shaderOf);
        const auto applyMaterials = [this, dir = options.cacheDir]() {
            const auto& r = settings::renderer();
            const bool dump = r.materialSources->asBool();
            const bool uber = r.ubershader->asBool();
            // the requests are collected for the dump and for the GLSL material shaders; the file only for the dump
            engine::matsrc::setOutput(dump && !dir.empty() ? dir / "materials" / "sources.jsonl" : std::filesystem::path());
            engine::matsrc::setEnabled(dump || uber || gpuscene::materialtable().active());  // (the material table's graph ids)
            dev_.setUbershaderSkip(static_cast<std::uint32_t>(r.ubershaderSkip->asInt()));
            dev_.setUbershader(uber);
        };
        applyMaterials();
        for (settings::Setting* s : {rs.materialSources, rs.ubershader, rs.ubershaderSkip}) {
            reg.onChange(*s, [applyMaterials](const settings::Setting&) { applyMaterials(); }, this);
        }
    }
    if (rs.shadowStride != nullptr) {
        engine::shadowmaps::setUpdateStride(static_cast<int>(rs.shadowStride->asInt()));
        reg.onChange(*rs.shadowStride, [](const settings::Setting& s) { engine::shadowmaps::setUpdateStride(static_cast<int>(s.asInt())); }, this);
    }
    engine::loadaccel::setReadCacheKiB(frozen::kReadCacheKiB);
    engine::loadaccel::setAbsorbMode(frozen::kAbsorbMode);
    if (rs.streamStats != nullptr) {
        engine::loadaccel::setStats(static_cast<int>(rs.streamStats->asInt()));
        reg.onChange(*rs.streamStats, [](const settings::Setting& s) { engine::loadaccel::setStats(static_cast<int>(s.asInt())); }, this);
    }
    if (rs.prefetchStride != nullptr) {
        engine::lod::setPrefetchStride(static_cast<int>(rs.prefetchStride->asInt()));
        reg.onChange(*rs.prefetchStride, [](const settings::Setting& s) { engine::lod::setPrefetchStride(static_cast<int>(s.asInt())); }, this);
    }
    if (options.overlay && dev_.presentValid()) {
        overlay_ = std::make_unique<overlay::Overlay>();
        overlay::Options oo;
        oo.intoBackBuffer = options.overlayInScreenshots;
        oo.commands = options.overlayCommands;
        oo.settingsFile = options.settingsFile;
        dev_.setOverlay(overlay_.get(), oo.intoBackBuffer);  // first: the overlay asks the device for its colour format
        if (!overlay_->init(dev_, p.window, oo)) {
            dev_.setOverlay(nullptr, false);
            overlay_.reset();
            spdlog::warn("hal: the settings overlay is not available this session");
        }
    }
    crashdump::deviceCreated();  // (14.26: from the window's WM_CLOSE on, a process termination waits for this device's shutdown)
    // 14.26: the engine destroys its window before it releases the device; AMD's frame generation swapchain goes at WM_CLOSE, while the window exists
    overlay::setWindowCloseHandler([](void* self) {
        auto* d = static_cast<HalDevice*>(self);
        if (d->framegen_) d->framegen_->windowClosing();
    }, this);
    return true;
}

void HalDevice::applyWater() {
    const auto& rs = settings::renderer();
    if (!water_ || rs.waterEnabled == nullptr) return;
    enh::Water::Settings s;
    s.enabled = rs.waterEnabled->asBool();
    const std::string_view q = rs.waterQuality->asEnum();
    s.quality = q == "low" ? 0 : q == "high" ? 2 : 1;
    s.ssr = rs.waterReflections->asEnum() == "ssr";
    const std::string_view oc = rs.waterOcean->asEnum();
    s.ocean = oc == "off" ? 0 : oc == "gerstner" ? 1 : 2;
    s.oceanWaves = s.ocean != 0;
    const std::string_view oq = rs.waterOceanQuality->asEnum();
    s.oceanQuality = oq == "low" ? 0 : oq == "high" ? 2 : 1;
    const std::string_view ss = rs.waterSeaState->asEnum();
    s.seaState = ss == "calm" ? 0 : ss == "moderate" ? 1 : ss == "rough" ? 2 : ss == "storm" ? 3 : -1;
    s.windDirection = static_cast<int>(rs.waterWindDirection->asInt());
    s.waveHeight = rs.waterWaveScale->asFloat();
    s.murkiness = frozen::kWaterMurkiness;
    s.reflections = frozen::kWaterReflection;
    // rt.reflections (effective value: off while the ray tracing tier is unavailable)
    if (rs.rtReflections != nullptr) {
        const std::string_view rt = rs.rtReflections->asEnum();
        s.rtTier = rt == "low" ? 1 : rt == "medium" ? 2 : rt == "high" ? 3 : 0;
    }
    if (rs.rtReflectionTweaks != nullptr) s.rtTweaks = static_cast<int>(rs.rtReflectionTweaks->asInt());
    if (rtScene_ != nullptr) rtScene_->setBatchHeuristic((s.rtTweaks & 64) != 0);
    const std::string_view d = rs.waterDebug->asEnum();
    s.debug = d == "shore_field" ? 1 : d == "depth" ? 2 : d == "normals" ? 3 : d == "reflection" ? 4 : d == "foam" ? 5 : d == "sea_floor" ? 6 : d == "waves" ? 7 : d == "reflection_source" ? 8 : d == "reflection_shaded" ? 9 : 0;
    water_->apply(s);
    // the enhanced water reflects the engine's sky cube everywhere: keep it current (the whole cube every frame; the
    // engine's own refresh, one face per frame every ~10 s, left lightning flashes baked into some faces: ocean.md 9.8)
    dev_.setSkyCubeEveryFrame(s.enabled);
}

void HalDevice::applyGi() {
    const auto& rs = settings::renderer();
    if (!gi_ || rs.giEnabled == nullptr) return;
    enh::Gi::Settings s;
    s.enabled = rs.giEnabled->asBool();
    const std::string_view q = rs.giQuality->asEnum();
    s.quality = q == "low" ? 0 : q == "high" ? 2 : 1;
    s.strength = rs.giStrength->asFloat();
    s.replaceLightmaps = rs.giLightmaps->asEnum() == "replace";
    s.characters = frozen::kGiCharacters;
    s.bleed = frozen::kGiBleed;
    s.directional = frozen::kGiDirectional;
    if (rs.giFoliage != nullptr) {
        const std::string_view fo = rs.giFoliage->asEnum();
        s.foliage = fo == "off" ? 0 : fo == "far" ? 2 : 1;
    }
    s.shadowFill = frozen::kGiShadowFill;
    s.cutoutsTested = frozen::kGiCutoutsTested;
    s.emissive = frozen::kGiEmissive;
    s.lightCount = frozen::kGiLightCount;
    if (rs.giReference != nullptr) {
        static const char* const kViews[] = {"off", "reference", "ddgi", "screen", "error", "reference_indirect", "screen_indirect", "error_ddgi"};
        const std::string_view rv = rs.giReference->asEnum();
        for (int i = 0; i < 8; ++i) {
            if (rv == kViews[i]) s.reference = i;
        }
        s.referenceSpp = static_cast<int>(rs.giReferenceSpp->asInt());
        s.referenceMax = static_cast<int>(rs.giReferenceMax->asInt());
        s.referenceBounces = static_cast<int>(rs.giReferenceBounces->asInt());
    }
    s.lights = frozen::kGiLightsWorld;
    const std::string_view d = rs.giDebug->asEnum();
    s.debug = d == "irradiance" ? 1 : d == "cascades" ? 2 : d == "vertex_ao" ? 3 : 0;
    if (rs.giScreen != nullptr) {
        const std::string_view sc = rs.giScreen->asEnum();
        s.screen = sc == "low" ? 1 : sc == "medium" ? 2 : sc == "high" ? 3 : 0;
    }
    gi_->apply(s);
}

void HalDevice::applyMaterials() {
    const auto& rs = settings::renderer();
    const auto index = [](std::string_view v, std::initializer_list<std::string_view> names) {
        float i = 0;
        for (const auto n : names) {
            if (n == v) return i;
            ++i;
        }
        return 0.0f;
    };
    const float quality = index(rs.materialsQuality->asEnum(), {"off", "low", "medium", "high"});
    const float debug = index(rs.materialsDebug->asEnum(), {"off", "roughness", "metalness", "orm", "coverage"});
    dev_.removePasses(&matPack_);
    if (const int n = rs.materialsAlternate != nullptr ? rs.materialsAlternate->asInt() : 0; n > 0) {
        // the variants on / off every n frames, decided at the start of each frame's main view (the whole frame uses one state)
        dev_.addPass(d3d9::inject::Point::BeforeDepthPrepass, "materials.alternate", [this, n](d3d9::PassContext&) {
            dev_.setPackBypass(((dev_.frameValue() / static_cast<std::uint64_t>(n)) & 1u) != 0);
        }, &matPack_, 0);
    } else {
        dev_.setPackBypass(false);
    }
    if (matPack_ && rs.materialsDerive != nullptr) {
        enh::matpack::DeriveOptions d;
        d.enabled = rs.materialsDerive->asBool();
        d.minRoughness = frozen::kDeriveMin;
        d.maxRoughness = frozen::kDeriveMax;
        const auto& cur = matPack_->deriveOptions();
        if (cur.enabled != d.enabled || cur.minRoughness != d.minRoughness || cur.maxRoughness != d.maxRoughness) {
            matPack_->setDerive(d);
            spdlog::info("enh: materials: derive from specular {} (roughness {:.2f} .. {:.2f})", d.enabled ? "on" : "off", d.minRoughness, d.maxRoughness);
        }
    }
    dev_.setFrameParam(shader::abi::kFrameParamMaterials + 1, {1.0f, 0.0f, 0.0f, 0.0f});
    // materials.quality = high: the traced reflections (enh/material_rt) carry the sky too: the raster's sky cube term is off while they run
    if (materialRt_) {
        enh::MaterialRt::Settings m;
        const std::string_view tier = rs.materialsRtReflections != nullptr ? rs.materialsRtReflections->asEnum() : std::string_view("medium");
        m.tier = quality >= 3.0f ? (tier == "low" ? 1 : tier == "high" ? 3 : 2) : 0;
        m.environment = frozen::kMaterialEnvironment;
        if (rs.materialsRtRange != nullptr) m.rangeMetres = rs.materialsRtRange->asFloat();
        m.strength = frozen::kMaterialRtStrength;
        if (rs.materialsGlass != nullptr) m.glass = rs.materialsGlass->asBool();
        if (rs.materialsRainWetness != nullptr) {
            m.rain = rs.materialsRainWetness->asFloat();
            m.puddles = m.rain * frozen::kPuddleShare;  // one "Rain" setting
        }
        if (rs.materialsRainForce != nullptr) m.forceRain = rs.materialsRainForce->asFloat();
        materialRt_->apply(m);
    }
    const float environment = materialRt_ && materialRt_->active() ? 0.0f : frozen::kMaterialEnvironment;
    dev_.setFrameParam(shader::abi::kFrameParamMaterials, {quality, frozen::kMaterialSpecular, environment, debug});
    spdlog::info("enh: materials: quality {}, specular {:.2f}, environment {:.2f}, debug {}", rs.materialsQuality->asEnum(), frozen::kMaterialSpecular,
                 frozen::kMaterialEnvironment, rs.materialsDebug->asEnum());
}

void HalDevice::applyLighting() {
    const auto& rs = settings::renderer();
    if (!lightTable_ || rs.lightingEnabled == nullptr) return;
    const std::string_view d = rs.lightingDebug->asEnum();
    lightTable_->configure(rs.lightingEnabled->asBool(), rs.lightingStyle->asEnum() == "physical" ? 1 : 0, d == "clusters" ? 1 : d == "slots" ? 2 : d == "shadows" ? 3 : d == "bake" ? 4 : d == "prebake" ? 5 : d == "path" ? 6 : 0,
                           frozen::kMaterialRoughness);
    lightTable_->setCollectRadius(frozen::kLightCollectRadius);
    {
        enh::LightTable::ShadowSettings ls;
        const std::string_view m = rs.lightingShadows->asEnum();
        ls.mode = renderMode() == RenderMode::Full ? (m == "nearby" ? 1 : m == "many" ? 2 : 0) : 0;
        const std::string_view q = rs.lightingShadowQuality->asEnum();
        ls.quality = q == "low" ? 1 : q == "high" ? 3 : 2;
        ls.radius = frozen::kLampRadiusCm;
        ls.baked = frozen::kLampShadowBaked;
        lightTable_->configureShadows(ls);
    }
    // table lighting needs the table, Full's composition and no compare run (a compare diffs against the engine's passes)
    const auto on = [](const settings::Setting* s) { return s != nullptr && s->asBool(); };
    const bool full = on(rs.lightingFullLit) && rs.lightingEnabled->asBool() && lightTable_->available() && renderMode() == RenderMode::Full &&
                      !on(rs.nativeStaticOpsCompare) && !on(rs.nativeSceneOpsCompare);
    enh::setFullLitActive(full);
    enh::setFullLitOptions(frozen::kTrustTable, frozen::kTreeMerge);
    if (staticOps_) staticOps_->setTableLit(full);
}

void HalDevice::applyGbuffer() {
    const auto& rs = settings::renderer();
    if (!gbuffer_ || rs.aoEnabled == nullptr) return;
    enh::Gbuffer::Settings s;
    s.ao = rs.aoEnabled->asBool();
    s.replaceEngineSsao = rs.aoMode == nullptr || rs.aoMode->asEnum() != "add";
    s.aoStrength = rs.aoStrength->asFloat();
    s.aoRadius = frozen::kAoRadiusMetres * 100.0f;
    const std::string_view d = rs.gbufferDebug->asEnum();
    s.debug = d == "normals" ? 1 : d == "motion" ? 2 : d == "ao" ? 3 : d == "screen_gi" ? 4 : d == "material" ? 5 : 0;
    // who reads what (no unread outputs): the screen probes (enh/gi) and the AO take only the variants (their normals come from the
    // depth); FSR (enh/upscale) and frame generation (enh/framegen) read the motion; the traced material reflections read the normal +
    // the pack's roughness / metalness and the motion (their filter's reprojection)
    const bool screenProbes = rs.giEnabled != nullptr && rs.giEnabled->asBool() && rs.giScreen != nullptr && rs.giScreen->asEnum() != "off";
    const bool fsr = upscale_ && enh::usesFsr(upscale_->mode());
    const bool fg = framegen_ && framegen_->wantsGbuffer();
    const bool matRt = materialRt_ && materialRt_->tier() > 0;
    s.forced = screenProbes || fsr || fg || matRt;
    s.needMotion = fsr || fg || matRt;
    s.needNormal = matRt;
    gbuffer_->apply(s);
}

// exclusive fullscreen is the player's choice unless AMD's frame generation swapchain presents (its real swapchain is private: no
// application controlled exclusive mode): borderless then, with a log line
void HalDevice::applyFullscreen() {
    const auto& rs = settings::renderer();
    if (rs.fullscreen == nullptr) return;
    bool exclusive = rs.fullscreen->asEnum() == "exclusive";
    if (exclusive && framegen_ && framegen_->wantsSwapchain()) {
        exclusive = false;
        if (!exclusiveBlockedLogged_) {
            exclusiveBlockedLogged_ = true;
            spdlog::info("hal: exclusive fullscreen is not available with frame generation (AMD's swapchain): borderless window");
        }
    }
    engine::window::setExclusive(exclusive);
}

void HalDevice::applyFramegen() {
    const auto& rs = settings::renderer();
    if (!framegen_ || rs.framegenMode == nullptr) return;
    enh::FrameGeneration::Settings s;
    const std::string_view m = rs.framegenMode->asEnum();  // the effective value: fsr4 applies as fsr3 where it is not offered
    s.mode = m == "fsr3" ? enh::FrameGenMode::Fsr3 : m == "fsr4" ? enh::FrameGenMode::Fsr4 : enh::FrameGenMode::Off;
    s.manual = rs.framegenManual != nullptr && rs.framegenManual->asBool();
    const std::string_view v = rs.framegenDebug != nullptr ? rs.framegenDebug->asEnum() : std::string_view("off");
    s.view = v == "motion" ? enh::FrameGenView::Motion
             : v == "depth" ? enh::FrameGenView::Depth
             : v == "hudless" ? enh::FrameGenView::Hudless
             : v == "generated" ? enh::FrameGenView::Generated
                                : enh::FrameGenView::Off;
    framegen_->apply(s);
    applyGbuffer();  // the motion vectors
    applyFullscreen();
}

void HalDevice::applyUpscale() {
    const auto& rs = settings::renderer();
    if (!upscale_ || rs.upscaleMode == nullptr) return;
    const std::string_view m = rs.upscaleMode->asEnum();
    if (rs.upscaleSharpness != nullptr) upscale_->setSharpness(rs.upscaleSharpness->asFloat());
    dev_.setPostAtDisplaySize(true);
    dev_.setAlphaDither(rs.upscaleAlphaDither != nullptr && rs.upscaleAlphaDither->asBool());
    dev_.setCutoutThicken(rs.upscaleCutoutThicken != nullptr ? rs.upscaleCutoutThicken->asFloat() : 0.0f);
    if (rs.upscaleReactive != nullptr) upscale_->setReactive(rs.upscaleReactive->asBool());
    if (rs.upscaleProvider != nullptr) upscale_->setMl(rs.upscaleProvider->asEnum() == "fsr4");
    if (rs.upscaleExposure != nullptr) upscale_->setEngineExposure(rs.upscaleExposure->asEnum() == "engine");
    upscale_->setReactiveTuning(frozen::kReactiveCutoff, frozen::kReactiveScale);
    unsigned flip = 0;
    if (rs.upscaleJitterFlip != nullptr) {
        const std::string_view f = rs.upscaleJitterFlip->asEnum();
        flip = f == "x" ? 1u : f == "y" ? 2u : f == "xy" ? 3u : 0u;
    }
    enh::UpscaleMode mode = enh::UpscaleMode::Off;
    if (m == "native_aa") mode = enh::UpscaleMode::NativeAa;
    else if (m == "high_quality") mode = enh::UpscaleMode::HighQuality;
    else if (m == "quality") mode = enh::UpscaleMode::Quality;
    else if (m == "balanced") mode = enh::UpscaleMode::Balanced;
    else if (m == "performance") mode = enh::UpscaleMode::Performance;
    else if (m == "ultra_performance") mode = enh::UpscaleMode::UltraPerformance;
    else if (m == "jitter_only") mode = enh::UpscaleMode::JitterOnly;
    upscale_->apply(mode, flip);
    if (objectMotion_) {
        const std::string_view om = rs.upscaleObjectMotion != nullptr ? rs.upscaleObjectMotion->asEnum() : std::string_view("characters");
        const bool fsr = enh::usesFsr(mode);
        objectMotion_->apply(fsr && om != "off", fsr && om == "all");
    }
    applyGbuffer();  // native AA needs the motion vectors
}

void HalDevice::applyShadows() {
    const auto& rs = settings::renderer();
    if (!shadows_ || rs.shadowsEnabled == nullptr) return;
    enh::Shadows::Settings s;
    s.enabled = rs.shadowsEnabled->asBool();
    const std::string_view f = rs.shadowsFilter->asEnum();
    s.taps = f == "low" ? 2 : f == "medium" ? 3 : 4;
    s.softness = frozen::kShadowSoftness;
    s.blend = frozen::kShadowBlend;
    s.fade = frozen::kShadowFade;
    s.bias = rs.shadowsBias->asFloat();
    const std::string_view dbg = rs.shadowsDebug->asEnum();
    s.debug = dbg == "cascades" ? 1 : dbg == "cascade_term" ? 2 : dbg == "terrain_term" ? 3 : dbg == "rt_term" ? 4 : dbg == "rt_coverage" ? 5 : 0;
    shadows_->apply(s);
    if (rtShadows_ && rs.shadowsRt != nullptr) {
        // the ray-traced term replaces the cascades' mask inside its range: only through the enhanced mask
        enh::RtShadows::Settings r;
        const std::string_view m = rs.shadowsRt->asEnum();
        r.mode = s.enabled ? (m == "near" ? 1 : m == "extended" ? 2 : 0) : 0;
        r.rangeMetres = rs.shadowsRtRange->asFloat();
        r.softness = frozen::kRtShadowSoftness;
        r.wind = frozen::kRtShadowWind;
        r.sync = rs.rtShadowSync->asBool();
        const std::string_view q = rs.shadowsRtQuality->asEnum();
        r.quality = q == "low" ? 1 : q == "high" ? 3 : 2;
        r.opaqueFirst = rs.rtReflectionTweaks != nullptr && (rs.rtReflectionTweaks->asInt() & 128) != 0;
        rtShadows_->apply(r);
    }
    // the engine's cascades (engine/shadow_maps.h): "auto" = the enhanced defaults while the enhanced mask is on
    engine::shadowmaps::Overrides o;
    const std::string_view size = rs.shadowsMapSize->asEnum();
    o.mapSize = size == "auto" ? (s.enabled ? 4096 : 0) : size == "game" ? 0 : std::atoi(std::string(size).c_str());
    const float metres = rs.shadowsRange->asFloat();
    o.range = metres > 0.0f ? metres * 100.0f : (s.enabled ? 10000.0f : 0.0f);
    o.splitFactor = o.range > 0.0f ? frozen::kShadowSplit : -1.0f;
    engine::shadowmaps::setOverrides(o);
    spdlog::info("hal: settings: shadow cascades: map size {}, range {}, split factor {}", o.mapSize > 0 ? std::to_string(o.mapSize) : "game",
                 o.range > 0.0f ? std::format("{:.0f} m", o.range / 100.0f) : std::string("game"),
                 o.splitFactor >= 0.0f ? std::format("{:.2f}", o.splitFactor) : std::string("game"));
}

void HalDevice::applyLod() {
    const auto& rs = settings::renderer();
    if (rs.lodTrees == nullptr) return;
    engine::lod::Scales s;
    s.trees = rs.lodTrees->asFloat();
    s.meshes = rs.lodMeshes->asFloat();
    s.view = rs.lodView->asFloat();
    engine::lod::setScales(s);
    dev_.setBillboardFadeScale(s.trees);
    spdlog::info("hal: settings: detail distances: trees x{:.2f}, objects x{:.2f}, view range x{:.2f}", s.trees, s.meshes, s.view);
}

void HalDevice::applyVsync() {
    const auto& rs = settings::renderer();
    if (rs.vsync == nullptr) return;
    const std::string_view v = rs.vsync->asEnum();
    const bool on = v == "on" ? true : v == "off" ? false : engineVsync_;
    if (on != dev_.vsync()) {
        spdlog::info("hal: settings: vsync {} ({})", on ? "on" : "off", v == "engine" ? "the game's option" : "player setting");
    }
    dev_.setVsync(on);
}

// render thread, before each present: setting callbacks, debounced saving of settings.toml
void HalDevice::serviceSettings() {
    auto& reg = settings::registry();
    reg.dispatch();
    if (!reg.dirty() || options_.settingsFile.empty()) {
        settingsDirtySince_ = {};
        return;
    }
    const auto now = std::chrono::steady_clock::now();
    if (settingsDirtySince_ == std::chrono::steady_clock::time_point{}) {
        settingsDirtySince_ = now;
    } else if (now - settingsDirtySince_ >= std::chrono::seconds(1)) {
        std::string error;
        if (reg.save(options_.settingsFile, &error)) {
            spdlog::info("settings: saved {}", api::utf8(options_.settingsFile));
        } else {
            spdlog::warn("settings: saving {} failed: {}", api::utf8(options_.settingsFile), error);
            reg.clearDirty();
        }
        settingsDirtySince_ = {};
    }
}

void HalDevice::setRenderState(std::uint32_t rs, std::uint32_t value) {
    if (rs >= RS_COUNT) return;
    if (blockRecording_ && !blockSaved_[rs]) {
        blockSaved_[rs] = true;
        blockValues_[rs] = dev_.renderState(rs);
    }
    dev_.setRenderState(rs, value);
}

void HalDevice::unbindTextures() {
    for (std::uint32_t s = 0; s < 16; ++s) {
        dev_.setTexture(s, nullptr);
    }
}

d3d9::VertexDecl* HalDevice::declFromHandle(void* handle) { return static_cast<d3d9::VertexDecl*>(handle); }

// ---- resources ------------------------------------------------------------------------------------------------------

bEResult HalDevice::CreatePrimititve(eCAPIPrimitive** a0) {
    if (a0 == nullptr) return bEResult_False;
    *a0 = new HalPrimitive(*this);
    return bEResult_Ok;
}

bEResult HalDevice::CreateIndexData(eCGfxShared::eSGfxIndexBufDesc const& a0, eCAPIIndexData** a1) {
    if (a1 == nullptr) return bEResult_False;
    auto b = dev_.createBuffer(a0.length, a0.usage, a0.pool, true, a0.format == 102 /* D3DFMT_INDEX32 */);
    *a1 = new HalIndexData(*this, a0, std::move(b));
    return bEResult_Ok;
}

namespace {

// "Engine.dll+0x1234 < Engine.dll+0x5678 < ..." for the callers of the current function (diagnostics)
std::string callerChain(int skip, int frames) {
    void* stack[16]{};
    const USHORT n = RtlCaptureStackBackTrace(static_cast<DWORD>(skip + 1), static_cast<DWORD>(std::min(frames, 16)), stack, nullptr);
    std::string s;
    for (USHORT i = 0; i < n; ++i) {
        HMODULE mod = nullptr;
        GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           static_cast<LPCWSTR>(stack[i]), &mod);
        char name[MAX_PATH]{};
        if (mod != nullptr) GetModuleFileNameA(mod, name, MAX_PATH);
        const char* base = std::strrchr(name, '\\') != nullptr ? std::strrchr(name, '\\') + 1 : name;
        s += fmt::format("{}{}+0x{:x}", i ? " < " : "", mod != nullptr ? base : "?",
                         reinterpret_cast<std::uintptr_t>(stack[i]) - reinterpret_cast<std::uintptr_t>(mod));
    }
    return s;
}

}  // namespace

bEResult HalDevice::CreateVertexData(eCGfxShared::eSGfxVertexBufDesc const& a0, eCAPIVertexData** a1) {
    if (a1 == nullptr) return bEResult_False;
    // eCDX9VertexData 0x74dcf0: the stride is D3DXGetFVFVertexSize(fvf) for FVF buffers, desc.stride only when fvf == 0
    std::uint32_t stride = a0.stride;
    if (a0.fvf != 0) {
        stride = fvfVertexSize(a0.fvf);
        static std::atomic<int> logged{0};
        if (logged.fetch_add(1) < 2) {
            spdlog::info("hal: FVF vertex buffer 0x{:x} ({} bytes, stride {}, desc.stride {}) from {}", a0.fvf, a0.length, stride,
                         a0.stride, callerChain(1, 6));
        }
    }
    auto b = dev_.createBuffer(a0.length, a0.usage, a0.pool, false, false);
    *a1 = new HalVertexData(*this, a0, stride, std::move(b));
    return bEResult_Ok;
}

d3d9::VertexDecl* HalDevice::fvfDecl(std::uint32_t fvf) {
    const std::lock_guard lock(handleMutex_);
    if (auto it = fvfDecls_.find(fvf); it != fvfDecls_.end()) {
        return it->second.get();
    }
    const auto elements = fvfDeclaration(fvf);
    if (elements.empty()) {
        spdlog::warn("hal: invalid FVF 0x{:x}", fvf);
        return nullptr;
    }
    auto decl = dev_.createVertexDecl(elements);
    d3d9::VertexDecl* p = decl.get();
    fvfDecls_.emplace(fvf, std::move(decl));
    return p;
}

bEResult HalDevice::GetFVFSize(unsigned long a0, unsigned long* a1) {
    if (a1 == nullptr) return bEResult_False;
    *a1 = fvfVertexSize(static_cast<std::uint32_t>(a0));
    return bEResult_Ok;
}

bEResult HalDevice::PreprocessShader(eCGfxShared::eSGfxHLSLShaderSrc const& a0, bCString& a1) {
    using Fn = bEResult (*)(void*, const eCGfxShared::eSGfxHLSLShaderSrc*, bCString*);
    const Fn fn = dx9<Fn>("?PreprocessShader@eCDX9Device@@UEAA?AW4bEResult@@AEBUeSGfxHLSLShaderSrc@eCGfxShared@@AEAVbCString@@@Z");
    return fn != nullptr ? fn(dx9Standin_, &a0, &a1) : bEResult_False;
}

bEResult HalDevice::CompileEffect(eCGfxShared::eSGfxEffectSource const& a0, void** a1, void** a2) {
    using Fn = bEResult (*)(void*, const eCGfxShared::eSGfxEffectSource*, void**, void**);
    const Fn fn = dx9<Fn>("?CompileEffect@eCDX9Device@@UEAA?AW4bEResult@@AEBUeSGfxEffectSource@eCGfxShared@@PEAPEAX1@Z");
    return fn != nullptr ? fn(dx9Standin_, &a0, a1, a2) : bEResult_False;
}

bEResult HalDevice::CompileEffectFromFile(eCGfxShared::eSGfxEffectFileSource const& a0, void** a1, void** a2) {
    using Fn = bEResult (*)(void*, const eCGfxShared::eSGfxEffectFileSource*, void**, void**);
    const Fn fn = dx9<Fn>("?CompileEffectFromFile@eCDX9Device@@UEAA?AW4bEResult@@AEBUeSGfxEffectFileSource@eCGfxShared@@PEAPEAX1@Z");
    return fn != nullptr ? fn(dx9Standin_, &a0, a1, a2) : bEResult_False;
}

bEResult HalDevice::CreateAuxBuffer(unsigned long a0, void** a1) {
    using Fn = bEResult (*)(void*, unsigned long, void**);
    const Fn fn = dx9<Fn>("?CreateAuxBuffer@eCDX9Device@@UEAA?AW4bEResult@@KPEAPEAX@Z");
    return fn != nullptr ? fn(dx9Standin_, a0, a1) : bEResult_False;
}

bEResult HalDevice::DestroyAuxBuffer(void* a0) {
    using Fn = bEResult (*)(void*, void*);
    const Fn fn = dx9<Fn>("?DestroyAuxBuffer@eCDX9Device@@UEAA?AW4bEResult@@PEAX@Z");
    return fn != nullptr ? fn(dx9Standin_, a0) : bEResult_False;
}

bEResult HalDevice::GetAuxBuffer(void* a0, void** a1, unsigned long* a2) {
    using Fn = bEResult (*)(void*, void*, void**, unsigned long*);
    const Fn fn = dx9<Fn>("?GetAuxBuffer@eCDX9Device@@UEAA?AW4bEResult@@PEAXPEAPEAXPEAK@Z");
    return fn != nullptr ? fn(dx9Standin_, a0, a1, a2) : bEResult_False;
}

bEResult HalDevice::CreateHLSLShader(eCAPIHLSLShader** a0) {
    if (a0 == nullptr) return bEResult_False;
    *a0 = new HalHLSLShader(*this);
    return bEResult_Ok;
}

namespace {

TextureDesc textureDesc(const eCGfxShared::eSGfxLayersDesc& d, TextureType type) {
    TextureDesc t;
    t.type = type;
    t.width = std::max(1u, d.width);
    t.height = std::max(1u, type == TextureType::Cube ? d.width : d.height);
    t.depth = std::max(1u, d.depth);
    t.levels = d.levels;
    t.format = d.format;
    t.usage = d.usage;
    t.pool = d.pool;
    return t;
}

}  // namespace

bEResult HalDevice::CreateTexture(eCGfxShared::eSGfxLayersDesc const& a0, eCAPITexture** a1) {
    if (a1 == nullptr) return bEResult_False;
    auto t = dev_.createTexture(textureDesc(a0, TextureType::Tex2D));
    if (!t) {
        *a1 = nullptr;
        return bEResult_False;
    }
    *a1 = new HalTexture(*this, std::move(t));
    return bEResult_Ok;
}

bEResult HalDevice::CreateCubeMap(eCGfxShared::eSGfxLayersDesc const& a0, eCAPICubeMap** a1) {
    if (a1 == nullptr) return bEResult_False;
    auto t = dev_.createTexture(textureDesc(a0, TextureType::Cube));
    if (!t) {
        *a1 = nullptr;
        return bEResult_False;
    }
    *a1 = new HalCubeMap(*this, std::move(t));
    return bEResult_Ok;
}

bEResult HalDevice::CreateVolume(eCGfxShared::eSGfxLayersDesc const& a0, eCAPIVolume** a1) {
    if (a1 == nullptr) return bEResult_False;
    auto t = dev_.createTexture(textureDesc(a0, TextureType::Volume));
    if (!t) {
        *a1 = nullptr;
        return bEResult_False;
    }
    *a1 = new HalVolume(*this, std::move(t));
    return bEResult_Ok;
}

std::unique_ptr<d3d9::Texture> HalDevice::textureFromDds(const char* what, const void* data, std::uint32_t size, std::uint32_t skip,
                                                        int levelsWanted, std::uint32_t pool, std::uint32_t usage, int wantCube,
                                                        eCGfxShared::eSGfxLayersDesc& out) {
    std::string err;
    const auto dds = res::parseDds({static_cast<const std::uint8_t*>(data), size}, &err);
    if (!dds) {
        static std::atomic<int> logged{0};
        if (logged.fetch_add(1) < 10) spdlog::warn("hal: {}: {} ({} bytes)", what, err, size);
        return nullptr;
    }
    if (dds->volume || (wantCube >= 0 && dds->cube != (wantCube != 0))) {
        spdlog::warn("hal: {}: a {} DDS file is not supported here", what, dds->volume ? "volume" : dds->cube ? "cube" : "2D");
        return nullptr;
    }
    // skip `skip` top levels; levels <= 0: all of the file's
    if (skip >= dds->mips) skip = dds->mips - 1;
    std::uint32_t levels = dds->mips - skip;
    if (levelsWanted > 0) levels = std::min(levels, static_cast<std::uint32_t>(levelsWanted));
    TextureDesc td;
    td.type = dds->cube ? TextureType::Cube : TextureType::Tex2D;
    td.width = std::max(1u, dds->width >> skip);
    td.height = std::max(1u, dds->height >> skip);
    td.levels = levels;
    td.format = dds->d3dFormat;
    td.usage = usage;
    td.pool = pool;
    auto t = dev_.createTexture(td);
    if (!t) {
        return nullptr;
    }
    for (std::uint32_t face = 0; face < dds->faces; ++face) {
        for (std::uint32_t l = 0; l < levels; ++l) {
            if (const auto lv = dds->level(face, l + skip)) {
                t->setLevelData(l, face, lv->bytes, lv->rowPitch);
            }
        }
    }
    out.width = td.width;
    out.height = td.height;
    out.levels = levels;
    out.format = td.format;
    out.cubeMap = dds->cube;
    return t;
}

bEResult HalDevice::CreateImageFromMemory(eCGfxShared::eEPoolType a0, int a1, unsigned char a2, void const* a3, unsigned int a4,
                                          eCGfxShared::eSGfxLayersDesc& a5, void*& a6) {
    // levels -1 / -2: from the file for DDS (eCDX9Device::CreateImageFromMemory 0x73f030); skip a2 top levels
    a6 = nullptr;
    auto t = textureFromDds("CreateImageFromMemory", a3, a4, a2, a1, static_cast<std::uint32_t>(a0), 0, -1, a5);
    if (!t) {
        return bEResult_False;
    }
    a5.pool = static_cast<std::uint32_t>(a0);
    if (a5.cubeMap) {
        a6 = static_cast<eCAPICubeMap*>(new HalCubeMap(*this, std::move(t)));
    } else {
        a6 = static_cast<eCAPITexture*>(new HalTexture(*this, std::move(t)));
    }
    return bEResult_Ok;
}

namespace {

// eCDX9Texture / eCDX9CubeMap "from memory" ctors (0x74c650 / 0x73caa0): D3DXCreate(Cube)TextureFromFileInMemoryEx with the
// descriptor's size, levels, usage, format, pool, filter BOX | (skip << 26). We load DDS files as stored: a size or format
// that would need D3DX's resampling / conversion is refused. Returns the skip count, or -1 when refused.
int fromMemoryRequest(const char* what, const eCGfxShared::eSGfxLayersDesc& d, unsigned char skipLevels, const void* data,
                      unsigned int size) {
    constexpr std::uint32_t kDefault = 0xffffffffu, kDefaultNonPow2 = 0xfffffffeu, kFromFile = 0xfffffffdu;
    // D3D9: levels in (0, skip] clamp the skip to levels - 1
    std::uint32_t skip = skipLevels;
    if (d.levels != 0 && d.levels != kFromFile && d.levels <= skip) skip = d.levels - 1;
    const auto dds = res::parseDds({static_cast<const std::uint8_t*>(data), size});
    if (!dds) return static_cast<int>(skip);  // textureFromDds logs the reason
    const std::uint32_t s = std::min(skip, dds->mips - 1);
    const auto sizeOk = [](std::uint32_t want, std::uint32_t file) {
        return want == 0 || want == kDefault || want == kDefaultNonPow2 || want == kFromFile || want == file;
    };
    const bool formatOk = d.format == 0 || d.format == kFromFile || d.format == dds->d3dFormat;
    if (!sizeOk(d.width, std::max(1u, dds->width >> s)) || !sizeOk(d.height, std::max(1u, dds->height >> s)) || !formatOk ||
        (d.usage & (USAGE_RENDERTARGET | USAGE_DEPTHSTENCIL)) != 0) {
        spdlog::warn("hal: {}: {}x{} {} usage 0x{:x} from a {}x{} {} file needs D3DX resampling / conversion: not supported", what,
                     d.width, d.height, rhi::d3dFormatName(d.format), d.usage, dds->width, dds->height,
                     rhi::d3dFormatName(dds->d3dFormat));
        return -1;
    }
    return static_cast<int>(skip);
}

int levelsRequest(std::uint32_t levels) {
    // 0 / D3DX_DEFAULT / D3DX_FROM_FILE: the file's chain
    return levels == 0 || levels >= 0xfffffffdu ? 0 : static_cast<int>(levels);
}

}  // namespace

bEResult HalDevice::CreateTextureFromMemory(eCGfxShared::eSGfxLayersDesc& a0, unsigned char a1, void* a2, unsigned int a3,
                                            eCAPITexture** a4) {
    if (a4 == nullptr) return bEResult_False;
    *a4 = nullptr;
    const int skip = fromMemoryRequest("CreateTextureFromMemory", a0, a1, a2, a3);
    if (skip < 0) return bEResult_False;
    auto t = textureFromDds("CreateTextureFromMemory", a2, a3, static_cast<std::uint32_t>(skip), levelsRequest(a0.levels), a0.pool,
                            a0.usage & USAGE_DYNAMIC, 0, a0);
    if (!t) return bEResult_False;
    *a4 = new HalTexture(*this, std::move(t));
    return bEResult_Ok;
}

bEResult HalDevice::CreateCubeMapFromMemory(eCGfxShared::eSGfxLayersDesc& a0, unsigned char a1, void* a2, unsigned int a3,
                                            eCAPICubeMap** a4) {
    if (a4 == nullptr) return bEResult_False;
    *a4 = nullptr;
    const int skip = fromMemoryRequest("CreateCubeMapFromMemory", a0, a1, a2, a3);
    if (skip < 0) return bEResult_False;
    auto t = textureFromDds("CreateCubeMapFromMemory", a2, a3, static_cast<std::uint32_t>(skip), levelsRequest(a0.levels), a0.pool,
                            a0.usage & USAGE_DYNAMIC, 1, a0);
    if (!t) return bEResult_False;
    *a4 = new HalCubeMap(*this, std::move(t));
    return bEResult_Ok;
}

bEResult HalDevice::GetImageInfoFromMemory(void const* a0, unsigned int a1, eCGfxShared::eSGfxImageInfo& a2) {
    const auto dds = res::parseDds({static_cast<const std::uint8_t*>(a0), a1});
    if (!dds) return bEResult_False;
    a2.width = dds->width;
    a2.height = dds->height;
    a2.depth = dds->depth;
    a2.mipLevels = dds->mips;
    a2.format = dds->d3dFormat;
    a2.resourceType = dds->cube ? 5u : dds->volume ? 4u : 3u;
    a2.imageFileFormat = 4;  // D3DXIFF_DDS
    return bEResult_Ok;
}

bEResult HalDevice::CheckDeviceFormat(eCGfxShared::eSGfxLayersDesc const& a0, bool a1) {
    const bEResult r = checkFormat(a0, a1);
    spdlog::debug("hal: CheckDeviceFormat {} (0x{:x}) {}x{} usage 0x{:x} blend {}: {}", rhi::d3dFormatName(a0.format), a0.format,
                  a0.width, a0.height, a0.usage, a1, r == bEResult_Ok ? "yes" : "no");
    return r;
}

bEResult HalDevice::checkFormat(eCGfxShared::eSGfxLayersDesc const& a0, bool a1) {
    using namespace rhi::d3dfmt;
    const std::uint32_t f = a0.format;
    if (f == NULLFMT) {
        return bEResult_Ok;
    }
    // Shadow maps (eCShaderMaterialAdmin::PostInitialize 0x6ba9d0 probes R32F, R16F, D16, D24X8, DF16, DF24 in this order and
    // takes the last supported one; docs/research/engine-gfx.md 5). The ATI depth textures DF16 / DF24 are sampled as a plain
    // depth value (no compare, no fetch4 in post_SSM.fx): exactly a sampled Vulkan depth image, the D3D9 reference path on
    // AMD. D16 / D24 depth textures would need NVIDIA's implicit hardware-PCF compare on tex2Dproj and INTZ is not used:
    // reported unsupported. Without DF24 the engine falls back to R16F colour maps (16-bit float depth: striped shadows).
    if (f == DF16 || f == DF24) {
        if ((a0.usage & USAGE_DEPTHSTENCIL) == 0 || a0.depth > 1 || a0.cubeMap) {
            return bEResult_False;
        }
        const auto info = rhi::formatInfo(f, dev_.d24s8Format());
        if (!info) return bEResult_False;
        return dev_.game().formatSupport(info->format, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT) ? bEResult_Ok
                                                                                                                                   : bEResult_False;
    }
    if (f == INTZ || (a0.usage & USAGE_DEPTHSTENCIL) != 0) {
        return bEResult_False;
    }
    const auto info = rhi::formatInfo(f, dev_.d24s8Format());
    if (!info || info->depth) {
        return bEResult_False;
    }
    VkImageUsageFlags need = VK_IMAGE_USAGE_SAMPLED_BIT;
    if ((a0.usage & USAGE_RENDERTARGET) != 0) need |= VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    return dev_.game().formatSupport(info->format, need, (a0.usage & USAGE_RENDERTARGET) != 0 && a1) ? bEResult_Ok : bEResult_False;
}

bEResult HalDevice::CreateVertexDeclaration(eCGfxShared::eSGfxDclElement const* a0, void** a1) {
    if (a0 == nullptr || a1 == nullptr) return bEResult_False;
    std::size_t n = 0;
    while (n < 64 && a0[n].stream != 0xFF) ++n;
    auto decl = dev_.createVertexDecl({reinterpret_cast<const VertexElement*>(a0), n + 1});
    void* handle = decl.get();
    const std::lock_guard lock(handleMutex_);
    decls_.emplace(handle, std::move(decl));
    *a1 = handle;
    return bEResult_Ok;
}

bEResult HalDevice::DestroyVertexDeclaration(void* a0) {
    const std::lock_guard lock(handleMutex_);
    if (auto it = decls_.find(a0); it != decls_.end()) {
        dev_.release(std::move(it->second));
        decls_.erase(it);
    }
    return bEResult_Ok;
}

bEResult HalDevice::GetVertexDeclarationData(void* a0, eCGfxShared::eSGfxDclElement* a1, unsigned int* a2) {
    auto* decl = declFromHandle(a0);
    if (decl == nullptr || a2 == nullptr) return bEResult_False;
    const auto& e = decl->elements();
    const unsigned int count = static_cast<unsigned int>(e.size() + 1);  // incl. D3DDECL_END
    if (a1 != nullptr) {
        std::memcpy(a1, e.data(), e.size() * sizeof(VertexElement));
        a1[e.size()] = {0xFF, 0, 17, 0, 0, 0};
    }
    *a2 = count;
    return bEResult_Ok;
}

bEResult HalDevice::SetVertexDeclaration(void* a0) {
    HAL_LOG("hal> SetVertexDeclaration {}", a0);
    dev_.setVertexDecl(declFromHandle(a0));
    return bEResult_Ok;
}

bEResult HalDevice::CreateDepthBuffer(unsigned long a0, unsigned long a1, eCGfxShared::eEDepthFormat a2, void** a3) {
    if (a3 == nullptr) return bEResult_False;
    TextureDesc d;
    d.width = static_cast<std::uint32_t>(a0);
    d.height = static_cast<std::uint32_t>(a1);
    d.format = static_cast<std::uint32_t>(a2);
    d.usage = USAGE_DEPTHSTENCIL;
    d.pool = POOL_DEFAULT;
    auto t = dev_.createTexture(d);
    if (!t) return bEResult_False;
    d3d9::Surface* s = t->surface(0, 0);
    const std::lock_guard lock(handleMutex_);
    depthBuffers_.emplace(s, std::move(t));
    *a3 = s;
    return bEResult_Ok;
}

bEResult HalDevice::DestroyDepthBuffer(void* a0) {
    const std::lock_guard lock(handleMutex_);
    if (auto it = depthBuffers_.find(a0); it != depthBuffers_.end()) {
        dev_.release(std::move(it->second));
        depthBuffers_.erase(it);
    }
    return bEResult_Ok;
}

// ---- modes / frame --------------------------------------------------------------------------------------------------

bEResult HalDevice::ToggleNewMode(eCAPIDevice::eSAPIModeParameters const& a0) {
    spdlog::info("hal: ToggleNewMode {}x{} @ {} Hz (windowed {}, vsync {})", a0.backBufferWidth, a0.backBufferHeight, a0.refreshRate,
                 a0.windowed, a0.vsync);
    engineVsync_ = a0.vsync;
    applyVsync();
    if (!dev_.resize(a0.backBufferWidth, a0.backBufferHeight)) return bEResult_False;
    engine::window::onModeSet(window_, a0.windowed, a0.backBufferWidth, a0.backBufferHeight, a0.refreshRate);
    return bEResult_Ok;
}

bEResult HalDevice::IsResetNecessary(bool* a0) {
    if (a0 != nullptr) *a0 = true;  // eCDX9Device: true while the device is fine
    return bEResult_Ok;
}

bEResult HalDevice::IsResetNeeded(bool* a0) {
    if (a0 != nullptr) *a0 = false;
    return bEResult_Ok;
}

bEResult HalDevice::IsDeviceNotReset(bool* a0) {
    if (a0 != nullptr) *a0 = false;
    return bEResult_Ok;
}

bEResult HalDevice::Reset() { return bEResult_Ok; }

bEResult HalDevice::SetViewport(eCGfxShared::eSGfxViewport const& in) {
    // N5.5c: a shadow map's viewport (native/effect_ops_setup.cpp: compared, with take-over the composed one)
    eCGfxShared::eSGfxViewport a0 = in;
    if (effectOps_ && effectOps_->setupOn()) {
        native::fxc::Viewport v{static_cast<std::uint32_t>(a0.left), static_cast<std::uint32_t>(a0.top), static_cast<std::uint32_t>(a0.right - a0.left),
                                static_cast<std::uint32_t>(a0.bottom - a0.top), a0.minZ, a0.maxZ};
        effectOps_->setupViewport(v);
        a0.left = static_cast<std::int32_t>(v.x);
        a0.top = static_cast<std::int32_t>(v.y);
        a0.right = static_cast<std::int32_t>(v.x + v.width);
        a0.bottom = static_cast<std::int32_t>(v.y + v.height);
        a0.minZ = v.minZ;
        a0.maxZ = v.maxZ;
    }
    Viewport vp;
    vp.x = static_cast<std::uint32_t>(std::max(0, a0.left));
    vp.y = static_cast<std::uint32_t>(std::max(0, a0.top));
    const std::int32_t w = a0.right - a0.left, h = a0.bottom - a0.top;
    vp.width = w <= 0 ? 32u : static_cast<std::uint32_t>(w);  // eCDX9Device: a zero size becomes 32
    vp.height = h <= 0 ? 32u : static_cast<std::uint32_t>(h);
    vp.minZ = a0.minZ;
    vp.maxZ = a0.maxZ;
    dev_.setViewport(vp);
    return bEResult_Ok;
}

bEResult HalDevice::GetViewport(eCGfxShared::eSGfxViewport& a0) {
    const Viewport& vp = dev_.viewport();
    a0.left = static_cast<std::int32_t>(vp.x);
    a0.top = static_cast<std::int32_t>(vp.y);
    a0.right = static_cast<std::int32_t>(vp.x + vp.width);
    a0.bottom = static_cast<std::int32_t>(vp.y + vp.height);
    a0.minZ = vp.minZ;
    a0.maxZ = vp.maxZ;
    return bEResult_Ok;
}

bEResult HalDevice::ClearViewport(eCGfxShared::eEViewportClear a0, unsigned long a1, float a2, unsigned long a3) {
    HAL_LOG("hal> Clear flags {} color {:08x} z {}", static_cast<int>(a0), a1, a2);
    auto flags = static_cast<std::uint32_t>(a0), colour = static_cast<std::uint32_t>(a1), stencil = static_cast<std::uint32_t>(a3);
    if (effectOps_ && effectOps_->setupOn()) effectOps_->setupClear(flags, colour, a2, stencil);
    dev_.clear(flags, colour, a2, stencil);
    return bEResult_Ok;
}

bEResult HalDevice::SwapScreen(HWND__*, bCRect const*, bCRect const*) {
    // [debug] cpu_profile: the engine thread is sampled from the end of one present to the start of the next
    struct ProfileWindow {
        diag::StallSampler* s = diag::frameSampler();
        ProfileWindow() { if (s) s->leave(); }
        ~ProfileWindow() { if (s) s->enter(); }
    } profileWindow;
    const std::uint64_t frame = dev_.stats().frames + 1;
    serviceSettings();
    // The gpuscene tables record their uploads into this frame's upload list, which runs before the frame's draws in the same submission. They
    // flush FIRST at the present: everything below may submit mid-frame (the captures' back-buffer read-back, frame generation's BeforePresent pass /
    // context drop on a swapchain change, a window resize), and a table upload recorded after such a submission lands behind the draws that read it
    // (stale rows / indirect args: GPU page fault, frame-ownership.md 7.35).
    engine::scenetable::onPresent();
    native::f10::onPresent();
    native::f10req::onPresent();
    native::f10x::onPresent();
    {
        gpuscene::instances().flush(dev_, frozen::kGpuScene >= 2 ? frozen::kGpuScene : 0);
        gpuscene::illumtable().flush(dev_, true);
        gpuscene::lightingimages().flush(dev_, true);  // after the illumination table's flush: the compose pass reads this frame's records (frame-ownership.md 7.9)
        gpuscene::meshparts().flush(dev_, true);
        gpuscene::materialtable().flush(dev_, true);
        gpuscene::windowtables().flush(dev_);
        gpuscene::viewtable().flush(dev_);
        gpuscene::drawrecords().flush(dev_);
        gpuscene::clusterstore().flush(dev_);  // F5-1 (before the compaction: F5-2's cull reads the cluster table and the object table's rows)
        gpuscene::objecttable().flush(dev_);
        gpuscene::rowtable().flush(dev_);  // F2f (frame-ownership.md 14.19): the owned instance-list draws' window 2 images
        gpuscene::bonetable().flush(dev_);  // F6-1: the owned actors' bone images (before the frame's draws, with the other tables)
        dev_.boneMotionFrame();         // 14.13: bone-history motion (compare readback, the 5 s line)
        dev_.captureCensusFrame();      // 14.15: the vertex-capture census (`native: captures (5 s)`)
        gpuscene::ownedcompact().flush(dev_);  // F2c-4c: after the record table's upload (same list, barrier between)
        gpuscene::bbtable().flush(dev_);
    }
    if (overlay_) {
        overlay_->onPresent(frame);
        // [debug] overlay_in_screenshots: into the back buffer, before the captures read it
        dev_.drawOverlayIntoBackBuffer();
    }
    // the menu / loading / pause state of this present: frame generation runs in the world only (and the pacer follows it below)
    updateMenuPacing();
    // frame generation: the interpolated frame of this present is computed now, so that captures can read it too
    dev_.inject(d3d9::inject::Point::BeforePresent);
    HAL_LOG("hal> SwapScreen");
    // borderless / exclusive fullscreen: a pending window placement / mode correction, the window kept on its monitor,
    // exclusive fullscreen's display mode and Vulkan exclusive mode (docs/research/window-mode.md)
    const engine::window::Presentation wp = engine::window::onPresent();
    if (wp.displayModeChanged) dev_.displayModeChanged();
    dev_.setRefreshCap(wp.refreshCap);
    // vulkan.exclusive_acquire = false (diagnostics): the display mode change without Vulkan's exclusive swapchain
    const auto* acquireSetting = settings::renderer().exclusiveAcquire;
    const bool acquire = acquireSetting == nullptr || acquireSetting->asBool();
    if (!dev_.setFullScreenExclusive(acquire ? wp.exclusiveMonitor : nullptr, acquire && wp.holdExclusive) && wp.resized) {
        dev_.windowResized();
    }
    engine::matsrc::onPresent();
    engine::xinput::onPresent();
    engine::loadaccel::onPresent();
    auto& captures = api::captures();
    // [debug] screenshot_frames: a request of the tool API's queue, due at this present
    if (std::ranges::find(options_.screenshotFrames, static_cast<std::uint32_t>(frame)) != options_.screenshotFrames.end()) {
        captures.request(options_.screenshotDir / std::format("frame_{}.png", frame), api::ImageFormat::Png, 0, nullptr);
    }
    // rvk_screenshot: the finished back buffer (scene + post + GUI) before it is shown, as D3D9's Present hook / DoSnapshot see it.
    // The copy rides in the present's own submission and is read after it (frame-ownership.md 14.38): a read-back with its own mid-frame submit
    // (readBackBuffer's flushFrameAndWait) made the frame value jump, and the frames after every capture were disturbed for ~20 frames.
    // RVK_CAPTURE_FLUSH=1 = the old mid-frame read-back (A/B).
    static const bool flushCapture = GetEnvironmentVariableA("RVK_CAPTURE_FLUSH", nullptr, 0) != 0;
    const bool armed = !flushCapture && captures.dueNextPresent() && dev_.armBackBufferRead();
    const auto readBack = [this, armed](std::vector<std::uint8_t>& bgra, std::uint32_t& w, std::uint32_t& h) {
        return armed ? dev_.takeBackBufferRead(bgra, w, h) : dev_.readBackBuffer(bgra, w, h);
    };
    if (framegen_) framegen_->armGeneratedRead(armed);  // (its _gen.png: the same in-frame copy)
    const auto readGenerated = [this](std::vector<std::uint8_t>& bgra, std::uint32_t& w, std::uint32_t& h) {
        return framegen_ && framegen_->readGenerated(bgra, w, h);  // the manual frame generation's frame_<n>_gen.png
    };
    if (!armed) captures.onPresent(frame, readBack, readGenerated);
    engine::lod::onPresent();  // a changed object draw distance: the world's cache-in spheres (engine thread)
    dev_.present();
    writeSnapshots();  // the engine's snapshots of this frame (SaveBackbufferToFile), read after the present's submission
    if (armed) captures.onPresent(frame, readBack, readGenerated);
    const auto& s = dev_.stats();
    gpuscene::setVerifyFrame(s.frames);  // 7.27: the next frame's number (the owned path's check frames: frame % native.owned_check == 0)
    api::publishPresent(s.frames, api::FrameNumbers{
                                      .drawsLastFrame = s.lastFrameDraws,
                                      .unimplementedSlots = static_cast<std::uint32_t>(unimplementedCount()),
                                      .drawsTotal = s.totalDraws,
                                      .drawsSkippedTotal = s.skippedNoShader + s.skippedCompiling + s.skippedNoTarget + s.skippedVertexInput +
                                                           s.skippedPipeline,
                                      .textureCpuBytes = d3d9::Texture::cpuBytesTotal(),
                                  });
    if (const auto& cam = dev_.lastCamera(); cam.valid) {
        ++cameraFrames_;
        if (cam.prevValid && cam.position != cam.prevPosition) ++cameraMovedFrames_;
    }
    if (logThisFrame()) logFrameStructure();  // the -LogFrame frame, now complete
    updateLogFrame();
    logFrameStats();
    return bEResult_Ok;
}

// A menu page caps the frame rate at vulkan.menu_fps; the loading screen is never paced: the engine loads on this thread and
// presents after every load step, so each pacer wait would lengthen the load (docs/research/menu-fps.md)
void HalDevice::updateMenuPacing() {
    const engine::menu::Status st = engine::menu::query();
    const auto& rs = settings::renderer();
    const auto* menuFps = rs.menuFps;
    dev_.setUnpaced(st.state == engine::menu::State::Loading);
    // frame generation: the world only (Unknown = the GUI is not up yet: the world), not paused (app state 1)
    if (framegen_) framegen_->setGameState(st.state == engine::menu::State::Game || st.state == engine::menu::State::Unknown, st.appState == 1);
    dev_.setMenuCap(st.state == engine::menu::State::Menu && menuFps != nullptr ? menuFps->value() : 0.0);
    if (static_cast<int>(st.state) != menuState_) {
        menuState_ = static_cast<int>(st.state);
        spdlog::info("engine: menu state {} (pages {:#x}, app state {:#x}{}) at present {}: {}", engine::menu::name(st.state),
                     st.pages, st.appState, st.faulted ? ", the query faulted" : "", dev_.stats().frames + 1,
                     dev_.unpaced()                           ? std::string("unpaced")
                     : dev_.menuCap() > 0.0                   ? std::format("frame rate limit {} fps", dev_.menuCap())
                                                              : std::string("no menu limit"));
    }
}

// The last presented frame's phase sequence and main camera (docs/research/frame-phases.md), every 5 s and for -LogFrame
void HalDevice::logFrameStructure() {
    const auto& ph = dev_.phases();
    spdlog::info("hal: frame {} structure ({} draws{}): {}", dev_.stats().frames, ph.lastDraws(),
                 ph.unbalanced() != 0 ? std::format(", {} unbalanced markers", ph.unbalanced()) : std::string(),
                 d3d9::scene::PhaseTracker::summary(ph.lastFrame()));
    const auto& dr = dev_.drawRecords();
    spdlog::info("hal: frame {} draw kinds ({} records{}{}): {}", dev_.stats().frames, dr.last().size(),
                 dr.lastDropped() != 0 ? std::format(", {} dropped", dr.lastDropped()) : std::string(),
                 dr.lastHistogram().tinted != 0 ? std::format(", {} tinted", dr.lastHistogram().tinted) : std::string(),
                 dr.lastHistogram().summary());
    // untagged draws of camera views: a draw path without a hook (first three of the frame)
    int unknown = 0;
    for (const auto& r : dr.last()) {
        if (r.kind != draws::Kind::Unknown || unknown++ >= 3) continue;
        spdlog::info("hal:   untagged draw #{} in {}/{}: vs {:016x} ({}) ps {:016x} ({}), {} primitives, type {}, flags 0x{:x}, "
                     "vertex buffer {}#{}", r.index, d3d9::scene::name(r.view), d3d9::scene::name(r.pass), r.vsHash,
                     flagNames(r.vsFlags), r.psHash, flagNames(r.psFlags), r.primitives, r.primitiveType, r.flags,
                     r.vertexBuffer.id, r.vertexBuffer.generation);
    }
    const auto& c = dev_.lastCamera();
    if (!c.valid) {
        spdlog::info("hal: frame {}: no main view", dev_.stats().frames);
        return;
    }
    const float moved = c.prevValid ? std::hypot(c.position[0] - c.prevPosition[0], c.position[1] - c.prevPosition[1],
                                                 c.position[2] - c.prevPosition[2])
                                    : 0.0f;
    // the view direction: the inverse view's third row (bCMatrix rows = axes, D3D row-vector convention)
    spdlog::info("hal: camera of frame {}: position ({:.2f}, {:.2f}, {:.2f}), forward ({:.3f}, {:.3f}, {:.3f}), near {:.2f}, far {:.1f} "
                 "(engine {:.1f}), t {:.3f} s, moved {:.3f} since the previous frame{}; the camera moved in {} of {} frames with a "
                 "main view since the last report",
                 c.frame, c.position[0], c.position[1], c.position[2], c.inverseView.at(2, 0), c.inverseView.at(2, 1),
                 c.inverseView.at(2, 2), c.nearZ, c.farZ, c.engineFar, c.time, moved, c.prevValid ? "" : " (no previous camera)",
                 cameraMovedFrames_, cameraFrames_);
    cameraMovedFrames_ = 0;
    cameraFrames_ = 0;
    const d3d9::scene::CameraCheck k = d3d9::scene::checkCamera(c);
    if (k.available) {
        spdlog::info("hal: camera check against the first solid draw's VS constants: c0-c3 = view-projection {} max |d| {:.2g} "
                     "(matrix scale {:.3g}), c4-c7 inverse projection {:.2g}, c8-c10 inverse view {:.2g}, c11.xyz position {:.2g}, "
                     "c11.w {:.1f} (engine far {:.1f}, |d| {:.2g})",
                     k.transposed ? "(transposed)" : "(rows)", k.viewProjection, k.scale, k.inverseProjection, k.inverseView,
                     k.position, c.vsConstants[11][3], c.engineFar, k.farPlane);
    }
}

// The engine's snapshot (eCApplication::Snapshot / SnapshotEx -> Draw -> DoSnapshot, after the GUI and before SwapScreen).
// eCDX9Device: GetRenderTarget(0) -> D3DXSaveSurfaceToFileInMemory(D3DXIFF_BMP) -> eCArchiveManager file, whatever the
// extension (missing-slots.md 3). Here: read the back buffer, resolve the virtual path through the engine's VFS, refuse
// anything inside <game>\data (junctions into the game install), write the BMP D3DX writes (32 bpp, byte for byte).
bEResult HalDevice::SaveBackbufferToFile(char* const a0) {
    std::string error;
    const auto file = engine::physicalPath(a0, &error);
    if (!file) {
        spdlog::warn("hal: SaveBackbufferToFile('{}'): {}", a0 != nullptr ? a0 : "", error);
        return bEResult_False;
    }
    if (const auto refused = api::refuseDataPath(*file, api::gameDir())) {
        spdlog::warn("hal: SaveBackbufferToFile('{}'): refused, {}", a0, *refused);
        return bEResult_False;
    }
    if (dev_.renderTarget(0) != dev_.backBuffer()) {
        spdlog::warn("hal: SaveBackbufferToFile: render target 0 is not the back buffer; saving the back buffer");
    }
    // The copy rides in the frame's own submission and the file is written after the present (writeSnapshots; DoSnapshot runs right before SwapScreen and
    // only logs a failure): a read-back with its own mid-frame submit disturbed the frames after it (frame-ownership.md 14.38 / 14.40).
    // RVK_CAPTURE_FLUSH=1 = the old mid-frame read-back (A/B).
    static const bool flushCapture = GetEnvironmentVariableA("RVK_CAPTURE_FLUSH", nullptr, 0) != 0;
    if (!flushCapture && dev_.backBuffer() != nullptr) {
        auto p = std::make_unique<PendingSnapshot>();
        p->enginePath = a0;
        p->file = *file;
        p->frame = dev_.stats().frames + 1;
        if (dev_.armBackBufferReadInto(p->read)) {
            snapshots_.push_back(std::move(p));
            return bEResult_Ok;
        }
    }
    std::vector<std::uint8_t> bgra;
    std::uint32_t w = 0, h = 0;
    if (!dev_.readBackBuffer(bgra, w, h)) {
        spdlog::warn("hal: SaveBackbufferToFile: back-buffer read-back failed");
        return bEResult_False;
    }
    const std::vector<std::uint8_t> bmp = api::encodeBmpLikeD3DX(bgra, w, h);
    if (bmp.empty() || !api::writeFileReplace(*file, bmp, &error)) {
        spdlog::warn("hal: SaveBackbufferToFile({}): {}", api::utf8(*file), error.empty() ? "encoding failed" : error);
        return bEResult_False;
    }
    spdlog::info("hal: snapshot '{}' -> {} ({}x{}, frame {})", a0, api::utf8(*file), w, h, dev_.stats().frames + 1);
    return bEResult_Ok;
}

void HalDevice::writeSnapshots() {
    if (snapshots_.empty()) return;
    auto due = std::move(snapshots_);
    snapshots_.clear();
    for (auto& p : due) {
        std::vector<std::uint8_t> bgra;
        if (!dev_.takeImageRead(p->read, bgra)) {
            spdlog::warn("hal: SaveBackbufferToFile('{}'): back-buffer read-back failed", p->enginePath);
            continue;
        }
        const std::uint32_t w = p->read.width, h = p->read.height;
        std::string error;
        const std::vector<std::uint8_t> bmp = api::encodeBmpLikeD3DX(bgra, w, h);
        if (bmp.empty() || !api::writeFileReplace(p->file, bmp, &error)) {
            spdlog::warn("hal: SaveBackbufferToFile({}): {}", api::utf8(p->file), error.empty() ? "encoding failed" : error);
            continue;
        }
        spdlog::info("hal: snapshot '{}' -> {} ({}x{}, frame {})", p->enginePath, api::utf8(p->file), w, h, p->frame);
    }
}

void HalDevice::logFrameStats() {
    const auto& s = dev_.stats();
    const auto now = std::chrono::steady_clock::now();
    // perf windows (core/perf_window.h, `perf window <frames> <tag>`): the stats interval closes right before a window's first frame and right after its
    // last, so the interval reports between the window's begin / end markers cover exactly its frames
    std::string windowTag;
    const perfwin::Edge edge = perfwin::afterPresent(s.frames, &windowTag);
    if (edge == perfwin::Edge::End) spdlog::info("perf: window {} closing at frame {}", windowTag, s.frames);
    if (s.frames == 1 || edge != perfwin::Edge::None || (!perfwin::running() && now - lastStatsTime_ >= std::chrono::seconds(5))) {  // (no 5 s cut inside a window)
        const double secs = std::chrono::duration<double>(now - lastStatsTime_).count();
        const double frames = std::max(1.0, static_cast<double>(s.frames - lastStatsFrame_));
        spdlog::info("hal: {:.1f} fps, {} draws in the last frame; presents {}, acquire failures {}, suboptimal {}; avg acquire "
                     "{:.2f} ms, present {:.2f} ms (present thread; render thread waited {:.2f} ms), pacing {:.2f} ms; loading "
                     "frames not shown {}",
                     static_cast<double>(s.frames - lastStatsFrame_) / secs, s.lastFrameDraws, s.presentsQueued, s.acquireFailures,
                     s.presentSuboptimal, s.acquireMs / frames, s.presentMs / frames, s.presentWaitMs / frames, s.pacerMs / frames,
                     s.loadingFramesDropped);
        dev_.resetTimings();
        logFrameGen(secs);
        if (dev_.perfEnabled()) {
            logPerf(dev_.takePerf());
        }
        lastStatsTime_ = now;
        lastStatsFrame_ = s.frames;
        logFrameStructure();
        if (frameTap_) frameTap_->report();
        if (effectOps_) effectOps_->report();
        spdlog::info("hal: frame {}: {} draws total, {} clears, {} render passes; skipped draws since start: {} no shader, {} while their shader compiled (a running total, not a pending count; "
                     "{} shaders queued now), {} no target, {} vertex input, {} pipeline; HAL Draw calls {}; {} unimplemented slots seen; texture CPU copies {:.1f} MiB; clears folded into load ops {}; stencil-tested draws {} (on a surface without stencil bits {})",
                     s.frames, s.totalDraws, s.totalClears, s.totalRenderPasses, s.skippedNoShader, s.skippedCompiling, dev_.queuedShaderJobs(), s.skippedNoTarget,
                     s.skippedVertexInput, s.skippedPipeline, halDraws_, unimplementedCount(),
                     static_cast<double>(d3d9::Texture::cpuBytesTotal()) / (1024.0 * 1024.0), s.scopeClears, s.stencilPlaneDraws,
                     s.stencilNoPlaneDraws);
    }
    if (edge == perfwin::Edge::Begin) spdlog::info("perf: window {} begin at frame {}", windowTag, s.frames + 1);
    if (edge == perfwin::Edge::End) spdlog::info("perf: window {} end at frame {}", windowTag, s.frames);
}

// frame generation: the rates and the pacing of the last stats interval (the gap statistics come from the sampler thread of [debug] perf)
void HalDevice::logFrameGen(double secs) {
    if (!framegen_ || !framegen_->active()) return;
    const d3d9::Device::FrameGenReport r = dev_.takeFrameGenReport();
    if (r.seconds <= 0.0) return;
    const double base = static_cast<double>(r.basePresents) / r.seconds;
    if (!r.swapchain) {
        spdlog::info("framegen: manual model: {:.1f} rendered frames/s, {:.1f} shown/s ({} generated presented in total)", base,
                     static_cast<double>(r.basePresents) / r.seconds + (dev_.stats().generatedPresented > 0 ? base : 0.0),
                     dev_.stats().generatedPresented);
        return;
    }
    const double out = static_cast<double>(r.realPresents) / r.seconds;
    spdlog::info("framegen: swapchain: base {:.1f} fps -> output {:.1f} fps ({} real presents in {:.1f} s = {:.2f} per rendered frame); "
                 "present callbacks {} ({} generated, {} with the overlay, {:.3f} ms each); generator {}",
                 base, out, r.realPresents, r.seconds, r.basePresents > 0 ? static_cast<double>(r.realPresents) / static_cast<double>(r.basePresents) : 0.0,
                 r.callbacks, r.generated, r.uiFrames, r.callbacks > 0 ? r.callbackMs / static_cast<double>(r.callbacks) : 0.0,
                 dev_.frameGenRunning() ? "running" : "idle (menu / loading / pause)");
    (void)secs;
    if (!r.gapsMs.empty()) {
        const rhi::PresentGapStats g = rhi::presentGapStats(r.gapsMs);
        spdlog::info("perf: framegen present gaps (ms): n {} mean {:.3f} stddev {:.3f} p1 {:.3f} p50 {:.3f} p99 {:.3f} max {:.3f}; every other gap: "
                     "{:.3f} +- {:.3f} / {:.3f} +- {:.3f}; {} stalls (> 100 ms) left out",
                     g.n, g.mean, g.stddev, g.p1, g.p50, g.p99, g.max, g.evenMean, g.evenStd, g.oddMean, g.oddStd, g.stalls);
    }
}

// [debug] perf: one line per stats interval (docs/research/performance.md explains the fields)
void HalDevice::logPerf(d3d9::PerfStats p) {
    if (p.frames == 0) {
        return;
    }
    const double n = static_cast<double>(p.frames);
    auto& iv = p.frameIntervalsMs;
    if (iv.size() >= 16 && p.frameSplits.size() == iv.size()) {
        // frame spikes: intervals above 2x the median (and 4 ms over it), their spacing in frames and where their time went
        std::vector<float> sorted = iv;
        std::ranges::nth_element(sorted, sorted.begin() + sorted.size() / 2);
        const float med = sorted[sorted.size() / 2];
        {
            // F15-1 (frame-ownership.md 14.20): the interval's frame-time distribution (hitch gate: p99 / max)
            std::vector<float> all = iv;
            std::ranges::sort(all);
            const auto at = [&](double q) { return all[std::min(all.size() - 1, static_cast<std::size_t>(q * static_cast<double>(all.size() - 1) + 0.5))]; };
            spdlog::info("perf: frame intervals (ms): n {} p50 {:.2f} p95 {:.2f} p99 {:.2f} max {:.2f}", all.size(), at(0.5), at(0.95), at(0.99), all.back());
        }
        const float limit = std::max(med * 2.0f, med + 4.0f);
        std::size_t count = 0, last = 0, gapSum = 0, gapMin = SIZE_MAX, gapMax = 0;
        double sumIv = 0, sumCpu = 0, sumPresent = 0, sumSlot = 0, nCpu = 0, nPresent = 0, nSlot = 0;
        for (std::size_t i = 1; i < iv.size(); ++i) {
            // interval i = the previous frame's present call + this frame's CPU (present start to present start)
            const auto& s = p.frameSplits[i];
            const float prevPresent = p.frameSplits[i - 1].presentMs;
            if (iv[i] <= limit) {
                nCpu += s.cpuMs;
                nPresent += prevPresent;
                nSlot += s.slotWaitMs;
                continue;
            }
            if (count > 0) {
                const std::size_t g = i - last;
                gapSum += g;
                gapMin = std::min(gapMin, g);
                gapMax = std::max(gapMax, g);
            }
            last = i;
            ++count;
            sumIv += iv[i];
            sumCpu += s.cpuMs;
            sumPresent += prevPresent;
            sumSlot += s.slotWaitMs;
        }
        if (count > 0) {
            const double c = static_cast<double>(count), rest = static_cast<double>(iv.size() - 1 - count);
            spdlog::info("perf: frame spikes: {} of {} intervals > {:.1f} ms (median {:.2f}), gap {} frames (min {} / max {}); spike frames: "
                         "interval {:.2f} ms = CPU {:.2f} (slot wait {:.2f}) + present {:.2f}; other frames: CPU {:.2f} (slot wait {:.2f}) + present {:.2f}",
                         count, iv.size(), limit, med, count > 1 ? std::format("{:.1f}", static_cast<double>(gapSum) / (c - 1)) : std::string("-"),
                         count > 1 ? gapMin : 0, gapMax, sumIv / c, sumCpu / c, sumSlot / c, sumPresent / c, rest > 0 ? nCpu / rest : 0.0,
                         rest > 0 ? nSlot / rest : 0.0, rest > 0 ? nPresent / rest : 0.0);
        }
    }
    std::ranges::sort(iv);
    const auto pct = [&](double q) { return iv.empty() ? 0.0 : static_cast<double>(iv[std::min(iv.size() - 1, static_cast<std::size_t>(q * static_cast<double>(iv.size())))]); };
    const double other = p.cpuFrameMs - p.drawMs - p.readbackMs;
    spdlog::info("perf: {} frames, interval p50 {:.2f} / p95 {:.2f} / max {:.2f} ms; CPU per frame: engine+HAL {:.2f} ms (draw calls {:.2f}, "
                 "read-backs {:.2f} = {} per frame, engine+HAL rest {:.2f}), present {:.2f} ms (submit {:.2f}, slot wait {:.2f}); "
                 "GPU per frame {:.2f} ms (+ uploads {:.3f}) over {:.2f} submissions",
                 p.frames, pct(0.5), pct(0.95), iv.empty() ? 0.0 : static_cast<double>(iv.back()), p.cpuFrameMs / n, p.drawMs / n,
                 p.readbackMs / n, static_cast<double>(p.readbacks) / n, other / n, p.presentMs / n, p.submitMs / n, p.slotWaitMs / n,
                 p.gpuSubmits > 0 ? p.gpuMs / n : 0.0, p.gpuSubmits > 0 ? p.gpuUploadMs / n : 0.0, static_cast<double>(p.gpuSubmits) / n);
    // GPU memory of the renderer (VMA's heap statistics: cheap); the ray tracing tier's buffer usage flags show up here
    dev_.game().logMemoryStats();
    spdlog::info("perf: draw tags + records {:.3f} ms per frame ({:.0f} ns per draw)", p.drawRecordMs / n,
                 p.draws > 0 ? p.drawRecordMs * 1e6 / static_cast<double>(p.draws) : 0.0);
    // enhancement plumbing (docs/research/injection.md): FrameBlock uploads, injection points that ran passes, their CPU time
    spdlog::info("perf: frame blocks {:.2f} per frame; injected passes: {:.2f} points per frame, {:.3f} ms per frame (CPU recording)",
                 static_cast<double>(p.frameBlocks) / n, static_cast<double>(p.injections) / n, p.injectMs / n);
    // stage constants (vulkanize plan.md V1): blocks per stage, bytes written into the ring, blocks without the int / bool part,
    // setter calls that changed nothing on a clean stage
    spdlog::info("perf: stage constants: VS {:.0f} / PS {:.0f} blocks per frame, {:.0f} KiB written, int / bool skipped in {:.0f}, "
                 "{:.0f} unchanged register writes",
                 static_cast<double>(p.constUploads[0]) / n, static_cast<double>(p.constUploads[1]) / n,
                 static_cast<double>(p.constBytes) / n / 1024.0, static_cast<double>(p.constIntBoolSkipped) / n,
                 static_cast<double>(p.constSetsUnchanged) / n);
    {
        // split constants (plan.md V2): per window the blocks written (and reused) per frame, KiB written; draws split / legacy
        const char* vsNames[4] = {"view", "frame", "object", "material"};
        const char* psNames[4] = {"view", "c5", "material", "light"};
        std::string line;
        for (std::uint32_t st = 0; st < 2; ++st) {
            line += st == 0 ? " VS" : "; PS";
            for (std::uint32_t w = 0; w < 4; ++w) {
                if (st == 1 && w == 1) continue;
                line += std::format(" {} {:.0f}+{:.0f} ({:.1f} KiB)", st == 0 ? vsNames[w] : psNames[w],
                                    static_cast<double>(p.splitBlocks[st][w]) / n, static_cast<double>(p.splitReused[st][w]) / n,
                                    static_cast<double>(p.splitBytes[st][w]) / n / 1024.0);
            }
        }
        spdlog::info("perf: split constants (blocks written + reused per frame):{}; draws split {:.0f} / {:.0f}, legacy {:.0f} / {:.0f} "
                     "(VS / PS), verify mismatches {}",
                     line, static_cast<double>(p.splitDraws[0]) / n, static_cast<double>(p.splitDraws[1]) / n,
                     static_cast<double>(p.legacyDraws[0]) / n, static_cast<double>(p.legacyDraws[1]) / n, p.splitMismatches);
    }
    {
        std::string passes;
        for (const auto& [name, ms] : p.injectPassMs) passes += std::format(" {} {:.3f};", name, ms / n);
        if (!passes.empty()) spdlog::info("perf: injected pass CPU (ms per frame):{}", passes);
    }
    spdlog::info("perf: per frame {:.0f} draws ({:.0f} batched in {:.1f} batches), {:.1f} render "
                 "passes, {:.0f} pipeline binds; ring {:.0f} KiB, texture uploads {:.0f} KiB, buffer uploads {:.0f} KiB; in the interval: {} "
                 "pipelines built ({:.1f} ms), {} shaders translated ({} compiled, {:.1f} ms)",
                 static_cast<double>(p.draws) / n, static_cast<double>(p.batchedDraws) / n, static_cast<double>(p.batches) / n,
                 static_cast<double>(p.renderPasses) / n, static_cast<double>(p.pipelineBinds) / n,
                 static_cast<double>(p.ringBytes) / n / 1024.0, static_cast<double>(p.textureUploadBytes) / n / 1024.0,
                 static_cast<double>(p.bufferUploadBytes) / n / 1024.0, p.pipelinesBuilt, p.pipelineMs, p.shadersBuilt, p.shadersCompiled,
                 p.shaderMs);
    if (p.prewarmed > 0) {
        spdlog::info("perf: {} pipelines of earlier runs built in the background ({:.1f} ms)", p.prewarmed, p.prewarmMs);
    }
    if (p.libraries > 0 || p.optimized > 0) {
        spdlog::info("perf: pipeline libraries compiled {} ({:.1f} ms, mostly on loader threads), optimised pipelines {} ({:.1f} ms, "
                     "background)",
                     p.libraries, p.libraryMs, p.optimized, p.optimizeMs);
    }
    if (p.pacerRate > 0.0) {
        const double w = std::max(1.0, static_cast<double>(p.pacer.waits));
        spdlog::info("perf: pacer {:.3f} fps{}: {} of {} frames waited, avg sleep {:.2f} ms + spin {:.3f} ms, timer late avg {:.3f} / "
                     "max {:.3f} ms, release error avg {:.3f} ms, {} cadence restarts",
                     p.pacerRate, p.pacerVblank ? " (vblank-locked)" : "", p.pacer.waits, p.frames, p.pacer.sleptMs / w,
                     p.pacer.spunMs / w, p.pacer.lateMs / w, p.pacer.maxLateMs, p.pacer.releaseErrorMs / w, p.pacer.restarts);
    }
}

bEResult HalDevice::BeginScene() {
    HAL_LOG("hal> BeginScene");
    ++sceneDepth_;
    return bEResult_Ok;
}

bEResult HalDevice::EndScene() {
    if (sceneDepth_ > 0) --sceneDepth_;
    return bEResult_Ok;
}

// ---- master states (hal-structs.md 2.4) ----------------------------------------------------------------------------

bEResult HalDevice::SetMasterRenderMode(eCGfxShared::eEMasterRenderMode a0) {
    master_[M_RENDERMODE] = a0 != 0;
    if (a0 != 0) setRenderState(RS_FILLMODE, static_cast<std::uint32_t>(a0));
    return bEResult_Ok;
}
bEResult HalDevice::GetMasterRenderMode(eCGfxShared::eEMasterRenderMode* a0) {
    if (a0) *a0 = static_cast<eCGfxShared::eEMasterRenderMode>(master_[M_RENDERMODE] ? dev_.renderState(RS_FILLMODE) : 0);
    return bEResult_Ok;
}
bEResult HalDevice::SetMasterShadeMode(eCGfxShared::eEMasterShadeMode a0) {
    master_[M_SHADE] = a0 != 0;
    if (a0 != 0) setRenderState(RS_SHADEMODE, static_cast<std::uint32_t>(a0));
    return bEResult_Ok;
}
bEResult HalDevice::GetMasterShadeMode(eCGfxShared::eEMasterShadeMode* a0) {
    if (a0) *a0 = static_cast<eCGfxShared::eEMasterShadeMode>(master_[M_SHADE] ? dev_.renderState(RS_SHADEMODE) : 0);
    return bEResult_Ok;
}
bEResult HalDevice::SetMasterCullingMode(eCGfxShared::eEMasterCullingMode a0) {
    master_[M_CULL] = a0 != 0;
    if (a0 != 0) setRenderState(RS_CULLMODE, static_cast<std::uint32_t>(a0));
    return bEResult_Ok;
}
bEResult HalDevice::GetMasterCullingMode(eCGfxShared::eEMasterCullingMode* a0) {
    if (a0) *a0 = static_cast<eCGfxShared::eEMasterCullingMode>(master_[M_CULL] ? dev_.renderState(RS_CULLMODE) : 0);
    return bEResult_Ok;
}
bEResult HalDevice::SetMasterDepthTesting(eCGfxShared::eEMasterCmpFunc a0) {
    master_[M_DEPTHFUNC] = a0 != 0;
    if (a0 != 0) setRenderState(RS_ZFUNC, static_cast<std::uint32_t>(a0));
    return bEResult_Ok;
}
bEResult HalDevice::GetMasterDepthTesting(eCGfxShared::eEMasterCmpFunc* a0) {
    if (a0) *a0 = static_cast<eCGfxShared::eEMasterCmpFunc>(master_[M_DEPTHFUNC] ? dev_.renderState(RS_ZFUNC) : 0);
    return bEResult_Ok;
}
bEResult HalDevice::SetMasterDepthWrite(eCGfxShared::eEMasterWrite a0) {
    master_[M_DEPTHWRITE] = a0 != 0;
    if (a0 != 0) setRenderState(RS_ZWRITEENABLE, a0 == 2 ? 1u : 0u);
    return bEResult_Ok;
}
bEResult HalDevice::GetMasterDepthWrite(eCGfxShared::eEMasterWrite* a0) {
    if (a0) {
        *a0 = static_cast<eCGfxShared::eEMasterWrite>(master_[M_DEPTHWRITE] ? (dev_.renderState(RS_ZWRITEENABLE) == 1 ? 2 : 1) : 0);
    }
    return bEResult_Ok;
}
bEResult HalDevice::SetMasterDepthBias(bool a0, float a1, float a2) {
    HAL_LOG("hal> SetMasterDepthBias {} {} {}", a0, a1, a2);
    master_[M_DEPTHBIAS] = a0;
    if (a0) {
        setRenderState(RS_DEPTHBIAS, asDword(a1));
        setRenderState(RS_SLOPESCALEDEPTHBIAS, asDword(a2));
    }
    return bEResult_Ok;
}
bEResult HalDevice::GetMasterDepthBias(bool* a0, float*, float*) {
    if (a0) *a0 = master_[M_DEPTHBIAS];  // eCDX9Device returns no floats
    return bEResult_Ok;
}
void HalDevice::applyStencil(bool enable, const eCGfxShared::eSGfxStencilData& d) {
    setRenderState(RS_STENCILENABLE, enable ? 1u : 0u);
    if (enable) {
        setRenderState(RS_STENCILFAIL, d.failOp);
        setRenderState(RS_STENCILZFAIL, d.depthFailOp);
        setRenderState(RS_STENCILPASS, d.passOp);
        setRenderState(RS_STENCILFUNC, static_cast<std::uint32_t>(d.func));
        setRenderState(RS_STENCILREF, d.ref);
        setRenderState(RS_STENCILMASK, d.mask);
        setRenderState(RS_STENCILWRITEMASK, d.writeMask);
    }
}
bEResult HalDevice::SetMasterStencilTesting(eCGfxShared::eEMasterStencil a0, eCGfxShared::eSGfxStencilData const& a1) {
    master_[M_STENCIL] = a0 != 0;
    if (a0 != 0) applyStencil(a0 == 2, a1);
    return bEResult_Ok;
}
bEResult HalDevice::GetMasterStencilTesting(eCGfxShared::eEMasterStencil* a0, eCGfxShared::eSGfxStencilData* a1) {
    if (a0) *a0 = static_cast<eCGfxShared::eEMasterStencil>(master_[M_STENCIL] ? (dev_.renderState(RS_STENCILENABLE) ? 2 : 1) : 0);
    if (a1 && master_[M_STENCIL]) {
        a1->failOp = dev_.renderState(RS_STENCILFAIL);
        a1->depthFailOp = dev_.renderState(RS_STENCILZFAIL);
        a1->passOp = dev_.renderState(RS_STENCILPASS);
        a1->func = static_cast<eCGfxShared::eECmpFunc>(dev_.renderState(RS_STENCILFUNC));
        a1->ref = dev_.renderState(RS_STENCILREF);
        a1->mask = dev_.renderState(RS_STENCILMASK);
        a1->writeMask = dev_.renderState(RS_STENCILWRITEMASK);
    }
    return bEResult_Ok;
}
bEResult HalDevice::SetMasterBlending(eCGfxShared::eEMasterBlending a0, eCGfxShared::eEMasterBlending a1) {
    master_[M_BLEND] = a0 != 0 && a1 != 0;
    if (master_[M_BLEND]) {
        setRenderState(RS_SRCBLEND, static_cast<std::uint32_t>(a0));
        setRenderState(RS_DESTBLEND, static_cast<std::uint32_t>(a1));
    }
    return bEResult_Ok;
}
bEResult HalDevice::GetMasterBlending(eCGfxShared::eEMasterBlending* a0, eCGfxShared::eEMasterBlending* a1) {
    if (a0) *a0 = static_cast<eCGfxShared::eEMasterBlending>(master_[M_BLEND] ? dev_.renderState(RS_SRCBLEND) : 0);
    if (a1) *a1 = static_cast<eCGfxShared::eEMasterBlending>(master_[M_BLEND] ? dev_.renderState(RS_DESTBLEND) : 0);
    return bEResult_Ok;
}
void HalDevice::applyAlphaTest(bool enable, const eCGfxShared::eSGfxAlphaTestData& d) {
    setRenderState(RS_ALPHATESTENABLE, enable ? 1u : 0u);
    if (enable) {
        setRenderState(RS_ALPHAREF, d.ref);
        setRenderState(RS_ALPHAFUNC, static_cast<std::uint32_t>(d.func));
    }
}
bEResult HalDevice::SetMasterAlphaTesting(eCGfxShared::eEMasterAlphaTest a0, eCGfxShared::eSGfxAlphaTestData const& a1) {
    master_[M_ALPHATEST] = a0 != 0;
    if (a0 != 0) applyAlphaTest(a0 == 2, a1);
    return bEResult_Ok;
}
bEResult HalDevice::GetMasterAlphaTesting(eCGfxShared::eEMasterAlphaTest* a0, eCGfxShared::eSGfxAlphaTestData* a1) {
    if (a0) {
        *a0 = static_cast<eCGfxShared::eEMasterAlphaTest>(master_[M_ALPHATEST] ? (dev_.renderState(RS_ALPHATESTENABLE) ? 2 : 1) : 0);
    }
    if (a1 && master_[M_ALPHATEST]) {
        a1->ref = dev_.renderState(RS_ALPHAREF);
        a1->func = static_cast<eCGfxShared::eECmpFunc>(dev_.renderState(RS_ALPHAFUNC));
    }
    return bEResult_Ok;
}
bEResult HalDevice::SetMasterFogging(eCGfxShared::eEMasterFogging a0) {
    master_[M_FOG] = a0 == 1;
    if (a0 == 1) setRenderState(RS_FOGENABLE, 0);
    return bEResult_Ok;
}
bEResult HalDevice::GetMasterFogging(eCGfxShared::eEMasterFogging* a0) {
    if (a0) *a0 = static_cast<eCGfxShared::eEMasterFogging>(master_[M_FOG] ? 1 : 0);
    return bEResult_Ok;
}

// ---- normal setters (no-ops while their master flag is set) --------------------------------------------------------

bEResult HalDevice::SetRenderMode(eCGfxShared::eERenderMode a0) {
    if (!master_[M_RENDERMODE]) normal(RS_FILLMODE, static_cast<std::uint32_t>(a0));
    return bEResult_Ok;
}
bEResult HalDevice::SetShadeMode(eCGfxShared::eEShadeMode a0) {
    if (!master_[M_SHADE]) normal(RS_SHADEMODE, static_cast<std::uint32_t>(a0));
    return bEResult_Ok;
}
bEResult HalDevice::SetCullingMode(eCGfxShared::eECullingMode a0) {
    if (!master_[M_CULL]) normal(RS_CULLMODE, static_cast<std::uint32_t>(a0));
    return bEResult_Ok;
}
bEResult HalDevice::SetDepthTesting(eCGfxShared::eECmpFunc a0) {
    if (!master_[M_DEPTHFUNC]) normal(RS_ZFUNC, static_cast<std::uint32_t>(a0));
    return bEResult_Ok;
}
bEResult HalDevice::SetDepthWrite(eCGfxShared::eEWrite a0) {
    if (!master_[M_DEPTHWRITE]) normal(RS_ZWRITEENABLE, a0 == 2 ? 1u : 0u);
    return bEResult_Ok;
}
bEResult HalDevice::SetDepthBias(float a0, float a1) {
    HAL_LOG("hal> SetDepthBias {} {}{}", a0, a1, master_[M_DEPTHBIAS] ? " (master set)" : "");
    if (!master_[M_DEPTHBIAS]) {
        normal(RS_DEPTHBIAS, asDword(a0));
        normal(RS_SLOPESCALEDEPTHBIAS, asDword(a1));
    }
    return bEResult_Ok;
}
bEResult HalDevice::SetStencilTesting(eCGfxShared::eEStencil a0, eCGfxShared::eSGfxStencilData const& a1) {
    if (!master_[M_STENCIL]) applyStencil(a0 == 2, a1);
    return bEResult_Ok;
}
bEResult HalDevice::SetBlending(eCGfxShared::eEBlending a0, eCGfxShared::eEBlending a1) {
    if (!master_[M_BLEND]) {
        normal(RS_SRCBLEND, static_cast<std::uint32_t>(a0));
        normal(RS_DESTBLEND, static_cast<std::uint32_t>(a1));
    }
    return bEResult_Ok;
}
bEResult HalDevice::SetAlphaTesting(eCGfxShared::eEAlphaTest a0, eCGfxShared::eSGfxAlphaTestData const& a1) {
    if (!master_[M_ALPHATEST]) applyAlphaTest(a0 == 2, a1);
    return bEResult_Ok;
}
bEResult HalDevice::SetFog(eCGfxShared::eSGfxFogData& a0) {
    if (master_[M_FOG]) return bEResult_Ok;
    normal(RS_FOGENABLE, a0.enable ? 1u : 0u);
    if (a0.enable) {
        normal(RS_RANGEFOGENABLE, a0.rangeFog ? 1u : 0u);
        normal(RS_FOGCOLOR, a0.color);
        normal(a0.vertexFog == 1 ? RS_FOGVERTEXMODE : RS_FOGTABLEMODE, a0.mode);
        normal(a0.vertexFog == 1 ? RS_FOGTABLEMODE : RS_FOGVERTEXMODE, 0);
        normal(RS_FOGSTART, asDword(a0.start));
        normal(RS_FOGEND, asDword(a0.end));
        normal(RS_FOGDENSITY, asDword(a0.density));
    }
    return bEResult_Ok;
}
bEResult HalDevice::SetColorFactor(unsigned long a0) {
    normal(RS_TEXTUREFACTOR, static_cast<std::uint32_t>(a0));
    return bEResult_Ok;
}
bEResult HalDevice::SetLighting(bool a0) {
    normal(RS_LIGHTING, a0 ? 1u : 0u);
    return bEResult_Ok;
}
bEResult HalDevice::GetLighting(bool* a0) {
    if (a0) *a0 = dev_.renderState(RS_LIGHTING) != 0;
    return bEResult_Ok;
}
bEResult HalDevice::SetAlphaBlending(bool a0) {
    normal(RS_ALPHABLENDENABLE, a0 ? 1u : 0u);
    return bEResult_Ok;
}
bEResult HalDevice::GetAlphaBlending(bool* a0) {
    if (a0) *a0 = dev_.renderState(RS_ALPHABLENDENABLE) != 0;
    return bEResult_Ok;
}
bEResult HalDevice::SetColorWriting(bool a0) {
    normal(RS_COLORWRITEENABLE, (dev_.renderState(RS_COLORWRITEENABLE) & 8u) | (a0 ? 7u : 0u));
    return bEResult_Ok;
}
bEResult HalDevice::GetColorWriting(bool* a0) {
    if (a0) *a0 = (dev_.renderState(RS_COLORWRITEENABLE) & 7u) != 0;
    return bEResult_Ok;
}
bEResult HalDevice::SetAlphaWriting(bool a0) {
    normal(RS_COLORWRITEENABLE, (dev_.renderState(RS_COLORWRITEENABLE) & 7u) | (a0 ? 8u : 0u));
    return bEResult_Ok;
}
bEResult HalDevice::GetAlphaWriting(bool* a0) {
    if (a0) *a0 = (dev_.renderState(RS_COLORWRITEENABLE) & 8u) != 0;
    return bEResult_Ok;
}
bEResult HalDevice::SetSpecularEffect(bool a0) {
    normal(RS_SPECULARENABLE, a0 ? 1u : 0u);
    return bEResult_Ok;
}
bEResult HalDevice::GetSpecularEffect(bool* a0) {
    if (a0) *a0 = dev_.renderState(RS_SPECULARENABLE) != 0;
    return bEResult_Ok;
}
bEResult HalDevice::SetGammaRamp(bool, eCGfxShared::eSGfxGammaRamp const* a1) {
    if (a1) gamma_ = *a1;  // windowed D3D9 ignores the gamma ramp as well
    return bEResult_Ok;
}
bEResult HalDevice::GetGammaRamp(eCGfxShared::eSGfxGammaRamp* a0) {
    if (a0) *a0 = gamma_;
    return bEResult_Ok;
}
bEResult HalDevice::EnableClipping(bool a0) {
    normal(RS_CLIPPING, a0 ? 1u : 0u);
    return bEResult_Ok;
}
bEResult HalDevice::GetClippingEnables(bool& a0) {
    a0 = dev_.renderState(RS_CLIPPING) != 0;
    return bEResult_Ok;
}
bEResult HalDevice::SetScissor(bCRect const& a0) {
    dev_.setScissorRect(a0.left, a0.top, a0.right, a0.bottom);
    return bEResult_Ok;
}
bEResult HalDevice::GetScissor(bCRect& a0) {
    a0 = {0, 0, static_cast<std::int32_t>(dev_.viewport().width), static_cast<std::int32_t>(dev_.viewport().height)};
    return bEResult_Ok;
}
bEResult HalDevice::EnableScissor(bool a0) {
    normal(RS_SCISSORTESTENABLE, a0 ? 1u : 0u);
    return bEResult_Ok;
}
bool HalDevice::IsScissorEnabled() { return dev_.renderState(RS_SCISSORTESTENABLE) != 0; }
bEResult HalDevice::SetClipPlane(unsigned long a0) {
    normal(RS_CLIPPLANEENABLE, static_cast<std::uint32_t>(a0));
    if (a0 != 0) {
        static bool logged = false;
        if (!logged) {
            logged = true;
            spdlog::warn("hal: user clip planes (mask 0x{:x}) are not emulated", a0);
        }
    }
    return bEResult_Ok;
}
bEResult HalDevice::SetClipPlaneData(unsigned long a0, eCGfxShared::eSGfxPlaneData& a1) {
    if (a0 < 6) std::memcpy(clipPlanes_[a0], &a1, 16);
    return bEResult_Ok;
}
bEResult HalDevice::GetClipPlaneData(unsigned long a0, eCGfxShared::eSGfxPlaneData& a1) {
    if (a0 < 6) std::memcpy(&a1, clipPlanes_[a0], 16);
    return bEResult_Ok;
}

void HalDevice::BeginRenderStateBlock() {
    blockRecording_ = true;
    blockSaved_.fill(false);
}

void HalDevice::EndRenderStateBlock() {
    blockRecording_ = false;
    for (std::uint32_t rs = 0; rs < RS_COUNT; ++rs) {
        if (blockSaved_[rs]) {
            dev_.setRenderState(rs, blockValues_[rs]);
        }
    }
    blockSaved_.fill(false);
}

// ---- transforms (fixed function only: stored for the getters) -------------------------------------------------------

bEResult HalDevice::SetViewAndProjectionMatrices(bCMatrix const* a0, bCMatrix const* a1) {
    if (a0) view_ = *a0;
    if (a1) projection_ = *a1;
    return bEResult_Ok;
}
bEResult HalDevice::SetWorldMatrix(bCMatrix const* a0) {
    if (a0) world_ = *a0;
    return bEResult_Ok;
}
bEResult HalDevice::GetWorldMatrix(bCMatrix* a0) {
    if (a0) *a0 = world_;
    return bEResult_Ok;
}
bEResult HalDevice::SetViewMatrix(bCMatrix const* a0) {
    if (a0) view_ = *a0;
    return bEResult_Ok;
}
bEResult HalDevice::GetViewMatrix(bCMatrix* a0) {
    if (a0) *a0 = view_;
    return bEResult_Ok;
}

// ---- draws ----------------------------------------------------------------------------------------------------------

bEResult HalDevice::Draw(eCGfxShared::eSGfxDrawCallDesc const& a0) {
    ++halDraws_;
    HAL_LOG("hal> Draw indexed {} type {} prims {} start {}/{} base {} (z {} write {} func {}, stencil {} func {} ref {}, colour mask {:x})",
            a0.indexed, static_cast<int>(a0.type), a0.primitiveCount, a0.startVertex, a0.startIndex, a0.baseVertexIndex,
            dev_.renderState(d3d9::RS_ZENABLE), dev_.renderState(d3d9::RS_ZWRITEENABLE), dev_.renderState(d3d9::RS_ZFUNC),
            dev_.renderState(d3d9::RS_STENCILENABLE), dev_.renderState(d3d9::RS_STENCILFUNC), dev_.renderState(d3d9::RS_STENCILREF),
            dev_.renderState(d3d9::RS_COLORWRITEENABLE));
    const auto type = static_cast<std::uint32_t>(a0.type);
    const FfConstants ff = beginFixedFunctionDraw();
    if (a0.indexed) {
        dev_.drawIndexedPrimitive(type, a0.baseVertexIndex, a0.minVertexIndex, a0.numVertices, a0.startIndex, a0.primitiveCount);
    } else {
        dev_.drawPrimitive(type, a0.startVertex, a0.primitiveCount);
    }
    endFixedFunctionDraw(ff);
    return bEResult_Ok;
}

bEResult HalDevice::DrawUP(eCGfxShared::eEPrimitiveType a0, unsigned int a1, void* const a2, unsigned int a3) {
    HAL_LOG("hal> DrawUP type {} prims {} stride {} (z {} write {} func {}, stencil {} func {} ref {}, colour mask {:x})",
            static_cast<int>(a0), a1, a3, dev_.renderState(d3d9::RS_ZENABLE), dev_.renderState(d3d9::RS_ZWRITEENABLE),
            dev_.renderState(d3d9::RS_ZFUNC), dev_.renderState(d3d9::RS_STENCILENABLE), dev_.renderState(d3d9::RS_STENCILFUNC),
            dev_.renderState(d3d9::RS_STENCILREF), dev_.renderState(d3d9::RS_COLORWRITEENABLE));
    const FfConstants ff = beginFixedFunctionDraw();
    dev_.drawPrimitiveUP(static_cast<std::uint32_t>(a0), a1, a2, a3);
    endFixedFunctionDraw(ff);
    return bEResult_Ok;
}

bEResult HalDevice::DrawIndexedUP(eCGfxShared::eEPrimitiveType a0, unsigned int a1, unsigned int a2, unsigned int a3, void* const a4,
                                  void* const a5, unsigned int a6) {
    // always 16-bit indices (eCDX9Device::DrawIndexedUP 0x742160)
    const FfConstants ff = beginFixedFunctionDraw();
    dev_.drawIndexedPrimitiveUP(static_cast<std::uint32_t>(a0), a1, a2, a3, a4, false, a5, a6);
    endFixedFunctionDraw(ff);
    return bEResult_Ok;
}

// ---- constants ------------------------------------------------------------------------------------------------------

bEResult HalDevice::SetVertexProgramRegisterF(unsigned long a0, float const* a1, unsigned long a2) {
    dev_.setVertexShaderConstantF(static_cast<std::uint32_t>(a0), a1, static_cast<std::uint32_t>(a2));
    return bEResult_Ok;
}
bEResult HalDevice::SetVertexProgramRegisterI(unsigned long a0, int const* a1, unsigned long a2) {
    dev_.setVertexShaderConstantI(static_cast<std::uint32_t>(a0), a1, static_cast<std::uint32_t>(a2));
    return bEResult_Ok;
}
bEResult HalDevice::SetVertexProgramRegisterB(unsigned long a0, int const* a1, unsigned long a2) {
    dev_.setVertexShaderConstantB(static_cast<std::uint32_t>(a0), a1, static_cast<std::uint32_t>(a2));
    return bEResult_Ok;
}
bEResult HalDevice::GetVertexProgramRegisterF(unsigned long a0, float* a1, unsigned long a2) {
    dev_.getVertexShaderConstantF(static_cast<std::uint32_t>(a0), a1, static_cast<std::uint32_t>(a2));
    return bEResult_Ok;
}
// ---- fixed function (d3d9/ffshader.h; docs/research/debug-mode.md 8.3) --------------------------------------------------

bEResult HalDevice::SetVertexProgram(unsigned long a0, bool a1, bool a2) {
    HAL_LOG("hal> SetVertexProgram 0x{:x} fvf {} {}", a0, a1, a2);
    if (!a1) {
        // an assembly vertex program (CreateVertexProgram: not implemented, never created)
        noteUnimplemented(keCAPIDeviceId, 109);
        return bEResult_Ok;
    }
    // eCDX9Device: SetVertexDeclaration(NULL) + SetFVF, no vertex shader
    const auto fvf = static_cast<std::uint32_t>(a0);
    dev_.setVertexDecl(fvfDecl(fvf));
    if (!d3d9::ff::supportedFvf(fvf)) {
        static bool logged = false;
        if (!logged) {
            logged = true;
            spdlog::warn("hal: fixed-function FVF 0x{:x} is not emulated (only untransformed XYZ positions): such draws are skipped", fvf);
        }
        dev_.setVertexShader(nullptr);
        return bEResult_Ok;
    }
    const bool diffuse = (fvf & d3d9::ff::kFvfDiffuse) != 0;
    auto& vs = ffVs_[diffuse ? 1 : 0];
    if (!vs) {
        vs = dev_.createShader(d3d9::ff::vertexShader(diffuse));
        spdlog::info("hal: fixed-function vertex shader (FVF 0x{:x}{}): {}", fvf, diffuse ? ", vertex colour" : "",
                     vs ? "built" : "FAILED");
    }
    dev_.setVertexShader(vs);
    return bEResult_Ok;
}

bEResult HalDevice::SetPixelProgram(unsigned long a0) {
    HAL_LOG("hal> SetPixelProgram 0x{:x}", a0);
    if (a0 != 0) {
        noteUnimplemented(keCAPIDeviceId, 119);  // an assembly pixel program (CreatePixelProgram: not implemented)
        return bEResult_Ok;
    }
    if (!ffPs_) {
        ffPs_ = dev_.createShader(d3d9::ff::pixelShader());
        spdlog::info("hal: fixed-function pixel shader: {}", ffPs_ ? "built" : "FAILED");
    }
    dev_.setPixelShader(ffPs_);
    return bEResult_Ok;
}

bEResult HalDevice::SetColorCalc(unsigned long a0, eCGfxShared::eETexOperation a1, eCGfxShared::eETexArgument a2,
                                 eCGfxShared::eETexArgument a3) {
    HAL_LOG("hal> SetColorCalc stage {} op {} args {} {}", a0, static_cast<int>(a1), static_cast<int>(a2), static_cast<int>(a3));
    if (a0 == 0) {
        ffStage0_.colorOp = static_cast<std::uint32_t>(a1);
        ffStage0_.colorArg1 = static_cast<std::uint32_t>(a2);
        ffStage0_.colorArg2 = static_cast<std::uint32_t>(a3);
    }
    return bEResult_Ok;
}

bEResult HalDevice::SetAlphaCalc(unsigned long a0, eCGfxShared::eETexOperation a1, eCGfxShared::eETexArgument a2,
                                 eCGfxShared::eETexArgument a3) {
    HAL_LOG("hal> SetAlphaCalc stage {} op {} args {} {}", a0, static_cast<int>(a1), static_cast<int>(a2), static_cast<int>(a3));
    if (a0 == 0) {
        ffStage0_.alphaOp = static_cast<std::uint32_t>(a1);
        ffStage0_.alphaArg1 = static_cast<std::uint32_t>(a2);
        ffStage0_.alphaArg2 = static_cast<std::uint32_t>(a3);
    }
    return bEResult_Ok;
}

HalDevice::FfConstants HalDevice::beginFixedFunctionDraw() {
    FfConstants saved;
    const d3d9::Shader* vs = dev_.vertexShader();
    const d3d9::Shader* ps = dev_.pixelShader();
    const bool ffVs = vs != nullptr && (vs == ffVs_[0].get() || vs == ffVs_[1].get());
    const bool ffPs = ps != nullptr && ps == ffPs_.get();
    if (!ffVs && !ffPs) return saved;
    saved.active = true;
    dev_.getVertexShaderConstantF(d3d9::ff::kVsMatrix, saved.vs, 4);
    dev_.getPixelShaderConstantF(d3d9::ff::kPsMul, saved.ps, 2);
    const auto m = d3d9::ff::transposedProduct(reinterpret_cast<const float*>(&world_), reinterpret_cast<const float*>(&view_),
                                               reinterpret_cast<const float*>(&projection_));
    dev_.setVertexShaderConstantF(d3d9::ff::kVsMatrix, m.data(), 4);
    const d3d9::ff::Linear l = d3d9::ff::reduce(ffStage0_, dev_.renderState(d3d9::RS_TEXTUREFACTOR));
    float consts[8];
    std::memcpy(consts, l.mul.data(), 16);
    std::memcpy(consts + 4, l.add.data(), 16);
    dev_.setPixelShaderConstantF(d3d9::ff::kPsMul, consts, 2);
    if (ffDraws_++ == 0) {
        spdlog::info("hal: first fixed-function draw (the engine's debug drawing): stage 0 colour op {} ({}, {}), alpha op {} ({}, {}), "
                     "texture factor 0x{:08x}, {} vertex shader, {} pixel shader",
                     ffStage0_.colorOp, ffStage0_.colorArg1, ffStage0_.colorArg2, ffStage0_.alphaOp, ffStage0_.alphaArg1,
                     ffStage0_.alphaArg2, dev_.renderState(d3d9::RS_TEXTUREFACTOR), ffVs ? "generated" : "engine",
                     ffPs ? "generated" : "engine");
    }
    return saved;
}

void HalDevice::endFixedFunctionDraw(const FfConstants& saved) {
    if (!saved.active) return;
    dev_.setVertexShaderConstantF(d3d9::ff::kVsMatrix, saved.vs, 4);
    dev_.setPixelShaderConstantF(d3d9::ff::kPsMul, saved.ps, 2);
}

bEResult HalDevice::SetPixelProgramRegisterF(unsigned long a0, float const* a1, unsigned long a2) {
    dev_.setPixelShaderConstantF(static_cast<std::uint32_t>(a0), a1, static_cast<std::uint32_t>(a2));
    return bEResult_Ok;
}
bEResult HalDevice::SetPixelProgramRegisterI(unsigned long a0, int const* a1, unsigned long a2) {
    dev_.setPixelShaderConstantI(static_cast<std::uint32_t>(a0), a1, static_cast<std::uint32_t>(a2));
    return bEResult_Ok;
}
bEResult HalDevice::SetPixelProgramRegisterB(unsigned long a0, int const* a1, unsigned long a2) {
    dev_.setPixelShaderConstantB(static_cast<std::uint32_t>(a0), a1, static_cast<std::uint32_t>(a2));
    return bEResult_Ok;
}
bEResult HalDevice::GetPixelProgramRegisterF(unsigned long a0, float* a1, unsigned long a2) {
    dev_.getPixelShaderConstantF(static_cast<std::uint32_t>(a0), a1, static_cast<std::uint32_t>(a2));
    return bEResult_Ok;
}

// ---- samplers -------------------------------------------------------------------------------------------------------

bEResult HalDevice::SetTextureAdressing(unsigned long a0, eCGfxShared::eETexAddress a1, eCGfxShared::eETexAddress a2,
                                        eCGfxShared::eETexAddress a3) {
    const auto s = static_cast<std::uint32_t>(a0);
    dev_.setSamplerState(s, SAMP_ADDRESSU, static_cast<std::uint32_t>(a1));
    dev_.setSamplerState(s, SAMP_ADDRESSV, static_cast<std::uint32_t>(a2));
    dev_.setSamplerState(s, SAMP_ADDRESSW, static_cast<std::uint32_t>(a3));
    return bEResult_Ok;
}

bEResult HalDevice::SetTextureFiltering(unsigned long a0, eCGfxShared::eETexFilter a1, eCGfxShared::eETexFilter a2,
                                        eCGfxShared::eETexFilter a3) {
    const auto s = static_cast<std::uint32_t>(a0);
    dev_.setSamplerState(s, SAMP_MINFILTER, static_cast<std::uint32_t>(a1));  // argument order min, mag, mip
    dev_.setSamplerState(s, SAMP_MAGFILTER, static_cast<std::uint32_t>(a2));
    dev_.setSamplerState(s, SAMP_MIPFILTER, static_cast<std::uint32_t>(a3));
    return bEResult_Ok;
}

bEResult HalDevice::SetSamplerState(unsigned long a0, eCGfxShared::eESamplerState a1, unsigned long a2) {
    dev_.setSamplerState(static_cast<std::uint32_t>(a0), static_cast<std::uint32_t>(a1), static_cast<std::uint32_t>(a2));
    return bEResult_Ok;
}

bEResult HalDevice::SetTextureNULL(unsigned long a0) {
    dev_.setTexture(static_cast<std::uint32_t>(a0), nullptr);
    return bEResult_Ok;
}
bEResult HalDevice::SetIndexDataNULL() {
    dev_.setIndices(nullptr);
    return bEResult_Ok;
}
bEResult HalDevice::SetVertexDataNULL(unsigned long a0) {
    dev_.setStreamSource(static_cast<std::uint32_t>(a0), nullptr, 0, 0);
    return bEResult_Ok;
}

// ---- render targets (hal-structs.md 2.2) -----------------------------------------------------------------------------

bEResult HalDevice::SetRenderTarget(void* a0, void* a1) {
    // N5.5c: a shadow map's target bind / reset (native/effect_ops_setup.cpp)
    if (effectOps_ && effectOps_->setupOn()) effectOps_->setupTarget(a0, a1);
    auto* rt = static_cast<d3d9::Surface*>(a0);
    if (logThisFrame()) {
        const auto describe = [](const d3d9::Surface* s) {
            if (s == nullptr || s->texture() == nullptr) return std::string("-");
            const auto& d = s->texture()->desc();
            return std::format("{}x{} {} tex {}", s->width(), s->texture()->levelHeight(s->level()), rhi::d3dFormatName(d.format),
                               static_cast<const void*>(s->texture()));
        };
        HAL_LOG("hal> SetRenderTarget {} [{}] {} [{}] (back buffer {})", a0, describe(rt), a1,
                describe(static_cast<d3d9::Surface*>(a1)), static_cast<void*>(dev_.backBuffer()));
    }
    unbindTextures();
    dev_.setRenderTarget(0, rt);
    dev_.setDepthStencil(static_cast<d3d9::Surface*>(a1));
    // eCSkydome_PS::UpdateSkydomeEnvCubemap binds a face of the sky cube (frame-phases.md 4)
    if (rt != nullptr && rt->texture() != nullptr && rt->texture()->desc().type == d3d9::TextureType::Cube &&
        dev_.phases().view() == d3d9::scene::View::SkyCube) {
        dev_.tagImage(rt->texture(), d3d9::scene::ImageRole::SkyEnvCube);
        dev_.countSkyCubeFace();
    }
    return bEResult_Ok;
}

bEResult HalDevice::SetRenderTargetAt(void* a0, int a1) {
    if (a1 < 0 || a1 >= static_cast<int>(kRenderTargets)) return bEResult_False;
    unbindTextures();
    dev_.setRenderTarget(static_cast<std::uint32_t>(a1), static_cast<d3d9::Surface*>(a0));
    return bEResult_Ok;
}

bEResult HalDevice::GetRenderTargetAt(void** a0, int a1) {
    if (a0 == nullptr || a1 < 0 || a1 >= static_cast<int>(kRenderTargets)) return bEResult_False;
    *a0 = dev_.renderTarget(static_cast<std::uint32_t>(a1));
    return bEResult_Ok;
}

bEResult HalDevice::GetRenderTarget(void** a0, void** a1) {
    if (a0) *a0 = dev_.renderTarget(0);
    if (a1) *a1 = dev_.depthStencil();
    return bEResult_Ok;
}

bEResult HalDevice::GetBackBuffer(void** a0, void** a1) {
    if (a0) *a0 = dev_.backBuffer();
    if (a1) *a1 = dev_.autoDepthStencil();
    return bEResult_Ok;
}

// ---- occlusion queries: not emulated yet, everything is visible -----------------------------------------------------

bEResult HalDevice::CreateOcclusionQuery(void*& a0) {
    a0 = reinterpret_cast<void*>(static_cast<std::uintptr_t>(nextQuery_++));
    return bEResult_Ok;
}
bEResult HalDevice::DestroyOcclusionQuery(void* const) { return bEResult_Ok; }
bEResult HalDevice::BeginOcclusionTest(void* const) { return bEResult_Ok; }
bEResult HalDevice::EndOcclusionTest(void* const) { return bEResult_Ok; }
bEResult HalDevice::GetOcclusionTestInfo(void* const, unsigned long& a1, bool) {
    a1 = 100000;  // "visible"
    return bEResult_Ok;
}

}  // namespace rvk::hal::vulkan
