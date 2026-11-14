#include "engine/shadow_maps.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <format>
#include <string>

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
FitInfo g_lastFit[4];  // guarded by g_lastFitMutex (written on the render thread, read by lastFit() anywhere)
std::mutex g_lastFitMutex;
float g_loggedSide[4]{};  // render thread only; setOverrides asks for a reset through g_resetLog
std::atomic<bool> g_resetLog{false};

// ---- SDSM state (render thread; the pure parts are in sdsm::, shadow_maps.h) ----
std::atomic<std::uint64_t> g_counter{0};  // cascade updates so far (RenderShadowMaps calls)
std::atomic<void (*)()> g_sdsmPoll{nullptr};
std::atomic<bool> g_sdsmReset{false};  // setOverrides (any thread) asks the render thread to start the state afresh
std::atomic<float> g_playerRange{0.0f};   // the shadow range before the visible-depth clamp (the bins' zMax)
constexpr int kSdsmCalmFrames = 90;       // an extent shrinks after this many frames below its grid value
constexpr float kSdsmMaxAge = 3.0f;       // cascade updates a reduction may be old (counter difference)
constexpr double kSdsmMaxSeconds = 0.25;

double steadySeconds() {
    return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
}
struct SdsmState {
    SdsmFrame data;
    bool have = false;
    std::uint64_t cutCounter = 0;
    sdsm::Axis ext[4][2];
    sdsm::Axis range;
    // the camera of the last cascade update (cut detection)
    bool contValid = false;
    float lastPos[3]{}, lastTan[2]{};
    double lastTime = 0.0;
    bool cut = true;          // this update follows a cut: the sphere fit
    // the camera of this update
    float pos[3]{}, axes[9]{}, tan[2]{};
    bool camValid = false;
    // statistics (5 s window)
    int lastFail[4]{-1, -1, -1, -1};  // reason of this update's fallback (-1 = sdsm used), for the debug line
    std::uint64_t fits[4]{}, fallback[4][5]{};  // reasons: 0 no data, 1 old, 2 sun / fov changed, 3 motion / cut, 4 empty slice
    std::uint64_t ageSum = 0, ageCount = 0, cuts = 0;
    double lastReport = 0.0;
    float loggedSide[4][2]{};
    std::uint64_t ageMin = ~0ull, ageMax = 0, ageHist[8]{};  // the reduction's age at its uses (cascade updates; 7 = 7 or more)
    double marginSum = 0.0;  // margin / slice far end, summed over the fits
};
SdsmState g_sdsm;

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
// shadows.fit = sdsm: the same limits, tighter (a skipped update keeps a box fitted to the camera of the last real one; the drift goes into the margin)
constexpr float kSdsmMoveLimit = 60.0f;
constexpr float kSdsmTurnLimit = 0.006f;
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

// the engine's main camera: view (+0x10) and projection (+0x50) of the app's camera object
bool readEngineCamera(float view[16], float proj[16]) {
    if (g_engineBase == nullptr || sym().appInstancePtr == nullptr) return false;
    const std::uint8_t* appObj = nullptr;
    std::memcpy(&appObj, sym().appInstancePtr, sizeof appObj);
    const std::uint8_t* cam = nullptr;
    if (appObj != nullptr) {
        safeRead(appObj + 0x38, &cam, sizeof cam);
        if (cam == nullptr) safeRead(appObj + 0xc8, &cam, sizeof cam);
    }
    return cam != nullptr && safeRead(cam + 0x50, proj, 16 * sizeof(float)) && safeRead(cam + 0x10, view, 16 * sizeof(float)) && std::fabs(proj[0]) > 1e-6f &&
           std::fabs(proj[5]) > 1e-6f;
}

// the camera's pose from its view matrix: position and axes (the inverse view's rows)
bool cameraPose(const float view[16], float pos[3], float axes[9]) {
    Mat invV;
    if (!invert(view, invV)) return false;
    for (int i = 0; i < 3; ++i) {
        pos[i] = static_cast<float>(invV[12 + i]);
        for (int j = 0; j < 3; ++j) axes[i * 3 + j] = static_cast<float>(invV[i * 4 + j]);
    }
    return true;
}

