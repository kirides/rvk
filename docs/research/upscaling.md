# Upscaling / temporal AA (FSR)

Build and measurements of 2026-10-23 (RX 7900 XTX). Legend: [C] confirmed (measured), [I] inferred.
Built: **native AA** (FSR 3.1 at render size = display size) and **render scale / quality modes** (section 7) through AMD's
signed FFX API DLL, with an **automatic reactive mask** for translucent effects (section 8). Open: explicit / water masks,
FSR 4 (section 13), thin alpha-tested geometry such as nets (section 12), uncapped GPU cost (section 11). Per-object motion for characters / moving rigid draws: section 9. Jitter-aware depth
reconstruction (positions rebuilt from the jittered depth): section 10.

## 1. Code

| Part | Role |
|---|---|
| `src/vk/fsr.{h,cpp}` | wraps AMD's signed FFX API DLL `third_party/amd_fidelityfx/presigned_dll/amd_fidelityfx_vk.dll` (SDK 1.1.4, headers `third_party/amd_fidelityfx/ffx-api/include`); loaded from `bin\rvk` at run time (`run.ps1` / `package.ps1` copy it) |
| `src/enh/upscale.{h,cpp}`, `shaders/upscale/prep.comp` | the pass `upscale.fsr`, settings, jitter source, depth prep |
| `d3d9/device.*`, `d3d9/draw.cpp`, `enh/water.cpp` | `Device::setJitterSource` / `geometryJitter` / `jitter()` / `setGeometryLodBias` |
| `rvk_vktest --fsr` | self-test: load, versions, context, 12 jittered dispatches on a constant image (output within 0.0005), compares our jitter sequence with the DLL's; SKIP without the DLL |

Settings: `upscale.mode off|native_aa|quality|balanced|performance|ultra_performance|jitter_only` (Image category), `upscale.sharpness` 0..1 (default 0.4, main page, not
Advanced), `upscale.reactive` (default on) + advanced `upscale.reactive_cutoff` (0.08) / `upscale.reactive_scale` (1) (section 8), debug `upscale.jitter_flip none|x|y|xy`.

## 2. DLL findings [C]

* Providers on the RX 7900 XTX: FSR 3.1.4 (and 2.3.3). **No FSR 4 provider** is listed (RDNA3).
* `ffxCreateContext` crashed (null call inside the DLL) until the device enabled `VK_KHR_get_memory_requirements2` +
  `VK_KHR_dedicated_allocation` (the DLL fetches `vkGetBufferMemoryRequirements2KHR` by name). It also needs `shaderFloat16` +
  `shaderInt16` (its fp16 shaders; validation errors otherwise).
* The DLL's VK backend transitions each resource from the state given in the dispatch (COMPUTE_READ = SHADER_READ_ONLY,
  UNORDERED_ACCESS = GENERAL) and **leaves it there**; `vk::Fsr` sets the images' tracked layouts accordingly.
* `ffxQuery` GET_VERSIONS needs the `VkDevice` in `device` (null crashes).
* Known benign validation warning from inside the DLL: `rw_luma_history` declared rgba8, view R16G16B16A16_SFLOAT.

## 3. Jitter

`FrameBlock::jitter` and `DrawBlock::posFix += (2 jx / W, -2 jy / H)` NDC (Cauldron's convention) on the geometry of the main
view's DepthPrepass / Solid / Refracted / Alpha / AlphaLate into a scene-sized target (`Device::geometryJitter`). The first
main view of a frame asks the source: Halton(2,3) - 0.5, phases = ceil(8 (display / render)^2) from the actual sizes (8 for native AA; 18 / 23 / 32 / 72
at 1.5 / 1.7 / 2 / 3x [C]), identical to the DLL's. Motion vectors stay jitter-free: the G-buffer computes them from the unjittered matrices. Positions rebuilt from the jittered depth must subtract the jitter: section 10.

Checks [C]: `jitter_only`, still camera, 4 burst frames: consecutive frames differ by 1-2 grey levels (mode off: 0.00). Sign
check with `upscale.jitter_flip` (Laplacian-variance sharpness ratio vs the engine, native AA, no RCAS; harbour_mouth / jungle /
pub_interior): none 0.57 / 0.52 / 0.52, x 0.45 / 0.40 / 0.34, y 0.47 / 0.40 / 0.38, xy 0.41 / 0.33 / 0.27: the unflipped
convention is right.

## 4. The pass

`upscale.fsr` at AfterMainView (the scene HDR target is complete, linear A16B16G16R16F, before the post chain). `prep.comp`
copies the main view's hardware depth (D24S8 / D32S8; the DLL needs 32-bit) into an R32F transient. FSR gets {scene HDR, that
depth, the G-buffer's motion (uv units, `motionVectorScale` = render W,H), auto exposure, HDR flag, near / far / fov from the
camera block, `viewSpaceToMeters` 0.01}. The output is an **output-sized transient** (display size); there is no copy back:
`Device::setSceneSubstitute(from, bindless slot)` makes draws sampling the scene HDR texture (the post chain) get the FSR
image's slot until the frame ends (`d3d9/draw.cpp`), section 7.

