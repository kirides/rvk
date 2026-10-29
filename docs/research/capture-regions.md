# Capture regions (debug labels for RenderDoc / PIX)

Setting `debug.regions` (Debug tab, default on, NoAB). Names the frame's views, passes and injected passes in a graphics
debugger's capture. Built 2026-10-23. Object names (images, shaders, pipelines): section 5.

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
(restart setting, floor only); a crash seen under RenderDoc with ray traced lighting is noted in upscaling.md 14.

## 5. Resource names (2026-10-28)

RenderDoc's resource inspector lists images by name. `vkSetDebugUtilsObjectNameEXT` names every renderer object at creation
(code `vk/debug_names.{h,cpp}`; `Context::setName` / `namesEnabled`). Setting `debug.names` (Debug tab, default on, **restart**:
objects are named as they are created); zero cost without `VK_EXT_debug_utils` (`Context::namesEnabled` false) or with it off.
Names are made at creation / retag only, never per draw.

### 5.1 Scheme

| object | name |
|---|---|
| engine texture / render target (d3d9 `Texture`) | `tex <id> WxH FORMAT[ cube\| vol][ L<levels>][ dynamic] [slot N]`; render targets / depth `rt <id> ...` (`<id>` = `Texture::ref().id`, d3d9/identity.h; the engine gives no file name to `CreateTexture`) |
| ... once the scene tracker knows its role | `<role> \| rt <id> WxH FORMAT [slot N]`, the image **and its view** (`... (view)`) |
| roles (`Device::tagImage`, `Texture::setDebugLabel`) | `scene HDR target`, `refraction copy`, `depth map`, `sky environment cube`, `shadow cube`, `cascade shadow map #k` (k = the cascade's index in the split array), `terrain shadow map (variance)`, `point-light shadow atlas`, `scene depth-stencil`, `back buffer (D3D9 model)`, `back buffer depth-stencil` |
| shadow mask chain (`d3d9/target_names.cpp`, targets bound inside the UpdateShadowMask pass) | `shadow mask` (the scene-sized A8R8G8B8 bound last = the final mask the materials sample as s0), `shadow mask: before blur` (the one before it when SoftShadows blurs), `shadow mask: SSAO buffer #k` (smaller A8R8G8B8), `shadow mask: 1/4 depth` (R32F) |
| our pass images (`vk::Transients`, `ImageDesc::name`) | `<pass image key> [slot N]` (`water.rt reflection`, `ocean.disp`, `gbuffer ...`) |
| swapchain | `swapchain image N`, `(view)`, `rvk swapchain`, `rvk swapchain rendered/acquire` semaphores |
| buffers | `mesh arena block N`, `mesh arena dedicated buffer N`, `rvk upload ring chunk N`, plus the existing `gi ...`, `rt ...` names |
| samplers | `sampler [slot N] min .. mag .. mip .. \| U .. V .. W ..[ \| aniso Nx]` |
| shaders / pipelines | module `vs\|ps <hash16>[ <variant>]` (ubershader: `<file> <entry> <tag>`), pipeline `pipe <vs name> + <fs name>`; pass pipelines by pass name |
| command pools / buffers | `rvk frame slot N pool / commands / uploads`, `rvk record worker W slot S pool`, `rvk record secondary (slot S) #N` |
| bindless set, set layouts, pipeline layouts | `rvk bindless set (...)`, `rvk bindless set layout`, `rvk pass push set layout`, ... |

Not nameable: the per-draw constant blocks, dynamic vertex / index data and the FrameBlock are sub-ranges of the
`rvk upload ring chunk N` buffers (the FrameBlock address is in the push constants; the draw's constants in `DrawBlock`).

### 5.2 Bindless: finding an image

Our shaders sample through one bindless descriptor set: RenderDoc shows the whole array (`rvk bindless set`), the texture a draw
used is the index in its `DrawBlock.tex[]` (push constant -> buffer reference) = the image's **slot**. Slots are stable for a
texture's life, so the slot is part of every name (`[slot 123]`), and `rvk.log` prints one line per name change:

    names: depth map = image 'depth map | rt 107 1600x900 R32F [slot 111]' bindless slot 111
    names: shadow mask = image 'shadow mask | rt 121 1600x900 A8R8G8B8 [slot 117]' bindless slot 117

(first 160 changes, the log stays bounded; cube and volume textures index their own arrays: `[cube slot N]`, `[3d slot N]`). A
savegame run (`-Mode full`, 1600x900, 40 s) logged exactly one line per role and 0 validation messages: back buffer + depth
(slots 1 / 2), scene HDR target (rt 106, slot 110), depth map (rt 107, slot 111), `shadow mask: 1/4 depth` (400x225 R32F, slot 112),
`shadow mask: SSAO buffer #0 / #1` (800x450, slots 114 / 115), `shadow mask: before blur` (rt 118, slot 116), **`shadow mask`
(rt 121, A8R8G8B8, slot 117)**, refraction copy (rt 105, slot 109), `terrain shadow map (variance)` (1024 G32R32F), `cascade shadow
map #0..2` (2048 DF24), sky environment cube. Slots depend on load order: read the log line of the run. To find the
shadow mask in RenderDoc: Resource Inspector, filter `shadow mask` (the final one has no suffix; `shadow mask: before blur` is
the unblurred accumulation), or read `tex[0]` of a material draw in the Solid pass (the mask is s0 of every material pixel
shader) and match its slot to the `[slot N]` in the names. Cascade maps: filter `cascade shadow map`; the engine's depth map:
`depth map`; refraction: `refraction copy`; scene colour: `scene HDR target`.

### 5.3 Caveats

* Names are applied when the object is created and when the scene tracker (re)tags a texture: a target the engine's pool reuses
  for something else keeps its last role label until it is tagged again. The mask chain's labels come from the targets bound
  inside the UpdateShadowMask pass (A8R8G8B8, no role; the last scene-sized one is the final mask, inferred from the order of
  the binds and the documented s0 = the blurred mask [I]); a pooled target the engine reuses elsewhere in other frames can
  carry a stale `shadow mask*` name.
* Names are only visible to tools that capture the `vkSetDebugUtilsObjectNameEXT` calls; the validation layer prints them in
  its messages (`rvk_vktest --names` proves this with a deliberate misuse: the message carries the image's name).
* Turning `debug.names` off (restart) removes all of the above; `Device::create`'s own objects are then unnamed too.
* Not tested in RenderDoc itself (see section 4).

## 6. Open

* PIX markers via the engine's own D3DPERF events (the `eCAPIDevice` profiling callbacks are stubs).
* Names for the engine's image-effect filters.
