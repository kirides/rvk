# Pedantic clang-cl builds (clang-pedantic / clang-ubsan)

Purpose: catch UB-class bugs and clang noise that MSVC /W4 does not report. MSVC stays the shipping compiler; none of this is in
the normal build or CI.

## How to run

From a VS x64 developer shell (clang-cl + Ninja; `VCPKG_ROOT` set):

```powershell
cmake --preset clang-pedantic
cmake --build build-clang-pedantic --config RelWithDebInfo -- -k 0      # full build ~5 min on 12 jobs; the goal is 0 warnings
build-clang-pedantic\RelWithDebInfo\rvk_tests.exe

cmake --preset clang-ubsan                                                # -fsanitize=undefined, trap mode
cmake --build build-clang-ubsan --config RelWithDebInfo --target rvk_tests rvk_d3d9test rvk_d3d9
build-clang-ubsan\RelWithDebInfo\rvk_tests.exe ; build-clang-ubsan\RelWithDebInfo\rvk_d3d9test.exe [--backend d3d12]
```

`rvk_d3d9` (the d3d9.dll proxy) must be built next to `rvk_d3d9test.exe`: D3DX's shader assembler asks `d3d9.dll` for caps and
rejects the tests' ps_3_0 `mad oC0, v0, c220, c221` ("2 different constant registers") when it gets the system one.

## Flags (`RVK_PEDANTIC`, `rvk_warnings()` in CMakeLists.txt; clang-cl only, MSVC unchanged)

- `-Wall -Wextra -Wpedantic` + the UB-class set: `-Wuninitialized -Wsometimes-uninitialized -Wconditional-uninitialized -Wcast-align
  -Wshadow-uncaptured-local -Wimplicit-fallthrough -Wformat=2 -Wnull-dereference -Wundefined-reinterpret-cast
  -Warray-bounds-pointer-arithmetic -Wunreachable-code-aggressive -Wshorten-64-to-32`.
- **Every `-W` goes through `/clang:`**: clang-cl's own `-Wall` / `/Wall` means `-Weverything` (54,000 warnings, mostly `-Wpadded` and
  `-Wunsafe-buffer-usage`), and `/clang:` options are appended last, so the `-Wno-` ones win over `-Wextra`.