* Needs the G-buffer: forced on while native AA is on (HAL `applyGbuffer`).
* Reset on the first frame, on skipped frames (no G-buffer: loading) and without a previous camera.
* The context is recreated (the old one retired with the frame) when the scene size changes.
* Validation (`-Validation -SyncValidation`, testdemo): no errors, only the DLL's `rw_luma_history` warning. rvk.log
  `enh: upscale: N frames upscaled (S skipped)` every 5 s. The reactive mask (section 8) is generated just before this dispatch.

## 5. Texture LOD bias

`Device::setGeometryLodBias` is added to the samplers (slots < 16) of the jittered draws (`samplerIndexBiased`). Value
`upscaleLodBias(render, display)` = log2(render / display) - 1 (FSR's recommendation, as GD3D11) = -1 at native, -1.58 at 1.5x, -2 at 2x [C]; recomputed
from the actual sizes every frame. Laplacian-variance ratio vs the engine [C]: no bias 0.57 / 0.52 /
0.52 -> bias -1: 0.71 / 0.65 / 0.63.

## 6. Sharpness (RCAS, `upscale.sharpness`) [C]

| value | ratio (3 spots) | look |
|---|---|---|
| 0 | 0.71 / 0.65 / 0.63 | softer than the engine |
| 0.4 | 1.14 / 1.07 / 1.14 | detail of the engine's picture, edges anti-aliased: **chosen default** |
| 1.0 | 3.37 / 3.06 / 3.15 | visibly crunchy foliage |

Captures: `out\water\20261023-192008` (engine), `-193509` (bias, rcas 0), `-193824` (rcas 1), `-194155` (rcas 0.4).

## 7. Render scale (quality modes)

Modes and ratios (display / render): `native_aa` 1, `quality` 1.5, `balanced` 1.7, `performance` 2, `ultra_performance` 3
(`jitter_only` keeps 1). Render size = floor(display / ratio). rvk.log `scene: render scale: ...`, `enh: upscale: FSR provider
...: WxH -> WxH` (one line per context creation).

**Hook [C/RE].** The engine sizes all scene-sized targets from the application camera's screen rect (ints left, top, right,
bottom at camera + 0xc4; camera = `*(eCApplication + 0x38)`, else `*(eCApplication + 0xc8)`; `eCApplication::DoRender`
rewrites it from `GetClientRect` every frame). `eCImageFilterScene::UpdateRenderTargets` (0x75d140, vtable +0x150) creates the
FP16 scene image (+0x38), the A8R8G8B8 image (+0x40) and the R32F depth map (+0x48) directly, not via `AllocRenderTarget`
(0x663950; its single caller is `eCImageFilterRTBase::UpdateColorOutput` 0x6667f0 = the post filters, which size their outputs
as input size x a per-filter scale: the shadow mask / SSAO targets derive from the scene image, SSAO half size).
`eCCameraBase::ExecuteGfxCamera` (0x4f6ad0) sets the main view's viewport from the same rect; the aspect (camera + 0xd8) is
cached by `eCCameraBase::Update(2)` and unaffected; the image effects' SCREENSIZE constant is read live from the rect.

Our hook is the existing detour of `eCImageFilterScene::OnUpdateOutput` (0x75cf10, vtable 0x9eec08 slot +0x138): shrink right /
bottom (floor(size / ratio)) around the original call, restore, then `Device::setViewport` to the full rect (the original leaves
the scaled viewport for the last post filter, which draws into the back buffer). The post filters take the scene image by uv, so
a smaller scene image just works; their intermediate targets derive from its (smaller) size.

**Device side.** `Device::setRenderRatio` / `displaySize` (set by the hook from the real rect) / `setSceneSubstitute`. The
hardware depth (auto depth stencil) stays back-buffer sized and larger than the scene target (allowed; passes read the top-left
region with integer fetches). The G-buffer (forced on while an FSR mode is on), shadow mask, SSAO replacement and wet-shore pass
follow the scene size [C]: `g-buffer requested (1066x600)` at 1.5x on 1600x900, 800x450 at 2x. The GUI stays at output size and
sharp.

**Results [C]** (2026-10-23):

