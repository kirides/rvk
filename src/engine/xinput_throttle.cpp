#include "engine/xinput_throttle.h"

#include <windows.h>

#include <atomic>
#include <cstdint>

#include <spdlog/spdlog.h>

#include "core/hooks.h"

namespace rvk::engine::xinput {
namespace {

using GetStateFn = DWORD(WINAPI*)(DWORD, void*);
using SetStateFn = DWORD(WINAPI*)(DWORD, void*);
Hook<GetStateFn> g_get;
Hook<SetStateFn> g_set;
std::atomic<int> g_intervalMs{1000};
std::atomic<bool> g_installed{false};
std::atomic<std::uint64_t> g_skipped{0};
constexpr DWORD kPads = 4;
std::atomic<std::uint64_t> g_getRetryAt[kPads], g_setRetryAt[kPads];  // GetTickCount64 value before which a disconnected index is not asked again

bool throttled(std::atomic<std::uint64_t>* retry, DWORD index) {
    const int interval = g_intervalMs.load(std::memory_order_relaxed);
    if (interval <= 0 || index >= kPads) return false;
    return GetTickCount64() < retry[index].load(std::memory_order_relaxed);
}

void note(std::atomic<std::uint64_t>* retry, DWORD index, DWORD result) {
    if (index >= kPads) return;
    retry[index].store(result == ERROR_DEVICE_NOT_CONNECTED ? GetTickCount64() + static_cast<std::uint64_t>(g_intervalMs.load(std::memory_order_relaxed)) : 0,
                       std::memory_order_relaxed);
}

DWORD WINAPI detourGet(DWORD index, void* state) {
    if (throttled(g_getRetryAt, index)) {
        g_skipped.fetch_add(1, std::memory_order_relaxed);
        return ERROR_DEVICE_NOT_CONNECTED;
    }
    const DWORD r = g_get.original(index, state);
    note(g_getRetryAt, index, r);
    return r;
}

DWORD WINAPI detourSet(DWORD index, void* vibration) {
    if (throttled(g_setRetryAt, index)) {
        g_skipped.fetch_add(1, std::memory_order_relaxed);
        return ERROR_DEVICE_NOT_CONNECTED;
    }
    const DWORD r = g_set.original(index, vibration);
    note(g_setRetryAt, index, r);
    return r;
}

}  // namespace

void setIntervalMs(int ms) {
    g_intervalMs.store(ms < 0 ? 0 : ms, std::memory_order_relaxed);
    for (auto& a : g_getRetryAt) a.store(0, std::memory_order_relaxed);
    for (auto& a : g_setRetryAt) a.store(0, std::memory_order_relaxed);
}

void onPresent() {
    if (g_installed.load(std::memory_order_relaxed)) return;
    const HMODULE mod = GetModuleHandleW(L"xinput1_4.dll");
    if (mod == nullptr) return;
    g_installed.store(true, std::memory_order_relaxed);  // one attempt
    const auto get = reinterpret_cast<GetStateFn>(GetProcAddress(mod, "XInputGetState"));
    const auto set = reinterpret_cast<SetStateFn>(GetProcAddress(mod, "XInputSetState"));
    bool ok = get != nullptr && g_get.install("XInputGetState", get, &detourGet);
    ok = (set != nullptr && g_set.install("XInputSetState", set, &detourSet)) && ok;
    if (ok && hooks::enableAll()) {
        spdlog::info("engine: xinput: disconnected pads are polled every {} ms", g_intervalMs.load(std::memory_order_relaxed));
    } else {
        spdlog::warn("engine: xinput: hooks not installed (every poll goes to xinput1_4)");
    }
}

}  // namespace rvk::engine::xinput
