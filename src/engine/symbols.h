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
    /* window modes (engine/window_mode.*, docs/research/window-mode.md). bool eCApplication::SetResolutionEx(adapter, w, h,   \
       refresh, vsync, float, eCWindow::eEWindowMode) (0x4c02e0): every mode change; hooked (fullscreen -> monitor size) */    \
    X(appSetResolutionEx, L"Engine.dll", "?SetResolutionEx@eCApplication@@QEAA_NKKKK_NMW4eEWindowMode@eCWindow@@@Z", false)     \
    /* bool eCApplication::SetResolution(w, h, eEWindowMode) (0x4c0430), bool eCApplication::ToggleFullscreen() (0x4c01f0) */ \
    X(appSetResolution, L"Engine.dll", "?SetResolution@eCApplication@@QEAA_NKKW4eEWindowMode@eCWindow@@@Z", false)              \
    X(appToggleFullscreen, L"Engine.dll", "?ToggleFullscreen@eCApplication@@QEAA_NXZ", false)                                   \
    /* bEResult eCDX9Device::SwapScreen(HWND, bCRect const*, bCRect const*) (0x7407b0): IDirect3DDevice9::Present +         \
       GetBackBuffer; hooked outside Vulkan mode for [debug] present_stalls (docs/research/fps-limit.md) */                   \
    X(dx9SwapScreen, L"Engine.dll", "?SwapScreen@eCDX9Device@@UEAA?AW4bEResult@@PEAUHWND__@@PEBVbCRect@@1@Z", false)           \
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
    /* ---- draw tags (engine/scene_hooks.*, docs/research/draw-tags.md; all optional) ---- */                                \
    /* render-context vtables: slot 0 Render(op, db), 1 RenderAlpha, 2 RenderRefracted, 3 RenderZPass; a slot is patched    \
       only when it holds the class's own exported function (the base eCRenderContext slots are one empty function) */      \
    X(rcMeshRender, L"Engine.dll", "?Render@eCRenderContextMesh@@UEAAXAEBVeCRenderOp2@@AEAVeCFrustumDatabase@@@Z", false)      \
    X(rcMeshRenderAlpha, L"Engine.dll",                                                                                         \
      "?RenderAlpha@eCRenderContextMesh@@UEAAXAEBVeCRenderOp2@@AEAVeCFrustumDatabase@@@Z", false)                               \
    X(rcMeshRenderZPass, L"Engine.dll",                                                                                         \
      "?RenderZPass@eCRenderContextMesh@@UEAAXAEBVeCRenderOp2@@AEAVeCFrustumDatabase@@@Z", false)                               \
    X(rcAnimVftable, L"Engine.dll", "??_7eCRenderContextAnimation2@@6B@", false)                                                \
    X(rcAnimRender, L"Engine.dll",                                                                                              \
      "?Render@eCRenderContextAnimation2@@UEAAXAEBVeCRenderOp2@@AEAVeCFrustumDatabase@@@Z", false)                              \
    X(rcAnimRenderAlpha, L"Engine.dll",                                                                                         \
      "?RenderAlpha@eCRenderContextAnimation2@@UEAAXAEBVeCRenderOp2@@AEAVeCFrustumDatabase@@@Z", false)                         \
    X(rcAnimRenderZPass, L"Engine.dll",                                                                                         \
      "?RenderZPass@eCRenderContextAnimation2@@UEAAXAEBVeCRenderOp2@@AEAVeCFrustumDatabase@@@Z", false)                         \
    X(rcTreeVftable, L"Engine.dll", "??_7eCRenderContextSpeedTree@@6B@", false)                                                 \
    X(rcTreeRender, L"Engine.dll",                                                                                              \
      "?Render@eCRenderContextSpeedTree@@UEAAXAEBVeCRenderOp2@@AEAVeCFrustumDatabase@@@Z", false)                               \
    X(rcTreeRenderZPass, L"Engine.dll",                                                                                         \
      "?RenderZPass@eCRenderContextSpeedTree@@UEAAXAEBVeCRenderOp2@@AEAVeCFrustumDatabase@@@Z", false)                          \
    X(rcTreeBBVftable, L"Engine.dll", "??_7eCRenderContextSpeedTreeBB@@6B@", false)                                             \
    X(rcTreeBBRender, L"Engine.dll",                                                                                            \
      "?Render@eCRenderContextSpeedTreeBB@@UEAAXAEBVeCRenderOp2@@AEAVeCFrustumDatabase@@@Z", false)                             \
    X(rcTreeBBRenderZPass, L"Engine.dll",                                                                                       \
      "?RenderZPass@eCRenderContextSpeedTreeBB@@UEAAXAEBVeCRenderOp2@@AEAVeCFrustumDatabase@@@Z", false)                        \
    X(rcVegVftable, L"Engine.dll", "??_7eCRenderContextVegetation@@6B@", false)                                                 \
    X(rcVegRender, L"Engine.dll",                                                                                               \
      "?Render@eCRenderContextVegetation@@UEAAXAEBVeCRenderOp2@@AEAVeCFrustumDatabase@@@Z", false)                              \
    X(rcVegRenderZPass, L"Engine.dll",                                                                                          \
      "?RenderZPass@eCRenderContextVegetation@@UEAAXAEBVeCRenderOp2@@AEAVeCFrustumDatabase@@@Z", false)                         \
    X(rcDebugVftable, L"Engine.dll", "??_7eCRenderContextDebug@@6B@", false)                                                    \
    X(rcDebugRender, L"Engine.dll",                                                                                             \
      "?Render@eCRenderContextDebug@@UEAAXAEBVeCRenderOp2@@AEAVeCFrustumDatabase@@@Z", false)                                   \
    /* contexts of classes without an exported vtable (eCPrecipitationRenderContext, the sky dome's): their vtable is taken    \
       from the first context the property set's CreateRenderContext returns; eCRenderContext::Render (0x7faa0) = the empty   \
       base slot (one COMDAT function for all four slots) */                                                                   \
    X(rcBaseRender, L"Engine.dll", "?Render@eCRenderContext@@UEAAXAEBVeCRenderOp2@@AEAVeCFrustumDatabase@@@Z", false)          \
    X(precipCreateContext, L"Engine.dll", "?CreateRenderContext@eCPrecipitation_PS@@UEAAPEAVeCRenderContext@@XZ", false)       \
    X(skydomeCreateContext, L"Engine.dll", "?CreateRenderContext@eCSkydome_PS@@UEAAPEAVeCRenderContext@@XZ", false)            \
    /* detoured render functions (normal prologues, unique RVAs, capstone-checked): bool eCSubMesh::Render(eCMeshResource2    \
       const*, ulong, int, eEShaderMaterialLightmapCombi, eSShaderMaterialIllumination*, float const*, bCMatrix const*, bool, \
       uchar) (0x5c7eb0) */                                                                                                    \
    X(subMeshRender, L"Engine.dll",                                                                                             \
      "?Render@eCSubMesh@@QEAA_NPEBVeCMeshResource2@@KHW4eEShaderMaterialLightmapCombi@@PEAUeSShaderMaterialIllumination@@"     \
      "PEBMPEBVbCMatrix@@_NE@Z", false)                                                                                        \
    /* void eCSkydome_PS::Render() (0x6e4730), void eCParticle_PS::Render(float) (0x5e0080), void eCStrip_PS::Render(float)   \
       (0x6feae0), bool eCDecalAdmin::RenderDecals(eCFrustumDatabase*) (0x4fc220), bool eCBillboardAdmin::RenderBillboards() \
       (0x4f0530) */                                                                                                           \
    X(skydomeRender, L"Engine.dll", "?Render@eCSkydome_PS@@QEAAXXZ", false)                                                     \
    X(particleRender, L"Engine.dll", "?Render@eCParticle_PS@@QEAAXM@Z", false)                                                  \
    X(stripRender, L"Engine.dll", "?Render@eCStrip_PS@@QEAAXM@Z", false)                                                        \
    X(decalsRender, L"Engine.dll", "?RenderDecals@eCDecalAdmin@@QEAA_NPEAVeCFrustumDatabase@@@Z", false)                        \
    X(billboardsRender, L"Engine.dll", "?RenderBillboards@eCBillboardAdmin@@QEAA_NXZ", false)                                   \
    /* eCGfxCamera* eCGfxCamera::ms_pMasterCamera (0xc62590): eCGfxCamera::Execute renders through it when set */              \
    X(gfxCameraMaster, L"Engine.dll", "?ms_pMasterCamera@eCGfxCamera@@1PEAV1@EA", false)                                        \
    /* SharedBase.dll bCString(char const*), ~bCString(), char const* GetText() const */                                       \
    X(stringCtor, L"SharedBase.dll", "??0bCString@@QEAA@PEBD@Z", false)                                                         \
    X(stringDtor, L"SharedBase.dll", "??1bCString@@QEAA@XZ", false)                                                             \
    X(stringGetText, L"SharedBase.dll", "?GetText@bCString@@QEBAPEBDXZ", false)                                                   \
    /* the weather (engine/weather.h, docs/research/ocean.md 6): eCModuleAdmin& eCModuleAdmin::GetInstance(),                     \
       eCEngineComponentBase* eCModuleAdmin::FindModule(bCString const&) const; the eCWeatherAdmin vtable (the module found       \
       by class name is checked against it); int eCWeatherAdmin::GetWeatherStatesCount() const, eCWeatherStates*                  \
       eCWeatherAdmin::GetWeatherStatesAt(int) const; bCString const& eCWeatherStates::GetName() const, float                     \
       eCWeatherStates::GetBlendFactor() const (the weight of the state in the current blend; risen-spass reads the same);        \
       bCVector const& eCWeatherAdmin::GetPlayerPosition() const (0x713ea0: the player's position the game hands the weather      \
       for its zones; the FFT ocean's near-water wave cap follows it) */                                                          \
    X(moduleAdminGetInstance, L"Engine.dll", "?GetInstance@eCModuleAdmin@@SAAEAV1@XZ", false)                                     \
    X(moduleAdminFindModule, L"Engine.dll", "?FindModule@eCModuleAdmin@@QEBAPEAVeCEngineComponentBase@@AEBVbCString@@@Z", false)  \
    X(weatherAdminVftable, L"Engine.dll", "??_7eCWeatherAdmin@@6B@", false)                                                       \
    X(weatherStatesCount, L"Engine.dll", "?GetWeatherStatesCount@eCWeatherAdmin@@QEBAHXZ", false)                                 \
    X(weatherStatesAt, L"Engine.dll", "?GetWeatherStatesAt@eCWeatherAdmin@@QEBAPEAVeCWeatherStates@@H@Z", false)                  \
    X(weatherStateName, L"Engine.dll", "?GetName@eCWeatherStates@@QEBAAEBVbCString@@XZ", false)                                   \
    X(weatherStateBlend, L"Engine.dll", "?GetBlendFactor@eCWeatherStates@@QEBAMXZ", false)                                        \
    X(weatherPlayerPosition,L"Engine.dll", "?GetPlayerPosition@eCWeatherAdmin@@QEBAAEBVbCVector@@XZ", false)                     \
    /* the water's clock (engine/weather.h gameTimeMs, docs/research/ocean.md 4.5): static eCTimer& eCTimer::GetInstance()        \
       (0x4ceef0), unsigned long eCTimer::GetGameTime() const (0x4ceee0: the game time in ms, the sum of the scaled frame        \
       times; it stands still under the time freeze; risen-spass RE.md 60), unsigned long eCApplication::GetAppState() const     \
       (0x4c1ad0: 8 = the time freeze) */                                                                                         \
    X(timerGetInstance, L"Engine.dll", "?GetInstance@eCTimer@@SAAEAV1@XZ", false)                                                 \
    /* ---- the hemisphere light (engine/hemisphere.*, docs/research/rtgi.md 17) ---- */                                          \
    /* virtual void eCHemisphere_PS::AddToLightList(eSShaderMaterialLightList&) (0x593c20): updates the hemisphere's SH when    \
       dirty and copies it (7 registers in the c15..c21 layout) to list +0xb8, its indoor colour to list +0x128 */              \
    X(hemisphereAddToLightList, L"Engine.dll", "?AddToLightList@eCHemisphere_PS@@UEAAXAEAUeSShaderMaterialLightList@@@Z", false) \
    /* ---- sun cascades (engine/shadow_maps.*, docs/research/shadows.md 3) ---- */                                            \
    /* bool eCCascadeShadowMap::RenderShadowMaps() (0x59f200): fits and renders every cascade, each frame (reads MaxDistance  \
       +0x1c, LogSplitFactor +0x20) */                                                                                         \
    X(cascadeRenderShadowMaps, L"Engine.dll", "?RenderShadowMaps@eCCascadeShadowMap@@QEAA_NXZ", false)                          \
    /* void eCCascadeShadowMap::SetShadowMapSize(int const&) (0x59f600): +0x24, frees the splits; SetSplitCount(int const&)   \
       (0x59f650): creates them (map size, bias, cameras) */                                                                   \
    X(cascadeSetShadowMapSize, L"Engine.dll", "?SetShadowMapSize@eCCascadeShadowMap@@QEAAXAEBH@Z", false)                       \
    X(cascadeSetSplitCount, L"Engine.dll", "?SetSplitCount@eCCascadeShadowMap@@QEAAXAEBH@Z", false)                             \
    /* ---- detail distances (engine/lod.*, docs/research/lod.md) ---- */                                                       \
    /* void eCWraper_SpeedTreeRT::UpdateLoD(bCVector const&) (0x6f8f30): the tree's LOD from the camera distance, its LOD      \
       limits scaled by the factor +0x30 for the computation; GetBillboardFadeValues(float&, float&) (0x6f8a00): the same      \
       limits x factor -> the billboard fade range */                                                                          \
    X(speedTreeUpdateLoD, L"Engine.dll", "?UpdateLoD@eCWraper_SpeedTreeRT@@QEAAXAEBVbCVector@@@Z", false)                     \
    X(speedTreeBillboardFade, L"Engine.dll", "?GetBillboardFadeValues@eCWraper_SpeedTreeRT@@QEAAXAEAM0@Z", false)             \
    /* void eCRenderContextMesh::UpdateLoD(eCFrustumDatabase&) (0x5c74b0): picks the mesh LOD against eCMesh_PS's ranges      \
       (+0x60 float[], count +0x6c) */                                                                                         \
    X(meshUpdateLoD, L"Engine.dll", "?UpdateLoD@eCRenderContextMesh@@UEAAXAEAVeCFrustumDatabase@@@Z", false)                  \
    /* void eCFrustumDatabase::UpdateFromAppTemplate() (0x6708a0): copies eCRenderSystemAdmin's caps template (+0x70..0x97)   \
       into the database (+0x1590..), among them the view range factor +0x15b4 (always 1.0 in the game) */                     \
    X(frustumUpdateFromApp, L"Engine.dll", "?UpdateFromAppTemplate@eCFrustumDatabase@@QEAAXXZ", false)                        \
    /* the object draw distance's other gates (lod.md 6): bool eCPrefetchAdmin::GetPrefetchSphere(eCGeometryEntity*,         \
       bCSphere&) (0x6967e0: the cache-in sphere, radius = CacheInRange or bound + ViewRange / the default EntityROI);       \
       void eCPrefetchAdmin::SetDefaultCacheInRange(float) (0x695c10: re-evaluates every entity's sphere when the value       \
       changes); eECullStatus eCFrustumElementItem::CullTree(eCFrustumDatabase&) (0x6711a0: a parent entity's subtree cull    \
       with its raw ViewRange) */                                                                                              \
    X(prefetchGetSphere, L"Engine.dll", "?GetPrefetchSphere@eCPrefetchAdmin@@QEAA_NPEAVeCGeometryEntity@@AEAVbCSphere@@@Z", false) \
    /* void eCPrefetchAdmin::CacheEntities(bCVector const&, unsigned char, bool, bool) (0x6969c0): the per-frame cache-in walk of the         scene's sphere tree (OnPreRender); its two bools gate the walks (lod.md 7, performance.md 14) */                              X(prefetchCacheEntities, L"Engine.dll", "?CacheEntities@eCPrefetchAdmin@@QEAAXAEBVbCVector@@E_N1@Z", false)                      X(prefetchSetDefaultRange, L"Engine.dll", "?SetDefaultCacheInRange@eCPrefetchAdmin@@QEAAXM@Z", false)                       \
    /* eCShaderPoolEllement* eCShaderMaterialAdmin::CompileShaderPoolEllement(eSGfxHLSLShaderSrc const&, eEShaderMaterialVersion, \
       unsigned long) (0x6b8140): every material shader request (pool hits too): effect, entry, profile, final macro list */    \
    X(shaderPoolCompile, L"Engine.dll",                                                                                         \
      "?CompileShaderPoolEllement@eCShaderMaterialAdmin@@IEAAPEAVeCShaderPoolEllement@@AEBUeSGfxHLSLShaderSrc@eCGfxShared@@W4eEShaderMaterialVersion@@K@Z", \
      false)                                                                                                                    \
    X(frustumItemCullTree, L"Engine.dll",                                                                                       \
      "?CullTree@eCFrustumElementItem@@QEAA?AW4eECullStatus@eCFrustumFilter@@AEAVeCFrustumDatabase@@@Z", false)                \
    /* ---- high fps compatibility (engine/high_fps.*, docs/research/fps-limit.md 12): float eCTimer::GetFrameTimeInSeconds()  \
       const (0x4ced50); Game.dll gCCharacterMovement_PS::GetCurrentPosition (0x8dccb0, bCVector by value) and                 \
       SetCurrentPosition(bCVector const&, bool, bool) (0x8edfb0) ---- */                                                      \
    X(timerFrameTimeSeconds, L"Engine.dll", "?GetFrameTimeInSeconds@eCTimer@@QEBAMXZ", false)                                   \
    X(movementGetPosition, L"Game.dll", "?GetCurrentPosition@gCCharacterMovement_PS@@QEBA?AVbCVector@@XZ", false)               \
    X(movementSetPosition, L"Game.dll", "?SetCurrentPosition@gCCharacterMovement_PS@@IEAAXAEBVbCVector@@_N1@Z", false)           \
    X(timerGetGameTime, L"Engine.dll", "?GetGameTime@eCTimer@@QEBAKXZ", false)                                                    \
    X(appGetAppState, L"Engine.dll", "?GetAppState@eCApplication@@QEBAKXZ", false)                                              \
    /* ---- the material graph (engine/materials.*, docs/research/rt-albedo-facts.md 1): eCShaderBase* eCShaderBase::         \
       ms_pActiveShader (0xc62970, set by ExecuteShaderConstants 0x6d2f90), the shader / colour-source vtables ---- */         \
    X(shaderBaseActiveShader, L"Engine.dll", "?ms_pActiveShader@eCShaderBase@@1PEAV1@EA", false)                                \
    X(shaderDefaultVftable, L"Engine.dll", "??_7eCShaderDefault@@6B@", false)                                                   \
    X(shaderLeafsVftable, L"Engine.dll", "??_7eCShaderLeafs@@6B@", false)                                                       \
    X(colorSrcSamplerVftable, L"Engine.dll", "??_7eCColorSrcSampler@@6B@", false)                                               \
    X(colorSrcCombinerVftable, L"Engine.dll", "??_7eCColorSrcCombiner@@6B@", false)                                             \
    X(colorSrcBlendVftable, L"Engine.dll", "??_7eCColorSrcBlend@@6B@", false)                                                   \
    X(colorSrcMaskVftable, L"Engine.dll", "??_7eCColorSrcMask@@6B@", false)                                                     \
    X(colorSrcCubeSamplerVftable, L"Engine.dll", "??_7eCColorSrcCubeSampler@@6B@", false)                                       \
    X(colorSrcConstantVftable, L"Engine.dll", "??_7eCColorSrcConstant@@6B@", false)                                             \
    X(colorSrcVertexColorVftable, L"Engine.dll", "??_7eCColorSrcVertexColor@@6B@", false)

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