* Quality (1600x900 -> 1066x600) and performance (800x450), testdemo, `-Validation -SyncValidation`: 0 errors, 0 SYNC, only the
  DLL's `rw_luma_history` warning.
* `water-compare` captures `out\water\20261023-201511` (quality) and `-201650` (performance): picture registered, no offsets;
  sea / sky / HUD-free views fine; performance visibly soft (expected).
* refcompare of gui_inventory, gui_character, menu_esc, hero_closeup, pub_interior, quality vs the stored D3D9 reference
  (`out\refcompare\20261023-201917`): menu_esc PASS (ssim 0.9995), gui_character PASS (0.9969): the GUI stays sharp; 3D-heavy
  spots deviate as expected from a 1/1.5 render (gui_inventory WARN 0.956, pub_interior WARN 0.958, hero_closeup FAIL 0.927).
  The reference's thresholds are for unscaled renders.
* Live switching at 1920x1080 in one run (off -> native_aa -> quality -> performance -> off, `-Commands 'set upscale.mode ...'`):
  each switch recreates the FSR context once (1920x1080 -> 1920x1080, 1280x720 -> 1920x1080, 960x540 -> 1920x1080); scene
  targets / G-buffer / transients follow; no errors; GPU memory ~1.1 GiB steady (dips with smaller targets).

**Known limits.** Before the G-buffer is armed (first ~200 frames after a load, pipelines building) and in frames without a
G-buffer the FSR pass is skipped: those frames show the engine's own bilinear upscale of the small scene image (the menus' 3D
backdrop right after start). The render ratio is not dropped back to 1 for them (avoids target re-creation churn). The image
effects (bloom / DoF intermediates) run at render size. Unchecked: the SCREENSIZE vs TEXTURESIZE mismatch in the post filters
(per .fx), `Camera::Update(void)` frustum code reading the scaled rect during the scene pass, `WorldToScreen` users during the
scene pass.

## 8. Reactive mask (translucent effects)

Particles, rain and spell effects have no motion vectors of their own and ghost / smear in the temporal accumulation. FSR's
auto-generated reactive mask compares the scene without and with the translucent effects.

**Injection point.** `Point::BeforeAlpha` (`d3d9/inject.h`, injection.md 4.1) fires once per main view from
`Device::prepareDraw` at the first draw of the Main view's Alpha or AlphaLate pass [C]: the scene HDR target then holds the
opaque + refracted scene (sky, solid, decals, water) and no translucent effect. The pass `upscale.opaque` (`enh/upscale`)
copies it into a transient (render size, RGBA16F). At AfterMainView FSR's `ffxDispatchDescUpscaleGenerateReactiveMask` (flags
TONEMAP | THRESHOLD | COMPONENTS_MAX, scale, cutoff threshold, `binaryValue` 0.9) compares it with the final scene HDR and
writes the mask; the upscale dispatch gets it as `reactive`. The mask is an **R32F** transient (`upscale.reactive`): the format
the shader's `rw_output_autoreactive` declares (R8 gave a validation format-mismatch warning, undefined stores) [C]. Cutoff 0 =
no THRESHOLD flag (the scaled difference is the mask).

**Second context [C].** The DLL's VK backend (SDK `ffx_vk.cpp` `UnregisterResourcesVK` / `destroyDynamicViews`,
`FFX_MAX_QUEUED_FRAMES` = 4) advances the context's dynamic resource view ring by one slot per `ffxDispatch` and destroys the
views of the slot it enters. Two dispatches per frame (mask + upscale) on ONE context cycled the ring twice as fast as the
frames in flight finish: validation error `VUID-vkDestroyImageView-imageView-01026` (10x, views in use by FSR's command
buffers). Fix: the mask runs on its own second FFX context (32x32: the mask dispatch reads its size from the dispatch
description, only its pipeline is used); rvk.log `fsr: reactive mask context (32x32 ...)`. Afterwards testdemo with
`-Validation -SyncValidation` at quality and performance: 0 errors, 0 SYNC, only the known `rw_luma_history` warning.

**Settings.** `upscale.reactive` (default on), advanced `upscale.reactive_cutoff` (default 0.08), `upscale.reactive_scale`
(default 1).

**Tuning sweep [C]** (beach_rain = rain, harbour_night = torch fire; quality, 1 / 1.5, 4 burst frames; hf = mean |laplacian| of
the luma; "vs engine" = mean abs difference to the upscale-off image; engine hf 1.69 / 3.88):

