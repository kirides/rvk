# risen-enhanced-graphics: plan

A pure Vulkan renderer for **Risen Enhanced (x64)**. No D3D9 device is created; D3D9-era content (SM3 shader bytecode,
D3DX effects, DDS textures) is executed on Vulkan. Afterwards the renderer is the platform for enhanced graphics.

**North star (user, 2026-10-21):** the Vulkan backend is the base for future enhancements: custom water shaders and ocean
simulation, ray-traced reflections, and finally ray-traced global illumination. D3D9 parity (M3-M5) is the first step.
**Every enhancement is optional and configurable by players at their will**: per-feature toggles and quality levels, a
fallback when the hardware lacks a feature (RT stays optional on top of the 1.3 floor), and the plain parity path is always
available. Architecture work must keep that path open: scene and material semantics beyond the D3D9 draw stream (which
draw is water, terrain, a static mesh, a skinned character), points to inject or replace passes, buffers that can feed
acceleration structures when RT is enabled, and a player-facing settings system (docs/research/enhancements-roadmap.md).
**Shaders (user decision, 2026-10-21): GLSL only, no HLSL/DXC.** Hand-written / enhancement shaders are GLSL 4.60 compiled
to SPIR-V at build time (glslang / `glslc` + `spirv-val`), sharing one GLSL header with the D3D9 translator's ABI
(`src/shader/abi.h`: bindless set 0, push constants with buffer device addresses). GD3D11's HLSL (`Shaders/D3D12/*`: water,
RT, GTAO) is ported by hand to GLSL (`GL_EXT_ray_query`, `GL_EXT_buffer_reference`, `GL_EXT_nonuniform_qualifier`).
Why: GLSL is native to Vulkan, so the many GLSL examples on the web (water, ocean, RT) carry over directly; HLSL needs
SPIR-V patching (as GD3D11's `VulkanSpirvPatch.h` does), which can introduce bugs.
**Settings UI (user decision, 2026-10-21): an in-game ImGui overlay** + `bin\rvk\settings.toml` (never under `data\`); instant
per-feature A/B toggling is a first-class overlay feature (it lets players compare enhanced vs parity themselves).

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

### M3 Vulkan device, first light: reached (2026-10-21)
The main menu (docs/images/first-light-menu.png) and the game world (docs/images/first-light-world.png: the opening beach
with rain, water, vegetation, post chain and GUI) render through Vulkan, no D3D9 device; spass's `testdemo` passes in
`mode = "vulkan"`; ~65 fps unoptimised with ~1450 draws per frame. Layers: `hal/vulkan` (engine HAL) -> `d3d9::Device`
(D3D9 state model, `rvk_d3d9test`) -> `vk/` (Vulkan core, `rvk_vktest`); effects through `src/fx` + `hal/vulkan/effect.cpp`.
Open (next): D3D9 reference comparison at fixed poses; unimplemented slots seen in game (LoadSurfaceFromSurface,
SaveTextureToFile, FVF buffers: done, see below); performance pass (in game ~79.5 fps on a 120 Hz display with ~5 ms
inside vkQueuePresentKHR: done in M5, an external ~80 Hz present cadence, docs/research/performance.md).
Done after M3: vsync. FIFO presents returned at once in the game window (menu ~1500 fps; a plain window is throttled), so
`d3d9::Device` paces presents to the refresh rate of the window's monitor (`vk::FramePacer`, `[vulkan] max_fps`). HLSL
constant tables (CTAB via `src/fx`): the engine walks them for sampler masks (Engine.dll 0x6bb440); the unimplemented
GetConstantCount left its loop count uninitialised and froze the game for up to a minute after loading. Sun shadows: the
HAL refused every depth texture, so the engine's format probe (last supported of R32F, R16F, D16, D24X8, DF16, DF24) picked
**R16F** colour shadow maps (acne stripes, gaps); `DF24` is now a sampled `D32_SFLOAT` depth texture, the D3D9 reference
path on AMD (engine-gfx.md 5, docs/images/shadows-r16f-vs-df24.png; scenario `rvk_shadows` in risen-spass for the beach view).
GUI text (user report "character screen text breaks after a while", docs/research/fonts.md, confirmed and fixed): the
engine's glyph atlases get partial `LockRect`s for every new glyph, and `d3d9::Texture` had dropped its CPU copy after the
first upload, so each new glyph erased the atlas. Locked textures now keep their CPU copy (D3D9 managed semantics), only the
locked region is uploaded, and a dropped file texture locked again is read back from the GPU. Dynamic buffers copy only the
bytes written since the last draw instead of the whole buffer (the sprite VB is 448 KiB). Scenario `rvk_charscreen` in
risen-spass; `rvk_d3d9test` cases "partial texture lock", "lock after upload", "dynamic buffer across frames".
Early adapter query (docs/research/early-d3d9.md 9): `eCApplication::CreateEngine`'s pixel shader 3.0 check
(`eCGfxAdmin::sEnumAdapterDesc`) ran `Direct3DCreate9` + DxDiag before the window, loading the system d3d9, dxdiagn, d3d11,
d3d12 and the AMD D3D drivers into a Vulkan run. In Vulkan mode a detour now answers it from the (cached) Vulkan adapter
query; no system d3d9.dll is loaded in a Vulkan run any more (unless spass's screenshot hook runs), and `InitializeAdmin`
starts ~5-7 s earlier. No usable Vulkan device there -> the session falls back to passthrough before any D3D9 work.
Screenshot channel (renderer side of the D3D9 reference comparison, docs/research/reference-compare.md 4.1): exports
`rvk_capabilities`, `rvk_screenshot` (queued, captured in `SwapScreen` before the present, PNG/BMP written on a writer
thread, paths inside `<game>\data` refused), `rvk_screenshot_status`, `rvk_frame_index`, `rvk_stats`; public C header
`include/rvk_api.h` for risen-spass. HAL device slot 159 `SaveBackbufferToFile` (the engine snapshot) writes the same
32-bit BMP as D3DX through the engine's VFS. Verified in game (`run.ps1 -Snapshots`, `[debug] snapshot_frames`): engine
BMP and `rvk_screenshot` PNG of the same frame are pixel-identical; the BMP header equals the D3D9 run's byte for byte.
Remaining reachable HAL slots (docs/research/missing-slots.md): FVF vertex buffers take their stride from the FVF
(`d3d9/fvf.*`, checked against d3dx9_39's `D3DXGetFVFVertexSize` / `D3DXDeclaratorFromFVF`); the one creator,
`eCStrip_PS::CreateVertices`, passes stride 0, so its strips had stride 0 before. `Primitive::SetFVF` binds a cached
declaration from the FVF, device `GetFVFSize` answers. Cube `Lock/UnlockCubeMapLayer`, device `CreateTexture/CubeMapFromMemory`
(DDS as stored), `LoadSurfaceFromSurface` (GPU blit + read-back; the sky absorption image now gets real data) and
`SaveTextureToFile` (refused and logged; `[debug] dump_saved_textures` / `run.ps1 -DumpSavedTextures` writes a PNG to
`bin/rvk/dumps`). `testdemo` in Vulkan mode: 0 unimplemented slots, validation clean. `rvk_d3d9test` cases "fvf vertex buffer",
"cube map face lock", "load surface from surface".
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
  (refraction copy), shadow maps (DF24 sun cascades done; point lights use R16F cube maps as on D3D9/AMD; D16/D24 depth
  compare only if a reference machine needs it; depth bias on D32F is not exact yet: unused by the shadow pass), occlusion
  queries (unused by the game: only the editor's lightmap baker, "visible" stays; docs/research/occlusion.md), D3DX effects
  (post chain), fonts (the engine's own `eCFont` glyph atlases, not `eCAPIFont`: docs/research/fonts.md; atlas updates fixed),
  sprites, fixed-function remnants (generated shaders).
* **Verify**: screenshot-diff suite at N poses (D3D9 reference vs Vulkan) within thresholds; a play session without validation errors.
* Progress (2026-10-21): the suite exists and ran end to end: `scripts
efcompare.ps1` + `mods/rvkcompare` (13 spots: beach rain/sun,
  forest, harbour docks/night, pub, monastery, volcano, crystal cave, hero close-up with HUD, inventory, character screen, ESC
  menu), game time pinned before the freeze (risen-spass `set_game_time`), known-noise masks from two D3D9 runs. First result:
  11 PASS, 2 WARN (particles/sea phase), no renderer difference found; Vulkan is not brighter than D3D9 (dluma -0.2..+0.7).
  docs/research/reference-compare.md 11.

### M5 Robustness and performance
* pipeline cache on disk, async/background pipeline compilation, frame pacing, alt-tab/resize, device-lost diagnostics
  (`VK_EXT_device_fault`, breadcrumbs), GPU timings, memory budget.
* **Performance pass: done (2026-10-21, docs/research/performance.md).** Measured with `[debug] perf` / `perf_frame`
  (`run.ps1 -Perf`, `-PerfFrame`): GPU timestamps per submission and per pass, CPU split per frame, upload volume, pipeline
  and shader build counts; `[vulkan] vsync` override (`-Vsync`). The world's ~80 fps is an **external present cadence**:
  `vkQueuePresentKHR` returns every ~12.5 ms in the world (FIFO and IMMEDIATE alike), the game's own D3D9 renderer with
  vsync off also runs at 79.9 fps; CPU 6-7.5 ms and GPU ~1 ms per frame are far below it (open: AMD per-game settings for
  Risen, see performance.md 2). Done: presents on their own thread (`vk::Presenter`, queue lock in `Context`); frame
  pacer with a QPC cadence, DWM-vblank lock (also integer fractions) and an adaptive spin margin, adapted from GD3D11's
  `fpslimiter.h` (`max_fps` 60: 43-47 -> 60.0 fps); the draw path records only changed dynamic state / vertex input and
  skips redundant pipeline and sampler lookups (3.6 -> 2.8 ms per frame for ~1440 draws); pipelines of earlier runs are
  rebuilt in the background while the game loads (`pipelines.keys`: world entry 166 pipelines / 2.1 s -> 8 / 64 ms);
  the `VkPipelineCache` is saved while running. `VK_EXT_graphics_pipeline_library` support is in but **off**
  (`[vulkan] pipeline_libraries`): the AMD driver crashes on 2 of 2561 shaders as fragment libraries
  (`rvk_vktest --gpl-corpus` reproduces it). Open: intermittent overexposed frames in the beach intro (pre-existing, also
  before this pass; probably the intro's own effect, needs a frame-indexed D3D9 sequence).

### M6+ Enhanced graphics
* candidates: anisotropic filtering + MSAA/TAA/SMAA, higher-resolution and better-filtered shadows, modern tonemapping/HDR10,
  SSAO/GTAO, upscaling (FSR), improved water/sky/fog, ImGui settings overlay, per-feature toggles. Each measured and screenshotted.

## 3. Test tooling

* `risen.debug.screenshot` in spass grabs the D3D9 back buffer: with our renderer it needs a Vulkan path. Done on our side:
  `rvk_screenshot` and friends (`include/rvk_api.h`, reference-compare.md 4.1); spass selects the backend (3.1 there).
* Missing in spass for deterministic renderer tests (research spass.md section 5): fixed camera pose, fixed frame time/time scale,
  pinned time of day + weather, HUD toggle, "streaming done" signal. The engine exports what is needed
  (`gCCameraAdmin::SetFixCamera`, `eCTimer::SetFixedFrameTime`, `eCWeatherAdmin::SetCurrentDayTime`, ...).

## 4. Risks

* SM3 translation correctness (half-pixel offset, `texldl`/`texldd`, relative addressing, `dsx/dsy`, shadow compare samplers).
* D3DX effect runtime on a stub device: d3dx9_39 may call device methods we do not anticipate (fallback: own effect runtime).
* Engine code that bypasses the HAL in devirtualized branches we have not found (mitigated by own vtables, verified by M1 tracing).
* Pipeline explosion from D3D9 state combinations (mitigated by dynamic state + cache; EDS3 when available).
* Engine assumes at most 1 frame in flight (`BeginScene` waits on an event query): resource lifetime rules must match.
