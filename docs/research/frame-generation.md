# Frame generation (FSR 3 / FSR 4) - design and feasibility

Research of 2026-10-29 (RX 7900 XTX, Adrenalin 26.10.1 "AMD proprietary driver", Windows 11 26300). Request: "Add frame generation. FSR 3 and FSR 4
frame generation. Ensure proper setup and pacing." **Nothing is built in the renderer**; this document is the design plus the measurements of a
throwaway probe (`out/fg/`, git-ignored, section 6). Legend: [SDK] read in the SDK 2.4.0 headers / docs / sample source (clone
`B:\DevDrive\ffx-sdk-2.4.0-dl\src`, tag `v2.4.0`, paths are relative to `Kits/FidelityFX/` unless they start with `Samples/` or `docs/`), [C] code of
this repo, [M] measured with the probe, [E] estimate, [?] open: needs a prototype in the game.

## 0. Result in short

| question | answer |
|---|---|
| FSR 3 frame generation on this GPU | **works** [M]: FG 3.1.6 (version id `0xf600000000c01006`) creates a context, generates a correct midpoint frame (rect centre 579.5 vs expected 579.5, exact) in 0.8 ms at 1080p / 1.1 ms 1440p / 2.7 ms 4K (GPU) [M]; the AMD frame generation swapchain (3.1.7, and a driver-provided 4.0.0) presents 2 frames per rendered frame, 1.99 real presents per app frame, gaps 12.37 / 12.63 ms at an even 40 fps base [M] |
| FSR 4 (ML) frame generation on this GPU | **not offered** [M]: `ffxQuery(GET_VERSIONS)` for frame generation lists only `3.1.6` on the 7900 XTX; FSR Frame Generation 4.0.2 needs an RX 9000 (RDNA 4) [SDK `docs/techniques/frame-interpolation-ml.md` "Limitations"]. It is in the same DLL (`amd_fidelityfx_framegeneration_vk.dll` 4.0.2.4555) and the same API (Configure / PrepareV2 / Dispatch), so it can be added as a provider choice that appears where the query offers it; it cannot be verified here |
| Integration | **AMD's frame generation swapchain (replaces our `VkSwapchainKHR`) for presentation + pacing, FG context in the callback model; plus the same FG context driven manually (`FFX_FRAMEGENERATION_FLAG_NO_SWAPCHAIN_CONTEXT_NOTIFY`) as the capture / validation path** (section 2) |
| UI | HUDless back-buffer copy at `Point::BeforeGui` (cheap, first), ImGui overlay in the present callback, optional premultiplied UI texture later (section 3.4) |
| Pacing | base frames stay paced by our `FramePacer` at output/2; AMD's swapchain spaces the generated frame; our base cadence must be even (a 15.6 ms timer tick destroys the pairing: stddev 3 ms vs 0.03 ms [M]) |
| Cost | FG swapchain threads ~0.5 - 0.65 CPU core (spin) [M, rough]; +1 base frame of latency [E]; memory 106 MB (1080p) + swapchain 42 - 51 MB [M/SDK] |
| Blockers / costs | 4 distinct `VkQueue`s at device creation; **no application-controlled exclusive fullscreen** (the real swapchain is hidden); the 36.8 MB DLL is not vendored; the driver-provided `4.0.0 *` swapchain **crashes in `pOutDestroySwapchainFFXAPI`** [M] (pin 3.1.7); the AMD Radeon Chill 80 fps profile on `Risen.exe` would cap total presents (generated included) [E] |

## 1. What SDK 2.4.0 offers for Vulkan (question 1)

### 1.1 DLLs and versions

* Loader `amd_fidelityfx_loader_vk.dll` 1.0.2.4555 (vendored); effect DLLs found next to the exe or **next to the loader** [SDK `docs/getting-started/ffx-api.md`
  "Effect DLL lookup"]. Frame generation: `amd_fidelityfx_framegeneration_vk.dll` 4.0.2.4555, 36,806,928 bytes, SHA-256 `D002DABF...98B4` (matches
  `third_party/amd_fidelityfx/VERSION.md`), Authenticode `Valid`, signer Advanced Micro Devices [M, `Get-AuthenticodeSignature` before anything was loaded]. It holds
  the legacy and the ML providers [SDK `docs/getting-started/ffx-api.md` "DLLs structure"].
* Effects in the DLL [SDK `docs/techniques/*.md`]:

| effect | version | hardware | API structs (path) |
|---|---|---|---|
| FidelityFX Super Resolution Frame Generation (optical flow + frame interpolation) | **3.1.6** (2025-12-10) | none beyond storage-image load + `R16G16B16A16_UNORM` | `framegeneration/include/ffx_framegeneration.h` |
| AMD FSR Frame Generation (ML) | **4.0.2** (2026-10-28) | **RX 9000 series (RDNA 4) or later**, Windows 11, Vulkan 1.2+; designed for >= 30 fps input; "FRAMEGENERATION_FLAG_DRAW_DEBUG_VIEW" unsupported | same structs; call order Configure -> PrepareV2 -> Dispatch is **mandatory**; camera position / up / right / forward required |
| Frame Generation Swapchain (Vulkan) | **3.1.7** (DLL) and **4.0.0** (RX 9000 + driver 26.10.1 adds *driver-assisted frame pacing*) | 4.0.0 needs RDNA 4 [SDK `docs/techniques/frame-interpolation-swap-chain.md` "Limitations"] | `framegeneration/include/vk/ffx_api_framegeneration_vk.h` |