// Why an sdsm fit fell back to the sphere for a cascade (SdsmState::fallback)
enum SdsmReason { kNoData = 0, kOld = 1, kChanged = 2, kMotion = 3, kEmpty = 4 };

bool sdsmDataUsable(std::uint64_t counter, SdsmReason& why) {
    const SdsmState& g = g_sdsm;
    if (!g.have || g.cut) {
        why = g.cut ? kMotion : kNoData;
        return false;
    }
    if (static_cast<float>(counter - g.data.counter) > kSdsmMaxAge || steadySeconds() - g.data.time > kSdsmMaxSeconds) {
        why = kOld;
        return false;
    }
    return true;
}

// The sdsm box of cascade `idx` (slice n .. f): x / y from the visible pixels' bounds. nb = the sphere's box on entry (the fallback and the cap), the result on
// exit. invL = the current light view's inverse. False = keep the sphere's box (the reason counted)
bool sdsmFit(int idx, float n, float f, const Mat& invL, int size, float nb[6], float& sideY) {
    SdsmState& g = g_sdsm;
    const SdsmFrame& d = g.data;
    SdsmReason why = kNoData;
    const auto fail = [&](SdsmReason r) {
        ++g.fallback[idx][r];
        g.lastFail[idx] = r;
        return false;
    };
    g.lastFail[idx] = -1;
    if (!sdsmDataUsable(g_counter.load(std::memory_order_relaxed), why)) return fail(why);
    if (!g.camValid) return fail(kNoData);
    sdsm::FitIn in;
    in.data = &d;
    in.n = n;
    in.f = f;
    for (int k = 0; k < 9; ++k) in.lightA[k] = static_cast<float>(invL[(k / 3) * 4 + k % 3]);
    for (int k = 0; k < 3; ++k) in.lightT[k] = static_cast<float>(invL[12 + k]);
    std::memcpy(in.pos, g.pos, sizeof in.pos);
    std::memcpy(in.axes, g.axes, sizeof in.axes);
    in.tanX = g.tan[0];
    in.tanY = g.tan[1];
    in.size = size;
    in.sphereLo[0] = nb[0];
    in.sphereLo[1] = nb[1];
    in.sphereHi[0] = nb[3];
    in.sphereHi[1] = nb[4];
    // cascades are redrawn only every n-th frame while the camera drifts little (the throttle): its drift allowance goes into the margin
    const bool throttled = !g_everyFrame.load(std::memory_order_relaxed) && g_stride.load(std::memory_order_relaxed) > 1;
    in.driftCm = throttled ? kSdsmMoveLimit : 0.0f;
    in.driftRad = throttled ? kSdsmTurnLimit : 0.0f;
    sdsm::FitOut out;
    if (!sdsm::fit(in, g.ext[idx], kSdsmCalmFrames, out)) {
        return fail(out.fail == sdsm::FitFail::Empty ? kEmpty : out.fail == sdsm::FitFail::Motion ? kMotion : kChanged);
    }
    nb[0] = out.lo[0];
    nb[3] = out.hi[0];
    nb[1] = out.lo[1];
    nb[4] = out.hi[1];
    sideY = nb[4] - nb[1];
    ++g.fits[idx];
    const std::uint64_t age = g_counter.load(std::memory_order_relaxed) - d.counter;
    g.ageSum += age;
    ++g.ageCount;
    g.ageMin = std::min(g.ageMin, age);
    g.ageMax = std::max(g.ageMax, age);
    ++g.ageHist[std::min<std::uint64_t>(age, 7)];
    g.marginSum += out.margin / std::max(f, 1.0f);
    return true;
}

