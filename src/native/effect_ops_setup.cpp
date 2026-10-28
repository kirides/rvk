// N5.5c, group 2048 of native.scene_ops_groups: the shadow views' setup (docs/research/engine-frame/README.md "13. N5.5c result").
// For every eCShadowMapBase::RenderShadowMap (0x5a1070) of a sun cascade or the terrain variance map:
//  * the cascade fit of eCCascadeShadowMap::RenderShadowMaps (0x59f200: split starts, light view, light box, slice frustum)
//    re-done with the very SharedBase functions the engine calls, from the application camera and the cascade object, before
//    the engine's own (compared with the split's fields at its RenderShadowMap);
//  * the shadow camera of eCShadowMapDirectionalLight2::UpdateCamera (0x5a13e0: view, orthographic projection with the texel
//    snap, clip depth, the texture matrix +0xa0) from that fit (the terrain map: from its stored light view / box), compared
//    at the map's ExecuteShadowMap;
//  * the device calls of eCDepthMap (SetRenderTarget 0x59fee0: the working colour buffer + the map as depth, or the map +
//    its depth buffer; SetViewport (0, 0, size, size, +0x50, +0x54); ClearViewPort 0x59fe50; ResetRenderTarget 0x5a00e0) compared
//    with the HAL calls and, with take-over, made with the composed values instead of the engine's.
// The fit and the camera stay engine-internal inputs of the culling (compared, not replaced).

#include "native/effect_ops.h"

#include <windows.h>

#include <algorithm>
#include <cstring>
#include <format>

#include <spdlog/spdlog.h>

#include "engine/memread.h"
#include "engine/shadow_maps.h"
#include "engine/symbols.h"
#include "hal/vulkan/hal_vulkan.h"