Min input rates [SDK swap-chain doc "Minimum Framerate"]: 3.1.6 designed for >= 60 fps (works >= 30, pacing not optimal), 4.0.2 >= 30 fps.
The API produces **one** generated frame per rendered frame in the swapchain (`FFX_FRAME_INTERPOLATION_SWAP_CHAIN_INTERPOLATION_OUTPUTS_COUNT 2`,
`PacingData::FrameType {Interpolated_1, Real}` in `framegeneration/fsr3/vk/FrameInterpolationSwapchainVK.h`); `ffxDispatchDescFrameGeneration::outputs[4]` /
`numGeneratedFrames` exist in the header but 2x is what the swapchain presents [SDK].

### 1.2 What the provider query says here [M]

`ffxQuery(nullptr, ffxQueryDescGetVersions{createDescType, device = &VkDeviceContext})`, loader 1.0.2 + framegeneration DLL, 7900 XTX, driver 26.10.1:

| create desc type | versions offered |
|---|---|
| `FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATION` | **`3.1.6` only** (`0xf600000000c01006`). No 4.0.2 (the ML provider is filtered by the GPU). With `device = null` the count is 0 (the query needs the device) |
| `FFX_API_CREATE_CONTEXT_DESC_TYPE_FGSWAPCHAIN_VK` | **`4.0.0 *`** (`0xf65d564b81000000`, a **driver-provided** version: trailing `*`, bit 31 set) and `3.1.7` (`0xf65d564b00c01007`) |

* The FFX API picks the newest version without an override and prefers a driver-provided one only when strictly higher [SDK ffx-api.md "Driver-provided versions"], so
  **without `ffxOverrideVersion` the swapchain context is the driver's `4.0.0 *`** even on RDNA 3. It creates and presents (below) but **destroying it crashes**
  (`pOutDestroySwapchainFFXAPI` access violation, four teardown orders tried, 3.1.7 teardown clean) [M]. Pin 3.1.7 through the override (the version ids come from the query,
  never hard-coded; pick by name "3.1.7" / not driver-provided).
* ML FG contexts cannot be created through the supported override path because no id is offered; we do not guess ids or spoof the GPU (same stance as
  docs/research/upscaling.md 13: OptiScaler-style impersonation is out of scope).
* Licence (`third_party/amd_fidelityfx/LICENSE.md`): redistribution of the signed binaries allowed, **no reverse engineering**: everything here is exports / headers /
  behaviour; the provider sources in the SDK clone (`framegeneration/fsr3/**`, MIT) are readable reference for the legacy providers.

### 1.3 API shape [SDK]

Context creation (`ffxCreateContext`) takes `ffxCreateContextDescFrameGeneration {flags, displaySize, maxRenderSize, backBufferFormat}` chained with
`ffxCreateBackendVKDesc {VkDeviceContext}`, `ffxCreateContextDescFrameGenerationVersion {FFX_FRAMEGENERATION_VERSION}` (required since SDK 2.1), optional
`ffxCreateContextDescFrameGenerationHudless {hudlessBackBufferFormat}` and `ffxOverrideVersion`. Flags: `ASYNC_WORKLOAD_SUPPORT`, `DISPLAY_RESOLUTION_MOTION_VECTORS`,
`MOTION_VECTORS_JITTER_CANCELLATION`, `DEPTH_INVERTED`, `DEPTH_INFINITE`, `HIGH_DYNAMIC_RANGE`, `DEBUG_CHECKING`. Per frame:

1. `ffxConfigure(ffxConfigureDescFrameGeneration {swapChain, presentCallback, frameGenerationCallback, frameGenerationEnabled, allowAsyncWorkloads, HUDLessColor, flags, onlyPresentGenerated, generationRect, frameID})`
2. `ffxDispatch(ffxDispatchDescFrameGenerationPrepareV2 {frameID, flags, commandList, renderSize, jitterOffset, motionVectorScale, frameTimeDelta (ms), reset, cameraNear/Far/FovAngleVertical, viewSpaceToMetersFactor, depth, motionVectors, cameraPosition/Up/Right/Forward})` recorded into the game's command buffer
3. the interpolation `ffxDispatchDescFrameGeneration {commandList, presentColor, outputs[], numGeneratedFrames, reset, backbufferTransferFunction, minMaxLuminance, generationRect, frameID}`: in the swapchain's callback model dispatched *by the swapchain during present*.

`frameID` must increase by exactly 1 per frame, any other step resets the generator [SDK ffx_framegeneration.h]. `reset` = camera cut. Thread safety: contexts are not
thread safe, a mutex must cover Create/Destroy/Prepare/Dispatch and `present` when the callback model dispatches [SDK frame-interpolation-api.md "Thread safety"].

## 2. Integration model (question 2)

### 2.1 The two models

**(i) AMD frame generation swapchain** [SDK frame-interpolation-swap-chain.md, `framegeneration/fsr3/vk/*.cpp` readable source of the 3.1.7 provider]:
`ffxCreateContext(ffxCreateContextDescFrameGenerationSwapChainVK)` *replaces* our `VkSwapchainKHR` (it is destroyed and the proxy handle returned in `*swapchain`; the real
swapchain is created inside with `TRANSFER_DST | COLOR_ATTACHMENT` added). `ffxQueryDescSwapchainReplacementFunctionsVK` returns replacements for create / destroy /
get images / acquire / present / set HDR metadata plus `pOutGetLastPresentCountFFXAPI`. The app renders into **replacement ("fake back buffer") images** (usage
`TRANSFER_SRC|DST|SAMPLED|STORAGE|COLOR_ATTACHMENT`, format = `createInfo.imageFormat`; `minImageCount` of them, "always specify 2") with a binary acquire semaphore; its
present call (`pOutQueuePresentKHR`) kicks off interpolation, UI composition and the pacing. It needs:

