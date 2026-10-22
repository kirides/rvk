# risen-enhanced-graphics: guidance for Claude

A **pure Vulkan renderer for Risen Enhanced (x64)**. It ships as a `d3d9.dll` proxy in `<Risen>\bin` (Engine.dll is the only
importer of d3d9, and only of `Direct3DCreate9`). It replaces the engine's own graphics HAL (`eCAPIInstance` / `eCAPIDevice` and
the `eCAPI*` resource interfaces) with a Vulkan implementation; no D3D9 device is created in Vulkan mode.

Read first: `docs/PLAN.md` (decisions, milestones), `docs/research/engine-gfx.md` (the engine's HAL, frame, shaders, formats, RVAs),
`docs/research/spass.md` (loading chain, test tooling of risen-spass), `docs/research/gd3d11.md` (GD3D11's Vulkan backend, reuse).

## Hard rules

1. **Never touch the real game install** `B:\SteamLibrary\steamapps\common\Risen` (nor Risen Classic 2009). Test only in the copy
   `B:\DevDrive\risen-test-vk`, **always windowed**. Careful: the copy's `data\common` and `data\compiled` are **junctions into the
   real install**; never write through them (the engine writes `Data/Compiled/Materials/ShaderMaterialPool_User.smp` on shader
   pool misses: keep the pool hits intact / redirect such writes before shaders change).
2. **Vulkan floor**: Vulkan 1.3 core + `VK_KHR_push_descriptor`; everything else optional with a fallback (PLAN.md section 1).
   **Dynamic rendering only**: never `VkRenderPass` / `VkFramebuffer`.
3. **Commit at milestones**: small, reviewable commits once a feature is built *and* verified (in the game where it touches the
   engine). Commit trailers come from the session's attribution reminder.
4. Verify engine behaviour in the running game instead of assuming. Record findings (RVAs, layouts, confirmed vs inferred) in
   `docs/research/engine-gfx.md` (or a new `docs/research/*.md`), update `docs/PLAN.md` milestones.

## Build

```powershell
$env:VCPKG_ROOT = 'E:\git\github\vcpkg'
cmake --preset default                                  # only after CMakeLists.txt changed (new files!)
cmake --build build --config RelWithDebInfo             # -> build\RelWithDebInfo\d3d9.dll, rvk_tests.exe
build\RelWithDebInfo\rvk_tests.exe                      # unit tests (pure logic, no game)
build\RelWithDebInfo\rvk_vktest.exe --rt                 # GPU: optional RT tier (BLAS/TLAS/ray query self-test; also --passes)
```
C++23, MSVC (VS 18 2026), CMake + vcpkg manifest (`vcpkg.json`, `builtin-baseline` = the user's vcpkg checkout; when they pull,
take `git -C E:\git\github\vcpkg log -1 --format=%H`). Triplet `x64-windows-static-md`. New source files go into `CMakeLists.txt`.
volk is compiled in `src/vk/volk_impl.c` (`VK_NO_PROTOTYPES`, `VK_USE_PLATFORM_WIN32_KHR`), not vcpkg's `volk.lib`.
The editor's clang diagnostics ("file not found", "undeclared identifier std") are noise: only the MSBuild result counts.
`rvk_core` has a private PCH (std + spdlog): files outside it (the DLL target, tests) must include what they use.
The Vulkan SDK (`C:\VulkanSDK\1.4.363.0`, `VULKAN_SDK`) provides validation layers, `spirv-val`, `glslc`, `dxc`.

## Test loop

```powershell
.\scripts\run.ps1 -Mods testdemo -Scenario testdemo -WaitForResult -Seconds 240      # mode passthrough (default)
.\scripts\run.ps1 -Mode trace ...      # -Mode vulkan, -Validation, -RvkLogLevel debug, -NoRenderer (reference D3D9 run)
```
Vulkan-mode debugging (`run.ps1` switches -> `bin/rvk/config.toml` `[debug]`): `-Screenshots '450,500'` (back buffer ->
`bin/rvk/screenshots/frame_<n>.png`, view them with Read; the world appears ~frame 450 of `testdemo`, loading screens run
at ~0.1 fps), `-LogDraws N` (first N draws in detail: shaders, textures, targets, states), `-LogFrame N` (every key HAL call
of frame N), `-Snapshots N,M` (at those `eCApplication::Draw` calls: the engine snapshot (slot 159 -> BMP in the copy's
`localappdata\Local\Risen\ScreenShots`) + a self-test of the `rvk_screenshot` exports), `-Validation`, `-DumpSavedTextures` (the engine's refused `SaveTextureToFile` writes -> `bin/rvk/dumps/*.png`), `-AllowFocus` (test runs
stay in the background by default). rvk.log prints fps / draw counts every 5 s and every unimplemented HAL slot once, plus the
last frame's phase structure (`Post{Shadow{..} Main{DepthPrepass(n) ShadowMask(n) Solid(n) Refracted(n) Alpha(n)} ..}`), its
main camera and a camera cross-check (docs/research/frame-phases.md); `-LogFrame` lines carry a `[View/Pass]` prefix. Also the
draw-kind histogram ("draw kinds": kind x view; "untagged draw" lines name draws no hook tagged; docs/research/draw-tags.md).
Debug view: `-Setting 'debug.tint_draws=kind'` (or `pass`; overlay category Debug) tints draws by kind (water blue, SpeedTree
green, skinned red, instanced cyan, unknown magenta, ...); `rvkcompare` `-Param show_actors=1` keeps the player / NPCs visible.
Scenario `rvk_camera` (this repo's mods) holds fixed camera poses to compare the camera block with `risen.camera.get`.
Injection test view: `-Setting 'debug.inject_view=depth'` (distance bands + a 5 m world grid from the depth map and the
FrameBlock camera, drawn after the opaque scene) or `tint`; `-SyncValidation` = `-Validation` + the layer's
synchronization validation (`[vulkan] sync_validation`; environment variables do not reach the game).
Performance (docs/research/performance.md): `-Perf` (GPU timestamps, CPU split, uploads, pipeline builds, pacer: `perf:`
lines every 5 s), `-PerfFrame N` (GPU time of every render pass of frame N), `-Vsync on|off|engine`, `-MaxFps N`; the
`rvk_shadows` scenario logs the fps for either renderer (also `-NoRenderer`). Other games on the GPU skew GPU times.
Player settings + overlay (docs/research/overlay.md): every run deletes `bin/rvk/settings.toml` (defaults) unless
`-KeepSettings`; `-Setting 'vulkan.max_fps=60','vulkan.vsync=off'` writes one. The ImGui overlay (F10, A/B flip F11) is
never in captures unless `-OverlayInScreenshots`; `-Commands '1500: overlay open','1600: key c','1700: ab flip'` runs
overlay / settings commands at presents (`key`/`click` post real window messages: input tests in the background);
`-NoOverlay`. config.toml's non-default `[vulkan] vsync / max_fps / adapter` lock the matching player setting.
Enhanced water (docs/research/water.md; settings category Water, default off): `-Setting 'water.enabled=true'` (+
`water.quality=low|medium|high`, `water.reflections=sky|ssr`, `water.ocean=off|gerstner|fft`, `water.ocean_quality=low|medium|high`, `water.sea_state=auto|calm|moderate|rough|storm`,
`water.wind_direction`, `water.wave_scale`, `water.murkiness`,
`water.reflection_strength`; debug view `water.debug=shore_field|depth|normals|reflection|foam|sea_floor|waves`). The look is the engine's
own water material (its pixel shader's model ported in `shaders/water/water.frag`, picked by sampler layout; the engine's
VS stays on its meshes; water.md 2.3, 4.3) + traced reflections, waves, foam, wet beach. rvk.log: `water: material ...`
(each water material's name / mesh / kind once), `water: draw of material ...` (per material x pixel shader: its engine
shaders, declaration, states, constants c6..c14 and VS c232..c241; bytecode in `bin/rvk/cache/water/`), `enh: water:
material ... -> model ...` (`none` = an unknown water shader: engine draw), `enh: water: body ...`, `sea mesh mapping`,
`coast overlay` / `shore field` lines. Side by side engine vs enhanced: `.\scripts\water-compare.ps1 [-Spots a,b]
[-Setting k=v] [-EngineDir out\water\<stamp>] [-Mask | -MaskDir out\water\<stamp>]` -> `out\water\<stamp>\sbs_<spot>.png` +
water luma / hue table (`tools/water_stats.py`; `-Mask` = a `water.debug=normals` run for the water masks; `-Burst 4
-Interval 8` = a motion sequence per spot -> `motion_<spot>.png`).
FFT ocean (docs/research/ocean.md, M8; `water.ocean=fft`): compute passes at AfterOpaque (`pass water.ocean` in
`-PerfFrame`), the sea floor field from the main view's static meshes (`DrawReplacer::observeOpaque`), baked on a worker
and cached in `bin\rvk\cache\ocean\floor_v1.bin` (delete it to re-learn the coast); rvk.log `enh: ocean: ...` (cascades,
sea state / Hs, weather -> sea state, `sea floor field ... ms`, `foam buffer`, `player position`). The surf's foam: a
camera-centred buffer (`ocean_foam.comp`, ocean.md 4.4) + the pattern in `shaders/water/water_noise.glsl`; tune foam
patterns offline first (a numpy port of the noise renders coverage ramps to PNG in seconds; game runs take minutes);
`water.debug=foam` shows the terms (green white water, blue residual, red whitecaps, grey contact). Ocean spots of rvkcompare: `sea_cliff`, `sea_horizon`,
`harbour_mouth` (the camera's distance is capped near 6,000 units: give poses as target + distance <= 5,500). Swim check:
scenario `rvk_swim` (the hero in the sea, his swim line logged, pictures `swim_follow_b*` / `swim_water_b*`). Survey
scenario `rvk_water` (water level-mesh entities' boxes + a look at each; `-Param 'only=Levelmesh_Lakemesh_06','find=SP_Don'`;
`'around=x;y;z','radius=2500'` = 17 pictures around a place; with `-RvkLogLevel trace` rvk.log also names opaque materials).
Ray tracing tier (docs/research/raytracing.md): optional, enabled at device creation when the GPU has it and `rt.allow`
(restart setting, default on; `-Setting 'rt.allow=false'` = floor only); rvk.log `vulkan: ray tracing ...` + `caps:` lines
(the capability report, also the overlay's "Capabilities"). `-RtSelfTest` (`[debug] rt_selftest`) builds a BLAS/TLAS and
traces it with ray query at device creation ("rt selftest: PASS"). Settings needing a capability declare
`Spec::requirements` (greyed + parity fallback when unmet; features read `asX()` = the effective value).

`run.ps1` creates the test copy on first use (`scripts/new-test-copy.ps1`, from risen-spass's `B:\DevDrive\risen-test`), deploys
`d3d9.dll` + `bin\rvk\config.toml`, and drives the game with **risen-spass's `scripts/test-run.ps1 -GameDir`** (windowed, muted,
watchdog, scenario result; mods come from this repo's `mods/` (`-ModPath`), then the spass repo `B:\DevDrive\git\github\risen-spass\mods`). Our log:
`B:\DevDrive\risen-test-vk\bin\rvk\rvk.log`. Check free RAM first; kill a `steamwebhelper.exe` above 5 GiB (user instruction).
**Process liveness and RAM checks run on the `sysprobe` agent** (`.claude/agents/sysprobe.md`, Haiku; user instruction):
"is Risen.exe / the run still alive", free RAM, steamwebhelper size. Delegate them to it instead of checking yourself
(if you cannot spawn agents, keep such checks to one cheap command).

**Cheaper agents for routine work** (`.claude/agents/`, user instruction: delegate reasonable work to Haiku / Sonnet;
implementation and design stay on the main model):

| agent | model | use for |
|---|---|---|
| `sysprobe` | Haiku | process liveness, free RAM, steamwebhelper > 5 GiB |
| `build-runner` | Haiku | build + rvk_tests / rvk_vktest / rvk_d3d9test / shader corpus / compare_disasm, pass/fail + error excerpts |
| `game-runner` | Sonnet | one in-game run (run.ps1 / refcompare.ps1 / water-compare.ps1) + digest of verdicts, rvk.log, validation |
| `shot-reviewer` | Sonnet | first-pass visual review of screenshots / side-by-sides / motion sequences, with crops |
| `re-lookup` | Sonnet | focused Ghidra / export lookups (RVA, field, callers, thunk / COMDAT check), read-only |
| `docs-scribe` | Sonnet | docs/research, PLAN, CLAUDE.md updates from given facts; commits doc files only |
risen-spass's own `risen.debug.screenshot` grabs the D3D9 back buffer (hooks `Present`): it does not work in Vulkan mode.

Screenshot diff (docs/research/reference-compare.md 6): `python tools/refcompare.py compare <d3d9 dir> <vk dir> -o <out>`
(pairs by spot name, bursts `<spot>_b<k>.png` -> median + noise mask, `--meta-a/--meta-b` RVKCMP pose check; metrics, heat
maps, blink GIFs, `report.html`/`report.json`; exit 1 on FAIL, 2 on INVALID); `calibrate A A2 -o thr.json` derives thresholds
from two runs of one renderer (+ known-noise masks). Tests: `python -m unittest discover -s tools/tests`. Keep its output outside the repo or in `/out/`.
**Reference comparison end to end**: `.\scripts\refcompare.ps1` (17 fixed spots of `mods/rvkcompare`: game D3D9 run + Vulkan
run in the dedicated copy `B:\DevDrive\risen-test-vk-cmp`, instance 3, pinned ConfigUser.xml; report
`out\refcompare\<stamp>\report\report.html`). `-Calibrate` = two D3D9 runs -> a reusable reference (thresholds + known-noise
masks); `-ReferenceDir out\refcompare\<stamp>` = only the Vulkan run against it (current stored reference:
`out\refcompare\20261022-093252`, 20 spots); `-Spots a,b`; `-Setting k=v` (Vulkan run's
player settings, e.g. `rt.allow=false`). Renderer scenario mods live in
this repo's `mods/` (never in risen-spass). Details, spot list, noise handling: docs/research/reference-compare.md 5, 7, 8, 11.

Startup facts (measured, docs/research/early-d3d9.md): `eCApplication::CreateEngine` calls `eCGfxAdmin::sEnumAdapterDesc`
(pixel shader 3.0 check) before the window: in passthrough/trace it runs `Direct3DCreate9` + DxDiag (5.7-7.4 s, loads
d3d9/dxdiagn/d3d11/d3d12), then `InitializeAdmin` calls `Direct3DCreate9` again; device creation in the AMD D3D9 driver takes
~10 s. In Vulkan mode our detour answers it from Vulkan (~0.15 s) and no system d3d9.dll is loaded. rvk.log's "startup state
at InitializeAdmin" line lists uptime, D3D/Vulkan/overlay modules and the process's windows. The hidden
`temp_d3d_window_4039785` window is OBS Studio's `graphics-hook64.dll`, loaded as the implicit Vulkan layer
`VK_LAYER_OBS_HOOK` by our first `vkCreateInstance` (`DISABLE_VULKAN_OBS_CAPTURE=1` disables the layer).

## Shader translator

`rvk_shadertool corpus <dir>` translates + validates every captured shader (corpus from `mode = "trace"`:
`<test copy>/bin/rvk/corpus`, copy it to `<repo>/corpus`, git-ignored: game-derived). `show <file>` prints listing + GLSL,
`refdisasm <file>` the D3DX reference listing. `python tools/compare_disasm.py corpus` diffs our listing against D3DX for the
whole corpus (must stay 100 %). The contract between translated shaders and the renderer is `src/shader/abi.h`; its GLSL
side is `shaders/include/rvk_abi.glsl`, which the translator pastes into every translated shader (change both together;
rvk_tests `abi_glsl_layout_matches_cpp` compares the offsets; bump `kTranslatorVersion` in `d3d9/device.cpp` when the
translated GLSL changes).

## Hand-written shaders (GLSL only)

Enhancement / pass shaders are **GLSL 4.60 in `shaders/`** (no HLSL, PLAN.md). `rvk_glslc` (`tools/glslc`, built from the
vcpkg glslang + SPIRV-Tools: no Vulkan SDK needed) compiles each to SPIR-V 1.6 at build time, runs spirv-val, writes
`build/generated/shaders/<path>.h` + a depfile (editing an include rebuilds its users); they are embedded into `d3d9.dll`
(`shader::embedded::spirv("debug/inject_view.comp")`). **A new shader goes into `RVK_SHADERS` in `CMakeLists.txt`** (then
`cmake --preset default`). `#include "rvk_abi.glsl"` (push constants, buffer references, FrameBlock, bindless set; pass
shaders `#define RVK_PASS_SHADER` first and start their push block with `RVK_PASS_PUSH_HEADER`) and `rvk_common.glsl`
(sampling, depth -> world helpers). Passes plug in at injection points (`Device::addPass(inject::Point::AfterOpaque, ...)`,
`vk/passes.h`: pass layout, compute / fullscreen pipelines, transient images): docs/research/injection.md. A feature that
takes over engine draws registers a `d3d9::DrawReplacer` (`Device::setDrawReplacer`: skip / other shaders on the engine's
geometry / procedural geometry, at the draw's own place; `registerReplacement` builds its pipelines in the background):
docs/research/water.md 4.1.
`rvk_vktest --passes` checks the pass building blocks on the GPU with synchronization validation (`--hazard`: proves it is live).

## Architecture (src/)

| Path | Role |
|---|---|
| `proxy/` | `DllMain`, the `d3d9.dll` exports (`d3d9.def`: renamed `rvk_*` functions; forward to the system d3d9 loaded lazily) |
| `api/` + `include/rvk_api.h` | exports for tools (risen-spass): `rvk_screenshot` queue + writer thread, `rvk_stats`, path guard (never into `data`), `rvk_setting_*` / `rvk_ab` / `rvk_overlay` |
| `bootstrap.cpp` | config, log, engine symbols, hooks (in DllMain: Engine.dll is mapped but not initialised, never call into it there) |
| `core/` | config (`bin\rvk\config.toml`), log (`bin\rvk\rvk.log`), paths, MinHook wrapper (`Hook<Fn>`), export resolution; `settings.*`: the player settings registry (`bin\rvk\settings.toml`, A/B; new features register their settings here, with capability requirements); `caps.*`: the capability report |
| `overlay/` | in-game Dear ImGui overlay (settings, A/B, perf panel), window-procedure input filter, `[debug] commands`; ImGui's Vulkan backend is vendored in `third_party/imgui_vulkan` (compiled against volk) |
| `engine/symbols.h` | X-macro table of the Engine.dll symbols we bind by decorated name |
| `engine/scene_hooks.*` | frame phase markers (Vulkan mode only): detours on Execute / z pass / shadow / solid / alpha / sky / post / GUI, the RenderRefracted vtable slot, image-role tags, the main camera read (docs/research/frame-phases.md) |
| `vk/` | Vulkan core: context (floor), swapchain, frames + upload ring, bindless set, images, pipeline cache, formats; `passes.*` = pass pipeline layout, compute / fullscreen pipelines, transient images; `raytracing.*` = optional RT tier: AS builds, ray query self-test |
| `d3d9/` | D3D9 device model on Vulkan (states, resources, draws, present): what the HAL and effects drive; `rvk_d3d9test`; `scene.*` = frame semantics (phase tracker, image roles, camera block); `draws.*` draw tags, records, tint palette; `identity.*` resource ids + generations; `semantics.*` CTAB shader semantics (draw-tags.md); `inject.*` injection points (injection.md) |
| `hal/vulkan/` | the engine HAL on `d3d9::Device` (mode = "vulkan"): instance, device, resources, HLSL shaders, effects |
| `fx/` | fx_2_0 effect binaries: parser, constant tables, preshaders (`rvk_fxtool`) |
| `res/` | DDS parsing |
| `hal/` | generated HAL interfaces (`tools/gen_hal.py` -> `api_generated.h`, `trace_generated.h`), engine allocator, trace mode |
| `shader/` | D3D9 bytecode decoder, listing, GLSL generator (`abi.h` = shader/renderer contract; variant hooks `GlslOptions`), glslang wrapper, `embedded.h` (build-time shaders) |
| `enh/` | enhancements: `debug_view.*` = `debug.inject_view` (an injected pass, docs/research/injection.md 5); `water.*` = the enhanced water (a `d3d9::DrawReplacer`: engine water draws drawn with `shaders/water/*`, the sea's wave grid), `shore_field.*` (coast distance field bake), `water_profile.*` (look per body of water): docs/research/water.md |

## Tooling gotchas

- The Bash tool mangles backslash escapes even in quoted heredocs (a backslash-digit becomes a control character,
  backslash-n inside Python strings breaks lines): write files with the Write tool and edit with Edit when the text
  contains backslashes.

## Reverse engineering

- Exports with RVAs: `%TEMP%\risen_re\Engine.demangled.txt` (decorated names: `Engine.exports.txt`). Bind by decorated name.
- Headless Ghidra: see risen-spass `.claude/skills/decompile/SKILL.md` (Ghidra `B:\D\ghidra_12.0_PUBLIC`, projects in
  `B:\DevDrive\risen-ghidra`, `JAVA_HOME=C:\Users\johnh\scoop\apps\openjdk21\current`; never re-import or delete lock files).
  Graphics RE material: `B:\DevDrive\risen-gfx-re\` (decompiled DX9 layer `dx9_layer.txt`, frame `frame.txt`, `py\vt.py <vtable rva>`
  prints a vtable with demangled slot names).
- Many tiny exported getters are COMDAT-folded (one address, several names): never detour a 1-3 instruction export.
- The PGO build devirtualizes speculatively (`if vtable == eCDX9Device::vftable { inlined D3D9 code }`): replacement objects must
  have **their own vtables**; never patch the `eCDX9*` vtables in place.
