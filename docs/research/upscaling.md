# Upscaling / temporal AA (FSR)

Build and measurements of 2026-10-23 (RX 7900 XTX). Legend: [C] confirmed (measured), [I] inferred.
Built: **native AA** (FSR 3.1 at render size = display size) and **render scale / quality modes** (section 7) through AMD's
signed FFX API DLL. Open: masks, FSR 4, uncapped GPU cost (section 9).

## 1. Code

| Part | Role |
|---|---|
| `src/vk/fsr.{h,cpp}` | wraps AMD's signed FFX API DLL `third_party/amd_fidelityfx/presigned_dll/amd_fidelityfx_vk.dll` (SDK 1.1.4, headers `third_party/amd_fidelityfx/ffx-api/include`); loaded from `bin\rvk` at run time (`run.ps1` / `package.ps1` copy it) |
| `src/enh/upscale.{h,cpp}`, `shaders/upscale/prep.comp` | the pass `upscale.fsr`, settings, jitter source, depth prep |
| `d3d9/device.*`, `d3d9/draw.cpp`, `enh/water.cpp` | `Device::setJitterSource` / `geometryJitter` / `jitter()` / `setGeometryLodBias` |
| `rvk_vktest --fsr` | self-test: load, versions, context, 12 jittered dispatches on a constant image (output within 0.0005), compares our jitter sequence with the DLL's; SKIP without the DLL |

Settings: `upscale.mode off|native_aa|quality|balanced|performance|ultra_performance|jitter_only` (Image category), `upscale.sharpness` 0..1 (default 0.4, main page, not
Advanced), debug `upscale.jitter_flip none|x|y|xy`.

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
at 1.5 / 1.7 / 2 / 3x [C]), identical to the DLL's. Motion vectors stay jitter-free: the G-buffer computes them from the unjittered matrices.

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
  `enh: upscale: N frames upscaled (S skipped)` every 5 s.

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

## 8. Cost

Native AA / render scale, jungle hold [C, noisy]: the Radeon Chill profile caps the world at ~80 fps (p50 12.4 ms in every
mode), so fps says nothing. GPU ms / frame: off 3.4-7.8, native AA 5.5, quality 4.1-5.9, performance 3.4-4.1. CPU recording of
the injected FSR pass 0.36-0.46 ms. A clean GPU cost needs `-Exe RisenTest.exe` (uncapped) with `-PerfFrame` (`pass
upscale.fsr`): not done.

## 9. Open

* Render scale: reactive / transparency masks (below), the uncapped GPU cost (section 8), refcompare thresholds for scaled
  renders (recalibrate).
* Reactive / transparency masks (water, particles and the alpha pass have background motion).
* Motion of moving non-skinned objects is camera-only.
* Exposure input (auto now).
* FSR 4: untested until an RDNA4 card (no provider on RDNA3, section 2).
* The engine's DoF / post at render size (section 7).
* Jitter in shadow-mask / SSR lookups: sub-pixel mismatch (accepted).
* GPU cost (section 8); loading / menu transitions rely on the reset.
* Refcompare with upscale off must stay identical to before (not run yet).
