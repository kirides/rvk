#include "engine/scene_hooks.h"

#include <windows.h>

#include <array>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <string>
#include <unordered_map>
#include <utility>

#include <spdlog/spdlog.h>

#include "core/hooks.h"
#include "d3d9/device.h"
#include "engine/symbols.h"
#include "hal/vulkan/hal_vulkan.h"
#include "vk/formats.h"

namespace rvk::engine::scene {
namespace {

namespace draws = d3d9::draws;
using d3d9::scene::ImageRole;
using d3d9::scene::Pass;
using d3d9::scene::View;

std::atomic<d3d9::Device*> g_device{nullptr};

// call-nesting flags: which caller an eCFrustumDatabase::Execute belongs to (render thread only)
int g_inSceneFilter = 0;   // eCImageFilterScene::OnUpdateOutput: the main camera into the image effect's scene target
int g_inCameraRender = 0;  // eCCameraBase::Render: the main camera without an image effect (DoRender)
int g_inRenderEntity = 0;  // eCRenderSystemAdmin::RenderEntity: entity previews

// the attached device when called on its render thread
d3d9::Device* device() {
    d3d9::Device* d = g_device.load(std::memory_order_acquire);
    return d != nullptr && d->recordingThread() ? d : nullptr;
}

void logPhase(d3d9::Device& d, const char* what) {
    if (!hal::vulkan::logThisFrame()) return;
    const auto p = d.phases().current();
    spdlog::info("phase> {} -> {}/{} (depth {})", what, d3d9::scene::name(p.view), d3d9::scene::name(p.pass), d.phases().depth());
}

// a view scope around an original call
struct ViewScope {
    d3d9::Device* d;
    const char* what;
    ViewScope(View v, Pass p, const char* w) : d(device()), what(w) {
        if (d != nullptr) {
            d->phases().push(v, p);
            logPhase(*d, what);
        }
    }
    ~ViewScope() {
        if (d != nullptr) {
            d->phases().pop();
            logPhase(*d, "leave");
        }
    }
    ViewScope(const ViewScope&) = delete;
    ViewScope& operator=(const ViewScope&) = delete;
};

// a pass inside the current view around an original call; the previous pass continues afterwards
struct PassScope {
    d3d9::Device* d;
    Pass saved = Pass::None;
    PassScope(Pass p, const char* what) : d(device()) {
        if (d != nullptr) {
            saved = d->phases().pass();
            d->phases().setPass(p);
            logPhase(*d, what);
        }
    }
    ~PassScope() {
        if (d != nullptr) d->phases().setPass(saved);
    }
    PassScope(const PassScope&) = delete;
    PassScope& operator=(const PassScope&) = delete;
};

void setPass(Pass p, const char* what) {
    if (d3d9::Device* d = device()) {
        d->phases().setPass(p);
        logPhase(*d, what);
    }
}

// engine memory reads that cannot crash the game (pointers of objects we do not own)
bool safeRead(const void* src, void* dst, std::size_t size) {
    __try {
        std::memcpy(dst, src, size);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

template <class T>
bool readAt(const void* base, std::size_t offset, T& out) {
    return base != nullptr && safeRead(static_cast<const std::uint8_t*>(base) + offset, &out, sizeof(T));
}

// The main view's camera (engine-gfx.md 4, frame-phases.md 3): eCFrustumDatabase +0x14c0 = its eCCameraBase, whose +0x1738
// is the eCGfxCamera that eCCameraBase::ExecuteGfxCamera executes (eCGfxCamera::Execute renders through ms_pMasterCamera
// instead when that is set). eCGfxCamera: view +0x20, projection +0x60, view-projection +0xe0, inverse view +0x120, camera
// position +0x150 (the inverse view's translation row). eCCameraBase +0xc0: the far plane SetCamera puts into c11.w.
void readMainCamera(d3d9::Device& d, const void* db) {
    if (d.camera().valid) return;  // the first main view of the frame counts
    const void* base = nullptr;
    const void* cam = nullptr;
    if (!readAt(db, 0x14c0, base) || !readAt(base, 0x1738, cam) || cam == nullptr) return;
    if (const auto* master = static_cast<void* const*>(sym().gfxCameraMaster); master != nullptr) {
        const void* m = nullptr;
        if (safeRead(master, &m, sizeof(m)) && m != nullptr) cam = m;
    }
    d3d9::scene::CameraInput in;
    if (!readAt(cam, 0x20, in.view.m) || !readAt(cam, 0x60, in.projection.m) || !readAt(cam, 0xe0, in.viewProjection.m) ||
        !readAt(cam, 0x120, in.inverseView.m) || !readAt(cam, 0x150, in.position) || !readAt(base, 0xc0, in.engineFar)) {
        return;
    }
    d.setMainCamera(in);
}

// A bCString field (SharedBase: a char* whose int length sits 8 bytes before the text) -> its text ("" when unreadable)
std::string readCString(const void* field) {
    const char* text = nullptr;
    if (!readAt(field, 0, text) || text == nullptr) return {};
    std::int32_t length = 0;
    if (!safeRead(text - 8, &length, sizeof(length))) length = 0;  // unknown: up to the terminator
    char buf[256]{};
    const std::size_t n = length > 0 ? std::min<std::size_t>(static_cast<std::size_t>(length), sizeof(buf) - 1) : sizeof(buf) - 1;
    if (!safeRead(text, buf, n)) {
        // the buffer may end before 255 bytes: read byte by byte up to the terminator
        std::size_t i = 0;
        for (; i < sizeof(buf) - 1; ++i) {
            if (!safeRead(text + i, &buf[i], 1) || buf[i] == 0) break;
        }
        buf[i] = 0;
    }
    buf[n] = 0;
    return std::string(buf, strnlen(buf, n));
}

// Material names (docs/research/water.md 2): eCMaterialResource2 derives from eCResource2, whose +0x40 bCString is the
// resource's file title (eCResource2::GetFileTitleA 0xa02e0 copies it); eCMeshResource2 likewise. Cached per material
// (render thread); re-read when the string buffer changes (a freed material's address reused by another).
struct MaterialInfo {
    const char* text = nullptr;
    std::string name;
    draws::Water water = draws::Water::None;
};
std::unordered_map<const void*, MaterialInfo> g_materials;

const MaterialInfo* materialInfo(const void* material, const void* meshResource, Pass pass) {
    const char* text = nullptr;
    if (material == nullptr || !readAt(material, 0x40, text)) return nullptr;
    auto [it, inserted] = g_materials.try_emplace(material);
    MaterialInfo& e = it->second;
    if (!inserted && e.text == text) return &e;
    e.text = text;
    e.name = readCString(static_cast<const std::uint8_t*>(material) + 0x40);
    e.water = draws::classifyWater(e.name);
    const bool interesting = e.water != draws::Water::None || pass == Pass::Refracted;
    if (interesting || spdlog::should_log(spdlog::level::debug)) {
        const std::string mesh = meshResource != nullptr ? readCString(static_cast<const std::uint8_t*>(meshResource) + 0x40) : "";
        spdlog::log(interesting ? spdlog::level::info : spdlog::level::debug, "water: material {} '{}' (mesh '{}') in the {} pass -> {}",
                    material, e.name, mesh, d3d9::scene::name(pass), draws::name(e.water));
    }
    return &e;
}

d3d9::Texture* textureOf(d3d9::Surface* s, d3d9::Device& d) {
    if (s == nullptr || s == d.backBuffer() || s->texture() == nullptr || s->texture()->isNull()) return nullptr;
    return s->texture();
}

// ---- detours ------------------------------------------------------------------------------------------------------------

using ExecuteFn = void (*)(void* db, void* desc);
Hook<ExecuteFn> g_execute;
void detourExecute(void* db, void* desc) {
    const View v = g_inRenderEntity > 0 ? View::Preview : (g_inSceneFilter > 0 || g_inCameraRender > 0) ? View::Main : View::Other;
    ViewScope scope(v, Pass::Setup, "eCFrustumDatabase::Execute");
    if (scope.d != nullptr && v == View::Main) {
        scope.d->beginMainView();
        readMainCamera(*scope.d, db);
        // the image effect's scene target is bound when its eCImageFilterScene calls Execute
        if (g_inSceneFilter > 0) {
            if (d3d9::Texture* t = textureOf(scope.d->renderTarget(0), *scope.d)) scope.d->tagImage(t, ImageRole::SceneHdr);
        }
    }
    g_execute.original(db, desc);
    // injection point (d3d9/inject.h): the scene is complete, the post chain has not run
    if (scope.d != nullptr && v == View::Main) {
        scope.d->inject(d3d9::inject::Point::AfterMainView);
    }
}

using ExecuteShadowMapFn = void (*)(void* db, void* shadowMap);
Hook<ExecuteShadowMapFn> g_executeShadowMap;
void detourExecuteShadowMap(void* db, void* shadowMap) {
    ViewScope scope(View::Shadow, Pass::ShadowDepth, "eCFrustumDatabase::ExecuteShadowMap");
    if (scope.d != nullptr) {
        // the shadow map's targets are bound before (eCShadowMapBase::RenderShadowMap, vtable +0x120): a colour map (R16F /
        // R32F, cubes for point lights) or the NULL colour target + a sampled DF24 / DF16 depth texture (sun cascades)
        if (d3d9::Texture* t = textureOf(scope.d->renderTarget(0), *scope.d)) {
            scope.d->tagImage(t, t->desc().type == d3d9::TextureType::Cube ? ImageRole::ShadowCube : ImageRole::ShadowMap);
        }
        if (d3d9::Surface* ds = scope.d->depthStencil(); ds != nullptr && ds->texture() != nullptr) {
            const std::uint32_t f = ds->texture()->desc().format;
            if (f == vk::d3dfmt::DF24 || f == vk::d3dfmt::DF16) scope.d->tagImage(ds->texture(), ImageRole::ShadowMap);
        }
    }
    g_executeShadowMap.original(db, shadowMap);
}

using BeginZPassFn = bool (*)(void* admin, void* depthMap, bool a, bool b);
Hook<BeginZPassFn> g_beginZPass;
bool detourBeginZPass(void* admin, void* depthMap, bool a, bool b) {
    if (d3d9::Device* d = device()) {
        if (d->phases().view() != View::Shadow) {
            d->phases().setPass(Pass::DepthPrepass);
            logPhase(*d, "BeginZPass");
            // eCFrustumDatabase::Execute binds the depth map (the caller's eSRenderTargetDesc +8) before BeginZPass(nullptr)
            if (d3d9::Texture* t = textureOf(d->renderTarget(0), *d)) d->tagImage(t, ImageRole::DepthMap);
        }
    }
    return g_beginZPass.original(admin, depthMap, a, b);
}

using EndZPassFn = bool (*)(void* admin);
Hook<EndZPassFn> g_endZPass;
bool detourEndZPass(void* admin) {
    const bool r = g_endZPass.original(admin);
    if (d3d9::Device* d = device(); d != nullptr && d->phases().pass() == Pass::DepthPrepass) {
        d->phases().setPass(Pass::Setup);
        logPhase(*d, "EndZPass");
        d->inject(d3d9::inject::Point::AfterDepthPrepass);
    }
    return r;
}

using VoidThisFn = void (*)(void* self);
Hook<VoidThisFn> g_updateShadowMask;
void detourUpdateShadowMask(void* self) {
    PassScope scope(Pass::ShadowMask, "UpdateShadowMask");
    g_updateShadowMask.original(self);
}

Hook<VoidThisFn> g_beginRenderSolid;
void detourBeginRenderSolid(void* self) {
    setPass(Pass::Solid, "DoBeginRenderSolid");
    g_beginRenderSolid.original(self);
}

Hook<VoidThisFn> g_beginRenderAlpha;
void detourBeginRenderAlpha(void* self) {
    // the end of the solid pass when no refraction copy marked it (d3d9/inject.h AfterOpaque: once per main view)
    if (d3d9::Device* d = device(); d != nullptr && d->phases().view() == View::Main) {
        d->inject(d3d9::inject::Point::AfterOpaque);
    }
    setPass(Pass::Alpha, "DoBeginRenderAlpha");
    g_beginRenderAlpha.original(self);
}

Hook<VoidThisFn> g_endRenderAlpha;
void detourEndRenderAlpha(void* self) {
    g_endRenderAlpha.original(self);
    setPass(Pass::AlphaLate, "DoEndRenderAlpha");
}

using SkyCubeFn = bool (*)(void* sky, void* image);
Hook<SkyCubeFn> g_skyCube;
bool detourSkyCube(void* sky, void* image) {
    ViewScope scope(View::SkyCube, Pass::Sky, "UpdateSkydomeEnvCubemap");
    return g_skyCube.original(sky, image);
}

using SceneUpdateFn = bool (*)(void* filter, int index);
Hook<SceneUpdateFn> g_sceneUpdate;
bool detourSceneUpdate(void* filter, int index) {
    ++g_inSceneFilter;
    const bool r = g_sceneUpdate.original(filter, index);
    --g_inSceneFilter;
    return r;
}

using EffectExecuteFn = bool (*)(void* effect);
Hook<EffectExecuteFn> g_effectExecute;
bool detourEffectExecute(void* effect) {
    ViewScope scope(View::Post, Pass::Post, "eCImageEffect::Execute");
    return g_effectExecute.original(effect);
}

using CameraRenderFn = void (*)(void* camera, void* desc);
Hook<CameraRenderFn> g_cameraRender;
void detourCameraRender(void* camera, void* desc) {
    ++g_inCameraRender;
    g_cameraRender.original(camera, desc);
    --g_inCameraRender;
}

using RenderEntityFn = void (*)(void* admin, void* db, void* entity);
Hook<RenderEntityFn> g_renderEntity;
void detourRenderEntity(void* admin, void* db, void* entity) {
    ++g_inRenderEntity;
    g_renderEntity.original(admin, db, entity);
    --g_inRenderEntity;
}

// ---- draw tags (docs/research/draw-tags.md) ---------------------------------------------------------------------------------

using draws::Source;

// the draw tag around an original call: the previous tag (an enclosing scope) comes back afterwards
struct TagScope {
    d3d9::Device* d;
    draws::Tag saved;
    // a new source (render context / property set / admin). entityAt: offset of the entity pointer in `object` (render
    // contexts and property sets keep it at +0x10), 0 = none. The engine passes live objects and reads the same fields itself.
    TagScope(Source source, const void* object, const void* op, std::size_t entityAt) : d(device()) {
        if (d == nullptr) return;
        draws::Tag& t = d->drawTag();
        saved = t;
        t = {};
        t.source = source;
        t.context = object;
        if (op != nullptr) t.opDesc = *reinterpret_cast<const std::uint32_t*>(static_cast<const std::uint8_t*>(op) + 8);
        if (entityAt != 0 && object != nullptr) {
            t.entity = *reinterpret_cast<const void* const*>(static_cast<const std::uint8_t*>(object) + entityAt);
        }
    }
    // a sub-mesh inside the current source (keeps source, context, entity); sub-mesh +0x38 = its eCMaterialResource2
    TagScope(const void* subMesh, const void* meshResource) : d(device()) {
        if (d == nullptr) return;
        draws::Tag& t = d->drawTag();
        saved = t;
        t.subMesh = subMesh;
        t.meshResource = meshResource;
        t.material = subMesh != nullptr ? *reinterpret_cast<const void* const*>(static_cast<const std::uint8_t*>(subMesh) + 0x38)
                                        : nullptr;
        // water classification by material name: only where water is drawn (the refracted list; alpha for non-refracting
        // water such as falls), main-like views
        const auto ph = d->phases().current();
        if (t.material != nullptr && (ph.pass == Pass::Refracted || ph.pass == Pass::Alpha || ph.pass == Pass::AlphaLate) &&
            ph.view != View::Shadow) {
            if (const MaterialInfo* m = materialInfo(t.material, meshResource, ph.pass)) {
                t.water = m->water;
                t.materialName = m->name.c_str();
            }
        } else if (t.material != nullptr && ph.pass == Pass::Solid && ph.view == View::Main &&
                   spdlog::should_log(spdlog::level::trace)) {
            // survey only (-RvkLogLevel trace): the opaque materials too (is there water drawn as an opaque surface?)
            materialInfo(t.material, meshResource, ph.pass);
        }
    }
    ~TagScope() {
        if (d != nullptr) d->drawTag() = saved;
    }
    TagScope(const TagScope&) = delete;
    TagScope& operator=(const TagScope&) = delete;
};

// Render-context vtable slots (eCRenderContext: 0 Render(op, db), 1 RenderAlpha, 2 RenderRefracted, 3 RenderZPass). The
// frustum database's loops call them indirectly; several are 16-byte tail-jump thunks (never detoured), so the slots of the
// classes' own vtables are patched (the "own vtables" rule of CLAUDE.md concerns the eCDX9* classes only). A slot is patched
// only when it holds the class's exported function.
using RenderOpFn = void (*)(void* context, const void* op, void* db);

struct ContextSlot {
    void* Symbols::*vtable;
    int slot;
    void* Symbols::*function;
    Source source;
    Pass pass;  // Pass::None = keep the current pass; Refracted: the Refracted pass around the call (its phase marker)
    const char* name;
};

constexpr ContextSlot kContextSlots[] = {
    {&Symbols::renderContextMeshVftable, 0, &Symbols::rcMeshRender, Source::Mesh, Pass::None, "eCRenderContextMesh::Render"},
    {&Symbols::renderContextMeshVftable, 1, &Symbols::rcMeshRenderAlpha, Source::Mesh, Pass::None,
     "eCRenderContextMesh::RenderAlpha"},
    {&Symbols::renderContextMeshVftable, 2, &Symbols::renderContextMeshRefracted, Source::Mesh, Pass::Refracted,
     "eCRenderContextMesh::RenderRefracted"},
    {&Symbols::renderContextMeshVftable, 3, &Symbols::rcMeshRenderZPass, Source::Mesh, Pass::None,
     "eCRenderContextMesh::RenderZPass"},
    {&Symbols::rcAnimVftable, 0, &Symbols::rcAnimRender, Source::Skinned, Pass::None, "eCRenderContextAnimation2::Render"},
    {&Symbols::rcAnimVftable, 1, &Symbols::rcAnimRenderAlpha, Source::Skinned, Pass::None,
     "eCRenderContextAnimation2::RenderAlpha"},
    {&Symbols::rcAnimVftable, 3, &Symbols::rcAnimRenderZPass, Source::Skinned, Pass::None,
     "eCRenderContextAnimation2::RenderZPass"},
    {&Symbols::rcTreeVftable, 0, &Symbols::rcTreeRender, Source::SpeedTree, Pass::None, "eCRenderContextSpeedTree::Render"},
    {&Symbols::rcTreeVftable, 3, &Symbols::rcTreeRenderZPass, Source::SpeedTree, Pass::None,
     "eCRenderContextSpeedTree::RenderZPass"},
    {&Symbols::rcTreeBBVftable, 0, &Symbols::rcTreeBBRender, Source::SpeedTreeBillboard, Pass::None,
     "eCRenderContextSpeedTreeBB::Render"},
    {&Symbols::rcTreeBBVftable, 3, &Symbols::rcTreeBBRenderZPass, Source::SpeedTreeBillboard, Pass::None,
     "eCRenderContextSpeedTreeBB::RenderZPass"},
    {&Symbols::rcVegVftable, 0, &Symbols::rcVegRender, Source::Vegetation, Pass::None, "eCRenderContextVegetation::Render"},
    {&Symbols::rcVegVftable, 3, &Symbols::rcVegRenderZPass, Source::Vegetation, Pass::None,
     "eCRenderContextVegetation::RenderZPass"},
    {&Symbols::rcDebugVftable, 0, &Symbols::rcDebugRender, Source::Debug, Pass::None, "eCRenderContextDebug::Render"},
};
constexpr std::size_t kSlotCount = std::size(kContextSlots);
std::array<RenderOpFn, kSlotCount> g_slotOriginal{};
bool g_slotsPatched = false;

template <std::size_t I>
void contextSlot(void* context, const void* op, void* db) {
    constexpr const ContextSlot& e = kContextSlots[I];
    TagScope tag(e.source, context, op, 0x10);
    if constexpr (e.pass != Pass::None) {
        PassScope pass(e.pass, e.name);
        g_slotOriginal[I](context, op, db);
    } else {
        g_slotOriginal[I](context, op, db);
    }
}

template <std::size_t... I>
constexpr std::array<RenderOpFn, sizeof...(I)> makeSlotThunks(std::index_sequence<I...>) {
    return {&contextSlot<I>...};
}
constexpr std::array<RenderOpFn, kSlotCount> kSlotThunks = makeSlotThunks(std::make_index_sequence<kSlotCount>{});

void patchContextSlots() {
    if (g_slotsPatched) return;
    g_slotsPatched = true;  // one attempt (the vtables are process-wide)
    const auto& s = sym();
    int patched = 0;
    for (std::size_t i = 0; i < kSlotCount; ++i) {
        const ContextSlot& e = kContextSlots[i];
        void* vtable = s.*e.vtable;
        void* expected = s.*e.function;
        if (vtable == nullptr || expected == nullptr) {
            spdlog::warn("scene: {} not found: its draws stay untagged", e.name);
            continue;
        }
        void** slot = static_cast<void**>(vtable) + e.slot;
        void* current = nullptr;
        if (!safeRead(slot, &current, sizeof(current)) || current != expected) {
            spdlog::warn("scene: vtable slot {} of {} is {} (expected {}): not patched", e.slot, e.name, current, expected);
            continue;
        }
        DWORD old = 0;
        if (!VirtualProtect(slot, sizeof(void*), PAGE_READWRITE, &old)) {
            spdlog::warn("scene: VirtualProtect on the vtable of {} failed ({})", e.name, GetLastError());
            continue;
        }
        g_slotOriginal[i] = reinterpret_cast<RenderOpFn>(current);
        *slot = reinterpret_cast<void*>(kSlotThunks[i]);
        DWORD ignored = 0;
        VirtualProtect(slot, sizeof(void*), old, &ignored);
        FlushInstructionCache(GetCurrentProcess(), slot, sizeof(void*));
        ++patched;
    }
    spdlog::info("scene: {} of {} render-context vtable slots patched (draw tags; eCRenderContextMesh::RenderRefracted marks the "
                 "Refracted pass)",
                 patched, kSlotCount);
}

// Contexts whose class exports no vtable (eCPrecipitationRenderContext, the sky dome's context): the property set's exported
// CreateRenderContext is detoured, and the vtable of the first context it returns gets its non-empty slots 0..3 patched (once
// per vtable; contexts are created while the world loads, on any thread).
struct DynamicSlot {
    std::atomic<RenderOpFn> original{nullptr};
    Source source = Source::None;
};
constexpr std::size_t kDynamicSlots = 8;
std::array<DynamicSlot, kDynamicSlots> g_dynamic;
std::mutex g_dynamicMutex;
std::size_t g_dynamicUsed = 0;
std::array<const void*, 4> g_dynamicVtables{};  // patched vtables
std::size_t g_dynamicVtableCount = 0;

template <std::size_t I>
void dynamicSlot(void* context, const void* op, void* db) {
    const DynamicSlot& e = g_dynamic[I];
    TagScope tag(e.source, context, op, 0x10);
    e.original.load(std::memory_order_acquire)(context, op, db);
}

template <std::size_t... I>
constexpr std::array<RenderOpFn, sizeof...(I)> makeDynamicThunks(std::index_sequence<I...>) {
    return {&dynamicSlot<I>...};
}
constexpr std::array<RenderOpFn, kDynamicSlots> kDynamicThunks = makeDynamicThunks(std::make_index_sequence<kDynamicSlots>{});

void patchContextVtable(const void* context, Source source, const char* what) {
    void* vtable = nullptr;
    if (context == nullptr || !safeRead(context, &vtable, sizeof(vtable)) || vtable == nullptr) return;
    const std::lock_guard lock(g_dynamicMutex);
    for (std::size_t i = 0; i < g_dynamicVtableCount; ++i) {
        if (g_dynamicVtables[i] == vtable) return;
    }
    if (g_dynamicVtableCount >= g_dynamicVtables.size()) return;
    g_dynamicVtables[g_dynamicVtableCount++] = vtable;
    void* const empty = sym().rcBaseRender;
    int patched = 0;
    for (int s = 0; s < 4; ++s) {
        void** slot = static_cast<void**>(vtable) + s;
        void* current = nullptr;
        if (!safeRead(slot, &current, sizeof(current)) || current == nullptr || current == empty) continue;
        if (g_dynamicUsed >= kDynamicSlots) break;
        DWORD old = 0;
        if (!VirtualProtect(slot, sizeof(void*), PAGE_READWRITE, &old)) continue;
        const std::size_t i = g_dynamicUsed++;
        g_dynamic[i].source = source;
        g_dynamic[i].original.store(reinterpret_cast<RenderOpFn>(current), std::memory_order_release);
        *slot = reinterpret_cast<void*>(kDynamicThunks[i]);
        DWORD ignored = 0;
        VirtualProtect(slot, sizeof(void*), old, &ignored);
        FlushInstructionCache(GetCurrentProcess(), slot, sizeof(void*));
        ++patched;
    }
    spdlog::info("scene: {} context vtable {}: {} render slots patched (draw tags: {})", what, vtable, patched, draws::name(source));
}

using CreateContextFn = void* (*)(void* propertySet);
Hook<CreateContextFn> g_precipCreateContext;
void* detourPrecipCreateContext(void* ps) {
    void* context = g_precipCreateContext.original(ps);
    if (sym().rcBaseRender != nullptr) patchContextVtable(context, Source::Precipitation, "eCPrecipitationRenderContext");
    return context;
}

Hook<CreateContextFn> g_skydomeCreateContext;
void* detourSkydomeCreateContext(void* ps) {
    void* context = g_skydomeCreateContext.original(ps);
    if (sym().rcBaseRender != nullptr) patchContextVtable(context, Source::Sky, "eCSkydome_PS");
    return context;
}

// bool eCSubMesh::Render(eCMeshResource2 const*, ulong node, int, eEShaderMaterialLightmapCombi, eSShaderMaterialIllumination*,
// float const* alpha, bCMatrix const* world, bool, uchar)
using SubMeshRenderFn = bool (*)(void* self, const void* mesh, std::uint32_t node, int lod, int lightmapCombi, void* illumination,
                                 const float* alpha, const void* world, bool flag, std::uint8_t flags);
Hook<SubMeshRenderFn> g_subMeshRender;
bool detourSubMeshRender(void* self, const void* mesh, std::uint32_t node, int lod, int lightmapCombi, void* illumination,
                         const float* alpha, const void* world, bool flag, std::uint8_t flags) {
    TagScope tag(self, mesh);
    return g_subMeshRender.original(self, mesh, node, lod, lightmapCombi, illumination, alpha, world, flag, flags);
}

Hook<VoidThisFn> g_skydomeRender;
void detourSkydomeRender(void* self) {
    TagScope tag(Source::Sky, self, nullptr, 0x10);
    g_skydomeRender.original(self);
}

using RenderFloatFn = void (*)(void* self, float t);
Hook<RenderFloatFn> g_particleRender;
void detourParticleRender(void* self, float t) {
    TagScope tag(Source::Particle, self, nullptr, 0x10);
    g_particleRender.original(self, t);
}

Hook<RenderFloatFn> g_stripRender;
void detourStripRender(void* self, float t) {
    TagScope tag(Source::Strip, self, nullptr, 0x10);
    g_stripRender.original(self, t);
}

using DecalsFn = bool (*)(void* admin, void* db);
Hook<DecalsFn> g_decalsRender;
bool detourDecalsRender(void* admin, void* db) {
    TagScope tag(Source::Decal, admin, nullptr, 0);
    return g_decalsRender.original(admin, db);
}

using BillboardsFn = bool (*)(void* admin);
Hook<BillboardsFn> g_billboardsRender;
bool detourBillboardsRender(void* admin) {
    TagScope tag(Source::Billboard, admin, nullptr, 0);
    return g_billboardsRender.original(admin);
}

}  // namespace

int install() {
    const auto& s = sym();
    int n = 0;
    const auto add = [&](auto& hook, const char* name, void* target, auto detour) {
        using Fn = decltype(hook.original);
        if (target == nullptr) {
            spdlog::warn("scene: {} not found: its phase marker is missing", name);
            return;
        }
        if (hook.install(name, reinterpret_cast<Fn>(target), detour)) ++n;
    };
    add(g_execute, "eCFrustumDatabase::Execute", s.frustumDbExecute, &detourExecute);
    add(g_executeShadowMap, "eCFrustumDatabase::ExecuteShadowMap", s.frustumDbExecuteShadowMap, &detourExecuteShadowMap);
    add(g_beginZPass, "eCShaderMaterialAdmin::BeginZPass", s.materialBeginZPass, &detourBeginZPass);
    add(g_endZPass, "eCShaderMaterialAdmin::EndZPass", s.materialEndZPass, &detourEndZPass);
    add(g_updateShadowMask, "eCIlluminationAdmin::UpdateShadowMask", s.illumUpdateShadowMask, &detourUpdateShadowMask);
    add(g_beginRenderSolid, "eCModuleAdmin::DoBeginRenderSolid", s.moduleBeginRenderSolid, &detourBeginRenderSolid);
    add(g_beginRenderAlpha, "eCModuleAdmin::DoBeginRenderAlpha", s.moduleBeginRenderAlpha, &detourBeginRenderAlpha);
    add(g_endRenderAlpha, "eCModuleAdmin::DoEndRenderAlpha", s.moduleEndRenderAlpha, &detourEndRenderAlpha);
    add(g_skyCube, "eCSkydome_PS::UpdateSkydomeEnvCubemap", s.skydomeUpdateEnvCubemap, &detourSkyCube);
    add(g_sceneUpdate, "eCImageFilterScene::OnUpdateOutput", s.imageFilterSceneUpdate, &detourSceneUpdate);
    add(g_effectExecute, "eCImageEffect::Execute", s.imageEffectExecute, &detourEffectExecute);
    add(g_cameraRender, "eCCameraBase::Render", s.cameraBaseRender, &detourCameraRender);
    add(g_renderEntity, "eCRenderSystemAdmin::RenderEntity", s.renderSystemRenderEntity, &detourRenderEntity);
    // draw tags
    add(g_subMeshRender, "eCSubMesh::Render", s.subMeshRender, &detourSubMeshRender);
    add(g_skydomeRender, "eCSkydome_PS::Render", s.skydomeRender, &detourSkydomeRender);
    add(g_particleRender, "eCParticle_PS::Render", s.particleRender, &detourParticleRender);
    add(g_stripRender, "eCStrip_PS::Render", s.stripRender, &detourStripRender);
    add(g_decalsRender, "eCDecalAdmin::RenderDecals", s.decalsRender, &detourDecalsRender);
    add(g_billboardsRender, "eCBillboardAdmin::RenderBillboards", s.billboardsRender, &detourBillboardsRender);
    add(g_precipCreateContext, "eCPrecipitation_PS::CreateRenderContext", s.precipCreateContext, &detourPrecipCreateContext);
    add(g_skydomeCreateContext, "eCSkydome_PS::CreateRenderContext", s.skydomeCreateContext, &detourSkydomeCreateContext);
    spdlog::info("scene: {} phase-marker / draw-tag detours created", n);
    return n;
}

void attach(d3d9::Device* device) {
    g_device.store(device, std::memory_order_release);
    if (device != nullptr && g_execute.installed()) patchContextSlots();
}

void detach(d3d9::Device* device) {
    d3d9::Device* expected = device;
    g_device.compare_exchange_strong(expected, nullptr, std::memory_order_acq_rel);
}

void appDrawEnter() {
    if (d3d9::Device* d = device()) {
        d->phases().push(View::Gui, Pass::Gui);
        logPhase(*d, "eCApplication::Draw");
        d->inject(d3d9::inject::Point::BeforeGui);
    }
}

void appDrawLeave() {
    // SwapScreen (inside Draw) ended the frame; the Gui scope it left open is closed here, in the next frame's sequence
    if (d3d9::Device* d = device(); d != nullptr && d->phases().view() == View::Gui) {
        d->phases().pop();
    }
}

const char* currentLabel() {
    static const auto labels = [] {
        std::array<std::array<std::string, static_cast<std::size_t>(Pass::Count)>, static_cast<std::size_t>(View::Count)> t;
        for (std::size_t v = 0; v < t.size(); ++v)
            for (std::size_t p = 0; p < t[v].size(); ++p)
                t[v][p] = std::string(d3d9::scene::name(static_cast<View>(v))) + "/" + d3d9::scene::name(static_cast<Pass>(p));
        return t;
    }();
    const d3d9::Device* d = device();
    if (d == nullptr) return "-";
    const auto p = d->phases().current();
    return labels[static_cast<std::size_t>(p.view)][static_cast<std::size_t>(p.pass)].c_str();
}

}  // namespace rvk::engine::scene
