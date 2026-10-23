# Upscaling / temporal AA (FSR)

Build and measurements of 2026-10-23 (RX 7900 XTX). Legend: [C] confirmed (measured), [I] inferred.
Built: **native AA** (FSR 3.1 at render size = display size) through AMD's signed FFX API DLL. Open: render scale / quality
modes, FSR 4 (section 8).

## 1. Code

| Part | Role |
|---|---|
| `src/vk/fsr.{h,cpp}` | wraps AMD's signed FFX API DLL `third_party/amd_fidelityfx/presigned_dll/amd_fidelityfx_vk.dll` (SDK 1.1.4, headers `third_party/amd_fidelityfx/ffx-api/include`); loaded from `bin\rvk` at run time (`run.ps1` / `package.ps1` copy it) |
| `src/enh/upscale.{h,cpp}`, `shaders/upscale/prep.comp` | the pass `upscale.fsr`, settings, jitter source, depth prep |
| `d3d9/device.*`, `d3d9/draw.cpp`, `enh/water.cpp` | `Device::setJitterSource` / `geometryJitter` / `jitter()` / `setGeometryLodBias` |
| `rvk_vktest --fsr` | self-test: load, versions, context, 12 jittered dispatches on a constant image (output within 0.0005), compares our jitter sequence with the DLL's; SKIP without the DLL |

Settings: `upscale.mode off|native_aa|jitter_only` (Image category), `upscale.sharpness` 0..1 (default 0.4, main page, not
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
main view of a frame asks the source: Halton(2,3) - 0.5, phases = ceil(8 (display / render)^2) = 8 for native AA, identical to
the DLL's. Motion vectors stay jitter-free: the G-buffer computes them from the unjittered matrices.

Checks [C]: `jitter_only`, still camera, 4 burst frames: consecutive frames differ by 1-2 grey levels (mode off: 0.00). Sign
check with `upscale.jitter_flip` (Laplacian-variance sharpness ratio vs the engine, native AA, no RCAS; harbour_mouth / jungle /
pub_interior): none 0.57 / 0.52 / 0.52, x 0.45 / 0.40 / 0.34, y 0.47 / 0.40 / 0.38, xy 0.41 / 0.33 / 0.27: the unflipped
convention is right.

## 4. The pass

`upscale.fsr` at AfterMainView (the scene HDR target is complete, linear A16B16G16R16F, before the post chain). `prep.comp`
copies the main view's hardware depth (D24S8 / D32S8; the DLL needs 32-bit) into an R32F transient. FSR gets {scene HDR, that
depth, the G-buffer's motion (uv units, `motionVectorScale` = W,H), auto exposure, HDR flag, near / far / fov from the camera
block, `viewSpaceToMeters` 0.01}; the output transient is copied back into the scene HDR target, which the engine's post chain
reads.

* Needs the G-buffer: forced on while native AA is on (HAL `applyGbuffer`).
* Reset on the first frame, on skipped frames (no G-buffer: loading) and without a previous camera.
* The context is recreated (the old one retired with the frame) when the scene size changes.
* Validation (`-Validation -SyncValidation`, testdemo): no errors, only the DLL's `rw_luma_history` warning. rvk.log
  `enh: upscale: N frames upscaled (S skipped)` every 5 s.

## 5. Texture LOD bias

`Device::setGeometryLodBias` is added to the samplers (slots < 16) of the jittered draws (`samplerIndexBiased`). Value
`upscaleLodBias(render, display)` = log2(render / display) - 1 (FSR's recommendation, as GD3D11) = -1 at native; recomputed
from the sizes every frame (follows a render scale later). Laplacian-variance ratio vs the engine [C]: no bias 0.57 / 0.52 /
0.52 -> bias -1: 0.71 / 0.65 / 0.63.

## 6. Sharpness (RCAS, `upscale.sharpness`) [C]

| value | ratio (3 spots) | look |
|---|---|---|
| 0 | 0.71 / 0.65 / 0.63 | softer than the engine |
| 0.4 | 1.14 / 1.07 / 1.14 | detail of the engine's picture, edges anti-aliased: **chosen default** |
| 1.0 | 3.37 / 3.06 / 3.15 | visibly crunchy foliage |

Captures: `out\water\20261023-192008` (engine), `-193509` (bias, rcas 0), `-193824` (rcas 1), `-194155` (rcas 0.4).

## 7. Cost

Not measured yet (`-Perf`, pass `upscale.fsr`).

## 8. Open

* Render scale / quality modes (Quality 1.5x etc.: render targets sized below the output, GUI at output size).
* Reactive / transparency masks (water, particles and the alpha pass have background motion).
* Motion of moving non-skinned objects is camera-only.
* Exposure input (auto now).
* FSR 4: untested until an RDNA4 card (no provider on RDNA3, section 2).
* The engine's DoF / post at output size.
* Jitter in shadow-mask / SSR lookups: sub-pixel mismatch (accepted).
* GPU cost (section 7); loading / menu transitions rely on the reset.
* Refcompare with upscale off must stay identical to before (not run yet).
