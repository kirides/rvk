// eCAPIDevice on d3d9::Device (docs/research/hal-structs.md sections 2.x).

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>
#include <cstdlib>
#include <format>
#include <string>

#include <spdlog/spdlog.h>

#include "api/capture.h"
#include "core/caps.h"
#include "core/exports.h"
#include "core/paths.h"
#include "core/settings.h"
#include "core/stall_sampler.h"
#include "d3d9/fvf.h"
#include "engine/scene_hooks.h"
#include "engine/lod.h"
#include "engine/memread.h"
#include "engine/material_source.h"
#include "engine/menu_state.h"
#include "engine/shadow_maps.h"
#include "engine/vfs.h"
#include "engine/window_mode.h"
#include "hal/vulkan/hal_vulkan.h"
#include "res/dds.h"
#include "vk/debug_names.h"
#include "vk/formats.h"
#include "vk/framegen.h"
#include "vk/raytracing.h"

namespace rvk::hal::vulkan {

using namespace rvk::d3d9;

namespace {
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
    frameTap_->setStaticOpsActive(staticOps_->active());
    frameTap_->setEffectOpsActive(effectOps_->active());
    // O1: the register file's source; the coverage counter only where it is read (a compare, the tap's own report, or asked for)
    staticOps_->setMirrorMode(rs.nativeMirror != nullptr ? rs.nativeMirror->asInt() : 0);
    // O2a / O2b: the engine's slot code skipped for the composed ops (per op: only where the group takes over; default on). A compare
    // needs the engine's code: without native.lean_check (its check frames) a compare run keeps lean off
    const int leanCheck = rs.nativeLeanCheck != nullptr ? rs.nativeLeanCheck->asInt() : 0;
    const bool leanWanted = on(rs.nativeLean);
    const bool leanOn = leanWanted && !((compare || sceneCompare) && leanCheck <= 0);
    if (leanWanted && !leanOn) spdlog::info("native: lean ops off: a compare is on and native.lean_check is 0 (the compare needs the engine's code)");
    staticOps_->setLean(leanOn, leanCheck);
    // O3: the effect units, sprite flushes and the PSSM pass (native.lean_effects: their A/B)
    effectOps_->setLean(leanOn && (rs.nativeLeanEffects == nullptr || on(rs.nativeLeanEffects)), leanCheck);
    staticOps_->setLeanRestore(rs.nativeLeanRestore == nullptr || on(rs.nativeLeanRestore));
    staticOps_->setRetained(on(rs.nativeRetained), rs.nativeRetainedGroups != nullptr ? rs.nativeRetainedGroups->asInt() : 0x2ff, on(rs.nativeRetainedCheck));
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
    engine::scene::detach(&dev_);
    settings::registry().removeCallbacks(this);
    if (framegen_) framegen_->shutdown();  // AMD's swapchain calls into the overlay from its threads: it goes first
    if (overlay_) {
        dev_.setOverlay(nullptr, false);
        // ImGui's shutdown waits for the device (vkDeviceWaitIdle touches the queue the present thread uses)
        auto lock = dev_.ctx().queueLock();
        overlay_.reset();
    }
    if (settings::registry().dirty() && !options_.settingsFile.empty()) {
        settings::registry().save(options_.settingsFile);
    }
    debugView_.reset();  // removes its pass, retires its pipelines
    // the RT scene's consumers (rt.debug, the water's traced reflections) release it in their destructors: before it
    rtDebug_.reset();    // releases the RT scene
    objectMotion_.reset();  // unregisters its observer
    framegen_.reset();   // removes its passes, retires the generator
    upscale_.reset();    // clears the device's jitter source
    gbuffer_.reset();    // removes its passes, the G-buffer attachments
    gi_.reset();         // unregisters its observer / pass, releases the RT scene
    water_.reset();      // unregisters its draw replacer, releases the RT scene
    shadows_.reset();
    rtShadows_.reset();  // removes its pass, releases the RT scene
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
    dev_.destroy();
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
    o.maxFps = rs.maxFps != nullptr ? rs.maxFps->value() : options.maxFps;
    o.cacheDir = options.cacheDir;
    o.logDraws = options.logDraws;
    // rt.allow is a restart setting: the value the session started with decides (its effective value waits for the report)
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
    vk::names::setEnabled(rs.debugNames == nullptr || rs.debugNames->startValue() != 0.0);
    if (!dev_.create(o)) {
        return false;
    }
    // fullscreen: a borderless window over the monitor, or exclusive fullscreen with vulkan.fullscreen = exclusive
    // (engine/window_mode.h)
    window_ = p.window;
    engine::window::onModeSet(p.window, p.windowed, o.width, o.height, p.refreshRate);
    spdlog::info("hal: Vulkan device for window {} ({}x{}, vsync {}, depth {})", static_cast<void*>(p.window), o.width, o.height,
                 o.vsync, vk::d3dFormatName(o.depthFormat));
    engine::scene::attach(&dev_);  // the frame phase markers (no-ops when they are not installed)
    auto& reg = settings::registry();
    // frame generation providers (vk/framegen.h): which of FSR 3 / FSR 4 this device can run, for the capability report
    dev_.ctx().setFrameGenReport(vk::probeFrameGeneration(dev_.ctx(), paths::dataDir() / "amd_fidelityfx_loader_vk.dll"));
    // capability report (core/caps.h, docs/research/raytracing.md 3): logged once, the overlay's "Capabilities", and the
    // resolution of settings with requirements (greyed + parity fallback when unmet)
    {
        caps::Report report = dev_.ctx().capabilities(options.pipelineLibraries);
        // the session's render mode (renderer.mode): the overlay's System tab "Graphics card" and the caps lines
        report.properties.emplace_back("render mode", std::format("{} at start (Vulkan; Wrapper / Full switch at run time)", toString(renderMode())));
        for (const auto& line : report.lines()) spdlog::info("caps: {}", line);
        caps::setCurrent(report);
        reg.setCapabilities(report);
    }
    if (options.rtSelfTest) {
        const vk::rt::SelfTestResult t = vk::rt::selfTest(dev_.ctx());
        if (!t.ran) {
            spdlog::info("rt selftest: skipped: {}", t.detail);
        } else {
            spdlog::log(t.pass ? spdlog::level::info : spdlog::level::err, "rt selftest: {} ({:.1f} ms): {}", t.pass ? "PASS" : "FAIL",
                        t.milliseconds, t.detail);
        }
    }
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
    if (rs.debugCrashMarkers != nullptr) {
        dev_.setCrashMarkers(rs.debugCrashMarkers->asBool());
        reg.onChange(*rs.debugCrashMarkers, [this](const settings::Setting& s) { dev_.setCrashMarkers(s.asBool()); }, this);
    }
    if (rs.debugRegions != nullptr) {
        dev_.setRegionsEnabled(rs.debugRegions->asBool());
        reg.onChange(*rs.debugRegions, [this](const settings::Setting& s) { dev_.setRegionsEnabled(s.asBool()); }, this);
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
    if (rs.constantCompare != nullptr) {
        dev_.setConstantCompare(rs.constantCompare->asBool());
        reg.onChange(*rs.constantCompare, [this](const settings::Setting& s) {
            dev_.setConstantCompare(s.asBool());
            spdlog::info("hal: settings: constant compare {}", s.asBool() ? "on" : "off");
        }, this);
    }
    if (rs.splitConstants != nullptr) {
        dev_.setSplitConstants(rs.splitConstants->asBool());
        reg.onChange(*rs.splitConstants, [this](const settings::Setting& s) {
            dev_.setSplitConstants(s.asBool());
            spdlog::info("hal: settings: split constants {}", s.asBool() ? "on" : "off");
        }, this);
    }
    if (rs.splitVerify != nullptr) {
        dev_.setSplitVerify(rs.splitVerify->asBool());
        reg.onChange(*rs.splitVerify, [this](const settings::Setting& s) { dev_.setSplitVerify(s.asBool()); }, this);
    }
    if (rs.drawRecords != nullptr) {
        dev_.setDrawRecords(rs.drawRecords->asBool());
        reg.onChange(*rs.drawRecords, [this](const settings::Setting& s) {
            dev_.setDrawRecords(s.asBool());
            spdlog::info("hal: settings: draw records {}", s.asBool() ? "on" : "off");
        }, this);
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
                                     rs.waterSeaState, rs.waterWindDirection, rs.waterWaveScale, rs.waterMurkiness,
                                     rs.waterReflectionStrength, rs.waterDebug, rs.rtReflections, rs.rtReflectionTweaks}) {
            if (s != nullptr) reg.onChange(*s, [this](const settings::Setting&) { applyWater(); }, this);
        }
        if (rs.waterEnabled->asBool()) applyWater();
    }
    // enhanced sun shadows (enh/shadows.h, docs/research/shadows.md): a draw replacer for the shadow mask, live settings
    shadows_ = std::make_unique<enh::Shadows>(dev_);
    rtShadows_ = std::make_unique<enh::RtShadows>(dev_, rtScene_.get());
    shadows_->setRtShadows(rtShadows_.get());
    if (rs.shadowsEnabled != nullptr) {
        for (settings::Setting* s : {rs.shadowsRt, rs.shadowsRtQuality, rs.shadowsRtSoftness, rs.shadowsRtRange, rs.shadowsRtWind, rs.rtShadowSync, rs.shadowsEnabled, rs.shadowsFilter, rs.shadowsSoftness, rs.shadowsBlend, rs.shadowsFade,
                                     rs.shadowsBias, rs.shadowsDebug, rs.shadowsMapSize, rs.shadowsRange, rs.shadowsSplit}) {
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
    // N5.1 static ops (native/static_ops.h): driven by the frame tap
    staticOps_ = std::make_unique<native::StaticOps>(*this);
    frameTap_->setStaticOps(staticOps_.get());
    effectOps_ = std::make_unique<native::EffectOps>(*this);
    frameTap_->setEffectOps(effectOps_.get());
    for (settings::Setting* s : {rs.nativeStaticOps, rs.nativeStaticOpsCompare, rs.nativeSceneOps, rs.nativeSceneOpsCompare, rs.nativeSceneOpsGroups,
                                 rs.nativeMirror, rs.nativeCoverage, rs.nativeFrameTap, rs.nativeLean, rs.nativeLeanCheck,
                                 rs.nativeLeanRestore, rs.nativeLeanEffects, rs.nativeRetained, rs.nativeRetainedCheck, rs.nativeRetainedGroups}) {
        if (s != nullptr) reg.onChange(*s, [this](const settings::Setting&) { applyStaticOps(); }, this);
    }
    // renderer.mode (Wrapper / Full): like the native.* settings applied by the registry's dispatch before a present, i.e. at a
    // frame boundary (no frame starts with one mode and ends with the other)
    if (rs.rendererMode != nullptr) reg.onChange(*rs.rendererMode, [this](const settings::Setting&) { applyRenderMode(); }, this);
    applyRenderMode();
    // ray-traced global illumination (enh/gi.h, docs/research/rtgi.md): probes + the material shaders' GI variants
    gi_ = std::make_unique<enh::Gi>(dev_, rtScene_.get());
    if (rs.giEnabled != nullptr) {
        for (settings::Setting* s : {rs.giEnabled, rs.giQuality, rs.giStrength, rs.giLightmaps, rs.giCharacters, rs.giScreen, rs.giBleed, rs.giLights, rs.giLightCount, rs.giEmissive, rs.giDebug}) {
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
        for (settings::Setting* s : {rs.aoEnabled, rs.aoMode, rs.aoStrength, rs.aoRadius, rs.gbufferDebug}) {
            if (s != nullptr) reg.onChange(*s, [this](const settings::Setting&) { applyGbuffer(); }, this);
        }
        applyGbuffer();
    }
    // camera jitter / FSR (enh/upscale.h, docs/research/upscaling.md)
    upscale_ = std::make_unique<enh::Upscale>(dev_);
    objectMotion_ = std::make_unique<enh::ObjectMotion>(dev_, rtScene_.get());
    if (rs.upscaleMode != nullptr) {
        for (settings::Setting* s : {rs.upscaleMode, rs.upscaleProvider, rs.upscaleObjectMotion, rs.upscaleReactive, rs.upscaleReactiveCutoff, rs.upscaleReactiveScale, rs.upscaleSharpness,
                                     rs.upscaleJitterFlip, rs.upscalePostFullSize, rs.upscaleAlphaDither, rs.upscaleCutoutThicken}) {
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
            engine::matsrc::setEnabled(dump || uber);
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
    if (rs.prefetchStride != nullptr) {
        engine::lod::setPrefetchStride(static_cast<int>(rs.prefetchStride->asInt()));
        reg.onChange(*rs.prefetchStride, [](const settings::Setting& s) { engine::lod::setPrefetchStride(static_cast<int>(s.asInt())); }, this);
    }
    if (options.overlay && dev_.swapchain().valid()) {
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
    s.murkiness = rs.waterMurkiness->asFloat();
    s.reflections = rs.waterReflectionStrength->asFloat();
    // rt.reflections (effective value: off while the ray tracing tier is unavailable)
    if (rs.rtReflections != nullptr) {
        const std::string_view rt = rs.rtReflections->asEnum();
        s.rtTier = rt == "low" ? 1 : rt == "medium" ? 2 : rt == "high" ? 3 : 0;
    }
    if (rs.rtReflectionTweaks != nullptr) s.rtTweaks = static_cast<int>(rs.rtReflectionTweaks->asInt());
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
    s.characters = rs.giCharacters->asBool();
    if (rs.giBleed != nullptr) s.bleed = rs.giBleed->asFloat();
    if (rs.giEmissive != nullptr) s.emissive = rs.giEmissive->asFloat();
    if (rs.giLightCount != nullptr) s.lightCount = rs.giLightCount->asInt();
    if (rs.giLights != nullptr) s.lights = rs.giLights->asEnum() == "world" ? 1 : 0;
    const std::string_view d = rs.giDebug->asEnum();
    s.debug = d == "irradiance" ? 1 : d == "cascades" ? 2 : 0;
    if (rs.giScreen != nullptr) {
        const std::string_view sc = rs.giScreen->asEnum();
        s.screen = sc == "low" ? 1 : sc == "medium" ? 2 : sc == "high" ? 3 : 0;
    }
    gi_->apply(s);
}

void HalDevice::applyGbuffer() {
    const auto& rs = settings::renderer();
    if (!gbuffer_ || rs.aoEnabled == nullptr) return;
    enh::Gbuffer::Settings s;
    s.ao = rs.aoEnabled->asBool();
    s.replaceEngineSsao = rs.aoMode == nullptr || rs.aoMode->asEnum() != "add";
    s.aoStrength = rs.aoStrength->asFloat();
    s.aoRadius = rs.aoRadius->asFloat() * 100.0f;
    const std::string_view d = rs.gbufferDebug->asEnum();
    s.debug = d == "normals" ? 1 : d == "motion" ? 2 : d == "ao" ? 3 : d == "screen_gi" ? 4 : 0;
    // the screen probes (enh/gi) read the G-buffer
    s.forced = rs.giEnabled != nullptr && rs.giEnabled->asBool() && rs.giScreen != nullptr && rs.giScreen->asEnum() != "off";
    // FSR (enh/upscale) reads the motion vectors
    s.forced = s.forced || (upscale_ && enh::usesFsr(upscale_->mode()));
    // frame generation (enh/framegen) reads them too
    s.forced = s.forced || (framegen_ && framegen_->wantsGbuffer());
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
    dev_.setPostAtDisplaySize(rs.upscalePostFullSize == nullptr || rs.upscalePostFullSize->asBool());
    dev_.setAlphaDither(rs.upscaleAlphaDither != nullptr && rs.upscaleAlphaDither->asBool());
    dev_.setCutoutThicken(rs.upscaleCutoutThicken != nullptr ? rs.upscaleCutoutThicken->asFloat() : 0.0f);
    if (rs.upscaleReactive != nullptr) upscale_->setReactive(rs.upscaleReactive->asBool());
    if (rs.upscaleProvider != nullptr) upscale_->setMl(rs.upscaleProvider->asEnum() == "fsr4");
    if (rs.upscaleReactiveCutoff != nullptr && rs.upscaleReactiveScale != nullptr) {
        upscale_->setReactiveTuning(rs.upscaleReactiveCutoff->asFloat(), rs.upscaleReactiveScale->asFloat());
    }
    unsigned flip = 0;
    if (rs.upscaleJitterFlip != nullptr) {
        const std::string_view f = rs.upscaleJitterFlip->asEnum();
        flip = f == "x" ? 1u : f == "y" ? 2u : f == "xy" ? 3u : 0u;
    }
    enh::UpscaleMode mode = enh::UpscaleMode::Off;
    if (m == "native_aa") mode = enh::UpscaleMode::NativeAa;
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
    s.softness = rs.shadowsSoftness->asFloat();
    s.blend = rs.shadowsBlend->asFloat();
    s.fade = rs.shadowsFade->asFloat();
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
        r.softness = rs.shadowsRtSoftness->asFloat();
        r.wind = rs.shadowsRtWind->asBool();
        r.sync = rs.rtShadowSync->asBool();
        const std::string_view q = rs.shadowsRtQuality->asEnum();
        r.quality = q == "low" ? 1 : q == "high" ? 3 : 2;
        rtShadows_->apply(r);
    }
    // the engine's cascades (engine/shadow_maps.h): "auto" = the enhanced defaults while the enhanced mask is on
    engine::shadowmaps::Overrides o;
    const std::string_view size = rs.shadowsMapSize->asEnum();
    o.mapSize = size == "auto" ? (s.enabled ? 4096 : 0) : size == "game" ? 0 : std::atoi(std::string(size).c_str());
    const float metres = rs.shadowsRange->asFloat();
    o.range = metres > 0.0f ? metres * 100.0f : (s.enabled ? 10000.0f : 0.0f);
    o.splitFactor = o.range > 0.0f ? rs.shadowsSplit->asFloat() : -1.0f;
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
                     d.width, d.height, vk::d3dFormatName(d.format), d.usage, dds->width, dds->height,
                     vk::d3dFormatName(dds->d3dFormat));
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
    spdlog::debug("hal: CheckDeviceFormat {} (0x{:x}) {}x{} usage 0x{:x} blend {}: {}", vk::d3dFormatName(a0.format), a0.format,
                  a0.width, a0.height, a0.usage, a1, r == bEResult_Ok ? "yes" : "no");
    return r;
}

bEResult HalDevice::checkFormat(eCGfxShared::eSGfxLayersDesc const& a0, bool a1) {
    using namespace vk::d3dfmt;
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
        const auto info = vk::formatInfo(f, dev_.d24s8Format());
        if (!info) return bEResult_False;
        VkFormatProperties fp{};
        vkGetPhysicalDeviceFormatProperties(dev_.ctx().physicalDevice(), info->format, &fp);
        const VkFormatFeatureFlags need = VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT;
        return (fp.optimalTilingFeatures & need) == need ? bEResult_Ok : bEResult_False;
    }
    if (f == INTZ || (a0.usage & USAGE_DEPTHSTENCIL) != 0) {
        return bEResult_False;
    }
    const auto info = vk::formatInfo(f, dev_.d24s8Format());
    if (!info || info->depth) {
        return bEResult_False;
    }
    VkFormatProperties fp{};
    vkGetPhysicalDeviceFormatProperties(dev_.ctx().physicalDevice(), info->format, &fp);
    VkFormatFeatureFlags need = VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT;
    if ((a0.usage & USAGE_RENDERTARGET) != 0) {
        need |= VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT;
        if (a1) need |= VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BLEND_BIT;
    }
    return (fp.optimalTilingFeatures & need) == need ? bEResult_Ok : bEResult_False;
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
    if (overlay_) {
        overlay_->onPresent(frame);
        // [debug] overlay_in_screenshots: into the back buffer, before the captures read it
        dev_.drawOverlayIntoBackBuffer();
    }
    // the menu / loading / pause state of this present: frame generation runs in the world only (and the pacer follows it below)
    updateMenuPacing();
    // frame generation: the interpolated frame of this present is computed now, so that captures can read it too
    dev_.inject(d3d9::inject::Point::BeforePresent);
    auto& captures = api::captures();
    // [debug] screenshot_frames: a request of the tool API's queue, due at this present
    if (std::ranges::find(options_.screenshotFrames, static_cast<std::uint32_t>(frame)) != options_.screenshotFrames.end()) {
        captures.request(options_.screenshotDir / std::format("frame_{}.png", frame), api::ImageFormat::Png, 0, nullptr);
    }
    // rvk_screenshot: the finished back buffer (scene + post + GUI) before it is shown, as D3D9's Present hook / DoSnapshot see it
    captures.onPresent(frame, [this](std::vector<std::uint8_t>& bgra, std::uint32_t& w, std::uint32_t& h) {
        return dev_.readBackBuffer(bgra, w, h);
    }, [this](std::vector<std::uint8_t>& bgra, std::uint32_t& w, std::uint32_t& h) {
        return framegen_ && framegen_->readGenerated(bgra, w, h);  // the manual frame generation's frame_<n>_gen.png
    });
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
    engine::lod::onPresent();  // a changed object draw distance: the world's cache-in spheres (engine thread)
    dev_.present();
    const auto& s = dev_.stats();
    api::publishPresent(s.frames, api::FrameNumbers{
                                      .drawsLastFrame = s.lastFrameDraws,
                                      .unimplementedSlots = static_cast<std::uint32_t>(unimplementedCount()),
                                      .drawsTotal = s.totalDraws,
                                      .drawsSkippedTotal = s.skippedNoShader + s.skippedNoTarget + s.skippedVertexInput +
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
    const bool paceLoading = rs.paceLoading != nullptr && rs.paceLoading->asBool();
    dev_.setUnpaced(st.state == engine::menu::State::Loading && !paceLoading);
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

void HalDevice::logFrameStats() {
    const auto& s = dev_.stats();
    const auto now = std::chrono::steady_clock::now();
    if (s.frames == 1 || now - lastStatsTime_ >= std::chrono::seconds(5)) {
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
        spdlog::info("hal: frame {}: {} draws total, {} clears, {} render passes; skipped: {} no shader, {} no target, {} vertex "
                     "input, {} pipeline; HAL Draw calls {}; {} unimplemented slots seen; texture CPU copies {:.1f} MiB",
                     s.frames, s.totalDraws, s.totalClears, s.totalRenderPasses, s.skippedNoShader, s.skippedNoTarget,
                     s.skippedVertexInput, s.skippedPipeline, halDraws_, unimplementedCount(),
                     static_cast<double>(d3d9::Texture::cpuBytesTotal()) / (1024.0 * 1024.0));
    }
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
        const vk::FgGapStats g = vk::fgGapStats(r.gapsMs);
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
    {
        const VkPhysicalDeviceMemoryProperties* mp = nullptr;
        vmaGetMemoryProperties(dev_.ctx().allocator(), &mp);
        VmaBudget budgets[VK_MAX_MEMORY_HEAPS]{};
        vmaGetHeapBudgets(dev_.ctx().allocator(), budgets);
        std::uint64_t blocks = 0, allocs = 0, count = 0, local = 0;
        for (std::uint32_t h = 0; h < mp->memoryHeapCount; ++h) {
            blocks += budgets[h].statistics.blockBytes;
            allocs += budgets[h].statistics.allocationBytes;
            count += budgets[h].statistics.allocationCount;
            if ((mp->memoryHeaps[h].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) != 0) local += budgets[h].statistics.blockBytes;
        }
        spdlog::info("perf: GPU memory {:.1f} MiB in blocks ({:.1f} MiB device-local), {:.1f} MiB in {} allocations; ray tracing tier {}",
                     static_cast<double>(blocks) / 1048576.0, static_cast<double>(local) / 1048576.0,
                     static_cast<double>(allocs) / 1048576.0, count, dev_.ctx().features().rayTracing.enabled ? "on" : "off");
        const vk::MeshArena::Stats a = dev_.meshArena().stats();
        spdlog::info("perf: mesh arena {} blocks of {} MiB + {} dedicated buffers, {:.1f} MiB used", a.blocks,
                     vk::MeshArena::kBlockSize >> 20, a.dedicated, static_cast<double>(a.used) / 1048576.0);
    }
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
            return std::format("{}x{} {} tex {}", s->width(), s->texture()->levelHeight(s->level()), vk::d3dFormatName(d.format),
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
