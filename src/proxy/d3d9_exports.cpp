// The exports of our d3d9.dll. Engine.dll imports only Direct3DCreate9 (docs/research/engine-gfx.md); the rest exists for
// tools that probe d3d9.dll (PIX markers). In passthrough mode everything forwards to the system d3d9.dll, which is loaded
// lazily: in Vulkan mode it is never loaded at all.

#include "proxy/d3d9_exports.h"

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <cwchar>
#include <filesystem>
#include <format>
#include <intrin.h>
#include <mutex>
#include <string>

#include <windows.h>

#include <spdlog/spdlog.h>

#include "api/capture.h"
#include "api/settings_api.h"
#include "core/caps.h"
#include "overlay/overlay.h"
#include "rvk_api.h"

namespace rvk::proxy {

void* systemD3D9() {
    static std::once_flag once;
    static HMODULE mod = nullptr;
    std::call_once(once, [] {
        wchar_t dir[MAX_PATH]{};
        const UINT n = GetSystemDirectoryW(dir, MAX_PATH);
        if (n == 0 || n >= MAX_PATH) {
            spdlog::error("d3d9 proxy: GetSystemDirectoryW failed");
            return;
        }
        const std::wstring path = std::wstring(dir) + L"\\d3d9.dll";
        mod = LoadLibraryW(path.c_str());
        if (mod == nullptr) {
            spdlog::error("d3d9 proxy: cannot load the system d3d9.dll (error {})", GetLastError());
        } else {
            spdlog::info("d3d9 proxy: system d3d9.dll loaded");
        }
    });
    return mod;
}

}  // namespace rvk::proxy

namespace {

template <class Fn>
Fn real(const char* name) {
    HMODULE mod = static_cast<HMODULE>(rvk::proxy::systemD3D9());
    return mod != nullptr ? reinterpret_cast<Fn>(GetProcAddress(mod, name)) : nullptr;
}

}  // namespace

extern "C" {

void* WINAPI rvk_Direct3DCreate9(UINT sdkVersion) {
    static std::atomic<int> calls{0};
    // who asks: Engine.dll calls it from eCGfxAdmin::InitializeAdmin and once earlier (docs/research/hal.md 3)
    void* caller = _ReturnAddress();
    HMODULE callerModule = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       static_cast<LPCWSTR>(caller), &callerModule);
    char name[MAX_PATH]{};
    GetModuleFileNameA(callerModule, name, MAX_PATH);
    const char* base = std::strrchr(name, '\\') != nullptr ? std::strrchr(name, '\\') + 1 : name;
    spdlog::info("Direct3DCreate9(0x{:x}) call #{} from {}+0x{:x}", sdkVersion, ++calls, base,
                 reinterpret_cast<std::uintptr_t>(caller) - reinterpret_cast<std::uintptr_t>(callerModule));
    using Fn = void*(WINAPI*)(UINT);
    static const Fn fn = real<Fn>("Direct3DCreate9");
    return fn != nullptr ? fn(sdkVersion) : nullptr;
}

HRESULT WINAPI rvk_Direct3DCreate9Ex(UINT sdkVersion, void** out) {
    spdlog::info("Direct3DCreate9Ex(0x{:x})", sdkVersion);
    using Fn = HRESULT(WINAPI*)(UINT, void**);
    static const Fn fn = real<Fn>("Direct3DCreate9Ex");
    return fn != nullptr ? fn(sdkVersion, out) : E_NOTIMPL;
}

int WINAPI rvk_D3DPERF_BeginEvent(DWORD color, LPCWSTR name) {
    using Fn = int(WINAPI*)(DWORD, LPCWSTR);
    static const Fn fn = real<Fn>("D3DPERF_BeginEvent");
    return fn != nullptr ? fn(color, name) : -1;
}

int WINAPI rvk_D3DPERF_EndEvent() {
    using Fn = int(WINAPI*)();
    static const Fn fn = real<Fn>("D3DPERF_EndEvent");
    return fn != nullptr ? fn() : -1;
}

DWORD WINAPI rvk_D3DPERF_GetStatus() {
    using Fn = DWORD(WINAPI*)();
    static const Fn fn = real<Fn>("D3DPERF_GetStatus");
    return fn != nullptr ? fn() : 0;
}

BOOL WINAPI rvk_D3DPERF_QueryRepeatFrame() {
    using Fn = BOOL(WINAPI*)();
    static const Fn fn = real<Fn>("D3DPERF_QueryRepeatFrame");
    return fn != nullptr ? fn() : FALSE;
}

void WINAPI rvk_D3DPERF_SetMarker(DWORD color, LPCWSTR name) {
    using Fn = void(WINAPI*)(DWORD, LPCWSTR);
    static const Fn fn = real<Fn>("D3DPERF_SetMarker");
    if (fn != nullptr) {
        fn(color, name);
    }
}

void WINAPI rvk_D3DPERF_SetOptions(DWORD options) {
    using Fn = void(WINAPI*)(DWORD);
    static const Fn fn = real<Fn>("D3DPERF_SetOptions");
    if (fn != nullptr) {
        fn(options);
    }
}

void WINAPI rvk_D3DPERF_SetRegion(DWORD color, LPCWSTR name) {
    using Fn = void(WINAPI*)(DWORD, LPCWSTR);
    static const Fn fn = real<Fn>("D3DPERF_SetRegion");
    if (fn != nullptr) {
        fn(color, name);
    }
}

