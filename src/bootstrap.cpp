#include "bootstrap.h"

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cwctype>
#include <string>

#include <windows.h>
#include <psapi.h>

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

std::string modulePathOf(HMODULE mod) {
    char buf[MAX_PATH]{};
    GetModuleFileNameA(mod, buf, MAX_PATH);
    return buf;
}

// seconds since the process was created
double processUptime() {
    FILETIME created{}, exited{}, kernel{}, user{}, now{};
    if (!GetProcessTimes(GetCurrentProcess(), &created, &exited, &kernel, &user)) return -1.0;
    GetSystemTimeAsFileTime(&now);
    const auto ticks = [](const FILETIME& f) {
        return (static_cast<std::uint64_t>(f.dwHighDateTime) << 32) | f.dwLowDateTime;
    };
    return static_cast<double>(ticks(now) - ticks(created)) / 1e7;
}

// Startup diagnostics (docs/research/early-d3d9.md): uptime, which D3D / DxDiag / Vulkan / overlay modules are loaded, and
// the process's windows (top-level and message-only). In Vulkan mode no system d3d9.dll, dxdiagn.dll, d3d11.dll or d3d12.dll
// should be listed at InitializeAdmin.
void logStartupState(const char* where) {
    std::string mods;
    HMODULE handles[1024];
    DWORD bytes = 0;
    if (K32EnumProcessModules(GetCurrentProcess(), handles, sizeof(handles), &bytes)) {
        wchar_t sysDir[MAX_PATH]{};
        GetSystemDirectoryW(sysDir, MAX_PATH);
        const std::wstring sysD3D9 = std::wstring(sysDir) + L"\\d3d9.dll";
        const DWORD count = std::min<DWORD>(bytes / sizeof(HMODULE), 1024);
        for (DWORD i = 0; i < count; ++i) {
            wchar_t path[MAX_PATH]{};
            GetModuleFileNameW(handles[i], path, MAX_PATH);
            const wchar_t* slash = wcsrchr(path, L'\\');
            std::wstring base = slash != nullptr ? slash + 1 : path;
            for (auto& c : base) c = static_cast<wchar_t>(towlower(c));
            const bool interesting = _wcsicmp(path, sysD3D9.c_str()) == 0 || base == L"dxdiagn.dll" || base == L"d3d11.dll" ||
                                     base == L"d3d12.dll" || base == L"dxgi.dll" || base == L"d3d9on12.dll" ||
                                     base.starts_with(L"atidx") || base.starts_with(L"amdx") ||
                                     base == L"vulkan-1.dll" || base.starts_with(L"gameoverlayrenderer") ||
                                     base.starts_with(L"graphics-hook");
            if (interesting) {
                if (!mods.empty()) mods += ", ";
                for (const wchar_t c : base) mods += static_cast<char>(c);
            }
        }
    }
    std::string windows;
    const auto describe = [](HWND w, LPARAM p) -> BOOL {
        DWORD pid = 0;
        GetWindowThreadProcessId(w, &pid);
        if (pid != GetCurrentProcessId()) return TRUE;
        auto& list = *reinterpret_cast<std::string*>(p);
        wchar_t cls[128]{}, title[128]{};
        GetClassNameW(w, cls, 128);
        GetWindowTextW(w, title, 128);
        char cls8[512]{}, title8[512]{};
        WideCharToMultiByte(CP_UTF8, 0, cls, -1, cls8, sizeof(cls8), nullptr, nullptr);
        WideCharToMultiByte(CP_UTF8, 0, title, -1, title8, sizeof(title8), nullptr, nullptr);
        if (!list.empty()) list += ", ";
        list += std::string("'") + cls8 + "'/'" + title8 + "'" + (IsWindowVisible(w) ? "" : " (hidden)");
        return TRUE;
    };
    EnumWindows(describe, reinterpret_cast<LPARAM>(&windows));
    for (HWND w = FindWindowExW(HWND_MESSAGE, nullptr, nullptr, nullptr); w != nullptr;
         w = FindWindowExW(HWND_MESSAGE, w, nullptr, nullptr)) {
        describe(w, reinterpret_cast<LPARAM>(&windows));
    }
    spdlog::info("startup state at {}: {:.2f} s after process start; D3D/overlay modules: [{}]; windows: [{}]", where,
                 processUptime(), mods, windows);
}

