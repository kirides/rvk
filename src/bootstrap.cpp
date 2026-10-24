#include "bootstrap.h"

#include <algorithm>
#include <charconv>
#include <atomic>
#include <cstdint>
#include <cwctype>
#include <filesystem>
#include <format>
#include <ranges>
#include <string>
#include <vector>

#include <windows.h>
#include <psapi.h>

#include <spdlog/spdlog.h>

#include "api/capture.h"
#include "core/config.h"
#include "core/hooks.h"
#include "core/log.h"
#include "core/paths.h"
#include "core/settings.h"
#include "core/stall_sampler.h"
#include "engine/scene_hooks.h"
#include "engine/high_fps.h"
#include "engine/hemisphere.h"
#include "engine/lights.h"
#include "engine/lod.h"
#include "engine/material_source.h"
#include "engine/shadow_maps.h"
#include "engine/symbols.h"
#include "engine/vfs.h"
#include "engine/window_mode.h"
#include "hal/trace.h"
#include "hal/vulkan/hal_vulkan.h"
#include "hal/types_base.h"
#include "overlay/keys.h"
#include "rvk_api.h"
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
                                     base.starts_with(L"graphics-hook") || base.starts_with(L"amdvlk") ||
                                     base.starts_with(L"steamoverlayvulkanlayer") || base.starts_with(L"discordhook") ||
                                     base.starts_with(L"rtsshooks") || base.starts_with(L"nvspcap") || base == L"dxcore.dll";
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
            // before the game window exists: its size, the monitor rectangles and the modes in physical pixels
            // (borderless fullscreen, docs/research/window-mode.md 4)
            engine::window::enableDpiAwareness();
            return 1;  // bEResult_Ok, as the original always returns
        }
        spdlog::error("eCGfxAdmin::sEnumAdapterDesc: no usable Vulkan device: the game's D3D9 renderer runs this session "
                      "(mode = passthrough)");
        g_config.mode = Mode::Passthrough;
        api::setMode(Mode::Passthrough);
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
    if (g_config.presentStalls > 0.0) {
        diag::enablePresentSampler(g_config.mode == Mode::Vulkan ? "present thread vkQueuePresentKHR" : "SwapScreen (D3D9 Present)",
                                   g_config.presentStalls);
    }
    if (g_config.cpuProfile && g_config.mode == Mode::Vulkan) diag::enableFrameSampler();
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
        vo.syncValidation = g_config.syncValidation;
        vo.adapter = g_config.adapter;
        vo.maxFps = g_config.maxFps;
        vo.vsync = g_config.vsync;
        vo.pipelineLibraries = g_config.pipelineLibraries;
        vo.perf = g_config.perf || g_config.perfFrame != 0;
        vo.perfFrame = g_config.perfFrame;
        vo.cacheDir = paths::dataDir() / L"cache";
        vo.screenshotFrames = g_config.screenshotFrames;
        vo.logDraws = g_config.logDraws;
        vo.logFrame = g_config.logFrame;
        vo.screenshotDir = paths::dataDir() / L"screenshots";
        if (g_config.dumpSavedTextures) vo.dumpDir = paths::dataDir() / L"dumps";
        vo.overlay = g_config.overlay;
        vo.overlayInScreenshots = g_config.overlayInScreenshots;
        vo.overlayCommands = g_config.commands;
        vo.settingsFile = paths::dataDir() / L"settings.toml";
        vo.rtSelfTest = g_config.rtSelfTest;
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

// [debug] present_stalls outside Vulkan mode: the engine's D3D9 present (eCDX9Device::SwapScreen) on the main thread
using SwapScreenFn = int (*)(void* self, HWND window, const void* src, const void* dst);
Hook<SwapScreenFn> g_swapScreen;

int detourSwapScreen(void* self, HWND window, const void* src, const void* dst) {
    diag::StallSampler* sampler = diag::presentSampler();
    if (sampler != nullptr) sampler->enter();
    const int r = g_swapScreen.original(self, window, src, dst);
    if (sampler != nullptr) sampler->leave();
    return r;
}

// ---- [debug] snapshot_frames: the engine's snapshot + a self-test of the tool API (docs/research/reference-compare.md 4) ----

