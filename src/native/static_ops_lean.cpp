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

bool StaticOps::leanGroup(Group g) const {
    switch (g) {
        case Group::Static:
        case Group::Mesh:
        case Group::Decals:
        case Group::Sky:
        case Group::Billboards:
        case Group::SkyCube: return true;
        // O2b: the animated / simulated kinds, in the main view and in the shadow maps' z lists
        case Group::Actors:
        case Group::Trees:
        case Group::Vegetation:
        case Group::ShadowViews:
        case Group::Particles:
        case Group::Strips:
        case Group::Precipitation: return leanAnimVerified_ > 0;
        default: return false;
    }
}

namespace {

// instruction sites the O2b side effects stand for (e8 = call rel32 to `target`; else the bytes)
struct LeanSite {
    std::uint32_t rva;
    std::array<std::uint8_t, 10> b;
    std::uint32_t n;
    std::uint32_t target;
};
constexpr LeanSite kLeanSites[] = {
    {0x5e00ab, {0xe8}, 1, 0x5d1290},                                        // eCParticle_PS::Render: GetMasterInstance
    {0x5e00d0, {0xff, 0x92, 0x48, 0x02, 0, 0}, 6, 0},                       // ... call [vtable + 0x248] (OnProcess, no live particle)
    {0x5e00d9, {0xc7, 0x83, 0xac, 0x03, 0, 0, 0, 0, 0, 0}, 10, 0},          // ... mov dword [PS + 0x3ac], 0
    {0x5d58e1, {0x89, 0x8f, 0xb0, 0x03, 0, 0}, 6, 0},                       // UpdateVertices: the stamp +0x3b0
    {0x5d58ea, {0xe8}, 1, 0x5d8680},                                        // ... DepthSortParticles
    {0x5d58f2, {0xe8}, 1, 0x5d7210},                                        // ... CreateVertices
    {0x5d6eb3, {0xe8}, 1, 0x5dffc0},                                        // ApplyRenderStates: CreateMaterial
    {0x5d6ecd, {0xe8}, 1, 0x684100},                                        // ... RequestData(+0x450, 1)
    {0x5d6f1a, {0xe8}, 1, 0x5d75c0},                                        // ... CreateImage
    {0x5d6f30, {0x41, 0xff, 0x10}, 3, 0},                                   // ... call [sampler material vtable] (SetImage)
    {0x6fd978, {0xe8}, 1, 0x6fde70},                                        // eCStrip_PS::UpdateVertices: CreateVertices
    {0x5c4b27, {0xe8}, 1, 0x5c2450},                                        // eCLightStreaks_PS::Render: CreateBuffers
    {0x5c4b42, {0xe8}, 1, 0x684100},                                        // ... RequestData(+0x70, 1)
    {0x6f1da2, {0x48, 0x8d, 0x15, 0xef, 0x9d, 0x28, 0}, 7, 0},              // RenderBranches: lea rdx, the MaskReference property
    {0x6f1dd7, {0xc6, 0x47, 0x58, 0x50}, 4, 0},                             // ... mov byte [shader + 0x58], 0x50
};

}  // namespace

