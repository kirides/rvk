#include "engine/shadow_maps.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstring>

#include <windows.h>

#include <spdlog/spdlog.h>

#include "core/hooks.h"
#include "d3d9/device.h"
#include "engine/memread.h"
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
std::atomic<bool> g_everyFrame{false};
std::atomic<float> g_sun[3]{};
std::atomic<bool> g_sunKnown{false};
std::atomic<float> g_trueSun[3]{};
std::atomic<bool> g_trueSunKnown{false};
std::atomic<const void*> g_cascadeObject{nullptr};
std::atomic<int> g_fit{0};
std::atomic<bool> g_fitActive{false};
FitInfo g_lastFit[4];
float g_loggedSide[4]{};

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

std::atomic<void (*)(void*)> g_fitTap{nullptr};

// ---- the fit override (shadows.md 8): a pre-hook of eCShadowMapDirectionalLight2::UpdateCamera (0x5a13e0) rewrites the split's light
// box (+0xe0: min xyz, +0xec: max xyz, in the split's light view +0x60) before the engine builds its camera, projection (with its own
// texel snap) and texture matrix from it, so every consumer (cascade draws, mask, RT shadows, owned cascades) sees one consistent
// fit. The caster-cull beam (+0x100: the slice frustum, swept along the light by the beam tree) does not depend on the box and stays
// valid: our box lies inside the engine's cube.
constexpr std::uint32_t kUpdateCameraRva = 0x5a13e0, kCascadeVtableRva = 0x9a29e0;
constexpr std::size_t kSplitStride = 0x168;

using UpdateCameraFn = bool (*)(void* self, int face);
Hook<UpdateCameraFn> g_updateCamera;
const std::uint8_t* g_engineBase = nullptr;

using Mat = std::array<double, 16>;  // row-major, row vectors (D3D / bCMatrix)

bool invert(const float* m, Mat& out) {
    double a[16];
    for (int i = 0; i < 16; ++i) a[i] = m[i];
    double inv[16];
    inv[0] = a[5] * a[10] * a[15] - a[5] * a[11] * a[14] - a[9] * a[6] * a[15] + a[9] * a[7] * a[14] + a[13] * a[6] * a[11] - a[13] * a[7] * a[10];
    inv[4] = -a[4] * a[10] * a[15] + a[4] * a[11] * a[14] + a[8] * a[6] * a[15] - a[8] * a[7] * a[14] - a[12] * a[6] * a[11] + a[12] * a[7] * a[10];
    inv[8] = a[4] * a[9] * a[15] - a[4] * a[11] * a[13] - a[8] * a[5] * a[15] + a[8] * a[7] * a[13] + a[12] * a[5] * a[11] - a[12] * a[7] * a[9];
    inv[12] = -a[4] * a[9] * a[14] + a[4] * a[10] * a[13] + a[8] * a[5] * a[14] - a[8] * a[6] * a[13] - a[12] * a[5] * a[10] + a[12] * a[6] * a[9];
    inv[1] = -a[1] * a[10] * a[15] + a[1] * a[11] * a[14] + a[9] * a[2] * a[15] - a[9] * a[3] * a[14] - a[13] * a[2] * a[11] + a[13] * a[3] * a[10];
    inv[5] = a[0] * a[10] * a[15] - a[0] * a[11] * a[14] - a[8] * a[2] * a[15] + a[8] * a[3] * a[14] + a[12] * a[2] * a[11] - a[12] * a[3] * a[10];
    inv[9] = -a[0] * a[9] * a[15] + a[0] * a[11] * a[13] + a[8] * a[1] * a[15] - a[8] * a[3] * a[13] - a[12] * a[1] * a[11] + a[12] * a[3] * a[9];
    inv[13] = a[0] * a[9] * a[14] - a[0] * a[10] * a[13] - a[8] * a[1] * a[14] + a[8] * a[2] * a[13] + a[12] * a[1] * a[10] - a[12] * a[2] * a[9];
    inv[2] = a[1] * a[6] * a[15] - a[1] * a[7] * a[14] - a[5] * a[2] * a[15] + a[5] * a[3] * a[14] + a[13] * a[2] * a[7] - a[13] * a[3] * a[6];
    inv[6] = -a[0] * a[6] * a[15] + a[0] * a[7] * a[14] + a[4] * a[2] * a[15] - a[4] * a[3] * a[14] - a[12] * a[2] * a[7] + a[12] * a[3] * a[6];
    inv[10] = a[0] * a[5] * a[15] - a[0] * a[7] * a[13] - a[4] * a[1] * a[15] + a[4] * a[3] * a[13] + a[12] * a[1] * a[7] - a[12] * a[3] * a[5];
    inv[14] = -a[0] * a[5] * a[14] + a[0] * a[6] * a[13] + a[4] * a[1] * a[14] - a[4] * a[2] * a[13] - a[12] * a[1] * a[6] + a[12] * a[2] * a[5];
    inv[3] = -a[1] * a[6] * a[11] + a[1] * a[7] * a[10] + a[5] * a[2] * a[11] - a[5] * a[3] * a[10] - a[9] * a[2] * a[7] + a[9] * a[3] * a[6];
    inv[7] = a[0] * a[6] * a[11] - a[0] * a[7] * a[10] - a[4] * a[2] * a[11] + a[4] * a[3] * a[10] + a[8] * a[2] * a[7] - a[8] * a[3] * a[6];
    inv[11] = -a[0] * a[5] * a[11] + a[0] * a[7] * a[9] + a[4] * a[1] * a[11] - a[4] * a[3] * a[9] - a[8] * a[1] * a[7] + a[8] * a[3] * a[5];
    inv[15] = a[0] * a[5] * a[10] - a[0] * a[6] * a[9] - a[4] * a[1] * a[10] + a[4] * a[2] * a[9] + a[8] * a[1] * a[6] - a[8] * a[2] * a[5];
    const double det = a[0] * inv[0] + a[1] * inv[4] + a[2] * inv[8] + a[3] * inv[12];
    if (!(std::fabs(det) > 1e-30)) return false;
    for (std::size_t i = 0; i < 16; ++i) out[i] = inv[i] / det;
    return true;
}