* **four distinct `VkQueue` handles**: `gameQueue` (graphics + compute, where present is called), `asyncComputeQueue` (compute, optional), `presentQueue` (present + transfer;
  graphics + compute on it = compose on the present queue, "optimal"), `imageAcquireQueue` (any); `init` rejects equal queues, a present queue without surface support,
  a game queue without graphics + compute [SDK `FrameInterpolationSwapchainVK.cpp` `init`]. `VkQueueInfoFFXAPI::submitFunc` is an optional `vkQueueSubmit` hook for a shared queue.
* timeline semaphores, synchronization2-style barriers (all in Vulkan 1.3), `R16G16B16A16_UNORM` storage image load [SDK "Limitations"]; no `present_wait` / `present_id` /
  `swapchain_maintenance1` / `anti_lag` extension is referenced anywhere in the Vulkan swapchain sources (grep of `framegeneration/fsr3/vk`: no hits) [SDK].
* two CPU threads (interpolation wait + pacing; present with a busy-wait loop), command pools, own images; present waits for the previous frame's interpolation + UI
  composition (latency control), so two back buffers suffice even with vsync.
* UI: (a) `presentCallback` per presented frame (real and generated), (b) a UI texture registered with `ffxConfigureDescFrameGenerationSwapChainRegisterUiResourceVK`
  (premultiplied optional, optional internal double buffering), (c) `HUDLessColor` in the FG configure: the interpolation shader detects UI by difference and suppresses
  distortion there [SDK frame-interpolation-api.md "UI Composition"].

**(ii) Manual dispatch**: the FG context is driven without any swapchain: `flags |= FFX_FRAMEGENERATION_FLAG_NO_SWAPCHAIN_CONTEXT_NOTIFY` in Configure (and Prepare)
makes the provider skip the swapchain ABI check and callback setup ("allow ... run configure without swapchain" - the comment in
`framegeneration/fsr3/internal/ffx_provider_fsr3framegeneration.cpp`; the SDK sample does the same when it has no swapchain context,
`Samples/Upscalers/FidelityFX_FSR/internal/fsrapirendermodule.cpp` ~1303, ~1857). Prepare + Dispatch are recorded into **our** command buffer, the interpolated frame lands in
**our** image (`outputs[0]`). We would present both frames and pace them ourselves. The docs call this "possible, but highly discouraged" and say Driver-Assisted Frame Pacing
(RX 9000) is only used with the swapchain [SDK frame-interpolation-api.md "Dispatch frame generation", swap-chain doc "Driver-Assisted Frame Pacing"]. **Measured to work on
this GPU** (section 5.2).

### 2.2 Comparison for this renderer

| concern | (i) AMD swapchain | (ii) manual + own presents |
|---|---|---|
| pacing (the hard part) | done by AMD: moving average of frame times, variance / safety margin tuning (`FFX_API_CONFIGURE_FG_SWAPCHAIN_KEY_FRAMEPACINGTUNING`, header comment in `ffx_framegeneration_api_types.h`), busy wait presents, 1.99 presents per app frame at 12.4 / 12.6 ms gaps [M] | ours: second present at +0.5 x estimated interval on the presenter thread (`FramePacer` has the waiting primitives: [C] `src/vk/pacer.*`), estimator, FIFO / vsync interaction, VRR behaviour: a project of its own |
| driver-assisted pacing on RDNA 4 (swapchain 4.0.0) | yes (selected automatically when the driver allows) | no |
| `vk::Swapchain` / `Presenter` / `Device::present` [C] | acquire / present / images / recreate go through function pointers (small, mechanical); one new multi-queue requirement | unchanged |
| device queues [C `vk/context.cpp` creates one queue] | 4 queues (family 0 has 8, family 1 has 8 on the RX 7900 XTX [M `vulkaninfo`]) | none extra |
| exclusive fullscreen (`vulkan.fullscreen=exclusive`, window-mode.md 8: `VK_EXT_full_screen_exclusive` APPLICATION_CONTROLLED, `vkAcquireFullScreenExclusiveModeEXT(swapchain)`) | **not possible**: the real swapchain is private (no accessor in the API); `VkSurfaceFullScreenExclusiveInfoEXT` in `createInfo.pNext` is passed through but acquire / release need the handle. FG on => borderless only (the default) | works |
| overlay (F11 ImGui, drawn into the swapchain image after the blit [C `Device::present`]) | drawn into the replacement image it would be interpolated: use the present callback (section 3.4) | drawn into the final image of each presented frame as today |
| screenshots (`rvk_screenshot`, `-Snapshots`) read our back buffer [C] | unchanged for real frames; **generated frames never exist in an image we own** | generated frame is our image: capturable, comparable |
| RenderDoc | multiple queues + AMD threads: use FG off (as `rt.allow=false` for RT) | works (no extra queues) |
| menus / loading / `menu_fps` / unpaced loading [C] | FG disabled via Configure (`frameGenerationEnabled=false`, present = one copy more); loading frames not shown never touch the swapchain (as today) | trivially unchanged |
| quality / bugs / maintenance | AMD's | ours |
| FSR 4 FG (RX 9000 users) | same path, plus driver pacing | works but misses driver pacing |

### 2.3 Recommendation

**Use (i) for presentation, and build the FG dispatch as one component usable in both modes** (swapchain callback for real, manual with `NO_SWAPCHAIN_CONTEXT_NOTIFY` for the
developer path):

1. Pacing is the part users notice and AMD ships and tunes it; reimplementing it for one game (with FIFO, borderless, VRR and the 80 fps driver cap in the mix) costs more than
   the integration surface of the swapchain, which is a handful of function pointers behind `vk::Swapchain` + the queue creation.
