// The exports of our d3d9.dll. Engine.dll imports only Direct3DCreate9 (docs/research/engine-gfx.md); the rest exists for
// tools that probe d3d9.dll (PIX markers). In passthrough mode everything forwards to the system d3d9.dll, which is loaded
// lazily: in Vulkan mode it is never loaded at all.

#include "proxy/d3d9_exports.h"

#include <atomic>
#include <cstdint>
#include <cstring>
#include <intrin.h>
#include <mutex>
#include <string>

#include <windows.h>

#include <spdlog/spdlog.h>

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

// "0.1.0": lets tools detect that the renderer is loaded (GetProcAddress(GetModuleHandle("d3d9"), "rvk_version")).
const char* rvk_version() { return "0.1.0"; }

}  // extern "C"
