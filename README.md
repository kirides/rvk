# risen-enhanced-graphics

A pure Vulkan renderer for **Risen Enhanced** (64-bit). Work in progress: see [docs/PLAN.md](docs/PLAN.md).

It is a `d3d9.dll` placed next to `Risen.exe` that replaces the engine's graphics layer with a Vulkan implementation
(dynamic rendering, Vulkan 1.3).

## Build

Requirements: Visual Studio 2026 (C++ workload), CMake >= 3.25, [vcpkg](https://github.com/microsoft/vcpkg) with `VCPKG_ROOT` set.

```powershell
cmake --preset default
cmake --build build --config RelWithDebInfo
```

## Install

Nightly build: [releases/nightly](https://github.com/kirides/rvk/releases/tag/nightly), `risen-enhanced-graphics.zip`:
unpack it into `<Risen>\bin` (`d3d9.dll` + `rvk\config.toml` + `rvk\README.txt`). In game, F11 opens the settings overlay.

From a local build: `.\scripts\package.ps1` packs the same zip from `build\RelWithDebInfo` into `out\package`.
Start configuration: `<Risen>\bin\rvk\config.toml` ([package/rvk/config.toml](package/rvk/config.toml)), player
settings: `rvk\settings.toml`, log: `rvk\rvk.log`.

## Credits

- High fps gameplay fixes (falling, climbing, thunder; setting "High Fps compatibility (Vaana)" in the overlay's System
  tab): ported from [VaanaCZ's Risen FPS Patch](https://github.com/VaanaCZ/Risen-FPS-Patch) (VaanaCZ and Auronen).
