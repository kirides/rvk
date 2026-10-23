# risen-enhanced-graphics

A Vulkan renderer for **Risen Enhanced** (64-bit): a `d3d9.dll` next to `Risen.exe` that replaces the engine's graphics layer
(Vulkan 1.3, dynamic rendering). Work in progress, see [docs/PLAN.md](docs/PLAN.md).

[Image Comparison](https://www.diffchecker.com/image-compare/7Ttb27k6/)

![RVK-RT-image](docs/images/rvk_enhanced_rt.jpg)
![RVK-Original](docs/images/rvk_original.jpg)

## Install

Download `risen-enhanced-graphics.zip` from the [nightly release](https://github.com/kirides/rvk/releases/tag/nightly) and unpack
it into `<Risen>\bin` (`d3d9.dll`, `rvk\config.toml`, `rvk\README.txt`). In game, F11 opens the settings overlay.

Files in `<Risen>\bin\rvk\`: `config.toml` (start configuration), `settings.toml` (player settings), `rvk.log`.

## Build

Visual Studio 2026 (C++ workload), CMake >= 3.25, [vcpkg](https://github.com/microsoft/vcpkg) with `VCPKG_ROOT` set.

```powershell
cmake --preset default
cmake --build build --config RelWithDebInfo
.\scripts\package.ps1      # optional: the release zip, into out\package
```

## Credits

- High fps gameplay fixes (falling, climbing, thunder; setting "High Fps compatibility (Vaana)" in the overlay's System
  tab): ported from [VaanaCZ's Risen FPS Patch](https://github.com/VaanaCZ/Risen-FPS-Patch) (VaanaCZ and Auronen).
