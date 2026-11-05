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
| P2 | slim RHI (section 5: dynamic rendering + descriptor indexing + constant buffers + push constants, Vulkan structs as the baseline): interface `src/rhi/rhi.h`, Vulkan backend over the existing `vk::` classes (`src/rhi/vulkan`); the migration of the renderer's own pass code onto it is done for every compute / fullscreen pass of the enhancements (section 8: 2026-10-30 second round); what still calls Vulkan directly: the D3D9 device model (`src/d3d9`, `vk/pipeline*`) and what it owns; the draw replacers have no Vulkan of their own (section 8, third round) | **interface + Vulkan backend built**, migration started |
| P3 | DX12 backend slice (section 6; SM 6.6 bindless required): `src/rhi/d3d12` (library `rvk_rhi_d3d12`), the same HLSL compiled to DXIL, fixed root signature, bindless heaps; `rvk_rhitest` runs one test on both backends (compute with constant buffer + bindless texture + RVK_REF + push constants + storage image, graphics with dynamic rendering), validation layers on | **built** incl. a swapchain on a hidden window, PASS on both |
| S1 | DXIL parity: every shader compiles to signed DXIL from the same HLSL and fits the fixed root signature; `rvk_shadertool dxil-check`; GPU equivalence tests (section 9) | **built** |
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
- Same HLSL compiled `-T *_6_6` to DXIL (`dxil.dll` next to `dxcompiler.dll` signs it). `rvk_abi.hlsli`'s DXIL branch (reworked in section 9): `RVK_REF(T)` = `RvkRef<T> {slot, offset}` (a
  ByteAddressBuffer in the heap; `.Get()` / `RVK_LOAD(T, ref)`), `RVK_PUSH` = `ConstantBuffer<T> : register(b0, space0)` (root constants), `RVK_TEX2D(i)` etc. read
  `ResourceDescriptorHeap` / `SamplerDescriptorHeap` through typed locals (**DXC 1.9.2602 crashes with an internal compiler error on a cast of a heap
  access followed by a method call**: `((Texture2D)ResourceDescriptorHeap[i]).Load(..)`; assigning to a local first works).
- Fixed root signature: 32 root constants (b0 space0), root CBV (b8 space1), table of 8 UAVs (u0..u7 space1, `rhi::kStorageImages`; was 4), both heaps directly indexed. Heaps: one shader-visible
  CBV/SRV/UAV heap of 65,536 (entry 0 = a null SRV that reads (0, 0, 0, 1)), a sampler heap of 2,048 (entry 0 = linear clamp), RTV / DSV rings, a one-entry CPU-only UAV heap (the source of the clears). The last 256 entries of the
  big heap are the ring of UAV tables (32 tables of 8, written per `setStorageViews` / `clearTexture`).
- Resource states: textures track their state (`VkImageLayout` -> `D3D12_RESOURCE_STATES`), transitioning to the same GENERAL layout is a UAV barrier; device-heap buffers
  move between copy and shader states in the copy helpers / `bufferBarrier`; upload / readback heaps keep theirs.
- Swapchain: `Device::createSwapchain` / `renderAndPresent` (DXGI flip model with 3 buffers; the Vulkan side wraps `vk::Swapchain`, its images adopted as RHI textures); resize tested.
- Gaps closed 2026-10-30 (both backends, one HLSL source and one test each in `rvk_rhitest`):
  - **Textures**: `TextureDesc::cube` (6 layers = faces), 3D textures (`VK_IMAGE_TYPE_3D`), mip chains, array layers. `heapSampled()` is the whole-resource view of the kind (Texture2D / TextureCube / Texture3D;
    Vulkan `vk::Bindless` kinds Cube / Volume, DX12 TEXTURECUBE / TEXTURE3D, mip count = all levels); a 2D array has no whole-array kind in the bindless set, so `heapSampled()` = layer 0 and
    `heapSampledLayer(n)` gives a Texture2D view per layer (DX12: a TEXTURE2DARRAY view of one slice read as `Texture2D`: accepted by the debug layer + GPU based validation). `Subresource{mip, layer}` on
    `copyBufferToTexture / copyTextureToBuffer` (`mipExtent`, `copyBytes`; DX12 needs buffer offsets of texture copies to be multiples of 512), `StorageView{texture, mip, layer}` for
    `setStorageViews` (3D view for volumes = `RWTexture3D`, a one-slice array view when the texture has layers = `RWTexture2DArray` with z = 0, else 2D), `RenderingInfo::Attachment::mip / layer`.
  - **Buffer UAVs**: `Buffer::heapIndexRw()` (a raw `RWByteAddressBuffer` UAV in the heap; device-memory STORAGE buffers), `Buffer::shaderRefRw()` (Vulkan: the device address, DX12: {UAV slot, offset}),
    `bufferBarrier(buffer, BufferAccess::ShaderRead | ShaderWrite)` (DX12: transition to ALL_SHADER_RESOURCE / UNORDERED_ACCESS, a UAV -> UAV step is a UAV barrier; Vulkan: the global barrier as before).
    Shader side (`rvk_abi.hlsli` section 5): `RVK_BUF_LOAD / RVK_BUF_STORE / RVK_BUF_ADD(ref, byte offset, ...)` over the `uint2` of `shaderRefRw` (Vulkan `vk::RawBufferLoad / Store` + `InterlockedAdd`
    on a `BufferPointer`, DXIL `RWByteAddressBuffer` from the heap through a typed local). Atomics checked with 1000 threads on one address.
  - **Vertex / index / indirect**: `GraphicsPipelineDesc::vertexBindings / vertexAttributes / topology` (Vulkan structs; location N = semantic `ATTR<N>` on DX12; Vulkan strides are dynamic state
    `VERTEX_INPUT_BINDING_STRIDE`, supplied by `setVertexBuffers`), `setVertexBuffers`, `setIndexBuffer`, `drawIndexed`, `drawIndexedIndirectCount` (Vulkan `vkCmdDrawIndexedIndirectCount`, DX12
    `ExecuteIndirect` with a DRAW_INDEXED signature per stride; `D3D12_DRAW_INDEXED_ARGUMENTS` = `VkDrawIndexedIndirectCommand`, 20 bytes). `DeviceCaps::drawIndirectCount`. DX12 moves device-memory
    buffers into VERTEX_AND_CONSTANT_BUFFER / INDEX_BUFFER / INDIRECT_ARGUMENT in those calls.
  - **Ray queries**: `DeviceDesc::rayTracing`, `DeviceCaps::rayQuery` (Vulkan: ray query + acceleration structures + the bindless TLAS array; DX12: DXR tier 1.1), `Device::createBlas(span<TriangleGeometry>)`
    / `createTlas(maxInstances)`, `CommandList::buildBlas / buildTlas(instanceBuffer, offset, count)`, `AccelerationStructure::address() / tlasSlot()`. Instance records are
    `VkAccelerationStructureInstanceKHR` = `D3D12_RAYTRACING_INSTANCE_DESC` (64 bytes; the BLAS reference is `address()`). Scratch is owned by the object (kept for rebuilds). DX12: storage in
    RAYTRACING_ACCELERATION_STRUCTURE state for good, scratch promoted from COMMON, a UAV barrier after each build, the TLAS as a RAYTRACING_ACCELERATION_STRUCTURE SRV in the heap. Vulkan TLAS slots
    are 1..7 of `g_tlas[]`. HLSL: `RVK_DECLARE_TLAS` once at file scope + `RVK_TLAS(slot)` as the argument of `TraceRayInline` (a macro: DXIL needs the cast of the heap access inside the call,
    SPIR-V cannot return an acceleration structure from a function); `rvk_rt.hlsli` uses them too.
- Still not built: capture regions on DX12 (PIX events need WinPixEventRuntime; `beginRegion / endRegion` are no-ops on both backends), device timestamps, `drawIndirect` without a count and
  compute-generated argument buffers beyond the copy path (the ExecuteIndirect plumbing is the same), the renderer's own passes through the RHI (P2 migration, started: section 8), host-cache flush / invalidate of
  non-coherent upload / readback memory on Vulkan (coherent on the tested GPU).
- Verification: `rvk_rhitest [--vulkan] [--d3d12] [--validation]`: one test, both backends, 64x64 compare of a compute pass and a graphics pass with the CPU's expected values;
  `--validation` = Vulkan validation layer + D3D12 debug layer with GPU based validation (messages routed to the log and counted: `Device::validationErrors`).
  2026-10-30: PASS on both (RX 7900 XTX), 0 validation errors. The gap checks (textures, buffer UAVs, vertex / index / indirect draws, ray query) print their own PASS / SKIP line per backend.