// virtual void eCApplication::Draw(HWND): GUI, then DoSnapshot when eCApplication::Snapshot asked for one, then SwapScreen
using AppDrawFn = void (*)(void* self, HWND hwnd);
Hook<AppDrawFn> g_appDraw;
std::uint32_t g_drawCalls = 0;

struct SelfTest {
    std::string name;
    std::uint32_t ticket = 0;
    bool reported = false;
};
std::vector<SelfTest> g_selfTests;

template <class Fn>
Fn apiExport(const char* name) {
    return reinterpret_cast<Fn>(GetProcAddress(static_cast<HMODULE>(paths::selfModule()), name));
}

// Called on the game's thread before Draw: requests through the exports (GetProcAddress, as risen-spass does). In Vulkan mode
// the "ok" requests must get a ticket and the others must be refused; in the other modes everything is refused.
void apiSelfTestRequest(std::uint32_t drawCall) {
    const auto shot = apiExport<decltype(&rvk_screenshot)>("rvk_screenshot");
    const auto caps = apiExport<decltype(&rvk_capabilities)>("rvk_capabilities");
    const auto frameIndex = apiExport<decltype(&rvk_frame_index)>("rvk_frame_index");
    const auto stats = apiExport<decltype(&rvk_stats)>("rvk_stats");
    if (shot == nullptr || caps == nullptr || frameIndex == nullptr || stats == nullptr) {
        spdlog::error("api selftest: exports missing");
        return;
    }
    struct rvk_stats st{};
    const int32_t statsBytes = stats(&st, sizeof(st));
    spdlog::info("api selftest at Draw {}: caps 0x{:x}, frame index {}, rvk_stats {} bytes: mode {}, frame {}, draws {} (total "
                 "{}, skipped {}), unimplemented slots {}, pending shots {}, fps {:.1f}, texture CPU {:.1f} MiB",
                 drawCall, caps(), frameIndex(), statsBytes, st.mode, st.frame_index, st.draws_last_frame, st.draws_total,
                 st.draws_skipped_total, st.unimplemented_slots, st.screenshots_pending, st.fps,
                 static_cast<double>(st.texture_cpu_bytes) / (1024.0 * 1024.0));
    const bool vulkan = (caps() & RVK_CAP_SCREENSHOT) != 0;
    const std::filesystem::path dir = paths::dataDir() / L"screenshots";
    const std::filesystem::path game = paths::binDir().parent_path();
    const std::wstring stem = L"api_draw" + std::to_wstring(drawCall);
    struct Case {
        const char* name;
        std::filesystem::path path;
        std::uint32_t wait, flags;
        bool ok;
    };
    const Case cases[] = {
        {"png now", dir / (stem + L".png"), 0, 0, true},
        {"bmp now", dir / (stem + L".bmp"), 0, 0, true},
        {"png after 3 presents", dir / (stem + L"_w3.png"), 3, 0, true},
        {"relative path", std::filesystem::path(L"api_relative.png"), 0, 0, false},
        {"game data dir", game / L"data" / L"api_refused.png", 0, 0, false},
        {"junction into the install", game / L"data" / L"common" / L"api_refused.png", 0, 0, false},
        {"dot-dot into data", dir / L".." / L".." / L".." / L"data" / L"api_refused.png", 0, 0, false},
        {"jpg extension", dir / (stem + L".jpg"), 0, 0, false},
        {"unknown flag", dir / (stem + L"_flag.png"), 0, 1, false},
    };
    for (const auto& c : cases) {
        const std::uint32_t ticket = shot(c.path.c_str(), c.wait, c.flags);
        const bool expectTicket = vulkan && c.ok;
        spdlog::info("api selftest: {} -> ticket {} ({})", c.name, ticket,
                     (ticket != 0) == expectTicket ? "PASS" : "FAIL: expected " + std::string(expectTicket ? "a ticket" : "refusal"));
        if (ticket != 0) {
            g_selfTests.push_back(SelfTest{c.name, ticket, false});
        }
    }
}

