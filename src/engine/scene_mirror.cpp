#include "engine/scene_mirror.h"

#include <windows.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <unordered_set>

#include <spdlog/spdlog.h>

#include "core/hooks.h"
#include "engine/memread.h"
#include "engine/symbols.h"

namespace rvk::engine::mirror {

namespace {

::rvk::scene::Mirror g_mirror;
std::atomic<std::uint64_t> g_frame{0};
std::atomic<void*> g_admin{nullptr};  // the eCPrefetchAdmin (this of the cache-in event)

constexpr std::uint64_t kRefreshEvery = 8;  // frames between two snapshot refreshes (N0: validation, not a render input)
constexpr double kReportSeconds = 5.0;

// Entity layout [C], native-renderer.md 8: +0x28 flags, +0x60 world matrix (row-major 4x4), +0xb8 world tree sphere (radius,
// centre), +0xf0 head of the render context list (context +0 vtable, +8 next)
bool snapshot(const void* e, ::rvk::scene::EntityState& s) {
    if (!readAt(e, 0x60, s.world)) return false;
    readAt(e, 0x28, s.flags);
    readAt(e, 0xb8, s.sphere[3]);
    readAt(e, 0xbc, s.sphere[0]);
    readAt(e, 0xc0, s.sphere[1]);
    readAt(e, 0xc4, s.sphere[2]);
    s.contexts = 0;
    s.kinds = {};
    const void* ctx = nullptr;
    readAt(e, 0xf0, ctx);
    while (ctx != nullptr && s.contexts < 64) {
        const void* vtable = nullptr;
        readAt(ctx, 0, vtable);
        if (s.contexts < s.kinds.size()) s.kinds[s.contexts] = vtable;
        ++s.contexts;
        const void* next = nullptr;
        if (!readAt(ctx, 8, next)) break;
        ctx = next;
    }
    return true;
}

using EntityEventFn = void (*)(void* self, void* entity);
Hook<EntityEventFn> g_cachedIn, g_cachedOut, g_notifyDelete;

// post-hook: the entity's render contexts exist now (eCFrustumElementItem::CacheIn ran before the admin was told)
void detourCachedIn(void* self, void* entity) {
    g_cachedIn.original(self, entity);
    g_admin.store(self, std::memory_order_relaxed);
    ::rvk::scene::EntityState s;
    if (snapshot(entity, s)) g_mirror.cachedIn(entity, s, g_frame.load(std::memory_order_relaxed));
}

// pre-hook: before the contexts are freed
void detourCachedOut(void* self, void* entity) {
    g_mirror.cachedOut(entity);
    g_cachedOut.original(self, entity);
}

// the delete net: an entity destroyed while still mirrored (counted: the engine always cached it out first if this stays 0)
void detourNotifyDelete(void* self, void* entity) {
    g_mirror.deleted(entity);
    g_notifyDelete.original(self, entity);
}

std::chrono::steady_clock::time_point g_lastReport = std::chrono::steady_clock::now();
double g_refreshMs = 0.0;
std::size_t g_refreshed = 0;

void report() {
    const ::rvk::scene::Counters c = g_mirror.counters();
    std::uint32_t engineIn = 0, engineCtx = 0;
    if (void* pa = g_admin.load(std::memory_order_relaxed)) {
        readAt(pa, 0x528, engineIn);
        readAt(pa, 0x52c, engineCtx);
    }
    spdlog::info("scene mirror: {} entities ({} with render contexts); the engine counts {} cached in ({} with contexts); added {}, removed {} "
                 "(repeated cache-in {}, cache-out of unknown {}, deleted while mirrored {} / unknown {}); refreshed {} in {:.2f} ms, moved {}; "
                 "draws resolved {} / unresolved {}",
                 g_mirror.size(), g_mirror.withContexts(), engineIn, engineCtx, c.added, c.removed, c.doubleIn, c.outUnknown, c.deletedLive,
                 c.deletedUnknown, g_refreshed, g_refreshMs, c.moved, c.drawsResolved, c.drawsUnresolved);
}

}  // namespace

::rvk::scene::Mirror& get() { return g_mirror; }

bool install() {
    const auto& s = sym();
    bool ok = true;
    if (!g_cachedIn.install("eCPrefetchAdmin::OnEntityCachedIn", reinterpret_cast<EntityEventFn>(s.prefetchEntityCachedIn), &detourCachedIn)) ok = false;
    if (!g_cachedOut.install("eCPrefetchAdmin::OnEntityCachedOut", reinterpret_cast<EntityEventFn>(s.prefetchEntityCachedOut), &detourCachedOut)) ok = false;
    if (!g_notifyDelete.install("eCEntityAdmin::NotifyDeleteEntities", reinterpret_cast<EntityEventFn>(s.entityAdminNotifyDelete), &detourNotifyDelete)) ok = false;
    if (!ok) spdlog::warn("engine: scene mirror hooks incomplete (OnEntityCachedIn / OnEntityCachedOut / NotifyDeleteEntities)");
    return ok;
}

void onPresent() {
    const std::uint64_t frame = g_frame.fetch_add(1, std::memory_order_relaxed) + 1;
    if (frame % kRefreshEvery == 0) {
        const auto t0 = std::chrono::steady_clock::now();
        const std::vector<const void*> all = g_mirror.pointers();
        std::size_t n = 0;
        for (const void* e : all) {
            ::rvk::scene::EntityState s;
            if (snapshot(e, s) && g_mirror.update(e, s, frame)) ++n;
        }
        g_refreshed = n;
        g_refreshMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    }
    const auto now = std::chrono::steady_clock::now();
    if (std::chrono::duration<double>(now - g_lastReport).count() >= kReportSeconds) {
        g_lastReport = now;
        report();
    }
}

void crossCheck(const std::vector<const void*>& drawEntities) {
    std::unordered_set<const void*> seen;
    int logged = 0;
    for (const void* e : drawEntities) {
        if (e == nullptr || !seen.insert(e).second) continue;
        if (!g_mirror.noteDraw(e) && logged < 3) {
            ++logged;
            std::uintptr_t vt = 0;
            readAt(e, 0, vt);
            spdlog::info("scene mirror: a drawn entity {} is not mirrored (vtable +0x{:x})", e, vt >= 0x180000000ull ? vt - 0x180000000ull : vt);
        }
    }
}

}  // namespace rvk::engine::mirror
