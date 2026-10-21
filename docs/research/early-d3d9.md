# The early Direct3DCreate9 (Engine.dll+0x74ec0b) and other D3D9 users in Vulkan mode

Question: why does the system `d3d9.dll` still get loaded in Vulkan mode, who calls `Direct3DCreate9` before
`eCGfxAdmin::InitializeAdmin`, what is done with the result, and how do we avoid it?
Evidence: headless Ghidra (Engine, Game projects; dumps `B:\DevDrive\risen-ghidra\rvk_earlyd3d9_dump.txt`, `..._dump2.txt`,
`..._game_dump.txt`), capstone scans of the test copy's binaries (`B:\DevDrive\risen-test-vk\bin`), and the Vulkan-mode
`rvk.log` of 2026-10-21 15:43. All addresses are RVAs (image base 0x180000000). **[C]** = confirmed by code or log,
**[I]** = inferred.

## 1. Answer in short

* `Engine.dll+0x74ec0b` is the return address of `call [rip+IAT Direct3DCreate9]` at `0x74ec05` **inside
  `eCGfxAdmin::sEnumAdapterDesc` (`0x74eb80`)**. It is a direct IAT call (IAT slot `0x805c18`), not an import thunk:
  the caller really is Engine.dll, not risen-spass. **[C]**
* The only caller is **`eCApplication::CreateEngine` (`0x4c4810`)**. It uses the result for exactly one thing: a
  "pixel shader 3.0 or better" check. The desc is a stack local and is thrown away afterwards. **[C]**
* It runs in Vulkan mode because `ms_pAPIInstance` is still null at that point. Our `HalInstance` is installed only in
  the `InitializeAdmin` detour, ~6.4 s later. With a null instance, `sEnumAdapterDesc` creates its own `IDirect3D9`
  and also runs a **DxDiag** query. **[C]**
* A second call in the same Vulkan run, `Direct3DCreate9 call #2 from sentry.dll+0x21895d` (mid-game, 15:44:40),
  **is risen-spass**. In the test copy `sentry.dll` *is* spass (spass.md 1.1), and the binary contains spass's
  `SpassDummyD3D` window class next to the `Direct3DCreate9` string. It is `findPresent()` of
  `risen.debug.screenshot` (`risen-spass/src/game/screenshot.cpp:210`). **[C]**
* Recommendation: **detour `eCGfxAdmin::sEnumAdapterDesc`** in Vulkan mode and fill the desc from our Vulkan adapter
  info. This skips both `Direct3DCreate9` and DxDiag. Separately, change spass so its screenshot uses our renderer when
  `rvk` is in Vulkan mode (section 6).

## 2. Startup chain [C]

```
Risen.exe  -> eCApplication::Create(HINSTANCE, bCString const&, bCString const&)   0x4c5520 (imported by Risen.exe)
  -> virtual Create() (vtable +0x58)                       eCApplication::Create      0x4c55b0 (Game.dll imports it [I: its app subclass chains to it])
     GenomeMutex, OnInitializeApplication (+0x598), eCVirtualFileSystem::Initialize, LoadConfiguration (+0x610)
     -> eCApplication::CreateEngine                        0x4c4810
        eCLocAdmin2::ApplyConfiguration
        OnInitializeEngine (vtable +0x5a8 -> 0x4c5aa0) == 1, else return 3
        adapter = <eSWindowOptions member @0xc6eed0> [I: the configured adapter index]
        eCGfxAdmin::sEnumAdapterDesc(adapter, desc)        0x74eb80   <-- EARLY Direct3DCreate9 (+ DxDiag)
        if ((desc.caps.PixelShaderVersion & 0xff00) < 0x300) -> MessageBox "HUD2_InsufficientGraphicsHardware", return 0
        IGameExplorer::VerifyAccess (CLSID_GameExplorer 9A5EA990-..., parental controls)
        "noaudio", window options, OnCreateWindow (+0x5a0), eCWindow::Create (+0x4f0)   <-- game window created here
        FPU options, eCTimer::ReadConfig
        -> CreateGraphicsSubsystem                         0x4c4330
           -> eCGfxAdmin::Create(3) 0x74f0d0 -> InitializeAdmin 0x74e510   <-- our hook; 2nd Direct3DCreate9 in D3D9 mode
        CreateModules ...
```

Check of the PS version: `call 0x74eb80` at `0x4c48ac` with `rdx = rbp-0x10`, then `mov eax,[rbp+0x4cc]; and eax,0xff00`.
That is desc `+0x4dc`, i.e. `D3DCAPS9` offset `0xcc` = `PixelShaderVersion`, because the desc stores caps from offset 8
at `+0x418`. The desc (`local_6b8`) is not read anywhere else in `CreateEngine`. **[C]**

