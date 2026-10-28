// O2a lean ops (docs/research/engine-frame/opt-plan.md "O2a result"; settings native.lean, native.lean_check): for an op of the
// mesh-like groups whose draws we composed and issued, the engine's slot code (or the non-op path: RenderDecals,
// RenderBillboards, the sky cube faces' Render) is not run. What that code does besides drawing (opt-side-effects-mesh.md 0 / 7)
// is collected while composing and run here instead, at the op's place:
//   * resource touches: eCResource2::RequestData(image, 1) for every image a composed draw samples (ExecuteShaderConstants'
//     QueueData / CacheResource of the live sampler slots: the resource cache's LRU stamp, first loads), overlay materials
//     (OnGetPasses), decal / billboard materials; once per frame per resource
//   * eCMeshResource2::BindLightmaps(1) (the lightmaps' touches + the admin's s4 / s5 slots and the material mode's stage bind)
//   * the visual's stream 3 (RenderZPass: off; the hardware instanced batches: on), the render-instance sort + `res +0xa4 = 0`
//   * the opaque billboards' queue reset `admin +0x28 = 0`, the decal / billboard admins' first-use buffers / default materials
//   * the sky (eCSkydome_PS::Render): first-use buffers, stars, moon; the cloud scale nodes, the scroller's direction and the dome
//     shader's cloud fields
// An op whose side effects are not covered (a video texture bound) or whose composition failed keeps running the engine's code
// (its draws dropped, as before). Part of StaticOps (static_ops.h).

#include <windows.h>

#include <algorithm>
#include <cstring>
#include <format>

#include <spdlog/spdlog.h>

#include "engine/memread.h"
#include "engine/symbols.h"
#include "hal/vulkan/hal_vulkan.h"
#include "native/static_ops.h"

