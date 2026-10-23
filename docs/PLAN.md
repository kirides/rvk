# risen-enhanced-graphics: plan

## Goal

A pure Vulkan renderer for **Risen Enhanced (x64)**: no D3D9 device; SM3 shader bytecode, D3DX effects and DDS textures run on
Vulkan. Then the renderer is the platform for enhanced graphics: custom water and ocean, ray-traced reflections, ray-traced GI
(docs/research/enhancements-roadmap.md). **Every enhancement is optional and player-configurable** (toggles, quality levels,
fallback when the hardware lacks a feature); the plain parity path always stays.

Read first: `docs/research/engine-gfx.md` (HAL, frame, shaders, formats, RVAs), `docs/research/spass.md` (loading chain, test
tooling of risen-spass), `docs/research/gd3d11.md` (GD3D11's Vulkan backend, reuse).

## 1. Decisions

| topic | decision |
|---|---|
| replacement level | the engine's own HAL: our `eCAPIInstance` / `eCAPIDevice` (213 slots) + 10 `eCAPI*` resource interfaces on Vulkan; objects with their own vtables (devirtualized fast paths, engine-gfx.md 1.3); a COM-level d3d9 replacement would need D3DX internals (DXVK-sized) |
| loading | `bin\d3d9.dll` proxy, standalone (no spass dependency); Engine.dll is the only d3d9 importer; our DllMain runs first; forwards to the system d3d9 when Vulkan is off or fails |
| Vulkan floor | Vulkan 1.3 core + `VK_KHR_push_descriptor`; everything else optional with a fallback (EDS3, vertex-input dynamic state, graphics pipeline library, device fault, memory budget); dynamic rendering only, no `VkRenderPass` / `VkFramebuffer` |
| descriptors | bindless texture + sampler arrays (update-after-bind), per-draw constants in a ring buffer, indices via push constants; no mutable descriptors / DGC (GD3D11 lessons) |
| shaders | own D3D9 SM1-3 bytecode -> GLSL -> SPIR-V translator (`src/shader/`) with disk cache, covers all paths uniformly (DXVK `dxso` approach), testable offline |
| hand-written shaders | **GLSL 4.60 only, no HLSL/DXC** (user, 2026-10-21): compiled at build time (glslang + spirv-val), sharing `abi.h` / `rvk_abi.glsl`; GLSL examples carry over, HLSL needs SPIR-V patching; GD3D11's HLSL is ported by hand |
| D3DX effects | own `eCAPIEffect` runtime over the `fx_2_0` binaries the engine hands to `CreateEffect`; d3dx9_39 only as a CPU compiler (`D3DXCreateEffectCompiler`, `D3DXCompileShader`) |
| textures | own DDS loader, BC1-3 native, legacy formats converted or swizzled |
| settings UI | in-game ImGui overlay + `bin\rvk\settings.toml` (never under `data\`); instant A/B toggling is a first-class feature (user, 2026-10-21) |
| build | CMake + vcpkg manifest, MSVC, C++23, x64-windows-static-md |
| testing | own copy `B:\DevDrive\risen-test-vk`, driven by risen-spass's `test-run.ps1 -GameDir`; D3D9 vs Vulkan screenshots at fixed poses |

## 2. Milestones

Each milestone ends verified (in the game where it touches the engine) and committed.

| Milestone | Status | What it delivered | Details |
|---|---|---|---|
| M0 Scaffolding | done (2026-10-21) | repo, CMake/vcpkg, forwarding `d3d9.dll`, log, config, MinHook, Vulkan probe at `InitializeAdmin`, test scripts | spass.md |
| M1 HAL + tracing | done (2026-10-21) | generated HAL interfaces (`tools/gen_hal.py`), struct RE, `mode = "trace"` passthrough, shader corpus capture | hal.md, hal-structs.md |
| M2 Shader translator | done (2026-10-21) | SM2/3 decoder (listing identical to D3DX for all 2483 corpus shaders), GLSL 4.60 generator, glslang; all pass `spirv-val` | fx2.md, materials.md |
| M3 Vulkan device, first light | done (2026-10-21) | menu and world through Vulkan (`testdemo` passes); `vk/`, `d3d9::Device`, `hal/vulkan`, effects; later: vsync pacing, CTAB constants, DF24 sun shadows, fonts, FVF / cube / surface slots, early adapter query, screenshot channel, savegame thumbnails | missing-slots.md, fonts.md, early-d3d9.md, savegame-thumbnails.md, reference-compare.md 4.1, docs/images |
| M4 World parity | done (2026-10-21) | materials, instancing, skinning, SpeedTree, particles, sky, water, shadows, post chain, fonts; `scripts\refcompare.ps1` + `mods/rvkcompare` (13, now 20 spots): no renderer difference found | reference-compare.md, occlusion.md |
| M5 Robustness, performance | done (2026-10-21..23) | pipeline cache + background builds, presenter thread, frame pacer, perf tooling; window modes and exclusive fullscreen; debug mode / clean quit | performance.md, window-mode.md, debug-mode.md, fps-limit.md |
| M6 Platform | done (2026-10-21..23) | settings registry + overlay + A/B; phase markers, image roles, camera block; draw tags, resource identity, CTAB semantics, tint view; GLSL pipeline, injection points; optional RT tier + capability resolution; detail distances; public-release CMake preset (commit 2e758a0: d3d9.dll 12.0 -> 5.3 MB, d3d9.pdb 430.8 -> 45.9 MB) | overlay.md, frame-phases.md, draw-tags.md, injection.md, raytracing.md, lod.md, enhancements-roadmap.md 5, 6 |
| M7 Enhanced water | done (2026-10-22) | draw replacer; the engine's own water materials in GLSL, SSR, sea wave grid with lowered swashing shoreline, foam, wet beach | water.md |
| M8 Ocean | done (2026-10-22) | FFT cascades (JONSWAP + swell, weather sea state), sea floor field, breaking waves, foam buffer, game-time clock, night seam fix | ocean.md |
| Sun shadows | built (2026-10-22) | enhanced shadow mask (draw replacer) + cascade overrides (map size, range, split factor); default off at the time, now player default on | shadows.md |
| M9 RT water reflections | P1-P3 done (2026-10-22) | RT scene (BLAS cache, TLAS), materials at hits, `rt.reflections` for water | rt-reflections.md, rt-scene-facts.md, rt-albedo-facts.md |
| High fps compatibility | done (2026-10-23, 4817bab) | ported Vaana's falling / climbing / thunder fixes (`compat.high_fps`) | fps-limit.md 12 |
| Menu frame rate limit | done (2026-10-23) | `vulkan.menu_fps` (default 60) while a menu page is open; loading screens never paced (a 30 fps cap made a quick load 20 s instead of 2 s) | menu-fps.md |
| M10 RT GI | Stages A-C built (2026-10-23, 12cc3f5, 451823e, 4dad586) | Delivered: DDGI probes as the engine's ambient (c14..c21 overrides), multi-bounce, characters in the RT scene, thin G-buffer (oct normal + id, motion), per-object motion for characters, GTAO replacing the game's SSAO, screen probes correcting the DDGI per surface (details rtgi.md 9-11). Plan: Stage A: DDGI probes holding L2 SH in the engine's ambient format (constant overrides of c14..c21), RT lighting incl. characters (skinned BLASes); Stage B: thin G-buffer + motion vectors (also for FSR 3/4, GTAO); Stage C: screen probes, later ReSTIR / RT pipelines | rtgi.md 6-7 |
| M11+ | open | anisotropic filtering, AA (MSAA / TAA / SMAA), tonemapping / HDR10, SSAO / GTAO, upscaling (FSR), sky / fog. Upscaling (FSR): native AA built (2026-10-23, FSR 3.1 via the signed FFX API DLL, jitter + RCAS + LOD bias); render scale + quality modes built (2026-10-23: scene targets at display / ratio via the `eCImageFilterScene::OnUpdateOutput` detour, output-sized FSR image sampled by the post chain; quality / balanced / performance / ultra performance = 1.5 / 1.7 / 2 / 3); open: reactive / transparency masks, FSR 4 check (needs RDNA4), uncapped GPU cost, refcompare recalibration for scaled renders | enhancements-roadmap.md 3.6, upscaling.md |

### Open

* M2: SM 1.x (unused by the game), GPU-level semantic checks of the translator, D3D9 `0 * inf = 0` emulation if needed.
* M4: depth bias on D32F not exact (unused by the shadow pass); D16 / D24 depth compare only if a reference machine needs it; point lights use R16F cube maps as on D3D9/AMD.
* M5: window modes on different-size / scaled multi-monitor setups; exclusive fullscreen on 360 Hz desktops, NVIDIA / Intel, HDR, secondary monitor.
* M5: intermittent overexposed frames in the beach intro (pre-existing, probably the intro's own effect; needs a frame-indexed D3D9 sequence; performance.md).
* M5: `VK_EXT_graphics_pipeline_library` is off (`[vulkan] pipeline_libraries`): the AMD driver crashes on 2 of 2561 shaders (`rvk_vktest --gpl-corpus`).
* M5: the world's ~80 fps cap is an AMD Radeon Chill profile on `Risen.exe`, not ours (user action: Chill off; fps-limit.md 11).
* M5: not ours: Ctrl+E (stock crash), exit faults in `SharedBase.dll+0x7caa9` / Steam's `tier0_s64` (debug-mode.md).
* M6: spass Lua binding for the settings exports; presets with the first enhancement; the camera moves in only ~half of the rendered frames in a flight (ocean.md); Preview / point-light views unseen; light-streak / dynamic-light contexts unseen; deferred: motion / normal outputs, previous-constants map, TAA jitter, CTAB-name overrides; lod.md 5 (cost, motion test, `lod.view` > 1).
* M7: underwater post, area profiles, wet-sand sheen (water.md 10).
* M8: ocean.md 10.
* Shadows: the cliff walk itself (exact place), RT shadows, terrain map beyond the range.
* M10: interior volumes (portal rooms), true hemisphere SH from the engine (`eCHemisphere_PS`) instead of the most frequent draw set, interior brightness tuning (the `keep` remainder over-brightens some rooms; pub +24 luma), `water_rt` on the captured light, lightmaps at hits, per-pass GPU timing via `-PerfFrame` (rtgi.md 9); stage B: AO arc pattern at the pub wall, contact shade under thin overlay meshes, no depth history for temporal rejection, no TAA jitter / FSR, camera-only motion for moving non-skinned objects, AO cost unmeasured (rtgi.md 10); stage C: the DDGI's multi-bounce uses the cheap lookup (possible leaks at the second bounce), GI grain, `gi.screen=high` cost over time unverified, luminance-only ratio (no colour bleeding) (rtgi.md 11); follow-ups: characters / alpha get no same-frame AO or screen probe correction, characters lag one frame in the RT scene (rtgi.md 12).
* M9: P4 (leaf cards / billboards / grass, skinned characters, lightmaps / point lights at hits); SpeedTree strip details and cascade-only instance textures (rt-reflections.md 3.2); dynamic geometry, motion vectors, denoise (enhancements-roadmap.md 3.3-3.5).

## 3. Test tooling

* Screenshots: `rvk_screenshot` and friends (`include/rvk_api.h`, reference-compare.md 4.1); spass's `risen.debug.screenshot` grabs the D3D9 back buffer only.
* Deterministic renderer tests need in spass (spass.md 5): fixed camera pose, fixed frame time, pinned time of day + weather, HUD toggle, "streaming done" signal. The engine exports what is needed (`gCCameraAdmin::SetFixCamera`, `eCTimer::SetFixedFrameTime`, `eCWeatherAdmin::SetCurrentDayTime`).
* Everything else (run.ps1 switches, refcompare, water-compare, perf, scenarios): `CLAUDE.md`.

## 4. Risks

* SM3 translation correctness (half-pixel offset, `texldl` / `texldd`, relative addressing, `dsx` / `dsy`, shadow compare samplers).
* D3DX effect runtime on a stub device: d3dx9_39 may call device methods we do not anticipate (fallback: own effect runtime).
* Engine code bypassing the HAL in devirtualized branches (mitigated by own vtables, M1 tracing).
* Pipeline explosion from D3D9 state combinations (dynamic state + cache; EDS3 when available).
* The engine assumes at most 1 frame in flight (`BeginScene` waits on an event query): resource lifetime rules must match.