2. The manual dispatch is what makes quality work reviewable: our screenshot tooling cannot see swapchain-presented generated frames, the manual path writes the interpolated
   frame into a transient we can save (`-Screenshots`, `water-compare`, `refcompare` burst spots) and compare, with `debug.framegen_view=generated|real|diff`. It is also the
   `rvk_vktest --fg` self-test and the fallback if the swapchain proves unusable in the game (e.g. a driver regression) - we would then still have a working (if unpaced)
   "generated frames only in captures" mode.
3. The costs of (i) are all acceptable: exclusive fullscreen already is opt-in; RenderDoc runs use FG off; the queue requirement is a capability (`framegen` unavailable with < 3 queues).

Not recommended: manual presentation as the shipped path (pacing risk) and the DX-style "HUDless-only, no swapchain" variants.

## 3. Inputs the generator needs (question 3)

Our frame, relevant points [C `docs/research/injection.md`, `src/enh/upscale.cpp`, `src/enh/gbuffer.h`]: the scene HDR target (render size) -> `upscale.fsr` at `AfterMainView`
-> FSR output (display size) substituted into the engine's post chain (bloom, DoF, tonemapping) -> back buffer (display size, B8G8R8A8 UNORM) -> `BeforeGui` -> engine GUI
(sprite batcher) -> `Device::present` blit to the swapchain image + overlay.

### 3.1 Table

| FG input | source here | notes |
|---|---|---|
| `presentColor` (swapchain: the app-facing back buffer image) | the final frame = engine back buffer incl. GUI, blitted at present [C `Device::present`] | must be **display size** and the swapchain extent: if the engine's back buffer size differs from the window (the blit scales today), FG stays off (log reason) |
| `HUDLessColor` | copy of the engine back buffer at **`Point::BeforeGui`** ("the back buffer holds the tonemapped frame, no GUI yet" [C inject.h]) into a transient, same format as the back buffer (`ffxCreateContextDescFrameGenerationHudless` allows another format) | one image copy per frame (~0.1 ms [E]); must outlive the interpolation: double-buffer when `allowAsyncWorkloads` [SDK "Resource Lifetime"] |
| `depth` | `upscale.depth` (R32F, **D3D convention 0..1, not inverted**: `shaders/upscale/prep.comp` converts the reversed hardware depth, docs/research/reverse-z.md) | no `DEPTH_INVERTED` / `DEPTH_INFINITE` flags. Today that image exists only with FSR: factor `prep.comp` + the depth / motion transients into a shared "frame data" provider used by upscale and framegen |
| `motionVectors` | `gbuffer.motion` RG16F, uv units, **previous - current**, jitter-free, camera motion for every material pixel + per-object motion for skinned / moving rigid draws (`upscale.object_motion`) [C upscaling.md 9] | `motionVectorScale = (render W, render H)` exactly as the FSR dispatch [C `Upscale::record`]. FG forces the G-buffer on (like FSR does). Sky, particles, rain, water surfaces have camera-only motion: FG's optical flow on the colour handles them in part |
| `renderSize` | scene target size (`hdr->desc()`) | `maxRenderSize` at creation = display size (or the render size of the quality mode) |
| `jitterOffset` | the same value handed to `FsrDispatch::jitterX/Y` (`Device::jitter()`, unflipped convention verified in upscaling.md 3) | 0 without FSR. The SDK sample passes the same sign for the upscaler and for Prepare (`Samples/.../fsrapirendermodule.cpp` 1685 / 1730) [SDK] |
| camera near / far / fovY / `viewSpaceToMetersFactor` | `CameraBlock` (`nearZ`, `farZ`, `projection.at(1,1)`), 0.01 (engine units are cm) as the FSR dispatch | the sample passes 0 for the factor; 0.01 is what the upscaler gets |
| camera position / up / right / forward | `CameraBlock.position` and the columns of `inverseView` | required for FSR 4 (ML) FG, ignored by 3.1.6 [SDK provider source: only stored]. Sign convention (the sample passes forward = `-direction`) must be checked with the ML provider on RDNA 4 [?] |
| `frameTimeDelta` (ms) | `cam.time - lastCamera.time` x 1000, clamped, as `Upscale::record` | |
| `reset` | camera cut: `!cam.prevValid`, first frame, skipped G-buffer frames (loading), teleports (position jump > 50 m or view direction jump > 60 degrees in one frame, tune with `rvk_perfsave` / teleport spots), FOV change, `upscale` context re-creation, FG enabled / disabled, menu <-> game transitions | also done implicitly by a `frameID` that does not step by 1; **do not advance `frameID` for frames that are submitted but not presented** (loading frames not shown [C `Device::present`]) |
| `backbufferTransferFunction` | `FFX_API_BACKBUFFER_TRANSFER_FUNCTION_SRGB` | the swapchain derives it from the colour space (`SRGB_NONLINEAR` -> sRGB); LDR: no `HIGH_DYNAMIC_RANGE` flag [M: the probe ran LDR B8G8R8A8] |
| `generationRect` | full display rect | letterboxing is not used |
| UI | section 3.4 | |

### 3.2 Problems to expect (artefact checklist for F4)

* Translucent effects (particles, rain, spells, spray) have no motion; water (sea is in the Refracted pass, animated by waves) has camera motion only: ghosting / tearing
  at wave tops; the FSR reactive mask does not feed FG. Candidates: the transparency / composition information FG 3.1.6 does not take (no mask inputs in `ffxDispatchDescFrameGeneration`),
  so only mitigation is accurate motion for the water draws or disabling generation there (not possible per region) [E].