namespace rvk::native {

namespace {

using engine::readAt;
using U8 = const std::uint8_t;

template <class T>
inline T get(const void* p, std::size_t off, T fallback = T{}) {
    T v{};
    return readAt(p, off, v) ? v : fallback;
}
inline U8* ptr(const void* p, std::size_t off) { return get<U8*>(p, off); }

// a guarded 4-byte store into an engine object
bool store4(const void* p, std::uint32_t v) {
    __try {
        *static_cast<volatile std::uint32_t*>(const_cast<void*>(p)) = v;
        return true;
    } __except (1 /* EXCEPTION_EXECUTE_HANDLER */) {
        return false;
    }
}

using ThisFn = void (*)(const void* self);
using BoolThisFn = bool (*)(const void* self);
using RequestFn = int (*)(const void* self, int mode);
using BindLightmapsFn = int (*)(const void* self, int mode);
using Vec2Fn = void (*)(const void* self, const float* v);

}  // namespace

void StaticOps::setLean(bool on, int check) {
    if (on != lean_ || check != leanCheck_) {
        spdlog::info("native: lean ops {} (check every {} frames)", on ? "on" : "off", check);
    }
    lean_ = on;
    leanCheck_ = std::max(check, 0);
    leanFrameNo_ = ~0ull;
}

bool StaticOps::leanGroup(Group g) {
    switch (g) {
        case Group::Static:
        case Group::Mesh:
        case Group::Decals:
        case Group::Sky:
        case Group::Billboards:
        case Group::SkyCube: return true;
        default: return false;  // O2b / O2c: actors, trees, vegetation, shadow views, particles, strips, precipitation
    }
}

void StaticOps::leanUpdateFrame() {
    const std::uint64_t f = dev_.stats().frames;
    if (f == leanFrameNo_) return;
    leanFrameNo_ = f;
    leanFrameKind_ = sc::leanFrameKind(f, leanCheck_);
    leanFrameOn_ = lean_ && leanFrameKind_ == sc::LeanFrame::Lean;
    ++leanWindow_.frames;
    if (leanFrameKind_ == sc::LeanFrame::Check) ++leanWindow_.checkFrames;
}

bool StaticOps::leanFrame() {
    leanUpdateFrame();
    return leanFrameOn_;
}

void StaticOps::leanBegin(Group g) {
    leanCollect_ = false;
    leanBlock_ = 0;
    leanTouches_.clear();
    leanLightmaps_.clear();
    leanStream3_.clear();
    if (!lean_ || g_ == nullptr || !g_->takeOver || !leanGroup(g)) return;
    if (leanVerified_ == 0) {
        const auto& s = engine::sym();
        const void* need[] = {s.resourceRequestData, s.meshBindLightmaps, s.meshSortRenderInstances, s.decalCreateBuffers,
                              s.decalCreateDefaultShader, s.billboardCreateBuffers, s.billboardCreateDefaultMaterial,
                              s.skydomeCreateBuffers, s.skydomeCreateStars, s.skydomeCreateMoon, s.texScaleSetScale,
                              s.texScrollerSetDirection};
        leanVerified_ = std::all_of(std::begin(need), std::end(need), [](const void* p) { return p != nullptr; }) ? 1 : -1;
        if (leanVerified_ < 0) spdlog::warn("native: lean ops: an engine function of the side effects is missing: the engine's code keeps running");
    }
    if (leanVerified_ < 0 || !leanFrame()) return;
    leanCollect_ = true;
}

bool StaticOps::leanCommit() {
    if (!leanCollect_) return false;
    leanCollect_ = false;
    if (g_ == nullptr) return false;
    const auto gi = static_cast<std::size_t>(g_ - groups_.data());
    if (leanBlock_ != 0) {
        ++leanWindow_.kept[gi];
        if ((leanBlock_ & kBlockVideo) != 0) ++leanWindow_.blockedVideo;
        return false;
    }
    const auto& s = engine::sym();
    // eCMeshResource2::Render / ExecuteRenderInstances / RenderDecals: BindLightmaps(1) before the draws
    for (const void* res : leanLightmaps_) {
        reinterpret_cast<BindLightmapsFn>(s.meshBindLightmaps)(res, 1);
        ++leanWindow_.lightmaps;
    }
    for (const void* res : leanTouches_) touchNow(res);
    for (const LeanStream3& x : leanStream3_) {
        static_cast<hal::vulkan::HalPrimitive*>(x.primitive)->EnableStream(3, x.on);
        ++leanWindow_.stream3;
    }
    slotSkipped_ = true;
    ++leanWindow_.skipped[gi];
    return true;
}

void StaticOps::touchNow(const void* resource) {
    if (resource == nullptr) return;
    ++leanWindow_.touches;
    if (!touched_.first(resource, dev_.stats().frames + 1)) return;
    ++leanWindow_.touchCalls;
    reinterpret_cast<RequestFn>(engine::sym().resourceRequestData)(resource, 1);
}

void StaticOps::leanImage(const void* resource, const void* imageData) {
    if (imageData == nullptr) {
        // bound as the placeholder (not loaded / evicted): the behaviour check of the lean path (must settle to 0 as without it)
        ++leanWindow_.placeholderBinds;
        if (placeholderImages_.size() < 256 && std::find(placeholderImages_.begin(), placeholderImages_.end(), resource) == placeholderImages_.end()) {
            placeholderImages_.push_back(resource);
            if (lean_ && placeholderNames_.size() < 6) placeholderNames_.push_back(engine::readCString(static_cast<U8*>(resource) + 0x40));
        }
    }
    if (!leanCollect_) return;
    leanTouches_.push_back(resource);
    const void* video = nullptr;
    if (imageData != nullptr && readAt(imageData, 0x70, video) && video != nullptr) {
        // eCGfxImageData::Execute advances a Bink video bound to a stage (FUN_18070fd00): not reproduced, the op keeps the engine's code
        leanBlock_ |= kBlockVideo;
        if (videoLogged_ < 8) {
            ++videoLogged_;
            spdlog::info("native: lean ops: image '{}' carries a video (image data {} +0x70 {}): its ops keep the engine's code",
                         engine::readCString(static_cast<U8*>(resource) + 0x40), imageData, video);
        }
    }
}

void StaticOps::leanStream3(const std::uint8_t* res, bool on) {
    if (!leanCollect_) return;
    if (auto* prim = hal::vulkan::HalPrimitive::fromApi(ptr(ptr(res, 0x88), 0x10))) leanStream3_.push_back({prim, on});
}

void StaticOps::leanSortInstances(const std::uint8_t* res) {
    // eCRenderContextMesh::Render 0x5c7110: once per frame per resource, the render instances sorted in place, then +0xa4 = 0
    if (!leanCollect_ || get<std::int32_t>(res, 0xa4) == 0) return;
    reinterpret_cast<ThisFn>(engine::sym().meshSortRenderInstances)(res);
    store4(res + 0xa4, 0);
    ++leanWindow_.instanceSorts;
}

void StaticOps::leanDecalsPre(const std::uint8_t* admin) {
    if (!leanCollect_) return;
    const auto& s = engine::sym();
    // RenderDecals 0x4fc220 first: CreateBuffers (early-out with the visual +0x58), CreateDefaultShader (early-out with +0x48)
    const bool had = ptr(admin, 0x58) != nullptr && ptr(admin, 0x48) != nullptr;
    reinterpret_cast<BoolThisFn>(s.decalCreateBuffers)(admin);
    reinterpret_cast<BoolThisFn>(s.decalCreateDefaultShader)(admin);
    if (!had && ptr(admin, 0x58) != nullptr && ptr(admin, 0x48) != nullptr) {
        ++leanWindow_.creates;
        spdlog::info("native: lean ops: the decal admin's buffers / default shader created by the lean path (frame {})", dev_.stats().frames);
    }
}

void StaticOps::leanBillboardsPre(const std::uint8_t* admin) {
    if (!leanCollect_) return;
    const auto& s = engine::sym();
    // RenderBillboards 0x4f0530 first: CreateBuffers (early-out with the visual +0x40), CreateDefaultMaterial (with +0x30)
    const bool had = ptr(admin, 0x40) != nullptr && ptr(admin, 0x30) != nullptr;
    reinterpret_cast<BoolThisFn>(s.billboardCreateBuffers)(admin);
    reinterpret_cast<BoolThisFn>(s.billboardCreateDefaultMaterial)(admin);
    if (!had && ptr(admin, 0x40) != nullptr && ptr(admin, 0x30) != nullptr) {
        ++leanWindow_.creates;
        spdlog::info("native: lean ops: the billboard admin's buffers / default material created by the lean path (frame {})", dev_.stats().frames);
    }
}

void StaticOps::leanBillboardsPost(const std::uint8_t* admin) {
    // RenderBillboards' end: the queue (filled by the requests) emptied inside its `count != 0` block, after the buffers and the
    // default material were made (else it returns false first and the queue stays)
    const std::int32_t n = get<std::int32_t>(admin, 0x28);
    leanWindow_.queueMax = std::max(leanWindow_.queueMax, n);
    if (ptr(admin, 0x40) != nullptr && ptr(admin, 0x30) != nullptr && n != 0) {
        store4(admin + 0x28, 0);
        ++leanWindow_.queueResets;
    }
}

void StaticOps::leanSkyPre(const std::uint8_t* sky) {
    // eCSkydome_PS::Render 0x6e4730 (all inside `PS +0x18 & 1`): CreateBuffers (false: return), [camera hack], CreateStars
    // (rebuilds when StarCount +0x28 != +0x290; draws), the sun (+0x138: SetBillboardRenderCount(+0x170, 1), its one-billboard
    // array; draws), CreateMoon (draws), the cloud scale nodes (+0x110 / +0x118: the "Scale" notify pair around +0x48 = +0x4c =
    // 1 / CloudSize +0xb8 / +0xbc, = SetScale), SetDirection(+0x108, sky +0xb0), the dome shader's fields, [restore], the dome.
    // The draws are ours; their composition reads only what these write (stars, moon, sun record) or the dome's next notify
    if (!leanCollect_ || (get<std::uint8_t>(sky, 0x18) & 1) == 0) return;
    const auto& s = engine::sym();
    if (!reinterpret_cast<BoolThisFn>(s.skydomeCreateBuffers)(sky)) return;
    reinterpret_cast<BoolThisFn>(s.skydomeCreateStars)(sky);
    if (ptr(sky, 0x138) != nullptr && s.skyObjectSetBillboardRenderCount != nullptr && get<std::int32_t>(sky, 0x188) == 1) {
        // (the array's first use / a count change: the composition leaves that frame to the engine, Skip::SkyRebuilt)
        using CountFn = bool (*)(const void* self, std::uint32_t n);
        reinterpret_cast<CountFn>(s.skyObjectSetBillboardRenderCount)(sky + 0x170, 1);
    }
    reinterpret_cast<BoolThisFn>(s.skydomeCreateMoon)(sky);
    for (const auto& [node, size] : {std::pair<std::size_t, std::size_t>{0x110, 0xb8}, {0x118, 0xbc}}) {
        U8* n = ptr(sky, node);
        if (n == nullptr) continue;
        const float v = 1.0f / get<float>(sky, size);  // divss 1.0 (DAT_1809f1698) / size
        const float scale[2] = {v, v};
        reinterpret_cast<Vec2Fn>(s.texScaleSetScale)(n, scale);
    }
    if (U8* scroller = ptr(sky, 0x108); scroller != nullptr) reinterpret_cast<Vec2Fn>(s.texScrollerSetDirection)(scroller, reinterpret_cast<const float*>(sky + 0xb0));
    if (U8* dome = ptr(sky, 0xf0); dome != nullptr) {
        static constexpr std::pair<std::size_t, std::size_t> kFields[] = {{0x300, 0xa0}, {0x304, 0xa4}, {0x308, 0xa8}, {0x310, 0x90}};
        for (const auto& [dst, src] : kFields) store4(dome + dst, get<std::uint32_t>(sky, src));
    }
    ++leanWindow_.skyWrites;
    // (report: what was written, read back while the objects live)
    SkySnap& k = leanSky_;
    k.valid = true;
    U8* s1 = ptr(sky, 0x110);
    U8* s2 = ptr(sky, 0x118);
    U8* sc = ptr(sky, 0x108);
    U8* dome = ptr(sky, 0xf0);
    k.size[0] = get<float>(sky, 0xb8);
    k.size[1] = get<float>(sky, 0xbc);
    k.scale[0][0] = get<float>(s1, 0x48);
    k.scale[0][1] = get<float>(s1, 0x4c);
    k.scale[1][0] = get<float>(s2, 0x48);
    k.scale[1][1] = get<float>(s2, 0x4c);
    k.dir[0] = get<float>(sky, 0xb0);
    k.dir[1] = get<float>(sky, 0xb4);
    k.scroller[0] = get<float>(sc, 0x50);
    k.scroller[1] = get<float>(sc, 0x54);
    static constexpr std::size_t kDome[4] = {0x300, 0x304, 0x308, 0x310}, kSrc[4] = {0xa0, 0xa4, 0xa8, 0x90};
    for (std::size_t i = 0; i < 4; ++i) {
        k.dome[i] = get<float>(dome, kDome[i]);
        k.src[i] = get<float>(sky, kSrc[i]);
    }
}

void StaticOps::leanReport() {
    LeanStats& w = leanWindow_;
    LeanStats& t = leanTotal_;
    t.frames += w.frames;
    t.checkFrames += w.checkFrames;
    for (std::size_t i = 0; i < kGroups; ++i) {
        t.skipped[i] += w.skipped[i];
        t.kept[i] += w.kept[i];
    }
    t.blockedVideo += w.blockedVideo;
    t.touches += w.touches;
    t.touchCalls += w.touchCalls;
    t.lightmaps += w.lightmaps;
    t.stream3 += w.stream3;
    t.instanceSorts += w.instanceSorts;
    t.queueResets += w.queueResets;
    t.placeholderBinds += w.placeholderBinds;
    t.skyWrites += w.skyWrites;
    t.creates += w.creates;
    t.queueMax = std::max(t.queueMax, w.queueMax);
    if (active()) {
        std::string skipped, kept;
        for (std::size_t i = 0; i < kGroups; ++i) {
            if (!leanGroup(static_cast<Group>(i))) continue;
            skipped += std::format("{}{} {}", skipped.empty() ? "" : ", ", name(static_cast<Group>(i)), w.skipped[i]);
            if (w.kept[i] != 0) kept += std::format("{}{} {}", kept.empty() ? "" : ", ", name(static_cast<Group>(i)), w.kept[i]);
        }
        std::string names;
        for (const std::string& n : placeholderNames_) names += std::format("{}'{}'", names.empty() ? "" : ", ", n);
        spdlog::info("native: lean (5 s): {}, {} frames ({} check frames); engine code skipped: {}; kept (video) {}; touches {} ({} RequestData "
                     "calls), lightmaps {}, stream 3 {}, instance sorts {}, billboard queue resets {} (longest queue {}), sky {}, first-use creates "
                     "{}; placeholder binds {} ({} images{}{})",
                     lean_ ? "on" : "off", w.frames, w.checkFrames, skipped, kept.empty() ? "0" : kept, w.touches, w.touchCalls, w.lightmaps,
                     w.stream3, w.instanceSorts, w.queueResets, w.queueMax, w.skyWrites, w.creates, w.placeholderBinds, placeholderImages_.size(),
                     names.empty() ? "" : ": ", names);
        if (leanSky_.valid) {
            // the cloud fields Render keeps in step with the weather (the scale nodes, the scroller, the dome shader's fields), as last written
            const SkySnap& k = leanSky_;
            spdlog::info("native: lean: sky clouds: size ({}, {}) -> scale nodes ({}, {}) ({}, {}); direction ({}, {}) -> scroller ({}, {}); "
                         "dome fields ({}, {}, {}, {}) from ({}, {}, {}, {})",
                         k.size[0], k.size[1], k.scale[0][0], k.scale[0][1], k.scale[1][0], k.scale[1][1], k.dir[0], k.dir[1], k.scroller[0],
                         k.scroller[1], k.dome[0], k.dome[1], k.dome[2], k.dome[3], k.src[0], k.src[1], k.src[2], k.src[3]);
        }
    }
    placeholderImages_.clear();
    placeholderNames_.clear();
    w = {};
}

}  // namespace rvk::native