| setting | beach_rain hf (vs engine) | harbour_night hf (vs engine) |
|---|---|---|
| mask off | 0.94 (1.13) | 1.71 (1.60) |
| cutoff 0.2 | 0.97 (1.27) | 1.73 (1.62) |
| cutoff 0.08 | 0.97 (1.11) | 1.76 (1.57) |
| cutoff 0.03 | 0.99 (1.46) | 1.72 (1.59), frame-to-frame 0.10 |
| cutoff 0 | 0.97 (1.24) | 1.71 (1.60) |
| cutoff 0.03, scale 2 | 1.01 (1.16), most visible rain, f2f 0.08 | 1.76 (1.81), f2f 0.18 = flicker, less AA |

Chosen default cutoff 0.08 / scale 1: stable frame to frame (0.06), smallest difference to the engine picture at the torch
spot. The rain's thin 1 px streaks are inherently fainter at 1 / 1.5 resolution; the mask recovers only part of them (crops
`out\water\20261023-204730` vs `-203931`; sweep dirs `-205226`, `-205402`, `-205545`, `-205717`).

**Not tested:** actual spell effects (no scenario / spot has one: needs a pose with a cast spell); a debug visualisation of the
mask. **Not in the mask:** the sea is in the Refracted pass, before the BeforeAlpha snapshot: it is not reactive and
accumulates with background motion (a transparency and composition mask would be needed).

## 9. Per-object motion (characters, moving rigid draws)

**Before [C].** The G-buffer's motion (`enh/gbuffer`, `GbufferMotion` pixel variant) = the camera motion of the surface point
for every material pixel. Per-object motion existed only for skinned draws and only while the RT scene was active (GI / RT
reflections): `enh/rt_scene`'s capture (`VariantSlot::Capture`, SPIR-V tag `cap3`: the vertex shader writes each vertex's world
position, = invViewProj * clip position before the posFix / jitter, into a ring buffer indexed by the index buffer value; the
header holds the slots + the previous frame's capture address) was driven by its `DrawObserver`, registered only while the RT
scene is active. With only FSR on, characters had camera-only motion: the idle animation accumulated misregistered (blur / lost
detail).

**Code.** `src/enh/object_motion.{h,cpp}`: `ObjectMotion : d3d9::DrawObserver`, registered while an FSR mode is on and
`upscale.object_motion` (advanced; an enum `off | characters | all`, default `characters`: skinned draws only; `all` adds the
moving rigid draws, untested with weapons). It observes Main view / Solid pass draws of `Kind::Skinned` (skipped while
`RtScene::capturingCharacters()` = active && dynamic) and `Kind::StaticMesh` (rigid, `all` only; **only with
`q.tag->entity` set**, see the jitter bug below). Key = entity pointer x vertex / index
buffer ids + generations x range; per key `State{previous capture address + slots + frame, hash}`. Per draw: resolve the last
request (it ran if `Device::vertexCapturedPrev(addr)`); skinned draws are always captured; rigid draws only while the hash of
the VS constants c31..c37 (`g_vInstanceMisc` / `g_vInstanceData`: placement) changed in the last 4 frames (a fade's alpha
changes them too: harmless, previous position = current, motion = the camera's). It allocates 16 + 32 x slots bytes from the
frame ring (slots = highest index of the range, scanned once; rigid cap 200k slots, max 256 captures per frame), writes the
header (slots, previous capture) and calls `Device::requestVertexCapture`; the G-buffer draw path then uses the Capture vertex
variant + the `GbufferMotion` pixel variant (`draw.cpp`: `gbufMaterial && scopeGbuf_`). Same tag `cap3` as the RT scene.
Requires `vertexPipelineStoresAndAtomics`. rvk.log: `enh: object motion: on (skinned yes, moving rigid draws yes)` and every
10 s `N captures so far (M rigid draws seen moving), K tracked draws`. Validation (testdemo, `-Validation -SyncValidation`,
quality): 0 errors, only the known `rw_luma_history` warning. In game: ~75-80k captures in a 90 s run, 150-200 rigid draws seen
moving (includes things that sway / swing: the detector is placement-based, not weapon-specific).

**Test caveat [C].** `mods/rvkcompare` calls `risen.debug.time_freeze(true)` before each capture (unless `spot.freeze ==
false`): animations stop, so characters do not animate in the existing spots (also `hero_closeup` etc.) and the benefit cannot
show there. New explicit spots `hero_idle` (hero, `freeze = false`, camera distance 260) and `pub_idle` (pub; its NPCs need
`-Param show_actors=1`) run the clock.

**Results [C]** (`water-compare -Spots hero_idle -Param show_actors=1 -Burst 8 -Interval 1`; shirt region (750,360)-(850,500) of
the 1600x900 capture; engine = upscale off; dirs `out\water\20261023-211643` native AA motion off, `-211936` native on,
`-212102` quality off, `-212223` quality on): frame-to-frame mean abs difference over the 8 burst frames (flicker):

| mode | object motion off | on |
|---|---|---|
| native AA | 0.35 | 0.32 |
| quality | 0.65 | 0.47 (-28 %) |

Crops (face / shirt): quality without object motion smears the face and shirt (the blur and lost detail in idle poses), with it
clearly crisper; native AA looks close to the engine's picture either way (cloth fibre detail softer: Laplacian-variance ratio
0.44-0.46). The Laplacian-variance ratio does **not** rank on / off correctly (quality 0.15 off vs 0.11 on: the noisier picture
has more high-frequency energy): use flicker + crops. Quality still loses fine cloth detail vs the engine (renders at 1 / 1.5,
ratio 0.11-0.15).

