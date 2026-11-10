#include "engine/image_effects.h"

#include <windows.h>

#include <charconv>
#include <cstdint>
#include <format>
#include <vector>

#include <spdlog/spdlog.h>

#include "engine/memread.h"
#include "engine/symbols.h"

namespace rvk::engine::imagefx {

namespace {

struct alignas(16) EngineString {
    unsigned char storage[32]{};
};
using StringCtorFn = void* (*)(EngineString*, const char*);
using StringDtorFn = void (*)(EngineString*);
using AdminFn = void* (*)();
using ActivateFn = bool (*)(void*, const EngineString*);
using DefaultFn = bool (*)(void*);
using StartOverlayFn = bool (*)(void*, const EngineString*, float, float, float);
using StopOverlayFn = bool (*)(void*, bool);

// Engine.dll: FUN_180095750 returns the eCImageFilterAdmin (the cached FindModule lookup; the call at 0x666209 of eCImageFilterImageEffect::GetOutputAt is
// checked, as engine/exposure.cpp does)
constexpr std::uint32_t kFilterAdmin = 0x95750;

std::uint32_t callTarget(const std::uint8_t* engine, std::uint32_t rva) {
    std::uint8_t op = 0;
    std::int32_t rel = 0;
    if (!readAt(engine, rva, op) || op != 0xe8 || !readAt(engine, rva + 1, rel)) return 0;
    return static_cast<std::uint32_t>(static_cast<std::int64_t>(rva) + 5 + rel);
}

std::vector<std::string_view> words(std::string_view s) {
    std::vector<std::string_view> w;
    std::size_t i = 0;
    while (i < s.size()) {
        while (i < s.size() && s[i] == ' ') ++i;
        const std::size_t j = s.find(' ', i);
        const std::size_t e = j == std::string_view::npos ? s.size() : j;
        if (e > i) w.push_back(s.substr(i, e - i));
        i = e;
    }
    return w;
}

float number(std::string_view s, float fallback) {
    float v = fallback;
    const auto r = std::from_chars(s.data(), s.data() + s.size(), v);
    return r.ec == std::errc() ? v : fallback;
}

}  // namespace

bool runCommand(std::string_view rest, std::string* out) {
    const auto fail = [&](std::string why) {
        if (out != nullptr) *out = std::move(why);
        return false;
    };
    const auto* engine = reinterpret_cast<const std::uint8_t*>(GetModuleHandleW(L"Engine.dll"));
    const auto& s = sym();
    if (engine == nullptr || s.stringCtor == nullptr || s.stringDtor == nullptr) return fail("Engine.dll / bCString not available");
    if (callTarget(engine, 0x666209) != kFilterAdmin) return fail("the image filter admin getter is not the verified one");
    void* admin = reinterpret_cast<AdminFn>(const_cast<std::uint8_t*>(engine) + kFilterAdmin)();
    if (admin == nullptr) return fail("no image filter admin");
    const HMODULE mod = GetModuleHandleW(L"Engine.dll");
    const auto proc = [&](const char* name) { return reinterpret_cast<void*>(GetProcAddress(mod, name)); };
    const std::vector<std::string_view> w = words(rest);
    if (w.empty()) return fail("post effect <name> | post default | post overlay <name> [duration [fade-in [fade-out]]] | post overlay stop [fade]");
    const auto withName = [&](std::string_view name, auto&& call) {
        EngineString str;
        const std::string n(name);
        reinterpret_cast<StringCtorFn>(s.stringCtor)(&str, n.c_str());
        const bool r = call(&str);
        reinterpret_cast<StringDtorFn>(s.stringDtor)(&str);
        return r;
    };
    bool r = false;
    std::string what;
    if (w[0] == "effect" && w.size() == 2) {
        auto* fn = reinterpret_cast<ActivateFn>(proc("?ActivateEffect@eCImageFilterAdmin@@QEAA_NAEBVbCString@@@Z"));
        if (fn == nullptr) return fail("ActivateEffect not exported");
        r = withName(w[1], [&](const EngineString* str) { return fn(admin, str); });
        what = std::format("ActivateEffect({})", w[1]);
    } else if (w[0] == "default" && w.size() == 1) {
        auto* fn = reinterpret_cast<DefaultFn>(proc("?ActivateDefaultEffect@eCImageFilterAdmin@@QEAA_NXZ"));
        if (fn == nullptr) return fail("ActivateDefaultEffect not exported");
        r = fn(admin);
        what = "ActivateDefaultEffect";
    } else if (w[0] == "overlay" && w.size() >= 2 && w[1] == "stop") {
        auto* fn = reinterpret_cast<StopOverlayFn>(proc("?StopOverlayEffect@eCImageFilterAdmin@@QEAA_N_N@Z"));
        if (fn == nullptr) return fail("StopOverlayEffect not exported");
        const bool fade = w.size() >= 3 && w[2] == "fade";
        r = fn(admin, fade);
        what = std::format("StopOverlayEffect({})", fade);
    } else if (w[0] == "overlay" && w.size() >= 2) {
        auto* fn = reinterpret_cast<StartOverlayFn>(proc("?StartOverlayEffect@eCImageFilterAdmin@@QEAA_NAEBVbCString@@MMM@Z"));
        if (fn == nullptr) return fail("StartOverlayEffect not exported");
        const float dur = w.size() >= 3 ? number(w[2], 0.0f) : 0.0f, in = w.size() >= 4 ? number(w[3], 0.0f) : 0.0f, outT = w.size() >= 5 ? number(w[4], 0.0f) : 0.0f;
        r = withName(w[1], [&](const EngineString* str) { return fn(admin, str, dur, in, outT); });
        what = std::format("StartOverlayEffect({}, duration {}, fade-in {}, fade-out {})", w[1], dur, in, outT);
    } else {
        return fail("post effect <name> | post default | post overlay <name> [duration [fade-in [fade-out]]] | post overlay stop [fade]");
    }
    spdlog::info("engine: image effects: {} -> {}", what, r);
    if (!r) return fail(what + " returned false (an unknown effect name: the engine logs 'image effect ... nopt found')");
    if (out != nullptr) *out = what;
    return true;
}

}  // namespace rvk::engine::imagefx