void apiSelfTestPoll() {
    if (g_selfTests.empty()) return;
    const auto status = apiExport<decltype(&rvk_screenshot_status)>("rvk_screenshot_status");
    for (auto& t : g_selfTests) {
        if (t.reported) continue;
        char msg[256];
        const int32_t s = status(t.ticket, msg, sizeof(msg));
        if (s != RVK_SHOT_PENDING) {
            spdlog::info("api selftest: ticket {} ({}): {}: {}", t.ticket, t.name, s == RVK_SHOT_DONE ? "done" : "FAILED", msg);
            t.reported = true;
        }
    }
}

// [debug] commands outside Vulkan mode (no overlay): the key commands at eCApplication::Draw calls, so a D3D9 baseline gets the
// same scripted input (overlay/keys.h; docs/research/debug-mode.md 8)
struct DrawCommand {
    std::uint64_t draw;
    std::string text;
};
std::vector<DrawCommand> g_drawCommands;
overlay::keys::Poster g_drawKeys;

void parseDrawCommands() {
    if (g_config.mode == Mode::Vulkan) return;  // the overlay runs them at presents
    for (const auto& c : g_config.commands) {
        const auto colon = c.find(':');
        std::uint64_t n = 0;
        const auto [p, ec] = std::from_chars(c.data(), c.data() + (colon == std::string::npos ? 0 : colon), n);
        std::string text = colon == std::string::npos ? std::string() : c.substr(colon + 1);
        text.erase(0, text.find_first_not_of(" \t"));
        const auto verb = std::string_view(text).substr(0, text.find(' '));
        if (ec != std::errc() || n == 0 || !overlay::keys::Poster::handles(verb)) {
            spdlog::warn("[debug] commands: '{}' ignored in mode {} (only \"<draw>: key|keydown|keyup|type ...\" run without the overlay)",
                         c, toString(g_config.mode));
            continue;
        }
        g_drawCommands.push_back({n, std::move(text)});
    }
}

void runDrawCommands(std::uint64_t n, HWND hwnd) {
    g_drawKeys.tick(hwnd, n);
    for (auto it = g_drawCommands.begin(); it != g_drawCommands.end();) {
        if (it->draw <= n) {
            std::string out;
            const bool ok = g_drawKeys.run(hwnd, it->text, n, &out);
            spdlog::info("[debug] command at Draw {}: '{}' -> {}{}", n, it->text, ok ? "" : "FAILED: ", out);
            it = g_drawCommands.erase(it);
        } else {
            ++it;
        }
    }
}

void detourAppDraw(void* self, HWND hwnd) {
    engine::scene::appDrawEnter();  // the Gui phase (Vulkan mode; a no-op otherwise)
    const std::uint32_t n = ++g_drawCalls;
    apiSelfTestPoll();
    runDrawCommands(n, hwnd);
    const bool snap = std::ranges::find(g_config.snapshotFrames, n) != g_config.snapshotFrames.end();
    if (snap) {
        const bool requested = engine::requestEngineSnapshot();
        spdlog::info("debug: eCApplication::Snapshot at Draw {} (frame index {}): {}", n, api::frameIndex(),
                     requested ? "requested" : "symbols missing");
        apiSelfTestRequest(n);
    }
    g_appDraw.original(self, hwnd);
    if (snap) {
        spdlog::info("debug: engine snapshot name '{}'", engine::lastEngineSnapshotName());
    }
    engine::scene::appDrawLeave();
}

