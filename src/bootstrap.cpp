#include "bootstrap.h"

#include <atomic>
#include <string>

#include <windows.h>

#include <spdlog/spdlog.h>

#include "core/config.h"
#include "core/hooks.h"
#include "core/log.h"
#include "core/paths.h"
#include "engine/symbols.h"
#include "hal/trace.h"
#include "hal/vulkan/hal_vulkan.h"
#include "hal/types_base.h"
#include "vk/probe.h"

namespace rvk::bootstrap {
namespace {

Config g_config;
std::atomic<bool> g_probed{false};

// void eCGfxAdmin::InitializeAdmin(eCGfxShared::eEAPIType): the first graphics code the engine runs, on the main thread
// after every DllMain and before Direct3DCreate9 (engine-gfx.md 1.2).
using InitializeAdminFn = void (*)(void* self, int apiType);
Hook<InitializeAdminFn> g_initializeAdmin;

void detourInitializeAdmin(void* self, int apiType) {
    spdlog::info("eCGfxAdmin::InitializeAdmin(this={}, apiType={})", self, apiType);
    if (g_config.probe && !g_probed.exchange(true)) {
        vk::probe();
    }
    const auto& s = engine::sym();
    if (g_config.mode == Mode::Vulkan) {
        // InitializeAdmin creates an eCDX9Instance only for API type 3; for any other type it drives the instance already in
        // ms_pAPIInstance through its virtual slots (Init, adapters, modes). Ours goes there; the type is restored to 3 for
        // code that checks it afterwards (docs/research/hal-structs.md 1).
        hal::vulkan::Options vo;
        vo.validation = g_config.validation;
        vo.adapter = g_config.adapter;
        vo.cacheDir = paths::dataDir() / L"cache";
        vo.screenshotFrames = g_config.screenshotFrames;
        vo.logDraws = g_config.logDraws;
        vo.logFrame = g_config.logFrame;
        vo.screenshotDir = paths::dataDir() / L"screenshots";
        *static_cast<void**>(s.gfxAdminApiInstance) = static_cast<hal::eCAPIInstance*>(new hal::vulkan::HalInstance(vo));
        g_initializeAdmin.original(self, 4);
        *reinterpret_cast<int*>(static_cast<char*>(self) + 0x648) = apiType;
        spdlog::info("  -> Vulkan HAL instance installed");
        return;
    }
    g_initializeAdmin.original(self, apiType);
    spdlog::info("  -> ms_pAPIInstance = {}", *static_cast<void**>(s.gfxAdminApiInstance));
    if (g_config.mode == Mode::Trace) {
        hal::trace::Options opt;
        opt.logFirst = g_config.traceLogFirst;
        opt.capture = g_config.traceCapture;
        opt.captureDir = paths::dataDir() / L"corpus";
        opt.statsFile = paths::dataDir() / L"trace-stats.txt";
        hal::trace::configure(opt);
        if (!hal::trace::install(static_cast<void**>(s.gfxAdminApiInstance))) {
            spdlog::error("trace: install failed, running untraced");
        }
    }
}

std::string modulePathOf(HMODULE mod) {
    char buf[MAX_PATH]{};
    GetModuleFileNameA(mod, buf, MAX_PATH);
    return buf;
}

}  // namespace

void onProcessAttach(void* module) {
    paths::init(module);
    std::string cfgError;
    g_config = loadConfig(paths::dataDir() / L"config.toml", &cfgError);
    log::init(paths::dataDir() / L"rvk.log", g_config.logLevel);
    spdlog::info("risen-enhanced-graphics 0.1.0 ({}), mode = {}", modulePathOf(static_cast<HMODULE>(module)),
                 toString(g_config.mode));
    if (!cfgError.empty()) {
        spdlog::warn("config.toml: {}", cfgError);
    }

    if (GetModuleHandleW(L"Engine.dll") == nullptr) {
        // loaded by something other than Risen (a tool, a test): stay a plain forwarder
        spdlog::info("Engine.dll is not mapped: plain d3d9 forwarder");
        return;
    }
    if (!engine::resolveSymbols()) {
        spdlog::error("required engine symbols missing (unknown Risen build?): plain d3d9 forwarder");
        return;
    }
    if (g_config.mode != Mode::Passthrough && !hal::initEngineAllocator()) {
        spdlog::error("bCMemoryAdmin (SharedBase.dll) not found: the game's D3D9 renderer stays");
        g_config.mode = Mode::Passthrough;
    }
    if (!hooks::initialize()) {
        return;
    }
    const auto& s = engine::sym();
    bool ok = g_initializeAdmin.install("eCGfxAdmin::InitializeAdmin",
                                        reinterpret_cast<InitializeAdminFn>(s.gfxAdminInitializeAdmin),
                                        &detourInitializeAdmin);
    if (ok) {
        ok = hooks::enableAll();
    }
    spdlog::info("hooks {}", ok ? "enabled" : "FAILED");
}

void onProcessDetach(bool processTerminating) {
    spdlog::info("process detach (terminating = {})", processTerminating);
    if (g_config.mode == Mode::Trace) {
        hal::trace::writeStats();
    }
    if (!processTerminating) {
        hooks::shutdown();
    }
    log::shutdown();
}

}  // namespace rvk::bootstrap