bool StaticOps::verifyLeanAnim() {
    const auto& s = engine::sym();
    if (engine_ == nullptr) return false;
    const std::pair<void*, std::uint32_t> fns[] = {
        {s.actorUpdateRenderMatrices, 0x4dfee0}, {s.particleGetMasterInstance, 0x5d1290}, {s.particleDepthSort, 0x5d8680},
        {s.particleCreateVertices, 0x5d7210},    {s.particleCreateMaterial, 0x5dffc0},    {s.particleCreateImage, 0x5d75c0},
        {s.particleOnProcess, 0x5d94a0},         {s.stripCreateVertices, 0x6fde70},       {s.streaksCreateBuffers, 0x5c2450}};
    for (const auto& [p, rva] : fns) {
        if (p != engine_ + rva) {
            spdlog::warn("native: lean ops: an engine function of the O2b side effects is not at RVA {:#x} ({}): actors, trees, vegetation, "
                         "particles and the shadow views keep the engine's code",
                         rva, p);
            return false;
        }
    }
    if (s.particleModuleGetInstance == nullptr) return false;
    for (const LeanSite& x : kLeanSites) {
        std::array<std::uint8_t, 10> got{};
        const std::uint32_t n = x.target != 0 ? 5 : x.n;
        bool ok = engine::safeRead(engine_ + x.rva, got.data(), n) && std::equal(got.begin(), got.begin() + x.n, x.b.begin());
        if (ok && x.target != 0) {
            std::int32_t rel = 0;
            std::memcpy(&rel, got.data() + 1, 4);
            ok = static_cast<std::int64_t>(x.rva) + 5 + rel == x.target;
        }
        if (!ok) {
            spdlog::warn("native: lean ops: the code at RVA {:#x} differs: actors, trees, vegetation, particles and the shadow views keep the "
                         "engine's code",
                         x.rva);
            return false;
        }
    }
    // eCParticle_PS's vtable slot +0x248 (Render's in-slot simulation) is OnProcess
    const void* onProcess = nullptr;
    if (!readAt(engine_ + 0x9bbbd8, 0x248, onProcess) || onProcess != s.particleOnProcess) {
        spdlog::warn("native: lean ops: eCParticle_PS vtable +0x248 is not OnProcess: the O2b groups keep the engine's code");
        return false;
    }
    updateRenderMatrices_ = reinterpret_cast<bool (*)(const void*, std::uint32_t)>(s.actorUpdateRenderMatrices);
    particleModule_ = reinterpret_cast<void* (*)()>(s.particleModuleGetInstance);
    getMasterInstance_ = reinterpret_cast<void* (*)(void*, const void*)>(s.particleGetMasterInstance);
    depthSortParticles_ = reinterpret_cast<void (*)(const void*)>(s.particleDepthSort);
    particleCreateVertices_ = reinterpret_cast<bool (*)(const void*)>(s.particleCreateVertices);
    particleCreateMaterial_ = reinterpret_cast<bool (*)(const void*)>(s.particleCreateMaterial);
    particleCreateImage_ = reinterpret_cast<bool (*)(const void*)>(s.particleCreateImage);
    stripCreateVertices_ = reinterpret_cast<bool (*)(const void*)>(s.stripCreateVertices);
    streaksCreateBuffers_ = reinterpret_cast<bool (*)(const void*)>(s.streaksCreateBuffers);
    maskRefProperty_ = engine_ + 0x97bb98;
    spdlog::info("native: lean ops: O2b checks passed (UpdateRenderMatrices, the particle Render / UpdateVertices / ApplyRenderStates "
                 "sites, the strip / light streak creation, the SpeedTree MaskReference store)");
    return true;
}

// ---- O2b: the animated / simulated kinds -----------------------------------------------------------------------------------------

void StaticOps::leanActorPre(const std::uint8_t* wrapper, std::uint16_t primIdx) {
    // eCAnimation_PS::Render 0x4d6fe0: the wrapper's resource (+0x18) with a HardRenderActor2 (+0x98) -> UpdateRenderMatrices(wrapper,
    // the frame id) 0x4dfee0 (rebuilds the palette +0x48 once per frame: its stamp +0x58); true -> HardRenderActor2::Render 0x4dd650:
    // the touch of the HRA's resource (HRA +0; inline QueueData / CacheResource), cached (state 3): the touch of the LOD primitive's
    // material (prim +0x10), then the draw
    U8* res = ptr(wrapper, 0x18);
    U8* hra = ptr(res, 0x98);
    if (res == nullptr || hra == nullptr || frameAdmin_ == nullptr) return;
    const std::uint32_t frame = get<std::uint32_t>(frameAdmin_(), 0x4c);
    if (!updateRenderMatrices_(wrapper, frame)) return;
    ++leanWindow_.palettes;
    U8* hres = ptr(hra, 0);
    touchNow(hres);
    if (get<std::int32_t>(hres, 0x38) != 3) return;
    const std::uint32_t lod = sc::actorLod(get<std::uint32_t>(ptr(wrapper, 8), 0x250));
    const std::uint32_t primIndex = sc::actorPrimitive(get<std::uint16_t>(hra, 0x38 + 0x14 * static_cast<std::size_t>(lod)), primIdx);
    U8* prims = ptr(hra, 0x90);
    if (prims == nullptr) return;
    touchNow(ptr(prims + 0x20 * static_cast<std::size_t>(primIndex), 0x10));
}

