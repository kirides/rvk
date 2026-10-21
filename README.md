# risen-enhanced-graphics

A pure Vulkan renderer for **Risen Enhanced** (64-bit). Work in progress: see [docs/PLAN.md](docs/PLAN.md).

It is a `d3d9.dll` placed next to `Risen.exe` that replaces the engine's graphics layer with a Vulkan implementation
(dynamic rendering, Vulkan 1.3). Today (milestone M0) it only forwards to the system D3D9 and probes Vulkan.

## Build

Requirements: Visual Studio 2026 (C++ workload), CMake >= 3.25, [vcpkg](https://github.com/microsoft/vcpkg) with `VCPKG_ROOT` set.

```powershell
cmake --preset default
cmake --build build --config RelWithDebInfo
```

## Install

Copy `build\RelWithDebInfo\d3d9.dll` into `<Risen>\bin`. Settings: `<Risen>\bin\rvk\config.toml`, log: `<Risen>\bin\rvk\rvk.log`.

```toml
[renderer]
mode = "passthrough"   # passthrough (the game's D3D9 renderer) | trace | vulkan (not implemented yet)
[log]
level = "info"
[vulkan]
validation = false
```