// ---- tool API (include/rvk_api.h, docs/research/reference-compare.md 4.1) ----------------------------------------------

// "0.1.0": lets tools detect that the renderer is loaded (GetProcAddress(GetModuleHandle("d3d9"), "rvk_version")).
const char* rvk_version() { return "0.1.0"; }

uint32_t rvk_capabilities() {
    if (rvk::api::mode() != rvk::Mode::Vulkan) {
        return 0u;
    }
    return RVK_CAP_SCREENSHOT | RVK_CAP_STATS | (rvk::overlay::Overlay::instance() != nullptr ? RVK_CAP_OVERLAY : 0u) |
           (rvk::caps::current().satisfies(rvk::caps::bit(rvk::caps::Cap::RayQuery)) ? RVK_CAP_RAY_QUERY : 0u);
}

uint32_t rvk_screenshot(const wchar_t* path, uint32_t wait_frames, uint32_t flags) {
    namespace api = rvk::api;
    std::string reason;
    std::uint32_t ticket = 0;
    std::filesystem::path file;
    if (path != nullptr && wcsnlen(path, 32768) < 32768) {
        file = std::filesystem::path(path);
    }
    if (api::mode() != rvk::Mode::Vulkan) {
        reason = "not in Vulkan mode";
    } else if (flags != 0) {
        reason = std::format("unknown flags 0x{:x}", flags);
    } else if (file.empty()) {
        reason = "no path";
    } else {
        if (auto refused = api::refuseOutputPath(file, api::gameDir())) {
            reason = std::move(*refused);
        } else {
            ticket = api::captures().request(file, *api::formatFromExtension(file), wait_frames, &reason);
        }
    }
    if (ticket == 0) {
        spdlog::warn("rvk_screenshot({}, wait {}, flags 0x{:x}) refused: {}", api::utf8(file), wait_frames, flags, reason);
    } else {
        spdlog::debug("rvk_screenshot #{}: {} after {} presents (frame index {})", ticket, api::utf8(file), wait_frames,
                      api::frameIndex());
    }
    return ticket;
}

int32_t rvk_screenshot_status(uint32_t ticket, char* msg, uint32_t msg_size) {
    std::string text;
    const int state = rvk::api::captures().status(ticket, &text);
    if (msg != nullptr && msg_size > 0) {
        const std::size_t n = std::min<std::size_t>(text.size(), msg_size - 1);
        std::memcpy(msg, text.data(), n);
        msg[n] = '\0';
    }
    return state;
}

uint64_t rvk_frame_index() { return rvk::api::frameIndex(); }

int32_t rvk_stats(struct rvk_stats* out, uint32_t size) {
    static_assert(sizeof(struct rvk_stats) == 56, "rvk_stats v1 is 56 bytes; only append fields");
    if (out == nullptr || size < 8) {
        return 0;
    }
    namespace api = rvk::api;
    struct rvk_stats s{};
    const api::Snapshot snap = api::snapshot();
    const rvk::Mode mode = api::mode();
    s.mode = mode == rvk::Mode::Vulkan ? RVK_MODE_VULKAN : (mode == rvk::Mode::Trace ? RVK_MODE_TRACE : RVK_MODE_PASSTHROUGH);
    s.frame_index = snap.frame;
    s.draws_last_frame = snap.numbers.drawsLastFrame;
    s.unimplemented_slots = snap.numbers.unimplementedSlots;
    s.draws_total = snap.numbers.drawsTotal;
    s.draws_skipped_total = snap.numbers.drawsSkippedTotal;
    s.screenshots_pending = api::captures().pending();
    s.fps = snap.fps;
    s.texture_cpu_bytes = snap.numbers.textureCpuBytes;
    const std::uint32_t n = std::min<std::uint32_t>(size, sizeof(s));
    s.size = n;
    std::memcpy(out, &s, n);
    return static_cast<int32_t>(n);
}

// ---- player settings, A/B, overlay (docs/research/overlay.md 4) ----------------------------------------------------------

uint32_t rvk_setting_count() { return rvk::api::settingCount(rvk::settings::registry()); }

int32_t rvk_setting_info(uint32_t index, struct rvk_setting_info* out, uint32_t size) {
    return rvk::api::settingInfo(rvk::settings::registry(), index, out, size);
}

int32_t rvk_setting_get(const char* key, char* value, uint32_t value_size) {
    return rvk::api::settingGet(rvk::settings::registry(), key, value, value_size);
}

int32_t rvk_setting_set(const char* key, const char* value, uint32_t flags) {
    return rvk::api::settingSet(rvk::settings::registry(), key, value, flags);
}

int32_t rvk_ab(int32_t op) { return rvk::api::ab(rvk::settings::registry(), op); }

int32_t rvk_overlay(int32_t op) {
    if (op < RVK_OVERLAY_QUERY || op > RVK_OVERLAY_TOGGLE) {
        return RVK_SETTING_BAD_ARGUMENT;
    }
    // the pointer is cleared before the overlay goes away; open / close only touch an atomic
    rvk::overlay::Overlay* o = rvk::overlay::Overlay::instance();
    if (o == nullptr) {
        return -1;
    }
    if (op == RVK_OVERLAY_OPEN) o->setOpen(true);
    if (op == RVK_OVERLAY_CLOSE) o->setOpen(false);
    if (op == RVK_OVERLAY_TOGGLE) o->setOpen(!o->isOpen());
    return o->isOpen() ? 1 : 0;
}

}  // extern "C"