void StaticOps::leanTreeSort(const std::uint8_t* rb, bool zpass) {
    // eCRenderContextSpeedTree::Render 0x6ee320 / RenderZPass 0x6ee3f0, before the type dispatch: a non-zero stamp (+0xe0 solid, +0xe4
    // z: set by the request) -> qsort of the queue (+0xb8 / +0xd0, count +0xc0 / +0xd8, 0x18-byte entries) with SortInstances /
    // SortInstancesZPass when it holds entries, then the stamp = 0
    const std::size_t stamp = zpass ? 0xe4 : 0xe0;
    if (get<std::int32_t>(rb, stamp) == 0) return;
    const std::int32_t n = get<std::int32_t>(rb, zpass ? 0xd8 : 0xc0);
    U8* queue = ptr(rb, zpass ? 0xd0 : 0xb8);
    if (n > 0 && queue != nullptr) qsort_(const_cast<std::uint8_t*>(queue), static_cast<std::size_t>(n), 0x18, zpass ? treeCmpZ_ : treeCmp_);
    store4(rb + stamp, 0);
    ++leanWindow_.treeSorts;
}

void StaticOps::leanMaskRef(const std::uint8_t* shader) {
    // RenderBranches 0x6f1d9f..0x6f1dde (and the other five): NotifyPropertyValueChangedEnterEx (vtable +0xb0), MaskReference
    // (+0x58) = 0x50, NotifyPropertyValueChangedExitEx (+0xb8), the property 0x97bb98, 0; once 80 it stays (a reload resets it)
    if (shader == nullptr || get<std::uint8_t>(shader, 0x58) == 0x50) return;
    using NotifyFn = void (*)(const void* self, const void* property, int);
    U8* vt = ptr(shader, 0);
    const auto enter = reinterpret_cast<NotifyFn>(ptr(vt, 0xb0));
    const auto exit = reinterpret_cast<NotifyFn>(ptr(vt, 0xb8));
    if (enter == nullptr || exit == nullptr) return;
    enter(shader, maskRefProperty_, 0);
    const std::uint8_t v = 0x50;
    engine::safeWrite(const_cast<std::uint8_t*>(shader) + 0x58, &v, 1);
    exit(shader, maskRefProperty_, 0);
    ++leanWindow_.maskRefs;
}

bool StaticOps::leanParticlePre(const std::uint8_t* ps) {
    // eCParticle_PS::Render 0x5e0080: (1) an instanced emitter (+0x29d == 1): eCParticleModule::GetMasterInstance(module, PS) (creates
    // the hidden master per creator template on first use); null -> return; (2) not instanced (+0x29d == 0) and no live particle
    // (+0x390 == 0): the emitter's OnProcess (vtable +0x248: DoProcessV2, guarded by its own tick stamp); (3) the inactivity timer
    // +0x3ac = 0; then RenderVertices
    const std::uint8_t instanced = get<std::uint8_t>(ps, 0x29d);
    if (instanced == 1) {
        ++leanWindow_.masters;
        if (getMasterInstance_(particleModule_(), ps) == nullptr) {
            ++leanWindow_.noMaster;
            return false;
        }
    } else if (instanced == 0) {
        if (get<std::int32_t>(ps, 0x390) == 0) {
            using OnProcessFn = void (*)(const void* self);
            const auto onProcess = reinterpret_cast<OnProcessFn>(ptr(ptr(ps, 0), 0x248));
            if (onProcess == nullptr) {
                leanBlock_ |= kBlockEdge;
                return true;
            }
            onProcess(ps);
            ++leanWindow_.kicks;
            if (get<std::int32_t>(ps, 0x390) != 0) ++leanWindow_.kicksSpawned;
        }
    } else {
        // (+0x29d neither 0 nor 1: Render calls neither; the composition's instanced path tests != 0)
        leanBlock_ |= kBlockEdge;
    }
    store4(ps + 0x3ac, 0);
    ++leanWindow_.inactivity;
    return true;
}

