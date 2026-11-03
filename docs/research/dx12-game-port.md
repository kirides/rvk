# The game on the DX12 backend: plan (user order 2026-10-30: "make dx12 work, do everything necessary")

Goal: `vulkan.api = vulkan|d3d12` (restart setting, config.toml `[vulkan] api`, `run.ps1 -Api d3d12`); the D3D9-model device (`d3d9::Device`) renders through the DX12 RHI
backend in wrapper and full mode with all enhancements, FSR + frame generation (AMD DX12 DLLs), overlay, screenshots, pacing. Vulkan stays the default and must not regress
(CPU frame at the perf savegame: no regression beyond noise; D3D12 <= Vulkan + 5 %). Decisions: user approved vendoring the DX12 FFX DLLs (~87 MB, upstream v2.4.0,
`Kits/FidelityFX/signedbin/` loader_dx12 / upscaler_dx12 / framegeneration_dx12 + `framegeneration/include/dx12` headers; procedure in third_party/amd_fidelityfx/VERSION.md) and
shipping D3D12Core.dll (Agility 619, `rvk\d3d12`, hlsl-rhi.md 6d). Only the RX 7900 XTX is available for validation.

## Architecture (decided)
One d3d9 implementation; a COARSE runtime seam `rhi::GameDevice : rhi::Device` (src/rhi/game_device.h; src/rhi/vulkan/game_device.cpp, src/rhi/d3d12/game_device.cpp) that owns
the frame loop (Context/Frames/MeshArena/Bindless/PipelineCache/Swapchain/Presenter/GpuTimer/Transients/FgSwapchain on Vulkan). d3d9::Device holds a unique_ptr<GameDevice>, no vk/*.h.
Opaque handles `ShaderModule`, `GamePipeline`, `ChunkHandle` (uint64), `GpuRef` (one 64-bit value replacing VkDeviceAddress in shader-visible data: Vulkan = device address,
D3D12 = slot | offset << 32 = RvkBufRef{slot, offset}; `plus(delta)` shifts by refShift 0 / 32). Per-draw logic (prepareDraw, constants, DrawBlock, interning, batching, replacers)
stays neutral inline; virtual calls only per chunk / scope / pipeline resolve / resource (<600 per frame). The Vulkan implementation of recordChunk is the current code moved verbatim.
No templates, no second d3d9::Device, no per-draw virtual RHI calls.

D3D12 specifics: PSO key (blend, raster, depth, stencil masks, topology class, input layout) next to PipelineDesc, async creation, draws skipped while pending (skippedCompiling
mechanism), ID3D12PipelineLibrary file bin\rvk\cache\d3d12_psos.bin; one direct command list per record chunk (160 packets) from per-thread allocator pools (no bundles), appended in
order at scope ends; present on a dedicated present queue (fence wait) from the existing Presenter thread, waitable swapchain, ALLOW_TEARING for vsync off; one bindless heap
partitioned by kind (65,536 CBV/SRV/UAV, 2,048 samplers), deferred slot free by fence value; legacy resource states; D3D12MA; ring in GPU_UPLOAD heap when supported;
scaled vertex formats (UBYTE4, SHORT*, UDEC3, DEC3N) via integer inputs + VS variant (inputOverrides); triangle fans via generated index buffer; no gl_PointSize (point lists:
count in WP0, expand to quads if any); no scaling copy: StretchRect / present blit / mips via backend draw or compute (`textureBlit = true`); vertex-store captures (rt skin, object motion)
go to a device-local UAV pool `allocCapture`; RT builds from raw VAs need `registerAddressRange`; AMD finding: never ALLOW_UPDATE + ALLOW_COMPACTION on one BLAS.
Shader ABI: DXIL has no pointers: `RVK_PTR_LOAD / LOAD_AT / STORE` macros, RvkDrawPush as 7 x uint4 (arrays in cbuffers pad to 16 B), translator emits the macros.

## Work packages (order: WP1 || WP2 (a,b,c) -> WP3 -> WP4 -> WP5 -> WP6 -> WP7 -> WP8 -> WP9; WP0 first, small)
- WP0 spike: count distinct PSO keys (testdemo + perf savegame), point-list / fan draws, exotic decl types, capture-variant draws.
- WP1 (= stage S1 "DXIL parity") every shader compiles to signed DXIL from the same HLSL: macros above, `shader::embedded::dxil(name)`, DXIL disk cache bin\rvk\cache\dxil, translated shaders
  compile only for the active backend; checker `rvk_shadertool corpus corpus --dxil` + variants.
- WP2a seam definition + GpuRef + shader modules (Vulkan only, no behaviour change); WP2b textures/passes on rhi::CommandList, optional-module interfaces (Upscaler, FrameGenerator,
  PresentWrapper); WP2c packet application / record threads / pipelineFor / StretchRect / readbacks / timestamps / regions behind the seam; `rvk_d3d9test --backend`.
  Gate after each: rvk_tests, rvk_d3d9test --validation, rvk_vktest (--passes --rhi --rt --skin --fsr --fg), in-game wrapper + full with -SyncValidation, CPU A/B at the savegame.
- WP3 D3D12 core: queue/fence timeline, 2 frames in flight, per-slot allocators, retire(), ring chunks, D3D12MA, mesh arena (GENERIC_READ), full format table (BC, D16/D24S8/D32S8,
  swizzles for A8/L8/A8L8/A2R10G10B10), texture upload alignment, samplers, DXIL modules, readbacks, `vulkan.api` setting + bootstrap, DXGI adapter enumeration, delay-load of d3d12/dxgi.
- WP4 D3D12 draw path: PSO factory, state mapping, packet application (root constants 28 DWORDs), chunk lists on the 2 record threads, clears, fans, vertex fixups, blit. Verify: all 20
  rvk_d3d9test cases on d3d12, in-game wrapper + full to the world, `refcompare` Vulkan vs D3D12.
- WP5 presentation: DXGI flip swapchain, present queue, Presenter generalised, resize / minimise / tearing / exclusive fullscreen, present blit + overlay; menu fps / vsync / MaxFps unchanged.
- WP6 enhancements + RT on D3D12: remaining raw leftovers behind the backend (replacer shader modules, vk::rt::*, CheckDeviceFormat), capture pool, RT scene records / TLAS slots;
  verify water-compare, shadow-compare, rvk_rtwalk, perfsave with water.ocean=fft, rt.reflections, shadows.rt, ao, gi; -PerfFrame vs Vulkan.
- WP7 FSR upscaling + frame generation DX12 (vendor the DLLs; ffx_api_dx12.h; FG swapchain `ffxCreateContextDescFrameGenerationSwapChainForHwndDX12`, UI resource registration).
- WP8 overlay: ImGui DX12 backend (vcpkg imgui `dx12-binding`), UI resource for FG.
- WP9 hardening: timers (query heap), PIX events, DRED / crash markers, memory budget, PSO prewarm tuning, CPU A/B (D3D12 <= Vulkan + 5 %), package.ps1 (D3D12Core.dll + licence, FFX DX12 DLLs),
  CLAUDE.md + hlsl-rhi.md, CI proving the Vulkan-only build runs without d3d12.dll, unit tests for PSO key / GpuRef / format table.

## Status

### WP2a (2026-10-30, Vulkan only, no behaviour change) - built, verified
Built on 2774402. New: `src/rhi/game_device.h` (comment block there = the seam's design and its TODO list), `tests/gpu_ref_tests.cpp`.
- **`rhi::GpuRef`** (`uint64 bits`; `fromWords(low, high)`, `low() / high() / pair()`, `null()` / `explicit operator bool`, `plus(delta)` = the active backend's shift, `plusAt(delta, shift)` for tests / other backends,
  `==`). Vulkan: the bits are the buffer device address (layout of `abi.h` unchanged: the push constant / block fills write `ref.bits`, the ABI structs stay `uint64`). D3D12 (later): `slot | offset << 32`, shift 32:
  `plus` adds the byte delta to the high word only, a negative delta (the biased windows `base - first * 16`) wraps modulo 2^32 inside the offset word and never touches the slot; the shader adds the index back with 32-bit
  arithmetic (rvk_tests `gpu_ref_*`: shift 0 = plain address arithmetic, shift 32 incl. the wrap, the window registers for every VS / PS window start, `refShift()` switch). `rhi::refShift()` / `setRefShift()` = a process-wide
  value set by the game device's creation (one device per process).
- **`rhi::GpuAddr {GpuRef ref; uint64 va}`**: a location in both forms (shader reference + raw GPU VA for acceleration structure geometry / TLAS instances); `plus`, `==`. **`rhi::RingAlloc {uint64 buffer (opaque handle; vk::vkBuffer()),
  uint64 offset, void* ptr, GpuRef ref, uint64 gpuVa; addr()}`** replaces `vk::RingAlloc`; `vk::MeshArena::Alloc::address` -> `addr` (GpuAddr); `d3d9::Buffer::deviceAddress()` -> `gpuAddress()` (GpuAddr). The two forms are now
  distinct types in `enh/rt_scene` (records / push data take `.ref`, `TriangleGeometry` / `buildTlasAt` take `.va`), `enh/gi` and the skinning verify buffer read `rhi::Buffer::shaderRef / shaderRefRw` instead of `gpuAddress()`.
  No `VkDeviceAddress` is left in `d3d9/` or `enh/`.
- **`rhi::ShaderModule`** (`enum class : uint64 { None }`, by value hashable / atomic): `d3d9::Shader::module / tintModule / lodModule / variants[]`, `Device::variant / variantModule / uberModule / tintModule / lodModule`, `moduleHashes_`,
  `vk::PipelineDesc::vs / fs`, `PipelineCache::prepareShader` and the replacers' modules (`enh/water`, `shadows`, `gbuffer`) use it. Vulkan: the value is the `VkShaderModule` (`vk::vkModule / rhiModule`); `PipelineDesc` stays byte-hashed.
- **`rhi::GameDevice : rhi::Device`** (`createShaderModule(const rhi::Shader&, const char* name = nullptr)` any thread, `destroyShaderModule(m, deferred)`, `refShift()`). Vulkan implementation = `VkDeviceImpl` (it now derives from
  `GameDevice`), made by `rhi::vulkan::wrapGame(ctx, frames, bindless)` (`wrap()` returns the same object as a `Device`). `d3d9::Device` holds `unique_ptr<rhi::GameDevice> game_` (created in `create()` right after the bindless set,
  reset before `frames_` in `destroy()`), `game()` / `rhi()` return it; the async shader compile pool calls `game_->createShaderModule`.
- Counts (`vk::` / `Vk*` mentions, `#include "vk/..."`), before -> after: device.h 68 / 113 / 14 -> 68 / 82 / 14; device.cpp 69 / 76 / 1 -> 64 / 66 / 1; draw.cpp 41 / 64 / 0 -> 37 / 49 / 0. No d3d9 file gained a `vk/` include.
- Verification (RX 7900 XTX): rvk_tests 207 / 207 (9 new), rvk_d3d9test --validation 20/20 PASS (0 validation errors), rvk_rhitest --validation PASS (Vulkan + D3D12), rvk_vktest plain / `--passes` / `--passes --rhi` / `--rt` /
  `--skin` PASS (0 validation errors). In game (windowed, `-Validation`; wrapper with `-SyncValidation`; water.ocean=fft, rt.reflections=medium, shadows.rt=near, ao, gi; sea_cliff / beach_sun / lake_jungle / pub_interior /
  shadow_inland_a; references = committed build in `build\Ref4`, `out\rhi-migrate\wp2a_ref_*`): 0 VUID / SYNC / warnings / errors in wrapper and full, every feature's first-frame line present; pictures equal within run-to-run
  noise (mean abs difference 0.5 - 9.1 of 255; two runs of the reference differ by 7.1 / 7.6 at sea_cliff / shadow_inland_a in full mode, new vs the second reference 5.4 / 5.6). CPU at the savegame (`rvk_perfsave`, lod 2x, ~1,440
  draws, 14 windows per run, interleaved A/B): wrapper CPU per frame ref 6.41 / 6.48 / 6.50 / 7.09 / 6.99 ms, new 6.45 / 6.50 / 6.83 / 7.14 / 6.47 ms (one new run 7.45 with 2.2 ms present: a disturbed run, rerun clean);
  draw calls 1.92 - 2.14 on both sides; pipeline binds 445 - 449 (ref) vs 440 - 442 (new); full mode CPU ref 9.99 / 9.29, new 9.53 / 9.38 ms: no difference beyond the +-0.15 ms noise of one run, the machine drifts by +-0.5 ms
  between the hours. `-PerfFrame 4200` (24 frames averaged, 1920x1080) passes ref -> new: sea_cliff water.ocean 0.307 -> 0.291, water.rt 0.318 -> 0.273, gi 1.033 -> 0.999, gi.screen 0.731 -> 0.729, gbuffer.ao 0.467 -> 0.461;
  lake_jungle shadows.rt trace 0.969 -> 0.969, water.rt 0.828 -> 0.827, gi.screen 1.364 -> 1.351: equal.
- Left for WP2b / WP2c (TODO): `Texture` / `Surface` / `Buffer` still hold `vk::Image`, `VkBuffer`, `vk::MeshArena::Alloc`; `prepareForSampling(VkCommandBuffer)`, `PassContext::cmd`, `PresentOverlay::record(VkCommandBuffer)`;
  `Frames` / `MeshArena` / `Bindless` / `PipelineCache` / `Swapchain` / `Presenter` / `GpuTimer` / `Transients` / `FgSwapchain` are still members of `d3d9::Device` (the GameDevice only wraps ctx / frames / bindless), the ring
  allocator is still `vk::Frames::allocate` (needs a non-virtual neutral front: it is called several times per draw); `RecordState` / packet application / record threads, `pipelineFor` (`GamePipeline`), `StretchRect`, read-backs,
  `ctx_.features()` queries, `moduleLabel(ctx_...)`, `vk::names::*`; `shader::abi::PushConstants` is still 8 x uint64 (RvkDrawPush as 7 x uint4 for DXIL comes with WP1 / WP4); raw `rhi::Buffer::gpuAddress()` users with shader
  writes into Readback memory (`gi` diag, the skin verify buffer) need the capture pool on D3D12 (WP6).

### WP2b (2026-10-30, Vulkan only, no behaviour change) - built, verified
Built on 7540a9a (uncommitted tree when written). Resources, passes and the optional modules are behind `rhi::GameDevice`; what is left raw in d3d9 is the frame loop / draw path (WP2c).
- **RHI additions** (`rhi.h`, both backends; D3D12 implements them too: `rvk_rhitest` PASS): `TextureDesc::swizzle` (the sampled view's channel mapping: L8 -> rrr1), `Texture::layout()` (the tracked layout, MAX_ENUM while mips
  differ; Vulkan = `vk::Image`'s tracker, the one source of truth; D3D12 = the layout of the last transition) + `Texture::setDebugName`, `CommandList::copyBufferToTextureBox(dst, src, BufferTextureCopy{bufferOffset, rowPitch (0 = tight),
  sub, offset, extent})` (the dirty-box upload), `TextureBlit::srcOffset / dstOffset` (StretchRect boxes), `rhi::FormatBlock formatBlock(VkFormat)` + `formatBytes` / `bufferRowPitch` / `copyBytes` for every D3D9 storage
  format incl. BC1-5 and 16-bit packed (read-backs). A texture blitted onto itself that lies in GENERAL stays in GENERAL (the engine's mip chains; found by the first in-game run: VUID-vkCmdBlitImage2-srcImageLayout).
- **`rhi::GameDevice`** (`game_device.h`) grew: `gameCaps()` (`GameCaps`: rayTracing, vertexStores, storageImageExtendedFormats, compute limits, `uploadRowAlign` 1 / `uploadOffsetAlign` 16 on Vulkan: D3D12 will say 256 / 512 and
  `Texture::upload` pads its staging rows to it), `formatSupport(VkFormat, VkImageUsageFlags, blend)`, `frameCommands()` / `uploadCommands()` / `passCommands()` (persistent `rhi::CommandList` wrappers over the frame's command
  buffers; `passCommands` also resets the bound state: what an injected pass records into), `nativeCommandBuffer(CommandList&)` (the Vulkan-only escape hatch: `void*`; used by nobody outside the backend yet because the modules take
  `CommandList*`), `allocRing(size, align, minOffset)` + `ringBuffer(RingAlloc)` (the chunk as an `rhi::Buffer`), `allocCapture(size, align)` (vertex-store captures: rt skin outputs, object motion; Vulkan = the ring),
  `frameValue / frameSlot / completedFrame / waitFrame / retire`, `allocMesh / freeMesh` (`MeshAlloc{Buffer* buffer, native, offset, GpuAddr addr, backend[3]}`: the arena behind `d3d9::Buffer`), `transient(key, TransientDesc)` /
  `releaseTransient` / `transientBytes` (the passes' images are `rhi::Texture`s now; `vk::Transients` stays for `rvk_vktest` only), start-up diagnostics (`probeFrameGeneration`, `capabilityReport`, `runRaytracingSelfTest`,
  `logMemoryStats`), `createUpscaler / createFrameGenerator / createImGuiBackend`; free functions `rhi::enumerateAdapters(Backend)` (`AdapterInfo`), `setDebugNames / debugNamesEnabled`, `rhi::kFramesInFlight`.
- **Optional modules** (`src/rhi/modules.h`; Vulkan adapters in `src/rhi/vulkan/modules.cpp` over `vk/fsr.*`, `vk/framegen.*`, `vk/fg_swapchain.*`, which stay where they are): `rhi::Upscaler` (load, create / destroy context,
  `dispatch(UpscalerDispatch)` with `Texture*` + `CommandList*`, `generateReactiveMask`, provider / jitter / version queries), `rhi::FrameGenerator` (`configure / prepare / dispatch`, `FrameGeneratorConfigure::present` names the
  `PresentWrapper`), `rhi::PresentWrapper` (AMD's frame generation swapchain: `contextMutex`, `setUi(PresentUi{wants, record(CommandList&, extent)})`, counters, sampler, `realPresentCount`), `rhi::ImGuiBackend`
  (`src/rhi/vulkan/imgui_backend.cpp`; the vendored ImGui Vulkan backend moved to `rhi/vulkan/imgui_vulkan_impl.cpp`; its texture upload and shutdown hold the queue lock themselves), `rhi::presentGapStats`.
  `rhi/vulkan/wrap.h` gained `imageOf / commandBufferOf / contextOf` (the backend's own users), `wrapFgSwapchain`, `wrapGame(..., MeshArena*)`.
- **d3d9**: `Texture` owns an `rhi::Texture` (`rhi()`; the slot is `heapSampled()`, the layout `rhi().layout()`; `image()` is the one remaining bridge to the Vulkan image for the not yet moved draw path: scope attachments / packet
  application / present), `Buffer` holds a `rhi::MeshAlloc` (`Binding.buffer` is the opaque backend handle), `prepareForSampling / upload / readable / readImageLevel / blit / StretchRect / loadSurfaceFromSurface` run on
  `rhi::CommandList`s (`Device::frameList() / uploadList()`; read-backs through an `rhi::Buffer` in Readback memory, repacked to tight rows), the upload ring + dirty-box logic call `GameDevice::allocRing / ringBuffer`,
  `PassContext::cmd` is `rhi::CommandList&`, `PresentOverlay::record(rhi::CommandList&, extent)`, `GbufferTargets` / `setGeneratedFrame` take `rhi::Texture*`, `Device::transient / releaseTransient / ring / frameSlot / retire`,
  `presentWrapper()`; the public `ctx() / frames() / transients() / swapchain()` accessors are gone. `enh/rhi_scope.h` is a 20-line facade (`wrap(Texture)` identity). hal: `CheckDeviceFormat` -> `formatSupport`, adapter query ->
  `enumerateAdapters`, caps report / RT self test / memory stats / FG probe -> `GameDevice`.
- Counts before -> after (`vk::` mentions / any `Vk*`-`VK_*` token (mostly RHI vocabulary: VkFormat, VK_IMAGE_LAYOUT_*, VkExtent2D; overlay: Win32 `VK_F1`..) / lines naming a Vulkan object or call (VkCommandBuffer, VkImage, vkCmd*..)
  / `#include "vk/..."`): d3d9 182 / 501 / 155 / 18 -> 153 / 473 / 138 / 18 (the rest is the draw path, record threads, frame loop, pipelines, present: WP2c; `resources.cpp` has none but the format table `vk::formatInfo` / `d3dfmt`),
  enh 106 / 258 / 20 / 38 -> 9 / 228 / 0 / 2 (left: `vk::d3dFormatName`, the HLSL attribute text `[[vk::location]]`, `vk/formats.h`), overlay 2 / 50 / 4 / 0 -> 0 / 47 / 0 / 0, hal/vulkan 25 / 19 / 8 / 7 -> 17 / 3 / 0 / 2 (`vk/formats.h`),
  native 0, engine 3 / 1 / 0 / 2 -> 1 / 1 / 0 / 1 (`vk/formats.h`).
- Verification (RX 7900 XTX): rvk_tests 207 / 207, rvk_d3d9test --validation 21 / 21 (new: mip chain StretchRect, the engine's blit-onto-itself) 0 validation errors, rvk_rhitest --validation PASS (Vulkan + D3D12), rvk_vktest plain /
  `--passes` / `--passes --rhi` / `--rt` / `--skin` / `--fsr` / `--fg` / `--names` PASS. In game (windowed, `-Validation -SyncValidation`, water.ocean=fft, rt.reflections=medium, shadows.rt=near, ao, gi; sea_cliff / beach_sun /
  lake_jungle / pub_interior / shadow_inland_a; references = HEAD build `build\Ref5`, pictures in `out\rhi-migrate\wp2b_*`): wrapper and full 0 VUID / SYNC / warnings, every feature's first-frame line present; with
  `upscale.mode=native_aa` + `framegen.mode=fsr3` only AMD's accepted `rw_*` format messages; overlay open (graphics tab, scrolled) clean in the back buffer (`out/wp2b/overlay2`) and through the frame generation swapchain's
  present callback (402 of 402 callbacks with the overlay, `overlay3`). Pictures: mean abs difference new vs reference 1.1 - 4.4 of 255 (shadow_inland_a 8.7 - 15.4); two runs of the reference differ by 1.9 - 4.3 / 14.1 - 15.3
  (the sky / water of that spot move between runs). CPU at the savegame (`rvk_perfsave`, lod 2x, ~1,510 draws, 14 windows per run, 5 interleaved runs each): wrapper CPU per frame ref 8.33 / 8.25 / 8.15 / 8.41 / 9.85 ms,
  new 8.31 / 8.12 / 8.31 / 7.79 / 8.60; draw calls 2.44 vs 2.37 ms; pipeline binds 404.6 vs 405.9; full mode (2 runs each) 13.70 / 14.77 vs 13.47 / 13.51 ms (the machine ran hotter than during WP2a: 6.5 ms then).
  `-PerfFrame 4200` (24 frames averaged, 1920x1080, three runs each, sea_cliff / lake_jungle): the GPU time per pass swung by up to 3x from run to run in BOTH builds this evening (e.g. water.rt at sea_cliff ref 0.21 / 1.23 / 0.47 ms, new 1.01 / 0.28 / 0.39; gi.screen at lake_jungle ref 2.05 / 3.02 / 3.13, new 3.41 / 1.78 / 1.94; shadows.rt trace ref 1.69 / 1.85 / 1.93, new 1.85 / 1.07 / 0.93), the ranges overlap everywhere: no difference beyond noise can be read from them (WP2a measured within 5 % on a quiet machine). Raw logs `out/rhi-migrate/perf/wp2b_*`.
- Left for WP2c: `Frames` / `MeshArena` / `Bindless` / `PipelineCache` / `Swapchain` / `Presenter` / `GpuTimer` / `FgSwapchain` are still members of `d3d9::Device`; `RecordState` / packet application / record threads, scope
  attachments (`Texture::image()`), `pipelineFor` (`GamePipeline`), the present path (blit to the swapchain, the overlay's rendering scope), `submitFrame / beginFrame` (read-backs flush the frame through them), `ctx_.features()`
  queries, `moduleLabel(ctx_...)`; `bootstrap.cpp`'s `vk::probe()`. `GameCaps::uploadRowAlign / uploadOffsetAlign` are untested above 1 / 16 (the padded-row staging path in `Texture::upload` is compiled but never taken on Vulkan).

## Next steps (written 2026-10-30, at the break; user order: "make dx12 work, do everything necessary, do not report back until it works")

Done and committed: S1 = WP1 (2774402: every shader to signed DXIL, uint2-pair address ABI, parity tests, checker `rvk_shadertool dxil-check`; the helpers are `RvkRef` / `rvkPtr` / `rvkRw*`, not the `RVK_PTR_*` macros
the plan sketched), WP2a (7540a9a: `rhi::GameDevice`, `GpuRef` / `GpuAddr` / `RingAlloc`, `ShaderModule`). Earlier: Agility SDK setup (2db438a), RHI feature gaps, passes + RT scene on the RHI (957dc08 .. 49372b1).
WP2b was in progress when this was written and is now committed (see Status): **WP2b** (agent a2d5e1e2346396237, uncommitted tree: textures / surfaces / buffers as RHI objects, `PassContext::cmd` -> `rhi::CommandList&`, `Upscaler` / `FrameGenerator` /
`PresentWrapper` modules in src/rhi/modules.h, capture pool, `formatSupport`, adapter enumeration). When it reports: rebuild, run rvk_tests / rvk_d3d9test --validation / rvk_rhitest --validation, check its in-game numbers, commit.

Remaining, in order (each one agent with a brief like the previous ones: read CLAUDE.md + this doc + hlsl-rhi.md, gates = offline suites + in-game Vulkan wrapper / full with -Validation -SyncValidation + pictures vs a reference build +
savegame CPU A/B within about 0.15 ms, no commit by the agent, docs Status section updated):
1. **WP2c** (Vulkan only): `RecordState` / packet application / record threads behind `recordChunk / executeChunks / beginScope / endScope`; `pipelineFor` + pipeline cache + prewarm + `StaticRasterKey`; `StretchRect`, read-backs,
   timestamps, regions behind the seam; `Frames` / `MeshArena` / `Bindless` / `PipelineCache` / `Swapchain` / `Presenter` / `GpuTimer` / `Transients` / `FgSwapchain` owned by the backend (inline neutral ring front end: it is
   called several times per draw); `rvk_d3d9test --backend vulkan|d3d12`. After it d3d9 / enh / overlay / hal / native contain no Vulkan.
2. **WP0 spike** (small, can run any time before WP3/4): distinct PSO keys at testdemo + perf savegame, point-list and triangle-fan draws, exotic decl types (UBYTE4, SHORT*, UDEC3, DEC3N), capture-variant draw counts.
3. **WP3** D3D12 core: queue + fence timeline, 2 frames in flight, retire(), ring chunks (GPU_UPLOAD heap if available), D3D12MA, mesh arena, full format table (BC, depth, swizzles), texture upload alignment,
   samplers, DXIL modules, readbacks, `vulkan.api` setting (+ config.toml, `run.ps1 -Api d3d12`), DXGI adapter enumeration, delay-load of d3d12 / dxgi, `TypedUAVLoadAdditionalFormats` in the requirements check, a default
   texture per kind at heap slot 0 (cube / 3D), SRV + UAV pairs for GI / capture buffers and raw SRVs for the engine's vertex / index buffers (hlsl-rhi.md 9 host contracts).
4. **WP4** D3D12 draw path: PSO factory (async, skip while pending, `ID3D12PipelineLibrary`), state mapping, packet application (28 root DWORDs), chunk command lists on the 2 record threads, clears, triangle fans, vertex format
   fixups, point size, blit; verify `rvk_d3d9test --backend d3d12` 20/20, then in game wrapper + full to the world, `refcompare` Vulkan vs D3D12.
5. **WP5** present: DXGI flip swapchain, dedicated present queue, Presenter thread, waitable object, tearing, resize / minimise / exclusive fullscreen.
6. **WP6** enhancements + RT on D3D12 (capture pool, `registerAddressRange`, RT records, TLAS slots; never ALLOW_UPDATE + ALLOW_COMPACTION on one BLAS), verified with water-compare, shadow-compare, rvk_rtwalk, perfsave, -PerfFrame.
7. **WP7** FSR + frame generation on DX12 (vendor loader / upscaler / framegeneration `_dx12` DLLs + the dx12 headers from upstream v2.4.0 `Kits/FidelityFX/signedbin`, ~87 MB, signature-checked per VERSION.md; user approved).
8. **WP8** overlay on ImGui DX12 (vcpkg imgui `dx12-binding`), UI resource for the FG swapchain.
9. **WP9** hardening: GPU timers, PIX events, DRED / crash markers, memory budget, PSO prewarm, CPU A/B (D3D12 <= Vulkan + 5 %), package.ps1 (D3D12Core.dll + licence, FFX DX12 DLLs), CLAUDE.md + hlsl-rhi.md, CI proving the
   Vulkan-only build runs without d3d12.dll, unit tests (PSO key, GpuRef, format table).
Orchestration notes: one agent at a time in the tree (shared build dir + GPU lock); verify and commit between agents; Vulkan stays default and must not regress; only the RX 7900 XTX can be validated (document NVIDIA / Intel
as untested; the backend falls back to Vulkan with a log line when SM 6.6 / binding tier 3 / required features are missing). Open question for later: ship `vulkan.api=d3d12` as experimental after WP6, or after all packages.

**Update (break, after WP2b):** WP2b is committed. Next agent = WP2c (open items listed in its Status section: Frames / MeshArena / Bindless / PipelineCache / Swapchain / Presenter / GpuTimer / FgSwapchain still members of d3d9::Device, RecordState + packet application, pipelineFor, present path, submitFrame/beginFrame, `ctx_.features()`, bootstrap `vk::probe()`; the padded-row texture upload path and D3D12 `copyBufferToTextureBox` are compiled but untested in game; `-PerfFrame` pass times were too noisy in WP2b: rerun on a quiet machine).

## D3D12 feature availability on the RX 7900 XTX (2026-11-04)

Measured with `rvk_rhitest --d3d12` (the D3D12 backend logs `caps: d3d12 <feature>: <value>` lines at device creation: `D3D12DeviceImpl::surveyFeatures`,
also in `DeviceCaps::featureSurvey`) and `rvk_vktest --rt` (the Vulkan caps lines + the new feature survey properties of `Context::appendFeatureSurvey`, which
also appear in the game's rvk.log `caps:` lines and the overlay's System tab "Graphics card" properties). Information only: nothing is enabled by it.
Driver: AMD Radeon RX 7900 XTX (RDNA 3), Agility SDK 1.619 (D3D12SDKVersion 619) active. [C] = measured.

| feature | D3D12 (CheckFeatureSupport) | Vulkan equivalent |
|---|---|---|
| runtime / SDK | Agility SDK 619 | Vulkan 1.4 (floor 1.3 + push descriptor) |
| shader model | 6.9 highest | SPIR-V via DXC |
| wave size | 32 - 64 lanes, wave ops | subgroup 64 (min 32, max 64), size control supported |
| ray tracing | **DXR tier 1.1** (no 1.2) | ray query + acceleration structure, position fetch, maintenance1, pipeline |
| opacity micromap | not in DXR 1.1; the vendored headers have no separate tier query (follows DXR 1.2) | **VK_EXT_opacity_micromap unsupported** |
| shader execution reordering | OPTIONS22 `ShaderExecutionReorderingActuallyReorders` = false (API only) | `VK_EXT_ray_tracing_invocation_reorder` listed (no NV one); whether RDNA 3 really reorders is not exposed |
| variable rate shading | tier 2 (tile 8x8) | VK_KHR_fragment_shading_rate: pipeline + primitive + attachment |
| mesh shaders | tier 1 | VK_EXT_mesh_shader: task + mesh |
| sampler feedback | tier 1.0 | (none) |
| enhanced barriers | supported (OPTIONS12) | n/a (sync2 is core) |
| relaxed format casting | supported | mutable format images |
| advanced texture ops (OPTIONS14) | supported | n/a |
| GPU upload heap | supported (OPTIONS16) | device-local + host-visible memory type |
| resizable BAR | yes (implied by the GPU upload heap; D3D12 has no direct query) | yes, 24,560 MiB device-local + host-visible heap |
| work graphs | tier 1.0 | VK_AMDX_shader_enqueue unsupported |
| ExecuteIndirect | tier 1.1 | VK_EXT_device_generated_commands / indirect count (indirect count used) |
| other Vulkan | | cooperative matrix, descriptor buffer, host image copy supported; present wait, cluster acceleration structure unsupported |

Consequences: the 7900 XTX stays at DXR 1.1, so no OMM / SER path exists on either API for this GPU; mesh shaders, VRS, work graphs, enhanced barriers and the GPU upload
heap are there for later experiments (none planned). A feature struct a runtime does not know is logged `unknown (...)`. Not in the vendored headers: a separate
opacity micromap tier / SER tier query (only `RaytracingTier >= 1.2` and OPTIONS22). Side note: `rvk_rhitest --d3d12` run from the repository root reported 3 ubershader
assembly failures (`X5752`, D3DX assembler) once; from `build\RelWithDebInfo` it passes (cwd dependent, not caused by this change).