* Fast camera turns and teleports (reset), disocclusion at the screen edges (inpainting), thin geometry (nets), the sky cube redraw (`scene: sky cube refreshed every frame`
  with water on), eye adaptation flashes, lightning (`thunder = true` spots), the post chain's DoF at render size.
* Characters: skinned motion only from the solid pass capture (alpha-pass hair / cloaks have camera motion, upscaling.md 9), SpeedTree wind: camera motion only.
* Menus with a live 3D backdrop and the GUI scenes (inventory / character / map): off (`engine::menu` state != Game).

### 3.3 State machine

`engine::menu::query` (Script.dll page predicates [C menu-fps.md 2]) gives Game / Menu / Loading / Unknown. FG active only in **Game** with a valid G-buffer. On any
other state `Configure(frameGenerationEnabled=false)` (the swapchain keeps working; "the present is slightly more expensive: one extra surface copy" [SDK]), re-enable
after the state is Game again with `reset=true` and a 2-frame warm-up; AMD recommends not toggling more often than every few seconds and re-averaging after a toggle.

### 3.4 UI composition

| mode | what | cost / quality | when |
|---|---|---|---|
| **HUDless** (`HUDLessColor`) | engine GUI stays in the back buffer; FG compares with the copy at `BeforeGui` | one copy; text near moving edges may still wobble; zero engine plumbing | **F3 default** |
| **present callback** for the ImGui overlay | callback per presented frame (real + generated): blit `currentBackBuffer` -> `outputSwapChainBuffer` and record the overlay on top (today's `Device::present` code moved into the callback; `commandList` is a `VkCommandBuffer`) | tiny; overlay at display rate, never interpolated | F3 |
| **UI texture** (premultiplied) | redirect the engine's GUI scope (`BeforeGui` -> end of `eCApplication::Draw`) into a cleared RGBA target; blend states rewritten to premultiplied-over (`color: SRC_ALPHA, INV_SRC_ALPHA`; `alpha: ONE, INV_SRC_ALPHA`), additive and other modes approximated; compose in the swapchain | the 100 % clean UI; plumbing in the draw path, GUI draws that read the back buffer break; in Full mode the sprite batcher (group 65536, `effect_ops_gui.cpp`) owns the GUI draws and could target it directly | F4 if HUDless artefacts are visible |

## 4. Pacing and game integration (question 4)

### 4.1 Rates

* Output rate = 2 x base rate. The engine runs at base (it renders and ticks once per present call it makes): **generated frames never run game code**: the swapchain
  interpolates on the GPU from two finished frames and presents them; physics, climbing, thunder (the high-fps fixes [C `src/engine/high_fps.*`, fps-limit.md 6, 10]) still see
  the base frame time. A side benefit: a 144 - 165 Hz output with the base at 72 - 83 fps stays in the range tested for gameplay (fps-limit.md 10: failures appeared at ~190 fps).
* Settings semantics when FG is on: `vulkan.max_fps` = the **output** cap, the base cap is half of it; `vulkan.menu_fps` unchanged (FG is off in menus); vsync: base = refresh / 2.
  The `FramePacer` already supports integer fractions of the refresh with DWM vblank lock [C performance.md 4]: use divisor 2 of the output refresh.
* The pacer sleeps *before* present (`pacer_.wait()` in `Device::present` [C]); keep it: AMD's present blocks until the previous frame's interpolation and composition are done and
  then places the generated and the real frame itself. **The base cadence must be even**: with a 15.6 ms timer tick (`Sleep(1)`) the two gaps varied 11.0 .. 20 ms (stddev 3.0 ms), with
  `timeBeginPeriod(1)` they were 12.37 / 12.63 ms (stddev 0.03 / 0.28 ms) [M, section 5.3]. Our `FramePacer` uses a high-resolution waitable timer + spin (lateness 0.3 ms, release error 0.005 - 0.016 ms [C]).
* FG slows the app to half the refresh with FIFO ("AMD FSR Frame Generation will slow down the application to half refresh rate, so every interpolated and real frame gets displayed for one refresh" [SDK]); measured: base asked 50 fps on a 60 Hz window -> 32 fps base, 60.0 presents/s with stddev 0.03 ms [M].
* Windowed mode: "VRR and tearing only in fullscreen"; borderless (the default) is a DWM-flipped window and was not measured with the probe [?].
* **Radeon Chill**: the 80 fps world cap is the driver's limiter inside `vkQueuePresentKHR` for `Risen.exe` (fps-limit.md 9). The swapchain's real presents are driver presents too, so the cap would apply to *total* presents [E]: base 40 / output 80 on the dev machine's profile. Tests use `-Exe RisenTest.exe`; players must switch Chill off for the profile (the README notes need a line).
* CPU: we are CPU bound at ~5 ms per frame without the cap [C performance.md]; FG adds GPU work (0.8 ms at 1080p) and ~0.5 - 0.65 core of spin in AMD's threads [M]. Output FPS gain only at the display, not in the simulation; the base must reach ~60 for 3.1.6 to look right (designed >= 60).
* Latency: +1 base frame period (the real frame is shown half an interval later and interpolation needs frame n before n-1..n is shown) [E: 12 ms at 80 fps base]. Options: Anti-Lag 2 is DX12-only in the SDK (`framegeneration/fsr3/dx12/antilag2/ffx_antilag2_dx12.h`); the Vulkan device offers `VK_AMD_anti_lag` revision 1 [M `vulkaninfo`] which could be called per frame (`vkAntiLagUpdateAMD`) - not evaluated, F5 candidate [?].
* Present timing extensions on this driver [M `out/vk0r/vulkaninfo.txt` and probe]: `VK_KHR_present_wait2`, `VK_KHR_present_id2`, `VK_KHR_swapchain_maintenance1`, `VK_KHR_swapchain_mutable_format`, `VK_AMD_anti_lag`, `VK_KHR_calibrated_timestamps` / `VK_EXT_calibrated_timestamps`, `VK_EXT_full_screen_exclusive`, `VK_EXT_hdr_metadata`, `VK_EXT_present_mode_fifo_latest_ready` are present; `VK_KHR_present_wait`, `VK_KHR_present_id`, `VK_GOOGLE_display_timing`, `VK_EXT_present_timing`, `VK_KHR_present_timing`, `VK_NV_low_latency2`, `VK_KHR_incremental_present` are not. None is needed by the FG swapchain; for verification we use the swapchain's own `pOutGetLastPresentCountFFXAPI` (the probe's sampler gives present times to ~1 us) and external PresentMon / OCAT.

