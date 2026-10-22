#include "engine/shadow_maps.h"

#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstring>

#include <spdlog/spdlog.h>

#include "core/hooks.h"
#include "engine/symbols.h"

namespace rvk::engine::shadowmaps {
namespace {

// eCCascadeShadowMap (shadows.md 3.1): +0x1c MaxDistance, +0x20 LogSplitFactor, +0x24 map size, +0x28 split count
constexpr std::size_t kMaxDistance = 0x1c;
constexpr std::size_t kSplitFactor = 0x20;
constexpr std::size_t kMapSize = 0x24;
constexpr std::size_t kSplitCount = 0x28;

std::atomic<int> g_size{0};
std::atomic<float> g_range{0.0f};
std::atomic<float> g_factor{-1.0f};

template <class T>
T field(void* self, std::size_t offset) {
    T v;
    std::memcpy(&v, static_cast<std::uint8_t*>(self) + offset, sizeof(T));
    return v;
}
template <class T>
void setField(void* self, std::size_t offset, T v) {
    std::memcpy(static_cast<std::uint8_t*>(self) + offset, &v, sizeof(T));
}

// the game's values of the cascade object (render thread): what it had before we wrote, followed when the game changes them
// (eCIlluminationAdmin::ApplyConfiguration after the Shadow Quality option changed)
struct Tracked {
    void* object = nullptr;
    float gameRange = 0.0f, gameFactor = 0.0f;
    int gameSize = 0;
    float wroteRange = 0.0f, wroteFactor = 0.0f;
    int wroteSize = 0;
};
Tracked g_tracked;

using SetIntFn = void (*)(void* self, const int* value);
SetIntFn g_setSize = nullptr;
SetIntFn g_setCount = nullptr;

using RenderFn = bool (*)(void* self);
Hook<RenderFn> g_render;

bool detourRender(void* self) {
    if (self != nullptr) {
        Tracked& t = g_tracked;
        const float range = field<float>(self, kMaxDistance);
        const float factor = field<float>(self, kSplitFactor);
        const int size = field<int>(self, kMapSize);
        const int count = field<int>(self, kSplitCount);
        if (t.object != self) {
            t = Tracked{self, range, factor, size, range, factor, size};
            spdlog::info("engine: shadow cascades: {} of {}^2, range {:.0f}, split factor {:.2f} (the game's)", count, size, range, factor);
        } else {
            if (range != t.wroteRange) t.gameRange = range;
            if (factor != t.wroteFactor) t.gameFactor = factor;
            if (size != t.wroteSize) t.gameSize = size;
        }
        const int wantSize = g_size.load(std::memory_order_relaxed);
        const float wantRange = g_range.load(std::memory_order_relaxed);
        const float wantFactor = g_factor.load(std::memory_order_relaxed);
        const float newRange = wantRange > 0.0f ? wantRange : t.gameRange;
        const float newFactor = wantFactor >= 0.0f ? wantFactor : t.gameFactor;
        const int newSize = wantSize > 0 ? wantSize : t.gameSize;
        if (newRange != range || newFactor != factor) {
            setField(self, kMaxDistance, newRange);
            setField(self, kSplitFactor, newFactor);
            spdlog::info("engine: shadow cascades: range {:.0f}, split factor {:.2f} (game {:.0f}, {:.2f})", newRange, newFactor,
                         t.gameRange, t.gameFactor);
        }
        t.wroteRange = newRange;
        t.wroteFactor = newFactor;
        if (newSize != size && newSize > 0 && count > 0 && g_setSize != nullptr && g_setCount != nullptr) {
            // what ApplyConfiguration does for a new size: the size (frees the splits), then the splits again
            g_setSize(self, &newSize);
            g_setCount(self, &count);
            spdlog::info("engine: shadow cascades: {} maps of {}^2 (game {}^2)", count, newSize, t.gameSize);
        }
        t.wroteSize = field<int>(self, kMapSize);
    }
    return g_render.original(self);
}

}  // namespace

void setOverrides(const Overrides& o) {
    g_size.store(o.mapSize, std::memory_order_relaxed);
    g_range.store(o.range, std::memory_order_relaxed);
    g_factor.store(o.splitFactor, std::memory_order_relaxed);
}

bool install() {
    const auto& s = sym();
    g_setSize = reinterpret_cast<SetIntFn>(s.cascadeSetShadowMapSize);
    g_setCount = reinterpret_cast<SetIntFn>(s.cascadeSetSplitCount);
    if (s.cascadeRenderShadowMaps == nullptr) {
        spdlog::warn("engine: eCCascadeShadowMap::RenderShadowMaps not found: shadow map size / range settings have no effect");
        return false;
    }
    return g_render.install("eCCascadeShadowMap::RenderShadowMaps", reinterpret_cast<RenderFn>(s.cascadeRenderShadowMaps), &detourRender);
}

float splitStart(float range, float factor, int i, int n) {
    if (n <= 0) return 1.0f;
    const float t = static_cast<float>(i) / static_cast<float>(n);
    const float lin = (range - 1.0f) * t + 1.0f;
    const float lg = std::pow(range, t);
    return (lin - lg) * factor + lg;
}

}  // namespace rvk::engine::shadowmaps
