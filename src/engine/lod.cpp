#include "engine/lod.h"

#include <atomic>
#include <cstdint>
#include <cstring>
#include <set>
#include <tuple>

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
    if (g_treeLimits.emplace(nearDist, farDist, factor).second) {
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
    if (g_meshRanges.size() < kLogDistinct &&
        g_meshRanges.emplace(count, ranges[0], count > 2 ? ranges[1] : 0.0f, count > 3 ? ranges[2] : 0.0f).second) {
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

}  // namespace

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

void setScales(const Scales& s) {
    g_trees.store(s.trees, std::memory_order_relaxed);
    g_meshes.store(s.meshes, std::memory_order_relaxed);
    g_view.store(s.view, std::memory_order_relaxed);
}

Scales scales() {
    return {g_trees.load(std::memory_order_relaxed), g_meshes.load(std::memory_order_relaxed), g_view.load(std::memory_order_relaxed)};
}

bool install() {
    const auto& s = sym();
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
    return ok;
}

}  // namespace rvk::engine::lod