bool detourUpdateCamera(void* self, int face) {
    if (g_resetLog.exchange(false, std::memory_order_relaxed)) {
        for (float& v : g_loggedSide) v = 0.0f;
    }
    const int mode = g_fit.load(std::memory_order_relaxed);
    void* cascade = const_cast<void*>(g_cascadeObject.load(std::memory_order_relaxed));
    bool done = false;
    if (mode > 0 && self != nullptr && cascade != nullptr && g_engineBase != nullptr && sym().appInstancePtr != nullptr) {
        // self first: the engine calls this on a live object, but g_cascadeObject (refreshed only in RenderShadowMaps) may be gone (Shadow Quality change, world
        // change): every read of it is guarded, and only a split of the object we hold counts
        std::uintptr_t vt = 0;
        const std::uint8_t* splits = nullptr;
        int count = 0;
        auto* me = static_cast<std::uint8_t*>(self);
        const std::ptrdiff_t stride = static_cast<std::ptrdiff_t>(kSplitStride);
        std::ptrdiff_t off = -1;
        if (safeRead(self, &vt, sizeof vt) && vt == reinterpret_cast<std::uintptr_t>(g_engineBase + kCascadeVtableRva) &&
            safeRead(static_cast<const std::uint8_t*>(cascade) + 0x30, &splits, sizeof splits) && safeRead(static_cast<const std::uint8_t*>(cascade) + kSplitCount, &count, sizeof count) &&
            splits != nullptr)
            off = me - splits;
        if (off >= 0 && off % stride == 0 && off / stride < count && count <= 4) {
            const int idx = static_cast<int>(off / stride);
            float n = -1.0f, f = 0.0f;
            const bool read = safeRead(me + 0x160, &n, sizeof n) &&
                              (idx + 1 < count ? safeRead(splits + (idx + 1) * stride + 0x160, &f, sizeof f) : safeRead(static_cast<const std::uint8_t*>(cascade) + kMaxDistance, &f, sizeof f));
            if (!read) n = -1.0f;
            float proj[16], view[16], light[16], box[6];
            if (n >= 0.0f && f > n && readEngineCamera(view, proj) && safeRead(me + 0x60, light, sizeof light) && safeRead(me + 0xe0, box, sizeof box)) {
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
                    float nb[6] = {static_cast<float>(l[0]) - r, static_cast<float>(l[1]) - r, box[2], static_cast<float>(l[0]) + r, static_cast<float>(l[1]) + r,
                                   static_cast<float>(l[2]) + r};
                    if (std::isfinite(nb[0]) && std::isfinite(nb[5]) && 2.0f * r <= engineSide * 1.001f) {
                        int size = 0;
                        safeRead(me + 0xc, &size, sizeof size);
                        float sideY = 2.0f * r;
                        bool sdsmUsed = false;
                        if (mode == 2) {
                            if (idx == 0) {
                                // once per update: this frame's camera (cut detection was done in detourRender)
                                SdsmState& g = g_sdsm;
                                g.camValid = cameraPose(view, g.pos, g.axes);
                                g.tan[0] = 1.0f / std::fabs(proj[0]);
                                g.tan[1] = 1.0f / std::fabs(proj[5]);
                            }
                            sdsmUsed = sdsmFit(idx, n, f, invL, size, nb, sideY);
                            if (spdlog::should_log(spdlog::level::debug)) {
                                static const char* const kWhy[] = {"no-data", "old", "sun-fov", "motion", "empty"};
                                const int lf = g_sdsm.lastFail[idx];
                                spdlog::debug("enh: shadows: sdsm update {} c{}: {} box {:.1f} x {:.1f} cm", g_counter.load(std::memory_order_relaxed), idx,
                                              sdsmUsed ? "sdsm" : (lf >= 0 && lf < 5 ? kWhy[lf] : "sphere"), nb[3] - nb[0], nb[4] - nb[1]);
                            }
                        }
                        std::memcpy(me + 0xe0, nb, sizeof nb);
                        done = true;
                        const float sideX = nb[3] - nb[0];
                        {
                            const std::lock_guard lock(g_lastFitMutex);
                            g_lastFit[idx] = FitInfo{engineSide, sideX, size, sideY, sdsmUsed};
                        }
                        float* logged = g_sdsm.loggedSide[idx];
                        if (std::fabs(g_loggedSide[idx] - sideX) > 0.01f * r || (mode == 2 && std::fabs(logged[1] - sideY) > 0.05f * sideY)) {
                            g_loggedSide[idx] = sideX;
                            logged[1] = sideY;
                            spdlog::info("enh: shadows: fit {}: cascade {} {:.0f} .. {:.0f}: box {:.1f} x {:.1f} (sphere {:.1f}, engine {:.1f}) cm, {:.3f} x {:.3f} cm per texel (engine {:.3f}) at {}^2, x{:.2f}",
                                         sdsmUsed ? "sdsm" : "sphere", idx, n, f, sideX, sideY, 2.0f * r, engineSide, sideX / static_cast<float>(std::max(size, 1)),
                                         sideY / static_cast<float>(std::max(size, 1)), engineSide / static_cast<float>(std::max(size, 1)), size, engineSide / std::sqrt(sideX * sideY));
                        }
                    }
                }
            }
        }
    }
    if (done) g_fitActive.store(true, std::memory_order_relaxed);
    return g_updateCamera.original(self, face);
}

