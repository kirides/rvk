// N5.7, group 32768 of native.scene_ops_groups: the post chain (docs/research/engine-frame/README.md "15. N5.7 result").
// Every eCImageFilterEffect job eCImageEffect::Execute runs outside the scene job (the scene effect's luminance, bloom, streaks,
// tone map and DoF filters; the overlay effect's filter) is composed by EffectOps::composeEffect (effect_ops.cpp) with the rules
// here: the semantic parameter classes' values (each class's Set, vtable +0x28, re-done from its engine source), the images of
// the graph's links (the scene filter's ColorOut / DepthOut, effect filters' outputs, textures, images and nested effects'
// public outputs) and the target (texture mode: the output image + the depth bound at Execute's entry; screen mode: what is
// bound). Render thread.

#include "native/effect_ops.h"

#include <windows.h>

#include <algorithm>
#include <cstring>
#include <format>

#include <spdlog/spdlog.h>

#include "engine/memread.h"
#include "engine/symbols.h"
#include "hal/vulkan/hal_vulkan.h"

namespace rvk::native {

namespace {

using U8 = const std::uint8_t;
using engine::readAt;

template <class T>
inline T get(const void* p, std::size_t off, T fallback = T{}) {
    T v = fallback;
    if (!readAt(p, off, v)) return fallback;
    return v;
}
inline U8* ptr(const void* p, std::size_t off) { return get<U8*>(p, off, nullptr); }

// the semantic parameter classes (eSImageEffectParam*, Engine.dll vtable RVAs) and their Set (+0x28) RVAs (README.md 15)
constexpr std::uint32_t kFrameTime = 0x9ee500, kTime = 0x9ee580, kSceneExposure = 0x9ee680, kBloomIntensity = 0x9ee400,
                        kBloomThreshold = 0x9ee780, kBloomOffset = 0x9ee700, kColorMatrix = 0x9ee600, kOverlayBlend = 0x9ee380,
                        kCamera = 0x9ee480, kSkyDome = 0x9ee300, kImage = 0x9ee900, kTexture = 0x9ee880, kImageSize = 0x9ee280;
constexpr std::pair<std::uint32_t, std::uint32_t> kSemanticSets[] = {
    {kFrameTime, 0x289aa0}, {kTime, 0x2899c0},     {kSceneExposure, 0x758ad0}, {kBloomIntensity, 0x758900}, {kBloomThreshold, 0x758880},
    {kBloomOffset, 0x758800}, {kColorMatrix, 0x758980}, {kOverlayBlend, 0x758780}, {kCamera, 0x289b10},     {kSkyDome, 0x289fc0},
    {kImage, 0x289390},     {kTexture, 0x289790},  {kImageSize, 0x28a310}};

// the target of a `call rel32` at `rva` (0 when the bytes there are not E8)
std::uint32_t callTarget(U8* engine, std::uint32_t rva) {
    std::uint8_t op = 0;
    std::int32_t rel = 0;
    if (!readAt(engine, rva, op) || op != 0xe8 || !readAt(engine, rva + 1, rel)) return 0;
    return static_cast<std::uint32_t>(static_cast<std::int64_t>(rva) + 5 + rel);
}

template <class T>
void putBytes(std::vector<std::uint8_t>& out, const T* v, std::size_t n) {
    out.resize(n * sizeof(T));
    std::memcpy(out.data(), v, out.size());
}

}  // namespace

bool EffectOps::verifyPost() {
    const auto& s = engine::sym();
    const auto at = [&](const void* p, std::uint32_t rva) { return p == engine_ + rva; };
    vtScene_ = s.imageFilterSceneVftable;
    vtImageEffect_ = s.imageFilterImageEffectVftable;
    vtImage_ = s.imageFilterImageVftable;
    if (!at(vtScene_, 0x9eec08) || !at(vtImageEffect_, 0x9c6910) || !at(vtImage_, 0x9c6560)) {
        spdlog::warn("native: post: the scene / nested effect / image filter vtables are not at the expected RVAs: the post chain stays the engine's");
        return false;
    }
    for (const auto& [vt, set] : kSemanticSets) {
        const void* f = nullptr;
        if (!readAt(engine_, vt + 0x28, f) || f != engine_ + set) {
            spdlog::warn("native: post: parameter class {:#x}: Set is not RVA {:#x}: off", vt, set);
            return false;
        }
    }
    // the module getters the Set functions call (unexported, cached FindModule lookups), the nested effect lookup of
    // eCImageFilterImageEffect::GetOutputAt 0x6661f0 (the admin's effect map +0x68 by name)
    struct Site {
        std::uint32_t at, target;
    };
    static constexpr Site kSites[] = {{0x758ae8, 0x275c30}, {0x758798, 0x95750}, {0x289fcd, 0x14f330},
                                      {0x666209, 0x95750},  {0x666216, 0x1f2410}, {0x2899d8, 0x4ceef0}};
    for (const Site& site : kSites) {
        if (callTarget(engine_, site.at) != site.target) {
            spdlog::warn("native: post: the call at {:#x} is not to {:#x}: off", site.at, site.target);
            return false;
        }
    }
    if (s.timerGetInstance != engine_ + 0x4ceef0 || s.illumGetSkydome != engine_ + 0x574690 || s.sharedEpsilon == nullptr ||
        s.vectorTransformDirection == nullptr || s.vectorMulMatrix == nullptr || s.matrixMul == nullptr || s.gfxEffectOptimize == nullptr ||
        s.gfxEffectReset == nullptr || s.gfxEffectActive == nullptr) {
        spdlog::warn("native: post: engine symbols missing: off");
        return false;
    }
    // the ColorMatrix weights and the constants of Time / FOCALRANGENEAR / SUNLIGHTSCREENVECTOR
    float w0 = 0, w1 = 0, w2 = 0;
    if (!readAt(engine_, 0x9f15f0, w0) || !readAt(engine_, 0x9f1640, w1) || !readAt(engine_, 0x9f15c8, w2) || !readAt(engine_, 0x9f17cc, thousand_) ||
        !readAt(engine_, 0x9f1854, minusOne_)) {
        return false;
    }
    lumWeights_[0] = w0;
    lumWeights_[1] = w1;
    lumWeights_[2] = w2;
    filterAdmin_ = reinterpret_cast<void* (*)()>(const_cast<std::uint8_t*>(engine_) + 0x95750);
    weatherAdmin_ = reinterpret_cast<void* (*)()>(const_cast<std::uint8_t*>(engine_) + 0x275c30);
    illumAdmin_ = reinterpret_cast<void* (*)()>(const_cast<std::uint8_t*>(engine_) + 0x14f330);
    effectMapFind_ = reinterpret_cast<void* (*)(void*, const void*)>(const_cast<std::uint8_t*>(engine_) + 0x1f2410);
    getSkydome_ = reinterpret_cast<void* (*)(void*)>(s.illumGetSkydome);
    timer_ = reinterpret_cast<void* (*)()>(s.timerGetInstance);
    epsilon_ = static_cast<const float*>(s.sharedEpsilon);
    vecTransformDirection_ = reinterpret_cast<VecMatFn2>(s.vectorTransformDirection);
    vecMulAssign_ = reinterpret_cast<VecMatFn2>(s.vectorMulMatrix);
    matMul_ = reinterpret_cast<MatMulFn>(s.matrixMul);
    spdlog::info("native: post: engine checks passed (13 parameter classes, 6 call sites, luminance weights {} {} {}, eCGfxEffect::ms_bOptimize {})", w0,
                 w1, w2, *static_cast<const bool*>(s.gfxEffectOptimize));
    return true;
}

bool EffectOps::executeSkipped(const std::uint8_t* gfxEffect) const {
    const auto& s = engine::sym();
    if (gfxEffect == nullptr || !get<bool>(s.gfxEffectOptimize, 0)) return false;
    return ptr(s.gfxEffectActive, 0) == gfxEffect && get<std::uint8_t>(gfxEffect, 0x2c) == 0 && !get<bool>(s.gfxEffectReset, 0);
}

const std::uint8_t* EffectOps::nestedEffect(const std::uint8_t* filter) const {
    void* admin = filterAdmin_();
    if (admin == nullptr) return nullptr;
    return static_cast<U8*>(effectMapFind_(static_cast<std::uint8_t*>(admin) + 0x68, filter + 0x30));
}

std::int32_t EffectOps::outputCount(const std::uint8_t* filter, int depth) const {
    const void* vt = ptr(filter, 0);
    if (vt == vtScene_) return 2;
    if (vt == vtTexture_ || vt == vtTextureRandom_ || vt == vtImage_) return 1;
    if (vt == vtEffect_ || vt == vtMask_ || vt == vtCopy_) return get<std::int32_t>(filter, 0x38, -1) == 0 ? 1 : 0;
    if (vt == vtImageEffect_ && depth < 6) {
        // eCImageFilterImageEffect::GetOutputCount 0x6662f0: the nested effect's public filters' outputs
        U8* nested = nestedEffect(filter);
        if (nested == nullptr) return 0;
        const std::int32_t n = get<std::int32_t>(nested, 0x20);
        U8* filters = ptr(nested, 0x18);
        std::int32_t sum = 0;
        for (std::int32_t i = 0; i < n; ++i) {
            U8* f = ptr(filters, 8 * static_cast<std::size_t>(i));
            if (f == nullptr || get<std::uint8_t>(f, 0x18) == 0) continue;
            const std::int32_t c = outputCount(f, depth + 1);
            if (c < 0) return -1;
            sum += c;
        }
        return sum;
    }
    return -1;
}

bool EffectOps::graphOutput(const std::uint8_t* filter, std::int32_t index, const void*& image, int depth) const {
    image = nullptr;
    if (filter == nullptr) return true;
    const void* vt = ptr(filter, 0);
    if (vt == vtScene_) {
        // eCImageFilterScene::GetOutputAt 0x75d770: 0 ColorOut = the FP16 scene image +0x38 (else the A8R8G8B8 one +0x40), 1 DepthOut +0x48
        if (index == 0) image = ptr(filter, 0x38) != nullptr ? ptr(filter, 0x38) : ptr(filter, 0x40);
        else if (index == 1) image = ptr(filter, 0x48);
        return true;
    }
    if (vt == vtImage_) {
        // eCImageFilterImage::GetOutputAt 0x665c00: the resource +0x30's image +0x70
        U8* res = ptr(filter, 0x30);
        if (index == 0 && res != nullptr) image = ptr(res, 0x70);
        return true;
    }
    if (vt == vtImageEffect_) {
        if (depth >= 6) return false;
        U8* nested = nestedEffect(filter);
        if (nested == nullptr) return true;
        const std::int32_t n = get<std::int32_t>(nested, 0x20);
        U8* filters = ptr(nested, 0x18);
        for (std::int32_t i = 0; i < n; ++i) {
            U8* f = ptr(filters, 8 * static_cast<std::size_t>(i));
            if (f == nullptr || get<std::uint8_t>(f, 0x18) == 0) continue;
            const std::int32_t c = outputCount(f, depth + 1);
            if (c < 0) return false;
            if (index < c) return graphOutput(f, index, image, depth + 1);
            index -= c;
        }
        return true;  // no such output: GetOutputAt false, the input is attached as null
    }
    return outputImage(filter, index, image);
}

std::string EffectOps::graphName(const std::uint8_t* effect) const {
    std::string s;
    const std::int32_t n = get<std::int32_t>(effect, 0x20);
    U8* filters = ptr(effect, 0x18);
    for (std::int32_t i = 0; i < n && i < 32; ++i) {
        U8* f = ptr(filters, 8 * static_cast<std::size_t>(i));
        if (f == nullptr) continue;
        const void* vt = ptr(f, 0);
        std::string what = vt == vtScene_ ? "scene" : vt == vtImageEffect_ ? "effect " + engine::readCString(f + 0x30)
                         : vt == vtEffect_ ? std::format("{}:{}{}", engine::readCString(f + 0xa0), get<std::int32_t>(f, 0xf0),
                                                         get<std::int32_t>(f, 0x38) == 1 ? " screen" : "")
                         : vt == vtCopy_ ? "copy" : vt == vtTexture_ ? "texture" : vt == vtImage_ ? "image" : "?";
        s += std::format("{}{}({})", s.empty() ? "" : ", ", engine::readCString(f + 0x10), what);
    }
    return s;
}

void EffectOps::postBegin(void* effectPtr) {
    if (verified_ <= 0 || postVerified_ <= 0 || !postOn()) return;
    auto* effect = static_cast<U8*>(effectPtr);
    if (postDepth_++ > 0) return;  // (a nested Execute: none in the game; the outer one's state stays)
    postEffect_ = effect;
    postRt_ = dev_.renderTarget(0);
    postDepthSurface_ = dev_.depthStencil();
    const d3d9::Viewport& v = dev_.viewport();
    postViewport_ = {v.x, v.y, v.width, v.height, v.minZ, v.maxZ};
    // which graph: the admin's scene effect +0x90 / overlay effect +0x98
    U8* admin = static_cast<U8*>(filterAdmin_());
    const char* role = effect == ptr(admin, 0x90) ? "scene" : effect == ptr(admin, 0x98) ? "overlay" : "other";
    auto [it, inserted] = graphsSeen_.try_emplace(effect);
    if (inserted) {
        it->second = std::format("{} {}", role, graphName(effect));
        spdlog::info("native: post: effect {} ({}): {}", effectPtr, role, it->second);
    }
    postGraph_ = std::format("{} ({} filters)", role, get<std::int32_t>(effect, 0x20));
    ++graphRuns_[postGraph_];
    if (role[0] == 'o') overlayWeight_ = get<float>(admin, 0xb0);
}

void EffectOps::postEnd(void* /*effect*/) {
    if (postDepth_ == 0) return;
    if (--postDepth_ > 0) return;
    if (unit_ == Unit::Post) finishUnit();
    postEffect_ = nullptr;
}

EffectOps::Skip EffectOps::semanticValue(const std::uint8_t* obj, std::uint32_t vt, const fx::Parameter& param, std::vector<std::uint8_t>& out,
                                         bool& set) {
    set = true;
    out.clear();
    const auto weather = [&]() { return static_cast<U8*>(weatherAdmin_()); };
    switch (vt) {
    case kFrameTime: {  // eCTimer +0x2c (GetFrameTimeInSeconds)
        const float f = get<float>(timer_(), 0x2c);
        putBytes(out, &f, 1);
        return Skip::None;
    }
    case kTime: {  // eCTimer +0xc / 1000
        const float f = get<float>(timer_(), 0xc) / thousand_;
        putBytes(out, &f, 1);
        return Skip::None;
    }
    case kSceneExposure:
    case kBloomIntensity:
    case kBloomThreshold:
    case kBloomOffset: {
        U8* w = weather();
        if (w == nullptr) return Skip::Unreadable;
        float f = 0.0f;
        if (vt == kSceneExposure) f = get<float>(w, 0x158);
        else if (vt == kBloomIntensity) f = get<float>(w, 0x15c) / get<float>(w, 0x158);
        else if (vt == kBloomThreshold) f = get<float>(w, 0x160);
        else f = get<float>(w, 0x164);
        putBytes(out, &f, 1);
        return Skip::None;
    }
    case kColorMatrix: {
        U8* w = weather();
        if (w == nullptr) return Skip::Unreadable;
        const float tint[3] = {get<float>(w, 0x180), get<float>(w, 0x184), get<float>(w, 0x188)};
        float m[9];
        fxc::colorMatrix(get<float>(w, 0x158), get<float>(w, 0x170), tint, one_, lumWeights_, m);
        putBytes(out, m, 9);
        return Skip::None;
    }
    case kOverlayBlend: {  // the image filter admin's overlay weight +0xb0 (Process 0x663080)
        const float f = get<float>(filterAdmin_(), 0xb0);
        putBytes(out, &f, 1);
        return Skip::None;
    }
    case kImage: {  // eSImageEffectParamImage: the resource +0x58's image +0x70
        U8* res = ptr(obj, 0x58);
        const void* img = res != nullptr ? ptr(res, 0x70) : nullptr;
        putBytes(out, &img, 1);
        return Skip::None;
    }
    case kCamera: {
        U8* cam = camera();
        if (cam == nullptr) return Skip::Unreadable;
        const float camFar = get<float>(cam, 0xc0);
        switch (get<std::int32_t>(obj, 0x58, -1)) {
        case 0:
        case 1: {  // VIEW / PROJ: the camera's matrices as they are
            float m[16];
            if (!readAt(cam, get<std::int32_t>(obj, 0x58) == 0 ? 0x10 : 0x50, m)) return Skip::Unreadable;
            putBytes(out, m, 16);
            return Skip::None;
        }
        case 3:
        case 4: {  // INVVIEW / INVPROJ: bCMatrix::Invert of a copy
            float m[16];
            if (!readAt(cam, get<std::int32_t>(obj, 0x58) == 3 ? 0x10 : 0x50, m)) return Skip::Unreadable;
            matInvert_(m);
            putBytes(out, m, 16);
            return Skip::None;
        }
        case 6: {  // NEARFAR: +0xbc
            float nf[2];
            if (!readAt(cam, 0xbc, nf)) return Skip::Unreadable;
            putBytes(out, nf, 2);
            return Skip::None;
        }
        case 7: {  // SCREENSIZE: the rect +0xc4 (right - left, bottom - top) as floats
            const float wh[2] = {static_cast<float>(get<std::int32_t>(cam, 0xcc) - get<std::int32_t>(cam, 0xc4)),
                                 static_cast<float>(get<std::int32_t>(cam, 0xd0) - get<std::int32_t>(cam, 0xc8))};
            putBytes(out, wh, 2);
            return Skip::None;
        }
        case 8:
        case 9: {  // DEPTHTOWORLD (inv(V) *= inv(diag(P11, P22, 1, 1)) *= scale(camFar)) / DEPTHTOVIEW (the same without the view)
            float p[16], sc[16];
            matIdentity_(p);
            p[0] = get<float>(cam, 0x50);
            p[5] = get<float>(cam, 0x64);
            matInvert_(p);
            matScale_(sc, camFar);
            if (get<std::int32_t>(obj, 0x58) == 9) {
                matMulAssign_(p, sc);
                putBytes(out, p, 16);
                return Skip::None;
            }
            float m[16];
            if (!readAt(cam, 0x10, m)) return Skip::Unreadable;
            matInvert_(m);
            matMulAssign_(m, p);
            matMulAssign_(m, sc);
            putBytes(out, m, 16);
            return Skip::None;
        }
        case 10: {  // FOCALRANGENEAR
            float v[2];
            fxc::focalRangeNear(get<float>(cam, 0xe4), camFar, *epsilon_, minusOne_, v);
            putBytes(out, v, 2);
            return Skip::None;
        }
        case 11: {  // FOCALRANGEFAR
            float v[2];
            fxc::focalRangeFar(get<float>(cam, 0xe8), get<float>(cam, 0xec), camFar, one_, v);
            putBytes(out, v, 2);
            return Skip::None;
        }
        case 2:
        case 5: return Skip::Semantic;  // VIEWPROJ / INVVIEWPROJ: not used by the game's 13 effects
        default: set = false; return Skip::None;
        }
    }
    case kSkyDome: {
        void* illum = illumAdmin_();
        U8* sky = illum != nullptr ? static_cast<U8*>(getSkydome_(illum)) : nullptr;
        if (sky == nullptr) {
            set = false;  // no sky dome: Set returns false, nothing is set
            return Skip::None;
        }
        const std::int32_t kind = get<std::int32_t>(obj, 0x58, -1);
        if (kind >= 0 && kind <= 2) {  // SUN / MOON / STARS MATRIX: +0x310 / +0x350 / +0x2d0
            float m[16];
            if (!readAt(sky, kind == 0 ? 0x310 : kind == 1 ? 0x350 : 0x2d0, m)) return Skip::Unreadable;
            putBytes(out, m, 16);
            return Skip::None;
        }
        U8* light = ptr(sky, 0xf0);
        float d[3];
        if (light == nullptr || !readAt(light, 0x318, d)) return Skip::Unreadable;
        if (kind == 3) {  // SUNLIGHTVECTOR
            putBytes(out, d, 3);
            return Skip::None;
        }
        if (kind != 4 && kind != 5) {
            set = false;
            return Skip::None;
        }
        U8* cam = camera();
        if (cam == nullptr) return Skip::Unreadable;
        vecTransformDirection_(d, cam + 0x10);
        if (kind == 5) {  // SUNFLAREINTENSITY
            const float f = fxc::sunFlare(d[2]);
            putBytes(out, &f, 1);
            return Skip::None;
        }
        // SUNLIGHTSCREENVECTOR: the view-space direction x far, through the projection (bCVector::operator*=), x / y clamped
        const float camFar = get<float>(cam, 0xc0);
        d[0] = d[0] * camFar;
        d[1] = d[1] * camFar;
        d[2] = d[2] * camFar;
        vecMulAssign_(d, cam + 0x50);
        fxc::clampScreenVector(d, one_, minusOne_);
        putBytes(out, d, 3);
        return Skip::None;
    }
    default: (void)param; return Skip::ParamClass;
    }
}

void EffectOps::reportPost() {
    if (!postOn() || postVerified_ <= 0) return;
    std::string graphs;
    for (auto& [name, n] : graphRuns_) {
        graphRunsTotal_[name] += n;
        graphs += std::format("{}{} {}", graphs.empty() ? "" : ", ", name, n);
    }
    graphRuns_.clear();
    std::string totals;
    for (const auto& [name, n] : graphRunsTotal_) totals += std::format("{}{} {}", totals.empty() ? "" : ", ", name, n);
    spdlog::info("native: post (5 s): effects run: {}; total {}; engine draws of the Post view outside the units {}; copies in the post chain left "
                 "to the engine {}; overlay weight {:.3f}",
                 graphs.empty() ? "-" : graphs, totals.empty() ? "-" : totals, postDrawsOutside_, postCopies_, overlayWeight_);
    postDrawsOutside_ = 0;
    postCopies_ = 0;
}

}  // namespace rvk::native
