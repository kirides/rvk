# Opt: side effects of the frame skeleton, the views, the effect graphs and the GUI (RE, 2026-10-28)

Slice "frame" of the double-dipping removal (README.md 4 "then" row, 17 "Full"): in render mode Full our code issues ~99.98 % of the
draws, but the engine's render code still runs and its draws / device calls are dropped. Before that code can be skipped, every write
it makes that is not pure draw preparation must be known, so that a lean frame keeps (or reproduces) exactly those. This file covers
the frame skeleton (`eCApplication::DoRender` / `OnPostRender` / `Draw` / `SwapScreen`), the scene job and the main camera's
culling / extraction, `eCFrustumDatabase::Execute`, the shadow views, the image-effect graphs (incl. the shadow mask), `eCGfxEffect`
and the GUI sprite batcher. The per-context render slots (meshes, actors, trees, particles, sky ...) are other slices.

Sources: the existing dumps (`B:\DevDrive\risen-ghidra\enginecpu\d1.txt` Execute / Update / Request / PreExecute, `frameC_dump1.txt`
BeginZPass / EndZPass / BeginMaterial / EndMaterial, `frameA_dump1..3`, `frameF_dump1..5`, `imgparam_dump.txt`, `rvk_sunshadow_dump.txt`
/ `_dump2.txt`, `frameE_dump1.txt`, `B:\DevDrive\risen-gfx-re\frame.txt`), capstone disassembly of the test copy's Engine.dll /
Game.dll (`B:\DevDrive\risen-ghidra\n56\edis.py`, new helpers in `B:\DevDrive\risen-ghidra\optframe\`: `impcalls.py` = call sites of an
import in a DLL, `xref.py` = rel32 calls / pointers to an RVA, `gdis.py` = edis for Game.dll). No game run. [C] = read in the
decompilation / disassembly, [I] = inferred, [?] = open. RVAs are Engine.dll unless marked `Game.dll`.

Classes: **S** simulation / time integration (must advance every frame as the engine would), **C** caching / first use (lazy
creation, streaming, pools: must happen at least once / keep happening), **K** state our composition or gameplay reads (must keep
being written, by the engine or by us), **D** droppable (device work, statistics, caches only the dropped engine draws use).

## 1. Answers in short

* **Nothing in the culling / extraction is render-only.** `eCCameraBase::Update` (frame stamps, frustum, extraction, PreExecute's
  request half, Request) produces the lists our composition reads **and** gameplay state: the visible stamp (request bit 0x40,
  `item +0xc = db +8`) is read by **Game.dll** (`eCFrustumElementItem::WasVisibleLastFrame` from `gCCameraAdmin::SetCameraTargets`,
  `gCCameraAdmin::CalculateTargetPositionOffset`, the head-bone helper FUN_1807c7530 and twice from `gCScriptProcessingUnit::
  UpdateChannel` (AI)) [C: import call sites]; the illumination update (bit 0x80) integrates the outdoor factor over time (S) and
  feeds the lighting we compose; LoD (bit 1) picks `ctx +0x28` and prefetches (K + C). Only the statistics counters and profile
  timings are D. **Update stays whole**; only PreExecute's RenderPrePass ops (the shadow views) are split (section 5).
* **The sort is done twice today** (the tap's copy + Execute's in-place qsort; likewise ExecuteShadowMap's). In a lean frame Execute
  does not run: our executor calls **the engine's own qsort 0x4a4aa0 with the engine's comparator in place** on the database's
  list (the call the engine would make), then walks the live array: one sort, no copy, and each op pointer is `&data[i]` exactly as
  the engine's slots receive it (needed for the fallback, section 9). Nothing reads the lists after Execute except the next Request
  (which clears the counts) and the unused export `GetAlphaElement` 0x66e0c0 [C]. Before the lean frame (copy model), the double sort
  could also be removed by redirecting Execute's three `call qsort` sites (+0x2c1 / +0x42e / +0x976, already decoded by the tap) and
  ExecuteShadowMap's (+0x72) to a thunk "qsort, then snapshot that list" [I: not built].
* **Eye adaptation has no CPU integrator.** The adaptation (`AdaptiveLiminance`), the luminance hand-off (`AvgLuminance` Copy) and the
  bloom accumulation (`GlowVAddBloom2`) are GPU passes over persistent 1x1 / 309x210 targets (ShareTexture false: the filter keeps
  `+0x70` across frames, the pool never frees it); the CPU only supplies `FRAMETIME` = `eCTimer +0x2c` through a pure read [C]. So
  they are **S on the GPU**: our (already composed, group 32768) draws must run exactly once per world frame, in the engine's job
  order (Adaptive reads last frame's AvgLuminance before the Copy overwrites it), and not in frames the engine renders no world
  (app state & 3, DefaultEffect "Bloom"). The CPU-side S of the post chain is the overlay weight `+0xb0` in
  `eCImageFilterAdmin::Process` (not render code: stays).
* **The cheap engine brackets must keep running** (or be reproduced exactly): `ExecuteGfxCamera` + `eCGfxFog::Execute` (ms_pActiveCamera,
  admin camera / fog pools, the camera states' stencil call), `BeginZPass` / `EndZPass` / `BeginMaterial` / `EndMaterial` (admin flags
  `+0x311..+0x314` our composition reads, master states, cache resets, **HAL render-state blocks**: section 4.2), the material admin's
  sampler holders (`+0x248` / `+0x270` / `+0x298`), `ClearCells` at the end of UpdateShadowMask, the billboard queue reset, the sprite
  record reset. They cost microseconds; the engine's cost is in the per-op slots, the effect parameter pushes and the per-quad sprite
  locks.
* **New engine draws outside every unit** [C, disassembly]: `gCFocusModule2::OnPostRender` (Game.dll 0x82e460, in
  `eCApplication::OnPostRender` before the GUI) calls slot +0x138 of each focus indicator; `gCFocusCrossHairIndicator2` (Game.dll vtable
  0xeebaa8, slot 39 = 0x8388b0) saves `eCGfxMixerAdmin::GetActiveCamera`, sets its own eCGfxCamera, `BeginMaterial`, draws a mesh with
  `eCMeshResource2::Render`, `EndMaterial`, re-executes the saved camera; `gCFocusScreenPointIndicator2` (0xeece08, slot 39 = 0x83a920)
  draws through a sprite + `eCSprite::Flush`. A-frame.md 2.11's "no draws [I]" is wrong for the crosshair indicator [C]; whether it
  is visible in normal play (ranged aiming?) [?].

## 2. The frame skeleton

### 2.1 `eCApplication::DoRender` 0x4c0d40 [C] (d1.txt)

| write / call | reader | freq | ours today | class |
|---|---|---|---|---|
| gfx admin / RSA valid, `app +0x152`, panic: early returns | gate | frame | read | K (keep) |
| device-lost check (slot 22 IsDeviceNotReset) / `app +0xc48` -> `Reset`, else `SetResolution` | HAL reset | rare | our HalDevice | C (keep) |
| `eCFrustumDatabase::UpdateFromAppTemplate(cam +0xf8)`: caps `+0x1590..+0x15b7` from RSA `+0x70..`, `cam +0xc0` = caps far | Request (mask bits), cull, c11.w / PS c0.w, fog clamp | frame | read | K |
| camera rect `+0xc4..+0xd0` = back buffer (editor: client rect) | ExecuteGfxCamera viewport, SCREENSIZE, scene target size, render-scale hook | frame | read | K |
| vtbl +0x108 UpdateProjection(true) -> Update(2): aspect `+0xd8`, CalcProjectionMatrix (`+0x50`, gfx camera `+0x60` / `+0xe0` / `+0xa0`) | culling, composition, shadow fit | frame | read | K |
| Update(1): CalcViewMatrix (`+0x10`), `eCGfxCamera::SetViewMatrix` (`+0x20`, `+0x120`, dirty) | as above | frame | read | K |
| GetBackBuffer + SetRenderTarget(bb, depth) + mixer ClearViewport(7, 0, 1.0, 0) | device | frame | engine (HAL) | D, but the clear is the only content of menu / loading frames: our executor must clear |
| vtbl +0x5b8 OnPreRender = `eCPrefetchAdmin::CacheEntities` | streaming | frame (our stride) | engine | S / C (keep) |
| `eSDebugOptions +0x50` extra clear | debug | never | - | D |
| app state `& 3` gate; scene effect `+0x90` Execute (or `eCCameraBase::Render(NULL)`), overlay `+0x98` Execute | the world, post | frame | hooked | replaced by the executor (sections 3 - 6) |
| GetBackBuffer + SetRenderTarget again | the GUI's target | frame | engine | D (our executor binds the back buffer) |
| vtbl +0x5b0 OnPostRender: `eCModuleAdmin::DoPostRender` (focus module: indicator draws, section 1) then vtbl +0x568 Draw | GUI, present | frame | engine | keep |
| `eCTimer::Update((state \| state >> 3) & 1)` | everything (game time, frame time) | frame | engine | S (keep) |

### 2.2 `eCApplication::OnPostRender` 0x4c06c0, `Draw` 0x4c13b0, `SwapScreen` 0x50fc90 [C]

* OnPostRender: `eCModuleAdmin::DoPostRender` (modules' vtbl +0x170; in the game only `gCFocusModule2::OnPostRender`: section 1) then
  Draw. Keep: the indicators' meshes are engine draws in their own bracket (`BeginMaterial` ... `EndMaterial`, their own camera; they
  need the admin outside material / z pass, i.e. our executor must leave `+0x311` / `+0x312` clear, as Execute does).
* Draw: `gCGameApp` vtbl +0x5c0 OnRenderDraw -> `eCGuiModule2::Draw` (section 7); `DoSnapshot` 0x4c0c50 when `app +0x144`
  (SaveBackbufferToFile after the GUI: K, keep); `eCGfxMixerAdmin::SwapScreen` -> our HalDevice present (keep).

## 3. The scene job and the main camera's extraction

### 3.1 `eCImageFilterScene::OnUpdateOutput` 0x75cf10 [C] (frameF_dump2 1531)

| write / call | class |
|---|---|
| vtbl +0x150 = `UpdateRenderTargets` 0x75d140: scene images `+0x38` FP16 / `+0x40` copy / `+0x48` R32F depth, recreated on format / size change | C (keep: the desc `{+0x40, +0x48}` and the material admin's holders point at them) |
| `eCCameraBase::Update(current camera)` 0x4f7270 | K / S (3.2; keep) |
| GetRenderTarget, SetRenderTarget(scene `+0x38` handle, saved depth) | D (ours) |
| `eCFrustumDatabase::Execute(cam db, filter +0x40)` | replaced (section 4) |
| GetViewport, restore RT / viewport (HDR) or `CopyCurrentRTToImage` into `+0x40` (non-HDR) | D (ours) |

### 3.2 `eCCameraBase::Update` 0x4f7270 [C] (d1.txt 217)

| write | reader | class |
|---|---|---|
| `RSA +0x64 ++` when caps `+0x15a1` | `eCRenderSystemAdmin::GetTimeStamp` 0x66c360 (no direct caller found) | K? keep (free) |
| `DAT_180c628c0 ++` (extraction stamp -> `entity +0xe0`) | ExtractFromEntity / AddShadowCasterEntity dedupe | K |
| db `+0x20 = 0`, db frame `+8 ++` | every per-db-frame stamp: visible stamp, `res +0xa4` (instances), SpeedTree `+0xe0` / `+0xe4`, decals `entity +0xe4 == db +8`, the pre-pass globals `DAT_180c62884` / `+88` | K |
| `UpdateFrameDependencies` (frustum, sphere, beam, depth-key plane `+0x6c..+0x7c`) | culling, sort keys | K |
| `ExtractFromFrustum` (portals, occluders, sphere tree; counters `RSA +0x98..+0xa8`) | the candidate set; counters only for statistics | K (counters D) |
| `PreExecute`: RequestPrePasses (dynamic light: one op per db frame; point lights: `AddShadowCells` -> atlas cells) | the atlas, PSSM | K |
| `PreExecute`: RenderPrePass ops (UpdateShadowMaps -> terrain map + cascades; `eCPackedShadowMap::RenderShadowMap`) | the shadow views | split (section 5) |
| `Request` -> `RequestRenderOps`: list counts 0, per item `CalculateFadeOut`, `eCFrustumElementItem::Request` | | |
| -- bit 0x80 `eCIlluminated_PS::UpdateIllumination` (light sets, static lights / SH, outdoor factor eased at 1/0.3 per s) | our lighting composition, RT GI lights | S + K |
| -- bit 0x40 visible stamp `item +0xc = db +8` | decals; **Game.dll camera + AI** (section 1) | K (gameplay) |
| -- bit 1 vtbl +0x30 UpdateLoD (`ctx +0x28`, PrefetchData of the wanted level) | the op's resource at execution; streaming | K + C |
| -- vtbl +0x40 RequestRenderOps: the lists (keys, sort ids, fade); resource / material QueueData / CacheResource; `AddRenderInstance`; SpeedTree queues + `UpdateWindMatrices`; billboard admin queue; vegetation grid nodes (+ `SortGridNodes` qsort); particle `CreateMaterial` | our composition; streaming; wind | K / C / S |
| profile timings `RSA +0x108..+0x130` | statistics | D |

No step exists only to feed the render functions we replace. Note for the shadow databases (section 5): their extraction runs with the
shadow camera as the **application's current camera** (`SetCurrentCamera` swap in `eCShadowMapBase::RenderShadowMap` /
`eCCameraBase::RenderShadowMap` 0x4f71c0) because `UpdateFrameDependencies` reads `app +0x38` [C]: keep the swap.

## 4. `eCFrustumDatabase::Execute` 0x66e990 [C] (d1.txt 307)

### 4.1 Step by step

| # | step | reader of its effects | class | lean frame |
|---|---|---|---|---|
| 1 | RSA stats `+0xb0..+0xc8` += list counts; `local = DAT_180c62814` (a call counter for stat deltas) | eCStatistic / debug | D | drop |
| 2 | `ExecuteGfxCamera(db +0x14c0)`: GetViewport, SetViewport(cam rect, 0..1), `eCGfxCamera::Execute`: `ms_pActiveCamera` = it, `eCGfxCameraStates::Execute` (device slot 0x268 **SetStencilTesting**), SetViewAndProjectionMatrices, `SetCamera`: admin `+0x184` VP, `+0x1c4` depth-bias VP (read by the composition), SetCamObstacleDistances, VS pool c0..c11, PS c0 / c1 | composition (`+0x1c4`, `ms_pActiveCamera` for particles / sky), the engine's own later draws (pools), device stencil state | K | keep (call it) |
| 3 | `eCGfxFog::Execute(db +0x14c8)`: `ms_pActiveFog`, SetFog, admin PS c3 / c4, VS c30 | fallback draws (composition reads the fog object) | K | keep |
| 4 | GetRenderTarget / GetViewport; depth map handle; ClearViewport(h ? 1 : 7, 0xff000000) | device | D | ours |
| 5 | material admin depth holder `+0x290` released, = NULL | UpdateShadowMask gate, Texture filters mode 2, s2 | K | keep (`SetDepthmap` / FUN_1806bc540) |
| 6 | z pre-pass (caps `+0x1590`): SetRenderTarget(depth map), Clear(7, 0xffffffff), `BeginZPass(NULL, true, h != 0)`, **qsort `+0x1518`**, slot +0x18 RenderZPass per op, `EndZPass`, EndScene, RT / viewport restored | 4.2 | brackets K, sort 4.3, loop -> executor | |
| 7 | `FUN_1806bc540(admin +0x270, desc +8)` (= SetDepthmap: the depth image AddRef'd into s2's holder) | mask chain, s2 of every material | K | keep |
| 8 | `eCIlluminationAdmin::UpdateShadowMask` | section 5.5 | mixed | lean entry |
| 9 | BeginScene; `BeginMaterial`; `DoBeginRenderSolid` (modules +0x150 if overridden: none in the game) | 4.2 | K / D | keep the bracket; modules optional |
| 10 | **qsort `+0x1500`**, slot +0 Render per op | | | executor |
| 11 | debug list `+0x1560` inside EndMaterial / BeginMaterial (only `/Engine/Render/RenderDebug`) | | D (never) | engine fallback if met |
| 12 | debug boxes (caps `+0x1594`, ToggleBoundingBoxes): master depth write / test saved, DrawBox, restored | | D (never) | engine if toggled |
| 13 | every enabled module's +0x158 OnEndRenderSolid: only `eCPhysicsScene::OnEndRenderSolid` -> `DebugRender` 0x5e5100, which with its flag `+0x18` draws two line lists **and frees them** (`+0x1a0` / `+0x1b8` / `+0x80` arrays) | physics debug | D, but keep the call (cheap) so that a debug flag cannot leak the arrays | keep |
| 14 | `eCDecalAdmin::RenderDecals(db)`: render queue `+0x38` / `+0x40` rebuilt each call (`+0x40 = 0`), qsort, lock cycles | | D (rebuilt per call) | ours |
| 15 | `eCBillboardAdmin::RenderBillboards`: qsort of the queue `+0x20`, **`+0x28 = 0` at the end** | the queue is filled by Request (solid billboards) | K duty | ours + reset `+0x28` |
| 16 | list `+0x1530` slot +0 (the sky) | | | executor |
| 17 | refraction copy (desc `+0` and gfx admin `+0x5b8`): EndScene, `EndMaterial`, CopyCurrentRTToImage, **`SetRefractmap` holder `+0x248`**, BeginScene, `BeginMaterial` (re-binds s1) | s1 of the refracted / alpha ops, Texture filters mode 1 (EdgeSmooth) | K | keep holder + brackets; copy ours |
| 18 | `DoBeginRenderAlpha` (none overridden), list `+0x1578` slot +0x10 (refracted, unsorted), **qsort `+0x14e8`**, slot +8 RenderAlpha, `DoEndRenderAlpha`, list `+0x1548` slot +8 | | | executor |
| 19 | EndScene, `EndMaterial`, refraction holder image `+0x268` released = NULL; profile counters | holders | K | keep |

### 4.2 The brackets [C] (frameC_dump1)

* `BeginZPass` 0x6b9720 (refuses inside a material / z pass): render states `{alpha test from the struct, blend off, master depth
  write mode param + 1, cull master 0, depth func 4, colour / alpha write = "not a depth format"}` through `SetCurrentRenderStates`
  (SetMasterBlending / DepthWrite / RenderMode (`+0x340` fill master) / DepthTesting / ColorWriting / AlphaWriting; PS pool c5.w for
  alpha-test mode 2); admin `+0x313` = depth-write flag, `+0x314` = pass type (`map vtbl +0xf8`, 0 without a map), `+0x312 = 1`;
  SetMasterFogging(1), SetMasterCullingMode(0); `ms_pActiveShaderConstants = ms_pActiveShader = 0`, `DAT_180c62990`, `0xc62950/58`
  = 0, the global pool's and the lighting pool's `+0x48` = 0, both admin pools marked dirty (FUN_1806b5d20). **Device slot 208
  BeginRenderStateBlock** first.
* `EndZPass` 0x6b9910: the same cache resets, **slot 209 EndRenderStateBlock**, SetMasterRenderMode(0), SetMasterDepthTesting(0),
  SetMasterDepthWrite(0), `+0x312 = 0`, `+0x314 = 6`.
* `BeginMaterial` 0x6b9d20: BeginRenderStateBlock, SetMasterFogging(1), SetMasterCullingMode(0), the cache resets and dirty pools,
  `ExecuteShaderSamplers` 0x6b7bc0 (binds the admin's s0 / s1 / s2 / s4 / s5 records with their sampler states), `+0x311 = 1`.
* `EndMaterial` 0x6b9e40: EndRenderStateBlock, the three master resets, the cache resets, **SetTextureNULL(0..5)**, `+0x311 = 0`.

Our HAL implements the block (`HalDevice::BeginRenderStateBlock` / `EndRenderStateBlock`, device.cpp 1820): it records the first old
value of each render state **set through the HAL** and restores it at the end. So in the engine, every render state an op changes
inside Execute reverts at the next EndZPass / EndMaterial: the mask chain, the post filters and the GUI inherit the pre-block states
(fx passes only set what they declare). Readers of the brackets' state: the composition reads `+0x312` (actors), `+0x313` and `+0x314`
(z passes), `+0x340`, the admin sampler records; the engine's own later draws (fallback ops, focus indicators, decals of a left
group) rely on the cache resets and dirty pools. **Consequence for a lean frame**: (1) keep calling the four brackets at Execute's
places (they are cheap HAL setter calls); (2) because our composed draws set state on `d3d9::Device` directly (not through
`HalDevice::setRenderState`), the block does not see them: today's per-op save / restore (of the 10 render states the composition
owns) hides that; without it, our executor must
restore the render states it changed at the bracket end itself (e.g. a `d3d9::Device` render-state snapshot at BeginRenderStateBlock,
restored at EndRenderStateBlock), or the post / GUI composition (which evaluates fx passes on the inherited device state) drifts from
the engine's; (3) textures 0..5 unbound at EndMaterial and the stencil state of `eCGfxCameraStates` likewise.

### 4.3 The three sorts and the loops

* Sites [C]: z `qsort(+0x1520, +0x1528, 0x18, 0x66e670)`, solid `(+0x1508, +0x1510, 0x66e6a0)`, alpha `(+0x14f0, +0x14f8, 0x66e600)`;
  shadow `(+0x1520, +0x1528, 0x66e670)` in ExecuteShadowMap. In place, deterministic (MSVC qsort, byte-identical input). The engine's
  `DepthSort` 0x66e030 / `Sort` 0x66e090 / `ReverseSort` 0x66e060 wrap the same three calls (unused by Execute).
* Loops [C]: `for i < count: (*data[i].ctx)->slot(data[i].ctx, &data[i], db)` with the count **re-read each iteration** (`*(int*)(this
  + 0x1528)`): a slot that appended to the list during the loop would extend it (the tap's "list count changed during Execute" check
  has stayed 0). The executor should walk the live array the same way.
* Lean: sort in place with the engine's qsort + comparator (one sort), walk the array, compose each op; the FrameDesc / attribution
  layer of the tap becomes optional (debug), the copy and the per-view `raw_` buffer go away.

## 5. The shadow views

### 5.1 `eCDirectionalLight_PS::UpdateShadowMaps` 0x591fb0 [C] (frameA_dump1 2071)

| write | reader | class |
|---|---|---|
| shadow direction `+0x8c..+0x94` := true direction `+0x80` when acos(dot) > 0.5 deg (stepping) | cascade object LightDirection, the terrain re-aim test, `engine::shadowmaps` | S (stepped state) + K |
| terrain re-aim test: camera position in light space (fmodf by the engine's CRT), `|d| > +0x9c x 2 +0x98 / size` or the direction stepped -> `+0xa0..+0xa8` = the new centre | next frame's test | S (hysteresis) |
| on re-aim: terrain map `+0x60` = LookAt(dir), `+0x1c..` box = camera sphere in light space, vtbl +0x170 RenderShadowMap | the terrain map's UpdateCamera, the mask's VSSM matrix | K + render (5.3) |
| cascade object `+0x10..+0x18` = shadow direction (property notifies), `RenderShadowMaps` | the fit | K + render |

Keep the function; only its render calls change (5.2 - 5.4). Our `debug.shadow_stride` pre-hook skips `RenderShadowMaps` on still
frames: the splits and maps keep the previous frame's state, which the lean executor must not clear.

### 5.2 `eCCascadeShadowMap::RenderShadowMaps` 0x59f200, `UpdateCamera` 0x5a13e0 [C] (rvk_sunshadow_dump.txt)

* The fit (pure CPU, SharedBase / CRT maths): `MaxDistance +0x1c` initialised from the camera far once (C); per split `+0x160` start
  (K: mask FadOut / ZDistance), `+0x60` light view, `+0xe0` box (min z -100000), `+0x100` the world slice frustum (K: the shadow
  database's beam through `eCShadowMapDirectionalLight2Ex::UpdateFrustumFilter` 0x5a1870 = shadow culling) -> vtbl +0x170.
* UpdateCamera: the shadow camera (`split +0x58`): Perspective off, position `+0x90`, ClipDepth (0, max z), db `+0x1694` = max z
  (depth-key scale of the shadow list), view / projection with **UpdateViewport(false) / UpdateProjection(false)** (the gfx camera's
  `+0x20 / +0x60 / +0xe0 / +0xa0`, dirty), texture matrix `+0xa0` = bias x P x V (K: the mask's DepthToShadowMatrix, our enhanced mask,
  RT shadows). All K: keep (cheap); our group 2048 already only compares them.

### 5.3 `eCShadowMapBase::RenderShadowMap` 0x5a1070 and the terrain map's 0x5a1b40 [C]

| call | class | lean frame |
|---|---|---|
| vtbl +0x118 CreateRenderTarget (eCDepthMap 0x59fd00: the map image, pooled per desc) | C | keep |
| +0x128 SetRenderTarget (0x59fee0: GetRenderTarget / viewport saved at `+0x30/+0x38/+0x40`; depth formats bind `GetShadowMapWorkingBuffer` (a per-size colour buffer created on first use, C) + the map; SetViewport) | C + D | keep the C part (first use), device calls ours (group 2048 issues them already) |
| +0x120 ClearViewPort (0x59fe50) | D | ours |
| +0x160 UpdateCamera | K | keep |
| +0x168 RenderShadowScene: `SetCurrentCamera(shadow cam)`, `eCRenderSystemAdmin::RenderShadowMap` 0x66ce00 = `ExtractShadowMap` 0x66f470 (db frame ++, extraction stamp ++, frustum from the **current** camera, vtbl +0x150 UpdateFrustumFilter, casters: tree walk or caster list) + `RequestShadowMap` 0x66f5e0 (mask `0x10 \| LoD`) + OnPostExtraction + `ExecuteShadowMap`; camera restored | K (lists, LoD re-pick, SpeedTree z queue) | keep, ExecuteShadowMap replaced (5.4) |
| +0x130 ResetRenderTarget (0x5a00e0 / the inline restore) | D | ours |
| terrain map only (`eCShadowMapDirectionalLight2VS::RenderShadowMap` 0x5a1b40): first call creates the texture filter `+0x118`, the `post_blur.fx` effect filter `+0x110` (SetEffectFileName: compile, C) and the temp image `+0x108` (G32R32F, C); per call: technique by name -> `+0xf0`, `eCImageFilterBase::UpdateOutput(-1)` twice (Blur7H into the temp, Blur7V back into the map), SetTexture / SetRenderTarget, the input texture filter cleared | C + D | keep (thin: its ExecuteEffect replaced, section 6) |

### 5.4 `eCFrustumDatabase::ExecuteShadowMap` 0x66f630 [C]

RSA stats `+0xb4 / +0xb8` (D); `ExecuteGfxCamera(shadow cam)` (K: ms_pActiveCamera, admin camera pools, viewport = the cell rect for
the atlas; keep); BeginScene (D); **qsort z list** (one sort, ours in place); `BeginZPass(map, true, false)` (K: `+0x313` / `+0x314` =
map vtbl +0xf8, masters, block; keep); per op slot +0x18 (executor); `EndZPass` (keep); profile (D); EndScene (D).

### 5.5 Point-light atlas, `UpdateShadowMask` and the mask filter [C]

* `eCPackedShadowMap::RenderShadowMap` 0x5a08f0: CreateRenderTarget (C), SetRenderTarget(6) + ClearViewPort once (D / ours), per cell
  vtbl +0x180 UpdateCamera 0x5a09b0 (K: Perspective off, ClipDepth cell `+0x114 / +0x118`, db `+0x1694`, view / projection, Screen rect =
  the cell viewport; keep) + `eCCameraBase::RenderShadowMap` (camera swap + extract / request / ExecuteShadowMap), ResetRenderTarget (D).
* `eCIlluminationAdmin::UpdateShadowMask` 0x5748d0 (rvk_sunshadow_dump 4): if the admin's depth image `+0x290` exists:
  `eCImageFilterTexture::SetTexture(illum +0x460, depth)` (the graph's input; AddRef), `UpdateOutput(mask or its Blur +0x458 with
  HighQuality)`, **`FUN_1806bc540(admin +0x298, output)`** (= SetShadowMask: s0 of every material: K for our composition's admin slot
  and for engine fallback draws), texture filter `+0x40` released, **`eCPackedShadowMap::ClearCells(illum +0x380)`** (K duty: the
  cells added by this frame's RequestPrePasses must be cleared every frame, or the quadtree fills and the PSSM pass grows); else the
  admin's mask image `+0x2b8` released.
* `eCImageFilterShadowMask::OnUpdateOutput` 0x668560: UpdateColorOutput (C: the mask target from the pool); ClearViewport(1,
  ClearColor `+0x110`) / BeginScene (D); `SetTechinque` -> `+0xf0` (D: only its own ExecuteEffect reads it); the parameter objects'
  values through their +0x70 Set (`+0x58`: the carry-over store of the engine's next call; our composition already composes all of
  them natively, so in a lean frame our executor keeps its own store); per unit `ExecuteEffect` (replaced); the PSSM part:
  `UpdatePackedShadowMapVisual` 0x669e00 (one DISCARD lock + the pyramids; D, composed natively) and the pass loop **only when it
  returns true** (`bVar6 = UpdatePackedShadowMapVisual(..); if (bVar6) { UpdateEffect; BeginEffectTechnique; record +0x1c = 0; per cell
  ... }`, rvk_sunshadow_dump2.txt line 726) [C]; inputs reset, RT restored (D). Minimal entry: our PSSM composition at
  UpdatePackedShadowMapVisual's detour, then **return false**: the engine skips its whole PSSM loop (stencil, three passes per cell).
* The 1/4 depth copy (`eCImageFilterCopy::OnUpdateOutput` 0x665640) and the SSAO filters are jobs of the mask's own UpdateOutput: thin
  as the post chain (section 6).

## 6. The image-effect graphs [C] (frameF_dump1..5, imgparam_dump)

| function | writes / calls | class | lean frame |
|---|---|---|---|
| `eCImageEffect::Execute` 0x662630 | a local job list (AddJobs per filter, freed at the end) | D | thin: keep |
| `eCImageFilterBase::AddJobs` 0x665370 | `AddRefOutputAt` on each input's source (RTBase `+0x88 ++`) | K (balances the release) | keep with ExecuteJobs |
| `ExecuteJobs` 0x664d30 | per job OnUpdateOutput (vtbl +0x138), then `ReleaseOutputAt` per input: RTBase `+0x88 --`; at 1 with ShareTexture `FreeRenderTarget` (pool entry in-use = 0) + `+0x70 = NULL` | K (pool) | keep |
| `eCImageFilterRTBase::UpdateColorOutput` 0x6667f0 | output size `+0x80 / +0x84` (input size x scale, ceilf), the output image: kept when format / size match (persistent filters), else `AllocRenderTarget` 0x663950 (first free equal entry, in-use 1, stamp `admin +0xb4`; or a new eCGfxImageData + CreateLayers) | C + K (TEXTURESIZEn, viewports, the images our draws target) | keep |
| `eCImageFilterAdmin::Process` 0x663080 (module Process, before DoRender) | overlay elapsed / weight `+0xac / +0xb0`, stop at duration; **frees pool entries with in-use 0 unused > 1 frame**; `+0xb4 ++` | S + C | stays (not render code); if the graph walk were dropped, the shared targets would be freed after two frames: an executor without the walk needs its own targets |
| `eCImageFilterEffect::OnUpdateOutput` 0x7597a0 | UpdateColorOutput; vtbl +0x178 UpdateVisual 0x75adf0 (the quad's VB / IB / declaration, once: C); +0x180 UpdateEffect 0x75aec0 (lazy fx compile through `CompileEffectFromFile`: C, the source of our EffectInfo); +0x148 SetRenderTarget (saves RT / viewport, binds `+0x70` with the saved depth: D + K for Reset); BeginScene; +0x1b8 ExecuteEffect; EndScene; +0x150 ResetRenderTarget | C / D | keep, ExecuteEffect replaced |
| `ExecuteEffect` 0x75af70 -> `BeginEffectTechnique` 0x75b090 | `OnSetParams` 0x75c740: every used parameter object's +0x28 Set and every input's `AttachImmImageData` (AddRef + the API effect's SetTexture); SetTechnique + BeginTechnique (our HAL effect: BeginRenderStateBlock) | D (the values are pure reads of eCTimer `+0x2c` / `+0xc`, weather `+0x158..+0x188`, image filter admin `+0xb0`, the app camera, the sky dome; nothing is stored) | replaced: our composition computes them natively; **the API effect's "current values" (the carry-over our post composition starts from today) are then no longer updated by the engine**: the executor needs its own per-API-effect value store, applied in the engine's order |
| `ExecuteEffectPass` 0x75b1a0 -> `eCGfxEffect::Execute` 0x750210 | `ms_pActiveEffect` (D: `ms_bOptimize` is false; the toggle at 0x50f824 / 0x50f887 sets camera / effect / fog together [C], so camera and fog are not cached either [I]); SetPassNumber | D | replaced |
| `ExecuteVisual` -> `eCGfxVisual::Execute` 0x5069a0 | master render mode / cull / depth test / depth write, the quad | D (masters: device-state K) | replaced |
| `EndEffectTechnique` 0x75b230 | `OnResetParams` 0x75c8a0 (inputs detached, refs released; parameter +0x30 Reset), EndTechnique (EndRenderStateBlock) | K (refcount balance with the attach: if neither runs, balanced) | replaced |
| semantic Set functions (0x289aa0 FrameTime, 0x2899c0 Time, 0x758ad0 / 0x758900 / 0x758880 / 0x758800 weather, 0x758980 ColorMatrix, 0x758780 BlendValue, 0x289b10 Camera, 0x289fc0 SkyDome, 0x28a310 ImageSize, 0x289790 Texture, 0x289390 Image) | read + `SetParameter(handle, value, n)` only; ImageSize / SkyDome set nothing when no match / no sky dome (the carry-over case) | D | ours |

Persistent targets (ShareTexture false: AdaptiveLiminance 1x1, AvgLuminance Copy 1x1, Bloom 309x210): their images live in the
filters' `+0x70` and are never freed by the pool (in-use stays 1) [C]; with the thin walk they stay the engine's, so a fallback to the
engine's post (or a switch Full -> Wrapper) continues the same adaptation state. Reset only by `OnResetResources` (device reset /
resize), as the engine does.

The overlay effect (`+0x98`) is the same machinery without a scene job; OverlayRadialBlur's `eCImageFilterCopy` without input
(CopyCurrentRTToImage) stays a HAL copy (not a draw).

## 7. The GUI [C] (frameF_dump3, Game.dll imports)

| function | writes / calls | class | lean frame |
|---|---|---|---|
| `eCGuiModule2::Draw` 0x519a60 | timestamp `+0x44` (stats); cursor -> CFXGui vtbl +0xe8; CFXGui +0x68 update(`eCTimer +0x2c`); BeginScene; GUI device scale / offset `+0x1e0..+0x1f4` from the module; CFXGui +0x70 Draw (GUI2.dll walks its windows and calls `eCGuiDevice2` Draw*: **the sprite records**, font atlas / deferred bitmap work); EndScene | S (GUI time / input) + K (records) + C (glyphs, bitmaps) | keep |
| `eCGuiDevice2::BeginFrame` 0x517290 | `CreateSprite` (C); `eCSprite::Begin(0)`; device view = scale x translate (slot 0x288, read back by Flush via slot 0x290); listeners' OnBeginFrame (`+0x1d0` array: Game.dll `gCGUIEntityRenderer`, registered through `RegisterDrawCallback`) | C + K | keep |
| `eCSprite::Begin` 0x757ff0 | mesh capacity / OnResetDevice (C, first use), `eCSpriteEffect::Create` (inline HLSL, C), world / view identity (slots 0x278 / 0x288), GetViewport -> `+0x48` = `D3DXMatrixOrthoOffCenterLH`, `+8` 2x2 identity, `SetBlend(0)`, flags `+0` | K (`+0x48`, the device view, the blend state of the first batch) | keep |
| `eCGuiDevice2::EndFrame` 0x517130 | Flush; view matrix restored; listeners' OnEndFrame (**Game.dll `gCGUIEntityRenderer::DrawScreenMessages`: more records + its own Flush**); Flush again; mesh `+0xd8 / +0xdc` (count, ring start) = 0; texture detached | K | keep (the Flushes replaced) |
| `eCSprite::Flush` 0x757d10 | qsort_s for flags 0xc0 / 0x20 (dead in the shipped game, README 16); SetTechnique / BeginTechnique; WVP = sprite `+0x48` x device view through SetParameter; RenderBatches; EndTechnique; **`+0xb0 = 0`** | D except the record reset | replaced: compose from the records, issue, then `+0xb0 = 0` |
| `RenderBatches` 0x757180 / `DrawBatch` 0x7570b0 / `eCSpriteMesh::Draw` 0x528650 | batch state `+0x88 / +0x90 / +0x98 / +0x9c` (reset per flush), **one VB Lock per quad** (DISCARD at 0 else NOOVERWRITE into our HAL buffer), SetBlend (alpha blending + SetMasterBlending: device state that the next flush's first blend-0 batch inherits), texture DeAttach / Attach (refcount), `eCGfxEffect::Execute`, the draw record and `eCGfxVisual::Execute`, mesh `+0x24 += count` | D (device state across flushes: K for the next flush; our executor tracks it) | replaced |
| `SwapScreen`, `DoSnapshot` | present, BMP | K | keep |

Loading screens (`eCGuiModule2::RenderEdit` 0x519c90) and videos (`eCApplication::PlayVideo` 0x4c1b00) use the same Draw / Flush
(their clears are device work: ours).

## 8. Gameplay readers in the other DLLs (import scan, `optframe\impcalls.py`) [C]

| import | Game.dll / Script.dll user | consequence |
|---|---|---|
| `eCFrustumElementItem::WasVisibleLastFrame` (`item +0xc == db +8`) | Game.dll `gCCameraAdmin::SetCameraTargets` 0x7a9280, `CalculateTargetPositionOffset` 0x7aa9c0 (when the target is not visible it forces `eCWrapper_emfx3ActorInstance::UpdateTransformations` before reading Bone_Head_Head_1), FUN_1807c7530 (head-bone position), `gCScriptProcessingUnit::UpdateChannel` 0x96b310 (two sites) | the main Request's bit 0x40 is gameplay input: keep Request [C sites; I: the bone transforms are otherwise only refreshed by rendering, slice D] |
| `eCGfxMixerAdmin::GetActiveCamera`, `eCGfxCamera::Execute` / `SetViewMatrix` / `SetProjectionMatrix`, `BeginMaterial` / `EndMaterial`, `eCMeshResource2::Render` | the focus indicators (section 1) | `ms_pActiveCamera` is only saved / restored by them; the admin must be outside material / z pass at OnPostRender |
| `eCGuiDevice2::RegisterDrawCallback` / `DrawImage` / `DrawTextW`, `eCSprite::DrawQuad` / `SetTransform` / `Flush` | `gCGUIEntityRenderer` (screen messages), indicators | records through the replaced Flush |
| `eCImageFilterAdmin::StartOverlayEffect` / `StopOverlayEffect` | Game.dll `gCEffectCommandStartImageEffect::OnStart` 0x828550, gCCameraAdmin region 0x7b429d; Script.dll | the overlay graph's activation: unchanged |
| `eCFrustumDatabase::GetCaps` / `ClearExtractedItems`, `eCRenderSystemAdmin::GetFrustumDatabaseCapsTemplate` (Script_Game) | `gCNavigation_PS::OnRenderDebug`, `gCWorld::OnReleaseData` | not frame work |

## 9. The lean frame skeleton (proposal)

**Principle**: the engine keeps every decision and every S / C / K write; our executor replaces the loops that call render slots,
the non-op render paths and the device work, at the engine's own call sites (the existing detours), and walks the engine's lists
after **one** in-place sort. A view is executed where the engine executes it (the per-op mutable context state is only valid
there, section 10).

| engine function | lean frame |
|---|---|
| `eCApplication::Process` (modules: weather, illumination incl. light sets + sky cube state machine, image filter admin, ...), `eCTimer::Update`, OnPreRender | run as is |
| `DoRender` | run as is (its device calls land on our HAL; the scene effect call goes to our executor) |
| `eCImageEffect::Execute` (scene, overlay) | run as is (thin walk: jobs, pool targets, UpdateVisual / UpdateEffect, SetRenderTarget) |
| `eCImageFilterScene::OnUpdateOutput` | run as is (UpdateRenderTargets, camera Update, Execute -> ours) |
| `eCCameraBase::Update` (Extract, PreExecute, Request) | run as is |
| `UpdateShadowMaps`, `RenderShadowMaps` (fit), `UpdateCamera`s, `RenderShadowScene` (camera swap, ExtractShadowMap, RequestShadowMap), the shadow-map CreateRenderTarget / working buffer, `eCPackedShadowMap::RenderShadowMap`'s per-cell cameras | run as is; their SetRenderTarget / Clear / Reset device calls issued by us (group 2048 already does) |
| `ExecuteShadowMap` | **replaced**: `ExecuteGfxCamera(db cam)`, `BeginZPass(map, true, false)`, qsort z list in place (engine qsort / comparator), executor over the list, `EndZPass` |
| `eCFrustumDatabase::Execute` | **replaced** by a re-implementation of 4.1 that calls the engine's own camera / fog / bracket / holder / module functions in Execute's order and runs the executor instead of each slot loop; decals / billboards composed (+ the billboard queue `+0x28 = 0`) |
| `UpdateShadowMask` | run as is; inside it `ExecuteEffect` of every mask unit replaced, `UpdatePackedShadowMapVisual` = our PSSM composition + return false; `ClearCells` and the `+0x298` holder stay the engine's |
| `eCImageFilterEffect::ExecuteEffect` (mask, terrain blur, post, overlay) | **replaced**: our composed draws with our own per-effect value store (no OnSetParams / API effect calls); the persistent targets stay the filters' images |
| CFXGui Draw, BeginFrame / EndFrame, listeners, `eCSprite::Begin` | run as is |
| `eCSprite::Flush` | **replaced**: compose from `+0xa8 / +0xb0`, issue from our buffers, `+0xb0 = 0` |
| OnPostRender (focus indicators), `DoSnapshot`, `SwapScreen` | run as is |

The executor per op (the slots' own S / C / K duties are the other slices' "minimal entry points": material / image `RequestData`
touches, lightmap binds, SpeedTree queue sort + stamp reset, instance sort + `res +0xa4 = 0`, actor palette stamp, particle update
tick, ...): resolve -> compose -> issue; an op the composition leaves (permutation not compiled, buffers not created, an
unknown kind) runs **the engine's slot** with `&data[i]` inside a re-validated bracket (section 9.1).

### 9.1 Engine fallback inside the executor

An engine slot (or a non-op engine path: debug list, debug boxes, a disabled group) can run in the middle of our executor if, right
before it: (a) the bracket of its pass is open (`+0x311` or `+0x312` as Execute would have it) - the brackets kept, this holds; (b)
the engine's constant / shader caches are invalid after our draws: our draws overwrite registers the engine believes uploaded (its
pools upload dirty ranges only; the lighting pool rewrites c14..c37 only on light-list changes), so call `EndMaterial` +
`BeginMaterial` (or `EndZPass` + `BeginZPass(same map, true, colour)` in a z pass) before the fallback slot: they reset
`ms_pActiveShader` / `ms_pActiveShaderConstants`, the lighting / global pools' `+0x48`, mark the admin pools dirty and re-bind the admin
samplers, at the price of one state-block cycle [I: this is exactly what Execute does around the debug list]; (c) the camera / fog
pools hold this view's values: `ExecuteGfxCamera` / `eCGfxFog::Execute` ran at the view's start and no other camera executed since
(the sky dome's camera hack inside a fallback restores itself) [C]. Today's per-op device save / restore (shaders, declaration, streams,
index buffer, the 10 owned render states, every stage's texture + 7 sampler states, all VS / PS float registers; `StaticOps::issue`,
static_ops.cpp 1586 - 1718) is then no longer needed for the engine's sake; only the
render-state block semantics of 4.2 must be reproduced.

### 9.2 Can the executor drive the whole frame from the extracted lists (one sort)?

Yes per view, not as one deferred pass: the main lists exist from the main Request to the end of Execute and the shadow lists from
their RequestShadowMap to the end of their ExecuteShadowMap; each is sorted once by us in place and consumed there. Recording the whole
frame later (e.g. from a FrameDesc after the scene job) would need a capture of the mutable per-op state at the view's time (10.1);
the CPU work would be the same, so executing at the engine's call sites is simpler and keeps the command order the engine's.

## 10. Ordering constraints

1. **Shadow views before the next Request**: `RequestRenderOps` of each database re-picks state shared by all views - the mesh LOD
   `ctx +0x28` (from the db camera: shadow databases pick from the shadow camera), the SpeedTree z queue (`+0xd0 / +0xd8` reset by the
   next database's request), the instance arrays (`res +0xa4` per db frame); the actor palette is stamped at the first Render of a
   frame. So a shadow view's ops must be composed (or their mutable state captured) inside its ExecuteShadowMap, before the next
   cascade / the main view requests [C: B-frustum-db.md 2.5, deviation 11].
2. Inside PreExecute's one dynamic-light op: [terrain map + blur] -> cascades -> [point atlas] (UpdateShadowMaps, then the point
   light's pre-pass op), all after the main extraction and before the main Request; the shadow extraction needs the
   `SetCurrentCamera` swap (UpdateFrameDependencies reads `app +0x38`).
3. Main view: camera + fog -> clear -> z pass (BeginZPass ... EndZPass) -> `SetDepthmap` (`+0x270`) -> UpdateShadowMask (needs `+0x290`;
   SSAO / cascades / VSSM / PSSM; then `+0x298` and ClearCells) -> BeginMaterial -> solid -> [debug] -> module OnEndRenderSolid ->
   decals (visible stamp of this frame) -> billboards (+ queue reset) -> `+0x1530` sky -> EndMaterial, refraction copy, `SetRefractmap`,
   BeginMaterial (s1) -> refracted -> alpha -> late list -> EndMaterial -> `+0x268` released.
4. Post after Execute and the scene-job restore; within it the engine's post-order job list (inputs first; Adaptive before the Copy;
   GlowVAddBloom2 onto last frame's bloom); once per world frame only (the eye adaptation / bloom state is the GPU targets').
5. GUI after DoRender's back-buffer rebind and OnPostRender (focus indicators); EndFrame's listener callbacks between its two Flushes;
   snapshot after the GUI, then present.
6. The sky cube faces (eCIlluminationAdmin::Process, before DoRender): their BeginMaterial runs before the face's SetRenderTarget, which
   unbinds all stages (README 9): a lean replacement keeps that order.
7. Render-state blocks: states changed by our draws inside a bracket must be reverted at its end (4.2) before the mask, the post chain,
   the GUI or an engine draw evaluates the inherited device state.

## 11. Open / to check in game

* The focus crosshair indicator's mesh draws (Game.dll 0x8388b0): when they occur (ranged aiming, spell targeting?) and how the
  coverage counter classifies them (Gui / None view, "outside every native unit") [?]; composing them = an eCMeshResource2::Render with
  the indicator's camera, a candidate for the static-op machinery.
* `RSA +0x64` (GetTimeStamp) readers [?]; `DAT_180c62814` (the counter Execute's stats subtract) writer [?].
* Whether any of the 13 graphs' parameter classes depends on a carry-over in practice (ImageSize without match, no sky dome in
  interiors: SUNLIGHTSCREENVECTOR keeps the outdoor value) - a per-effect value store reproduces it either way [I].
* `eCGfxCamera::ms_bOptimize` / `eCGfxFog::ms_bOptimize` at run time (0 in the image, toggled together with eCGfxEffect's, which is false
  in game) [I].
* The cost split of a lean frame: measure the brackets, the thin effect walk and UpdateShadowMask's CPU part once the slots are gone
  (`-CpuProfile`), before deciding to replace the graph walk by our own targets.
