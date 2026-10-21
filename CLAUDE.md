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
stay in the background by default). rvk.log prints fps / draw counts every 5 s and every unimplemented HAL slot once.

`run.ps1` creates the test copy on first use (`scripts/new-test-copy.ps1`, from risen-spass's `B:\DevDrive\risen-test`), deploys
`d3d9.dll` + `bin\rvk\config.toml`, and drives the game with **risen-spass's `scripts/test-run.ps1 -GameDir`** (windowed, muted,
watchdog, scenario result; mods come from the spass repo `B:\DevDrive\git\github\risen-spass\mods`). Our log:
`B:\DevDrive\risen-test-vk\bin\rvk\rvk.log`. Check free RAM first; kill a `steamwebhelper.exe` above 5 GiB (user instruction).
risen-spass's own `risen.debug.screenshot` grabs the D3D9 back buffer (hooks `Present`): it does not work in Vulkan mode.

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
whole corpus (must stay 100 %). The contract between translated shaders and the renderer is `src/shader/abi.h`.

## Architecture (src/)

| Path | Role |
|---|---|
| `proxy/` | `DllMain`, the `d3d9.dll` exports (`d3d9.def`: renamed `rvk_*` functions; forward to the system d3d9 loaded lazily) |
| `api/` + `include/rvk_api.h` | exports for tools (risen-spass): `rvk_screenshot` queue + writer thread, `rvk_stats`, path guard (never into `data`) |
| `bootstrap.cpp` | config, log, engine symbols, hooks (in DllMain: Engine.dll is mapped but not initialised, never call into it there) |
| `core/` | config (`bin\rvk\config.toml`), log (`bin\rvk\rvk.log`), paths, MinHook wrapper (`Hook<Fn>`), export resolution |
| `engine/symbols.h` | X-macro table of the Engine.dll symbols we bind by decorated name |
| `vk/` | Vulkan core: context (floor), swapchain, frames + upload ring, bindless set, images, pipeline cache, formats |
| `d3d9/` | D3D9 device model on Vulkan (states, resources, draws, present): what the HAL and effects drive; `rvk_d3d9test` |
| `hal/vulkan/` | the engine HAL on `d3d9::Device` (mode = "vulkan"): instance, device, resources, HLSL shaders, effects |
| `fx/` | fx_2_0 effect binaries: parser, constant tables, preshaders (`rvk_fxtool`) |
| `res/` | DDS parsing |
| `hal/` | generated HAL interfaces (`tools/gen_hal.py` -> `api_generated.h`, `trace_generated.h`), engine allocator, trace mode |
| `shader/` | D3D9 bytecode decoder, listing, GLSL generator (`abi.h` = shader/renderer contract), glslang wrapper |

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