### 4.2 Integration points in this code base [C]

| place | change |
|---|---|
| `vk/context.cpp` (one queue, `queueFamily_`) | request up to 3 queues of the graphics family (+1 compute-only family queue as async), restart setting `framegen.allow`; keep `queue_` as the game queue; `Context::queueSubmit` mutex also guards the game queue for the swapchain's `submitFunc` hook (pass a static trampoline) |
| `vk/swapchain.*` | optional `FgSwapchain` backend: `create` = `ffxCreateContext(swapchain VK)` with `createInfo` as built today (+ `minImageCount` 2), `acquire` / `present` / `images` / `recreate` / `destroy` through the replacement table; `fullScreenExclusive*` disabled while FG is on; present-mode choice unchanged (FIFO / MAILBOX / IMMEDIATE) |
| `vk/presenter.*` | present goes through `pOutQueuePresentKHR(gameQueue, ...)` on the presenter thread (the FG present dispatches the callback and may block for the previous frame's interpolation: exactly the work the thread exists for); the FG context mutex around Prepare (render thread) and Present (presenter thread) |
| `d3d9/device.cpp` `present()` | blit stays (unless the callback does it); `frameID++` only for presented frames; overlay moved to the callback in F3b; unpaced loading path unchanged |
| `enh/upscale.*` | split depth prep + motion handles into `enh/framedata` (shared) |
| new `vk/framegen.{h,cpp}` | wrapper like `vk::Fsr`: `load` (shares the loader), `versionList`, `createContext(swapchain/manual)`, `prepare`, `dispatchManual`, `destroy` with the documented teardown (Configure disabled -> WaitForPresents -> destroy FG -> destroy swapchain ctx -> destroy swapchain) |
| new `enh/framegen.{h,cpp}` | the feature: settings, state machine, hudless copy pass at `BeforeGui`, Prepare recording at `AfterMainView` (after `upscale.fsr`), reset detection, stats |
| `engine/menu_state.*`, `hal/vulkan/device.cpp updateMenuPacing` | FG on only in Game state; pacer rate = output / 2 |
| `overlay` | present callback entry; performance panel shows base fps, output fps, generated-frame cost |
| `core/settings.*`, `core/caps.*` | settings below; capability "Frame generation" (needs >= 3 queues, a framegeneration DLL, the FG version list non-empty) |
| `rvk_api` | no change: `rvk_screenshot` keeps reading the engine back buffer (real frames) |

### 4.3 Settings (proposal)

`framegen.mode` = `off` (default) | `fsr3` | `fsr4` (listed only when the query offers a major-4 FG version) in the Image category; `framegen.ui` = `hudless` | `texture` (advanced);
`framegen.async` (advanced, default off: the sample says profile first, memory cost); restart setting `framegen.allow` (default on, like `rt.allow`: extra queues); debug
`framegen.debug_pacing` (FG's pacing lines / tear lines flags), `debug.framegen_view`. FG forces the G-buffer on and is refused in `renderer.mode=original`; it is
meaningful in Wrapper and Full (both draw through our device). Interaction with `upscale.*`: independent; FG's input is the *upscaled, post-processed* back buffer (display
size); motion / depth are at render size; with `upscale.mode=off` the G-buffer motion + a depth prep still run (no jitter). RT features: unrelated (their output is already in the frame).

## 5. Measurements (question 5)

### 5.1 Machine and extensions [M]

RX 7900 XTX, driver 26.10.1 (LLPC), Vulkan 1.4.359 device; queue families: 0 graphics|compute|transfer|sparse x8 (present: yes), 1 compute|transfer x8 (present: yes), 2 transfer x1, 3 video
encode x2, 4 / 5 video decode x1; shaderFloat16 / Int8 / Int16 / subgroupSizeControl / computeFullSubgroups / timeline / sync2 / dynamicRendering and
`VK_KHR_compute_shader_derivatives` present. Extensions: see 4.1. The probe used family 0 queues 0 - 2 (game, present, acquire) + family 1 queue 0 (async compute).

### 5.2 FG context, manual dispatch (`fgprobe manual W H [hudless]`) [M]

| item | result |
|---|---|
| loader load | OK (`out/fg/bin`, loader + framegeneration DLL only) |
| `ffxCreateContext` FG 3.1.6, 1920x1080, LDR B8G8R8A8, with and without the hudless-format chain, with and without version override | rc 0 (all four) |
| memory query V2 (1080p) | 106.5 MB total / 39.1 MB aliasable; 114.0 MB with an RGBA16F hudless format |
| `ffxConfigure` (NO_SWAPCHAIN_CONTEXT_NOTIFY), Prepare V2, Dispatch | rc 0 / 0 / 0 |
| generated frame | moving 160x120 rect (+40 px / frame) with game motion vectors: generated rect starts 20 px behind the real one in all three frames at 1080p, 1440p, 4K and with HUDLess: **exact midpoint** (frame 0 after reset passes the frame through) |
| GPU time (optical flow + prepare + interpolation, avg of 50) | 1920x1080 **0.81 ms** (hudless 0.78); 2560x1440 **1.10 ms**; 3840x2160 **2.68 ms** (GPU alone, nothing else running) |
| validation (`VK_LAYER_KHRONOS_validation`) | no errors from our side; 5 `OpImageWrite-08795` errors and 10+ `StorageImage-FormatMismatch-ImageView` warnings from inside AMD's shaders (`rw_dilated_motion_vectors` Rg32f vs R16G16, `rw_optical_flow_input` R32ui vs R8), same class as docs/research/upscaling.md 15 |

### 5.3 AMD swapchain (`fgprobe swapchain <mode> <base fps> <frames> <fg> <version idx>`) [M]

Window 960x540 on the 60 Hz monitor (`\\.\DISPLAY1`, 2560x1440 @ 60 Hz currently), 4 queues, FG 3.1.6 in the callback model, replacement
functions, B8G8R8A8_UNORM, 2 images, QPC-sampled `pOutGetLastPresentCountFFXAPI` (1 spinning sampler thread in every run).

| case | result |
|---|---|
| create swapchain context, default version (`4.0.0 *`) | rc 0, presents fine, **teardown crashes** in `pOutDestroySwapchainFFXAPI` (access violation) in all four orders; `ffxQueryGetProviderVersion` on the swapchain context returns rc 6 |
| create with override 3.1.7 | rc 0, presents fine, clean teardown (documented order: Configure disabled, WaitForPresents, destroy FG, destroy swapchain context, destroy swapchain) |
| FIFO, FG on, base asked 50 | app throttled to 32 fps (half of 60 Hz), **60.0 real presents/s, stddev 0.03 ms** (gaps 16.63 - 16.71) |
| FIFO, FG off | 1.00 presents per app frame, app present call 0.16 ms (one extra copy) |
| IMMEDIATE / MAILBOX base 40, timer 1 ms | 1.99 presents per app frame; gaps **12.366 ms (stddev 0.03) and 12.634 ms (stddev 0.28)**, max 13.9 |
| FIFO base 25 / 29, timer 1 ms | 19.86 / 20.14 ms and 17.12 / 17.37 ms alternate gaps |
| same with the default 15.6 ms timer (`Sleep(1)` pacing in the test app) | gaps 11.0 .. 20.2 ms (stddev 3.0 ms): an uneven base spoils the pairing |
| app-side present call with FG on (IMMEDIATE) | 0.25 ms (the work is handed to AMD's threads) |
| process CPU (base 25 FIFO: FG off 1.06 cores, on 1.57; base 40 IMMEDIATE on 1.65) | the baseline contains the probe's own spinning sampler thread: FG threads ~0.5 - 0.65 cores |

Not measured (open [?]): borderless / fullscreen presentation (flip, VRR), HUDLess composition through the swapchain, the present callback, async compute, hot toggling,
resize, exclusive fullscreen, the driver-provided 4.0.0 teardown crash cause, behaviour with the engine, RDNA 2 / 4 / other vendors.

### 5.4 Reproduction

`out/fg/fgprobe.cpp` (+ `fg_manual.inl`, `fg_swapchain.inl`, `build.cmd`: cl with the SDK clone's headers, `vulkan-1.lib`; standalone raw Vulkan, does not touch the repo build);
`out/fg/bin/` = the Authenticode-checked loader + framegeneration DLL copies; logs `query1.txt`, `manual_*.txt`, `sc_*.txt`. Modes: `fgprobe query`, `fgprobe manual W H [hudless]`,
`fgprobe swapchain <fifo|mailbox|immediate> <base fps> <frames> <fg 0|1> <version idx 0|1|2> [async] [WxH]`, env `FGPROBE_ORDER` selects the teardown order, `VK_INSTANCE_LAYERS=VK_LAYER_KHRONOS_validation`.

## 6. Staged plan (question 6)

| stage | content | verification | files | est. [E] |
|---|---|---|---|---|
| **F1 scaffolding** | `vk::FrameGen` wrapper (load / version list / create / destroy / manual prepare + dispatch); queue creation (3 + 1) behind `framegen.allow`; settings + capability (greyed without queues / DLL); packaging decision for the 36.8 MB DLL (below); `rvk_vktest --fg` = productised manual probe (midpoint check on a synthetic scene, SKIP without the DLL, runs with validation) | rvk_vktest --fg PASS; rvk.log `framegen: versions ...`, `queues: ...`; unit tests for the state machine / reset rules | `src/vk/framegen.*`, `src/vk/context.*`, `src/core/settings.*`/`caps.*`, `tools/vktest/main.cpp`, `CMakeLists.txt`, `scripts/run.ps1`/`package.ps1` | 2 days |
| **F2 inputs** | `enh/framedata` shared depth / motion; hudless copy pass at `BeforeGui`; camera data + `reset` rules; **manual generation in the frame** (Prepare after `upscale.fsr`, dispatch at BeforeGui+, output transient) with `debug.framegen_view = generated | real | diff` and capture support | game runs (`rvkcompare` spots incl. `hero_idle`, `net8_*`, `beach_rain`, `sea_cliff`, harbour spots; `water-compare -Burst` with generated frames saved) + review by `shot-reviewer`; validation clean apart from AMD's | `src/enh/framedata.*`, `src/enh/framegen.*`, `shaders/` none, `docs` | 3 days |
| **F3 presentation + pacing** | FG swapchain backend in `vk::Swapchain` (+ presenter / device queue lock / `frameID` / state machine / Configure per frame), pacer rate = output / 2, present callback with the overlay, HUDLess in Configure, fps panel (base / output / generated ms), `rvk.log framegen:` 5 s stats (real presents per app frame, gap mean / p5 / p95 / stddev, even / odd gap) | testdemo + `rvk_highfps` frame-time distribution with `RisenTest.exe`; `framegen.debug_pacing` lines via `scripts/capture-window.ps1`; real-present cadence from `pOutGetLastPresentCountFFXAPI`; sync validation; menu <-> game <-> loading toggles (`rvk_menufps`: quick-load time must stay 2.1 s); window modes (`window` commands), resize / vsync toggle (recreate path), borderless 120 Hz | `src/vk/swapchain.*`, `presenter.*`, `d3d9/device.cpp`, `hal/vulkan/device.cpp`, `enh/framegen.*`, `overlay/*` | 4 - 5 days |
| **F4 quality** | artefact review per checklist 3.2; UI texture mode if HUDless wobble is visible; per-feature fallbacks (e.g. FG off for the inventory scenes, within-water spots), reset tuning (teleports via `rvk_perfsave`), pacing tuning keys (`FfxApiSwapchainFramePacingTuning`), async compute A/B, Anti-Lag evaluation | burst captures of generated frames (manual view) vs ground truth at 2x base (render at 2x fps and drop every other frame as reference), motion sequences at the listed spots; PresentMon / OCAT frame-time capture in borderless | `enh/framegen.*`, `d3d9/draw.cpp` (UI redirect), `shaders` (UI compose if needed) | 3 - 5 days, open ended |
| **F5 FSR 4 provider + defaults** | provider choice from the version query (`fsr4` listed only where offered), ML call order and camera vectors, driver pacing note, defaults decision (off), README text (Chill, base fps >= 60, borderless), packaging | needs an RDNA 4 GPU: **cannot be verified on this machine**; keep behind the query so it is inert here | `vk/framegen.*`, settings, `package/rvk/README.txt` | 1 - 2 days + hardware |

**Packaging of the DLL** (decision needed): vendoring `amd_fidelityfx_framegeneration_vk.dll` (36.8 MB) next to the 36 MB upscaler grows the repo and the nightly zip to ~75 MB.
Alternatives: ship it in a second optional asset (`risen-enhanced-graphics-framegen.zip`, unzip into `bin\rvk`) produced by `package.ps1` from a hash-verified SDK download, or fetch at
CI time instead of committing the binary. `VERSION.md` records tag / commit / hash either way; the loader finds it next to itself.

**Verification of even pacing** (all stages): `framegen:` log lines from the swapchain's present counter sampled on the presenter thread (the probe's method), compared with the
target (`target_output = 2 x base`); `-PresentStalls` samples the presenting thread's stack (module / export of any wait); GPU timestamps `-Perf` for the FG passes
(`pass framegen.prepare`, hudless copy, the callback); external capture (PresentMon / OCAT, an RTSS-free frame-time trace) in borderless; the FG debug flags
(`FFX_FRAMEGENERATION_FLAG_DRAW_DEBUG_PACING_LINES` "two grey lines when pacing is right", `DRAW_DEBUG_TEAR_LINES`, `DRAW_DEBUG_RESET_INDICATORS`) seen in `capture-window.ps1`.

## 7. Risks

| risk | mitigation |
|---|---|
| driver-provided swapchain `4.0.0 *` is the default pick and crashes at teardown | always pass the override with the 3.1.7 id (chosen by name from the query); re-test per driver; log the choice |
| 4 queues unavailable (Intel: one family queue; hypothetical) | capability check at device creation; FG greyed out with the reason; fallback manual mode only for debug |
| exclusive fullscreen incompatible | FG forces borderless (logged); the setting is greyed in the overlay |
| Radeon Chill / FRTC caps in the driver count generated presents | document; detect (output fps stuck at 80 with base 40) and show a hint in the overlay [E] |
| FG toggling while loading / menus / teleports | state machine + reset + warm-up; AMD: do not toggle more often than every few seconds |
| thread safety (render thread Prepare vs presenter thread Present with the callback model) | one mutex around both, as the sample does (`FGContextScopeLock`) |
| frame-time spikes (shader/pipeline builds, [C performance.md 6]) disturb the pacer's moving average | FG is disabled until pipelines are warm (the G-buffer arming logic already waits), warm-up frames after hitches |
| validation noise from AMD's shaders | document as known (like `rw_luma_history`); our own errors must stay 0 |
| RenderDoc / PIX | FG off (`rt.allow=false` advice extended) |
| the engine's GUI is not separable cheaply | HUDless first; texture mode only if needed |
| gains: CPU-bound game | the simulation does not speed up; the value is display smoothness and high-refresh output; say so in the README |

## 8. What could not be answered without a prototype in the game [?]

* Visual quality of 3.1.6 on Risen's content (water, particles, GUI over a live scene, nets, foliage) and whether HUDLess is enough.
* Actual latency and CPU cost with the engine thread (the probe is synthetic), borderless / VRR behaviour of the real presents, the Chill interaction.
* Whether `renderer.mode=full` (native frame) changes any input timing (it should not: same images).
* FSR 4 FG anything (no RDNA 4 here), the ML provider's camera vector conventions, driver-assisted pacing.
* Why the driver-provided swapchain 4.0.0 crashes at destroy (ours or theirs; reproduced with the SDK-documented order).
* RDNA 2 / NVIDIA / Intel behaviour of FG 3.1.6 and of the queue requirement (the SDK states no vendor restriction for 3.1.x).