void StaticOps::leanParticleStates(const std::uint8_t* ps) {
    // ApplyRenderStates 0x5d6e50 (after the gfx admin / vtable / entity / visual checks): CreateMaterial, the material (+0x450) or
    // return, RequestData(material, 1), its shader cast to eCShaderParticle or return, with a sampler material (+0x448): CreateImage
    // and the sampler material's SetImage(+0x440) (vtable +0); the rest is draw state (composed)
    particleCreateMaterial_(ps);
    U8* material = ptr(ps, 0x450);
    if (material == nullptr) return;
    touchNow(material);
    U8* shader = ptr(material, 0x70);
    if (shader == nullptr || get<const void*>(shader, 0) != engine_ + 0x9b6d38) return;  // ??_7eCShaderParticle (else: no state)
    U8* sampMat = ptr(ps, 0x448);
    if (sampMat == nullptr) return;
    const bool had = ptr(ps, 0x440) != nullptr;
    particleCreateImage_(ps);
    using SetImageFn = void (*)(const void* self, const void* image);
    if (const auto setImage = reinterpret_cast<SetImageFn>(ptr(ptr(sampMat, 0), 0)); setImage != nullptr) setImage(sampMat, ptr(ps, 0x440));
    if (!had && ptr(ps, 0x440) != nullptr) ++leanWindow_.particleCreates;
}

void StaticOps::leanPoolFrom(const std::uint8_t* var, std::uint32_t count) {
    if (!leanCollect_ || count == 0 || count > 32) return;
    leanPool_.var = var;
    leanPool_.count = count;
    std::memcpy(leanPool_.regs, fxPool_, 16 * static_cast<std::size_t>(count));
}