Timing in `rvk.log` (Vulkan mode): call #1 at 15:44:04.712, system d3d9 loaded 15:44:04.715, `InitializeAdmin` at
15:44:11.082. The 6.4 s in between include DxDiag, the Game Explorer check, the window creation and the config work.
**[I]** DxDiag (section 3) is the likely bulk; to measure it, log around the original in a detour in passthrough mode.

## 3. What `eCGfxAdmin::sEnumAdapterDesc` (0x74eb80) does [C]

`static bEResult sEnumAdapterDesc(unsigned long adapter, eSGfxAdapterDesc& desc)`, decorated
`?sEnumAdapterDesc@eCGfxAdmin@@SA?AW4bEResult@@KAEAUeSGfxAdapterDesc@eCGfxShared@@@Z`. It always returns 1.

1. `memset(&desc, 0, 0x548)`.
2. If `ms_pAPIInstance` (`0xc62ad8`) is **null**:
   * `IDirect3D9* d3d = Direct3DCreate9(0x20)`. If it is null, it returns 1 with the desc all zero, so the PS check
     fails and the game shows the "insufficient hardware" box and quits.
   * `d3d->GetAdapterIdentifier(adapter, 0, &id)` (vtable `+0x28`, slot 5) -> Driver[512] `+0`, Description[512] `+0x200`,
     DriverVersion `+0x400`, VendorId/DeviceId/SubSysId/Revision `+0x408..+0x414`.
   * `d3d->GetDeviceCaps(adapter, D3DDEVTYPE_HAL, &caps)` (`+0x70`, slot 14) -> `D3DCAPS9[8..0x130)` to `+0x418`.
   * `d3d->Release()` (`+0x10`). **No CreateDevice, no window, no mode enumeration, no CheckDevice*.**
   * Then **DxDiag**: `CoInitialize`, `CoCreateInstance(CLSID_DxDiagProvider A65B8071-..., IID_IDxDiagProvider
     9C6B4CB0-...)`, `Initialize({dwSize 0x18, version 111, bAllowWHQLChecks 0})`, `GetRootContainer`,
     `DxDiag_DisplayDevices`. For the child whose `iAdapter == adapter` it reads `szDisplayMemoryLocalized` ->
     `desc+0x540` (MB) and logs "eCGfxAdmin::sEnumAdapterDesc - detected adapter number %i: %s with %s of total video
     memory!". `dxdiagn.dll` statically imports `d3d11`, `d3d12`, `dxgi`, `MFPlat`, `WMICLNT`. That loads the GPU's
     D3D11/12 user-mode drivers into the process (heavy). The DxDiag block runs only if `Direct3DCreate9` returned non-null.
3. Else, if the vtable is `eCDX9Instance::vftable` (devirtualized), `eCDX9Device::sGetAdapterDesc` (`0x73d7e0`).
4. Else (**foreign instance**) virtual slot 4: `ms_pAPIInstance->GetAdapterDesc(adapter, &desc)`. Our
   `HalInstance::GetAdapterDesc` already fills a valid SM3 desc and needs no prior `Init`.

All Engine.dll `Direct3DCreate9` call sites (capstone, IAT `0x805c18`): `eCDX9Device::sInit` `0x73dda3` (no callers),
`eCDX9Instance::Init` `0x74b159`, `InitializeAdmin` `0x74e5dc` (type 3 only, skipped by our detour),
`eCGfxAdmin::sGetAdapterCount` `0x74e83c` (null-instance branch; **no callers in any binary**, not imported),
`sEnumAdapterDesc` `0x74ec05`. **[C]**

`ms_pAPIInstance` writers: `InitializeAdmin`, `eCGfxAdmin::Destroy` and **the `eCGfxAdmin` constructor
(`0x74e982`, stores 0)**. Readers: `eCGfxContextAdmin::Create` (`0x5080f0`), `sGetAdapterCount`, `sEnumAdapterDesc`,
`InitializeAdmin`. **[C]**

## 4. Who reads the results later