void transformPoint(const Mat& m, const double p[3], double out[3]) {
    for (std::size_t c = 0; c < 3; ++c) out[c] = p[0] * m[c] + p[1] * m[4 + c] + p[2] * m[8 + c] + m[12 + c];
}

bool detourUpdateCamera(void* self, int face) {
    const int mode = g_fit.load(std::memory_order_relaxed);
    void* cascade = const_cast<void*>(g_cascadeObject.load(std::memory_order_relaxed));
    bool done = false;
    if (mode > 0 && self != nullptr && cascade != nullptr && g_engineBase != nullptr && sym().appInstancePtr != nullptr) {
        std::uintptr_t vt = 0;
        std::memcpy(&vt, self, sizeof vt);
        const auto* splits = field<std::uint8_t*>(cascade, 0x30);
        const int count = field<int>(cascade, kSplitCount);
        auto* me = static_cast<std::uint8_t*>(self);
        const std::ptrdiff_t off = splits != nullptr ? me - splits : -1;
        const std::ptrdiff_t stride = static_cast<std::ptrdiff_t>(kSplitStride);
        if (vt == reinterpret_cast<std::uintptr_t>(g_engineBase + kCascadeVtableRva) && off >= 0 && off % stride == 0 && off / stride < count && count <= 4) {
            const int idx = static_cast<int>(off / stride);
            const float n = field<float>(self, 0x160);
            const float f = idx + 1 < count ? field<float>(const_cast<std::uint8_t*>(splits) + (idx + 1) * stride, 0x160) : field<float>(cascade, kMaxDistance);
            const std::uint8_t* appObj = nullptr;
            std::memcpy(&appObj, sym().appInstancePtr, sizeof appObj);
            const std::uint8_t* cam = nullptr;
            if (appObj != nullptr) {
                safeRead(appObj + 0x38, &cam, sizeof cam);
                if (cam == nullptr) safeRead(appObj + 0xc8, &cam, sizeof cam);
            }
            float proj[16], view[16], light[16], box[6];
            if (cam != nullptr && n >= 0.0f && f > n && safeRead(cam + 0x50, proj, sizeof proj) && safeRead(cam + 0x10, view, sizeof view) &&
                safeRead(me + 0x60, light, sizeof light) && safeRead(me + 0xe0, box, sizeof box) && std::fabs(proj[0]) > 1e-6f && std::fabs(proj[5]) > 1e-6f) {
                const SliceSphere ss = minimalSphere(1.0f / std::fabs(proj[0]), 1.0f / std::fabs(proj[5]), n, f);
                Mat invV, invL;
                if (invert(view, invV) && invert(light, invL) && ss.radius > 0.0f) {
                    const double c[3] = {0.0, 0.0, static_cast<double>(ss.centreZ)};
                    double w[3], l[3];
                    transformPoint(invV, c, w);
                    transformPoint(invL, w, l);
                    // GD3D11: the radius rounded up to 1/16, so float noise of the projection never rescales the map
                    const float r = std::ceil(ss.radius * 16.0f) / 16.0f;
                    const float engineSide = box[3] - box[0];
                    const float nb[6] = {static_cast<float>(l[0]) - r, static_cast<float>(l[1]) - r, box[2], static_cast<float>(l[0]) + r, static_cast<float>(l[1]) + r,
                                         static_cast<float>(l[2]) + r};
                    if (std::isfinite(nb[0]) && std::isfinite(nb[5]) && 2.0f * r <= engineSide * 1.001f) {
                        std::memcpy(me + 0xe0, nb, sizeof nb);
                        done = true;
                        const int size = field<int>(self, 0xc);
                        g_lastFit[idx] = FitInfo{engineSide, 2.0f * r, size};
                        if (std::fabs(g_loggedSide[idx] - 2.0f * r) > 0.01f * r) {
                            g_loggedSide[idx] = 2.0f * r;
                            spdlog::info("enh: shadows: fit sphere: cascade {} {:.0f} .. {:.0f}: box side {:.1f} (engine {:.1f}) cm, {:.3f} cm per texel (engine {:.3f}) at {}^2, x{:.2f}",
                                         idx, n, f, 2.0f * r, engineSide, 2.0f * r / static_cast<float>(std::max(size, 1)), engineSide / static_cast<float>(std::max(size, 1)),
                                         size, engineSide / (2.0f * r));
                        }
                    }
                }
            }
        }
    }
    if (done) g_fitActive.store(true, std::memory_order_relaxed);
    return g_updateCamera.original(self, face);
}

