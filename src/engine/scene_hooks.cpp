#include "engine/scene_hooks.h"

#include <windows.h>

#include <array>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <string>

#include <spdlog/spdlog.h>

#include "core/hooks.h"
#include "d3d9/device.h"
#include "engine/symbols.h"
#include "hal/vulkan/hal_vulkan.h"
#include "vk/formats.h"

namespace rvk::engine::scene {
namespace {

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
        readMainCamera(*scope.d, db);
        // the image effect's scene target is bound when its eCImageFilterScene calls Execute
        if (g_inSceneFilter > 0) {
            if (d3d9::Texture* t = textureOf(scope.d->renderTarget(0), *scope.d)) scope.d->tagImage(t, ImageRole::SceneHdr);
        }
    }
    g_execute.original(db, desc);
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

// eCRenderContextMesh::RenderRefracted(eCRenderOp2 const&, eCFrustumDatabase&): vtable slot 2
using RenderOpFn = void (*)(void* context, const void* op, void* db);
RenderOpFn g_refractedOriginal = nullptr;
bool g_refractedPatched = false;
void refractedSlot(void* context, const void* op, void* db) {
    PassScope scope(Pass::Refracted, "RenderRefracted");
    g_refractedOriginal(context, op, db);
}

void patchRefractedSlot() {
    if (g_refractedPatched) return;
    g_refractedPatched = true;  // one attempt
    const auto& s = sym();
    if (s.renderContextMeshVftable == nullptr || s.renderContextMeshRefracted == nullptr) {
        spdlog::warn("scene: eCRenderContextMesh vtable not found: no Refracted pass (water draws count as Alpha)");
        return;
    }
    void** slot = static_cast<void**>(s.renderContextMeshVftable) + 2;
    void* current = nullptr;
    if (!safeRead(slot, &current, sizeof(current)) || current != s.renderContextMeshRefracted) {
        spdlog::warn("scene: eCRenderContextMesh vtable slot 2 is {} (expected RenderRefracted {}): not patched", current,
                     s.renderContextMeshRefracted);
        return;
    }
    DWORD old = 0;
    if (!VirtualProtect(slot, sizeof(void*), PAGE_READWRITE, &old)) {
        spdlog::warn("scene: VirtualProtect on the eCRenderContextMesh vtable failed ({})", GetLastError());
        return;
    }
    g_refractedOriginal = reinterpret_cast<RenderOpFn>(current);
    *slot = reinterpret_cast<void*>(&refractedSlot);
    DWORD ignored = 0;
    VirtualProtect(slot, sizeof(void*), old, &ignored);
    FlushInstructionCache(GetCurrentProcess(), slot, sizeof(void*));
    spdlog::info("scene: eCRenderContextMesh::RenderRefracted vtable slot patched (Refracted pass)");
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
    spdlog::info("scene: {} phase-marker detours created", n);
    return n;
}

void attach(d3d9::Device* device) {
    g_device.store(device, std::memory_order_release);
    if (device != nullptr && g_execute.installed()) patchRefractedSlot();
}

void detach(d3d9::Device* device) {
    d3d9::Device* expected = device;
    g_device.compare_exchange_strong(expected, nullptr, std::memory_order_acq_rel);
}

void appDrawEnter() {
    if (d3d9::Device* d = device()) {
        d->phases().push(View::Gui, Pass::Gui);
        logPhase(*d, "eCApplication::Draw");
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