namespace rvk::native {

namespace {

using U8 = const std::uint8_t;
using engine::readAt;
using engine::safeRead;

template <class T>
inline T get(const void* p, std::size_t off, T fallback = T{}) {
    T v = fallback;
    if (!readAt(p, off, v)) return fallback;
    return v;
}
inline U8* ptr(const void* p, std::size_t off) { return get<U8*>(p, off, nullptr); }

constexpr std::uint32_t kCascadeVtable = 0x9a29e0;  // eCShadowMapDirectionalLight2Ex (README.md 12)
constexpr std::uint32_t kTerrainVtable = 0x9a2848;  // eCShadowMapDirectionalLight2VS
constexpr std::uint32_t kPackedVtable = 0x976738;   // eCPackedShadowMap (the point-light atlas, README.md 16)
constexpr std::size_t kCellStride = 0x120;          // eCPackedShadowMap::eSShadowMapCell (AddCell 0x5a0400)
constexpr std::uint32_t kPowf = 0x4ac030, kFmodf = 0x4ad148;  // the engine's CRT powf / fmodf (static CRT)
constexpr std::uint32_t kRenderShadowMaps = 0x59f200, kUpdateCamera = 0x5a13e0;
constexpr std::size_t kSplitStride = 0x168;

EffectOps* g_setupOps = nullptr;
void fitTrampoline(void* cascade) {
    if (g_setupOps != nullptr) g_setupOps->fitBegin(cascade);
}

// a rel32 call to `target` within [fn, fn + len)
bool callsTarget(U8* engine, std::uint32_t fn, std::uint32_t len, std::uint32_t target) {
    std::vector<std::uint8_t> b(len);
    if (!safeRead(engine + fn, b.data(), len)) return false;
    for (std::uint32_t i = 0; i + 5 <= len; ++i) {
        if (b[i] != 0xe8) continue;
        std::int32_t rel = 0;
        std::memcpy(&rel, &b[i + 1], 4);
        if (static_cast<std::int64_t>(fn) + i + 5 + rel == target) return true;
    }
    return false;
}

bool same(const void* a, const void* b, std::size_t n) { return std::memcmp(a, b, n) == 0; }

}  // namespace

const char* EffectOps::name(SetupField f) {
    static constexpr const char* kNames[] = {"fit", "camera", "texture matrix", "target", "viewport", "clear", "restore", "calls"};
    static_assert(std::size(kNames) == static_cast<std::size_t>(SetupField::Count));
    return kNames[static_cast<std::size_t>(f)];
}

void EffectOps::SetupStats::add(const SetupStats& o) {
    units += o.units;
    composed += o.composed;
    calls += o.calls;
    cleanCalls += o.cleanCalls;
    cameras += o.cameras;
    cleanCameras += o.cleanCameras;
    fits += o.fits;
    cleanFits += o.cleanFits;
    taken += o.taken;
    skipped += o.skipped;
    for (std::size_t i = 0; i < mismatches.size(); ++i) mismatches[i] += o.mismatches[i];
}

bool EffectOps::verifySetup() {
    const auto& s = engine::sym();
    if (s.shadowMapBaseRender == nullptr || s.matrixLookAt == nullptr || s.vectorNormalize == nullptr || s.vectorMulMatrix == nullptr ||
        s.boxSetMinMax == nullptr || s.boxSetCentreRadius == nullptr || s.boxSetCopy == nullptr || s.frustumFromBox == nullptr ||
        s.frustumInvTransform == nullptr || s.frustumTransform == nullptr || s.frustumNormalize == nullptr || s.frustumGetVertex == nullptr ||
        s.sphereInvalidate == nullptr || s.sphereMerge == nullptr || s.matrixSetToScalingVector == nullptr) {
        spdlog::warn("native: shadow setup: SharedBase / Engine symbols missing: the setup stays the engine's");
        return false;
    }
    // the CRT powf / fmodf the fit and UpdateCamera call (Engine.dll links its CRT statically: ours may round differently)
    if (!callsTarget(engine_, kRenderShadowMaps, 0x400, kPowf) || !callsTarget(engine_, kUpdateCamera, 0x300, kFmodf)) {
        spdlog::warn("native: shadow setup: RenderShadowMaps / UpdateCamera do not call powf {:#x} / fmodf {:#x}: off", kPowf, kFmodf);
        return false;
    }
    vecMul_ = reinterpret_cast<VecMatFn>(s.vectorMulMatrix);
    vecNormalize_ = reinterpret_cast<VecFn>(s.vectorNormalize);
    lookAt_ = reinterpret_cast<LookAtFn>(s.matrixLookAt);
    boxMinMax_ = reinterpret_cast<BoxMinMaxFn>(s.boxSetMinMax);
    boxCentre_ = reinterpret_cast<BoxCentreFn>(s.boxSetCentreRadius);
    boxCopy_ = reinterpret_cast<BoxCopyFn>(s.boxSetCopy);
    frustumFromBox_ = reinterpret_cast<FrustumBoxFn>(s.frustumFromBox);
    frustumInvTransform_ = reinterpret_cast<FrustumMatFn>(s.frustumInvTransform);
    frustumTransform_ = reinterpret_cast<FrustumMatFn>(s.frustumTransform);
    frustumNormalize_ = reinterpret_cast<FrustumFn>(s.frustumNormalize);
    frustumVertex_ = reinterpret_cast<VertexFn>(s.frustumGetVertex);
    sphereInvalidate_ = reinterpret_cast<SphereFn>(s.sphereInvalidate);
    sphereMerge_ = reinterpret_cast<SphereMergeFn>(s.sphereMerge);
    matScaleVec_ = reinterpret_cast<ScaleVecFn>(s.matrixSetToScalingVector);
    powf_ = reinterpret_cast<FloatFn>(const_cast<std::uint8_t*>(engine_ + kPowf));
    fmodf_ = reinterpret_cast<FloatFn>(const_cast<std::uint8_t*>(engine_ + kFmodf));
    spdlog::info("native: shadow setup: engine checks passed (SharedBase fit functions, powf / fmodf call sites)");
    return true;
}

void EffectOps::installSetupTap() {
    const bool on = setupOn() && setupVerified_ > 0;
    g_setupOps = on ? this : nullptr;
    engine::shadowmaps::setFitTap(on ? &fitTrampoline : nullptr);
}

// eCCascadeShadowMap::RenderShadowMaps 0x59f200, re-done on our own objects (the engine's side effects: the light direction
// normalised in place, MaxDistance set to the camera's far below 1: not ours)
void EffectOps::fitBegin(void* cascadePtr) {
    if (setupVerified_ <= 0 || !setupOn()) return;
    auto* cascade = static_cast<U8*>(cascadePtr);
    for (Fit& f : fit_) f.valid = false;
    fitCascade_ = cascade;
    fitFrame_ = dev_.stats().frames;
    U8* cam = camera();
    if (cam == nullptr) return;
    alignas(16) float invView[16], proj[16];
    if (!readAt(cam, 0x10, invView) || !readAt(cam, 0x50, proj)) return;
    matInvert_(invView);
    const float mn[3] = {-1.0f, -1.0f, 0.0f}, mx[3] = {1.0f, 1.0f, 1.0f};
    alignas(16) std::uint8_t box[32]{};
    boxMinMax_(box, mn, mx);
    alignas(16) std::uint8_t frN[0x60]{};
    frustumFromBox_(frN, box);
    frustumInvTransform_(frN, proj);
    frustumNormalize_(frN);
    alignas(16) float light[16], invLight[16];
    matIdentity_(light);
    alignas(16) float dir[4]{};
    if (!readAt(cascade, 0x10, dir[0]) || !readAt(cascade, 0x14, dir[1]) || !readAt(cascade, 0x18, dir[2])) return;
    vecNormalize_(dir);
    lookAt_(light, dir);
    light[12] = get<float>(cam, 0x90);
    light[13] = get<float>(cam, 0x94);
    light[14] = get<float>(cam, 0x98);
    std::memcpy(invLight, light, 64);
    matInvert_(invLight);
    float farDist = get<float>(cascade, 0x1c);
    if (farDist < one_) farDist = get<float>(cam, 0xc0);
    const float lambda = get<float>(cascade, 0x20);
    const std::int32_t count = std::min(get<std::int32_t>(cascade, 0x38), static_cast<std::int32_t>(fit_.size()));
    for (std::int32_t i = 0; i < count; ++i) {
        const float t = static_cast<float>(i) / static_cast<float>(count);
        const float pw = powf_(farDist, t);
        fit_[static_cast<std::size_t>(i)].start = (((farDist - one_) * t + one_) - pw) * lambda + pw;
    }
    for (std::int32_t i = 0; i < count; ++i) {
        Fit& f = fit_[static_cast<std::size_t>(i)];
        alignas(16) float sphere[4]{};
        sphereInvalidate_(sphere);
        alignas(16) std::uint8_t fr[0x60];
        std::memcpy(fr, frN, sizeof(fr));
        const float nearD = -f.start;
        const float farD = i + 1 < count ? fit_[static_cast<std::size_t>(i) + 1].start : farDist;
        std::memcpy(fr + 0x0c, &nearD, 4);
        std::memcpy(fr + 0x1c, &farD, 4);
        for (int v = 0; v < 8; ++v) {
            alignas(16) float vtx[4]{};
            frustumVertex_(fr, vtx, v);
            sphereMerge_(sphere, vtx);
        }
        alignas(16) float c[4] = {sphere[1], sphere[2], sphere[3], 0.0f};
        vecMul_(c, invView);
        vecMul_(c, invLight);
        alignas(16) std::uint8_t boxA[32]{}, boxB[32]{};
        boxCentre_(boxA, c, sphere[0]);
        boxCopy_(boxB, boxA);
        const float zMin = -100000.0f;
        std::memcpy(boxB + 8, &zMin, 4);
        std::memcpy(f.light, light, 64);
        std::memcpy(f.box, boxB, 24);
        frustumTransform_(fr, invView);
        std::memcpy(f.frustum, fr, sizeof(fr));
        f.valid = true;
    }
}

void EffectOps::setupMiss(SetupField f, const std::string& what) {
    if (setupStats_ != nullptr) ++setupStats_->mismatches[static_cast<std::size_t>(f)];
    if (setupLogged_ < 24) {
        ++setupLogged_;
        spdlog::info("native: shadow setup: mismatch ({}, {}): {}", setupKind_, name(f), what);
    }
}

void EffectOps::shadowMapBegin(void* mapPtr) {
    if (setupVerified_ <= 0 || !setupOn()) return;
    auto* map = static_cast<U8*>(mapPtr);
    const auto vt = static_cast<std::uint32_t>(static_cast<U8*>(ptr(map, 0)) - engine_);
    if (vt != kCascadeVtable && vt != kTerrainVtable && vt != kPackedVtable) return;
    setupMap_ = map;
    setupKind_ = vt == kCascadeVtable ? "cascade" : vt == kTerrainVtable ? "terrain map" : "point-light atlas";
    // N5.8: eCPackedShadowMap::RenderShadowMap 0x5a08f0: one SetRenderTarget (face 6) + ClearViewPort for the whole atlas, then
    // per cell UpdateCamera 0x5a09b0 (no device call) and the scene (ExecuteShadowMap -> eCCameraBase::ExecuteGfxCamera 0x4f6ad0:
    // the viewport = the camera's Screen rect = the cell's, z 0 .. 1), then ResetRenderTarget
    packed_ = vt == kPackedVtable;
    cell_ = 0;
    cells_ = packed_ ? get<std::int32_t>(map, 0x70) : 0;
    setupStats_ = &setupWindow_[setupKind_];
    ++setupStats_->units;
    setupStep_ = 0;
    inScene_ = false;
    setupComposed_ = false;
    entryRt_ = dev_.renderTarget(0);
    entryDepth_ = dev_.depthStencil();
    const d3d9::Viewport& v = dev_.viewport();
    entryViewport_ = {v.x, v.y, v.width, v.height, v.minZ, v.maxZ};
    // eCDepthMap::ResetRenderTarget: the target saved by SetRenderTarget (GetRenderTarget) only when both were bound
    restoreTarget_ = entryRt_ != nullptr && entryDepth_ != nullptr;
    // eCDepthMap::SetRenderTarget 0x59fee0: a depth format: the working colour buffer of the size (illum admin +0x318: a hash map
    // size -> image, buckets +0x320, count +0x328, (size >> 4) % count; nodes {key +0, image +8, next +0x18}) and the map as depth; a
    // colour format: the map and its own depth buffer +0x20 or the shared one (illum +0x310 while +0x308 >= size)
    const std::int32_t format = get<std::int32_t>(map, 8), size = get<std::int32_t>(map, 0xc);
    U8* image = ptr(map, 0x18);
    setupRt_ = nullptr;
    setupDepth_ = nullptr;
    bool ok = image != nullptr && size > 0 && get<std::uint8_t>(map, 0x10) == 0;
    if (ok && fxc::isDepthFormat(format)) {
        setupDepth_ = surfaceOfImage(image);
        const std::uint32_t buckets = get<std::uint32_t>(illum_, 0x328);
        U8* table = ptr(illum_, 0x320);
        U8* node = buckets != 0 && table != nullptr ? ptr(table, 8 * ((static_cast<std::uint32_t>(size) >> 4) % buckets)) : nullptr;
        for (int guard = 0; node != nullptr && guard < 64; ++guard, node = ptr(node, 0x18)) {
            if (get<std::int32_t>(node, 0) == size) {
                setupRt_ = surfaceOfImage(ptr(node, 8));
                break;
            }
        }
    } else if (ok) {
        setupRt_ = surfaceOfImage(image);
        void* own = const_cast<std::uint8_t*>(ptr(map, 0x20));
        if (own == nullptr && get<std::int32_t>(illum_, 0x308) >= size && size <= get<std::int32_t>(illum_, 0x304)) {
            own = const_cast<std::uint8_t*>(ptr(illum_, 0x310));
        }
        setupDepth_ = static_cast<d3d9::Surface*>(own);
    }
    ok = ok && setupRt_ != nullptr && setupDepth_ != nullptr;
    setupViewport_ = {0, 0, static_cast<std::uint32_t>(size), static_cast<std::uint32_t>(size), get<float>(map, 0x50), get<float>(map, 0x54)};
    setupClearFlags_ = fxc::depthMapClearFlags(format);
    setupClearColour_ = get<std::uint32_t>(map, 0x28);
    if (!ok) {
        ++setupStats_->skipped;  // the working buffer / depth buffer is created by this very call (first use of a size)
        return;
    }
    setupComposed_ = true;
    ++setupStats_->composed;
    if (packed_) return;  // the cells' cameras: at each scene's entry (shadowSceneBegin)
    // the cascade fit of this split (RenderShadowMaps ran it right before this call)
    if (vt == kCascadeVtable && fitFrame_ == dev_.stats().frames && fitCascade_ != nullptr) {
        U8* splits = ptr(fitCascade_, 0x30);
        const std::ptrdiff_t off = splits != nullptr ? map - splits : -1;
        const std::ptrdiff_t index = off >= 0 && off % static_cast<std::ptrdiff_t>(kSplitStride) == 0 ? off / static_cast<std::ptrdiff_t>(kSplitStride) : -1;
        if (index >= 0 && index < static_cast<std::ptrdiff_t>(fit_.size()) && fit_[static_cast<std::size_t>(index)].valid) {
            const Fit& f = fit_[static_cast<std::size_t>(index)];
            ++setupStats_->fits;
            alignas(16) std::uint8_t eng[0x60];
            bool equal = true;
            float start = 0;
            readAt(map, 0x160, start);
            if (!same(&start, &f.start, 4)) {
                equal = false;
                setupMiss(SetupField::Fit, std::format("split {} start {} vs engine {}", index, f.start, start));
            }
            if (!safeRead(map + 0x60, eng, 64) || !same(eng, f.light, 64)) {
                equal = false;
                setupMiss(SetupField::Fit, std::format("split {} light view", index));
            }
            if (!safeRead(map + 0xe0, eng, 24) || !same(eng, f.box, 24)) {
                equal = false;
                const auto* b = reinterpret_cast<const float*>(eng);
                setupMiss(SetupField::Fit, std::format("split {} box ({} {} {} / {} {} {}) vs engine ({} {} {} / {} {} {})", index, f.box[0], f.box[1],
                                                       f.box[2], f.box[3], f.box[4], f.box[5], b[0], b[1], b[2], b[3], b[4], b[5]));
            }
            if (!safeRead(map + 0x100, eng, 0x60) || !same(eng, f.frustum, 0x60)) {
                equal = false;
                setupMiss(SetupField::Fit, std::format("split {} slice frustum", index));
            }
            if (equal) ++setupStats_->cleanFits;
            composeCamera(f.light, f.box, map);
            return;
        }
        setupMiss(SetupField::Fit, "no fit of this split");
    }
    // the terrain map (or a cascade without a fit): its stored light view +0x60 and box +0xe0 (UpdateShadowMaps' re-aim)
    alignas(16) float light[16], box[6];
    if (readAt(map, 0x60, light) && readAt(map, 0xe0, box)) composeCamera(light, box, map);
}

// eCShadowMapDirectionalLight2::UpdateCamera 0x5a13e0
void EffectOps::composeCamera(const float light[16], const float box[6], const std::uint8_t* map) {
    alignas(16) float v[16], p[16], t[4]{};
    std::memcpy(v, light, 64);
    matInvert_(v);
    float scale[3], tr[3];
    fxc::shadowProjection(box, scale, tr);
    alignas(16) float sc[4] = {scale[0], scale[1], scale[2], 0.0f};
    matScaleVec_(p, sc);
    p[12] = tr[0];
    p[13] = tr[1];
    p[14] = tr[2];
    t[0] = v[12];
    t[1] = v[13];
    t[2] = v[14];
    vecMul_(t, p);
    const std::int32_t size = get<std::int32_t>(map, 0xc);
    const float r = k_.packedKernel / static_cast<float>(size);  // 2 / size (DAT_1809f16f8): whole texels of the [-1, 1] range
    p[12] -= fmodf_(t[0], r);
    p[13] -= fmodf_(t[1], r);
    std::memcpy(camView_, v, 64);
    std::memcpy(camProj_, p, 64);
    camFar_ = box[5];
    alignas(16) float tex[16];
    fxc::shadowTextureBias(size, get<float>(map, 0xf8), tex);
    matMulAssign_(tex, p);
    matMulAssign_(tex, v);
    std::memcpy(texMatrix_, tex, 64);
}

void EffectOps::shadowSceneBegin(void* mapPtr) {
    if (setupMap_ == nullptr || mapPtr != setupMap_) return;
    inScene_ = true;
    if (!setupComposed_) return;
    if (setupStep_ != 3) setupMiss(SetupField::Calls, std::format("the scene began at step {}", setupStep_));
    setupStep_ = 3;
    if (packed_) {
        packedCellScene();
        return;
    }
    // the camera UpdateCamera left (the map's eCCameraBase +0x58) and its texture matrix
    U8* cam = ptr(setupMap_, 0x58);
    ++setupStats_->cameras;
    alignas(16) float m[16];
    bool equal = true;
    if (!readAt(cam, 0x10, m) || !same(m, camView_, 64)) {
        equal = false;
        setupMiss(SetupField::Camera, "view");
    }
    if (!readAt(cam, 0x50, m) || !same(m, camProj_, 64)) {
        equal = false;
        setupMiss(SetupField::Camera, std::format("projection ({} {} {} / {} {} {}) vs engine ({} {} {} / {} {} {})", camProj_[0], camProj_[5], camProj_[10],
                                                  camProj_[12], camProj_[13], camProj_[14], m[0], m[5], m[10], m[12], m[13], m[14]));
    }
    const float nearZ = get<float>(cam, 0xbc, -1.0f), farZ = get<float>(cam, 0xc0), dbFar = get<float>(cam, 0x1694);
    if (nearZ != 0.0f || !same(&farZ, &camFar_, 4) || !same(&dbFar, &camFar_, 4) || get<std::uint8_t>(cam, 0xdc, 1) != 0) {
        equal = false;
        setupMiss(SetupField::Camera, std::format("clip {} .. {} (db {}), perspective {} vs composed 0 .. {}", nearZ, farZ, dbFar, get<std::uint8_t>(cam, 0xdc), camFar_));
    }
    if (!readAt(setupMap_, 0xa0, m) || !same(m, texMatrix_, 64)) {
        equal = false;
        setupMiss(SetupField::TextureMatrix, "texture matrix +0xa0");
    }
    if (equal) ++setupStats_->cleanCameras;
}

void EffectOps::shadowSceneEnd(void* mapPtr) {
    if (setupMap_ == nullptr || mapPtr != setupMap_) return;
    inScene_ = false;
    if (packed_ && ++cell_ < cells_) {
        setupStep_ = 3;  // the next cell's scene
        return;
    }
    setupStep_ = restoreTarget_ ? 4 : 5;
}

// the point-light atlas's cell `cell_`: the camera eCPackedShadowMap::UpdateCamera 0x5a09b0 left (Perspective off, ClipDepth cell
// +0x114 / +0x118 (also db +0x1694), view cell +0x10, projection cell +0x50, Screen = the cell rect +0 x, y, +8 w, h as (x, y,
// x + w, y + h)) and the viewport ExecuteGfxCamera sets from it inside the scene
void EffectOps::packedCellScene() {
    U8* cells = ptr(setupMap_, 0x68);
    if (cell_ < 0 || cell_ >= cells_ || cells == nullptr) {
        setupMiss(SetupField::Calls, std::format("a scene for cell {} of {}", cell_, cells_));
        cellViewportValid_ = false;
        return;
    }
    U8* cell = cells + kCellStride * static_cast<std::size_t>(cell_);
    U8* cam = ptr(setupMap_, 0x58);
    ++setupStats_->cameras;
    alignas(16) float want[16], m[16];
    bool equal = true;
    if (!readAt(cell, 0x10, want) || !readAt(cam, 0x10, m) || !same(m, want, 64)) {
        equal = false;
        setupMiss(SetupField::Camera, std::format("cell {} view", cell_));
    }
    if (!readAt(cell, 0x50, want) || !readAt(cam, 0x50, m) || !same(m, want, 64)) {
        equal = false;
        setupMiss(SetupField::Camera, std::format("cell {} projection", cell_));
    }
    const float nearZ = get<float>(cell, 0x114), farZ = get<float>(cell, 0x118);
    const float camNear = get<float>(cam, 0xbc, -1.0f), camFar = get<float>(cam, 0xc0), dbFar = get<float>(cam, 0x1694);
    if (!same(&nearZ, &camNear, 4) || !same(&farZ, &camFar, 4) || !same(&farZ, &dbFar, 4) ||
        get<std::uint8_t>(cam, 0xdc, 1) != 0) {
        equal = false;
        setupMiss(SetupField::Camera, std::format("cell {} clip {} .. {} vs engine {} .. {} (db {}), perspective {}", cell_, nearZ, farZ, get<float>(cam, 0xbc),
                                                  get<float>(cam, 0xc0), get<float>(cam, 0x1694), get<std::uint8_t>(cam, 0xdc)));
    }
    const std::int32_t r[4] = {get<std::int32_t>(cell, 0), get<std::int32_t>(cell, 4), get<std::int32_t>(cell, 8), get<std::int32_t>(cell, 0xc)};
    const std::int32_t screen[4] = {r[0], r[1], r[0] + r[2], r[1] + r[3]};
    std::int32_t engScreen[4]{};
    if (!readAt(cam, 0xc4, engScreen) || std::memcmp(engScreen, screen, sizeof(screen)) != 0) {
        equal = false;
        setupMiss(SetupField::Camera, std::format("cell {} screen ({} {} {} {}) vs engine ({} {} {} {})", cell_, screen[0], screen[1], screen[2], screen[3],
                                                  engScreen[0], engScreen[1], engScreen[2], engScreen[3]));
    }
    if (equal) ++setupStats_->cleanCameras;
    cellViewport_ = {static_cast<std::uint32_t>(r[0]), static_cast<std::uint32_t>(r[1]), static_cast<std::uint32_t>(r[2]), static_cast<std::uint32_t>(r[3]),
                     0.0f, one_};
    cellViewportValid_ = true;
}

void EffectOps::shadowMapEnd(void* mapPtr) {
    if (setupMap_ == nullptr || mapPtr != setupMap_) return;
    if (setupComposed_ && setupStep_ != 6) setupMiss(SetupField::Calls, std::format("the render ended at step {}", setupStep_));
    setupMap_ = nullptr;
    setupStats_ = nullptr;
}

void EffectOps::setupTarget(void*& rt, void*& depth) {
    if (setupMap_ == nullptr || inScene_ || !setupComposed_) return;
    ++setupStats_->calls;
    if (setupStep_ == 0 || setupStep_ == 4) {
        const bool restore = setupStep_ == 4;
        void* wantRt = restore ? entryRt_ : setupRt_;
        void* wantDepth = restore ? entryDepth_ : setupDepth_;
        if (rt == wantRt && depth == wantDepth) ++setupStats_->cleanCalls;
        else setupMiss(restore ? SetupField::Restore : SetupField::Target, std::format("target {} / depth {} vs engine {} / {}", wantRt, wantDepth, rt, depth));
        if (setupTake_) {
            rt = wantRt;
            depth = wantDepth;
            ++setupStats_->taken;
        }
        setupStep_ = restore ? 5 : 1;
        return;
    }
    setupMiss(SetupField::Calls, std::format("a target bind at step {}", setupStep_));
}

void EffectOps::setupViewport(fxc::Viewport& vp) {
    if (setupMap_ != nullptr && inScene_ && packed_ && setupComposed_ && cellViewportValid_) {
        // the cell's viewport (ExecuteGfxCamera at ExecuteShadowMap's start: once per cell)
        cellViewportValid_ = false;
        ++setupStats_->calls;
        if (vp == cellViewport_) ++setupStats_->cleanCalls;
        else setupMiss(SetupField::Viewport, std::format("cell {} viewport {} {} {}x{} z {}..{} vs engine {} {} {}x{} z {}..{}", cell_, cellViewport_.x,
                                                         cellViewport_.y, cellViewport_.width, cellViewport_.height, cellViewport_.minZ, cellViewport_.maxZ,
                                                         vp.x, vp.y, vp.width, vp.height, vp.minZ, vp.maxZ));
        if (setupTake_) {
            vp = cellViewport_;
            ++setupStats_->taken;
        }
        return;
    }
    if (setupMap_ == nullptr || inScene_ || !setupComposed_) return;
    ++setupStats_->calls;
    if (setupStep_ == 1 || setupStep_ == 5) {
        const bool restore = setupStep_ == 5;
        const fxc::Viewport want = restore ? entryViewport_ : setupViewport_;
        if (vp == want) ++setupStats_->cleanCalls;
        else setupMiss(restore ? SetupField::Restore : SetupField::Viewport, std::format("viewport {} {} {}x{} z {}..{} vs engine {} {} {}x{} z {}..{}", want.x, want.y,
                                                                             want.width, want.height, want.minZ, want.maxZ, vp.x, vp.y, vp.width, vp.height, vp.minZ, vp.maxZ));
        if (setupTake_) {
            vp = want;
            ++setupStats_->taken;
        }
        setupStep_ = restore ? 6 : 2;
        return;
    }
    setupMiss(SetupField::Calls, std::format("a viewport at step {}", setupStep_));
}

void EffectOps::setupClear(std::uint32_t& flags, std::uint32_t& colour, float& z, std::uint32_t& stencil) {
    if (setupMap_ == nullptr || inScene_ || !setupComposed_) return;
    ++setupStats_->calls;
    if (setupStep_ == 2) {
        if (flags == setupClearFlags_ && colour == setupClearColour_ && z == one_ && stencil == 0) ++setupStats_->cleanCalls;
        else setupMiss(SetupField::Clear, std::format("clear {:#x} {:08x} {} {} vs engine {:#x} {:08x} {} {}", setupClearFlags_, setupClearColour_, one_, 0, flags, colour, z, stencil));
        if (setupTake_) {
            flags = setupClearFlags_;
            colour = setupClearColour_;
            z = one_;
            stencil = 0;
            ++setupStats_->taken;
        }
        setupStep_ = 3;
        return;
    }
    setupMiss(SetupField::Calls, std::format("a clear at step {}", setupStep_));
}

void EffectOps::reportSetup() {
    if (!setupOn()) return;
    if (setupVerified_ < 0) {
        spdlog::info("native: shadow setup: idle (the engine checks failed)");
        return;
    }
    for (auto& [kind, w] : setupWindow_) {
        SetupStats& t = setupTotal_[kind];
        t.add(w);
        const auto line = [](const SetupStats& a) {
            std::string m;
            for (std::size_t i = 0; i < a.mismatches.size(); ++i) {
                if (a.mismatches[i] != 0) m += std::format("{}{} {}", m.empty() ? "" : ", ", name(static_cast<SetupField>(i)), a.mismatches[i]);
            }
            return std::format("{} renders ({} composed, {} with a buffer not created yet), fits {} / {} equal, cameras {} / {} equal, device calls {} / {} equal{}{}",
                               a.units, a.composed, a.skipped, a.cleanFits, a.fits, a.cleanCameras, a.cameras, a.cleanCalls, a.calls,
                               a.taken != 0 ? std::format(", made by the renderer {}", a.taken) : std::string(), m.empty() ? ", 0 mismatches" : "; mismatches: " + m);
        };
        spdlog::info("native: shadow setup [{}]: 5 s {}; total {}", kind, line(w), line(t));
        w = {};
    }
}

}  // namespace rvk::native