**Jitter bug on buildings [C by measurement, fixed 2026-10-23].** The user reported massive jitter on large buildings at Ultra
Performance with "Motion of characters and objects" on. Cause: rigid captures were keyed by entity pointer x buffer ids x range;
draws of one mesh whose entity pointer is null all shared one key / `State`, so different instances fed each other their
previous-frame world positions (and the per-frame "first draw of the key counts" dedupe skipped the others): wrong, large
per-pixel motion on repeated building pieces. The count of "rigid draws seen moving" also kept growing in a scene with frozen
game time (the hash of c31..c37 alternates between instances). Fix: a `StaticMesh` draw without `q.tag->entity` is not captured
(camera-only motion). The setting became the enum above, default `characters`.

**Measurement on frozen scenes [C].** `rvkcompare` spots freeze the game time, so any change between the 8 consecutive burst
frames is an artefact: temporal standard deviation per pixel over the 8 frames (mean std / p99 / % of pixels > 3), Ultra
Performance (533x300 -> 1600x900):

| spot | motion off | characters | all (before guard) | all (entity guard) |
|---|---|---|---|---|
| harbour_docks | 0.107 / 1.78 / 0.51 % | 0.109 / 1.61 / 0.44 % | 0.153 / 3.09 / 1.04 % | 0.106 / 1.83 / 0.49 % |
| harbour_mouth | 0.180 / 3.30 / 1.07 % | 0.130 / 1.37 / 0.52 % | 0.131 / 1.27 / 0.51 % | 0.131 / 1.30 / 0.54 % |
| monastery_hall | 0.150 / 1.22 / 0.26 % | 0.134 / 1.13 / 0.23 % | 0.144 (mean only) | 0.140 / 1.16 / 0.22 % |

Native AA + all (docks): 0.085 / 0.95 / 0.16 %. The bug shows only in the docks row (buildings: 0.153 / 1.04 % before, back to
the level of off after the guard). Capture dirs: `out\water\20261023-213926` (off), `-214247` (characters), `-214428` (all
before the guard), `-215544` (all with the guard), `-214607` (native all). Default stays `characters` (conservative): `all` is
for weapons / rigid objects, still untested. The script was a scratchpad `temporal.py` (not in the repo).

**Not tested:** weapons (no scene with a drawn weapon: needs a guard / the hero with the weapon drawn; check that the rigid path
catches it: it should show in the "rigid draws seen moving" count and get motion). **Known gaps:** characters in the shadow /
alpha passes (only the solid pass is captured: alpha-blended parts such as hair cards / cloaks keep camera motion), instanced
and non-indexed draws (the capture variant needs indexed, non-instanced), SpeedTree wind (sway: camera motion only), skinned
draws beyond 256 captures per frame; while the RT scene is active its own capture is used (its own limits).

## 10. Jitter and depth reconstruction [C, 2026-10-23]

The jitter (section 3) is a clip-space offset on the main view's geometry only; the FrameBlock matrices stay the unjittered
camera's. So the surface rasterised at a pixel whose centre has NDC `c` has the unjittered NDC `c - J`: any position rebuilt from
the main view's depth (depth map R32F or hardware depth) with the unjittered inverse matrices is off by `J` (up to 0.5 render
pixel = 1.5 output pixels at Ultra Performance). Fix: `rvkJitterNdc(RvkFrame f)` in `shaders/include/rvk_common.glsl` =
`vec2(2 f.jitter.x, -2 f.jitter.y) * f.viewport.zw` (zero without jitter); consumers use `rvkUvToNdc(uv) - rvkJitterNdc(f)`:

