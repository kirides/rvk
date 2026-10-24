#include "engine/shadow_maps.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstring>

#include <spdlog/spdlog.h>

#include "core/hooks.h"
#include "d3d9/device.h"
#include "engine/scene_hooks.h"
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
std::atomic<int> g_stride{2};
std::atomic<float> g_sun[3]{};
std::atomic<bool> g_sunKnown{false};
std::atomic<float> g_trueSun[3]{};
std::atomic<bool> g_trueSunKnown{false};

// the throttle's memory (render thread): the camera (inverse view: rows 0..2 the axes, row 3 the position) of the last real update
struct Throttle {
    bool valid = false;
    int skipped = 0;
    std::uint32_t cascades = 0;  // shadow depth maps the last real update drew
    float axes[12]{};
    float pos[3]{};
    float sun[3]{};
    float range = 0.0f, factor = 0.0f;
    int size = 0;
    double time = 0.0;
};
Throttle g_throttle;
constexpr float kMoveLimit = 150.0f;       // cm the camera may drift from the last update
constexpr float kTurnLimit = 0.015f;      // a view-projection entry (rows 0..2) moved: ~1 degree
constexpr double kStaleSeconds = 0.25;     // a longer gap (loading, menu): always redraw

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
using LightUpdateFn = void (*)(void* self);
Hook<LightUpdateFn> g_lightUpdate;

// eCDirectionalLight_PS::UpdateShadowMaps (shadows.md 3, rt-shadows.md 5): the sun's true direction at +0x80 (unnormalised or not:
// normalised here), before the call quantises it for the cascade object
void detourLightUpdate(void* self) {
    if (self != nullptr) {
        float v[3];
        for (int i = 0; i < 3; ++i) v[i] = field<float>(self, 0x80 + 4 * i);
        const float len = std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
        if (std::isfinite(len) && len > 1e-4f) {
            for (int i = 0; i < 3; ++i) g_trueSun[i].store(v[i] / len, std::memory_order_relaxed);
            g_trueSunKnown.store(true, std::memory_order_relaxed);
        }
    }
    g_lightUpdate.original(self);
}

bool detourRender(void* self) {
    if (self != nullptr) {
        for (int i = 0; i < 3; ++i) g_sun[i].store(field<float>(self, 0x10 + 4 * i), std::memory_order_relaxed);
        g_sunKnown.store(true, std::memory_order_relaxed);
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
    // alternate frames: the maps and their matrices stay in the cascade object, the mask pass reads them as they are
    const int stride = g_stride.load(std::memory_order_relaxed);
    d3d9::Device* dev = stride > 1 && self != nullptr ? engine::scene::renderDevice() : nullptr;
    // the cascades are drawn before the main view of the frame: its camera is not read yet, the last frame's is
    const auto* cam = dev != nullptr && dev->camera().prevValid ? &dev->camera() : nullptr;
    Throttle& th = g_throttle;
    if (cam != nullptr) {
        const auto& vp = cam->prevViewProjection;
        float axes[12], sun[3], pos[3] = {cam->prevPosition[0], cam->prevPosition[1], cam->prevPosition[2]};
        for (int r = 0; r < 3; ++r)
            for (int c = 0; c < 4; ++c) axes[r * 4 + c] = vp.at(r, c);  // rows 0..2: rotation and projection (row 3 = translation)
        for (int i = 0; i < 3; ++i) sun[i] = field<float>(self, 0x10 + 4 * i);
        const float range = field<float>(self, kMaxDistance), factor = field<float>(self, kSplitFactor);
        const int size = field<int>(self, kMapSize);
        const double now = dev->secondsSinceCreate();
        bool redraw = !th.valid || th.skipped + 1 >= stride || th.cascades == 0 || now - th.time > kStaleSeconds || range != th.range ||
                      factor != th.factor || size != th.size;
        if (!redraw) {
            const float dx = pos[0] - th.pos[0], dy = pos[1] - th.pos[1], dz = pos[2] - th.pos[2];
            redraw = dx * dx + dy * dy + dz * dz > kMoveLimit * kMoveLimit;
            for (int i = 0; i < 12 && !redraw; ++i) redraw = std::fabs(axes[i] - th.axes[i]) > kTurnLimit;
            const float sx = sun[0] - th.sun[0], sy = sun[1] - th.sun[1], sz = sun[2] - th.sun[2];
            redraw = redraw || sx * sx + sy * sy + sz * sz > 3e-6f;  // ~0.1 degree: the sun creeps with game time
        }
        static std::atomic<int> reasons[2]{};
        if (reasons[redraw ? 1 : 0].fetch_add(1) % 600 == 0) spdlog::info("engine: shadow cascades: {} updates, {} skipped so far", reasons[1].load(), reasons[0].load());
        if (!redraw) {
            ++th.skipped;
            dev->creditShadowDepthMaps(th.cascades);
            th.time = now;
            return true;
        }
        const bool r = g_render.original(self);
        th = Throttle{true, 0, static_cast<std::uint32_t>(std::max(field<int>(self, kSplitCount), 0)), {}, {}, {}, range, factor, size, now};
        std::memcpy(th.axes, axes, sizeof axes);
        std::memcpy(th.pos, pos, sizeof pos);
        std::memcpy(th.sun, sun, sizeof sun);
        return r;
    }
    th.valid = false;
    return g_render.original(self);
}

}  // namespace

void setUpdateStride(int stride) { g_stride.store(stride < 1 ? 1 : stride, std::memory_order_relaxed); }

void setOverrides(const Overrides& o) {
    g_size.store(o.mapSize, std::memory_order_relaxed);
    g_range.store(o.range, std::memory_order_relaxed);
    g_factor.store(o.splitFactor, std::memory_order_relaxed);
}

bool sunVector(float out[3]) {
    if (!g_sunKnown.load(std::memory_order_relaxed)) return false;
    for (int i = 0; i < 3; ++i) out[i] = g_sun[i].load(std::memory_order_relaxed);
    return true;
}

bool trueSunVector(float out[3]) {
    if (!g_trueSunKnown.load(std::memory_order_relaxed)) return false;
    for (int i = 0; i < 3; ++i) out[i] = g_trueSun[i].load(std::memory_order_relaxed);
    return true;
}

bool install() {
    const auto& s = sym();
    if (s.directionalLightUpdateShadowMaps != nullptr) {
        g_lightUpdate.install("eCDirectionalLight_PS::UpdateShadowMaps", reinterpret_cast<LightUpdateFn>(s.directionalLightUpdateShadowMaps),
                              &detourLightUpdate);
    }
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