- vcpkg headers are system includes (`/imsvc`): no warnings from spdlog / imgui / tomlplusplus / volk.
- Switched off, with the reason:
  - `-Wmissing-field-initializers`, `-Wmissing-designated-field-initializers` (511 sites): Vulkan / D3D structs are brace-initialised
    with the fields that matter, the rest is zero (the settings registry's `Spec{...}` too).
  - `-Wlanguage-extension-token` (99): `__try` (SEH around engine reads) and `__uuidof` (`IID_PPV_ARGS`) are intended MSVC extensions.
  - `-Wunknown-pragmas` (AMD FidelityFX headers' MSVC pragmas), `-Wunused-command-line-argument` (`/Zc:preprocessor` and friends),
    `-Wc++98-compat*`, `-Wnonportable-system-include-path`.
  - `-D_CRT_SECURE_NO_WARNINGS`: the `getenv` / `strncpy` / `_wfopen` "deprecated" advice (MSVC builds see C4996 for the same).
  - `third_party/imgui_dx12/imgui_impl_dx12.cpp` is compiled with `-w` here (vendored; three set-but-unused `hr`).
- Tried and not enabled: `-Wconversion` family. On this tree it reports 288 `-Wsign-conversion` (index / size arithmetic), 41
  `-Wimplicit-int-float-conversion` (uint64 nanosecond counters to double in stat lines, one int to float in overlay / tests) and 3
  `-Wshorten-64-to-32` (fixed, enabled). The first two are not worth 330 casts; run `-DCMAKE_CXX_FLAGS="/clang:-Wconversion /clang:-Wsign-conversion"`
  on a scratch build dir to look again.

## Result

| | warnings |
|---|---|
| first run (flags as asked, `-Wall` as clang-cl spells it) | 54,279 (-Weverything) |
| after `/clang:` routing, before triage | 668 unique (325 missing-field-initializers, 186 missing-designated-field-initializers, 99 language-extension-token, 21 deprecated-declarations, 6 unknown-pragmas, 6 unused-variable, 5 unused-private-field, 5 unused-but-set-variable, 5 macro-redefined, 3 unused-const-variable, 2 unused-parameter, 2 unused-function, 2 format-nonliteral, 1 deprecated-volatile) |
| now | 0 |

Real bugs / UB: none found at runtime. Zero hits of `-Wuninitialized`, `-Wsometimes-uninitialized`, `-Wconditional-uninitialized`,
`-Wcast-align`, `-Wimplicit-fallthrough`, `-Wnull-dereference`, `-Wformat=2` (apart from the two intended runtime formats),
`-Wundefined-reinterpret-cast`, `-Warray-bounds-pointer-arithmetic`, `-Wunreachable-code-aggressive`, sign-compare.

One clang-only hard error (MSVC accepts it): `src/native/f10_request.cpp` cast vtable slots (`const void*` elements) to function
pointers with `reinterpret_cast`, which clang rejects as casting away const (6 sites). Now `slotFn<Fn>(slot)` (a `const_cast` to
`void*` first): same code generated, no behaviour change.

Fixed (trivially safe, no behaviour change): `-Wdeprecated-volatile` in `vk/crash_markers.cpp` (`mapped_[0] = mapped_[1] = 0` on volatile
memory: two statements); redefined `WIN32_LEAN_AND_MEAN` / `NOMINMAX` (already defined for every target by CMake) in
`native/own_post_groups.cpp`, `shader/hlsl.cpp`, `tools/vktest/main.cpp`; unused variables / functions / parameters removed or
unnamed (`g_completeStamp`, `g_requestNs`, `g_tot`, `entity` / `res` in `static_ops_deps.cpp` / `static_ops_records.cpp`,
`rd` in `static_ops_veg_owned.cpp`, `bytesOf` in `fx_tests.cpp`, `probes` in `rhitest/parity.cpp`); five never-read private members
removed (`FrameGen::lastLogSwapchain_`, `MaterialRt::glassOn_`, `RtScene::lastBoxed_` / `proxyJobMs_`, `WindowTables::lastMatDraws_`);
engine layout constants kept as documentation with `[[maybe_unused]]` (`kSamplerCount`, `kPackedStride`, `kViewSlotTreeCascade0`);
`-Wshorten-64-to-32` in `shader/hlsl.cpp` (varying filler location: explicit `uint32_t` casts of values that are already >= 0).
Suppressed narrowly: `-Wformat-nonliteral` in `overlay/overlay_ui.cpp` (the setting spec's own printf format, push / pop pragma).
Noted, not changed: `dxil_check.inl` `cmdDxilUber` parses `--half` but ignores it (the 16-bit build exists in `uber-spv`); marked
`[[maybe_unused]]`.

## UBSan

`-fsanitize=undefined` with clang-cl on Windows: the standalone runtime (`clang_rt.ubsan_standalone-x86_64.lib`) is built `/MT` and
lld-link refuses it against this project's `/MD` (RuntimeLibrary mismatch), so the preset uses **trap mode**
(`-fsanitize-trap=undefined`: a failed check is a `ud2`, STATUS_ILLEGAL_INSTRUCTION; find the site with a debugger / WER dump).
`vptr` (needs the runtime) and `function` (reads the bytes before the callee; the tests call hand-made machine-code stubs through typed
pointers) are off.

Result: `rvk_tests` 351 / 351, `rvk_d3d9test` (Vulkan and `--backend d3d12`) PASS with 0 validation errors, no check fired (shift,
signed overflow, alignment, null, bounds, bool / enum, unreachable, return, VLA, ... all clean on what these tests execute).
Game-path code that only runs in the game (hooks, engine reads) is not exercised by them.
