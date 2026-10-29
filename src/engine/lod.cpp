#include "engine/lod.h"

#include <windows.h>

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <set>
#include <tuple>
#include <utility>

#include <spdlog/spdlog.h>

#include "core/hooks.h"
#include "engine/memread.h"
#include "engine/symbols.h"

namespace rvk::engine::lod {
namespace {

// eCWraper_SpeedTreeRT (lod.md 2): +0 CSpeedTreeRT*, +0x30 LoD factor (eCSpeedTree_PS::LoDFactor); the tree's LOD limits
// live in CSpeedTreeRT's instance data (*(tree + 8)): +0x54 near, +0x50 far
constexpr std::size_t kTreeFactor = 0x30;
// eCRenderContextMesh +0x18 -> eCMesh_PS: +0x60 LoD ranges (float[3]), +0x6c LoD count (byte)
constexpr std::size_t kMeshPs = 0x18;
constexpr std::size_t kMeshRanges = 0x60;
constexpr std::size_t kMeshLodCount = 0x6c;
constexpr int kMaxMeshLods = 4;
// eCFrustumDatabase +0x15b4: the view range factor (lod.md 3)
constexpr std::size_t kViewFactor = 0x15b4;

std::atomic<float> g_trees{1.0f};
std::atomic<float> g_meshes{1.0f};
std::atomic<float> g_view{1.0f};
std::atomic<int> g_prefetchStride{1};
std::atomic<bool> g_walkNext{true};

template <class T>
T field(const void* self, std::size_t offset) {
    T v;
    std::memcpy(&v, static_cast<const std::uint8_t*>(self) + offset, sizeof(T));
    return v;
}
template <class T>
void setField(void* self, std::size_t offset, T v) {
    std::memcpy(static_cast<std::uint8_t*>(self) + offset, &v, sizeof(T));
}

// diagnostics: the distinct LOD distances the game uses (first few of each, render thread)
constexpr std::size_t kLogDistinct = 24;
std::set<std::tuple<float, float, float>> g_treeLimits;
std::set<std::tuple<int, float, float, float>> g_meshRanges;

void logTreeLimits(const void* self) {
    if (g_treeLimits.size() >= kLogDistinct) return;
    void* tree = nullptr;
    void* instance = nullptr;
    float nearDist = 0.0f, farDist = 0.0f;
    if (!readAt(self, 0, tree) || !readAt(tree, 8, instance) || !readAt(instance, 0x54, nearDist) || !readAt(instance, 0x50, farDist)) {
        return;
    }
    const float factor = field<float>(self, kTreeFactor);
    // (find first: emplace allocates its node before it looks, and this runs for every tree update until 24 distinct are seen)
    if (const auto key = std::make_tuple(nearDist, farDist, factor); g_treeLimits.find(key) == g_treeLimits.end() && g_treeLimits.insert(key).second) {
        spdlog::info("engine: lod: tree LOD limits near {:.0f}, far {:.0f}, factor {:.2f} (= {:.1f} .. {:.1f} m)", nearDist, farDist, factor,
                     nearDist * factor / 100.0f, farDist * factor / 100.0f);
    }
}

using TreeUpdateFn = void (*)(void* self, const void* position);
Hook<TreeUpdateFn> g_treeUpdate;

void detourTreeUpdate(void* self, const void* position) {
    const float s = g_trees.load(std::memory_order_relaxed);
    if (self == nullptr) return g_treeUpdate.original(self, position);
    logTreeLimits(self);
    if (s == 1.0f) return g_treeUpdate.original(self, position);
    const float factor = field<float>(self, kTreeFactor);
    setField(self, kTreeFactor, factor * s);
    g_treeUpdate.original(self, position);
    setField(self, kTreeFactor, factor);
}

using TreeFadeFn = void (*)(void* self, float* start, float* end);
Hook<TreeFadeFn> g_treeFade;

void detourTreeFade(void* self, float* start, float* end) {
    const float s = g_trees.load(std::memory_order_relaxed);
    if (self == nullptr || s == 1.0f) return g_treeFade.original(self, start, end);
    const float factor = field<float>(self, kTreeFactor);
    setField(self, kTreeFactor, factor * s);
    g_treeFade.original(self, start, end);
    setField(self, kTreeFactor, factor);
}

using MeshUpdateFn = void (*)(void* self, void* frustum);
Hook<MeshUpdateFn> g_meshUpdate;

void detourMeshUpdate(void* self, void* frustum) {
    const float s = g_meshes.load(std::memory_order_relaxed);
    void* ps = self != nullptr ? field<void*>(self, kMeshPs) : nullptr;
    if (ps == nullptr) return g_meshUpdate.original(self, frustum);
    auto* bytes = static_cast<std::uint8_t*>(ps);
    const int count = bytes[kMeshLodCount];
    if (count < 2 || count > kMaxMeshLods) return g_meshUpdate.original(self, frustum);
    auto* ranges = reinterpret_cast<float*>(bytes + kMeshRanges);
    const auto rangeKey = std::make_tuple(count, ranges[0], count > 2 ? ranges[1] : 0.0f, count > 3 ? ranges[2] : 0.0f);
    if (g_meshRanges.size() < kLogDistinct && g_meshRanges.find(rangeKey) == g_meshRanges.end() && g_meshRanges.insert(rangeKey).second) {
        spdlog::info("engine: lod: mesh with {} LODs, ranges {:.0f} {:.0f} {:.0f}", count, ranges[0], count > 2 ? ranges[1] : 0.0f,
                     count > 3 ? ranges[2] : 0.0f);
    }
    if (s == 1.0f) return g_meshUpdate.original(self, frustum);
    float saved[kMaxMeshLods]{};
    const int n = scaleRanges(ranges, count, s, saved);
    g_meshUpdate.original(self, frustum);
    restoreRanges(ranges, n, saved);
}

using UpdateFromAppFn = void (*)(void* self);
Hook<UpdateFromAppFn> g_updateFromApp;

void detourUpdateFromApp(void* self) {
    g_updateFromApp.original(self);
    if (self == nullptr) return;
    // the game copies 1.0 from the caps template; anything else would be a new engine build's own value: keep it then
    const float game = field<float>(self, kViewFactor);
    if (game == 1.0f) setField(self, kViewFactor, g_view.load(std::memory_order_relaxed));
}

// ---- the object draw distance's other gates (lod.md 6) ----
// eCGeometryEntity: +0x28 flags, +0xcc ViewRange, +0xd0 CacheInRange; eCPrefetchAdmin +0x28 the default cache-in range
// (/Engine/Scene/EntityROI, 4000); eCFrustumElementItem +0 the entity
constexpr std::size_t kEntityViewRange = 0xcc;
constexpr std::size_t kEntityCacheInRange = 0xd0;
constexpr std::size_t kPrefetchDefaultRange = 0x28;

std::atomic<void*> g_prefetchAdmin{nullptr};  // the scene's eCPrefetchAdmin, seen in GetPrefetchSphere
std::atomic<bool> g_refreshPending{false};
std::mutex g_rangeLogMutex;
std::set<std::pair<float, float>> g_entityRanges;  // distinct (ViewRange, CacheInRange), diagnostics
std::atomic<std::size_t> g_entityRangesLogged{0};

using PrefetchSphereFn = bool (*)(void* self, void* entity, float* sphere);
Hook<PrefetchSphereFn> g_prefetchSphere;

// An entity is drawn only while cached in: the camera inside this sphere (eCPrefetchAdmin::CacheEntities ->
// OnTestCacheInPos). Its radius is the entity's unscaled range, so it is scaled with lod.view too.
bool detourPrefetchSphere(void* self, void* entity, float* sphere) {
    const bool ok = g_prefetchSphere.original(self, entity, sphere);
    if (!ok || self == nullptr || entity == nullptr || sphere == nullptr) return ok;
    g_prefetchAdmin.store(self, std::memory_order_relaxed);
    const float viewRange = field<float>(entity, kEntityViewRange);
    const float cacheIn = field<float>(entity, kEntityCacheInRange);
    // (a per-thread list of the pairs already looked at keeps the mutex and the set's node allocation off the common path)
    thread_local std::array<std::pair<float, float>, kLogDistinct> seenRanges;
    thread_local std::size_t seenRangeCount = 0;
    bool seen = false;
    for (std::size_t i = 0; i < seenRangeCount && !seen; ++i) seen = seenRanges[i].first == viewRange && seenRanges[i].second == cacheIn;
    if (!seen && g_entityRangesLogged.load(std::memory_order_relaxed) < kLogDistinct) {
        if (seenRangeCount < seenRanges.size()) seenRanges[seenRangeCount++] = {viewRange, cacheIn};
        const std::lock_guard lock(g_rangeLogMutex);
        if (g_entityRanges.size() < kLogDistinct && g_entityRanges.emplace(viewRange, cacheIn).second) {
            g_entityRangesLogged.store(g_entityRanges.size(), std::memory_order_relaxed);
            spdlog::info("engine: lod: entity ViewRange {:.0f}, CacheInRange {:.0f} (cache-in radius {:.0f}, default range {:.0f})",
                         viewRange, cacheIn, sphere[0], field<float>(self, kPrefetchDefaultRange));
        }
    }
    const float s = g_view.load(std::memory_order_relaxed);
    if (s != 1.0f) {
        sphere[0] = scaledCacheInRadius(sphere[0], cacheIn, viewRange, field<float>(self, kPrefetchDefaultRange), s);
    }
    return ok;
}

using CullTreeFn = int (*)(void* self, void* db);
Hook<CullTreeFn> g_cullTree;

// A parent entity culls its whole subtree with its raw ViewRange (CullNode multiplies by the database's factor, CullTree
// does not): scaled by the same factor for the call
int detourCullTree(void* self, void* db) {
    void* entity = self != nullptr ? field<void*>(self, 0) : nullptr;
    const float factor = db != nullptr ? field<float>(db, kViewFactor) : 1.0f;
    if (entity == nullptr || factor == 1.0f) return g_cullTree.original(self, db);
    const float range = field<float>(entity, kEntityViewRange);
    if (range <= 0.0f) return g_cullTree.original(self, db);
    setField(entity, kEntityViewRange, range * factor);
    const int r = g_cullTree.original(self, db);
    setField(entity, kEntityViewRange, range);
    return r;
}

using SetDefaultRangeFn = void (*)(void* self, float range);

// SEH only: SetDefaultCacheInRange re-evaluates every entity's sphere when the value changes (a different value, then back)
bool refreshSpheres(void* admin, SetDefaultRangeFn fn) {
    __try {
        const float d = field<float>(admin, kPrefetchDefaultRange);
        fn(admin, d + 1.0f);
        fn(admin, d);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

}  // namespace

float scaledCacheInRadius(float radius, float cacheInRange, float viewRange, float defaultRange, float scale) {
    // GetPrefetchSphere: CacheInRange when set, else the bounding radius + (ViewRange, else the default range)
    const float term = cacheInRange > 0.0f ? cacheInRange : viewRange > 0.0f ? viewRange : defaultRange;
    return term > 0.0f ? radius + (scale - 1.0f) * term : radius;
}

void onPresent() {
    if (!g_refreshPending.load(std::memory_order_relaxed)) return;
    void* admin = g_prefetchAdmin.load(std::memory_order_relaxed);
    const auto fn = reinterpret_cast<SetDefaultRangeFn>(sym().prefetchSetDefaultRange);
    if (admin == nullptr || fn == nullptr) return;  // no world yet: its entities get the current factor when they are added
    g_refreshPending.store(false, std::memory_order_relaxed);
    const auto t0 = std::chrono::steady_clock::now();
    const bool ok = refreshSpheres(admin, fn);
    spdlog::info("engine: lod: cache-in spheres re-evaluated for object draw distance x{:.2f}{} ({:.1f} ms)",
                 g_view.load(std::memory_order_relaxed), ok ? "" : ": faulted",
                 std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count());
}

int scaleRanges(float* ranges, int count, float scale, float* saved) {
    const int n = count - 1 < kMaxMeshLods ? count - 1 : kMaxMeshLods;
    for (int i = 0; i < n; ++i) {
        saved[i] = ranges[i];
        ranges[i] *= scale;
    }
    return n > 0 ? n : 0;
}

void restoreRanges(float* ranges, int n, const float* saved) {
    for (int i = 0; i < n; ++i) ranges[i] = saved[i];
}

void setPrefetchStride(int stride) {
    g_prefetchStride.store(stride < 1 ? 1 : stride > 8 ? 8 : stride, std::memory_order_relaxed);
    g_walkNext.store(true, std::memory_order_relaxed);
}

void setScales(const Scales& s) {
    g_trees.store(s.trees, std::memory_order_relaxed);
    g_meshes.store(s.meshes, std::memory_order_relaxed);
    if (g_view.exchange(s.view, std::memory_order_relaxed) != s.view) {
        g_refreshPending.store(true, std::memory_order_relaxed);
        g_walkNext.store(true, std::memory_order_relaxed);  // the changed spheres are walked at once
    }
}

Scales scales() {
    return {g_trees.load(std::memory_order_relaxed), g_meshes.load(std::memory_order_relaxed), g_view.load(std::memory_order_relaxed)};
}

using CacheEntitiesFn = void (*)(void* self, const float* center, std::uint8_t slot, bool walkRange, bool walkCache);
Hook<CacheEntitiesFn> g_cacheEntities;
float g_walkCenter[3]{};
int g_sinceWalk = 0;
constexpr float kWalkMoveSq = 100.0f * 100.0f;  // 1 m (world units are cm)

// eCApplication::OnPreRender calls it every frame with the camera position. A repeat call at about the same position changes
// nothing (the tree's per-node bits flip only on a sphere crossing), so most frames skip the two walks: the entities at the
// edge of their cache-in sphere appear or vanish up to stride - 1 frames later; a camera that moved a metre walks at once.
void detourCacheEntities(void* self, const float* center, std::uint8_t slot, bool walkRange, bool walkCache) {
    const int stride = g_prefetchStride.load(std::memory_order_relaxed);
    if (stride > 1 && center != nullptr && (walkRange || walkCache) && !g_walkNext.load(std::memory_order_relaxed) &&
        ++g_sinceWalk < stride) {
        const float dx = center[0] - g_walkCenter[0], dy = center[1] - g_walkCenter[1], dz = center[2] - g_walkCenter[2];
        if (dx * dx + dy * dy + dz * dz < kWalkMoveSq) {
            g_cacheEntities.original(self, center, slot, false, false);  // the dirty lists, the centre: no walks
            return;
        }
    }
    g_sinceWalk = 0;
    g_walkNext.store(false, std::memory_order_relaxed);
    if (center != nullptr) std::memcpy(g_walkCenter, center, sizeof(g_walkCenter));
    g_cacheEntities.original(self, center, slot, walkRange, walkCache);
}

bool install() {
    const auto& s = sym();
    if (!g_cacheEntities.install("eCPrefetchAdmin::CacheEntities", reinterpret_cast<CacheEntitiesFn>(s.prefetchCacheEntities),
                                 &detourCacheEntities)) {
        spdlog::warn("engine: eCPrefetchAdmin::CacheEntities not hooked: the cache-in walk runs every frame");
    }
    bool ok = true;
    if (!g_treeUpdate.install("eCWraper_SpeedTreeRT::UpdateLoD", reinterpret_cast<TreeUpdateFn>(s.speedTreeUpdateLoD), &detourTreeUpdate)) {
        spdlog::warn("engine: eCWraper_SpeedTreeRT::UpdateLoD not hooked: lod.trees has no effect");
        ok = false;
    }
    if (!g_treeFade.install("eCWraper_SpeedTreeRT::GetBillboardFadeValues", reinterpret_cast<TreeFadeFn>(s.speedTreeBillboardFade),
                            &detourTreeFade)) {
        spdlog::warn("engine: eCWraper_SpeedTreeRT::GetBillboardFadeValues not hooked");
    }
    if (!g_meshUpdate.install("eCRenderContextMesh::UpdateLoD", reinterpret_cast<MeshUpdateFn>(s.meshUpdateLoD), &detourMeshUpdate)) {
        spdlog::warn("engine: eCRenderContextMesh::UpdateLoD not hooked: lod.meshes has no effect");
        ok = false;
    }
    if (!g_updateFromApp.install("eCFrustumDatabase::UpdateFromAppTemplate", reinterpret_cast<UpdateFromAppFn>(s.frustumUpdateFromApp),
                                 &detourUpdateFromApp)) {
        spdlog::warn("engine: eCFrustumDatabase::UpdateFromAppTemplate not hooked: lod.view has no effect");
        ok = false;
    }
    if (!g_prefetchSphere.install("eCPrefetchAdmin::GetPrefetchSphere", reinterpret_cast<PrefetchSphereFn>(s.prefetchGetSphere),
                                  &detourPrefetchSphere)) {
        spdlog::warn("engine: eCPrefetchAdmin::GetPrefetchSphere not hooked: lod.view ends at the game's cache-in range");
        ok = false;
    }
    if (!g_cullTree.install("eCFrustumElementItem::CullTree", reinterpret_cast<CullTreeFn>(s.frustumItemCullTree), &detourCullTree)) {
        spdlog::warn("engine: eCFrustumElementItem::CullTree not hooked: grouped objects keep the game's view range");
        ok = false;
    }
    return ok;
}

}  // namespace rvk::engine::lod
