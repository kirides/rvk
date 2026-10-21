# risen-enhanced-graphics: plan

A pure Vulkan renderer for **Risen Enhanced (x64)**. No D3D9 device is created; D3D9-era content (SM3 shader bytecode,
D3DX effects, DDS textures) is executed on Vulkan. Afterwards the renderer is the platform for enhanced graphics.

Research that this plan rests on (read these before changing the architecture):
* `docs/research/engine-gfx.md`: the engine's graphics HAL, frame structure, shaders, formats (RVAs).
* `docs/research/spass.md`: loading chain, hook infrastructure, test tooling of risen-spass.
* `docs/research/gd3d11.md`: GD3D11's Vulkan backend, what to reuse, lessons learned.

## 1. Decisions

| topic | decision | why |
|---|---|---|
| replacement level | **the engine's own HAL**: our `eCAPIInstance` / `eCAPIDevice` (213 virtual slots) and the 10 `eCAPI*` resource interfaces, implemented on Vulkan | every D3D9/D3DX use in Engine.dll sits behind these pure-virtual interfaces with one global device pointer (`eCGfxContextAdmin::ms_pAPIDevice`); objects with *their own* vtables send all speculatively devirtualized fast paths through the virtual path (engine-gfx.md 1.3). A COM-level d3d9 replacement would also have to satisfy D3DX's internals (DXVK-sized). |
| loading | **`bin\d3d9.dll` proxy**, standalone (no dependency on spass) | Engine.dll is the only importer of d3d9 (`Direct3DCreate9` only), d3dx9_39 does not import d3d9. Our DllMain runs before Engine.dll's, so engine hooks are in place before `eCGfxAdmin::InitializeAdmin`. Clean fallback: forward to the system d3d9 when Vulkan is off or fails. |
| Vulkan floor | **Vulkan 1.3 core** (dynamic rendering, synchronization2, extended dynamic state 1/2, maintenance4) + **`VK_KHR_push_descriptor`** (core in 1.4, on every desktop driver). Everything else optional with a fallback: `VK_EXT_extended_dynamic_state3`, `VK_EXT_vertex_input_dynamic_state`, `VK_EXT_graphics_pipeline_library`, `VK_EXT_device_fault`, `VK_EXT_memory_budget`. | Dynamic rendering is required anyway; GPUs/drivers that expose `VK_KHR_dynamic_rendering` but not 1.3 are practically nonexistent on Windows (Kepler/GCN1-2 legacy drivers have neither). 1.3 core gives the D3D9-style dynamic state for free. No `VkRenderPass`/`VkFramebuffer` anywhere. |
| descriptors | bindless texture + sampler arrays (descriptor indexing, update-after-bind), per-draw constants in a ring buffer, indices via push constants / push descriptors | D3D9's "set texture per stage, set constants, draw" maps to cheap per-draw data without per-draw descriptor set allocation. No `VK_EXT_mutable_descriptor_type` / device-generated commands (GD3D11 lessons). |
| shaders | **D3D9 SM1-3 bytecode -> SPIR-V translator** (own, `src/shader/`), disk cache | the material shaders ship precompiled (`ShaderMaterialPool_Master.smp`), post effects compile through D3DX to SM3 bytecode; translating bytecode covers all paths uniformly (DXVK `dxso` approach). Fully testable offline against captured shaders + `spirv-val`. |
| D3DX effects | **own `eCAPIEffect` runtime over the compiled `fx_2_0` binaries** the engine hands to `CreateEffect` (techniques, passes, parameters, states, embedded SM2/3 shaders; format known from Wine's d3dx9) | the engine compiles the `.fx` files through the HAL (`CompileEffectFromFile`) and passes the binary to `CreateEffect` (docs/research/hal.md 4). Our `CompileEffect*` / `eCAPIHLSLShader::Compile` keep using d3dx9_39 **only as a CPU compiler** (`D3DXCreateEffectCompiler`, `D3DXCompileShader`: no device involved; Engine.dll imports d3dx9_39 anyway). No D3DX rendering objects. |
| textures | own DDS loader (`._ximg` is a DDS), BC1-3 native, legacy formats (L8, A8, 1555, 4444, X8R8G8B8) converted or swizzled | replaces `D3DXCreateTextureFromFileInMemoryEx`. |
| build | CMake + vcpkg manifest (`builtin-baseline` = the user's current vcpkg checkout), MSVC, **C++23**, x64-windows-static-md | user decision. |
| testing | own test copy `B:\DevDrive\risen-test-vk`, driven by risen-spass's `scripts/test-run.ps1 -GameDir`; reference screenshots from the D3D9 path vs. Vulkan at fixed poses | never touch the real install; screenshot diffs make parity measurable. |

## 2. Milestones

Each milestone ends verified (in the game where it touches the engine) and committed.

### M0 Scaffolding: done (2026-10-21, verified: testdemo PASS through the proxy, probe in rvk.log)
* repo, CMake/vcpkg, `d3d9.dll` proxy that forwards to the system d3d9, log (`bin\rvk\rvk.log`), config (`bin\rvk\config.toml`),
  MinHook, binding of Engine.dll exports by decorated name.
* hook `eCGfxAdmin::InitializeAdmin`; Vulkan probe there (instance, physical devices, required/optional features to the log).
* `scripts/` to create the test copy, deploy and run (on top of risen-spass's `test-run.ps1`).
* **Verify**: the game runs unchanged through the proxy; the log shows the probe.

### M1 HAL knowledge + tracing passthrough: tracing done (2026-10-21, docs/research/hal.md)
* `tools/gen_hal.py`: generate exact C++ interface headers for `eCAPIInstance`, `eCAPIDevice`, `eCAPITexture`, ... from the vtables
  and the demangled exports (slot order, signatures; unique names per slot so overloads cannot reorder).
* RE the structs crossing the interface (`eSGfxLayersDesc`, `eSGfxDrawCallDesc`, `eSGfxVertexBufDesc`, `eSAPIStartParameters`, ...).
* **Tracing passthrough** (`mode = "trace"`): our `eCAPIInstance`/`eCAPIDevice`/resource wrappers forward to the real `eCDX9*` objects
  and count/log every slot. Validates the generated layouts against the real engine (wrong layout = immediate crash) and yields the
  list of slots actually used, render-target formats, vertex declarations, lock patterns.
* Capture every shader bytecode (VS/PS programs, effect shaders) to a corpus directory for M2.

### M2 Shader translator (offline): done (2026-10-21)
Result: `src/shader/` decodes SM 2/3 bytecode (listing identical to `D3DXDisassembleShader` for all 2483 corpus shaders,
`tools/compare_disasm.py`), generates GLSL 4.60 against the contract in `src/shader/abi.h` and compiles it with glslang; all
corpus shaders pass `spirv-val`; unit tests assemble hand-written control-flow/texture shaders with `D3DXAssembleShader`.
Chosen: GLSL + glslang instead of a hand-written SPIR-V emitter (readable output, ~7 ms per shader -> disk cache in M3).
Open: SM 1.x (not used by the game so far), GPU-level semantic checks (M3/M4 screenshots), D3D9 `0 * inf = 0` emulation if needed.
Original scope:
* SM1-3 bytecode parser + SPIR-V emitter: registers (c/i/b, r, v, t, s, o), all opcodes the corpus uses, `dcl_*`, `texld*`, predicates,
  loops/rep, relative addressing, `oPos` half-pixel and clip-space fix-up, alpha test and fog as specialization constants.
* Tests: the whole corpus translates, `spirv-val` passes, a disassembly-diff for a few hand-checked shaders.

### M3 Vulkan device, first light
Progress (2026-10-21): Vulkan core in `src/vk/` (context with the 1.3 floor + optional EDS3 / vertex-input-dynamic / robustness2,
swapchain, frames on a timeline semaphore with deferred deletion and a BDA upload ring, bindless set + pipeline layout, images
with sync2 layout tracking, pipeline cache for dynamic rendering). `rvk_vktest` renders a translated D3D9 shader pair off-screen
with validation on and checks every pixel against D3D9's rasterisation rules (half-pixel fix-up = +0.5 px): PASS.
* device/queues/VMA/swapchain on the game window; frames in flight, upload rings, deferred deletion; validation + debug utils.
* HAL: textures (DDS), buffers (static + dynamic DISCARD/NOOVERWRITE), vertex declarations, programs, constants, render states ->
  pipeline key + dynamic state, draw/DrawUP, render targets, clear, present, reset/resize.
* **Verify**: the main menu and the GUI render on Vulkan (`mode = "vulkan"`), screenshot.

### M4 World parity
* materials, lightmaps, instancing (`SetStreamSourceFreq`), skinned meshes, SpeedTree, vegetation, particles, decals, sky, water
  (refraction copy), shadow maps (D16/D24S8 depth compare, DF16/DF24, VSM formats), occlusion queries, D3DX effects (post chain),
  fonts (`eCAPIFont` glyph data), sprites, fixed-function remnants (generated shaders).
* **Verify**: screenshot-diff suite at N poses (D3D9 reference vs Vulkan) within thresholds; a play session without validation errors.

### M5 Robustness and performance
* pipeline cache on disk, async/background pipeline compilation, frame pacing, alt-tab/resize, device-lost diagnostics
  (`VK_EXT_device_fault`, breadcrumbs), GPU timings, memory budget.

### M6+ Enhanced graphics
* candidates: anisotropic filtering + MSAA/TAA/SMAA, higher-resolution and better-filtered shadows, modern tonemapping/HDR10,
  SSAO/GTAO, upscaling (FSR), improved water/sky/fog, ImGui settings overlay, per-feature toggles. Each measured and screenshotted.

## 3. Test tooling

* `risen.debug.screenshot` in spass grabs the D3D9 back buffer: with our renderer it needs a Vulkan path. Our DLL exports
  `rvk_screenshot(path)` (and a config hotkey); spass can call it (small spass change, or `ffi` with `-Unsafe`).
* Missing in spass for deterministic renderer tests (research spass.md section 5): fixed camera pose, fixed frame time/time scale,
  pinned time of day + weather, HUD toggle, "streaming done" signal. The engine exports what is needed
  (`gCCameraAdmin::SetFixCamera`, `eCTimer::SetFixedFrameTime`, `eCWeatherAdmin::SetCurrentDayTime`, ...).

## 4. Risks

* SM3 translation correctness (half-pixel offset, `texldl`/`texldd`, relative addressing, `dsx/dsy`, shadow compare samplers).
* D3DX effect runtime on a stub device: d3dx9_39 may call device methods we do not anticipate (fallback: own effect runtime).
* Engine code that bypasses the HAL in devirtualized branches we have not found (mitigated by own vtables, verified by M1 tracing).
* Pipeline explosion from D3D9 state combinations (mitigated by dynamic state + cache; EDS3 when available).
* Engine assumes at most 1 frame in flight (`BeginScene` waits on an event query): resource lifetime rules must match.
