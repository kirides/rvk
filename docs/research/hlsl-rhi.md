# HLSL shaders, runtime DXC, and a slim RHI for Vulkan + DX12

User decision 2026-10-29 (reverses PLAN.md's "GLSL 4.60 only, no HLSL/DXC" of 2026-10-21): hand-written shaders become **HLSL**, compiled
the way GD3D11 does it (gd3d11.md 3.7), the D3D9 translator and the ubershader emit HLSL too, and a **slim RHI** lets the renderer run on
Vulkan and DX12 side by side. Scope chosen: phased, DX12 first as a *proof slice* (device, swapchain, bindless, a compute and a graphics
pass, one real enhancement), full-game DX12 stays a follow-up.

Status legend: **built** = in the tree and verified, **design** = decided, not built.

## 1. Phases

| Phase | Content | Status |
|---|---|---|
| P1a | runtime DXC service (`shader/dxc.*`), embedded HLSL sources, `embedded::spirv()` falls through to `<name>.hlsl`, disk cache, tests | **built** (e01b9f9) |
| P1b | HLSL ABI (`shaders/include/rvk_abi.hlsli`, `rvk_common.hlsli`, `rvk_draw.hlsli`), the debug-view shaders ported, layout tests against `abi.h` | **built**, GPU `--passes` PASS, in-game check see 7 |
| P1c | the other hand-written shaders (gbuffer, upscale, gi, rt, shadows, water, framegen, test) ported; GLSL stays until the A/B passes (`RVK_FORCE_GLSL=1` runs the originals); tools: `rvk_shadertool hlsl <name>`, `tools/port_check.py`, test `dxc_every_embedded_shader_compiles` | **built**, in-game A/B pending (7) |
| P1d | translator HLSL emitter (`shader/hlsl.cpp` next to `glsl.cpp`; `toHlsl` + `compileHlsl`, `kTranslatorVersion` 8): the whole corpus (2483 shaders) passes DXC + spirv-val (`rvk_shadertool corpus corpus --hlsl`), the D3D9 assembly unit tests too; the device uses it for base shaders, the LOD variant and the RT capture variant; material variants + ubershader (`ge_default.*`, back to HLSL: it comes from the engine's own HLSL) | **built** (hooks of the material variants: see 7) |
| P2 | slim RHI (section 5: dynamic rendering + descriptor indexing + constant buffers + push constants, Vulkan structs as the baseline): interface `src/rhi/rhi.h`, Vulkan backend over the existing `vk::` classes (`src/rhi/vulkan`); the migration of the renderer's own pass / resource code onto it is open (35 files call Vulkan directly) | **interface + Vulkan backend built**, migration open |
| P3 | DX12 backend slice (section 6; SM 6.6 bindless required): `src/rhi/d3d12` (library `rvk_rhi_d3d12`), the same HLSL compiled to DXIL, fixed root signature, bindless heaps; `rvk_rhitest` runs one test on both backends (compute with constant buffer + bindless texture + RVK_REF + push constants + storage image, graphics with dynamic rendering), validation layers on | **built** incl. a swapchain on a hidden window, PASS on both |
| P4 | GLSL gone: shaders/*.comp|vert|frag|glsl, `glsl.cpp`, `compileGlsl`, glslang (vcpkg), `rvk_glslc` (replaced by `rvk_embed`, a text embedder) removed 2026-10-30 after the in-game A/B of the ports (GLSL vs HLSL: no errors, within noise); `tools/shader_equiv.py` retired (its pixel check lives on as `tools/ubershader_check.py` through DXC: 183 / 183 captured PS requests EQUAL on the GPU), the vertex ubershader check is gone with it | **built** |

## 2. Compile scheme (GD3D11's, adapted) - built

- `IDxcCompiler3` from a **dxcompiler.dll loaded at run time** (`LoadLibraryEx` of an explicit path: `bin\rvk`, the module's and the
  executable's directory; never the default search order, an inbox DXC has no SPIR-V). Only the headers of the vcpkg port `directx-dxc` are
  used at build time (`find_path directx-dxc/dxcapi.h`); the build copies `dxcompiler.dll` (+ `dxil.dll`, for DXIL) and the licence next to
  every binary that compiles HLSL (`rvk_deploy_dxc`), `run.ps1` / `package.ps1` copy the DLL to `bin\rvk` (17 MB; the licence is
  `dxc_LICENSE.txt` in the zip).
- `dxc::available()` compiles a probe to SPIR-V once (GD3D11's `IsSpirvCodegenAvailable`); `unavailableReason()` says why not.
- Sources are **embedded as text** (`rvk_glslc text` over `shaders/**/*.hlsl|hlsli`, table `embedded::hlslSources()`); `RVK_SHADER_DIR`
  (`run.ps1 -Environment 'RVK_SHADER_DIR=<repo>\shaders'`) reads them from disk instead = edit, rerun, no rebuild (GD3D11's hot reload).
- Argument set (`dxc::arguments`, revision `kArgsRevision`, part of every cache key): `-E <entry> -T <profile> -HV 2021`, SPIR-V:
  `-spirv -fspv-target-env=vulkan1.3 -fspv-entrypoint-name=main -fvk-support-nonzero-base-instance` + `-fspv-extension=` ray_query,
  descriptor_indexing, physical_storage_buffer, shader_draw_parameters; `-O3`, or `-Zi -Od -fspv-debug=file|source|line` with debug info.
  Differences from GD3D11: no `-fvk-bind-resource-heap` / mutable descriptor type (typed bindless arrays instead, 4.3), no
  `-fvk-use-dx-layout` (ABI structs are float4/uint4 only, 4.1), no `-fspv-use-unknown-image-format` (storage images carry
  `[[vk::image_format]]`), entry points are renamed to `main` because every pipeline create call of the renderer says "main".
- **Disk cache** `bin\rvk\cache\hlsl\<key>.dxo`: key = args revision + compiler version + request + args + main source; the entry records the
  path and hash of every resolved `#include`, a hit re-hashes them (an edited include is a miss); written to `.tmp`, then renamed.
- Includes resolve against the embedded table (or `RVK_SHADER_DIR`): the path as DXC builds it, then with leading components dropped, then
  `include/<file name>` (the old `-I include`). Convention: shared headers live in `shaders/include`, `#include "rvk_common.hlsli"` works from
  anywhere.
- Gotchas found (confirmed): a macro parameter named like an attribute argument (`binding`) is substituted into `[[vk::binding(...)]]`;
  `-fvk-bind-register` for *one* resource makes DXC demand it for **every** resource (so the bindless heap uses `[[vk::binding]]`
  through `RVK_VK_BINDING`, everything else `register(<class><binding>, space<set>)`, which DXC maps 1:1 with shifts 0); `fnv1a("\n", h)`
  picked the (void*, size) overload (renamed `fnvBytes`).

## 3. Migration mechanics - built

`embedded::spirv("water/water.frag")` looks for `water/water.frag.hlsl` first (entry by stage extension: `.comp` `CSMain`, `.vert` `VSMain`,
`.frag` `PSMain`; profile `cs|vs|ps_6_6`), compiles it on first use (process-wide map, the returned span stays valid) and falls back to the
build-time GLSL entry. A shader is ported by adding the `.hlsl` and deleting the GLSL file + its `RVK_SHADERS` line; no C++ caller changes.
Compiles run under one mutex on the calling thread: a cold cache costs ~50-300 ms per shader once (P1c adds a background pre-warm for the
shaders of enabled features).

## 4. The HLSL ABI - built (`shaders/include/rvk_abi.hlsli`)

### 4.1 Layout
Every ABI struct is made of `float4` / `uint4` (and arrays of them) only, so std140 / std430 / scalar / ByteAddressBuffer packing agree and
`abi.h` mirrors it field by field. `RvkFrameData` = `FrameBlock` (976 B); matrices are four `float4` rows (the engine's row-major,
row-vector matrices as stored); `rvkLoadFrame()` builds the `float4x4`s (`mul(float4(world, 1), f.viewProj)` = the D3D clip position;
`f.proj[3][2]` is the same element as GLSL's `f.proj[3][2]`). Test `abi_hlsl_layout_matches_cpp` compiles `test/abi_frame_layout.comp.hlsl`
and compares `RvkFrameData` and the debug view's push block with `abi.h` / `enh::DebugViewPush` (replaces the GLSL `abi_pass_layout`).

### 4.2 References and push constants
Section 1 of the header is the only backend-specific text in the shader sources:
`RVK_REF(T)` = `vk::BufferPointer<T, 16>` (a push-constant member of 8 bytes, no shaderInt64 needed; `RVK_DEREF(ref)` = `ref.Get()`),
`RVK_PUSH(T, name)` = `[[vk::push_constant]] T name`. Pass shaders start their push block with `RVK_PASS_PUSH_HEADER` (frame reference,
passId, passFlags = the 16-byte `PassPushHeader`). Verified: member offsets 0 / 8 / 12 / 16.. in the SPIR-V. Open: the by-value
`RVK_DEREF` copy of a 976-byte block relies on dead-load elimination (looks right in the driver; measure in P1c with `--shader-stats`);
arrays that are indexed dynamically (`StageConsts.c[256]`, translated shaders) must be read through an accessor on the reference, not by value.

### 4.3 Bindless and pass resources
**Built (Vulkan, P1b)**: set 0 = the bindless heap: four unbounded typed arrays `g_t2D / g_tCube / g_t3D / g_samp` (Vulkan bindings 0..3 via
`RVK_VK_BINDING`), no `VK_EXT_mutable_descriptor_type` (gd3d11.md "Vulkan version implications"). The helpers of `rvk_common.hlsli`
(`rvkSample2D`, `rvkFetch2D`, ...) are the only users. Pass resources are still per-pass descriptors: `register(u<binding>, space<set>)` with the
C++ pass layout's numbers (`RWTexture2D<float4> o : register(u0, space1)` = set 1 binding 0).

**Decided for DX12 (section 6: SM 6.6 bindless required)**: on DX12 `rvk*` helpers read `ResourceDescriptorHeap[]` / `SamplerDescriptorHeap[]`
instead of the arrays, and pass inputs / outputs (storage images, buffers) are heap indices in the pass's push block too, not per-pass
descriptors. To keep one HLSL text, P2 moves the Vulkan pass resources onto the same model (a storage-image array `g_rwt2D[]` in the bindless
set, the `PassLayout` set 1 disappears); until then the ported pass shaders keep `register(u<n>, space1)` and are Vulkan-only.

## 5. The slim RHI - design (user, 2026-10-29)

**Surface**: exactly what the renderer uses on Vulkan 1.3 - **dynamic rendering, descriptor indexing (bindless), constant buffers, push
constants** - and nothing else. **Vulkan structs are the baseline** (`VkFormat`, `VkImageUsageFlags`, `VkPipelineStageFlags2`,
`VkImageLayout`, blend / depth / stencil enums, `VkViewport` ...): the RHI headers use them as the vocabulary, the Vulkan backend passes
them straight through, the **DX12 backend translates inside the RHI layer** (format table, state / barrier mapping, root signature from the
fixed binding model below). GD3D11 does the mirror image (a D3D12-shaped API with a Vulkan backend, gd3d11.md 3); we pick Vulkan as the
shape because the renderer already is Vulkan. No `VkRenderPass`, no `VkFramebuffer`, no descriptor-set zoo.

**Binding model (fixed, the same on both APIs)**
- **Bindless heap** (descriptor indexing): all sampled textures, storage images, samplers (and buffers read by shaders) live in one heap
  addressed by index; a shader receives indices in push constants / constant buffers. Index 0 is a 1x1 (0,0,0,1) texture.
- **Constant buffers**: per-frame / per-view / per-draw blocks are constant buffers (a UBO on Vulkan, a CBV on DX12), reached by a root
  / push binding or an index in the push block; blocks are float4 / uint4 only (4.1), <= 64 KB.
- **Push constants**: <= 128 bytes (Vulkan push constant range, DX12 root constants = 32 DWORDs).
- Nothing else is bound: no per-pass descriptor sets (those move into the heap, 4.3), no vertex-attribute pulling beyond what the draw
  model needs (vertex / index buffers bind as usual).

```
rhi::Device        create(adapter, flags) / caps() / queues / createBuffer|Texture|Sampler|Pipeline|Swapchain / timeline fence
rhi::CommandList   begin/end, barrier(span<VkImageMemoryBarrier2-like>), beginRendering(VkRenderingInfo-like) / endRendering,
                   setPipeline, dynamic state (viewport, scissor, depth/stencil, cull, blend constants), pushConstants(<=128 B),
                   bindConstantBuffer(slot, buffer, offset, size), bindVertexBuffers / IndexBuffer, draw*, drawIndirectCount, dispatch,
                   copies, timestamps, debug regions
rhi::Buffer/Texture{ create-info = Vk-style, bindlessSlot(), gpu reference for constant-buffer / heap use, map() }
rhi::BindlessHeap  allocate(view) -> index (textures, UAVs, samplers, buffers); update-after-bind; free list with retire-by-timeline
rhi::Pipeline      graphics(shader blobs, VkPipelineRenderingCreateInfo-like formats, blend, raster, depth, dynamic mask) / compute; cache blobs
rhi::Shader        SPIR-V words | DXIL bytes, produced by shader::dxc from the same HLSL
optional modules (not core): ray tracing (BLAS / TLAS / ray query), upscaler (FSR), frame generation, HDR / exclusive fullscreen
```
Rules: (1) capabilities, not backend names; (2) one HLSL source per shader (4.2: the ABI header is the only place that differs); (3) the frame
loop (frames in flight, upload ring, timeline) is part of the RHI; (4) nothing above the RHI includes `volk.h` / `d3d12.h`; (5) the existing
`vk::Context / Frames / Bindless / Transients / PassLayout / pipelines` become the Vulkan backend's internals, moved behind the interface, not rewritten.

Order of work (P2): (a) interface headers + a Vulkan backend over the existing `vk::` classes, no behaviour change, (b) migrate pass code
(`vk/passes`, `enh/*`) one feature at a time, `d3d9::Device` last, (c) build the Vulkan backend with `d3d12.h` absent to prove the layering.
35 source files call Vulkan directly today (`grep -E "vk[A-Z]\w+\("`): that list is the migration checklist.

## 6. DX12 backend - built (slice), SM 6.6 bindless required (user, 2026-10-29)

`src/rhi/d3d12/d3d12_device.cpp` (library `rvk_rhi_d3d12`, linked only by tools / the renderer when it picks the backend: d3d9.dll does not import d3d12.dll).
- Requirements checked at device creation (no fallback below them): Shader Model 6.6 (dynamic resources), Resource Binding Tier 3, feature level 12_0.
- Same HLSL compiled `-T *_6_6` to DXIL (`dxil.dll` next to `dxcompiler.dll` signs it). `rvk_abi.hlsli`'s DXIL branch: `RVK_REF(T)` = `RvkBufRef {slot, offset}` (a
  ByteAddressBuffer in the heap; `RVK_LOAD(T, ref)`), `RVK_PUSH` = `ConstantBuffer<T> : register(b0, space0)` (root constants), `RVK_TEX2D(i)` etc. read
  `ResourceDescriptorHeap` / `SamplerDescriptorHeap` through typed locals (**DXC 1.9.2602 crashes with an internal compiler error on a cast of a heap
  access followed by a method call**: `((Texture2D)ResourceDescriptorHeap[i]).Load(..)`; assigning to a local first works).
- Fixed root signature: 32 root constants (b0 space0), root CBV (b8 space1), table of 4 UAVs (u0..u3 space1), both heaps directly indexed. Heaps: one shader-visible
  CBV/SRV/UAV heap of 65,536 (entry 0 = a null SRV that reads (0, 0, 0, 1)), a sampler heap of 2,048 (entry 0 = linear clamp), RTV / DSV rings. The last 64 entries of the
  big heap are the ring of UAV tables (written per `setStorageTextures`).
- Resource states: textures track their state (`VkImageLayout` -> `D3D12_RESOURCE_STATES`), transitioning to the same GENERAL layout is a UAV barrier; device-heap buffers
  move between copy and shader states in the copy helpers / `bufferBarrier`; upload / readback heaps keep theirs.
- Swapchain: `Device::createSwapchain` / `renderAndPresent` (DXGI flip model with 3 buffers; the Vulkan side wraps `vk::Swapchain`, its images adopted as RHI textures); resize tested.
- Not built yet: ray queries (DXR inline: `RaytracingAccelerationStructure` from the heap), indirect draws, capture regions (PIX events
  need WinPixEventRuntime), buffer UAVs, mip / array views, cube / 3D textures, the renderer's own passes through the RHI (P2 migration).
- Verification: `rvk_rhitest [--vulkan] [--d3d12] [--validation]`: one test, both backends, 64x64 compare of a compute pass and a graphics pass with the CPU's expected values;
  `--validation` = Vulkan validation layer + D3D12 debug layer with GPU based validation (messages routed to the log and counted: `Device::validationErrors`).
  2026-10-30: PASS on both (RX 7900 XTX), 0 validation errors.

## 6b. Decisions after the first runs

- **No tiered compilation** (user, 2026-10-30): DXC costs ~40-60 ms per translated shader (the corpus of 2483 takes ~2.5 min at -O1; -O0 ~1.5 min; -O1 and -O3 cost the
  same), paid once into the disk caches (`bin\rvk\cache\spirv` for translated shaders, `bin\rvk\cache\hlsl` for the embedded ones). A background re-optimize would need a
  pipeline rebuild per module swap and buys nothing the driver's own compiler does not do; translated shaders stay at `-O1`.

## 7. Verification log

- 2026-10-29: `rvk_tests` 197 / 197 (dxc_* tests: embedded sources, valid SPIR-V, error reporting, cache invalidation through an include,
  debug info; `abi_hlsl_layout_matches_cpp`), `rvk_vktest --passes` PASS (0 bad values, 0 validation errors, sync validation on) with
  `debug/fullscreen.vert`, `inject_view.comp` / `.frag` compiled from HLSL by the running DXC.
- 2026-10-30: the hand-written ports in the game (GLSL originals vs HLSL, `RVK_FORCE_GLSL=1`, 13 validation runs incl. sync validation and the debug views, then 10 spots x
  3 runs with the full feature set: water + FFT ocean + RT reflections, RT sun shadows, GTAO, GI + screen probes, FSR quality): no errors from our code, no VUID / SYNC
  messages, differences within the GLSL-vs-GLSL noise (the camera jitter phase explains the one spot pair that looked worse). Translator: 2483 / 2483 corpus shaders and
  every variant corpus (`--gbuf` 2321, `--gbuf-motion` 2321, `--gi` 2246, `--capture` 149, `--tint` 2483) through DXC + spirv-val; the D3D9 assembly unit tests through the
  HLSL path; `rvk_vktest` (translated shaders on the GPU) and `--passes` / `--rt` PASS. Ubershader: 778 captured requests compile (with and without hooks); on the GPU
  `tools/ubershader_check.py` = 183 / 183 EQUAL against the translated game shaders. RHI: `rvk_rhitest --validation` PASS on Vulkan and D3D12 (0 validation errors).
  Unit tests 198 / 198 after the GLSL removal.
