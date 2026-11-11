#include "engine/scene_table.h"

#include <windows.h>

#include <chrono>
#include <cstring>
#include <unordered_map>
#include <vector>

#include <spdlog/spdlog.h>

#include "core/hooks.h"
#include "engine/memread.h"
#include "engine/symbols.h"

namespace rvk::engine::scenetable {

namespace {

constexpr std::uint32_t kNone = UINT32_MAX;
constexpr std::size_t kEntityWorld = 0x60, kEntityFlags = 0x28;

std::vector<Slot> g_slots;
std::vector<std::uint64_t> g_stamps;  // per slot: 0 = free, else 1<<63 | generation << 32 | version (a compact copy: change checks read 8 bytes, not the 200-byte slot)
inline std::uint64_t mkStamp(const Slot& s) { return (1ull << 63) | (static_cast<std::uint64_t>(s.generation & 0x7fffffffu) << 32) | s.version; }
std::vector<std::uint32_t> g_free;
bool g_trackFreed = false;
std::vector<std::uint32_t> g_freedDirty;  // slots freed since the last drainFreed (the GPU records are marked dead)
std::unordered_map<const void*, std::uint32_t> g_index;
Counters g_c;
std::uint64_t g_frame = 0;
std::uint32_t g_live = 0;
int g_badLogged = 0;
std::chrono::steady_clock::time_point g_lastReport = std::chrono::steady_clock::now();

using EntityEventFn = void (*)(void* self, void* entity);
using WorldMatrixFn = void (*)(void* self, const float* now, const float* before);
Hook<EntityEventFn> g_cachedIn, g_cachedOut, g_notifyDelete;
Hook<WorldMatrixFn> g_dynWorld, g_geoWorld;

void freeSlot(std::uint32_t i) {
    Slot& s = g_slots[i];
    g_index.erase(s.entity);
    s.entity = nullptr;
    s.dirty = false;
    g_stamps[i] = 0;
    g_free.push_back(i);
    if (g_trackFreed) g_freedDirty.push_back(i);
    --g_live;
}

void allocate(const void* entity) {
    std::uint32_t i;
    if (!g_free.empty()) {
        i = g_free.back();
        g_free.pop_back();
    } else {
        i = static_cast<std::uint32_t>(g_slots.size());
        g_slots.emplace_back();
        g_stamps.push_back(0);
    }
    Slot& s = g_slots[i];
    const std::uint32_t gen = s.generation + 1;
    s = Slot{};
    s.generation = gen;
    s.entity = entity;
    if (!readAt(entity, kEntityWorld, s.world)) std::memset(s.world, 0, sizeof(s.world));
    std::memcpy(s.prev, s.world, sizeof(s.prev));
    readAt(entity, kEntityFlags, s.flags);
    s.dirty = true;
    g_stamps[i] = mkStamp(s);
    g_index[entity] = i;
    ++g_live;
}

// post-hook: the contexts exist and the matrix has been read by the engine for them
void detourCachedIn(void* self, void* entity) {
    g_cachedIn.original(self, entity);
    ++g_c.cachedIn;
    const auto it = g_index.find(entity);
    if (it != g_index.end()) {
        ++g_c.doubleIn;
        freeSlot(it->second);
    }
    allocate(entity);
}

void detourCachedOut(void* self, void* entity) {
    const auto it = g_index.find(entity);
    if (it == g_index.end()) {
        ++g_c.outUnknown;
    } else {
        ++g_c.cachedOut;
        freeSlot(it->second);
    }
    g_cachedOut.original(self, entity);
}

void detourDelete(void* self, void* entity) {
    const auto it = g_index.find(entity);
    if (it != g_index.end()) {
        ++g_c.deletedLive;
        freeSlot(it->second);
    }
    ++g_c.deleted;
    g_notifyDelete.original(self, entity);
}

void onMatrix(void* self, const float* now) {
    ++g_c.matrixSignals;
    const auto it = g_index.find(self);
    if (it == g_index.end()) {
        ++g_c.matrixUnknown;  // not cached in (load time, off-screen): cache-in reads the matrix
        return;
    }
    Slot& s = g_slots[it->second];
    float m[16];
    if (!safeReadN<64>(now, m)) return;
    if (std::memcmp(m, s.world, sizeof(m)) == 0) {
        ++g_c.matrixSame;
        return;
    }
    std::memcpy(s.prev, s.world, sizeof(s.prev));
    std::memcpy(s.world, m, sizeof(m));
    ++s.version;
    g_stamps[it->second] = mkStamp(s);
    s.movedFrame = g_frame;
    s.dirty = true;
}

void detourDynWorld(void* self, const float* now, const float* before) {
    onMatrix(self, now);
    g_dynWorld.original(self, now, before);
}

void detourGeoWorld(void* self, const float* now, const float* before) {
    onMatrix(self, now);
    g_geoWorld.original(self, now, before);
}

}  // namespace

bool install() {
    const auto& s = sym();
    bool ok = true;
    ok &= g_cachedIn.install("eCPrefetchAdmin::OnEntityCachedIn", reinterpret_cast<EntityEventFn>(s.prefetchEntityCachedIn), &detourCachedIn);
    ok &= g_cachedOut.install("eCPrefetchAdmin::OnEntityCachedOut", reinterpret_cast<EntityEventFn>(s.prefetchEntityCachedOut), &detourCachedOut);
    ok &= g_notifyDelete.install("eCEntityAdmin::NotifyDeleteEntities", reinterpret_cast<EntityEventFn>(s.entityAdminNotifyDelete), &detourDelete);
    ok &= g_dynWorld.install("eCDynamicEntity::OnUpdatedWorldMatrix", reinterpret_cast<WorldMatrixFn>(s.dynEntityWorldMatrix), &detourDynWorld);
    ok &= g_geoWorld.install("eCGeometryEntity::OnUpdatedWorldMatrix", reinterpret_cast<WorldMatrixFn>(s.geoEntityWorldMatrix), &detourGeoWorld);
    if (!ok) spdlog::warn("engine: scene table: a signal hook is missing (cache-in / out, delete, world matrix): the table is incomplete");
    return ok;
}

std::uint32_t liveSlots() { return g_live; }
std::uint32_t capacity() { return static_cast<std::uint32_t>(g_slots.size()); }
std::uint64_t stamp(std::uint32_t i) { return i < g_stamps.size() ? g_stamps[i] : 0; }
const Slot* slot(std::uint32_t i) { return i < g_slots.size() && g_slots[i].entity != nullptr ? &g_slots[i] : nullptr; }
std::uint32_t find(const void* entity) {
    const auto it = g_index.find(entity);
    return it == g_index.end() ? kNone : it->second;
}
const Counters& counters() { return g_c; }

std::uint32_t drainDirty(const std::function<void(std::uint32_t, const Slot&)>& fn) {
    std::uint32_t n = 0;
    for (std::uint32_t i = 0; i < g_slots.size(); ++i) {
        Slot& s = g_slots[i];
        if (s.entity == nullptr || !s.dirty) continue;
        if (fn) fn(i, s);
        s.dirty = false;
        ++n;
    }
    g_c.drained += n;
    if (n != 0) ++g_c.batches;
    return n;
}

void trackFreed(bool on) {
    g_trackFreed = on;
    if (!on) g_freedDirty.clear();
}

std::uint32_t drainFreed(const std::function<void(std::uint32_t)>& fn) {
    const std::uint32_t n = static_cast<std::uint32_t>(g_freedDirty.size());
    if (fn) {
        for (const std::uint32_t i : g_freedDirty) fn(i);
    }
    g_freedDirty.clear();
    return n;
}

void checkEntity(const void* entity) {
    ++g_c.checked;
    const auto it = g_index.find(entity);
    if (it == g_index.end()) {
        ++g_c.unresolved;
        if (g_badLogged < 8) {
            ++g_badLogged;
            spdlog::info("scene table: an op's entity {} has no slot (frame {})", entity, g_frame);
        }
        return;
    }
    ++g_c.resolved;
    float m[16];
    if (!readAt(entity, kEntityWorld, m)) return;
    if (std::memcmp(m, g_slots[it->second].world, sizeof(m)) == 0) {
        ++g_c.matrixOk;
    } else {
        ++g_c.matrixBad;
        if (g_badLogged < 8) {
            ++g_badLogged;
            spdlog::info("scene table: entity {} slot {}: the matrix differs from the entity's (missed signal; slot version {}, moved frame {}, frame {})", entity,
                         it->second, g_slots[it->second].version, g_slots[it->second].movedFrame, g_frame);
        }
    }
}

void testCacheIn(const void* entity) {
    const auto it = g_index.find(entity);
    if (it != g_index.end()) freeSlot(it->second);
    allocate(entity);
}
void testCacheOut(const void* entity) {
    const auto it = g_index.find(entity);
    if (it != g_index.end()) freeSlot(it->second);
}
void testMove(const void* entity, const float world[16]) { onMatrix(const_cast<void*>(entity), world); }

void onPresent() {
    ++g_frame;
    const auto now = std::chrono::steady_clock::now();
    if (std::chrono::duration<double>(now - g_lastReport).count() < 5.0) return;
    g_lastReport = now;
    const Counters& c = g_c;
    spdlog::info("scene table: {} live slots ({} allocated); cache-in {} (repeated {}), cache-out {} (unknown {}), deleted {} (still live {}); matrix signals {} "
                 "(unknown entity {}, unchanged {}); checked {} resolved {} unresolved {} matrix equal {} DIFFERING {}; dirty batches {} ({} slots)",
                 g_live, g_slots.size(), c.cachedIn, c.doubleIn, c.cachedOut, c.outUnknown, c.deleted, c.deletedLive, c.matrixSignals, c.matrixUnknown,
                 c.matrixSame, c.checked, c.resolved, c.unresolved, c.matrixOk, c.matrixBad, c.batches, c.drained);
}

}  // namespace rvk::engine::scenetable