| consumer | place |
|---|---|
| GTAO, normals from depth, the motion fill | `gbViewPos` / `gbWorldPos`, `shaders/gbuffer/gbuffer_common.glsl` |
| screen probes (GI) | `spWorldPos`, `shaders/gi/screen_common.glsl` |
| generic helper | `rvkWorldFromViewDepth`, `rvk_common.glsl` |
| material variants' fragment world position | `shaders/include/rvk_material_variant.glsl` (inlined: that text does not include `rvk_common.glsl`) |
| enhanced sun shadow mask | `reconstruct()` in `shaders/shadows/shadow_mask.frag`: `p = d * (pixel NDC - J, 1)` |

**Not corrected:** the engine's own `post_SSM.fx` cascade draws (`shadows.enabled` off): the view ray is interpolated from the
fullscreen quad's vertices (not jittered: the ShadowMask pass is outside the jitter whitelist) and `p = depth x ray` feeds
`DepthToShadowMatrix`; a constant change cannot express the depth-dependent offset (needs the quad's input positions shifted by
`-J`, or jittering the engine's camera matrices before it computes its constants). Also not corrected: forward lookups of world
points into jittered buffers (water SSR, history reprojection): sub-pixel, FSR / the own filters absorb it.

Measurement (frozen scenes; temporal std per pixel over 8 consecutive frames; Ultra Performance 533x300 -> 1600x900,
`shadows.enabled` + `ao.enabled`; dirs `out\water\20261023-221713` before, `-222631` after, `-222105` upscale off), mean:

| spot | before | after | upscale off |
|---|---|---|---|
| sea_cliff | 0.577 (p99 6.45, > 3: 3.80 %) | 0.438 (p99 5.30, 2.68 %) | 0.820 (the sea animates: not comparable) |
| harbour_docks | 0.103 | 0.103 | 0.128 |
| jungle | 0.105 | 0.105 | 0.033 |
| beach_sun | 0.322 | 0.326 (unchanged within noise) | 0.642 |

So the mismatch was a minor shimmer source except at sea_cliff; the building jitter the user saw was the rigid object-motion
bug (section 9). Validation (testdemo `-Validation -SyncValidation`, ultra_performance + GI screen probes + GTAO): 0 errors,
0 SYNC, only the known `rw_luma_history` warning.

**Tooling lesson:** the material variants' GLSL is embedded text compiled at run time, so a missing helper only shows in the
game. After editing `rvk_material_variant.glsl` run the offline checks `rvk_shadertool corpus corpus --gbuf`, `--gi`,
`--gbuf-motion` (each ~40 s; the first run caught the missing `rvkUvToNdc` in that text: 2230 shaders failing).

## 11. Cost

Native AA / render scale, jungle hold [C, noisy]: the Radeon Chill profile caps the world at ~80 fps (p50 12.4 ms in every
mode), so fps says nothing. GPU ms / frame: off 3.4-7.8, native AA 5.5, quality 4.1-5.9, performance 3.4-4.1. CPU recording of
the injected FSR pass 0.36-0.46 ms. A clean GPU cost needs `-Exe RisenTest.exe` (uncapped) with `-PerfFrame` (`pass
upscale.fsr`): not done.

Uncapped attempt (`RisenTest.exe`, 2560x1440) [C, contaminated]: `AION2.exe` used ~70 % of the GPU, so totals are void. In fast
phases the FSR pass (`pass upscale.fsr`, `-PerfFrame 5000`) took 0.63 ms native AA / 0.42 quality / 0.33 performance; CPU
recording of the injected passes ~0.3 ms. A clean total frame cost needs the GPU to itself (to repeat).

## 12. Thin alpha-tested geometry (fishing nets) [C, 2026-10-23]

User complaint: fishing nets (and opaque leaves) look horrible / "too sharp" under FSR. The net's material in RenderDoc: texture
`t2D` BC1_UNORM 512x512; the PS is a translated engine material with a render-state alpha test (`rvk_alphaTest(oC0.w)`,
alpha = texture alpha x vertex alpha, `demote`).

**Test scene:** the user's savegame `C:\Users\johnh\AppData\Local\Risen\SaveGames\Risen_Save_0008.save` (read-only copy loaded
in the test copy with `run.ps1 -QuickSave` + scenario `rvk_perfsave`): hero at (-13098, 1041, -19673) near Harbour Town, camera
held 268 units behind and above. Explicit `rvkcompare` spots `net8_n` / `net8_e` / `net8_s` / `net8_w` (player at that position,
camera at eye height looking N / E / S / W): `net8_s` = a large fishing net against the sky (the problem case), `net8_w` a net
at the left edge, `net8_e` foliage.

**Measurements** (frozen scenes, net region (980,110)-(1590,330) of the 1600x900 capture of `net8_s`, 8 burst frames, engine
laplacian variance 21163; dirs `out\water\20261023-232515` .. `233320`, engine `20261023-232112`):