* The early desc: **nobody** after the PS check (stack local of `CreateEngine`). **[C]**
* The options menu resolution list reads the **`InitializeAdmin` context list**, not the early desc.
  `gCHudResolutionManager2::BuildResolutions` (Game.dll `0x8821d0`) uses `eCGfxAdmin::GetContextCount` / `EnumContext`.
  It keeps contexts of adapter 0 with width >= 800 whose aspect ratio is close to one of 4 table ratios, then groups
  refresh rates. The list comes from `HalInstance::GetModeCount` / `GetModeDesc` (EnumDisplaySettings, 32 bpp) and
  does not change with what we do to the early call. Other Game.dll users of `EnumContext` are `0x854f70`, `0x8553c0`,
  `0x855760` and `0x877e10` (unnamed; [I] options/HUD code). **[C]**
* There is no separate launcher config path in Engine/Game that consumes it. The VRAM value from DxDiag is only logged.
  **[C]** for Engine.dll and Game.dll; other DLLs import neither `sEnumAdapterDesc` nor `sGetAdapterCount`.

## 5. Options

**(b) Detour `eCGfxAdmin::sEnumAdapterDesc` (recommended).**
* In Vulkan mode: `memset(desc, 0, 0x548)` and fill it exactly as `HalInstance::GetAdapterDesc` does (factor that into a
  free function and cache the `AdapterInfo` so the second query in `InitializeAdmin` is free). Return 1 and do not call
  the original. Result: no `Direct3DCreate9`, no system d3d9, no DxDiag/d3d11/d3d12 in-process, ~seconds less startup.
* If `ms_pAPIInstance != null` (not the case at startup), call the original.
* It is also the right moment for the **"is Vulkan usable?" decision**. It runs on the main thread, outside the loader
  lock, before the window and before `InitializeAdmin`. If no usable device is found, switch to passthrough here and call
  the original: the D3D9 path is still untouched.
* Hook safety: an exported, ~1.3 KB function with a normal prologue (`push rbp; push rsi; push r14; lea rbp,[rsp-0x830]`,
  no rip-relative bytes in the first 5). It is not COMDAT-folded, its only caller does a direct `call 0x74eb80`, and there
  is no inlined copy (the 5 `Direct3DCreate9` sites above are all of them). Add it to `engine/symbols.h`. The PGO
  devirtualization is not an issue: the detour replaces the whole function, including its `eCDX9Instance::vftable` compare.

**(c) Pre-install our `HalInstance` into `ms_pAPIInstance` before `CreateEngine`** (e.g. from DllMain). The existing
slot-4 path would then serve `sEnumAdapterDesc`. Not recommended: the `eCGfxAdmin` constructor zeroes
`ms_pAPIInstance` (`0x74e982`), and when it runs relative to `CreateEngine` is [I] (lazy getter `FUN_1800956c0`).
`InitializeAdmin` with type 3 would also overwrite the pointer (leak). It is more fragile than (b) for no gain.