bool detourRender(void* self) {
    if (self != nullptr) {
        g_cascadeObject.store(self, std::memory_order_relaxed);
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
    const int stride = g_everyFrame.load(std::memory_order_relaxed) ? 1 : g_stride.load(std::memory_order_relaxed);
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
        if (auto* tap = g_fitTap.load(std::memory_order_relaxed); tap != nullptr && self != nullptr) tap(self);
        const bool r = g_render.original(self);
        th = Throttle{true, 0, static_cast<std::uint32_t>(std::max(field<int>(self, kSplitCount), 0)), {}, {}, {}, range, factor, size, now};
        std::memcpy(th.axes, axes, sizeof axes);
        std::memcpy(th.pos, pos, sizeof pos);
        std::memcpy(th.sun, sun, sizeof sun);
        return r;
    }
    th.valid = false;
    if (auto* tap = g_fitTap.load(std::memory_order_relaxed); tap != nullptr && self != nullptr) tap(self);
    return g_render.original(self);
}

}  // namespace

const void* cascadeObject() { return g_cascadeObject.load(std::memory_order_relaxed); }

void setEveryFrame(bool on) { g_everyFrame.store(on, std::memory_order_relaxed); }

void setUpdateStride(int stride) { g_stride.store(stride < 1 ? 1 : stride, std::memory_order_relaxed); }

void setOverrides(const Overrides& o) {
    g_size.store(o.mapSize, std::memory_order_relaxed);
    g_range.store(o.range, std::memory_order_relaxed);
    g_factor.store(o.splitFactor, std::memory_order_relaxed);
    const int fit = std::clamp(o.fit, 0, 1);
    if (fit != g_fit.exchange(fit, std::memory_order_relaxed)) {
        if (fit == 0) g_fitActive.store(false, std::memory_order_relaxed);
        for (float& v : g_loggedSide) v = 0.0f;
        spdlog::info("enh: shadows: cascade fit {}", fit == 0 ? "engine" : "sphere");
    }
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

SliceSphere minimalSphere(float tanX, float tanY, float n, float f) {
    SliceSphere s;
    const float k = tanX * tanX + tanY * tanY;
    float zc = 0.5f * (f + n) * (1.0f + k);  // the depth where the near and far corners are equally far
    zc = std::clamp(zc, n, f);
    const float rn = std::sqrt((zc - n) * (zc - n) + n * n * k), rf = std::sqrt((f - zc) * (f - zc) + f * f * k);
    s.centreZ = zc;
    s.radius = std::max(rn, rf);
    return s;
}

bool fitOverridden() { return g_fit.load(std::memory_order_relaxed) > 0 && g_fitActive.load(std::memory_order_relaxed); }

FitInfo lastFit(int cascade) { return cascade >= 0 && cascade < 4 ? g_lastFit[cascade] : FitInfo{}; }

void setFitTap(void (*tap)(void* cascadeObject)) { g_fitTap.store(tap, std::memory_order_release); }

bool install() {
    const auto& s = sym();
    if (s.directionalLightUpdateShadowMaps != nullptr) {
        g_lightUpdate.install("eCDirectionalLight_PS::UpdateShadowMaps", reinterpret_cast<LightUpdateFn>(s.directionalLightUpdateShadowMaps),
                              &detourLightUpdate);
    }
    g_setSize = reinterpret_cast<SetIntFn>(s.cascadeSetShadowMapSize);
    g_setCount = reinterpret_cast<SetIntFn>(s.cascadeSetSplitCount);
    g_engineBase = reinterpret_cast<const std::uint8_t*>(GetModuleHandleW(L"Engine.dll"));
    if (g_engineBase != nullptr) {
        // the cascade splits' vtable slot +0x160 must be UpdateCamera: the pre-hook is only valid for this function
        std::uintptr_t slot = 0;
        std::memcpy(&slot, g_engineBase + kCascadeVtableRva + 0x160, sizeof slot);
        if (slot == reinterpret_cast<std::uintptr_t>(g_engineBase + kUpdateCameraRva)) {
            g_updateCamera.install("eCShadowMapDirectionalLight2::UpdateCamera",
                                   reinterpret_cast<UpdateCameraFn>(const_cast<std::uint8_t*>(g_engineBase) + kUpdateCameraRva), &detourUpdateCamera);
        } else {
            spdlog::warn("engine: eCShadowMapDirectionalLight2::UpdateCamera is not where it was verified: shadows.fit has no effect");
        }
    }
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