### 6a. Additions of the pass migration (2026-10-30, both backends, `rvk_rhitest` check "texture ops")

- **Storage images u0..u7** (`rhi::kStorageImages` = 8): Vulkan push descriptor set 1 bindings 0..7, DX12 a table of 8 UAVs. One dispatch of `test/rhi_store8.comp.hlsl` writes all eight.
- **Per-mip layouts**: `CommandList::transitionMips(texture, layout, baseMip, mipCount)` (all layers; layers are tracked as one). `vk::Image` keeps one layout for the image or, after a `transitionMips`, one per
  mip level (`Image::transitionMips`, `mipLayout(m)`, `layout()` = `VK_IMAGE_LAYOUT_MAX_ENUM` while split; any whole-image `transition` / `setTrackedLayout` / `discard` brings the levels together again), so wrapped textures keep
  the game's bookkeeping as the one source of truth. DX12: `D3DTexture::mipStates` (same rule, one barrier per level and layer).
- **Clears / copies / blits**: `clearTexture(texture, VkClearColorValue, baseMip, mipCount)` (STORAGE colour textures in GENERAL; Vulkan `vkCmdClearColorImage`, DX12 `ClearUnorderedAccessView{Float,Uint}` through a view
  in the ring + the CPU-only heap), `copyTexture(dst, src, span<TextureCopy>)` (`TextureCopy{src, dst, layerCount, srcOffset, dstOffset, extent}`, extent 0 = the whole mip; `vkCmdCopyImage2` / `CopyTextureRegion` with a box),
  `blitTexture(dst, src, span<TextureBlit>, filter)` (`TextureBlit{src, dst, srcExtent, dstExtent}`; Vulkan `vkCmdBlitImage2`, mip chains blit between the levels of one texture). **DX12 has no scaling copy**:
  `DeviceCaps::textureBlit` is false there, `blitTexture` logs and returns false, `rvk_rhitest` prints SKIP for it.
- `CommandList::computeBarrier()`: the global barrier between dispatches (Vulkan: compute / fragment / transfer storage + transfer writes -> compute / fragment / vertex / transfer reads and writes; DX12: a UAV barrier on all
  resources). It replaces the passes' `VkMemoryBarrier2` compute -> compute barriers and does not change layouts.
