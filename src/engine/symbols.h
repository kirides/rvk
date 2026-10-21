#pragma once

// Engine.dll functions and data we bind by decorated export name (docs/research/engine-gfx.md; RVAs are for orientation).
// X(field, module, decorated name, required)

#define RVK_ENGINE_SYMBOLS(X)                                                                                                   \
    /* void eCGfxAdmin::InitializeAdmin(eCGfxShared::eEAPIType) (0x74e510): creates eCDX9Instance, Direct3DCreate9 */           \
    X(gfxAdminInitializeAdmin, L"Engine.dll", "?InitializeAdmin@eCGfxAdmin@@IEAAXW4eEAPIType@eCGfxShared@@@Z", true)            \
    /* static bEResult eCGfxAdmin::sEnumAdapterDesc(unsigned long, eSGfxAdapterDesc&) (0x74eb80): the pixel shader 3.0 check  \
       of eCApplication::CreateEngine, before the window; Direct3DCreate9 + DxDiag while ms_pAPIInstance is null */             \
    X(gfxAdminEnumAdapterDesc, L"Engine.dll",                                                                                   \
      "?sEnumAdapterDesc@eCGfxAdmin@@SA?AW4bEResult@@KAEAUeSGfxAdapterDesc@eCGfxShared@@@Z", false)                             \
    /* eCAPIInstance* eCGfxAdmin::ms_pAPIInstance (0xc62ad8) */                                                                 \
    X(gfxAdminApiInstance, L"Engine.dll", "?ms_pAPIInstance@eCGfxAdmin@@1PEAVeCAPIInstance@@EA", true)                          \
    /* eCAPIDevice* eCGfxContextAdmin::ms_pAPIDevice (0xc62750): the single global device pointer */                           \
    X(gfxContextAdminApiDevice, L"Engine.dll", "?ms_pAPIDevice@eCGfxContextAdmin@@1PEAVeCAPIDevice@@EA", true)              \
    /* snapshots (missing-slots.md 3, engine/vfs.h). eCVirtualFileSystem& eCVirtualFileSystem::GetInstance() (0x736450) */    \
    X(vfsGetInstance, L"Engine.dll", "?GetInstance@eCVirtualFileSystem@@SAAEAV1@XZ", false)                                     \
    /* bool eCVirtualFileSystem::MakeAbsPhysicalPath(bCString&) (0x733990) */                                                  \
    X(vfsMakeAbsPhysicalPath, L"Engine.dll", "?MakeAbsPhysicalPath@eCVirtualFileSystem@@QEAA_NAEAVbCString@@@Z", false)         \
    /* eCApplication& eCApplication::GetInstance() (0x4c64e0) */                                                               \
    X(appGetInstance, L"Engine.dll", "?GetInstance@eCApplication@@SAAEAV1@XZ", false)                                           \
    /* void eCApplication::Snapshot() (0x4bfc90): the debug snapshot key's action */                                           \
    X(appSnapshot, L"Engine.dll", "?Snapshot@eCApplication@@QEAAXXZ", false)                                                    \
    /* bCString const& eCApplication::GetLastSnapshotName() const (0x4c4040) */                                                \
    X(appGetLastSnapshotName, L"Engine.dll", "?GetLastSnapshotName@eCApplication@@QEBAAEBVbCString@@XZ", false)                 \
    /* virtual void eCApplication::Draw(HWND) (0x4c13b0): GUI, DoSnapshot, SwapScreen; hooked for [debug] snapshot_frames   \
       and the Gui phase marker */                                                                                             \
    X(appDraw, L"Engine.dll", "?Draw@eCApplication@@UEAAXPEAUHWND__@@@Z", false)                                                \
    /* ---- frame phase markers (engine/scene_hooks.*, docs/research/frame-phases.md; all optional) ---- */                     \
    /* void eCFrustumDatabase::Execute(eSRenderTargetDesc*) (0x66e990): one camera view (z pre-pass, shadow mask, solid,     \
       refraction copy, refracted, alpha) */                                                                                   \
    X(frustumDbExecute, L"Engine.dll", "?Execute@eCFrustumDatabase@@QEAAXPEAUeSRenderTargetDesc@1@@Z", false)                    \
    /* void eCFrustumDatabase::ExecuteShadowMap(eCShadowMapBase*) (0x66f630): a shadow map's z pass */                          \
    X(frustumDbExecuteShadowMap, L"Engine.dll", "?ExecuteShadowMap@eCFrustumDatabase@@QEAAXPEAVeCShadowMapBase@@@Z", false)      \
    /* bool eCShaderMaterialAdmin::BeginZPass(eCDepthMap*, bool, bool) (0x6b9720), EndZPass() (0x6b9910) */                   \
    X(materialBeginZPass, L"Engine.dll", "?BeginZPass@eCShaderMaterialAdmin@@QEAA_NPEAVeCDepthMap@@_N1@Z", false)                \
    X(materialEndZPass, L"Engine.dll", "?EndZPass@eCShaderMaterialAdmin@@QEAA_NXZ", false)                                      \
    /* void eCIlluminationAdmin::UpdateShadowMask() (0x5748d0): shadow maps + the screen-space shadow mask */                  \
    X(illumUpdateShadowMask, L"Engine.dll", "?UpdateShadowMask@eCIlluminationAdmin@@QEAAXXZ", false)                            \
    /* void eCModuleAdmin::DoBeginRenderSolid() (0x4cb810), DoBeginRenderAlpha() (0x4cb710), DoEndRenderAlpha() (0x4cb680) */ \
    X(moduleBeginRenderSolid, L"Engine.dll", "?DoBeginRenderSolid@eCModuleAdmin@@QEAAXXZ", false)                               \
    X(moduleBeginRenderAlpha, L"Engine.dll", "?DoBeginRenderAlpha@eCModuleAdmin@@QEAAXXZ", false)                               \
    X(moduleEndRenderAlpha, L"Engine.dll", "?DoEndRenderAlpha@eCModuleAdmin@@QEAAXXZ", false)                                   \
    /* bool eCSkydome_PS::UpdateSkydomeEnvCubemap(eCGfxImageData*) (0x6e7ed0): the sky environment cube */                    \
    X(skydomeUpdateEnvCubemap, L"Engine.dll", "?UpdateSkydomeEnvCubemap@eCSkydome_PS@@QEAA_NPEAVeCGfxImageData@@@Z", false)     \
    /* virtual bool eCImageFilterScene::OnUpdateOutput(int) (0x75cf10): the image effect's scene render (main camera) */       \
    X(imageFilterSceneUpdate, L"Engine.dll", "?OnUpdateOutput@eCImageFilterScene@@MEAA_NH@Z", false)                            \
    /* bool eCImageEffect::Execute() (0x662630): an image-effect chain (DoRender: the scene effect, then the overlay one) */   \
    X(imageEffectExecute, L"Engine.dll", "?Execute@eCImageEffect@@QEAA_NXZ", false)                                             \
    /* void eCCameraBase::Render(eSRenderTargetDesc*) (0x4f8470): the main view without an image effect */                     \
    X(cameraBaseRender, L"Engine.dll", "?Render@eCCameraBase@@QEAAXPEAUeSRenderTargetDesc@eCFrustumDatabase@@@Z", false)         \
    /* void eCRenderSystemAdmin::RenderEntity(eCFrustumDatabase&, eCGeometryEntity*) (0x66ceb0): entity previews */            \
    X(renderSystemRenderEntity, L"Engine.dll",                                                                                  \
      "?RenderEntity@eCRenderSystemAdmin@@QEAAXAEAVeCFrustumDatabase@@PEAVeCGeometryEntity@@@Z", false)                         \
    /* eCRenderContextMesh's vtable (0x9ae070); slot 2 (+0x10) = RenderRefracted (0x5c7400, a 16-byte tail-jump thunk: the    \
       slot is patched, the function is not detoured) */                                                                       \
    X(renderContextMeshVftable, L"Engine.dll", "??_7eCRenderContextMesh@@6B@", false)                                           \
    X(renderContextMeshRefracted, L"Engine.dll",                                                                                \
      "?RenderRefracted@eCRenderContextMesh@@UEAAXAEBVeCRenderOp2@@AEAVeCFrustumDatabase@@@Z", false)                           \
    /* eCGfxCamera* eCGfxCamera::ms_pMasterCamera (0xc62590): eCGfxCamera::Execute renders through it when set */              \
    X(gfxCameraMaster, L"Engine.dll", "?ms_pMasterCamera@eCGfxCamera@@1PEAV1@EA", false)                                        \
    /* SharedBase.dll bCString(char const*), ~bCString(), char const* GetText() const */                                       \
    X(stringCtor, L"SharedBase.dll", "??0bCString@@QEAA@PEBD@Z", false)                                                         \
    X(stringDtor, L"SharedBase.dll", "??1bCString@@QEAA@XZ", false)                                                             \
    X(stringGetText, L"SharedBase.dll", "?GetText@bCString@@QEBAPEBDXZ", false)

namespace rvk::engine {

struct Symbols {
#define RVK_SYMBOL_FIELD(field, module, name, required) void* field = nullptr;
    RVK_ENGINE_SYMBOLS(RVK_SYMBOL_FIELD)
#undef RVK_SYMBOL_FIELD
};

// Resolves every symbol; returns false (and logs) when a required one is missing.
bool resolveSymbols();
const Symbols& sym();

}  // namespace rvk::engine
