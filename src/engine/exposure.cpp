#include "engine/exposure.h"

#include <windows.h>

#include <cstdint>
#include <string>

#include <spdlog/spdlog.h>

#include "engine/memread.h"
#include "engine/symbols.h"

namespace rvk::engine::exposure {

namespace {

using U8 = const std::uint8_t;

// Engine.dll RVAs (README.md 15: checked the same way as native/effect_ops_post.cpp)
constexpr std::uint32_t kFilterAdmin = 0x95750;    // eCImageFilterAdmin* (cached FindModule lookup)
constexpr std::uint32_t kEffectMapFind = 0x1f2410;  // effect by name in the admin's map (+0x68)
constexpr std::uint32_t kImageEffectVftable = 0x9c6910;

U8* g_engine = nullptr;
bool g_verified = false;
bool g_checked = false;
using AdminFn = void* (*)();
using FindFn = void* (*)(void*, const void*);
AdminFn g_admin = nullptr;
FindFn g_find = nullptr;
const void* g_imageEffectVt = nullptr;

// the cached walk: the scene effect it was made for and the filter found in it
const void* g_sceneEffect = nullptr;
U8* g_filter = nullptr;
bool g_logged = false;

template <class T>
T get(const void* p, std::size_t off, T fallback = T{}) {
    T v = fallback;
    if (!readAt(p, off, v)) return fallback;
    return v;
}
U8* ptr(const void* p, std::size_t off) { return get<U8*>(p, off, nullptr); }

std::uint32_t callTarget(std::uint32_t rva) {
    std::uint8_t op = 0;
    std::int32_t rel = 0;
    if (!readAt(g_engine, rva, op) || op != 0xe8 || !readAt(g_engine, rva + 1, rel)) return 0;
    return static_cast<std::uint32_t>(static_cast<std::int64_t>(rva) + 5 + rel);
}

bool verify() {
    if (g_checked) return g_verified;
    g_checked = true;
    g_engine = reinterpret_cast<U8*>(GetModuleHandleW(L"Engine.dll"));
    const auto& s = sym();
    if (g_engine == nullptr || s.imageFilterImageEffectVftable != g_engine + kImageEffectVftable) {
        spdlog::warn("engine: exposure: the nested effect filter's vtable is not at the expected RVA: the engine's exposure is not read");
        return false;
    }
    // the two helpers are the ones the engine's own nested-effect lookup (GetOutputAt 0x6661f0) and the weather parameters call
    if (callTarget(0x666209) != kFilterAdmin || callTarget(0x666216) != kEffectMapFind) {
        spdlog::warn("engine: exposure: the nested effect lookup's call sites are not the verified ones: the engine's exposure is not read");
        return false;
    }
    g_admin = reinterpret_cast<AdminFn>(const_cast<std::uint8_t*>(g_engine) + kFilterAdmin);
    g_find = reinterpret_cast<FindFn>(const_cast<std::uint8_t*>(g_engine) + kEffectMapFind);
    g_imageEffectVt = s.imageFilterImageEffectVftable;
    g_verified = true;
    return true;
}

// the persistent texture-mode effect filter named "AvgLuminance" (class eCImageFilterEffect, output mode +0x38 == 0, ShareTexture
// +0x90 false) in `effect` or in the effects nested below it
U8* findFilter(U8* admin, U8* effect, int depth) {
    if (effect == nullptr || depth > 6) return nullptr;
    const std::int32_t n = get<std::int32_t>(effect, 0x20);
    U8* filters = ptr(effect, 0x18);
    if (n <= 0 || n > 64 || filters == nullptr) return nullptr;
    for (std::int32_t i = 0; i < n; ++i) {
        U8* f = ptr(filters, 8 * static_cast<std::size_t>(i));
        if (f == nullptr) continue;
        if (ptr(f, 0) == g_imageEffectVt) {
            U8* nested = static_cast<U8*>(g_find(const_cast<std::uint8_t*>(admin) + 0x68, f + 0x30));
            if (U8* r = findFilter(admin, nested, depth + 1)) return r;
            continue;
        }
        if (get<std::int32_t>(f, 0x38, -1) == 0 && get<std::uint8_t>(f, 0x90, 1) == 0 && readCString(f + 0x10) == "AvgLuminance") return f;
    }
    return nullptr;
}

}  // namespace

const void* adaptedLuminanceApi() {
    if (!verify()) return nullptr;
    U8* admin = static_cast<U8*>(g_admin());
    if (admin == nullptr) return nullptr;
    U8* scene = ptr(admin, 0x90);  // the scene effect (DefaultEffect)
    if (scene == nullptr) return nullptr;
    if (scene != g_sceneEffect || g_filter == nullptr) {
        g_sceneEffect = scene;
        g_filter = findFilter(admin, scene, 0);
        g_logged = false;
    }
    if (g_filter == nullptr) return nullptr;
    U8* image = ptr(g_filter, 0x70);  // eCGfxImageData of the filter's output
    if (image == nullptr) return nullptr;
    U8* api = ptr(image, 0x60);
    if (api != nullptr && !g_logged) {
        g_logged = true;
        spdlog::info("engine: exposure: adapted luminance = filter {} (image data {}, api image {}) of the scene effect {}", static_cast<const void*>(g_filter),
                     static_cast<const void*>(image), static_cast<const void*>(api), static_cast<const void*>(scene));
    }
    return api;
}

}  // namespace rvk::engine::exposure