| mode | sharpness vs engine | temporal std | p99 | pixels > 3 |
|---|---|---|---|---|
| native AA | 0.447 | 0.48 | 2.0 | 0.15 % |
| Quality | 0.11 | 4.57 | 31 | 40 % |
| Ultra Performance | 0.04 | 4.9-5.4 | 27 | ~50 % |

Native AA is clean and close to the engine's own weave. At Quality and below the net is **unstable noise, not over-sharpened**:
the strands are thinner than a render pixel, so there is nothing stable for the history to accumulate.

**Not effective** (all measured, no difference beyond noise; the features were reverted, not kept: `cutout_lod` and the four
tuning settings):

* The share of the texture LOD bias for alpha-tested / blended draws (scale 1.0 / 0.3 / 0.0). The pub net (512x512 magnified
  there: bias is irrelevant) and the jungle leaves gave identical sharpness and temporal std to 3 digits.
* RCAS sharpness 0.4 vs 0.
* The reactive mask on / off / cutoff 0.3 (the user also tested "Clean up effects" off in game: no difference).
* FSR 3.1's tuning keys via `ffxConfigure` (`ffxConfigureDescUpscaleKeyValue`, key + pointer to a float: `fVelocityFactor`,
  `fShadingChangeScale`, `fAccumulationAddedPerFrame`, `fMinDisocclusionAccumulation`): net temporal std 4.6-5.5 vs 5.14
  baseline over 8 variants (best: accumulation 0.1 = 4.6, softer); foliage 0.35-0.44 vs 0.41.

Conclusion: thin sub-pixel alpha-tested geometry is a known hard case for any temporal upscaler at Quality and below; native AA
(render scale 1) is clean. **Ideas not tried:** a stochastic (hashed) alpha test while the jitter is active (consistent noise in
the depth prepass and the colour pass, via a FrameBlock parameter + frame index); an alpha-to-coverage-like soft alpha for
cut-out textures; drawing alpha-tested thin meshes with a transparency and composition mask (FSR's second mask). A "native AA
for nets" is not possible (one render target size).

## 13. Driver-side FSR 4 provider (`amdvkffx64.dll`) [C, 2026-10-23]

AMD's driver package (here `C:\Windows\System32\DriverStore\FileRepository\u0205306.inf_amd64_ed4bdb51fc506ec7\B026676`, driver
26.10.1) ships `amdvkffx64.dll` ("VK AMD Driver Based FidelityFX Library" 2.4.0.4370, 72 MB, signed by Microsoft WHCP),
`amdxcffx64.dll` (DX12, 85 MB), `amdxc64.dll` etc. `amdvkffx64.dll` exports `UpdateFfxApiProvider` and
`UpdateFfxApiProviderEx`; it loads fine via `LoadLibrary` on the RX 7900 XTX (RDNA3).

**How the FFX API gets driver providers** (vendored source `third_party/amd_fidelityfx/ffx-api/src/ffx_provider.cpp`,
`ffx_provider_external.h`): `GetModuleHandleA("amdxc64.dll")` + `AmdExtD3DCreateInterface(ID3D12Device, IAmdExtFfxApi)` ->
`UpdateFfxApiProvider(ExternalProviderData{structVersion, descType, ffxProviderInterface})`. The signed 1.1.4
`amd_fidelityfx_vk.dll` contains the same code (strings `amdxc64.dll`, `AmdExtD3DCreateInterface`, `ffxProviderExternal`) and
**no Vulkan-specific route**: in a Vulkan process the driver's provider is never offered (our `ffxQuery` GET_VERSIONS lists only
3.1.4 and 2.3.3).

**The newer interface** (documented by OptiScaler commit b686ef4): `UpdateFfxApiProviderEx(void* data, uint32 size,
ProviderOption* options)`, struct version 3. `ExternalProviderData` is 0x60 bytes: `u32 structVersion`, `u64 descType`,
`ProviderInterface` (0x40 bytes: `versionId`, `versionName`, `canProvide`, `createContext`, `destroyContext`, `configure`,
`query`, `dispatch`), `bool (*isSupported)(const void* backend{VkDevice, VkPhysicalDevice, vkGetDeviceProcAddr})` at 0x50,
reserved. `ProviderOption` is 0x18 bytes `{u32 type (unused), u32 effect (0 upscale, 1 frame generation, 5 FG swapchain), bool
enable, next}`. Vulkan backend desc type `FFX_API_CREATE_CONTEXT_DESC_TYPE_BACKEND_VK_V2` 0x01000003. Versions:
`FFX_UPSCALER_VERSION_V2` = (4<<22)|(1<<12)|2 (4.1.2; the driver accepts 4.0.0-4.1.2), frame generation (4<<22)|2, FG
swapchain 4<<22 (>= 3.1.5). The driver's FSR 4 shaders read / write 16-bit float images (OptiScaler rewrites the SPIR-V to
32-bit via `NormalizeHalfImageIO` and swaps `vkGetDeviceProcAddr` for `vkCreateShaderModule`). OptiScaler also spoofs module
names (`GetModuleHandle` for `amd_fidelityfx_*.dll`), signature checks (Crypt32) and the AMD GPU identity (gdi32 D3DKMT adapter
info: family / revision at indices 12 / 13 of the 608-byte AMD private data; INT8 Lite = 150 / 1 gfx11.5, INT8 RDNA2 = 143 / 40)
to make older GPUs run INT8 models.

