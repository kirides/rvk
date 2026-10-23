# Capture regions (debug labels for RenderDoc / PIX)

Setting `debug.regions` (Debug tab, default on, NoAB). Names the frame's views, passes and injected passes in a graphics
debugger's capture. Built 2026-10-23.

## 1. Mechanism [C]

* `VK_EXT_debug_utils` is enabled on the instance whenever present (`Context::debugUtilsAvailable`); regions are
  `vkCmdBeginDebugUtilsLabelEXT` / `vkCmdEndDebugUtilsLabelEXT` in the frame's main command buffer. The upload command buffer
  has no labels.
* API (`d3d9/device.h`): `Device::regionBegin(const char* name, uint32 index = none)` / `regionEnd()`; RAII
  `RegionScope r(device, "name"[, index])` (end of `device.h`).
* Zero allocation: `Device::regions_` = `std::array<OpenRegion, 32>`, `OpenRegion{const char* base, uint32 index, char
  text[48]}`; the text is `"base"` or `"base #index"` (`std::to_chars`). The label colour is a hue from a hash of the name.

## 2. Frames and submissions

* The root `Frame N` (N = presented frames + 1) is opened by `regionsReopen()` in `beginFrame` (new command buffer): after a
  mid-frame flush the same stack is reopened, a new frame gets a new root.
* Labels never span submissions: `submitFrame` calls `regionsClose()` (`vkCmdEndDebugUtilsLabelEXT` x the open count) before
  `frames_.submit`; `beginFrame` -> `regionsReopen()`.

## 3. View / pass regions

They follow the phase tracker (`scene::PhaseTracker`, new accessor `enclosing()`; frame-phases.md). `Device::syncPhaseRegions()`
runs at `prepareDraw` and before an injection (skipped while injecting):

1. key = hash of the enclosing phases + the current one + `viewsEntered(current view)`; an unchanged key returns;
2. otherwise `flushBatch()` first (batched opaque draws must be recorded inside the regions they were issued in);
3. build the wanted path in a fixed array (view names = static strings from `scene::name`; Shadow / Main / Preview numbered
   `#k` = `viewsEntered`);
4. reconcile with the open stack by pointer + index equality: end the extras, begin the new ones.

Every injected pass opens a `RegionScope` with its pass name (`rt.scene`, `gbuffer.ao`, `water.ocean`, `upscale.opaque`,
`upscale.fsr`, ...). `Present` (an `optional<RegionScope>` around the back buffer blit + overlay, ended before `submitFrame`)
contains `Overlay`.

Tree in a capture: `Frame N > Post > Shadow #1 > ShadowDepth ...`, `Post > Main #1 > DepthPrepass / ShadowMask / Solid /
Refracted / Alpha`, the injected passes nested under the phase region they fire in, `Gui`, `Present`.

## 4. Verification

Validation (`-Validation -SyncValidation`, rvkcompare jungle; run-time commands set `debug.regions` false / true twice and
`upscale.mode` quality -> off -> ultra_performance): 0 label-related messages, 0 errors, 0 SYNC; only the known
`rw_luma_history` warning [C].

**Not tested in a real capture tool** (RenderDoc itself was not run by us). RenderDoc supports the ray tracing extensions only
partly (acceleration structures, ray query): captures with `rt.allow=true` are unsupported, use `-Setting 'rt.allow=false'`
(restart setting, floor only); a crash seen under RenderDoc with ray traced lighting is noted in upscaling.md 12.

## 5. Open

* PIX markers via the engine's own D3DPERF events (the `eCAPIDevice` profiling callbacks are stubs).
* Names for the engine's image-effect filters.