void StaticOps::leanUpdateFrame() {
    const std::uint64_t f = dev_.stats().frames;
    if (f == leanFrameNo_) return;
    leanFrameNo_ = f;
    leanFrameKind_ = sc::leanFrameKind(f, leanCheck_);
    leanFrameOn_ = lean_ && leanFrameKind_ == sc::LeanFrame::Lean;
    ++leanWindow_.frames;
    if (leanFrameKind_ == sc::LeanFrame::Check) ++leanWindow_.checkFrames;
    if (particleCache_.size() > 4096) particleCache_.clear();  // (dead emitters' entries: bounded; an entry lives one frame)
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
    leanVeg_ = {};
    leanParticleTarget_ = nullptr;
    leanPool_.count = 0;
    if (!lean_ || g_ == nullptr || !g_->takeOver) return;
    if (leanAnimVerified_ == 0 && engine_ != nullptr) leanAnimVerified_ = verifyLeanAnim() ? 1 : -1;
    if (!leanGroup(g)) return;
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
        if ((leanBlock_ & kBlockEdge) != 0) ++leanWindow_.blockedEdge;
        return false;
    }
    const auto& s = engine::sym();
    // O2b, deferred to the commit (a fallback slot must find the engine's own state): the vegetation buffer cursor as Render leaves
    // it; a particle target's in-place depth sort (UpdateVertices: the same qsort the composition ran on its copy) + our vertices of
    // this frame for the target's next instance; a class pool's registers as the op's ApplyRenderStates / passes wrote them
    if (leanVeg_.admin != nullptr) {
        store4(leanVeg_.admin + 0x40, leanVeg_.cursor);
        ++leanWindow_.vegCursors;
    }
    if (leanParticleTarget_ != nullptr) {
        depthSortParticles_(leanParticleTarget_);
        ParticleCache& c = particleCache_[leanParticleTarget_];
        c.frame = dev_.stats().frames;
        c.quads = leanParticleQuads_;
        c.vb.swap(leanParticleVb_);
        ++leanWindow_.particleSorts;
    }
    if (leanPool_.count != 0) {
        if (U8* data = ptr(ptr(leanPool_.var, 0), 0x38); data != nullptr) {
            engine::safeWrite(const_cast<std::uint8_t*>(data), leanPool_.regs, 16 * static_cast<std::size_t>(leanPool_.count));
            ++leanWindow_.poolWrites;
        }
    }
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
    t.blockedEdge += w.blockedEdge;
    t.palettes += w.palettes;
    t.treeSorts += w.treeSorts;
    t.maskRefs += w.maskRefs;
    t.vegCursors += w.vegCursors;
    t.poolWrites += w.poolWrites;
    t.poolSyncs += w.poolSyncs;
    t.masters += w.masters;
    t.noMaster += w.noMaster;
    t.kicks += w.kicks;
    t.kicksSpawned += w.kicksSpawned;
    t.inactivity += w.inactivity;
    t.particleSorts += w.particleSorts;
    t.particleCached += w.particleCached;
    t.particleCreates += w.particleCreates;
    t.stripCreates += w.stripCreates;
    t.streakCreates += w.streakCreates;
    if (active()) {
        std::string skipped, kept;
        for (std::size_t i = 0; i < kGroups; ++i) {
            if (!leanGroup(static_cast<Group>(i))) continue;
            skipped += std::format("{}{} {}", skipped.empty() ? "" : ", ", name(static_cast<Group>(i)), w.skipped[i]);
            if (w.kept[i] != 0) kept += std::format("{}{} {}", kept.empty() ? "" : ", ", name(static_cast<Group>(i)), w.kept[i]);
        }
        std::string names;
        for (const std::string& n : placeholderNames_) names += std::format("{}'{}'", names.empty() ? "" : ", ", n);
        spdlog::info("native: lean (5 s): {}, {} frames ({} check frames); engine code skipped: {}; kept {} (video {}, edge cases {}); touches {} "
                     "({} RequestData calls), lightmaps {}, stream 3 {}, instance sorts {}, billboard queue resets {} (longest queue {}), sky {}, "
                     "first-use creates {}; placeholder binds {} ({} images{}{})",
                     lean_ ? "on" : "off", w.frames, w.checkFrames, skipped, kept.empty() ? "0" : kept, w.blockedVideo, w.blockedEdge, w.touches,
                     w.touchCalls, w.lightmaps, w.stream3, w.instanceSorts, w.queueResets, w.queueMax, w.skyWrites, w.creates, w.placeholderBinds,
                     placeholderImages_.size(), names.empty() ? "" : ": ", names);
        if (leanAnimVerified_ > 0) {
            spdlog::info("native: lean (5 s) O2b: palettes {}, tree queue sorts {}, MaskReference stores {}, vegetation cursors {}; particles: "
                         "masters {} (none {}), in-slot simulations {} (spawned {}), inactivity resets {}, depth sorts {}, vertices reused {}, "
                         "images created {}; strip creates {}, light streak creates {}; class pool writes {}, engine pool syncs {}",
                         w.palettes, w.treeSorts, w.maskRefs, w.vegCursors, w.masters, w.noMaster, w.kicks, w.kicksSpawned, w.inactivity,
                         w.particleSorts, w.particleCached, w.particleCreates, w.stripCreates, w.streakCreates, w.poolWrites, w.poolSyncs);
        }
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