**Our probe:** `rvk_vktest --ffx-driver <path>` (committed) calls `UpdateFfxApiProviderEx` / `UpdateFfxApiProvider` from the
test executable with struct versions 1-4, sizes 0x40-0x70, with / without the option chain: 84 combinations, all `E_INVALIDARG`
(0x80070057). The driver library gates its caller (it expects AMD's signed FFX DLL; OptiScaler hooks to impersonate it). We do
**not** impersonate the signed module or hook signature checks.

**Plan [I, from the user]:** recent drivers support FSR 4.1 on RDNA2 / 3 natively (no spoofing needed). AMD's release page: FSR
SDK v2.3.0 (24 Jun), Upscaling 4.1.1 "adds RX 7000 discrete GPU support"; v2.x splits effects into a loader DLL + per-effect
DLLs. The v2.3.0 prebuilt does not contain Vulkan DLLs yet; v2.4.0 is expected to ship prebuilt Vulkan DLLs. The plan is to
adopt v2.4.0's signed Vulkan loader + upscaler DLLs (new headers, loader flow, version query / override) and keep the 1.1.4
DLL as the fallback; until then FSR 3.1.4 only. Status: waiting for SDK 2.4.0.

## 14. Open

* Thin alpha-tested geometry (nets, opaque leaves) is unstable at Quality and below (section 12): ideas not tried are hashed
  alpha while jittered, soft alpha, FSR's transparency and composition mask.
* Render scale: the uncapped GPU cost (section 11), refcompare thresholds for scaled renders (recalibrate).
* Reactive masks: the automatic form is built (section 8). Open: explicit per-draw reactive output (particle / precipitation
  draws writing a reactive value: the more precise alternative), a transparency and composition mask for water (the sea is
  not reactive), a debug view of the mask, a test with real spell effects.
* Per-object motion is built for skinned and moving rigid draws (section 9). Still camera-only: SpeedTree wind, alpha-pass
  parts of characters, instanced / non-indexed draws; weapons untested (no scene with a drawn weapon; `all` is for them).
  Why "rigid draws seen moving" keeps growing in frozen scenes (484 in a run with the guard) is unexplained; harmless
  (captures with previous == current).
* Known issues (2026-10-23): the user's real game died with 0xc0000374 (heap corruption, ntdll, 21:27:45, right after the
  first FSR frame) under RenderDoc when enabling ray traced lighting; the crashes at 18:43, 19:59 and 21:41 were 0xc0000005 in
  the engine's `SharedBase.dll` (stock engine crash, `RisenHighFps.exe`, not ours). Not reproduced in the test copy without
  RenderDoc (FSR native + quality with GI at startup, GI toggled at run time via `-Commands` with validation, object motion
  all, GTAO, water RT): no crash, no validation errors. RenderDoc only partly supports the ray tracing extensions
  (acceleration structures, ray query): treat captures with `rt.allow=true` as unsupported; for RenderDoc use `rt.allow=false`
  (restart setting, floor only).
* Exposure input (auto now).
* FSR 4: untested until an RDNA4 card (no provider on RDNA3, section 2); the driver's provider is not reachable from a Vulkan
  process with the 1.1.4 DLL (section 13), waiting for FSR SDK 2.4.0's signed Vulkan DLLs.
* The engine's DoF / post at render size (section 7).
* Jitter: depth reconstruction is corrected (section 10), except the engine's own `post_SSM.fx` cascade shadow draws
  (`shadows.enabled` off: needs the quad's input positions shifted by `-J` or jittered engine camera matrices); forward
  lookups into jittered buffers (water SSR, history reprojection) keep a sub-pixel mismatch (accepted).
* GPU cost (section 11); loading / menu transitions rely on the reset.
* Refcompare with upscale off must stay identical to before (not run yet).
