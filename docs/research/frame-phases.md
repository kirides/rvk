# Frame phases, image roles and the camera block (M6 do-now items 1, 7, 10)

The renderer now knows where in the engine's frame it is: which **view** (main camera, shadow map, sky cube, post chain,
GUI, ...) and which **pass** inside it (depth pre-pass, shadow mask, solid, refracted, alpha, ...), which engine images play
which **role** (scene HDR target, refraction copy, depth map, sky cube, shadow maps), and the main view's **camera** per frame
(current + previous). Nothing of this changes what is rendered (refcompare 13/13, section 6). It is the base for the
injection points and draw tags of the roadmap (docs/research/enhancements-roadmap.md 1.2, 5).

Legend: **[C]** confirmed (decompiled, or measured in the running game), **[I]** inferred. RVAs are Engine.dll.

## 1. What exists

| piece | files |
|---|---|
| phase state machine (view stack, pass, per-frame segment list + summary), image-role enum, camera block + matrix math, camera cross-check | `src/d3d9/scene.{h,cpp}` (unit tests `tests/scene_tests.cpp`) |
| state on the device: `phases()`, `camera()` / `lastCamera()`, `setMainCamera`, `tagImage` / `imageWithRole` / `Texture::role()`; draws counted per segment; the main view's c0..c11 captured at its first solid draw | `src/d3d9/device.{h,cpp}`, `src/d3d9/draw.cpp` |
| the detours (phase markers, role tags at hook points, camera read from engine memory), the vtable patch for RenderRefracted | `src/engine/scene_hooks.{h,cpp}`, symbols in `src/engine/symbols.h` |
| role tags at HAL calls: sky cube (`SetRenderTarget` in the SkyCube view), refraction copy (`CopyCurrentRTToImage` in Main/Solid) | `src/hal/vulkan/device.cpp`, `src/hal/vulkan/resources.cpp` |
| logging: `-LogFrame` lines prefixed `[View/Pass]` + `phase>` transitions + the frame's summary; every 5 s in rvk.log the last frame's structure, its camera and the camera cross-check | `src/hal/vulkan/device.cpp` (`HAL_LOG`, `logFrameStructure`) |
| overlay: "Frame structure" header (last frame's sequence, camera position, near / far) | `src/overlay/overlay.cpp` |
| camera verification scenario | `mods/rvk_camera` |

Installation: `bootstrap.cpp` creates the detours **in Vulkan mode only** (passthrough / trace stay untouched; the only
shared hook is `eCApplication::Draw`, which `[debug] snapshot_frames` already used in any mode: its phase calls are no-ops
without a Vulkan device). `HalDevice::create` attaches its `d3d9::Device`; detours called on another thread or without a
device just call the original. If `sEnumAdapterDesc` falls back to passthrough (no usable Vulkan GPU), nothing attaches.

## 2. Hook points [C]

All bound by decorated export name (unique RVAs in `Engine.exports.txt`, i.e. not COMDAT-folded; each starts with a normal
prologue, checked with capstone). They are called directly (not inlined) from `eCFrustumDatabase::Execute` and the other
callers in the decompiles (`frame.txt`, `B:\DevDrive\risen-ghidra\rvk_phase_dump.txt`).

| function (RVA) | marker |
|---|---|
| `eCFrustumDatabase::Execute` (`0x66e990`) | view scope. Kind from the caller: inside `eCImageFilterScene::OnUpdateOutput` (`0x75cf10`) or `eCCameraBase::Render` (`0x4f8470`) = **Main**; inside `eCRenderSystemAdmin::RenderEntity` (`0x66ceb0`) = **Preview**; else **Other** (`eCCameraBase::Execute`, `eCRenderSystemAdmin::Execute`). Main: camera read, scene target tagged |
| `eCFrustumDatabase::ExecuteShadowMap` (`0x66f630`) | **Shadow** view, pass ShadowDepth; tags the bound colour target (ShadowMap / ShadowCube) and a DF24 / DF16 depth texture (ShadowMap) |
| `eCShaderMaterialAdmin::BeginZPass` (`0x6b9720`) / `EndZPass` (`0x6b9910`) | pass DepthPrepass (not in Shadow views: `ExecuteShadowMap` calls them too, with the shadow map as `eCDepthMap`); tags render target 0 = the depth map |
| `eCIlluminationAdmin::UpdateShadowMask` (`0x5748d0`) | pass ShadowMask around the call |
| `eCModuleAdmin::DoBeginRenderSolid` (`0x4cb810`) / `DoBeginRenderAlpha` (`0x4cb710`) / `DoEndRenderAlpha` (`0x4cb680`) | passes Solid / Alpha / AlphaLate |
| `eCRenderContextMesh` vtable (`0x9ae070`) slot 2 = `RenderRefracted` (`0x5c7400`) | pass Refracted around each refracted op. The function is a 16-byte tail-jump thunk, so the **vtable slot is patched** (after checking it holds that address), at the first device attach. It is the only non-empty `RenderRefracted` (`eCRenderContext::RenderRefracted` is the empty base `0x7faa0`) |
| `eCSkydome_PS::UpdateSkydomeEnvCubemap` (`0x6e7ed0`) | **SkyCube** view |
| `eCImageEffect::Execute` (`0x662630`) | **Post** view (DoRender runs the scene effect `eCImageFilterAdmin+0x90`, then the overlay effect `+0x98`) |
| `eCApplication::Draw` (`0x4c13b0`) | **Gui** view (GUI, DoSnapshot, SwapScreen; the present happens inside it, so the Gui scope closes in the next frame's sequence and is left out of its summary when empty) |

Not hooked: `DoRender` (`0x4c0d40`): its clear of the back buffer shows as the frame's first `None` segment; `BeginMaterial`
/ `EndMaterial` (no pass boundary of their own).

## 3. The frame [C] (opening beach, `rvk_shadows`, frame 900 of a `-LogFrame` run, 1433 draws)

```
SkyCube{} Post{Shadow{ShadowDepth(33)} Shadow{ShadowDepth(69)} Shadow{ShadowDepth(100)}
  Main{DepthPrepass(380) ShadowMask(9) Solid(787) Refracted(6) Alpha(29)} Post(19)} Post{Post(1)} Gui{}
```

1. **Sky cube** (`eCIlluminationAdmin::Process` -> `UpdateSkydomeEnvCubemap`, before `DoRender`): called every frame, draws
   only when its timer (`+0x3b8` accumulates the frame time against `+0x3bc`) expires: 0 draws in the sampled frames; its
   faces were bound (and tagged) at frame 320.
2. `DoRender`: back buffer bound and cleared (no view).
3. **The scene image effect** (`eCImageEffect::Execute`, default `DoF`):
   * **The three sun cascades come first, before the main view**, from the effect's job list, each: `SetRenderTarget(NULL
     FOURCC target, DF24 1024²)`, clear z + stencil, `ExecuteShadowMap` (33 / 69 / 100 draws). So they are **not** rendered
     inside `UpdateShadowMask` (correcting engine-gfx.md 4.2 / engine-gfx.md 8). [I: the path is
     `eCDirectionalLight_PS::UpdateShadowMaps` -> `eCCascadeShadowMap::RenderShadowMaps` (`0x59f200`) ->
     `eCShadowMapBase::RenderShadowMap` -> `eCRenderSystemAdmin::RenderShadowMap` -> `ExecuteShadowMap`, started by a job of the
     effect; the job itself was not traced.]
   * **Main view** (`eCImageFilterScene::OnUpdateOutput` -> `Execute`): scene target (FP16) bound, clear colour; the depth map
     (R32F) bound, cleared white, **DepthPrepass** 380 draws, scene target re-bound; **ShadowMask** 9 draws (the screen-space
     mask `post_SSM.fx` into its own targets, several `SetRenderTarget`s); **Solid** 787 draws (solid list, debug, decals,
     billboards, list `+0x1530`); then the **refraction copy** (`CopyCurrentRTToImage`, FP16 1600x900, in Main/Solid);
     **Refracted** 6 draws (6 `RenderRefracted` ops: the sea); **Alpha** 29; **AlphaLate** 0-1.
   * the effect's post filters: 19 draws.
4. The **overlay effect** (`eCImageFilterAdmin+0x98`): 1 draw.
5. **GUI** (`eCApplication::Draw`): 0 draws here (no HUD in this view), 7-85 elsewhere; `SwapScreen`.

Loading screens: `None(9)` (no view; drawn outside every marker). Frame 1 (main menu backdrop): `Post{Main{ShadowMask(9)}
Post(19)}`. The 13 refcompare spots (rvk.log of run `out\refcompare\20261021-204240`) show the same skeleton everywhere;
interiors without sun (pub, monastery hall, caves) have **no Shadow views** (`Post{Main{DepthPrepass ShadowMask Solid ...}
Post} Post{Post} Gui{}`, 58-89 draws). The inventory and character screens draw the hero through the same main view (no
`Preview` view). No `Other` views and no point-light shadow cubes in any spot (point lights would show as `Shadow` views
with cube targets).

## 4. Image roles [C] (rvk.log "scene: ... = texture ...", logged when a texture gets a role)

| role | where it is tagged | found (1600x900 window) |
|---|---|---|
| SceneHdr | render target 0 at the Main view's `Execute` entry, when called by `eCImageFilterScene` (not the back buffer) | A16B16G16R16F 1600x900, frame 1 |
| DepthMap | render target 0 at `BeginZPass` outside Shadow views | R32F 1600x900, frame 1 |
| Refraction | the destination of `CopyCurrentRTToImage` in Main/Solid | A16B16G16R16F 1600x900 (its own texture, not the scene target), frame 1 |
| ShadowMap | colour target / DF24-DF16 depth texture bound at `ExecuteShadowMap` | 3 x DF24 1024² (sun cascades) + the G32R32F 1024² variance map, frame 319 |
| ShadowCube | a cube colour target at `ExecuteShadowMap` | (point lights; not at the beach) |
| SkyEnvCube | a cube face bound by `SetRenderTarget` in the SkyCube view | A8R8G8B8 128² cube, frame 320 |

`Device::imageWithRole(role)` gives the latest texture per role; a texture keeps one role (a new role replaces the old one,
logged as "was the ..."); dying textures leave the table (`~Texture` -> `forgetImage`).

## 5. Camera block [C]

* **Source**: at the first Main view of a frame, `Execute`'s database `+0x14c0` = its `eCCameraBase`, whose `+0x1738` is the
  `eCGfxCamera` that `ExecuteGfxCamera` executes (`eCGfxCamera::Execute` `0x500d30` renders through `ms_pMasterCamera` when
  that is set: honoured). `eCGfxCamera`: view `+0x20`, projection `+0x60`, depth-bias projection `+0xa0`, view-projection
  `+0xe0`, inverse view `+0x120`, position `+0x150` (= the inverse view's translation row; getters `0x7f8c0..0x7f900` are
  `lea rax,[rcx+X]`). `eCCameraBase +0xc0` = the far plane `SetCamera` (`0x6ba1d0`) writes into c11.w. Reads go through SEH.
* **`d3d9::scene::CameraBlock`**: view, projection, view-projection, its inverse, inverse view, inverse projection, position,
  near / far from the projection (`nearFar`: LH or RH D3D perspective), the engine's far, frame index (the present it
  belongs to), time (s since device creation), previous frame's view-projection / inverse / position (`prevValid` false after
  a frame without a main view). `Device::camera()` = the frame being recorded (valid from its main view on),
  `lastCamera()` = the last presented frame. No GPU buffer yet.
* **Cross-check against the shader constants** (`checkCamera`, logged every 5 s): at the main view's first solid draw the
  VS registers c0..c11 are captured. Measured in every report of the `rvk_shadows` and `rvk_camera` runs:
  **c0..c3 = the transposed view-projection, max |d| 0** (HLSL column-major packing); c4..c7 = transposed inverse projection
  (|d| 1e-7); c8..c10 = the inverse view's 3x3 (|d| 0); **c11 = position (|d| 0) and the engine far 60000**. The
  projection's own far comes out as 59995 / 60009 (float rounding of `1 - f/(f-n)` at far/near 2400-12000): use
  `engineFar`. Near: 25 (game camera at the intro), 5 (the cinema camera).
* **Against the game** (`mods/rvk_camera`, risen-spass `risen.camera.set` / `get`): pose 1 camera.get `(-37959.03, 265.36,
  -6169.85)` dir `(-0.819, -0.177, 0.546)`, renderer frame 578 the same to 0.01 / 0.001; pose 2 `(-38858.61, 915.17, -5070.26)`
  dir `(0.383, -0.690, -0.614)` = renderer frame 1377; the end of a flight `(-37958.71, 265.43, -6170.06)` = frame 2144. (The
  requested positions differ from the engine's by ~0.3 units: the cinema camera's own solve.)
* **Previous frame**: during the flight the position changed in 199 of 384 rendered frames ("moved 1.756 since the previous
  frame" at frame 1761), with a static camera in 0-44 of ~400 (the cinema camera's settling). So **the camera does not move
  every rendered frame** [C]; why (camera update rate vs render rate) is open [I: the camera admin's update is tied to the
  game's process step]. Motion vectors / TAA must take the previous camera from the previous *frame*, as this block does.

## 6. Verification

* `rvk_tests`: `scene_*` cases (summary of a scripted engine frame incl. the continued Gui scope, nesting / unbalanced pops,
  segment cap, matrix inverse + world -> clip -> world round trip, near / far LH + RH, camera block + previous frame,
  camera check incl. a wrong camera, names).
* `rvk_shadows` with `-Validation -LogFrame 900`: no validation messages, the structure above, roles found.
* `rvk_camera`: section 5.
* `testdemo` with `-Validation`: clean.
* Overlay: the "Frame structure" header opened by a posted click (`-OverlayInScreenshots -Commands '800: overlay
  open','820: click 110 583'`) shows the sequence and the camera.
* **`refcompare.ps1 -ReferenceDir out\refcompare\20261021-190139`** (the stored D3D9 reference, overlay closed, all detours
  active): **13 / 13 PASS** (SSIM 0.988-1.000, dluma -0.36..+0.50), run `out\refcompare\20261021-204240`: the markers leave
  the picture unchanged.

## 7. Open

* The job of the scene image effect that renders the sun cascades (section 3, [I]).
* Point-light shadow cubes, `Preview` (`RenderEntity`) and `Other` views: not seen in any refcompare spot; who uses them
  (editor, item previews?) is open.
* Why the camera moves only in about every second rendered frame (section 5).
* Draw tags per draw (roadmap item 2): done, docs/research/draw-tags.md (the RenderRefracted slot patch is now one entry of
  its render-context slot table). Injection points (item 6) build on these phases.
