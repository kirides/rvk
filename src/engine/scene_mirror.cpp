#include "engine/scene_mirror.h"

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <unordered_set>

#include <spdlog/spdlog.h>

#include "core/hooks.h"
#include "d3d9/device.h"
#include "engine/memread.h"
#include "engine/symbols.h"
#include "hal/vulkan/buffer_lookup.h"

namespace rvk::engine::mirror {

namespace {

::rvk::scene::Mirror g_mirror;
std::atomic<std::uint64_t> g_frame{0};
std::atomic<void*> g_admin{nullptr};  // the eCPrefetchAdmin (this of the cache-in event)

constexpr std::uint64_t kRefreshEvery = 8;  // frames between two snapshot refreshes (N0: validation, not a render input)
constexpr double kReportSeconds = 5.0;
constexpr std::size_t kMaxParts = 512;

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

// The mesh contexts of an entity [C]: eCRenderContextMesh +0x28 = eCMeshResource2*, resource +0x88 = eCGfxVisual* (+0x30 / +0x38 /
// +0x40 vertex data of streams 0..2, +0x50 declaration, +0x58 index data), sub-mesh table +0x58 (stride 0x48) with the count at +0x60
void readMeshResource(const void* resource, ::rvk::scene::MeshInfo& mi) {
    readAt(resource, 0x88, mi.visual);
    if (mi.visual != nullptr) {
        for (int i = 0; i < 3; ++i) readAt(mi.visual, 0x30 + 8 * static_cast<std::size_t>(i), mi.vertexData[i]);
        readAt(mi.visual, 0x50, mi.declaration);
        readAt(mi.visual, 0x58, mi.indexData);
    }
    const std::uint8_t* table = nullptr;
    std::int32_t count = 0;
    if (!readAt(resource, 0x58, table) || !readAt(resource, 0x60, count) || table == nullptr || count <= 0) return;
    const std::size_t n = std::min<std::size_t>(static_cast<std::size_t>(count), kMaxParts);
    std::vector<std::uint8_t> raw(n * 0x48);
    if (!safeRead(table, raw.data(), raw.size())) return;
    mi.parts.reserve(n);
    for (std::size_t i = 0; i < n; ++i) {
        const std::uint8_t* p = raw.data() + i * 0x48;
        ::rvk::scene::MeshPart part;
        part.subMesh = table + i * 0x48;
        std::memcpy(&part.firstIndex, p + 0x28, 4);
        std::memcpy(&part.indexCount, p + 0x2c, 4);
        std::memcpy(&part.firstVertex, p + 0x30, 4);
        std::memcpy(&part.vertexCount, p + 0x34, 4);
        std::memcpy(&part.material, p + 0x38, 8);
        mi.parts.push_back(part);
    }
}

bool isMeshContext(const void* ctx) {
    const void* vtable = nullptr;
    return readAt(ctx, 0, vtable) && vtable != nullptr && vtable == sym().ctxMeshVftable;
}

std::vector<::rvk::scene::MeshInfo> readMeshes(const void* entity) {
    std::vector<::rvk::scene::MeshInfo> out;
    const void* ctx = nullptr;
    readAt(entity, 0xf0, ctx);
    for (int n = 0; ctx != nullptr && n < 64; ++n) {
        if (isMeshContext(ctx)) {
            ::rvk::scene::MeshInfo mi;
            mi.context = ctx;
            readAt(ctx, 0x28, mi.meshResource);
            if (mi.meshResource != nullptr) readMeshResource(mi.meshResource, mi);
            out.push_back(std::move(mi));
        }
        const void* next = nullptr;
        if (!readAt(ctx, 8, next)) break;
        ctx = next;
    }
    return out;
}

// do the stored mesh contexts still name the engine's (same contexts, same mesh resources: a level of detail switch changes one)?
bool meshesCurrent(const void* entity, const std::vector<::rvk::scene::MeshInfo>& cur) {
    const void* ctx = nullptr;
    readAt(entity, 0xf0, ctx);
    std::size_t i = 0;
    for (int n = 0; ctx != nullptr && n < 64; ++n) {
        if (isMeshContext(ctx)) {
            const void* resource = nullptr;
            readAt(ctx, 0x28, resource);
            if (i >= cur.size() || cur[i].context != ctx || cur[i].meshResource != resource) return false;
            ++i;
        }
        const void* next = nullptr;
        if (!readAt(ctx, 8, next)) break;
        ctx = next;
    }
    return i == cur.size();
}

using EntityEventFn = void (*)(void* self, void* entity);
Hook<EntityEventFn> g_cachedIn, g_cachedOut, g_notifyDelete;

// post-hook: the entity's render contexts exist now (eCFrustumElementItem::CacheIn ran before the admin was told)
void detourCachedIn(void* self, void* entity) {
    g_cachedIn.original(self, entity);
    g_admin.store(self, std::memory_order_relaxed);
    ::rvk::scene::EntityState s;
    if (!snapshot(entity, s)) return;
    g_mirror.cachedIn(entity, s, g_frame.load(std::memory_order_relaxed));
    g_mirror.setMeshes(entity, readMeshes(entity));
}

// pre-hook: before the contexts are freed
void detourCachedOut(void* self, void* entity) {
    g_mirror.cachedOut(entity);
    g_cachedOut.original(self, entity);
}

// the delete net: an entity destroyed while still mirrored (its cache-out comes from inside Destroy, a moment later)
void detourNotifyDelete(void* self, void* entity) {
    g_mirror.deleted(entity);
    g_notifyDelete.original(self, entity);
}

std::chrono::steady_clock::time_point g_lastReport = std::chrono::steady_clock::now();
double g_refreshMs = 0.0;
std::size_t g_refreshed = 0, g_meshReloads = 0;

// N1: the draws' geometry against the mirror's mesh parts
struct PartCheck {
    std::uint64_t draws = 0, partMissing = 0, bufferOk = 0, bufferBad = 0, rangeOk = 0, rangeBad = 0, baseBad = 0, unresolvedBuffers = 0,
        offsetBad = 0, strideBad = 0;
} g_check;
int g_badLogged = 0;

void report() {
    const ::rvk::scene::Counters c = g_mirror.counters();
    std::uint32_t engineIn = 0, engineCtx = 0;
    if (void* pa = g_admin.load(std::memory_order_relaxed)) {
        readAt(pa, 0x528, engineIn);
        readAt(pa, 0x52c, engineCtx);
    }
    std::size_t meshes = 0, parts = 0;
    g_mirror.forEach([&](const ::rvk::scene::Entity& e) {
        meshes += e.meshes.size();
        for (const auto& m : e.meshes) parts += m.parts.size();
    });
    spdlog::info("scene mirror: {} entities ({} with render contexts); the engine counts {} cached in ({} with contexts); added {}, removed {} "
                 "(repeated cache-in {}, cache-out of unknown {}, deleted while mirrored {} / unknown {}); refreshed {} in {:.2f} ms, moved {}, "
                 "mesh contexts reloaded {}; draws resolved {} / unresolved {}; {} mesh contexts, {} parts",
                 g_mirror.size(), g_mirror.withContexts(), engineIn, engineCtx, c.added, c.removed, c.doubleIn, c.outUnknown, c.deletedLive,
                 c.deletedUnknown, g_refreshed, g_refreshMs, c.moved, g_meshReloads, c.drawsResolved, c.drawsUnresolved, meshes, parts);
    if (g_check.draws != 0) {
        spdlog::info("scene mirror: mesh parts vs draws: {} plain mesh draws, sub-mesh unknown to the mirror {}, buffers equal {} / differ {} "
                     "(unresolved HAL objects {}), index range inside the part {} / outside {}, base vertex != 0 {}, stream offset != 0 {}, stride differs {}",
                     g_check.draws, g_check.partMissing, g_check.bufferOk, g_check.bufferBad, g_check.unresolvedBuffers, g_check.rangeOk,
                     g_check.rangeBad, g_check.baseBad, g_check.offsetBad, g_check.strideBad);
    }
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
    if (s.ctxMeshVftable == nullptr) spdlog::warn("engine: eCRenderContextMesh vftable not found: the mirror has no mesh parts");
    return ok;
}

void onPresent() {
    const std::uint64_t frame = g_frame.fetch_add(1, std::memory_order_relaxed) + 1;
    if (frame % kRefreshEvery == 0) {
        const auto t0 = std::chrono::steady_clock::now();
        const std::vector<const void*> all = g_mirror.pointers();
        std::size_t n = 0, reloads = 0;
        for (const void* e : all) {
            ::rvk::scene::EntityState s;
            if (snapshot(e, s) && g_mirror.update(e, s, frame)) ++n;
            // a level of detail switch (another mesh resource) or a new context: read the mesh parts again
            if (!g_mirror.checkMeshes(e, [&](const std::vector<::rvk::scene::MeshInfo>& cur) { return meshesCurrent(e, cur); })) {
                g_mirror.setMeshes(e, readMeshes(e));
                ++reloads;
            }
        }
        g_refreshed = n;
        g_meshReloads += reloads;
        g_refreshMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    }
    const auto now = std::chrono::steady_clock::now();
    if (std::chrono::duration<double>(now - g_lastReport).count() >= kReportSeconds) {
        g_lastReport = now;
        report();
    }
}

void crossCheck(const std::vector<DrawProbe>& draws) {
    std::unordered_set<const void*> seen;
    int logged = 0;
    for (const DrawProbe& d : draws) {
        if (d.entity == nullptr) continue;
        if (seen.insert(d.entity).second && !g_mirror.noteDraw(d.entity) && logged < 3) {
            ++logged;
            std::uintptr_t vt = 0;
            readAt(d.entity, 0, vt);
            spdlog::info("scene mirror: a drawn entity {} is not mirrored (vtable +0x{:x})", d.entity, vt >= 0x180000000ull ? vt - 0x180000000ull : vt);
        }
        if (!d.plainIndexedMesh || d.subMesh == nullptr) continue;
        ++g_check.draws;
        ::rvk::scene::MeshInfo mi;
        ::rvk::scene::MeshPart part;
        if (!g_mirror.lookupPart(d.entity, d.subMesh, mi, part)) {
            ++g_check.partMissing;
            if (g_badLogged < 6) {
                ++g_badLogged;
                spdlog::info("scene mirror: draw of sub-mesh {} (entity {}, mesh resource {}) has no part in the mirror", d.subMesh, d.entity, d.meshResource);
            }
            continue;
        }
        // the buffers behind the mesh resource's HAL objects = the draw's stream 0 / index buffer?
        const void* vHal = nullptr;
        const void* iHal = nullptr;
        if (mi.vertexData[0] != nullptr) readAt(mi.vertexData[0], 0x20, vHal);
        if (mi.indexData != nullptr) readAt(mi.indexData, 0x20, iHal);
        ::rvk::hal::vulkan::VertexBufferRef vb;
        ::rvk::d3d9::Buffer* ib = nullptr;
        if (vHal == nullptr || iHal == nullptr || !::rvk::hal::vulkan::vertexBufferOf(vHal, vb) || !::rvk::hal::vulkan::indexBufferOf(iHal, ib) ||
            vb.buffer == nullptr || ib == nullptr) {
            ++g_check.unresolvedBuffers;
        } else if (vb.buffer->ref().id == d.vertexBufferId && ib->ref().id == d.indexBufferId) {
            ++g_check.bufferOk;
        } else {
            ++g_check.bufferBad;
            if (g_badLogged < 6) {
                ++g_badLogged;
                spdlog::info("scene mirror: sub-mesh {} buffers differ: mirror vb {} ib {}, draw vb {} ib {}", d.subMesh, vb.buffer->ref().id,
                             ib->ref().id, d.vertexBufferId, d.indexBufferId);
            }
        }
        const std::uint64_t end = static_cast<std::uint64_t>(d.first) + static_cast<std::uint64_t>(d.primitives) * 3;
        if (d.first >= part.firstIndex && end <= static_cast<std::uint64_t>(part.firstIndex) + part.indexCount) {
            ++g_check.rangeOk;
        } else {
            ++g_check.rangeBad;
            if (g_badLogged < 6) {
                ++g_badLogged;
                spdlog::info("scene mirror: sub-mesh {} range differs: part [{}, +{}), draw first {} primitives {}", d.subMesh, part.firstIndex,
                             part.indexCount, d.first, d.primitives);
            }
        }
        if (d.baseVertex != 0) ++g_check.baseBad;
        if (d.vertexOffset != 0) ++g_check.offsetBad;
        if (vb.buffer != nullptr && vb.stride != d.vertexStride) ++g_check.strideBad;
    }
}

}  // namespace rvk::engine::mirror
