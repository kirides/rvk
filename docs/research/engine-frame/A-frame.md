# A. The top-level frame and everything around the scene (RE, 2026-10-27)

Slice A of the engine render-sequence decompile (the other slices: `eCFrustumDatabase::Execute` internals, materials, shadows,
post filters, GUI). Headless Ghidra on Engine.dll (clone `clones\EngineA`), dumps `B:\DevDrive\risen-ghidra\frameA_dump1..4.txt`,
reused `B:\DevDrive\risen-gfx-re\frame.txt`; vtables with `risen-gfx-re\py\vt.py`, scans in `B:\DevDrive\risen-ghidra\frameA_py\`.
[C] = read in the decompilation (or disassembly), [I] = inferred, [?] = open. RVAs are Engine.dll unless marked.

Singletons used everywhere (getter -> object): `FUN_1800956c0` = `eCGfxAdmin` (`+0x5f8` the `eCAPIDevice` = our `HalDevice`,
`+0x5e8` the `eCGfxMixerAdmin`, `+0x5b8` the device again for copy / snapshot calls, `+0x648` API type, valid if != 0);
`FUN_180095630` = `eCRenderSystemAdmin` (per-frame statistics, the frustum-database template `+0x70..+0x97`);
`FUN_180095750` = `eCImageFilterAdmin`; `FUN_1800adda0` = `eCShaderMaterialAdmin`; `FUN_18014f330` = `eCIlluminationAdmin`;
`FUN_1800957e0` = `eCSceneAdmin` (`+0x78` entity admin, `+0xe8` prefetch admin); `FUN_180095870` = `eCGuiModule2`.
Device slot names: `src/hal/slots_generated.h` (`0xf0` SetViewport, `0xf8` GetViewport, `0x100` ClearViewport, `0x108`
SwapScreen, `0x110` BeginScene, `0x118` EndScene, `0x270` SetViewAndProjectionMatrices, `0x278` SetWorldMatrix, `0x2b8` SetFog,
`0x460` SetRenderTarget(colour, depth), `0x468` GetRenderTarget, `0x470` GetBackBuffer, `0x4f8` SaveBackbufferToFile; texture
`0x20` GetRenderTargetHandle(face), `0x58` CopyCurrentRTToImage).

The application object is Game.dll's `gCGameApp` (RTTI in Risen.exe; derived from `eCApplication`). It overrides none of the
render slots (OnPreRender / OnPostRender / OnRenderDraw / Draw / OnRun / OnProcess: Game.dll exports no such methods [C]), but
because its vtable is not `eCApplication::vftable`, every devirtualized fast path in the functions below falls back to the
virtual call, which reaches the same Engine.dll function. `IsEditorRunning` (slot 161) = false, `IsGameRunning` (slot 162,
`gCGameApp` 0x7a3d30 in Game.dll) = true in the game [I: by name].

## 1. Ordered call tree of one world frame

```
eCApplication::Run 0x4c0490                 loop: eCInputAdmin::UpdateX360; vtbl +0x110 eCMessageHandler::ProcessNextMessage
  -> (idle) eCApplication::OnRun 0x4c0570   [message dispatch to OnRun: I]
     [panic state -> return; app+0x150 -> bCValidateMemory::Validate]
     eCApplication::Process 0x4c0820                                    -- "update" half, no device calls of its own
       app state change (+0x134 != +0x138): SendEngineMessage(0x17), stamp +0x140
       state 0x10 for > 2000 ms -> next state (+0x13c) = 0
       eCTimer +0x50 ++ (master frame counter)
       eCProcessor::Process 0x4cd2a0 (filter +0xc00 = app state; a processible runs if filter == 0 or its mask (vtbl +8) & filter):
                                            256 priority buckets, each a linked list of eCProcessibles, vtbl +0x10
         = every module's Process, among them (order between them [?], bucket priorities not decoded):
           eCWeatherAdmin::Process 0x710730       weather blend; writes the sun light / hemisphere / sky dome props and the
                                                  current camera's fog (section 2.3)
           eCIlluminationAdmin::Process 0x579260  light-tree bookkeeping, sky cube step (2.4), UpdateLightSets 0x575130
           eCImageFilterAdmin::Process 0x663080   overlay fade weight, RT pool garbage collection (2.6)
           eCRenderSystemAdmin::Process 0x66c3a0  rsa+0x60 = 0
           ... (animation, physics, GUI, game session via app OnProcess) 
       app vtbl +0x5d0 OnProcess 0x4c0730 -> app+0xc50 (+0x20) vtbl +0x10   [I: the workspace / gCSession]
       eCEntityAdmin::KillEntities (scene admin +0x78)
       app state = requested state (+0x134 = +0x13c)
     eCApplication::DoRender(true) 0x4c0d40                             -- "render" half
       checks: gfx admin valid, render system admin vtbl +0x10 valid, app+0x152 != 1, no panic
       device lost? (slot 22 IsDeviceNotReset) or reset requested (app+0xc48) -> Reset 0x4c11d0 (else SetResolution, return)
       camera = app+0x38 (current) or app+0xc8 (default)
       eCFrustumDatabase::UpdateFromAppTemplate(camera+0xf8) 0x6708a0    (2.2)
       camera viewport rect +0xc4..+0xd0 = (0, 0, app+0x6c4 = back-buffer w, app+0x6c8 = h)   [editor: the client rect]
       camera vtbl +0x108 UpdateProjection(true) 0x4f6970 -> Update(2) -> aspect, CalcProjectionMatrix 0x4f6cb0
       camera UpdateViewport(true) / Update(1) 0x4f6b70 -> CalcViewMatrix 0x4f6fa0, eCGfxCamera::SetViewMatrix
       device GetBackBuffer(&c, &d) (0x470); SetRenderTarget(c, d) (0x460)
       ClearViewport(7 = colour|z|stencil, colour 0x00000000, z 1.0, stencil 0)   (mixer 0x50fbd0 -> device 0x100)
       app vtbl +0x5b8 OnPreRender 0x4c0650 -> eCPrefetchAdmin::CacheEntities(camera pos +0x90, 0, true, notSteady) (2.5)
       if (app state & 3) == 0:                                        -- states 1 (paused / ESC menu) and 2 (loading) render nothing
         [eSDebugOptions+0x50 -> one more ClearViewport(7, 0, 1.0, 0)]
         if image filter admin +0x90 (scene effect) == NULL: eCCameraBase::Render(camera, NULL) 0x4f8470     (3.1, no post)
         else eCImageEffect::Execute(+0x90) 0x662630                    (2.6): its eCImageFilterScene job =
           eCImageFilterScene::OnUpdateOutput 0x75cf10
             UpdateRenderTargets 0x75d140 (scene targets, 2.7)
             eCCameraBase::Update(camera) 0x4f7270
               db frame ++ (camera+0x100 = db+8), eCFrustumFilter::UpdateFrameDependencies 0x67ec40 (2.8)
               eCFrustumDatabase::ExtractFromFrustum 0x66fa60            (culling: slice B)
               eCFrustumDatabase::PreExecute 0x66e790                    -> PRE-PASSES = the off-screen views (2.9):
                 eCRenderContextDynamicLight::RenderPrePass 0x59e840 -> eCDirectionalLight_PS::UpdateShadowMaps 0x591fb0
                   -> [terrain shadow map illum+0x358 vtbl +0x170 when the camera / sun moved]
                   -> eCCascadeShadowMap::RenderShadowMaps 0x59f200 (illum+0x350): the sun cascades     (shadow slice)
                 eCRenderContextPointLight RenderPrePass 0x59eaa0 -> illum+0x380 eCPackedShadowMap vtbl +0x170 (point lights)
               eCFrustumDatabase::Request 0x66e8c0 (render ops)
             GetRenderTarget(&savedC, &savedD)
             [HDR: SetRenderTarget(scene+0x38 FP16 surface, savedD)]
             eCFrustumDatabase::Execute(camera+0xf8, desc = scene+0x40) 0x66e990     = the MAIN VIEW (slice B):
               ExecuteGfxCamera 0x4f6ad0 (2.10), eCGfxFog::Execute 0x501500 (2.3), clear, z pre-pass, UpdateShadowMask,
               solid (+ DoBeginRenderSolid, modules +0x158), debug, decals, billboards, refraction copy, DoBeginRenderAlpha,
               refracted, alpha, DoEndRenderAlpha, late list, EndMaterial
             GetViewport; [non-HDR: CopyCurrentRTToImage(back buffer -> scene+0x40)] [HDR: SetRenderTarget(savedC, savedD),
             SetViewport]
           then the effect's post filters (eCImageFilterEffect / Copy / ...: post slice), last one into the back buffer
         if image filter admin +0x98 (overlay effect) != NULL: eCImageEffect::Execute(+0x98)
       device GetBackBuffer + SetRenderTarget(back buffer, its depth)
       app vtbl +0x5b0 OnPostRender 0x4c06c0
         eCModuleAdmin::DoPostRender 0x4cd160 (modules vtbl +0x170; only gCFocusModule2::OnPostRender, Game.dll 0x82e460)
         app vtbl +0x568 Draw(app+0x18 HWND) 0x4c13b0  (tail call)
           eCGuiModule2::Draw(NULL) 0x519a60 if gui+0x3f            (via vtbl +0x5c0 OnRenderDraw 0x4c0700; GUI slice)
           app+0x144 -> DoSnapshot 0x4c0c50 (device 0x4f8 SaveBackbufferToFile(app+0x148 name))
           eCGfxMixerAdmin::SwapScreen(hwnd, NULL, NULL) 0x50fc90 -> device 0x108 SwapScreen = Present
       eCTimer::Update((state | state >> 3) & 1) 0x4cf0e0                -- frame time, FixedFPS / MaxFPS sleeps (fps-limit.md)
     eCProfilerAdmin::NextFrame