// static bEResult eCGfxAdmin::sEnumAdapterDesc(unsigned long adapter, eSGfxAdapterDesc& desc): called once by
// eCApplication::CreateEngine (main thread, before the game window and before InitializeAdmin) for its "pixel shader 3.0"
// check. With ms_pAPIInstance still null the original loads the system d3d9.dll (Direct3DCreate9, GetAdapterIdentifier,
// GetDeviceCaps) and runs a DxDiag query that pulls d3d11/d3d12/dxgi and the GPU's user-mode drivers into the process
// (docs/research/early-d3d9.md). In Vulkan mode we answer from the Vulkan adapter instead; it is also where we find out
// whether Vulkan is usable at all, early enough to fall back to the untouched D3D9 renderer.
using EnumAdapterDescFn = int (*)(unsigned long adapter, void* desc);
Hook<EnumAdapterDescFn> g_enumAdapterDesc;

int detourEnumAdapterDesc(unsigned long adapter, void* desc) {
    const auto& s = engine::sym();
    if (g_config.mode == Mode::Vulkan && *static_cast<void**>(s.gfxAdminApiInstance) == nullptr && desc != nullptr) {
        if (hal::vulkan::fillAdapterDesc(g_config.adapter, static_cast<hal::eCGfxShared::eSGfxAdapterDesc*>(desc))) {
            spdlog::info("eCGfxAdmin::sEnumAdapterDesc({}): answered from Vulkan (no Direct3DCreate9, no DxDiag)", adapter);
            return 1;  // bEResult_Ok, as the original always returns
        }
        spdlog::error("eCGfxAdmin::sEnumAdapterDesc: no usable Vulkan device: the game's D3D9 renderer runs this session "
                      "(mode = passthrough)");
        g_config.mode = Mode::Passthrough;
    }
    return g_enumAdapterDesc.original(adapter, desc);
}

// void eCGfxAdmin::InitializeAdmin(eCGfxShared::eEAPIType): the HAL setup in eCApplication::CreateGraphicsSubsystem, on the
// main thread after every DllMain; creates the eCDX9Instance (Direct3DCreate9) for API type 3 (engine-gfx.md 1.2).
using InitializeAdminFn = void (*)(void* self, int apiType);
Hook<InitializeAdminFn> g_initializeAdmin;

void detourInitializeAdmin(void* self, int apiType) {
    spdlog::info("eCGfxAdmin::InitializeAdmin(this={}, apiType={})", self, apiType);
    logStartupState("InitializeAdmin");
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
        vo.maxFps = g_config.maxFps;
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

}  // namespace

void onProcessAttach(void* module) {
    paths::init(module);
    std::string cfgError;
    g_config = loadConfig(paths::dataDir() / L"config.toml", &cfgError);
    log::init(paths::dataDir() / L"rvk.log", g_config.logLevel);
    spdlog::info("risen-enhanced-graphics 0.1.0 ({}), mode = {}, {:.2f} s after process start",
                 modulePathOf(static_cast<HMODULE>(module)), toString(g_config.mode), processUptime());
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
    // optional: without it the engine's early adapter query runs through the system d3d9.dll + DxDiag (slower, harmless)
    if (ok && g_config.mode == Mode::Vulkan &&
        !g_enumAdapterDesc.install("eCGfxAdmin::sEnumAdapterDesc",
                                   reinterpret_cast<EnumAdapterDescFn>(s.gfxAdminEnumAdapterDesc), &detourEnumAdapterDesc)) {
        spdlog::warn("eCGfxAdmin::sEnumAdapterDesc not hooked: the early adapter query uses D3D9 + DxDiag");
    }
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