// once per cascade update while the fit is sdsm (top of detourRender): collects the read-backs that completed, detects a camera cut (no usable reduction until one
// that ran after it arrives) and reports every 5 s
void sdsmBeginUpdate() {
    if (g_sdsmReset.exchange(false, std::memory_order_relaxed)) g_sdsm = SdsmState{};
    SdsmState& g = g_sdsm;
    const std::uint64_t counter = g_counter.load(std::memory_order_relaxed);
    if (auto* poll = g_sdsmPoll.load(std::memory_order_acquire); poll != nullptr) poll();
    float view[16], proj[16], pos[3], axes[9];
    const double now = steadySeconds();
    bool cut = true;
    if (readEngineCamera(view, proj) && cameraPose(view, pos, axes)) {
        const float tx = 1.0f / std::fabs(proj[0]), ty = 1.0f / std::fabs(proj[5]);
        if (g.contValid && now - g.lastTime <= 0.3) {
            const float dx = pos[0] - g.lastPos[0], dy = pos[1] - g.lastPos[1], dz = pos[2] - g.lastPos[2];
            cut = dx * dx + dy * dy + dz * dz > 1000.0f * 1000.0f || std::fabs(tx - g.lastTan[0]) > 0.02f * tx || std::fabs(ty - g.lastTan[1]) > 0.02f * ty;
        }
        std::memcpy(g.lastPos, pos, sizeof pos);
        g.lastTan[0] = tx;
        g.lastTan[1] = ty;
        g.contValid = true;
    } else {
        g.contValid = false;
    }
    g.lastTime = now;
    g.cut = cut;
    if (cut) {
        ++g.cuts;
        g.have = false;
        g.cutCounter = counter;
        for (auto& a : g.ext) a[0] = a[1] = sdsm::Axis{};
        g.range = sdsm::Axis{};
    }
    if (now - g.lastReport >= 5.0) {
        if (g.lastReport > 0.0) {
            std::string line;
            for (int i = 0; i < 4; ++i) {
                if (g.fits[i] == 0 && g.fallback[i][0] + g.fallback[i][1] + g.fallback[i][2] + g.fallback[i][3] + g.fallback[i][4] == 0) continue;
                line += std::format("{}c{}: {} sdsm / fallback none {} old {} sun-fov {} motion {} empty {}", line.empty() ? "" : "; ", i, g.fits[i], g.fallback[i][0], g.fallback[i][1],
                                    g.fallback[i][2], g.fallback[i][3], g.fallback[i][4]);
            }
            // the reduction's age in cascade updates (frames; min / median / max, histogram 0..6, 7+) and the margin as a share of the slice's far end
            std::uint64_t half = (g.ageCount + 1) / 2, acc = 0;
            int median = 0;
            for (int k = 0; k < 8; ++k) {
                acc += g.ageHist[k];
                if (acc >= half) {
                    median = k;
                    break;
                }
            }
            spdlog::info("enh: shadows: sdsm (5 s): {}; reduction age {:.2f} updates (min {} median {} max {}; hist {} {} {} {} {} {} {} {}+), margin {:.1f} % of the slice, cuts {}, range {:.0f}", line,
                         g.ageCount > 0 ? static_cast<double>(g.ageSum) / static_cast<double>(g.ageCount) : 0.0, g.ageCount > 0 ? g.ageMin : 0, median, g.ageMax, g.ageHist[0], g.ageHist[1],
                         g.ageHist[2], g.ageHist[3], g.ageHist[4], g.ageHist[5], g.ageHist[6], g.ageHist[7], g.ageCount > 0 ? 100.0 * g.marginSum / static_cast<double>(g.ageCount) : 0.0, g.cuts,
                         g.range.cur);
        }
        g.lastReport = now;
        std::memset(g.fits, 0, sizeof g.fits);
        std::memset(g.fallback, 0, sizeof g.fallback);
        g.ageSum = g.ageCount = g.cuts = 0;
        g.ageMin = ~0ull;
        g.ageMax = 0;
        std::memset(g.ageHist, 0, sizeof g.ageHist);
        g.marginSum = 0.0;
    }
}