**(a) Our proxy's `Direct3DCreate9` returns an own minimal `IDirect3D9` in Vulkan mode.** It works for the engine call,
which needs only `Release` (slot 2), `GetAdapterIdentifier` (5) and `GetDeviceCaps` (14). But **DxDiag still runs**
(non-null return). Every other in-process D3D9 user (spass, overlays) would also get a stub. If it is ever wanted as a
safety net for non-Engine callers, the full contract is:
`QueryInterface/AddRef/Release` (refcounted; QI `IID_IDirect3D9`/`IUnknown`); `RegisterSoftwareDevice` -> `D3DERR_NOTAVAILABLE`;
`GetAdapterCount` -> 1; `GetAdapterIdentifier` (adapter 0 else `D3DERR_INVALIDCALL`): Driver, Description, DeviceName
`\\.\DISPLAY1` (primary monitor's GDI name), DriverVersion, Vendor/Device/SubSys/Revision, DeviceIdentifier GUID,
WHQLLevel 1; `GetAdapterModeCount`/`EnumAdapterModes`/`GetAdapterDisplayMode` from `EnumDisplaySettings` for
`D3DFMT_X8R8G8B8` (0 modes and `D3DERR_NOTAVAILABLE` for other formats); `CheckDeviceType` S_OK for X8R8G8B8/HAL;
`CheckDeviceFormat`/`CheckDeviceMultiSampleType`/`CheckDepthStencilMatch`/`CheckDeviceFormatConversion` from a small
table or `D3DERR_NOTAVAILABLE`; `GetDeviceCaps` -> `DeviceType=HAL`, `AdapterOrdinal=0` plus `sm3Caps()`;
`GetAdapterMonitor` -> `MonitorFromPoint({0,0}, PRIMARY)`; `CreateDevice` -> `D3DERR_NOTAVAILABLE` (spass then fails
fast with "could not find IDirect3DDevice9::Present").
Recommendation: do not build it now. The proxy keeps forwarding unknown callers (and logs the caller module, as today).

## 6. risen-spass (call #2, `sentry.dll+0x21895d`) [C] code, [I] effect

`risen.debug.screenshot` -> `ensureHook` -> `findPresent()`:
* `LoadLibraryW(L"d3d9.dll")` returns **our proxy** (already loaded, same base name).
* `Direct3DCreate9` is forwarded to the system d3d9.
* It then creates a 64x64 `WS_OVERLAPPEDWINDOW` (class `SpassDummyD3D`, never shown) and a **real D3D9 device**. In the
  AMD driver that can take seconds (CLAUDE.md startup facts).
* It hooks `IDirect3DDevice9::Present` of the system driver. That code never runs in Vulkan mode, so requests stay
  pending (engine-gfx / CLAUDE.md: "does not work in Vulkan mode").

Proposed spass change (not done here, spass is another repo):
1. On the first screenshot request, `HMODULE d = GetModuleHandleW(L"d3d9.dll")`. If
   `GetProcAddress(d, "rvk_version")` exists, ask rvk instead of creating the dummy device.
2. rvk exports needed (new): e.g. `int rvk_mode()` (0 passthrough, 1 trace, 2 vulkan) and
   `int rvk_request_screenshot(const wchar_t* pngPath, int waitFrames)` / `const char* rvk_screenshot_status(...)`.
   They would be backed by the existing `HalDevice` back-buffer -> PNG path (`-Screenshots`).
3. In passthrough/trace mode (`rvk_mode() != 2`) spass keeps its D3D9 `Present` hook. Our proxy forwards to the system
   d3d9, so the D3D9 path stays correct.
4. With this change no D3D9 runtime is loaded in a Vulkan run, and spass screenshots work.

## 7. The hidden `temp_d3d_window_*` window (open)

The string `temp_d3d` (ASCII or UTF-16, any case) is **not present** in Engine.dll, spass's `sentry.dll`, the other
binaries in `bin`, any `System32\*.dll` (incl. `d3d9`, `d3d9on12`, `d3d11`, `d3d12`, `dxgi`, `dxdiagn`), the AMD driver
store DLLs (`atidx9loader64`, `amdxn64`, `amdxx64`, `amdxc64`, ...), `AppPatch` or the Steam overlay. So its creator is
not established statically. **[C]** for the search.
* The engine's early path creates no window itself. The only window spass creates is `SpassDummyD3D`. **[C]**
* **[I]** The name is built at runtime by a component that only gets loaded through a D3D path: the system d3d9/driver
  stack (call #1 or spass's dummy device) or DxDiag's d3d11/d3d12 enumeration.
* Option (b) removes the engine's D3D9 and DxDiag paths, and the spass change removes the rest. To pin it down, log
  `EnumThreadWindows`/`EnumWindows` for our process after `sEnumAdapterDesc` returns and after `InitializeAdmin`, in
  passthrough and Vulkan mode.
* **[I]** User hint: it may be the Steam overlay (`GameOverlayRenderer64.dll`) or the AMD overlay (Radeon Software,
  injected into D3D processes). Both hook D3D on first device creation and are injected / activated through the D3D path,
  which would also explain why the name is in no static binary that was searched (packed or downloaded at runtime). Check:
  list the process modules and the window's owning thread/module (`GetWindowThreadProcessId` + the thread's start
  address) while the window exists, with and without the Steam overlay (`-NoOverlay` / Steam setting) and AMD overlay.

## 8. Risks

* **PGO devirtualization**: irrelevant for (b) (whole-function detour). For (a) the engine never compares `IDirect3D9`
  vtables. `eCDX9Device::ms_p3DBase` stays null in Vulkan mode either way, and the early call never sets it (local, released).
* **PS check**: whatever fills the desc must report `PixelShaderVersion >= 0x300` (ours: `0xFFFF0300`). An all-zero desc
  (e.g. returning null from `Direct3DCreate9`) makes the game quit with "HUD2_InsufficientGraphicsHardware".
* **Adapter index**: `CreateEngine` passes the configured adapter (`[I]` eSWindowOptions). Our desc ignores it (single
  Vulkan adapter, `config.toml adapter` chooses). That is harmless because only the caps are checked.
* **Resolution list**: unaffected (it comes from `InitializeAdmin` -> `HalInstance` modes, section 4).
* **Timing**: a Vulkan instance creation (~0.1 s) moves earlier, before the game window. Cache it for `InitializeAdmin`.