```

Not in this frame: `eCApplication::OnPaint` 0x4bf9b0 -> `RenderAndDrawTo(hwnd, bool)` 0x4c1740 (swaps app+0x18, calls `DoRender`)
on WM_PAINT; `DrawTo` 0x4c1530 / 0x4c1480 and `RenderAndDrawTo(hwnd, camera, bool)` 0x4c1620 have no importer in Game.dll /
Script.dll / Script_Game.dll / GUI2.dll / Risen.exe and no Engine caller except each other [C: import-name scan] = editor paths.

## 2. The steps in detail

### 2.1 DoRender's own device calls [C]

| # | call | values |
|---|---|---|
| 1 | GetBackBuffer (0x470) | the swapchain image + its depth surface (DX9 inline: device+0xe40 / +0xe48) |
| 2 | SetRenderTarget (0x460) | (back buffer, back-buffer depth) |
| 3 | mixer ClearViewport -> device 0x100 | flags 7, colour 0x00000000, z 1.0f, stencil 0 (the frame's first "None" segment, frame-phases.md 2) |
| 4 | scene effect / camera render | section 3 |
| 5 | GetBackBuffer + SetRenderTarget | back buffer re-bound for the GUI |
| 6 | (Draw) GUI, SaveBackbufferToFile if a snapshot is pending, SwapScreen | |

No viewport is set by DoRender itself: the main view's viewport comes from `ExecuteGfxCamera` (2.10). No BeginScene / EndScene in
DoRender (the passes bracket themselves: z pre-pass, solid / alpha `0x110` / `0x118` inside Execute; sky cube faces).

### 2.2 Per-frame config template `eCFrustumDatabase::UpdateFromAppTemplate` 0x6708a0 [C]

Copies render system admin `+0x70..+0x97` to the camera's database `+0x1590..+0x15b7` every frame, then writes the camera's
ClipDepth: camera `+0xbc` (near) kept, `+0xc0` (far) = db `+0x159c`, and calls camera vtbl `+0x108` UpdateProjection(true).
The template (`eCRenderSystemAdmin::ApplyConfiguration` 0x66c3e0, `eSRenderOptions` 0xc89930, ctor 0x8b6a0):

| rsa | db | default / source | meaning (users: slice B) |
|---|---|---|---|
| +0x70 / +0x71 | +0x1590 / +0x1591 | 1 / 1 | z pre-pass (depth map) on; Request flag 4 |
| +0x74 | +0x1594 | 0 | debug bounding boxes (1 / 2) |
| +0x7c | +0x159c | `/Engine/Render/zFarLow / Medium / High` by `/Engine/Render/ViewDistance` (1 / 2 / 3; ConfigDefault 15000 / 30000 / 60000, ViewDistance 3) | far plane = camera +0xc0 = c11.w / PS c0.w; fog end clamp (2.3) |
| +0x80 | +0x15a0 | 0 | 1 = skip all module callbacks in Execute |
| +0x81 | +0x15a1 | 1 | statistics counting |
| +0x84 | +0x15a4 | `/Engine/Render/RenderDebug` (ConfigDefault false; editor 1) | Request flag 0x100 (debug ops) |
| +0x85 / +0x87 / +0x88 | +0x15a5 / 7 / 8 | 1 | Request flags 1 / 0x20 / 0x80 |
| +0x86 | +0x15a6 | `/Engine/Render/VegetationRenderingEnabled` | vegetation |
| +0x89 | +0x15a9 | 0 | 1 = no distance fade |
| +0x8a | +0x15aa | 1 | PreExecute enabled (pre-passes, 2.9) |
| +0x94 | +0x15b4 | 1.0 | view-range factor (our `lod.view`) |

Other `eSRenderOptions`: `/Engine/Render/FoV` (+8, 75), `zNear` (+0x18, 25), `zBias` (+0x68, 0.05), `MSAA` (+0x98), `Shadows/Quality`
(+0xa8), `/Engine/Render/PostProcessing/DefaultEffect` (+0xf8; code default "Bloom", ConfigDefault / PerformanceHigh / Normal
"DoF", PerformanceLow "HDRI"). `PostProcessingEnabled` (ConfigDefault) is not referenced by any DLL [C: string scan].

### 2.3 Fog: weather -> camera fog -> constants [C]

* **Writer**: `eCWeatherAdmin::Process` 0x710730 (end of the function). Weather `+0x50` = the blended current `eCWeatherState`
  (zones / environments / day-time states mixed into it earlier in the function). It writes the **current camera's** fog object
  `camera +0x15c0` (= its database `+0x14c8`):
  `eSGfxFogData` at fog `+0x18` (layout `src/hal/types.h`): enable = 1, rangeFog = 0, colour = ARGB8888 of weather `+0xe0`
  (bCFloatColor, alpha 0xff), vertexFog = 1, mode = 3 (LINEAR), start = weather `+0xf8`, end = weather `+0xfc`, density = weather
  `+0x100`; fog `+0x34` = 1 (dirty). If end > far (rsa `+0x7c`): start = start / end x far, end = far.
  The same function sets the World_MCP entity's `eCDirectionalLight_PS` Color (`+0x48`) = weather `+0x58..`, SpecularColor
  (`+0x60`) = `+0x70..`, Intensity (`+0x70`) = (day factor) x `+0x80` x `+0xb8`, DirectionOffset; `eCHemisphere_PS` GeneralAmbient
  `+0x48`, BackLight `+0x60`, Intensity `+0x94` = 1, SunLight `+0xa0`, the axis offsets, SetIndoorAmbientColor(weather `+0x88`);
  `eCSkydome_PS` rotations `+0x2d0 / +0x310 / +0x350`, `+0x90`, SetCloudColor, `eCShaderSkyDome` SunColor / HazeColor (`+0x120`) /
  SkyColor (`+0x108`) / SunNormal; rain emitter properties. Entity "World_MCP" is looked up by name when weather `+0x38` is unset.
* **Executor**: `eCGfxFog::Execute` 0x501500, called by `eCFrustumDatabase::Execute` with db `+0x14c8` (every view: main and
  shadow databases each have their own fog object; only the app camera's is fed by the weather [I: no other writer seen]).
  `ms_pMasterFog` overrides it when set; with `ms_bOptimize` the call is skipped when this fog is already active and not dirty.
  Device `0x2b8` SetFog(fog+0x18) (fixed-function, ignored by SM3), then `eCShaderMaterialAdmin::SetFog` 0x6b9fc0:
  **PS c3** = (R, G, B, 0) / 255 (bytes `+0x1e`, `+0x1d`, `+0x1c` of the colour) if enable, else 0;
  **PS c4 = VS c30** = (-start k, k, density, 0) with k = density / (end - start), all 0 when density <= 0 or fog disabled.
  (Corrects nothing in native-renderer.md 10; its open item "the weather path that sets the fog fields" is this.)

### 2.4 Sky cube (`eCIlluminationAdmin::Process` -> `eCSkydome_PS::UpdateSkydomeEnvCubemap` 0x6e7ed0) [C]

Before DoRender, every frame, cube = illum `+0x470` -> `+0x70` (`eCGfxImageData`, A8R8G8B8 128²; frame-phases.md 4), sky dome =
`GetSkydome` 0x574690 (World_MCP property set 0x21). A **one-face-per-frame state machine** (global counter `DAT_180c6ea90`):
timer `+0x3b8` += eCTimer `+0x2c` (frame seconds); when it exceeds `+0x3bc` (or a cycle is running) the counter advances and
the timer resets. Counter 1..5 render faces **0 (+X), 1 (-X), 2 (+Y), 4 (+Z), 5 (-Z)** on five consecutive frames; face 3 (-Y)
is never rendered (a dead counter-0 branch would clear it to 0x30303030). Per face: `BeginMaterial`, save RT / viewport,
viewport (0, 0, size, size, 0, 1), GetRenderTargetHandle(face), SetRenderTarget(face, NULL depth), sky camera (`+0x3c0`,
created by `CreateSkydomeEnvCubemapCamera`): projection rows (1,0,0,0) (0,1,0,0) (0,0,1.001001,1) (0,0,-1.001001,0) = 90°,
near 1, far 1000, view = inverse of the face basis at the origin, `eCGfxCamera::Execute`, ClearViewport(1 = colour, 0, 0, 0),
BeginScene, `eCSkydome_PS::Render` 0x6e4730 with `+0x3cc` = 1, EndScene. Then restore RT / viewport, `ExecuteGfxCamera(app
camera)` (re-uploads the main camera constants), `EndMaterial`. After face 5 the counter returns to 0.

### 2.5 OnPreRender [C]

`eCPrefetchAdmin::CacheEntities(camera +0x90, 0, true, notSteady)`, notSteady = !(state == 2 && next state == 2) (lod.md 6;
our `debug.prefetch_stride`). Streaming only, no device calls.

### 2.6 Image effects (`eCImageFilterAdmin`, `eCImageEffect`) [C]

* Admin: `+0x58 / +0x60` RT pool (0x20-byte entries: image, format, w, h, last-use frame, in-use byte), `+0x68` effect map (by name,
  loaded from `compiled_imageeffects.bin` / raw `.xieff`), **`+0x90` scene effect**, **`+0x98` overlay effect**, `+0xa0` fade-in,
  `+0xa4` fade-out, `+0xa8` duration, `+0xac` elapsed, `+0xb0` overlay weight (0..1, Process), `+0xb4` frame counter.
  `ActivateDefaultEffect` 0x663be0 / `ApplyConfiguration` 0x663460 set +0x90 from `/Engine/Render/PostProcessing/DefaultEffect`;
  `ActivateEffect` 0x663ee0 / 0x663f80 by name / pointer. `StartOverlayEffect(effect, duration, fadeIn, fadeOut)` 0x663e40 /
  0x663db0 (name): imported by Game.dll and Script.dll (spell / state effects [I]); `Process` drops it after `duration` and fades
  `+0xb0` in / out; frees pool RTs unused for > 1 frame. `AllocRenderTarget(format, size)` 0x663950 reuses a free entry of equal
  format + size.
* `eCImageEffect::Execute` 0x662630: jobs = `bTValArray<eSImageFilterJob>` (0x10: filter, output index). For each filter of the
  effect (`+0x18` array, `+0x20` count) vtbl `+0x140` AddJobs(-1, jobs); `eCImageFilterBase::ExecuteJobs` 0x664d30.
* `eCImageFilterBase::AddJobs` 0x665370: skip if (filter, index) is already listed; else append, for each input (`+0x108` count,
  `+0x110` GetInputAt) source `+0xf8` AddRefOutputAt + source AddJobs(its index), then move its own entry to the end: a
  **post-order, de-duplicated** list (inputs before consumers). `eCImageFilterRTBase::AddJobs` 0x668340: output mode `+0x38` 1
  (back buffer [I]) only for index < 0, mode 0 only index 0. `eCImageFilterScene::AddJobs` 0x75d130: always index 0.
  `eCImageFilterImageEffect::AddJobs` 0x666460 forwards into a nested effect (by name `+0x30`).
* `ExecuteJobs`: per job `OnUpdateOutput(index)` (vtbl `+0x138`), then `ReleaseOutputAt` (`+0x100`) on each input's source (pool
  RTs return when their last consumer ran). **The scene filter is the job that renders the world, the cascades and the point-light
  shadows (2.9) included**; the remaining jobs are post filters (`eCImageFilterEffect::OnUpdateOutput` 0x7597a0,
  `eCImageFilterCopy`, ...: post slice). `RTBase::SetRenderTarget` 0x668370: saves RT + viewport, binds output `+0x70` (cube face
  6 if cube) with the saved depth, viewport (0, 0, `+0x80`, `+0x84`, 0, 1); `ResetRenderTarget` 0x668500 restores.
  `UpdateColorOutput` 0x6667f0: size = input `+0x58`'s size x scale `+0x50 / +0x54` (size mode `+0x48` 1) or fixed, format `+0x68`,
  from the pool.
* The shadow mask is an image filter too but not a job of the effect: `UpdateShadowMask` 0x5748d0 (inside Execute) calls
  `eCImageFilterBase::UpdateOutput` 0x664f20 (its own job list) on illum `+0x448` (`+0x458` when `+0x448`'s byte `+0x114` is set),
  with illum `+0x460` (an `eCImageFilterTexture`) fed the depth map (material admin `+0x290`); result -> material admin `+0x298`;
  then `eCPackedShadowMap::ClearCells(illum+0x380)` (shadow slice).

### 2.7 Scene targets (`eCImageFilterScene::UpdateRenderTargets` 0x75d140) [C]

Size = current camera viewport (`+0xc4` rect); format 0x15 (A8R8G8B8), or 0x71 (A16B16G16R16F) when `EnabledHDR` (`+0x30`)
and `CheckDeviceFormat` (slot 18) accepts it. `+0x40` colour image (always), `+0x38` second FP16 image (HDR only), `+0x48` R32F
(0x72) depth output when `EnabledDepthOutput` (`+0x31`). Recreated when format / size change. Outputs (`GetOutputAt` 0x75d770,
count 2): 0 "ColorOut" = `+0x38` if present else `+0x40`; 1 "DepthOut" = `+0x48`. eSRenderTargetDesc passed to Execute =
`&scene+0x40` = {colour copy image `+0x40`, depth map `+0x48`}.
* **HDR** (the game's case: SceneHdr FP16, frame-phases.md 4): the world renders into `+0x38`; `+0x40` is the refraction copy
  destination (CopyCurrentRTToImage in Execute before the refracted list); post reads `+0x38`.
* **non-HDR**: the world renders into the bound back buffer; after Execute `CopyCurrentRTToImage(1, vp, vp)` copies it into `+0x40`
  (needs gfx admin `+0x5b8`).

### 2.8 Camera update [C]

`eCCameraBase::Update(void)` 0x4f7270 (= the first half of `Render` 0x4f8470): if db `+0x15a1` rsa `+0x64`++; global
`DAT_180c628c0`++; db `+0x20` = 0; db frame `+8`++; `eCFrustumFilter::UpdateFrameDependencies(camera+0x120, db)` 0x67ec40
(`UpdateCameraObject`, frustum from proj x view, bounding sphere of the 8 frustum corners `+0x44`, its box `+0x54..+0x68`, the
view matrix's third column `+0x6c..+0x78`, `+0x7c` = 16777215 / far (the 24-bit depth key scale), `eCBeamTree::Init`
(portals, db `+0x15a3`)); ExtractFromFrustum, PreExecute, Request (slice B). `eCCameraBase::Render` = the same + Execute(desc).
`CalcProjectionMatrix` 0x4f6cb0 (perspective, `+0xdc` set): cot = cos/sin(fov/2) with fov `+0xd4` (rad), m00 = cot x aspect
(`+0xd8` = h / w), m11 = cot, m22 = f / (f - n), m23 = 1, m32 = -n f / (f - n), m33 = 0 (D3D LH, n `+0xbc`, f `+0xc0`), stored at
camera `+0x50`, gfx camera `+0x60` and `+0xe0` = proj x view; the depth-bias projection (gfx camera `+0xa0`) has m22' = (f + b) /
(f - n), m32' = -(n + b) m22', b = `+0xe0`. Ortho (`+0xdc` clear): scale (2 / `+0xa8`, 2 / `+0xac`, 1 / (f - n)).
`CalcViewMatrix` 0x4f6fa0: rotate X by `+0x9c`, Y by -`+0xa0`, Z by -`+0xa4`, translation from position `+0x90`; an optional
scale `+0xa8..+0xb0` (perspective only if != 1).

### 2.9 Off-screen views = pre-passes of the main database [C] (frame-phases.md 7 open item: answered)

`eCFrustumDatabase::PreExecute` 0x66e790 (only if db `+0x15aa`): for every extracted element whose flags (`+0x10`) have 0x400,
every render context of it gets vtbl `+0x38` **RequestPrePasses(db)**; then every op of the pre-pass list (db `+0x14d0`, data
`+0x14d8`, count `+0x14e0`) gets vtbl `+0x28` **RenderPrePass**. Two light contexts implement them (both 0x28 bytes, flags 0x400,
`+0x10` entity, `+0x18` light PS):

| context (vtable) | created by | RequestPrePasses | RenderPrePass |
|---|---|---|---|
| dynamic light `0x9a98b0` | `eCDynamicLight_PS::CreateRenderContext` 0x5932f0 (directional, hemisphere, dynamic lights) | 0x59e7c0: one op per database frame (global `DAT_180c62884` = db `+8`) | 0x59e840: light vtbl `+0x2f0` UpdateShadowMaps; only `eCDirectionalLight_PS` 0x591fb0 does something (base 0x592b90 = ret) |
| point light `0x9a9908` | `eCPointLight_PS::CreateRenderContext` 0x598640 | 0x59e8c0: `eCStaticPointLight_PS` with shadows (`+0x68`) / `eCPointLight_PS`: level = (d - r) / r + 1 (static) / + 2; level < 6 and `AddShadowCells(level, db frustum +0xa8)` -> one op per db frame (`DAT_180c62888`) | 0x59eaa0: illum `+0x380` `eCPackedShadowMap` vtbl `+0x170` (= `UpdatePackedShadowMaps` 0x574a00): the point-light shadow atlas |

`eCDirectionalLight_PS::UpdateShadowMaps` 0x591fb0: true direction `+0x80..+0x88`, cascade direction `+0x8c..+0x94` follows it
when they differ by > 0.5° (DegToRad x 0.5); the **terrain shadow map** (illum `+0x358`, `eCShadowMapDirectionalLight2`) is
re-rendered (its matrix `+0x60` = LookAt(dir), box `+0xe0` = camera sphere (pos, far) in light space, vtbl `+0x170`) when the
camera moved more than `+0x9c` texels (texel = 2 `+0x98` / map size `+0xc`) in light space or the direction stepped; then the
cascade object (illum `+0x350`) gets "LightDirection" `+0x10..+0x18` = the stepped direction and
`eCCascadeShadowMap::RenderShadowMaps` 0x59f200 runs (cascades: shadow slice; each = `eCShadowMapBase::RenderShadowMap`
0x5a1070 -> `eCRenderSystemAdmin::RenderShadowMap` 0x66ce00 -> Extract / Request / `ExecuteShadowMap`). So the order is fixed:
**[terrain map] -> cascades -> [point-light atlas] -> main view**, all inside the scene filter's job, after the main camera's
culling and before its Execute. With no directional light in the extracted set (interiors) no `Shadow` view appears [I: the
light entity is not extracted / not flagged; matches frame-phases.md 3].

### 2.10 Camera to the device (`ExecuteGfxCamera` 0x4f6ad0, `eCGfxCamera::Execute` 0x500d30, `SetCamera` 0x6ba1d0) [C]

`ExecuteGfxCamera`: GetViewport, viewport = camera rect `+0xc4` + z 0..1 (SetViewport 0xf0), `eCGfxCamera::Execute(camera+0x1738)`.
`eCGfxCamera::Execute`: `ms_pMasterCamera` (0xc62590) replaces `this` when set; with `ms_bOptimize`, same active camera, not
dirty (`+0x160`) and no reset: only `eCGfxCameraStates::Execute` (`+0x18`, device `0x268`). Else `ms_pActiveCamera` = this,
camera states, device `0x270` SetViewAndProjectionMatrices(`+0x20`, `+0x60`), then `SetCamera`:
material admin `+0x184` = view-proj (`+0xe0`), `+0x1c4` = depth-bias proj (`+0xa0`) x view; `SetCamObstacleDistances(app camera
+0xe4)`; **VS c0..c3** = `+0x184` (or `+0x1c4` when admin `+0x310`), **c4..c7** = inverse projection, **c8..c10** = inverse view
(`+0x120`, 3 rows), **c11** = (position `+0x150..+0x158`, far of the **app's current camera** `+0xc0`); **PS c0** = the same
vector; **PS c1** = (0.5, -0.5, 0.5 + 0.5 / vpW, 0.5 + 0.5 / vpH) from the current viewport (native-renderer.md 10 writes
1 / vpW: it is 0.5 / vpW, the half-texel offset).

### 2.11 Module render callbacks (`eCModuleAdmin`, modules `+0x18` array, `+0x20` count, enabled byte `+0x10`) [C]

Slots: `+0x150` OnBeginRenderSolid (`DoBeginRenderSolid` 0x4cb810), `+0x158` OnEndRenderSolid (Execute calls it directly after
the solid list; `DoEndRenderSolid` 0x4cb7a0 has no caller), `+0x160` OnBeginRenderAlpha (0x4cb710), `+0x168` OnEndRenderAlpha
(0x4cb680), `+0x170` OnPostRender (0x4cd160). All skipped when db `+0x15a0`. Overrides (scan of every Engine vtable, Game.dll
exports): Engine only `eCPhysicsScene::OnEndRenderSolid` 0x5e0a80 -> `DebugRender` 0x5e5100 (physics debug line lists, only with
its flag `+0x18`); Game.dll `gCHudModule2` 0x83cdb0..0x83cdf0 (all five are `ret`), `gCFocusModule2::OnPostRender` 0x82e460 (walks
its focus list, vtbl `+0x138` per entry: no draws [I]). **In the game no module draws in these hooks** (Script*.dll exports none).

## 3. Data structures

**eCApplication** (`ms_pApplication` 0xc62568): `+0x18` HWND, `+0x38` current camera, `+0x40` main-camera lock, `+0xc8` default
camera, `+0x134` app state, `+0x138` last state, `+0x13c` requested state, `+0x140` state time stamp, `+0x144` snapshot pending,
`+0x148` snapshot name, `+0x150` validate memory, `+0x152` no-render flag, `+0x164` multisample, `+0x168` depth format, `+0x170`
`eSGfxContextDesc`, `+0x6c4 / +0x6c8` back-buffer size, `+0xc48` reset requested, `+0xc50` workspace. App states (menu-fps.md 2):
0 running, 1 paused (ESC menu), 2 loading / prefetch, 8 time freeze, 0x10 after a load; `SetAppState` 0x4c1ab0 writes +0x134 and
+0x13c, `RequestAppState` 0x4c1aa0 only +0x13c.

**eCCameraBase**: `+0x10` view, `+0x50` projection, `+0x90` position, `+0x9c..+0xa4` angles, `+0xa8..+0xb0` ortho size / view
scale, `+0xb4` ClipProjection (vec2), `+0xbc` near, `+0xc0` far (ClipDepth), `+0xc4..+0xd0` viewport rect (l, t, r, b), `+0xd4`
FoV (rad), `+0xd8` aspect h / w, `+0xdc` perspective, `+0xe0` depth bias, `+0xe4` obstacle distances, `+0xf8` embedded
`eCFrustumDatabase` (`+0x100` = db frame, `+0x118` = db element count, `+0x120` = db `+0x28` frustum filter, `+0x1699` = db
`+0x15a1`, `+0x15c0` = db `+0x14c8` fog, db `+0x14c0` = this), `+0x1738` `eCGfxCamera*`. vtbl `+0x100` UpdateViewport, `+0x108`
UpdateProjection.

**eCGfxCamera**: `+0x10` device, `+0x18` camera states, `+0x20` view, `+0x60` projection, `+0xa0` depth-bias projection, `+0xe0`
view-projection, `+0x120` inverse view, `+0x150` position, `+0x160` dirty. **eCGfxFog**: `+0x10` device, `+0x18` eSGfxFogData
(0x1c), `+0x34` dirty. **eCWeatherAdmin**: `+0x38` World_MCP proxy, `+0x48` blend time, `+0x50` current state (fog colour
`+0xe0`, start `+0xf8`, end `+0xfc`, density `+0x100`; sun colour `+0x58`, specular `+0x70`, intensity `+0x80`, indoor ambient
`+0x88`, sky `+0x108`, haze `+0x120`, cloud `+0x140`), `+0x2d0 / +0x2d8` day-time states, `+0x2e8 / +0x2f0` environments,
`+0x4a0 / +0x4a8` weather zones, `+0x4c4` mode. **eCSkydome_PS**: `+0x3b8` cube timer, `+0x3bc` interval, `+0x3c0` cube camera,
`+0x3cc` cube-render flag. **eCIlluminationAdmin**: `+0x350` cascade shadow map, `+0x358` terrain shadow map, `+0x370 / +0x374`
counters, `+0x380` packed point-light shadow map, `+0x448 / +0x458` shadow-mask filters, `+0x460` depth texture filter, `+0x470`
sky cube resource. **eCImageFilterScene**: `+0x30` HDR, `+0x31` depth output, `+0x38` / `+0x40` / `+0x48` images (2.7).

## 4. Frames that differ

| frame | what DoRender does [C unless marked] |
|---|---|
| ESC menu / pause (app state 1) | back buffer bound + cleared to black, **no scene, no post**, GUI, present (measured 16 draws, menu-fps.md 4). `eCProcessor` runs only processibles whose mask (vtbl `+8`) has a bit of the state (filter = state, 0 = all), so which modules (weather, illumination / sky cube) still process while paused is [?] |
| loading (state 2) | as state 1; the synchronous load itself presents through `eCGuiModule2::RenderEdit` 0x519c90 without DoRender (menu-fps.md 3) |
| after a load (0x10), time freeze (8) | normal world frame |
| main menu / title backdrop | normal frame with an (almost) empty world: `Post{Main{ShadowMask} Post}` (frame-phases.md 3) [C measured] |
| interiors, no sun | no Shadow views (no directional-light pre-pass, 2.9) [I cause]; point-light atlas only if a shadowing point light is in range |
| no scene effect (+0x90 NULL, e.g. an unknown DefaultEffect) | `eCCameraBase::Render(NULL)`: the world straight into the back buffer, no scene targets, no refraction source, no post |
| overlay effect running | a second `eCImageEffect::Execute` after the scene effect (frame-phases.md `Post{Post(1)}`) |
| inventory / character | normal main view (no Preview view, frame-phases.md 3); `RenderEntity` (Preview) has no caller in any game DLL [C] |
| editor (not the game) | viewport = client rect; `RenderAndDrawTo` / `DrawTo` |

## 5. For a native implementation

Engine data a native frame must read, per frame (all on the engine thread, between `eCApplication::Process` and Present):

* **Gate**: app state `+0x134` (render only when `& 3 == 0`), image filter admin `+0x90` / `+0x98` (effect present / overlay +
  weight `+0xb0`), scene filter flags `+0x30` / `+0x31`.
* **Camera**: app `+0x38` / `+0xc8` -> `eCCameraBase` (+0xbc / +0xc0 / +0xc4 / +0xd4 / +0xd8 / +0xe0) and its `eCGfxCamera`
  (`+0x20 / +0x60 / +0xa0 / +0xe0 / +0x120 / +0x150`), valid after `eCCameraBase::Update` in the scene job (the projection and
  view are recomputed in DoRender before; the frustum after `UpdateFrameDependencies`). `ms_pMasterCamera` overrides.
* **Fog**: camera `+0x15c0` -> `+0x18..+0x30` after `eCWeatherAdmin::Process` (already final before DoRender); constants c3 / c4 /
  VS c30 per 2.3 (no need to wait for the engine's draws).
* **Lights / sky**: `eCIlluminationAdmin::Process` results (light sets, hemisphere: native-renderer.md 10, rtgi.md 17), weather
  outputs on the World_MCP light / hemisphere / sky dome (2.3), sky cube image (5-frame cycle, face 3 stale).
* **Shadows**: they are produced inside the main database's PreExecute; a native renderer that keeps the engine's culling gets
  them for free; one that replaces the main view must still trigger (or replace) `eCDirectionalLight_PS::UpdateShadowMaps` and the
  packed point-light atlas, or skip them deliberately (the shadow mask filter reads them).
* **Order to reproduce**: [sky cube face] -> [scene targets] -> cull -> [terrain shadow map] -> cascades -> [point atlas] -> main
  view (Execute) -> post jobs (post-order list) -> overlay effect -> back buffer -> module OnPostRender -> GUI -> snapshot ->
  present -> eCTimer.

Hooks (all exported, unique, normal prologue unless noted): `eCApplication::DoRender` 0x4c0d40 (frame start; not hooked today),
`eCImageEffect::Execute` 0x662630 (Post view, hooked), `eCImageFilterScene::OnUpdateOutput` 0x75cf10 (the whole scene job;
replace it to own the world render while the post chain stays the engine's: it must leave `+0x38` / `+0x40` / `+0x48` filled),
`eCCameraBase::Update` 0x4f7270 (after culling = after the shadow pre-passes), `eCFrustumDatabase::PreExecute` 0x66e790 (pre-pass
boundary; the light contexts' `RequestPrePasses` / `RenderPrePass` are unexported vtable slots of 0x9a98b0 / 0x9a9908: patch the
slots, not the code), `eCDirectionalLight_PS::UpdateShadowMaps` 0x591fb0 (vtable slot 94 of 0x9a3dc8; exported),
`eCWeatherAdmin::Process` 0x710730 (fog / sun / sky inputs ready after it), `eCSkydome_PS::UpdateSkydomeEnvCubemap` 0x6e7ed0
(SkyCube view, hooked), `eCApplication::Draw` 0x4c13b0 (Gui view, hooked), `eCGfxMixerAdmin::SwapScreen` 0x50fc90 (present).
Never detour the tiny getters (`GetCurrentCamera` 0x4c19f0 etc.; COMDAT rule).

## 6. Open

* Order of the module `Process` calls inside `eCProcessor` (bucket priorities): does `eCWeatherAdmin::Process` run before
  `eCIlluminationAdmin::Process` (light sets from this frame's weather) or after (one frame lag)? [?]
* Which overlay effects Game.dll / Script.dll start (names, triggers) [?]; the DoF effect's filter graph (job list contents):
  data in `compiled_imageeffects.bin`, post slice.
* Interiors: confirm that the missing Shadow views are because the World_MCP directional light is not extracted (vs. a flag) [I].
* `eCCameraBase +0xe4` obstacle distances and `SetCamObstacleDistances` target registers [?].
* `eCGfxCameraStates` (`+0x1c`, `+0x20`, device `0x268`) content [?].
* The terrain shadow map's movement threshold `+0x9c` / `+0x98` values at run time [?].