// The shadow range the cascades are split over: the player's, or (sdsm, a usable reduction) the visible depth with room (the last cascade fades out over
// its last third, so everything visible must lie before that: x1.6), quantised with hysteresis. Smaller where little is visible (interiors): the cascades
// split a shorter range, so every one of them gets finer.
float sdsmRange(float playerRange) {
    SdsmState& g = g_sdsm;
    SdsmReason why;
    if (!sdsmDataUsable(g_counter.load(std::memory_order_relaxed), why)) {
        g.range = sdsm::Axis{};
        return playerRange;
    }
    float hi = 0.0f;
    for (int i = SdsmFrame::kBins - 1; i >= 0 && hi == 0.0f; --i)
        if (g.data.bounds[i][0] <= g.data.bounds[i][1]) hi = sdsm::binEdge(g.data, i + 1);
    if (hi <= 0.0f) {
        g.range = sdsm::Axis{};
        return playerRange;
    }
    const float floorRange = std::min(playerRange, 1500.0f);
    const float need = std::clamp(hi * 1.6f, floorRange, playerRange);
    return std::min(playerRange, sdsm::step(g.range, need, kSdsmCalmFrames));
}

bool detourRender(void* self) {
    g_counter.fetch_add(1, std::memory_order_relaxed);
    const int fitMode = g_fit.load(std::memory_order_relaxed);
    if (fitMode == 2 && self != nullptr) sdsmBeginUpdate();
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
        float newRange = wantRange > 0.0f ? wantRange : t.gameRange;
        g_playerRange.store(newRange, std::memory_order_relaxed);
        if (fitMode == 2) newRange = sdsmRange(newRange);
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
            const float moveLimit = g_fit.load(std::memory_order_relaxed) == 2 ? kSdsmMoveLimit : kMoveLimit;
            const float turnLimit = g_fit.load(std::memory_order_relaxed) == 2 ? kSdsmTurnLimit : kTurnLimit;
            redraw = dx * dx + dy * dy + dz * dz > moveLimit * moveLimit;
            for (int i = 0; i < 12 && !redraw; ++i) redraw = std::fabs(axes[i] - th.axes[i]) > turnLimit;
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
    const int fit = std::clamp(o.fit, 0, 2);
    if (fit != g_fit.exchange(fit, std::memory_order_relaxed)) {
        if (fit == 0) g_fitActive.store(false, std::memory_order_relaxed);
        g_resetLog.store(true, std::memory_order_relaxed);
        g_sdsmReset.store(true, std::memory_order_relaxed);
        spdlog::info("enh: shadows: cascade fit {}", fit == 0 ? "engine" : fit == 1 ? "sphere" : "sdsm");
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

FitInfo lastFit(int cascade) {
    const std::lock_guard lock(g_lastFitMutex);
    return cascade >= 0 && cascade < 4 ? g_lastFit[cascade] : FitInfo{};
}

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

void submitSdsm(const SdsmFrame& frame) {
    SdsmState& g = g_sdsm;
    if (frame.counter < g.cutCounter || (g.have && frame.counter <= g.data.counter)) return;
    g.data = frame;
    g.have = true;
}

void setSdsmPoll(void (*poll)()) { g_sdsmPoll.store(poll, std::memory_order_release); }

bool sdsmWanted() { return g_fit.load(std::memory_order_relaxed) == 2; }

bool sdsmSetup(SdsmSetup& out) {
    void* cascade = const_cast<void*>(g_cascadeObject.load(std::memory_order_relaxed));
    if (cascade == nullptr) return false;
    // (the object may be gone: refreshed only by RenderShadowMaps)
    const std::uint8_t* splits = nullptr;
    int count = 0;
    if (!safeRead(static_cast<const std::uint8_t*>(cascade) + 0x30, &splits, sizeof splits) || !safeRead(static_cast<const std::uint8_t*>(cascade) + kSplitCount, &count, sizeof count) ||
        splits == nullptr || count < 1)
        return false;
    float light[16];
    Mat invL;
    if (!safeRead(splits + 0x60, light, sizeof light) || !invert(light, invL)) return false;
    for (int i = 0; i < 3; ++i) {
        out.lightT[i] = static_cast<float>(invL[12 + i]);
        for (int j = 0; j < 3; ++j) out.lightA[i * 3 + j] = static_cast<float>(invL[i * 4 + j]);
    }
    out.range = g_playerRange.load(std::memory_order_relaxed);
    out.counter = g_counter.load(std::memory_order_relaxed);
    return out.range > 0.0f;
}

namespace sdsm {

float binEdge(const SdsmFrame& f, int i) {
    return f.zMin * std::exp(static_cast<float>(i) * std::log(f.zMax / f.zMin) / static_cast<float>(SdsmFrame::kBins));
}

Bounds merge(const SdsmFrame& frame, float n, float f) {
    Bounds out;
    if (!(f > n) || !(frame.zMax > frame.zMin) || f < frame.zMin) return out;
    const float scale = static_cast<float>(SdsmFrame::kBins) / std::log(frame.zMax / frame.zMin);
    const int lo = n <= frame.zMin ? 0 : std::clamp(static_cast<int>(std::floor(std::log(n / frame.zMin) * scale)), 0, SdsmFrame::kBins - 1);
    const int hi = f >= frame.zMax ? SdsmFrame::kBins - 1 : std::clamp(static_cast<int>(std::floor(std::log(f / frame.zMin) * scale)), 0, SdsmFrame::kBins - 1);
    for (int i = lo; i <= hi; ++i) {
        const float* b = frame.bounds[i];
        if (!(b[0] <= b[1]) || !(b[2] <= b[3])) continue;
        if (!out.any) {
            out.minX = b[0];
            out.maxX = b[1];
            out.minY = b[2];
            out.maxY = b[3];
            out.zLo = binEdge(frame, i);
            out.any = true;
        } else {
            out.minX = std::min(out.minX, b[0]);
            out.maxX = std::max(out.maxX, b[1]);
            out.minY = std::min(out.minY, b[2]);
            out.maxY = std::max(out.maxY, b[3]);
        }
        out.zHi = binEdge(frame, i + 1);
    }
    return out;
}

float quantUp(float v) {
    if (!(v > 1e-6f)) return 1e-6f;
    const float k = std::ceil(std::log2(v) * 16.0f - 1e-3f);
    return std::exp2(k / 16.0f);
}

float step(Axis& a, float need, int calmFrames) {
    need = std::max(need, 1e-3f);
    if (!(a.cur > 0.0f) || need > a.cur) {
        a.cur = quantUp(need);
        a.peak = 0.0f;  // the window below starts after the growth
        a.calm = 0;
        return a.cur;
    }
    a.peak = std::max(a.peak, need);
    if (++a.calm >= calmFrames) {
        const float target = quantUp(a.peak);
        if (target < a.cur) a.cur = target;
        a.peak = 0.0f;
        a.calm = 0;
    }
    return a.cur;
}

float snap(float c, float texel) { return texel > 0.0f ? std::floor(c / texel + 0.5f) * texel : c; }

float margin(float translation, float angle, float farZ, float fixedCm, float relative) { return translation + angle * farZ + fixedCm + relative * farZ; }

float axesAngle(const float a[9], const float b[9]) {
    float worst = 0.0f;
    for (int i = 0; i < 3; ++i) {
        const float d = a[3 * i] * b[3 * i] + a[3 * i + 1] * b[3 * i + 1] + a[3 * i + 2] * b[3 * i + 2];
        worst = std::max(worst, std::acos(std::clamp(d, -1.0f, 1.0f)));
    }
    return worst;
}

bool fit(const FitIn& in, Axis ext[2], int calmFrames, FitOut& out) {
    const SdsmFrame& d = *in.data;
    const auto fail = [&](FitFail r) {
        out.fail = r;
        return false;
    };
    out = FitOut{};
    for (int k = 0; k < 9; ++k)
        if (std::fabs(d.lightA[k] - in.lightA[k]) > 2e-4f) return fail(FitFail::Sun);  // the sun turned since the reduction
    if (std::fabs(d.tanX - in.tanX) > 0.01f * in.tanX || std::fabs(d.tanY - in.tanY) > 0.01f * in.tanY) return fail(FitFail::Fov);
    const float dx = in.pos[0] - d.pos[0], dy = in.pos[1] - d.pos[1], dz = in.pos[2] - d.pos[2];
    out.dp = std::sqrt(dx * dx + dy * dy + dz * dz);
    out.angle = axesAngle(d.axes, in.axes);
    if (out.dp > 400.0f || out.angle > 0.35f) return fail(FitFail::Motion);
    const Bounds b = merge(d, in.n, in.f);
    if (!b.any) return fail(FitFail::Empty);
    // the light space of the reduction -> this frame's (the origin moved with the camera)
    const float shift[2] = {in.lightT[0] - d.lightT[0], in.lightT[1] - d.lightT[1]};
    out.margin = margin(out.dp, out.angle, in.f, 30.0f + in.driftCm + in.driftRad * in.f, 0.01f);
    const float lo[2] = {b.minX + shift[0] - out.margin, b.minY + shift[1] - out.margin};
    const float hi[2] = {b.maxX + shift[0] + out.margin, b.maxY + shift[1] + out.margin};
    const float need[2] = {hi[0] - lo[0], hi[1] - lo[1]};
    if (!(need[0] > 0.0f && need[1] > 0.0f) || !std::isfinite(need[0] + need[1] + lo[0] + lo[1])) return fail(FitFail::Empty);
    // texels stay within 2:1 (the mask's filter is sized from the x axis)
    const float aspect[2] = {std::max(need[0], kMinAspect * need[1]), std::max(need[1], kMinAspect * need[0])};
    const float slack = 1.0f + 3.0f / static_cast<float>(std::max(in.size, 1));  // the engine's own snap and ours move the box by up to 1.5 texels
    for (int a = 0; a < 2; ++a) {
        out.lo[a] = in.sphereLo[a];
        out.hi[a] = in.sphereHi[a];
        const float sphereSide = in.sphereHi[a] - in.sphereLo[a];
        const float e = aspect[a] * slack;
        const float extent = step(ext[a], std::min(e, sphereSide), calmFrames);
        if (extent >= sphereSide || e >= sphereSide) continue;  // no gain on this axis: the sphere's
        const float texel = extent / static_cast<float>(std::max(in.size, 1));
        const float c = snap(0.5f * (lo[a] + hi[a]), texel);
        out.lo[a] = c - 0.5f * extent;
        out.hi[a] = c + 0.5f * extent;
    }
    return true;
}

}  // namespace sdsm

float splitStart(float range, float factor, int i, int n) {
    if (n <= 0) return 1.0f;
    const float t = static_cast<float>(i) / static_cast<float>(n);
    const float lin = (range - 1.0f) * t + 1.0f;
    const float lg = std::pow(range, t);
    return (lin - lg) * factor + lg;
}

}  // namespace rvk::engine::shadowmaps