- `SamplerDesc::maxAnisotropy` (Vulkan `samplerAnisotropy`, DX12 `D3D12_FILTER_ANISOTROPIC`), `Buffer::invalidate()` / `flush()` (readback / upload memory on Vulkan: `vmaInvalidateAllocation` / `vmaFlushAllocation`, no-ops on DX12).
- **Acceleration structures on the wrapped device**: `DeviceCaps::rayQuery` is on for `rhi::vulkan::wrap` when the renderer's RT tier is. The TLAS slots are the renderer's: `vk::Bindless::allocTlasSlot()` / `freeTlasSlot()` (a mask,
  thread-safe; slot 0 is valid) serve both `enh::RtScene` (its 3 per-frame TLASes now allocate theirs, `Tlas::slot` is what `Device::setRtScene` publishes) and the RHI's `createTlas`. Destruction of a wrapped acceleration structure is
  deferred through `Frames::retire` (it also frees the slot). Checked by `rvk_vktest --passes --rhi` (BLAS + TLAS built into the frame's command buffer, the TLAS slot differs from one the "renderer" holds, sync validation clean).
- Threading: `createComputePipeline` / `createGraphicsPipeline` of the wrapped device only read the pass layout and call `vkCreate*Pipelines` without a cache, so the passes that build pipelines on a worker (`std::async`) keep doing so
  through the RHI (gbuffer, gi, water.rt, shadows.rt, rt skin); the future is joined in the destructor, the pipeline object is retired with the device's frames.
- Pass-side helper `enh/rhi_scope.h` (`RhiScope`): the frame's command buffer as an `rhi::CommandList` + `wrapImage` over transients / engine textures, released at the end of the pass call.

### 6c. Acceleration structure batches (third round, 2026-10-30, both backends, `rvk_rhitest` check "acceleration structure ops")

What the RT scene (`enh/rt_scene.cpp`) needed, built generally:
- `Buffer::gpuAddress()` (Vulkan device address, D3D12 GPU virtual address; not a shader reference), `TriangleGeometry::vertexAddress / indexAddress` (raw addresses for the renderer's own memory: mesh arena, frame ring, shader-written
  captures; DX12: the memory must be readable by the build, builds cannot transition what they know only by address), `BlasDesc{geometries, allowUpdate, allowCompaction, preferFastBuild, name}` (the geometries only fix the sizes),
  `TlasDesc{maxInstances, preferFastBuild, name}`, `AccelerationStructure::storageBytes()`.
- `CommandList::buildBlases(span<BlasBuild{blas, geometries, update}>)`: a batch of builds or refits in one call (one `vkCmdBuildAccelerationStructuresKHR`; DX12 one `BuildRaytracingAccelerationStructure` each). Scratch comes
  from a **device-wide pool** (grown on demand: `Device::buildScratchBytes()`; wrapped device 32 MiB minimum; a replaced pool lives until the frame completes / the device dies); a batch larger than the pool is cut into runs with a barrier
  between them; a barrier precedes every batch (earlier builds reuse the scratch, ray queries still read) and follows it. BLASes no longer own scratch (TLAS keep theirs: they are rebuilt every frame, three of them).
  `buildBlas(blas)` is a batch of one. `buildTlasAt(tlas, rawAddress, count)` (the instance records in the frame ring) next to `buildTlas(tlas, Buffer, offset, count)`.
- Compaction: `Device::createSizeQuery(capacity)` (`SizeQuery::read(first, count, sizes*)`, 0 = not available), `CommandList::queryCompactedSizes(query, first, span<AS*>)` (Vulkan: a query pool of
  `ACCELERATION_STRUCTURE_COMPACTED_SIZE`; DX12: `EmitRaytracingAccelerationStructurePostbuildInfo` into a UAV buffer copied to a readback buffer, `read` valid after `submitAndWait`), `Device::createBlasStorage(bytes)` +
  `CommandList::compactBlas(dst, src)` (`VK_COPY_ACCELERATION_STRUCTURE_MODE_COMPACT_KHR` / `D3D12_..._COPY_MODE_COMPACT`).
  **Finding (AMD RX 7900 XTX, Vulkan and D3D12 alike): a BLAS built with both ALLOW_UPDATE and ALLOW_COMPACTION did not compact into a traceable structure (every ray missed, no validation message; the same BLAS traced fine
  uncompacted).** The RT scene never combines them (static BLASes: compaction only, characters / trees: update only); `rvk_rhitest` uses two BLASes for the two operations.
- `CommandList::memoryBarrier(srcStages, srcAccess, dstStages, dstAccess)` (one `VkMemoryBarrier2`; DX12 a UAV barrier): shader writes into raw-address memory that builds read; `fillBuffer(buffer, offset, size, value)` (Vulkan
  `vkCmdFillBuffer`, DX12 `ClearUnorderedAccessViewUint` through an R32_UINT view; later use wants a `bufferBarrier`); `rhi::vulkan::wrapBuffer(device, VkBuffer, address, size, mapped)` = a non-owning buffer over the frame ring
  (copy source). Vulkan RHI buffers start on 256 bytes when the RT tier is on (instance records need 16: a 128-byte VMA sub-allocation had a 4-byte aligned address and tripped VUID-03715 in the test).
- GPU timestamps were **not** added: the RT scene's own two-timestamp query only fed a debug number in its 5 s log line; `-PerfFrame` already lists `pass rt.scene` (the log now says so).
- Verification: `rvk_rhitest --validation`: Vulkan and D3D12 PASS, 0 validation errors (batch of three BLASes with raw addresses and per-build geometry, pooled scratch, compacted sizes read back, compaction, refit moving the
  geometry 5 units, TLAS from a raw address, buffer fills).

## 6d. Agility SDK (2026-10-30; GD3D11's scheme, `rhi/d3d12/d3d12_device.cpp` `agility()`)

The retail OS runtime of d3d12.dll is older than SM 6.6 root signature flags / the newer DXR and debug features; an exe picks the Agility SDK by exporting `D3D12SDKVersion` /
`D3D12SDKPath`, which a plugin DLL (d3d9.dll in the game, the tools' exe here) cannot do. The way around, once per process before the first device:
`D3D12GetInterface(CLSID_D3D12SDKConfiguration)` -> `ID3D12SDKConfiguration1::CreateDeviceFactory(D3D12SDKVersion of D3D12Core.dll, dir)` -> `ID3D12DeviceFactory::ApplyToGlobalState()`
(without it only factory-made devices use the SDK; the exported free functions, `D3D12SerializeRootSignature` for one, stay on the system runtime and reject the
`CBV_SRV_UAV_HEAP_DIRECTLY_INDEXED` flag on an OS runtime that predates SM 6.6). Devices are created with `factory->CreateDevice` (falls back to `D3D12CreateDevice`); the debug layer, GPU based
validation and DRED (`DeviceDesc::validation` only) are enabled through `factory->GetConfigurationInterface` (a factory-made device ignores the global `D3D12GetDebugInterface` state).

- **Layout**: `<dir>\D3D12Core.dll` (+ `d3d12SDKLayers.dll` for `validation` / `messages`). `<dir>` = `DeviceDesc::d3d12SdkDir`, else the environment variable `RVK_D3D12_SDK_DIR`, else `d3d12\` next
  to the module the backend is linked into (`build\<config>\d3d12\` for the tools); when the module has no `d3d12\` but `rvk\d3d12\D3D12Core.dll` exists beside it, that one (the game: d3d9.dll is in `bin`,
  the player zip and `scripts/run.ps1` put the runtime in `bin\rvk\d3d12\`). The version is read from the DLL's `D3D12SDKVersion` export. Missing DLL,
  an OS without `ID3D12SDKConfiguration1` or a refused factory = the system runtime (logged, no failure). Absolute paths only (a relative one is relative to the exe for the factory but not for the version probe).
  How it works for a proxy DLL (verified 2026-11-05, rvk_rhitest, AMD RX 7900 XTX, Windows 11 26300): the exe-export route (`D3D12SDKVersion` / `D3D12SDKPath` of Risen.exe) is not available to a DLL; the factory route above is the
  supported one (`SetSDKVersion`-style early call = `CreateDeviceFactory` + `ApplyToGlobalState`) and the log shows `d3d12: device from the Agility SDK 620 factory`.
- **Build**: vcpkg `directx12-agility`. The registry baseline has 1.619.5 only; **`vcpkg-overlay/directx12-agility`** (vcpkg-configuration.json `overlay-ports`) is the same port at the retail **1.620.1**
  (NuGet `Microsoft.Direct3D.D3D12` 1.620.1, SHA512 in its portfile; D3D12_SDK_VERSION 620 = the directx-headers 1.721.2 headers); drop the overlay when the registry catches up. The backend includes `<directx/d3d12.h>` (the Windows SDK 10.0.26100 headers
  only know SDK 616 and lack some of the newer interfaces) and links `Microsoft::DirectX-Guids`. CMake `rvk_deploy_agility(<target>)` copies D3D12Core.dll + d3d12SDKLayers.dll + the licence (`copyright`) to
  `$<TARGET_FILE_DIR>/d3d12` (rvk_rhitest, d3d9); `scripts/run.ps1` copies that folder to the test copy's `bin\rvk\d3d12`. **`scripts/package.ps1` ships `rvk\d3d12\D3D12Core.dll` + `D3D12_LICENSE.txt`**
  (Microsoft DirectX licence: the distributables of the package may be redistributed with the application, the layers `d3d12SDKLayers.dll` are a developer tool and stay out of the zip; ~4.9 MB). Without it
  (or on an OS that refuses it) the backend runs on the inbox runtime.
- **Availability** (checked 2026-11-05): NuGet `Microsoft.Direct3D.D3D12` has retail 1.619.x (.0 - .6) and **1.620.1** and previews up to 1.722.0-preview; the vcpkg registry port is 1.619.5. WinPixEventRuntime 1.0.240308001 is in the registry
  (`winpixevent`; headers used, the DLL is not needed: see PIX below).
- **Log / caps**: `d3d12: Agility SDK 620 from <dir>` + `d3d12: device from the Agility SDK 620 factory`, or `d3d12: system runtime (...)` / `device from the system runtime`;
  `DeviceCaps::agilitySdkVersion` (0 = system runtime), printed by `rvk_rhitest` (`[D3D12] adapter '...', Agility SDK 620`); `d3d12: caps: DXR 1.1 (...), SER reorders no, work graphs tier 1, ExecuteIndirect tier 1,
  enhanced barriers yes` (`DeviceCaps::dxrTier` 10 / 11 / 12, `serReorders`).
- **Used (2026-11-05, 1.620.1)**: the debug layer's `ID3D12Device::CheckFeatureSupport` warning (message 989, "use ID3D12Device16::CheckFeatureSupport1"; the 1.721.2 headers have no Device16) is denied through the info queue
  (the capability queries run once at device creation). The `ID3D12Debug7::SetEnableDriverReportedMessages` call the earlier text announced does not exist in the 1.620 / 1.721.2 headers (no `ID3D12Debug7`): removed;
  driver messages arrive through the info queue we already route when the driver reports them.
- **Debugging, built 2026-11-05** (`rvk_rhitest --device-lost` is the test):
  * DRED auto-breadcrumbs + breadcrumb contexts + page fault data are on by default for every D3D12 device (`DeviceDesc::dred`, default true; `RVK_DRED=0` / config `[debug] d3d12_dred = false` off), not only with validation.
  * Device removal: after each wait / present `GetDeviceRemovedReason` is checked (`Device::deviceLost()`); the first failure logs the reason + DRED report (unfinished command lists, the op the GPU stopped at with its neighbours,
    breadcrumb contexts, the page fault VA with live / recently freed allocations; `d3d12: DRED: ...` error lines) and writes `rvk_<stamp>_d3d12_device_removed.dmp` + `.txt` into `bin\rvk\dumps\` (`core/crash_dump.*`,
    `MiniDumpWriteDump`, at most 4 files per run). The fence wait is bounded (10 s -> reported as DXGI_ERROR_DEVICE_HUNG instead of hanging). Test hook `Device::simulateDeviceLoss()` = `ID3D12Device5::RemoveDevice`
    (`rvk_rhitest --device-lost`: PASS; a removal this way has no unfinished breadcrumbs, a real fault shows them).
  * Vulkan: `Context::reportDeviceLost` (VK_EXT_device_fault report, `devicefault.bin`) now also writes `rvk_<stamp>_vulkan_device_lost.dmp` + the report into `bin\rvk\dumps\`. `[debug] crash_dumps` (default true) installs an
    unhandled-exception filter writing `..._crash.dmp` (chains to the previous filter; the engine's own handler, installed later, replaces it).
  * Debug layer messages: `DeviceDesc::messages` / `RVK_D3D12_MESSAGES=1` / config `[debug] d3d12_messages` turn the layer on without GPU based validation and route its messages into rvk.log (`d3d12: validation <id>: ...`),
    validation keeps GBV. The game creates no D3D12 device yet: the config keys are read, the device creation site (WP4) passes them.
  * PIX / capture regions: `CommandList::beginRegion / endRegion` write PIX3-encoded events with the header-only encoder of the `winpixevent` port (`PixEventsLegacy::EncodeBeginEventForContext` + `list->BeginEvent(D3D12_EVENT_METADATA, ...)`):
    `PIXBeginEvent(list, ...)` itself also records a CPU timeline event that needs `WinPixEventRuntime.dll` (`PIXGetThreadInfo` / `PIXEventsReplaceBlock` do not link without it), so the DLL stays out. The debug layer accepts these events
    (rvk_rhitest --validation: 0 errors) and the DRED breadcrumbs list them as BeginEvent / EndEvent ops.
- **Usable later** (none built; the Vulkan side has no counterparts of the batched calls, so they stay DX12 only optimizations): 1.620: *batched async commands*
  (`CopyBufferRegions`, `ClearTextureSubresources`, `FillBuffers`, bound RTV / DSV clears: fewer calls for the RHI's `copyBuffer` / `clear*` / `fillBuffer` on AMD RX 7000+, RTX, Intel),
  *extended PIX markers*, *DirectX dump files*, `CheckFeatureSupport1`, castable format query (typeless depth / sRGB views). 1.722 preview: *DXR clustered geometry*, *partitioned TLAS* and *indirect
  acceleration structure builds* (the RT scene's per-frame TLAS rebuild and BLAS batches: GPU-driven instance lists; AMD RX 9000, Intel B-series, RTX only), *GPU timeline CopyDescriptors* (Resource
  Binding Tier 4, AMD RX 7000+). The preview runtime needs Windows developer mode and a driver that supports it; use retail for anything that ships.

## 6b. Decisions after the first runs

- **No tiered compilation** (user, 2026-10-30): DXC costs ~40-60 ms per translated shader (the corpus of 2483 takes ~2.5 min at -O1; -O0 ~1.5 min; -O1 and -O3 cost the
  same), paid once into the disk caches (`bin\rvk\cache\spirv` for translated shaders, `bin\rvk\cache\hlsl` for the embedded ones). A background re-optimize would need a
  pipeline rebuild per module swap and buys nothing the driver's own compiler does not do; translated shaders stay at `-O1`.
- **Translated shaders compile on a worker pool, draws skip while theirs compiles** (2026-10-30). Measured: the synchronous compile on the engine thread made the cold
  start 169 s to Lua (GLSL: 35 s) and tripped the 90 s stall watchdog of the test harness. `Device::createShader` now returns at once (translation, reflection, semantics on the
  calling thread), `enqueueShaderJob` hands the DXC compile + module creation to hardware threads - 2 (2..12); with the pool the cold start is 19 s to Lua, 84 s to the first
  world screenshot (GLSL 91 s). A draw whose vertex / pixel shader has no module yet is **skipped** (`Stats::skippedCompiling`, the `compiling` field of the `hal: frame` line)
  and its shader is moved to the front of the queue (`Device::prioritizeShader`, once per shader): the start menu's UI shaders compile first, the world's while the save
  loads, the rest in the background. `Shader::wait()` (blocking) exists for tools but no draw path uses it. Cost on a cold first run: transient holes in the first world frames
  on a CPU with few cores.

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
- 2026-10-30: DX12 gaps closed (section 6): `rvk_rhitest --validation` PASS on Vulkan and D3D12 with textures (cube, 3D, mips, array layers, per-mip / per-layer storage and render target views),
  buffer UAVs + atomics, vertex / index buffers with an indirect-count draw, and DXR 1.1 / Vulkan ray queries on the selfTest scene; 0 validation errors (Vulkan layer; D3D12 debug layer + GPU based
  validation, only the "clear values do not match" warning of ClearRenderTargetView). `rvk_tests` 198 / 198, `rvk_vktest --passes` and `--rt` PASS.
- 2026-10-30: Agility SDK (section 6d): `rvk_rhitest --validation` PASS on Vulkan and D3D12 with `d3d12\` deployed (`d3d12: Agility SDK 619 ..., device from the Agility SDK 619 factory`, debug layer through the
  factory) and with the folder absent (`d3d12: system runtime ...`); `RVK_D3D12_SDK_DIR` override checked; `rvk_tests` 198 / 198, d3d9.dll builds.

## 8. Pass migration (P2) - started 2026-10-30

**Decision.** The renderer's passes run inside the game's frame loop on the game's own `vk::Context / Frames / Bindless`, recording into the frame's `VkCommandBuffer`; a
second `VkDevice` (what `rhi::createVulkanDevice` makes) cannot share images, bindless slots or the queue. So the Vulkan backend got a second constructor and an adapter,
`src/rhi/vulkan/wrap.h` (implemented at the end of `vulkan_device.cpp`, next to the classes it needs; `rhi.h` is untouched and backend neutral):
- `rhi::vulkan::wrap(ctx, frames, bindless)` -> an `rhi::Device` over those objects (`VkDeviceImpl` holds references; the owning mode binds them to its own members). It creates only the
  fixed binding model's pass layout (set 0 = the game's bindless set, set 1 = push descriptors u0..u7 + b8, 128 B push constants). `d3d9::Device::rhi()` creates it on first use and
  resets it before `frames_`. Nothing in a wrapped device waits for the GPU: pipelines, buffers, textures and the device's samplers / layout are destroyed through `Frames::retire`
  (`VkDeviceImpl::defer`; an owning device keeps `waitIdle` + destroy). No swapchain / `submitAndWait` (the renderer owns submission), acceleration structures since the second round (6a: TLAS slots shared with `vk::Bindless`).
- `rhi::vulkan::wrapImage(dev, vk::Image&, sampledSlot, name)` -> a non-owning `rhi::Texture` (VkTextureImpl with `image_` pointing at the external `vk::Image`; the bindless slot is the
  caller's and is not released). **The layout bookkeeping is `vk::Image`'s own**: `CommandList::transition` on the wrapper calls `vk::Image::transition(.., force)`, so the renderer's barriers
  (`Device::readable`, the engine draws) and the pass's agree: one source of truth. Render attachments use the image's own `attachmentView` (identity swizzle, cached); storage views
  reuse `image.view()` when its swizzle is identity.
- `rhi::vulkan::wrapCommandBuffer(dev, cmd)` -> a non-owning `rhi::CommandList` on the frame's command buffer (no begin / end / submit). One per pass call: it carries only the bound pipeline
  and pending bindings (pipeline bind = bindless set 0; push descriptors are written at the next draw / dispatch). The injection machinery already leaves no D3D9 state to restore.
- Convention difference to remember: the RHI viewport is D3D style (y down; the Vulkan backend flips it with a negative height, `beginRendering` sets full-target viewport + scissor),
  so a fullscreen pass takes `shaders/debug/fullscreen_rhi.vert.hlsl` (clip y = 1 - 2 uv.y; uv 0,0 = top left) instead of `fullscreen.vert` (written for the unflipped raw-Vulkan viewport).
  The raw-Vulkan passes keep `fullscreen.vert` until they move.

**Migrated** (pipelines = `rhi::Pipeline`, recording through `rhi::CommandList`, textures through `wrapImage`; behaviour unchanged):
1. `enh/debug_view.*` (`debug.inject_view`): compute + the fullscreen blend composite (a blend state, `beginRendering` with LOAD).
2. `enh/upscale.*`: `upscale/prep.comp` (hardware depth -> R32F) and `upscale/exposure.comp` (1x1 exposure image). Left raw there: the exposure read-back (`vkCmdCopyImageToBuffer` into a
   host buffer for a log line), the `upscale.opaque` `vkCmdCopyImage` (no RHI image copy), FSR's own dispatches (`vk::Fsr`, AMD's DLL records into the command buffer).
3. `enh/framegen.*`: `framegen.depth` (`upscale/prep.comp` again) and `framegen.debug` (fullscreen draw into the back buffer, LOAD_OP_DONT_CARE). Left raw: `framegen.hudless`
   (`vkCmdCopyImage`, the copy helper at the top of the file), `vk::FrameGen` / the AMD swapchain.

**Verification (2026-10-30, RX 7900 XTX).** `rvk_tests` 198 / 198; `rvk_rhitest --validation` PASS on Vulkan and D3D12 (0 errors); `rvk_vktest --passes` PASS and the new
`--passes --rhi` (the same debug-view passes created and recorded through the wrapper over the test's own Context / Frames / Bindless, pixels checked, sync validation on) PASS with 0 errors,
`--passes --rhi --hazard` still reports the deliberate hazard (ctest `rvk_vktest_passes_rhi`); `--rt` PASS. In the game (windowed, copy `risen-test-vk`, `-SyncValidation`):
`debug.inject_view=depth` on `testdemo` (reference screenshot of the committed build taken first, `out\rhi-migrate\ref_*.png`): same picture (static regions differ by 1.6 - 6.5 grey levels of 255,
water / rain animate; orientation of the distance bands identical), no validation message; `upscale.mode=native_aa` on `rvkcompare lake_jungle`: FSR 3.1.6 ran 575 frames, `exposure engine 0.425
(adapted luminance 0.245)` = (1/9.6) / avgLum as before, clean picture, only AMD's own `rw_*` format warnings; `framegen.mode=fsr3` + `framegen.debug=depth`: the depth view is right side up and correct,
only AMD's accepted `rw_output` OpImageWrite messages, none of ours.

**Verification of the second round (2026-10-30, RX 7900 XTX).** `rvk_rhitest --validation` PASS on Vulkan and D3D12 (0 validation errors; "texture ops" = eight storage images, clears, per-mip transitions, batched
transitions, box copy; the blit check is SKIP on D3D12), `rvk_tests` 198 / 198, `rvk_vktest --passes`, `--passes --rhi` (now with a BLAS + TLAS on the wrapped device), `--rt`, `--skin` PASS, `--passes --rhi --hazard` still
reports the hazard. In the game (windowed, `-SyncValidation`, `water-compare.ps1 -SkipEngine`, settings water.ocean=fft, rt.reflections=medium, shadows.enabled + shadows.rt=near, ao, gi; spots sea_cliff / beach_sun /
lake_jungle / pub_interior; reference pictures of the committed build 957dc08 in `out/rhi-migrate/ref_all`): same pictures (mean abs difference 1.6 - 3.8 of 255, the sea's breakers / splashes / butterflies differ between
any two runs; two runs of the new build differ by 0.6 - 4.8), every feature's first-frame log line present (`enh: ocean`, `gbuffer: first AO`, `gi: first update`, `gi: screen probes`, `shadows.rt: first frame`, `water.rt: first`,
`water: wet shore pass`), no VUID / SYNC message. Upscaling + frame generation + characters (`upscale.mode=native_aa`, `framegen.mode=fsr3`, `rvkcompare hero_idle`): FSR 3.1.6 ran 3,324 frames with
`exposure engine 0.317 (adapted luminance 0.329)`, the AMD frame generation swapchain produced 240 generated of 481 presents per 5 s, `rt: skin` skinned 55,933 draws by compute; only AMD's accepted `rw_*` format messages.
GPU time per pass (`-PerfFrame 4200`, 24 frames averaged, 1920x1080, committed build vs this one; `out/rhi-migrate/perf`): sea_cliff water.ocean 0.312 -> 0.323 ms (a first version with one barrier per image measured 0.409:
per-image barriers cost ~0.1 ms, hence `CommandList::transitions` / `computeBarrier`), gi 1.079 -> 1.065, gi.screen 0.727 -> 0.730, gbuffer.ao 0.465 -> 0.468, water.rt 0.291 -> 0.297; lake_jungle water.ocean 0.123 -> 0.127,
shadows.rt trace 0.959 -> 0.961, gi.screen 1.361 -> 1.337, water.rt 0.820 -> 0.830, gi 0.684 -> 0.685: no regression beyond noise.

**Migrated, second round (2026-10-30, files: `enh/ocean.*`, `gbuffer.*`, `gi.*`, `water_rt.*`, `rt_shadows.*`, `rt_debug.*`, `water.cpp` wet shore, `upscale.cpp` / `framegen.cpp` copies, `rt_scene.cpp` skin):**
4. `enh/ocean.*` (water.ocean=fft): all five compute pipelines + the foam pass through `rhi::Pipeline`, the tile / foam images are `rhi::Texture`s (own images, sampled slots = `heapSampled()`), the wrapped sampler
   (`SamplerDesc::maxAnisotropy` 8), the clear of the foam history (`clearTexture`), the mip chains (`transitionMips` + `blitTexture`), compute -> compute barriers = `computeBarrier()`. `Ocean::record` / `recordFoam` take
   an `rhi::CommandList&`.
5. `upscale.opaque` (`copyTexture`), the exposure read-back (an `rhi::Buffer` Readback + `copyTextureToBuffer` + `invalidate`), `framegen.hudless` / the colour copy (`copyTexture`).
6. `enh/gbuffer.*`: GTAO + filter pipelines (async build kept), the two- and one-image dispatches, the debug composite. The SSAO replacement (a `d3d9::DrawReplacer` with its own `d3d9::Shader` modules) stays.
7. `enh/gi.*`: the four compute pipelines (async build kept), the DDGI update's barriers (`computeBarrier`), the screen probes' three- and two-image dispatches. Left raw: the probe / depth / ray buffers (`vk::rt::AddressBuffer`,
   shared by device address) with their `vkCmdFillBuffer` + barrier at creation.
8. `enh/water_rt.*`, `enh/rt_shadows.*` (trace / temporal / spatial), `enh/rt_debug.*` (compute + composite; the probe buffer stays an `AddressBuffer`), `water.wet_shore` (fullscreen blend), the RT skinning compute of `enh/rt_scene.cpp`.
9. Wrapped acceleration structures (section 6a): TLAS slots shared with `vk::Bindless`.

**Migrated, third round (2026-10-30): the RT scene and the buffers shared by device address** (RHI additions: section 6c):
10. `enh/rt_scene.*`: every BLAS (static: `allowCompaction`; characters / animated trees: `allowUpdate` + `preferFastBuild`, refitted up to 30 times) and the three per-frame TLASes are `rhi::AccelerationStructure`s; the builds are
    `CommandList::buildBlases` batches (pending BLASes, the characters' refits) and `buildTlasAt` (instance records in the frame ring), compaction = `queryCompactedSizes` / `createBlasStorage` / `compactBlas`, the strip index
    buffers `rhi::Buffer`s filled from the ring through `wrapBuffer`, the skinning verify buffer a readback `rhi::Buffer`. The renderer's own scratch buffers (static 32 MiB, characters 4 MiB), the query pools, the TLAS slot
    bookkeeping and the hand-written barriers are gone (the RHI puts the barriers before / after every build; the ones for shader writes -> builds are `memoryBarrier`). The pass's own two GPU timestamps are dropped (see 6c).
11. `enh/gi.*` (probe / depth / ray buffers: device-memory `rhi::Buffer`s, zeroed with `fillBuffer` + `memoryBarrier`; the diagnostics buffer a readback buffer) and `enh/rt_debug.*` (probe results: readback buffers).
    `vk::rt::AddressBuffer` is now used by `vk/raytracing.*` (the self-test, `rvk_vktest`) and the Vulkan backend's own acceleration structure storage only.

**Verification of the third round (2026-10-30, RX 7900 XTX, committed build 7f75fda = reference, copy `build\Ref3`).** `rvk_rhitest --validation` PASS on Vulkan and D3D12 (0 validation errors; new check "acceleration structure ops"),
`rvk_tests` 198 / 198, `rvk_vktest --passes --rhi`, `--rt`, `--skin` PASS. In the game (windowed, `-Validation -SyncValidation`, settings water.ocean=fft, rt.reflections=medium, shadows.enabled + shadows.rt=near, ao, gi; spots sea_cliff /
beach_sun / lake_jungle / pub_interior / shadow_inland_a; wrapper AND full mode; references of the committed build `out\rhi-migrate\ref3_wrap`, `ref3_full`): no VUID / SYNC / [error] message in either mode, every feature's first-frame line present,
pictures equal within the run-to-run noise (mean abs difference 0.3 - 7.0 of 255 against 3.1 / 6.4 / 7.8 between two runs of the committed build at beach_sun / sea_cliff / shadow_inland_a; the shadow_inland spot's sky and water
move between runs). RT scene numbers at the savegame (`rvk_perfsave`, 5 s windows of the hold, `cpuavg.py`): the first-build interval of sea_cliff (not comparable one to one: 278 BLASes / 454k triangles in the new run, 192 / 431k in the first
interval of the reference): build CPU 7.5 ms per frame against 20.9 ms; steady state pass CPU 0.33 ms (reference 0.34), GPU `pass rt.scene` 0.126 ms (0.131, `-PerfFrame` 24 frames, sea_cliff), CPU per frame at the savegame (lod 2x, 1,500 draws)
6.5 ms (6.9) without and 7.2 ms (7.8) with shadows.rt + rt.reflections + gi (the windows' spread is +-1 ms: no difference beyond noise); fps at the savegame unchanged.

**Assessment of `src/d3d9` and the replacers (third round, written before any change; the question was what can move onto the RHI without a CPU regression of the hottest path of the renderer).**
`src/d3d9` is not a pass: it is the D3D9 device model *implemented* on Vulkan, and its Vulkan use is the point of it. What it does per frame at the savegame (lod 2x): ~1,520 draws, 448 pipeline binds, 39 render passes, 3.3 MiB of
ring, 6.5 ms CPU of which ~2 ms are the draw calls, recorded by two worker threads into secondary command buffers. Per part:
- **Pipeline creation + cache (`vk::PipelineCache`, `vk/pipeline*.cpp`)** - not movable as it is. It builds pipelines from `VkShaderModule`s that outlive many pipelines (the engine's 2,500 shaders x state combinations: `rhi::Pipeline` takes
  SPIR-V per pipeline), with per-attachment blend, a stencil format, `VK_EXT_extended_dynamic_state3` blend / write mask, dynamic vertex input, the persistent `VkPipelineCache` file, prewarm threads (`prewarm / prewarmOnce`), a byte-image key
  and optional graphics pipeline libraries (fast link, optimise in the background; off by default). An RHI version would need a shader-module object, a pipeline description with all of that, a cache / library service and background
  builds in the interface: all Vulkan-shaped (a D3D12 backend would use `ID3D12PipelineLibrary` and gain none of the dynamic state), a second implementation does not exist and is not planned for the game's draws. It would be an
  interface with one implementation wrapping the existing class: no gain, a risk to the start-up path (pipelines are built on first use). **Not migrated.**
- **Draw recording (`draw.cpp` packets, `record.cpp` record threads)** - `rhi::CommandList` is a stateful pass recorder (it binds set 0 at every `bindPipeline`, writes push descriptors lazily, has no secondary command buffers, no
  indirect state caching). The packets are applied from worker threads into secondary command buffers with their own state tracking (`RecordState`, dynamic state compared per packet, vertex input interned); 448 pipeline binds x one
  extra `vkCmdBindDescriptorSets` and a second state layer on ~1,500 draws is the cost side, the DX12 equivalent (bundles / parallel lists) does not map. **Not migrated.**
- **`MeshArena`, the upload ring (`vk::Frames::allocate`), `resources.cpp` uploads** - sub-allocators over one 64 MiB buffer / the frame ring with device addresses handed to shaders and to the RT scene; as `rhi::Buffer` they would be
  the same single buffers plus a wrapper per access. They are the *frame loop* the RHI defers to (`rhi::vulkan::wrap` takes `vk::Frames`); a backend-neutral ring would be a new RHI feature without a user. **Not migrated.**
- **Small transfer helpers in `device.cpp` / `resources.cpp`** (the `StretchRect` blit, the `GetRenderTargetData` read-back, texture / buffer uploads): expressible as `blitTexture` / `copyTextureToBuffer` / `copyBuffer` on wrapped objects, but
  each needs a wrapper per call on a path that is hot on loading screens (texture uploads), for no change in behaviour. Not measured, not migrated: no benefit to weigh against even a small cost.
- **The draw replacers (`enh/water`, `enh/shadows`, `enh/gbuffer`'s SSAO replacement, `enh/material_variant`)** have **no Vulkan of their own**: they describe a `DrawReplacement` (other shaders, constants, procedural geometry from the
  frame ring) that the D3D9 model's pipeline cache and packet path execute at the draw's place. The only Vulkan calls left in them are `vk::createShaderModule` / `vkDestroyShaderModule` for their `d3d9::Shader` objects (the
  model's shader handle), so they move together with the model's pipeline creation or not at all. Nothing of the original item (b) was left to migrate on their own.
Decision: stop at the assessment (the instruction was to stop where a step is not clean). `src/d3d9`, `vk/pipeline*`, `vk/mesh_arena`, `vk/frames` are the Vulkan-specific implementation of the D3D9 model; the RHI is what the
renderer's *own* work (passes, acceleration structures, buffers) goes through. A D3D12 renderer for the game's draws would be a second implementation of the D3D9 model next to it, not a change to it.

**Status per source file (2026-10-30, after the third round)**

| files | state |
|---|---|
| `enh/debug_view`, `upscale` (prep / exposure / copies), `framegen` (depth / debug / copies), `ocean`, `gbuffer` (GTAO + filter + composite), `gi`, `water_rt`, `rt_shadows`, `rt_debug`, `water.cpp` wet shore, `rt_scene` | migrated (pipelines, recording, buffers, acceleration structures) |
| `enh/gbuffer` SSAO replacement, `enh/water`, `enh/shadows`, `enh/material_variant` | draw replacers: no Vulkan of their own except the `d3d9::Shader` module handle (follows `d3d9`) |
| `vk/fsr.*`, `vk/framegen.*`, `vk/fg_swapchain.*` | raw by necessity: AMD's DLLs record into the command buffer / own the swapchain |
| `vk/raytracing.*` | the self-test (`rvk_vktest`) and the helpers the backend's acceleration structures use |
| `d3d9/*`, `vk/pipeline*`, `vk/mesh_arena`, `vk/frames`, `vk/swapchain`, `vk/context`, `vk/bindless`, `vk/passes` | the Vulkan implementation of the D3D9 model and the backend's internals (assessment above) |
| `hal/vulkan/instance.cpp`, `device.cpp` (2 + 6 calls) | capability / format queries, not recording |

- `vk/passes.*` (`PassLayout`, `Transients`) stay as the backend's internals; `Transients::Entry` is still handed out as `vk::Image + slot` and wrapped per pass call (two small allocations;
  cache the wrapper next to the Entry when a pass wraps many images).
- No run-time A/B for the migrated passes: the old raw-Vulkan code is only in git history (the layouts are identical, so a dual path would have kept the whole `PassLayout` plumbing alive).

## 9. DXIL parity (S1) - 2026-10-30

Goal (stage S1 of the DX12 port, docs/research/dx12-game-port.md WP1): **every shader of the renderer compiles to valid, signed DXIL SM 6.6 from the same HLSL text as the SPIR-V, fits
the DX12 backend's fixed root signature, and behaves the same.** Before this stage only the test shaders were exercised on DX12; the production headers used Vulkan-only constructs
(`vk::BufferPointer`, `vk::RawBufferLoad / Store`, `[[vk::push_constant]]` arrays, the typed bindless arrays `g_t2D[]`, ...).

### 9.1 The checker: `rvk_shadertool dxil-check`

```
rvk_shadertool dxil-check embedded [--filter S] [--jobs N] [--quiet]       every embedded shader with a stage extension (ocean_fft once per FFT size), SPIR-V + spirv-val + DXIL
rvk_shadertool dxil-check corpus <dir> [--tint|--gi|--gbuf|--gbuf-motion|--capture] [--jobs N] [--max-errors N]
                                                                          the translated D3D9 shaders of a capture corpus (as `corpus`) and each enhancement variant
rvk_shadertool dxil-check uber <jsonl>... [--limit N]                      the HLSL ubershader for every captured engine request (out/matsrc/*.jsonl): plain, gbuffer, gbuffer+motion, gi hooks
```
Compiles run on `hardware threads - 2` workers (the whole corpus in ~40 s; the gi / gbuffer variants ~2 min). Every item must pass all four criteria or it is listed with its first error:
SPIR-V compiles and passes spirv-val; DXIL compiles (DXC runs `dxil.dll`'s validator inside the compile); the container is **signed** (`dxc::Result::dxilSigned`: the DXBC digest is not
zero; an unsigned module means `dxil.dll` was not next to `dxcompiler.dll`); and the module **fits the fixed root signature** (`dxc::dxilContractViolation`, DXC's container reflection):
the only bound resources may be the push constants (`b0 space0`, <= 128 bytes = 32 root constants), the root CBV (`b8 space1`) and the storage image table (`u0..u7 space1`); a bound SRV /
sampler / buffer UAV / `$Globals` / an acceleration structure at a register has no root parameter on DX12 (everything else comes from the heaps). The summary prints the largest push constant block (112 bytes: the draw ABI; the root signature holds 128). Exit code 1 on any failure.
`rvk_shadertool hlsl <name|file.cs.hlsl> --dxil out.dxil` prints the same verdicts for one shader. `shader::embedded::dxil(name)` / `dxilVariant(name, defines)` (new) are what the DX12
renderer asks for the embedded shaders (same names, defines and disk cache as `spirv`). Unit tests: `dxil_every_embedded_shader_compiles_signed`, the hooked translated shaders in
`hlsl_abi_header_and_hooks`.

### 9.2 Results

| family | before (DXIL ok / total) | after |
|---|---|---|
| embedded shaders (`dxil-check embedded`; ocean_fft x3 counted) | 29 / 53 (failing: gi 4, rt 4, shadows 5, material 2, water 4, test 5) | **54 / 54** (+ `test/parity_refs.comp`) |
| translated corpus, plain (vs20 3, vs30 146, ps20 4, ps30 2330) | 0 / 2483 (`use of undeclared identifier 'vk'`: every translated shader includes `rvk_draw.hlsli`) | **2483 / 2483** |
| `--tint` | (same cause) | **2483 / 2483** |
| `--gi` | (same) | **2246 / 2246** (237 shaders have no GI variant) |
| `--gbuf`, `--gbuf-motion` | (same) | **2325 / 2325** each (158 without) |
| `--capture` (vertex shaders) | (same) | **149 / 149** |
| ubershader, 183 captured PS_Default / PS_DefaultZPass requests x 4 variants | `ge_default.frag` did not compile (same include) | **732 / 732** |

SPIR-V stayed valid everywhere: `rvk_shadertool corpus corpus` 2483 / 2483 spirv-val clean; `rvk_tests` 199 / 199; `rvk_vktest` (plain, `--passes`, `--passes --rhi`, `--rt`, `--skin`) PASS with 0
validation errors; `rvk_d3d9test` 20 / 20 (it needed `Shader::wait()` after `createShader`: the shader workers compile asynchronously and a draw without its module is skipped; the test had been
failing for that reason since the async change); `tools/ubershader_check.py`: 183 / 183 captured requests EQUAL against the translated game shaders on the GPU (unchanged).
Exceptions: **one** root-signature exemption, `test/rt_query.comp` (the Vulkan floor's ray query self-test binds its TLAS as a push descriptor of its own layout; its DX12 counterpart is
`test/rhi_rt.comp`). Nothing else is excluded (frame generation debug, the FFT, the GI / RT passes, all water / shadow / debug shaders compile to signed DXIL).

### 9.3 The ABI on the DXIL side (`shaders/include/rvk_abi.hlsli` section 1 / 1b / 5, `rvk_draw.hlsli`)

All backend differences live in the ABI headers (+ the three generated texts below); pass shaders stay clean. Addresses are `uint2` pairs in blocks, records and push constants: Vulkan = the
64-bit device address (low, high), DX12 = **(heap slot of a ByteAddressBuffer / RWByteAddressBuffer, byte offset)** = `rhi::Buffer::shaderRef()` / `shaderRefRw()`. No 64-bit integers on the DXIL
side. The helpers (same names, both backends):

| helper | Vulkan | DXIL |
|---|---|---|
| `RVK_REF(T)` (first member of a push block: `RVK_PASS_PUSH_HEADER`), `.Get()` | `vk::BufferPointer<T, 16>` | `RvkRef<T>{slot, offset}`; `Get()` = `ByteAddressBuffer.Load<T>` of the whole struct by value (DXC keeps only the used loads, dynamically indexed array members too: 2 `rawBufferLoad` for `lights[i] + a[j]` of a 4 KB struct, checked) |
| `rvkPtr<T, A>(uint2)` | `BufferPointer<T, A>` | `RvkRef<T>` (a read-only view: SRV) |
| `RvkAddr`, `rvkAddr(uint2)`, `rvkAddrAdd(a, bytes)`, `rvkPairAdd(pair, bytes)`, `rvkAddrLow`, `rvkAddrAlign4` | `uint64_t` | `{slot, offset}` (32-bit offset arithmetic) |
| `rvkRawLoad<T, align>(a)` | `vk::RawBufferLoad<T>` | `ByteAddressBuffer.Load<T>` |
| `rvkRwLoad / rvkRwStore<T, align>`, `rvkRwAtomicAdd / Max(a, v)` (new) | `RawBufferLoad / Store`, `InterlockedAdd / Max` on a `BufferPointer<uint>` | `RWByteAddressBuffer` `Load<T> / Store<T> / InterlockedAdd / Max` |
| `RVK_BUF_LOAD / STORE / ADD` (section 5) | same, now thin wrappers of the Rw helpers | same |
| `RVK_TEX2D / TEXCUBE / TEX3D / SAMP (i)` and (new) `..._UNI(i)` for a draw-uniform index | `g_t2D[NonUniformResourceIndex(i)]`, `_UNI`: `g_t2D[i]` | heap access through a typed local, `_UNI` without `NonUniformResourceIndex` |

Rules the DXIL side forced (all verified with the checker):
- **Read vs write buffers.** DX12 needs the descriptor type to match the access: a buffer a shader WRITES (GI probes + depth moments in `gi_blend`, the GI ray buffer, `rt_skin` outputs / reference
  captures / error accumulators, the vertex-shader capture buffers of the RT scene / object motion, `rt_debug`'s probes, the screen-probe diagnostics) is reached through the `Rw` helpers with a
  `shaderRefRw` pair; everything the shaders only read (frame block, draw / stage blocks, geometry records, the engine's vertex / index buffers, the GI header and probes in the lookups) through
  `rvkPtr / rvkRawLoad` with a `shaderRef` (SRV) pair. The DX12 host therefore keeps **two descriptors per such buffer** and fills the pair that matches: the volume header the `gi_blend` pass reads carries
  the probes' / moments' UAV pair in `data.xy / data.zw` (its own copy of the header), the frame block's header the SRV pair. `rvk_gi.hlsli`: `rvkGiProbeKeyRw / RegRw / StoreKey / StoreReg`,
  `rvkGiLoadMomentRw`, `rvkGiStoreMoment`; `gi_common.hlsli`: the ray list is Rw.
- **Struct members of a root-constant block start on a 16-byte boundary** and **arrays of anything smaller than a float4 have a stride of 16** (cbuffer packing). So `RvkDrawPush` is 15 plain members
  (112 bytes, offsets identical to `abi::PushConstants`): `uint2 frame` instead of a `RVK_REF` member (`rvkLoadFrame(pc.frame)` takes the pair; old hook text `pc.frame.Get()` is now
  `rvkPtr<RvkFrameData, 16>(pc.frame).Get()`), and `vsw0..vsw3 / psw0..psw3` instead of `vsw[4] / psw[4]` (`rvkVsWin(w)` / `rvkPsWin(w)` select; folded for constant registers). A `RVK_REF` is
  only allowed as the **first** member of a push block. `RVK_VSC(n) / RVK_PSC(n)` are now a single `float4` raw load at `window + 16 n`.
- **Semantics.** Stage interfaces are matched by semantic name on D3D12 (by location on Vulkan): the engine water draws pair the translated VS (`VARYn`) with `water.frag` and `water_ocean.vert` is
  paired with it too, so those use `VARYn` (`TEXCOORDn / COLOR0` before). The pass shaders' own pairs (fullscreen vertex shader + `TEXCOORD0` pixel shaders) were already consistent.
  `[[vk::...]]` attributes are ignored by DXC for DXIL (`-Wno-ignored-attributes` is added to the DXIL argument set); `[[vk::builtin("PointSize")]]` on translated vertex shaders is dropped
  (D3D12 has no point size: point lists are the host's problem, dx12-game-port.md).
- **SV_InstanceID / SV_VertexID.** `SV_VertexID` = index + base vertex on both APIs (Vulkan `gl_VertexIndex`, D3D12 includes `BaseVertexLocation`): the capture variants' `vertexId - DrawBlock.alpha.w`
  holds. `SV_InstanceID` is 0-based per draw on D3D12 and `gl_InstanceIndex` (includes `firstInstance`) with this DXC's default SPIR-V mapping (`-fvk-support-nonzero-base-instance` would subtract
  `gl_BaseInstance` but needs the optional `shaderDrawParameters`): the D3D9 model's draws always have `firstInstance = 0` (`d3d9/draw.cpp`), so the capture variant's instance run
  `instanceId * slots + slot` is the same on both. A future draw with `firstInstance != 0` must pass it explicitly.
- Generated texts changed accordingly (`kTranslatorVersion` 9, capture variant tag `cap6`): the translator's texture / sampler objects (`RVK_TEX2D_UNI(rvk_draw.tex[..])`), the relative constant read
  (`RVK_VSC(i)`), the capture epilogue (`rvkRwLoad / rvkRwStore` on the capture buffer; `rt_scene.cpp`), the billboard fade input override (`device.cpp`).
- Limits checked by the contract test, not by the compiler: push constants <= 128 bytes (the draw ABI's 112 bytes is the largest), no bound SRV / sampler / buffer UAV, storage images
  only in `space1` (`test/dxc_probe.comp` moved from `space0` for that reason).

### 9.4 Findings in the backends (found by the equivalence tests)

- **A null SRV does not read (0, 0, 0, 1) on AMD** (it returned (0, 0, 0, 0) whatever `Shader4ComponentMapping` says): the engine's "no texture" index 0 sampled as a shadow mask shadowed everything
  (`m_fShadow = mask.a`). The DX12 backend's heap entry 0 is now a view of a real 1x1 (0, 0, 0, 255) texture (`D3D12DeviceImpl::create`, `defaultTexture_`); the shader-visible heap is write only, so
  the view is created, not copied. Cube / 3D "none" slots (Vulkan has per-kind defaults at slot 0) are still the 2D entry: a D3D9 draw with an unbound cube sampler needs a per-type default bound by
  the host (dx12-game-port.md WP3/WP4).
- **`rhi::GraphicsPipelineDesc::colorFormats[1..3]` were ignored by the Vulkan backend** (one attachment only): `vk::PassGraphicsDesc::moreColors` (same blend state on each) fixes it; the G-buffer
  variant (three render targets) now draws identically on both.

- **`pow(0, 0)` is 1 in D3D9 (legacy multiplication: 0 * anything = 0, also 0 * inf) and NaN in IEEE** (found in game, D3D12): `exp2(y * log2(x))` with x = y = 0 gave NaN in a refracted crystal's pixel shader (the gloss texture 0 x a dot product clamped to 0); the NaN pixels made the engine's AvgLuminance chain NaN, which its clamp turned into the constant 0.18 = a bright picture (monastery_hall, harbour_night). The translator emits `rvk_pow` (y == 0 -> 1) when the D3D12 backend is active; AMD's Vulkan compiler happens to behave like D3D9. Diagnostics: `RVK_EXPOSURE_LOG=1` (the adapted luminance, the luminance chain's levels and the HDR scene's NaN count every 60 frames), `RVK_NAN_BISECT=<frame>` (reads the scene back after every draw of that frame and names the draw that adds NaNs).
- **`p.Get().array[i]` with a loop index loads the whole block by value on DXIL** (no pointers: `ByteAddressBuffer.Load<T>`): GiPassParams holds 256 float4 of lights, a GI pass with 40+ lights took 10-15 ms on D3D12 against 1-2 ms on Vulkan (bisected with `RVK_SHADER_DIR` edits: trace only 1 ms, + hit surface 1 ms, + point lights 14 ms). Dynamically indexed arrays of a block are read with `rvkRawLoad<T, A>(rvkAddrAdd(rvkAddr(pair), offset + i * stride))` (`giLight`, offset asserted in enh/gi.h). Small arrays with constant indices are fine. After the change D3D12 lake_jungle: gi.screen 1.13 ms (Vulkan 2.05), gi 0.55 ms (1.01).
- `RVK_SHADER_DIR` did not reach `shader/dxc` (the CRT's `_dupenv_s` does not see variables the proxy sets with `SetEnvironmentVariableW` in DllMain): now `GetEnvironmentVariableA`.

### 9.5 Equivalence on the GPU: `rvk_rhitest` (`tools/rhitest/parity.cpp`, `parity_water.inl`, `shaders/test/parity_refs.comp.hlsl`)

Each case draws / dispatches **production shader code** through the RHI on both backends from one HLSL text; images / words of the first backend (Vulkan) are stored and the second (D3D12) is compared to
them (tolerance in the table), and where the CPU can compute the answer it is checked on each backend as well. RX 7900 XTX, `--validation` (Vulkan layer + D3D12 debug layer + GPU based validation), 0 errors:

| case | what runs | CPU check | Vulkan vs D3D12 (worst difference) |
|---|---|---|---|
| translated draw | a D3DX-assembled `vs_3_0` + `ps_3_0` translated by `toHlsl`, the real 112-byte push block (stage constants through the windows, DrawBlock with bindless texture / sampler, the FrameBlock reached by reference from a vertex input override and a pixel epilogue), vertex input locations -> `ATTRn`, varyings `VARYn` | 4 quadrant probes (texel x vertex colour x c0 x frame time) +-2 / 255 | 0 of 255 (64 x 64 x 4 bytes, strict +-2) |
| ubershader, lit diffuse | `ge_default.frag.hlsl` built by `uberPixelHlsl` from a captured request (ambient simple, directional light + shadow mask, diffuse x vertex colour x `g_ShaderData`) | non-trivial picture | 0 (tolerance 4, 0.4 % of values) |
| ubershader, material | a captured request with normal map, specular + power, lightmap, directional + complex ambient light: 7 material samplers + lightmaps + shadow mask / depth map slots, dependent texture reads | non-trivial picture | 0 |
| ubershader + hooks | the same with the `gbuffer + gi` hooks (`RVK_UBER_HOOKS`: frame block by reference, derivatives, **three render targets** RGBA8 + A2B10G10R10 + RG16F) | - | 0 (colour), 0 (normals), 0 (motion) |
| water pixel shader | `water/water.frag.hlsl` (lake model): the WaterBlock in `pc.draw`, three normal maps + two masks + a sky **cube** by engine slot, SSR through a depth-map texture by heap index, the refraction copy, the engine lighting; fed by an assembled VS writing the engine's interpolators | non-trivial picture | 0 |
| pipelines | every embedded compute shader (ocean_fft per size; 35) and the 8 vertex + pixel pairs the renderer builds, as `rhi` pipelines on this backend (Vulkan pass layout / D3D12 fixed root signature, debug layers on) | - | all created on both |
| references | `parity_refs.comp`: `rvkGiLookup` (frame block -> volume header -> probe array + depth moments, 32 probes, trilinear x visibility) and the RT record fetch (`rvkRtRecord`, 16-bit indices at a 2-byte offset and 32-bit indices, `rvkRtAttribute` of FLOAT3 / FLOAT2 / SHORT2N / absent, `rvkRtApplySegment` binary search through a segment table), results through `RVK_BUF_STORE` | position / normal / uv0 / uv1 / triangle / material segment of 32 invocations (1e-4) | 4.8e-7 (floats), 0 (ids) |

`RVK_PARITY_EXTRA="XFX_DIRLIGHTSHADOWS=0;..."` (macros added to the ubershader requests) and `RVK_PARITY_ONLY=<text>` bisect a difference (that is how the null SRV was found: shadows off made the two backends equal).

### 9.6 What S1 does not cover (hand-over to the DX12 renderer)

- The host fills the pairs (frame block `rtScene / gi / ao / sgi`, record `vertices / indices / segments`, `PushConstants` pointers, `GiPassParams.lists`, the capture addresses) with `shaderRef /
  shaderRefRw`; the engine's vertex / index buffers need raw SRVs, capture and GI buffers need both descriptors (rules above). `rhi::Buffer::shaderRef(offset)` offsets are 32-bit.
- `[[vk::image_format]]` storage images are untyped-format `RWTexture2D<float4/float2/float>` on DXIL: loads of RGBA16F / RGBA32F / RG16F / RGBA8 UAVs need the typed UAV load formats
  (`D3D12_FEATURE_DATA_D3D12_OPTIONS.TypedUAVLoadAdditionalFormats`; present on every FL 12_0+ GPU in practice, to be added to the backend's requirements check).
- The translated `[[vk::builtin("PointSize")]]` output, vertex fans, scaled vertex formats and the default texture per kind are D3D9-model matters (dx12-game-port.md).
- Shader permutations are compiled twice by the checker; at run time only the active backend's target is built (`embedded::dxil`, `device.cpp` follows with the backend seam).

## 10. The game seam (WP2a, 2026-10-30)
`rhi::GameDevice : rhi::Device` (`src/rhi/game_device.h`; docs/research/dx12-game-port.md "Status") is what `d3d9::Device` holds: the Vulkan implementation is the wrapped `VkDeviceImpl` (`rhi::vulkan::wrapGame`). It adds the
shader-visible address types of the RHI: `GpuRef` (one 64-bit value: Vulkan = device address, D3D12 = slot | offset << 32, `plus` by `refShift`), `GpuAddr` (reference + raw VA for builds), `RingAlloc`, and the opaque
`ShaderModule` of the game's pipelines. `rhi::Buffer::shaderRef / shaderRefRw` are the way to a `GpuRef` of an RHI buffer (`GpuRef::fromWords`).

WP2b (2026-10-30) put the game's textures, buffers, transient images, upload ring / mesh arena fronts, command lists (`frameCommands / uploadCommands / passCommands`) and the optional modules behind it: `rhi::Upscaler`,
`rhi::FrameGenerator`, `rhi::PresentWrapper`, `rhi::ImGuiBackend` (`src/rhi/modules.h`; Vulkan adapters over `vk/fsr`, `vk/framegen`, `vk/fg_swapchain` in `src/rhi/vulkan/modules.cpp`), `rhi::enumerateAdapters`,
`GameCaps`, `formatSupport`. `PassContext::cmd` is an `rhi::CommandList&`; `enh/rhi_scope.h` is only a facade now. Details and counts: dx12-game-port.md "WP2b".
