# risen-enhanced-graphics: guidance for Claude

A **pure Vulkan renderer for Risen Enhanced (x64)**. It ships as a `d3d9.dll` proxy in `<Risen>\bin` (Engine.dll is the only
importer of d3d9, and only of `Direct3DCreate9`). It replaces the engine's own graphics HAL (`eCAPIInstance` / `eCAPIDevice` and
the `eCAPI*` resource interfaces) with a Vulkan implementation; no D3D9 device is created in Vulkan mode.

Read first: `docs/PLAN.md` (decisions, milestones), `docs/research/engine-gfx.md` (the engine's HAL, frame, shaders, formats, RVAs),
`docs/research/spass.md` (loading chain, test tooling of risen-spass), `docs/research/gd3d11.md` (GD3D11's Vulkan backend, reuse), `docs/research/dxvk.md` (DXVK techniques: applied, candidates).

## Hard rules

1. **Never touch the real game install** `B:\SteamLibrary\steamapps\common\Risen` (nor Risen Classic 2009). Test only in the copy
   `B:\DevDrive\rvk-work\copies\risen-test-vk`, **always windowed** (only `run.ps1 -Fullscreen` tests the borderless path, in short runs). Careful: the copy's `data\common` and `data\compiled` are **junctions into the
   real install**; never write through them (the engine writes `Data/Compiled/Materials/ShaderMaterialPool_User.smp` on shader
   pool misses: keep the pool hits intact / redirect such writes before shaders change; a Shader Quality below High
   (Options > Performance, config `/Engine/Render/Shader/Quality`) makes every material miss the High-only Master pool → runtime
   compiles + writes to `ShaderMaterialPool_User.smp` (materials.md)).
2. **Vulkan floor**: Vulkan 1.3 core + `VK_KHR_push_descriptor`; everything else optional with a fallback (PLAN.md section 1).
   **Dynamic rendering only**: never `VkRenderPass` / `VkFramebuffer`.
3. **Commit at milestones**: small, reviewable commits once a feature is built *and* verified (in the game where it touches the
   engine). Commit trailers come from the session's attribution reminder.
4. **Scratch lives under B:\DevDrive\rvk-work only** (`copies\` = game test copies, `scratch\` = logs, scratch scripts, pictures, temp dirs; never the root of `B:\DevDrive`). One definition: `scripts/lib/rvk-work.ps1` (`$RvkWork`, env override `RVK_WORK`; `Resolve-RvkGameDir` maps old `B:\DevDrive\risen-test-vk*` paths to `copies\` and falls back to the old place with a warning while a copy has not been moved). The user deletes `rvk-work` in one sweep; `rvk-work\MANIFEST.md` lists what was moved there from the old root layout.
5. Verify engine behaviour in the running game instead of assuming. Record findings (RVAs, layouts, confirmed vs inferred) in
   `docs/research/engine-gfx.md` (or a new `docs/research/*.md`), update `docs/PLAN.md` milestones.

## Build

```powershell
$env:VCPKG_ROOT = 'E:\git\github\vcpkg'
cmake --preset default                                  # only after CMakeLists.txt changed (new files!)
cmake --build build --config RelWithDebInfo             # -> build\RelWithDebInfo\d3d9.dll, rvk_tests.exe
build\RelWithDebInfo\rvk_tests.exe                      # unit tests (pure logic, no game)
build\RelWithDebInfo\rvk_vktest.exe --rt                 # GPU: optional RT tier (BLAS/TLAS/ray query self-test; also --passes, --shader-stats)
```
C++23, MSVC (VS 18 2026), CMake + vcpkg manifest (`vcpkg.json`, `builtin-baseline` = the user's vcpkg checkout; when they pull,
take `git -C E:\git\github\vcpkg log -1 --format=%H`). Triplet `x64-windows-static-md`. New source files go into `CMakeLists.txt`.
volk is compiled in `src/vk/volk_impl.c` (`VK_NO_PROTOTYPES`, `VK_USE_PLATFORM_WIN32_KHR`), not vcpkg's `volk.lib`.
**Pedantic clang-cl build** (docs/research/clang-pedantic.md; run from time to time, from a VS x64 developer shell): `cmake --preset clang-pedantic; cmake --build build-clang-pedantic --config RelWithDebInfo` (`RVK_PEDANTIC=ON`: -Wall -Wextra -Wpedantic + UB-class warnings, optimised; must stay at 0 warnings, new code fixes its warnings or suppresses them narrowly); `--preset clang-ubsan` (`RVK_UBSAN=ON`, trap mode) builds `rvk_tests` / `rvk_d3d9test` / `rvk_d3d9` with -fsanitize=undefined (a failed check = ud2 crash). MSVC builds are unchanged.
The editor's clang diagnostics ("file not found", "undeclared identifier std") are noise: only the MSBuild result counts.
`rvk_core` has a private PCH (std + spdlog): files outside it (the DLL target, tests) must include what they use.
The Vulkan SDK (`C:\VulkanSDK\1.4.363.0`, `VULKAN_SDK`) provides validation layers, `spirv-val`, `dxc`; the vcpkg `directx-dxc` port provides the `dxcompiler.dll` the renderer loads.
CI (`.github/workflows/build.yml`, setup as in risen-spass): `cmake --preset public-release -DRVK_PCH=OFF` + ccache (rvk_core's
PCH headers force-included instead; preset "Public Release" = `ninja` into `build-release` linked with `/INCREMENTAL:NO
/OPT:REF /OPT:ICF`: the shipped d3d9.dll / d3d9.pdb; the local VS build stays incremental), d3d9.dll + rvk_tests; the nightly release (tag `nightly`, 03:00 UTC or manual dispatch on
main) ships `risen-enhanced-graphics.zip` = `scripts/package.ps1`: d3d9.dll + `package/rvk/config.toml` (mode vulkan) +
`package/rvk/README.txt` (player notes: keep it short; the build line is appended) + d3d9.pdb as a separate asset.

## Test loop

```powershell
.\scripts\run.ps1 -Mods testdemo -Scenario testdemo -WaitForResult -Seconds 240      # mode original (default)
.\scripts\run.ps1 -Mode wrapper ...    # -Mode full, -Mode trace, -Validation, -RvkLogLevel debug, -NoRenderer (reference D3D9 run)
```
Render modes (docs/research/overlay.md 2.7; player setting `renderer.mode`, System tab, default wrapper): `-Mode original`
(system d3d9 forwarded, no Vulkan; old name `passthrough`), `-Mode wrapper` (the D3D9 model on Vulkan + enhancements; old name
`vulkan`), `-Mode full` (the native frame takes over every draw: `native.static_ops` + `native.scene_ops`, all groups);
`trace` = developer. `original` / `full` in config.toml lock the setting; with `-Mode wrapper` it switches at run time
(`-Commands '2000: set renderer.mode full'`, `-Setting 'renderer.mode=full'`, A/B F10). rvk.log `renderer: mode ...`.

**Area references** (moved out of this file; read the one for the area you work on before testing or changing it):

| file | covers |
|---|---|
| `docs/claude/testing.md` | `run.ps1` debugging switches (`-Screenshots`, `-LogDraws`, `-LogFrame`, `-Snapshots`, `-Validation`, tint / inject views), performance (`-Perf`, `-PerfFrame`, **`perf-ab.ps1`**, `-CpuProfile`, savegame cost, CPU overhead switches), the 80 fps Radeon Chill cap / `-Exe RisenTest.exe`, menu fps, player settings + test baseline + overlay commands (`-Commands`, `key`, `click`), engine debug keys, window / display / exclusive fullscreen modes, capture regions + device-loss diagnostics, settings window, streaming / loading, integrated GPU preset + low bandwidth, real shutdown path (`-GracefulExit`) |
| `docs/claude/image.md` | detail distances (`lod.*`), frame generation (`framegen.*`), upscaling / native AA / FSR (`upscale.*`), jitter + depth, nets |
| `docs/claude/lighting-rt.md` | enhanced shadows, RT sun shadows, RT tier + RT scene + RT LOD preset, RT materials, GI (DDGI, screen probes, path-traced reference), material pack + derived gloss, traced material reflections + rain + glass, G-buffer / GTAO, table / clustered lighting + lamp shadows |
| `docs/claude/water.md` | enhanced water, FFT ocean + foam, RT water reflections, ocean mip chains, water scenarios / spots |
| `docs/claude/d3d12.md` | D3D12 backend, its diagnostics, Agility SDK, DRED / crash dumps |
| `docs/claude/full-mode.md` | native frame (`native.static_ops` / `scene_ops` groups, lean, packet cache), S-9 defaults, every frame-ownership stage (owned_* / clusters / cull / vertex pull / bone + rigid + wind motion / ordered / RT records / GPU RT build / own post, sky, gui, particles, decals), **how to A/B an owned stage** (`native: stages` line, `Assert-ArmStages`, picture check), frame memory epochs |

Key rules from those files (the details are there):
- A/B a run-time setting inside ONE session per scene with `scripts/perf-ab.ps1` (restart settings: `-Restart`); fps / CPU runs use `-Exe RisenTest.exe` (the `Risen.exe` 80 fps cap is Radeon Chill).
- An A/B arm in Full counts only when rvk.log's `native: stages (5 s)` line shows the claimed state (`scripts/lib/stage-state.ps1` `Assert-ArmStages`); new owned stages are checked with `native.picture_check` (FAILED must never appear in a settled window); `DIFFERING` / `MISS` counters must stay 0.
- Off = nothing exists for an optional feature (no pipelines / passes / images); every new setting goes through the settings registry (`core/settings.*`) with its `Spec::needs` / requirements; removed keys stay removed (`kRemovedSettings`, constants in `namespace frozen`).
- Every run writes the test baseline `bin/rvk/settings.toml` (`-PlayerDefaults` = shipped defaults, `-KeepSettings` keeps the file); `-Setting k=v` takes lowercase `true` / `false`.
- Full mode FAILs `monastery_hall`, `crystal_cave`, `pub_interior` in refcompare by design (table lighting); judge against Wrapper.
- Never read the frame ring back on the CPU (write-combined); per-frame history keys on `GameDevice::memoryEpoch`, not `frameValue() + 1`.

**GPU safety in test runs** (a device loss = a driver timeout on the user's desktop): new code that handles GPU addresses needs CPU null guards, an `rvk_d3d9test` / `rvk_rhitest` case through the same path run with `--validation`, and a first short game run with `-Validation -Environment 'VK_KHRONOS_VALIDATION_VALIDATE_GPU_BASED=GPU_BASED_GPU_ASSISTED'` before any longer or unvalidated run.

**Several agents / worktrees share one GPU** (and the default test copy): a machine-wide mutex guards the games (two games at once on one copy would overwrite each other's `d3d9.dll`). Correctness runs use a **private copy + instance + lock name**: `.\scripts\with-test-lock.ps1 -Name rvk-<task> -Who <name> -Run { .\scripts\run.ps1 -GameDir B:\DevDrive\rvk-work\copies\risen-test-vk-<task> -Instance N ...; copy rvk.log out inside the block }`; only perf timing (fps / CPU intervals) uses the default shared lock and copy (`with-test-lock.ps1 -Who <name> -Run { .\scripts\run.ps1 ... }`); hold a lock only for the run itself. GPU timestamps (`-PerfFrame`) are not disturbed by another agent's build, fps / CPU intervals are: repeat or compare inside one window. `run.ps1` escapes backslashes in `-Environment` / `-Commands` / `-Setting` strings (forward slashes for `RVK_SHADER_DIR` are no longer required, still harmless).
**Instance ids** (risen-spass renames the engine's single-instance mutex to `GenomeMutex_<id>`, id = max(`-Instance`, 1)): a game started while ANY other Risen holds the same id quits ~0.2 s after "hooks active", before Lua ("the game exits at once"; two copies run without `-Instance` both take id 1). `run.ps1` checks the mutex first: an explicit `-Instance` that is taken throws and names the holder; without `-Instance` it takes a free id 2..40 (warning `instance id 1 is taken by ...`).

`run.ps1` creates the test copy on first use (`scripts/new-test-copy.ps1`, from risen-spass's `B:\DevDrive\risen-test`), deploys
`d3d9.dll` + `bin\rvk\config.toml`, and drives the game with **risen-spass's `scripts/test-run.ps1 -GameDir`** (windowed, muted,
watchdog, scenario result; mods come from this repo's `mods/` (`-ModPath`), then the spass repo `B:\DevDrive\git\github\risen-spass\mods`). Our log:
`B:\DevDrive\rvk-work\copies\risen-test-vk\bin\rvk\rvk.log`. Check free RAM first; kill a `steamwebhelper.exe` above 5 GiB (user instruction).
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
run in the dedicated copy `B:\DevDrive\rvk-work\copies\risen-test-vk-cmp`, instance 3, pinned ConfigUser.xml; report
`out\refcompare\<stamp>\report\report.html`). `-Calibrate` = two D3D9 runs -> a reusable reference (thresholds + known-noise
masks); `-ReferenceDir out\refcompare\<stamp>` = only the Vulkan run against it (current stored reference:
`out\refcompare\20261022-093252`, 20 spots); `-Spots a,b`; `-Setting k=v` (Vulkan run's
player settings, e.g. `rt.allow=false`). Renderer scenario mods live in
this repo's `mods/` (never in risen-spass). Details, spot list, noise handling: docs/research/reference-compare.md 5, 7, 8, 11.
**Full mode expected differences** (user decision 2026-11-05, reference-compare.md 11.2): the lighting table makes Full show more lamp light, so `refcompare` Full FAILs `monastery_hall`, `crystal_cave`, `pub_interior` by design (judge against Wrapper, or `-Setting 'lighting.clustered=false'` for the old pass).

Startup facts (measured, docs/research/early-d3d9.md): `eCApplication::CreateEngine` calls `eCGfxAdmin::sEnumAdapterDesc`
(pixel shader 3.0 check) before the window: in passthrough/trace it runs `Direct3DCreate9` + DxDiag (5.7-7.4 s, loads
d3d9/dxdiagn/d3d11/d3d12), then `InitializeAdmin` calls `Direct3DCreate9` again; device creation in the AMD D3D9 driver takes
~10 s. In Vulkan mode our detour answers it from Vulkan (~0.15 s) and no system d3d9.dll is loaded. rvk.log's "startup state
at InitializeAdmin" line lists uptime, D3D/Vulkan/overlay modules and the process's windows. The hidden
`temp_d3d_window_4039785` window is OBS Studio's `graphics-hook64.dll`, loaded as the implicit Vulkan layer
`VK_LAYER_OBS_HOOK` by our first `vkCreateInstance` (`DISABLE_VULKAN_OBS_CAPTURE=1` disables the layer).

## Shader translator

The renderer translates D3D9 bytecode to **HLSL** (`src/shader/hlsl.cpp`, `toHlsl` + `compileHlsl` -> DXC; docs/research/hlsl-rhi.md; `kTranslatorVersion` in `d3d9/device.cpp`
is only a label of the variant tags now: the module caches are keyed by the generated text, see below): the output is `#include "rvk_draw.hlsli"` + structs `RvkVsIn / RvkVsOut / RvkPsIn / RvkPsOut` (explicit `[[vk::location]]` for
varyings, `SV_Target<n>` for pixel outputs: no explicit location there, DXC rejects a partial assignment) + static register globals + `VSMain` / `PSMain`. The variant hooks
(`TranslateOptions::prelude / prologue / epilogue / extraGlobals / constOverrides / inputOverrides / dotCaptures`, the HLSL-only `extraOutputs` / `extraInputs` = members of
`rvk_out` / `rvk_in`; `shader/translate.h`) are HLSL text (enh/material_variant, enh/rt_scene captureOptions, Device::lodModule).
`rvk_shadertool corpus <dir>` translates + compiles (DXC, `RVK_DXC_OPT` -O level, default 1) + `spirv-val`s every captured shader (corpus from `mode = "trace"`:
`<test copy>/bin/rvk/corpus`, copy it to `<repo>/corpus`, git-ignored: game-derived; ~2.5 min for 2483 shaders: DXC costs ~40-60 ms per shader, so a cold SPIR-V cache costs ~3 CPU minutes on the first run: `Device::createShader` returns at once, the
compile runs on a worker pool (hardware threads - 2), a draw whose shader still compiles is skipped and promotes it in the queue (`hal: frame` field `compiling`); the enhancement / scene variants have their own workers (hardware threads - 4, 3..12, below normal priority, `RVK_VARIANT_THREADS=n`; a header change makes every variant cold: ~900 on Vulkan with FSR, owned shares stay low until the 1 s line's `variant queue` reaches 0, so share gates need one warm-up run per backend; frame-ownership.md 14.27); every generated module (translated, ubershader, variants; SPIR-V and DXIL) goes through the DXC disk cache `bin\rvk\cache\hlsl`, keyed by the generated text + the DXC arguments (-O, half, debug) + DXC's version, each include's text hash checked on a hit (WS12, frame-ownership.md 14.52: a translator / header change that leaves a module's text alone keeps it, one that changes it recompiles exactly that module; a shader's identity, `Shader::hash` + `pipelines.keys`, is its bytecode alone, so the keys of the last session name what to build after an update; `bin\rvk\cache\spirv` of earlier builds is unused and can be deleted); the first minutes of a session (cold / updated / warm caches, Full vs Wrapper): `scripts/first-minutes.ps1` + `tools/firstminutes.py`), with
`--tint | --gi | --gbuf | --gbuf-motion | --capture` the enhancement variants; `corpus` / `dxil-check` run on hardware threads - 2 (`-j N`), `--cache <dir>` = dxc's disk cache across runs (an unchanged `dxil-check uber` repeat 4 s instead of ~4 min), dxil-check compiles identical requests once (`--no-memo`). `show <file>` prints listing + HLSL, `refdisasm <file>` the D3DX reference listing,
`hlsl <name> [-D X=Y] [-o out.spv] [--dxil out.dxil]` compiles one embedded / `RVK_SHADER_DIR` shader (or an exported file on disk) like the renderer.
`python tools/compare_disasm.py corpus` diffs our listing against D3DX for the whole corpus (must stay 100 %). `rvk_shadertool export corpus game-shaders` writes the game's
shaders as plain HLSL (`<name>.vs.hlsl` / `.ps.hlsl`: the translator's exact output, D3D9 listing + CTAB names in comments, `include/` = the ABI headers; git-ignored,
game-derived). The contract between translated shaders and the renderer is `src/shader/abi.h`; its HLSL side is `shaders/include/rvk_abi.hlsli` / `rvk_draw.hlsli` (change both
together; rvk_tests `abi_hlsl_*_matches_cpp` compare the offsets of the compiled SPIR-V). **The game's material shaders are one HLSL ubershader with `XFX_*` defines**
(docs/research/shader-permutations.md): `shaders/material/ge_default.{hlsli,frag.hlsl,vert.hlsl}` is the engine's own HLSL ported to this ABI (same defines; the engine's `XFX_PS_*`
expressions are valid HLSL as they are). `python tools/ubershader_check.py out/matsrc/*.jsonl` proves the pixel shader against the translated game shaders on the GPU
(`rvk_shadereq`; needs `game-shaders` from the export above: 183 of 183 captured PS_Default / PS_DefaultZPass requests EQUAL);
`tools/hlsl_infer.py` / `tools/hlsl_match.py` reproduce a corpus shader from the engine's HLSL + defines instruction for instruction (`out/hlsl-fx`, the pool was compiled with
`d3dx9_29.dll`, flags 0, `half3 vecWorldEye`). **HLSL material shader at run time** (shader-permutations.md 8-10): `-Setting 'debug.material_sources=true'` writes the engine's
request per material shader (`bin/rvk/cache/materials/sources.jsonl`; copy to `out/matsrc/`); `-Setting 'debug.ubershader=true'` (default off; `debug.ubershader_skip` = bit mask of
classes kept translated, bit 256 = the vertex shaders) uses it at run time, the vertex shaders (`ge_default.vert.hlsl`, S-1b, frame-ownership.md 14.9) and the leaf pixel shaders included; `python tools/vs_equiv.py out/matsrc/*.jsonl` proves the vertex port on the GPU (52 of 52 EQUAL).

## Hand-written shaders (HLSL, compiled at run time by DXC)

Enhancement / pass shaders are **HLSL (SM 6.6) in `shaders/`** (docs/research/hlsl-rhi.md; user decision 2026-10-29, GD3D11's scheme: the
vcpkg `directx-dxc` port's `dxcompiler.dll` is loaded at run time by `shader/dxc.*`, next to `d3d9.dll` in `bin\rvk`, with a disk cache under
`bin\rvk\cache\hlsl`). `shader::embedded::spirv("water/water.frag")` compiles `water/water.frag.hlsl` (entries `CSMain` / `VSMain` / `PSMain` by stage extension; SPIR-V entry renamed `main`;
`spirvVariant(name, "A=1;B=2")` for one module per size / mode). There is no GLSL in the repo any more (removed once the in-game A/B of the ports, GLSL vs HLSL, matched within the
GLSL-vs-GLSL noise: git history has the originals). HLSL files are picked up by a glob (re-run `cmake --preset default` after adding one); `rvk_embed` embeds them as text,
`dxcompiler.dll` compiles them on first use (`-Zi -Od` + names with `[debug] shader_debug_info` / `run.ps1 -ShaderDebug`). `#include "rvk_common.hlsli"` (-> `rvk_abi.hlsli`: push constants
`RVK_PUSH`, pointers `RVK_REF`, `RvkFrame`, the bindless helpers `rvkSample2D...` over `RVK_TEX2D / RVK_SAMP`); pass shaders start their push block with `RVK_PASS_PUSH_HEADER`; pass
resources are `register(u<binding>, space<set>)` with the C++ layout's numbers. ABI structs are float4 / uint4 only; matrices are rows (`mul(float4(world, 1), f.viewProj)`).
New shader: add `foo.comp.hlsl` (no CMake list), `cmake --preset default`. Porting rules and pitfalls: docs/research/hlsl-porting-guide.md; `tools/port_check.py` compares an HLSL
shader's interface with a reference SPIR-V.
`RVK_SHADER_DIR=<repo>\shaders` (`run.ps1 -Environment`) reads sources from disk (no rebuild). Checks: `rvk_tests` (`dxc_*`, `abi_hlsl_layout_matches_cpp`),
`rvk_vktest --passes`; offline with the port's `dxc.exe` (`build\vcpkg_installed\x64-windows-static-md\tools\directx-dxc`) and `spirv-val`.
Passes plug in at injection points (`Device::addPass(inject::Point::AfterOpaque, ...)`, `vk/passes.h`: pass layout, compute / fullscreen pipelines,
transient images): docs/research/injection.md. A feature that takes over engine draws registers a `d3d9::DrawReplacer` (`Device::setDrawReplacer`: skip /
other shaders on the engine's geometry / procedural geometry, at the draw's own place; `registerReplacement` builds its pipelines in the background):
docs/research/water.md 4.1.

**DXIL parity (S1, docs/research/hlsl-rhi.md 9)**: every shader must also compile to signed DXIL SM 6.6 from the same text and fit the DX12 backend's fixed root signature (push constants <= 128 bytes at b0, root CBV b8 space1, storage images u0..u7 space1, everything else from the heaps). Checker: `rvk_shadertool dxil-check embedded | corpus corpus [--tint|--gi|--gbuf|--gbuf-motion|--capture] | uber out/matsrc/cap1.jsonl` (0 failures = the gate after any shader / ABI / translator change; `RVK_SHADER_DIR` for edit-and-rerun). The ABI headers hide the backend: addresses are `uint2` pairs (Vulkan device address / DX12 heap slot + offset), use `rvkPtr<T, A>(pair).Get()` (read), `rvkAddr / rvkAddrAdd / rvkRawLoad<T, align>` (raw reads), the `rvkRw*` family (buffers a shader writes: a `shaderRefRw` pair), `RVK_TEX2D / RVK_SAMP` (+ `_UNI`): never `vk::` types, `uint64_t`, arrays of `uint2` or struct members in a push block (a root-constant block packs structs and arrays to 16 bytes; `RVK_REF` only as its first member, `RvkDrawPush.frame` is a `uint2`: `rvkLoadFrame(pc.frame)`, hook text `rvkPtr<RvkFrameData, 16>(pc.frame).Get()`). Stage interfaces pair by semantic name on D3D12: `VARYn` (the translator's), not `TEXCOORDn`, wherever a translated shader meets a hand-written one. GPU equivalence of production shaders on both backends: `rvk_rhitest` (`tools/rhitest/parity.cpp`: translated draw, ubershader draws, water pixel shader, GI / RT reference loads; `RVK_PARITY_ONLY`, `RVK_PARITY_EXTRA`). `shader::embedded::dxil(name)` = the DXIL of an embedded shader. **Every block shared between C++ and HLSL (push blocks, `RVK_REF` / `rvkPtr` blocks, raw records) has one field description in `src/shader/abi_blocks.cpp`** (`BLOCK` + `F / FM / FO / PH`, the C++ side an `offsetof`): `rvk_rhitest` generates a probe compute shader per block, reads every field on Vulkan AND D3D12 and compares it with the C++ memory (`abi probe` line, built-in mutant must be detected), `rvk_tests` lints every production HLSL struct used as `RVK_PUSH` / `RVK_REF` / `rvkPtr` against the descriptions: a new such struct without a `BLOCK` entry fails (docs/research/hlsl-rhi.md 9.7).

## Architecture (src/)

| Path | Role |
|---|---|
| `proxy/` | `DllMain`, the `d3d9.dll` exports (`d3d9.def`: renamed `rvk_*` functions; forward to the system d3d9 loaded lazily) |
| `api/` + `include/rvk_api.h` | exports for tools (risen-spass): `rvk_screenshot` queue + writer thread, `rvk_stats`, path guard (never into `data`), `rvk_setting_*` / `rvk_ab` / `rvk_overlay` |
| `bootstrap.cpp` | config, log, engine symbols, hooks (in DllMain: Engine.dll is mapped but not initialised, never call into it there) |
| `core/` | config (`bin\rvk\config.toml`), log (`bin\rvk\rvk.log`), paths, MinHook wrapper (`Hook<Fn>`), export resolution; `settings.*`: the player settings registry (`bin\rvk\settings.toml`, A/B; new features register their settings here, with capability requirements); `caps.*`: the capability report |
| `overlay/` | in-game Dear ImGui overlay (settings, A/B, perf panel), window-procedure input filter, `[debug] commands`; ImGui's Vulkan backend is vendored in `third_party/imgui_vulkan` (compiled against volk) |
| `engine/symbols.h` | X-macro table of the Engine.dll symbols we bind by decorated name |
| `engine/materials.*` | the material graph of a draw's shader (diffuse / opacity / normal slots, blend mode, pool images; docs/research/rt-albedo-facts.md); `engine/memread.*` = SEH-guarded engine reads |
| `engine/window_mode.*` | borderless fullscreen: `SetResolutionEx` pre-hook (fullscreen -> monitor size), window placement at present, DPI awareness; exclusive fullscreen's display mode switch / restore and focus handling (docs/research/window-mode.md) |
| `engine/shadow_maps.*` | the sun cascades' map size / range / split factor overrides (`eCCascadeShadowMap::RenderShadowMaps` pre-hook; docs/research/shadows.md 3) |
| `engine/hemisphere.*` | post-hook of `eCHemisphere_PS::AddToLightList`: the engine's weather-driven sky SH (7 registers) + indoor colour = the GI sky and the water RT hit light (docs/research/rtgi.md 17) |
| `engine/menu_state.*` | game / menu / loading from the GUI's page predicates (Script.dll `GUI2::IsLoading` / `GetMode` / `IsVisible`): the menu frame rate limit and the unpaced loading screen (docs/research/menu-fps.md) |
| `engine/material_source.*` | hook on `CompileShaderPoolEllement`: the engine's request (file, entry, macros) per material shader attached to the `d3d9::Shader`; `debug.material_sources` JSON lines (shader-permutations.md 8) |
| `shader/uber.*` | assembles the HLSL material shader (`shaders/material/ge_default.*`) from a request's macros + variant hooks; `debug.ubershader` (shader-permutations.md 10) |
| `engine/exposure.*` | the engine's adapted-luminance image (`adaptedLuminanceApi()`: the 1x1 R32F output of the "AvgLuminance" filter, found through the scene effect's nested effects) for FSR's exposure (`upscale.exposure`; docs/research/upscaling.md 20) |
| `engine/lod.*` | the `CacheEntities` stride hook (`debug.prefetch_stride`); detail distances (SpeedTree / mesh LOD and the view range factor scaled in detours; the billboard fade variant: docs/research/lod.md) |
| `d3d9/record.cpp` | record threads: draw-packet chunks recorded into secondary command buffers by worker threads (`debug.record_threads`; performance.md 14) |
| `engine/high_fps.*` | the high fps gameplay fixes (falling, climbing, thunder; call sites redirected; docs/research/fps-limit.md 12) |
| `engine/scene_hooks.*` | frame phase markers (Vulkan mode only): detours on Execute / z pass / shadow / solid / alpha / sky / post / GUI, the RenderRefracted vtable slot, image-role tags, the main camera read (docs/research/frame-phases.md) |
| `d3d9/device.*` (regions) | `Device::regionBegin/regionEnd` / `RegionScope`, `syncPhaseRegions` (debug-utils labels follow the phase tracker; capture-regions.md) |
| `vk/` | Vulkan core: context (floor), swapchain, frames + upload ring, bindless set, images, pipeline cache, formats; `passes.*` = pass pipeline layout, compute / fullscreen pipelines, transient images; `raytracing.*` = optional RT tier: AS builds, ray query self-test; `fsr.*` = wrapper of AMD's signed FFX API loader + upscaler DLLs (FSR 3.1 / FSR 4 ML: version override, context, dispatch, layout tracking; docs/research/upscaling.md) |
| `d3d9/` | D3D9 device model on Vulkan (states, resources, draws, present): what the HAL and effects drive; `rvk_d3d9test`; `scene.*` = frame semantics (phase tracker, image roles, camera block); `draws.*` draw tags, records, tint palette; `identity.*` resource ids + generations; `semantics.*` CTAB shader semantics (draw-tags.md); `inject.*` injection points (injection.md) |
| `hal/vulkan/` | the engine HAL on `d3d9::Device` (mode = "vulkan"): instance, device, resources, HLSL shaders, effects |
| `fx/` | fx_2_0 effect binaries: parser, constant tables, preshaders (`rvk_fxtool`) |
| `res/` | DDS parsing |
| `hal/` | generated HAL interfaces (`tools/gen_hal.py` -> `api_generated.h`, `trace_generated.h`), engine allocator, trace mode |
| `shader/` | D3D9 bytecode decoder, listing, CPU evaluator + backward slice (`sm_eval.*`), HLSL generator (`hlsl.cpp`; `abi.h` = shader/renderer contract; variant hooks `TranslateOptions`, `translate.h`), the DXC wrapper (`dxc.*`: runtime compile + disk cache), `embedded.h` (the embedded HLSL sources) |
| `enh/` | enhancements: `debug_view.*` = `debug.inject_view` (an injected pass, docs/research/injection.md 5); `water.*` = the enhanced water (a `d3d9::DrawReplacer`: engine water draws drawn with `shaders/water/*`, the sea's wave grid), `shore_field.*` (coast distance field bake), `water_profile.*` (look per body of water), `water_rt.*` (rt.reflections: the reflection image's compute pass): docs/research/water.md; `shadows.*` = the enhanced sun shadow mask (a `d3d9::DrawReplacer` for the shadow-mask pass, `shaders/shadows/*`): docs/research/shadows.md; `rt_scene.*` / `rt_registry.*` = the RT scene (a `d3d9::DrawObserver` + a pass: BLAS cache, instance registry, TLAS, geometry records), `rt_material.*` = its materials (albedo slot, uv map by CPU shader evaluation, opacity), `rt_debug.*` = `rt.debug`: docs/research/rt-reflections.md; `gi.*` = RT GI (DDGI probes as the engine's ambient constants, a DrawObserver + passes, `shaders/gi/*`, `rvk_gi.glsl`): rtgi.md 9; `gbuffer.*` = G-buffer / motion / GTAO (rtgi.md 10), `material_variant.*` = the material pixel shaders' enhancement variants; `upscale.*` = `upscale.mode` / `upscale.sharpness`: camera jitter source, depth prep (`shaders/upscale/prep.comp`, exposure image `shaders/upscale/exposure.comp`), the `upscale.fsr` pass (upscaling.md); `object_motion.*` = per-object motion for FSR (a `d3d9::DrawObserver`: vertex-position captures of skinned + moving rigid draws, upscaling.md 9) |

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