// Player settings (core/settings.h, docs/research/overlay.md 2): the renderer's settings, the player's settings.toml, then the
// developer overrides of config.toml (a non-default value there locks the setting for the session).
void loadSettings() {
    auto& reg = settings::registry();
    const settings::RendererSettings rs = settings::registerRendererSettings(reg);
    const auto file = paths::dataDir() / L"settings.toml";
    std::vector<std::string> warnings;
    if (!reg.load(file, &warnings)) {
        spdlog::warn("settings.toml: not loaded, defaults used");
    }
    for (const auto& w : warnings) spdlog::warn("settings.toml: {}", w);
    if (g_config.vsync >= 0) reg.lock(*rs.vsync, g_config.vsync != 0 ? 1.0 : 2.0, "config.toml [vulkan] vsync");
    if (g_config.maxFps > 0.0) reg.lock(*rs.maxFps, g_config.maxFps, "config.toml [vulkan] max_fps");
    if (g_config.adapter >= 0) reg.lock(*rs.adapter, g_config.adapter, "config.toml [vulkan] adapter");
    reg.markStarted();
    reg.dispatch();
    g_config.adapter = rs.adapter->asInt();
    std::string values;
    for (const settings::Setting* s : reg.all()) {
        if (!s->isDefault()) values += std::format("{}{} = {}{}", values.empty() ? "" : ", ", s->key(), reg.text(*s),
                                                   s->locked() ? " (config.toml)" : "");
    }
    spdlog::info("settings: {} registered, non-default: [{}]", reg.size(), values);
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
    // [debug] environment: before anything in the process creates a Vulkan instance or a D3D device
    for (const auto& kv : g_config.environment) {
        const auto eq = kv.find('=');
        const std::wstring name(kv.begin(), kv.begin() + static_cast<std::ptrdiff_t>(eq));
        const std::wstring value(kv.begin() + static_cast<std::ptrdiff_t>(eq) + 1, kv.end());
        SetEnvironmentVariableW(name.c_str(), value.empty() ? nullptr : value.c_str());
        spdlog::info("environment: {}", value.empty() ? kv + " (removed)" : kv);
    }

    api::setGameDir(paths::binDir().parent_path());
    loadSettings();

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
    // frame phase markers (docs/research/frame-phases.md): Vulkan mode only, passthrough / trace stay untouched
    if (ok && g_config.mode == Mode::Vulkan) {
        engine::scene::install();
        engine::shadowmaps::install();
        engine::hemisphere::install();  // the weather hemisphere light for the GI sky (docs/research/rtgi.md 17)
        engine::lights::install();      // the illumination admin: all of the world's lights for the GI (rtgi.md 19)
        engine::lod::install();     // detail distances (docs/research/lod.md)
        if (!engine::matsrc::install()) spdlog::warn("engine: CompileShaderPoolEllement not hooked: no material shader sources");
        engine::window::install();  // borderless fullscreen (docs/research/window-mode.md)
    }
    // gameplay fixes for high frame rates (any mode: not rendering; docs/research/fps-limit.md 12)
    if (ok) engine::highfps::install();
    // eCApplication::Draw: the Gui phase (Vulkan mode), the debug snapshot + tool-API self-test at fixed Draw calls (any mode) and
    // the scripted key commands outside Vulkan mode
    parseDrawCommands();
    if (ok && (g_config.mode == Mode::Vulkan || !g_config.snapshotFrames.empty() || !g_drawCommands.empty()) &&
        !g_appDraw.install("eCApplication::Draw", reinterpret_cast<AppDrawFn>(s.appDraw), &detourAppDraw)) {
        spdlog::warn("eCApplication::Draw not hooked: no Gui phase, [debug] snapshot_frames ignored");
    }
    if (ok && g_config.mode != Mode::Vulkan && g_config.presentStalls > 0.0 &&
        !g_swapScreen.install("eCDX9Device::SwapScreen", reinterpret_cast<SwapScreenFn>(s.dx9SwapScreen), &detourSwapScreen)) {
        spdlog::warn("eCDX9Device::SwapScreen not hooked: [debug] present_stalls has nothing to watch");
    }
    if (ok) {
        ok = hooks::enableAll();
    }
    spdlog::info("hooks {}", ok ? "enabled" : "FAILED");
    // the tool API (rvk_screenshot, ...) serves requests only while our Vulkan HAL renders
    api::setMode(ok ? g_config.mode : Mode::Passthrough);
}

void onProcessDetach(bool processTerminating) {
    spdlog::info("process detach (terminating = {})", processTerminating);
    // exclusive fullscreen: the desktop's display mode back (Windows also restores it when the process is killed)
    engine::window::restoreDisplayMode("process exit");
    if (g_config.mode == Mode::Trace) {
        hal::trace::writeStats();
    }
    if (!processTerminating) {
        hooks::shutdown();
    }
    log::shutdown();
}

}  // namespace rvk::bootstrap
