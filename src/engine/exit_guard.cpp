#include "engine/exit_guard.h"

#include "core/exports.h"
#include "core/hooks.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <atomic>

#include <spdlog/spdlog.h>

namespace rvk::engine::exitguard {
namespace {

using ShutdownInProgressFn = BOOLEAN(NTAPI*)();
ShutdownInProgressFn g_shutdownInProgress = nullptr;
std::atomic<unsigned> g_skipped{0};

bool processShuttingDown() { return g_shutdownInProgress != nullptr && g_shutdownInProgress() != FALSE; }

// eCGuiBitmapProxy2::~eCGuiBitmapProxy2() (Engine 0x52f320): `if (m_pBitmap) { m_pBitmap->Release(); m_pBitmap = nullptr; }`
using ProxyDtorFn = void (*)(void* self);
Hook<ProxyDtorFn> g_proxyDtor;

void detourProxyDtor(void* self) {
    if (self != nullptr && processShuttingDown()) {
        auto** bitmap = static_cast<void**>(self);
        if (*bitmap != nullptr) g_skipped.fetch_add(1, std::memory_order_relaxed);
        *bitmap = nullptr;  // (the member the original clears; the object itself is not touched: it may be destroyed already)
        return;
    }
    g_proxyDtor.original(self);
}

// unsigned long eCGuiResource2::Release() (Engine 0x8f140; `this` = the IFFUnknown subobject): any other holder of a GUI resource
// released during the process shutdown
using ReleaseFn = unsigned long (*)(void* self);
Hook<ReleaseFn> g_release;

unsigned long detourRelease(void* self) {
    if (processShuttingDown()) {
        g_skipped.fetch_add(1, std::memory_order_relaxed);
        return 1;  // (callers ignore it; not 0: nobody may conclude the object is gone and touch it)
    }
    return g_release.original(self);
}

// The rich presence thread (Game.dll's worker loop around gCSession::RichPresenceTime, Game 0x970e60 / 0x9710e0) keeps reading
// eCApplication::GetInstance() after WinMain's eCApplication (a local on the main thread's stack) was destroyed: a read through a
// null / garbage vtable (Game.dll+0x970c6e, "read of 0x510"), mostly in the same few ms as ExitProcess (which then kills the thread;
// a race the thread could win). After ~eCApplication the thread is parked: nothing it would do is wanted any more, and nothing joins it
// (it is still running at ExitProcess in every observed exit).
std::atomic<bool> g_appGone{false};

using AppDtorFn = void (*)(void* self);
Hook<AppDtorFn> g_appDtor;

void detourAppDtor(void* self) {
    g_appGone.store(true);
    g_appDtor.original(self);
}

using RichPresenceFn = bool (*)(void* session);
Hook<RichPresenceFn> g_richPresence;

bool detourRichPresence(void* session) {
    if (g_appGone.load()) {
        g_skipped.fetch_add(1, std::memory_order_relaxed);
        for (;;) Sleep(1000);  // (until ExitProcess terminates the thread)
    }
    return g_richPresence.original(session);
}

}  // namespace

bool install() {
    if (HMODULE ntdll = GetModuleHandleW(L"ntdll.dll"); ntdll != nullptr)
        g_shutdownInProgress = reinterpret_cast<ShutdownInProgressFn>(GetProcAddress(ntdll, "RtlDllShutdownInProgress"));
    if (g_shutdownInProgress == nullptr) {
        spdlog::warn("engine: exit guard: ntdll!RtlDllShutdownInProgress missing: the game's exit crash stays");
        return false;
    }
    const bool a = g_proxyDtor.install("eCGuiBitmapProxy2::~eCGuiBitmapProxy2",
                                       exports::resolveAs<ProxyDtorFn>(L"Engine.dll", "??1eCGuiBitmapProxy2@@QEAA@XZ"), &detourProxyDtor);
    const bool b = g_release.install("eCGuiResource2::Release", exports::resolveAs<ReleaseFn>(L"Engine.dll", "?Release@eCGuiResource2@@UEAAKXZ"),
                                     &detourRelease);
    const bool c = g_appDtor.install("eCApplication::~eCApplication", exports::resolveAs<AppDtorFn>(L"Engine.dll", "??1eCApplication@@UEAA@XZ"),
                                     &detourAppDtor) &&
                   g_richPresence.install("gCSession::RichPresenceTime",
                                          exports::resolveAs<RichPresenceFn>(L"Game.dll", "?RichPresenceTime@gCSession@@QEAA_NXZ"), &detourRichPresence);
    spdlog::info("engine: exit guard: {} (GUI resource releases skipped during the process shutdown, the rich presence thread parked after "
                 "~eCApplication: the game's exit crashes, docs/research/shutdown.md)",
                 a && b && c ? "on" : a || b || c ? "partly on" : "FAILED");
    return a || b || c;
}

unsigned skipped() { return g_skipped.load(std::memory_order_relaxed); }

}  // namespace rvk::engine::exitguard
