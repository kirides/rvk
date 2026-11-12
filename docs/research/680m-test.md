# 680M test guide: Full mode with every owned stage on, Radeon GPU Profiler captures (S-9, 2026-11-12)

What this test is for: the first measurement of the owned Full frame on real bandwidth-limited hardware (the Radeon 680M of a Ryzen 6000 laptop). Every number for the
680M in frame-ownership.md 14.3 / 14.11 / 15.4 is a model (XTX x 6.5 / x 15 / x 10); the captures decide L-1 (the load-time data layout: position stream, packing) and
F15-2 (removing the fallbacks and the check frames). Background: frame-ownership.md 14.41 (S-9), 15.4 rows 18 / 19.

## 1. Install

1. The game: Risen (64-bit, Steam "Risen", not "Risen Classic"). Graphics driver: a current AMD Adrenalin driver (Vulkan 1.3).
2. The build: unpack `risen-enhanced-graphics.zip` into `<Risen>\bin` (`d3d9.dll` next to `Risen.exe`, folder `rvk\`). Nothing else is needed for Vulkan.
3. **Radeon Developer Tool Suite** (RDTS, https://gpuopen.com/tools/: Radeon Developer Panel = RDP + Radeon GPU Profiler = RGP). Unzip anywhere.
4. Switch off Radeon Chill / any frame limiter for `Risen.exe` in Adrenalin (it caps the world at 80 fps on the main machine and distorts frame times: fps-limit.md).

## 2. Settings

* **Full mode**: start the game once, F11 -> Display page, section "Render mode" = **Full (Vulkan, experimental)** -> the banner says "restart needed" -> **Apply and
  restart**. (Switching from Wrapper applies at once but the owned stages' start-time tables exist only in a session that STARTED in Full: always restart.) Or put
  `mode = "full"` under `[renderer]` in `<Risen>\bin\rvk\config.toml` (then it is fixed for every start).
* Everything else: **leave the player defaults**. On an integrated GPU the first start picks the **Integrated** preset by itself (rvk.log `settings: integrated GPU:
  Integrated preset chosen by default, N values set`: water RT off, FSR Quality, 1024 cascades, 1.5x detail; frame-ownership.md 14.11) and Low Bandwidth = auto = on.
  Every owned stage is on by default in Full (nothing to switch on).
* **First start**: the shaders compile on the first start in Full (a few minutes of a cold cache: frame-ownership.md 14.41 "Cold start"). The first minute in the world
  can stutter and draws more of the frame the old way until the queue drains; play / wait ~2 - 3 minutes in the world and quit once before measuring. From the second
  start on the caches in `rvk\cache` are warm.
* **D3D12**: `<Risen>\bin\rvk\config.toml`, `[vulkan]` section: `api = "d3d12"` (needs Windows 11 / SM 6.6: rvk.log says when it falls back to Vulkan). The zip does not
  ship the D3D12 parts yet (AMD's `*_dx12.dll`, the Agility runtime: `scripts/package.ps1`), so on D3D12 FSR / frame generation stay off and the Integrated preset's FSR
  Quality runs at native resolution there (rvk.log says why); compare the two backends at the same render scale (`upscale.mode = off` on both, Image page) or copy the
  `_dx12` DLLs from a developer build into `rvk\`. A D3D12 first start compiles
  its own cache (DXIL) again.

## 3. What to capture with RGP

Setup (once): run RDP, "Enable" developer mode if asked, tab **Profiling**: enable it for the next started application; leave "Enable instruction tracing" off for
the first round (it makes captures large; turn it on for one capture per scene later). Start Risen through Steam as usual (RDP attaches to the next 3D application;
it shows `Risen.exe` and the API: Vulkan or DirectX 12). The capture hotkey is **Ctrl+Shift+C** (RDP's default; the game keeps running) or "Capture profile" in RDP;
the capture opens in RGP.

For each backend (Vulkan first, then D3D12), with the game warm (second start), standing still for 5 s before each capture, the F11 menu closed:

| # | scene | where | why |
|---|---|---|---|
| 1 | beach start | right after "New game" on the shore: look along the beach to the sea | sea / water + sky, few objects |
| 2 | Harbour Town street | the market square / docks, look along a street | many static meshes + characters (owned static / actors / instances) |
| 3 | jungle / forest | the forest on the way from the beach to the swamp, look into dense trees | SpeedTrees, grass (owned trees / vegetation), the 680M's worst case in the model |
| 4 | tavern interior | Harbour Town tavern, look across the room | lamps (lighting table, lamp shadows), interior |
| 5 | night outdoors | any of 2 / 3 at night | the lighting table at night, torches |
| 6 | turning | rotate the camera steadily (mouse) while capturing in 3 | records made anew while turning (frame-ownership.md 7.43) |

Also one capture per scene in **Wrapper** (Render mode Wrapper, restart) as the reference: the difference Full - Wrapper on the same spot is what the owned frame
gained or lost on the 680M.

Save every capture (`.rgp`) with a name `<api>_<mode>_<scene>.rgp` (e.g. `vk_full_jungle.rgp`).

What we read from them: the per-pass GPU time (the frame's regions are labelled: `Frame N > Post > Main #1 > Solid ...`, injected passes by name; capture-regions.md),
the wave occupancy / VGPRs of the ubershader pixel shaders (lit PS = the rank-1 item of 14.3), the z / cascade passes' vertex fetch (L-1's position stream decision),
cache hit rates / bandwidth of the solid pass, and the barriers / idle gaps between the owned flushes.

## 4. Logs to send back

After each session, `<Risen>\bin\rvk\rvk.log` (it is rewritten at every start: copy it out before the next start, name it like the capture set). The lines we read:

* `renderer: mode full at start ...`, `vulkan: ...` / `caps:` (the device and its capabilities), `settings: integrated ...` (the preset), `vulkan: low bandwidth: on (...)`.
* `native: stages (5 s): ...` every 5 s: each owned stage `active(...)`, `inactive(...)` with the reason, never `INCONSISTENT`.
* `native: owned static (5 s)`, `owned shadows`, `owned trees`, `owned actors`, `owned instances`, `owned ordered`, `owned vegetation draws`: the owned shares;
  `native: composed late`: how much is still drawn the old way (the `variant queue` number = shaders still compiling).
* `MISS` / `DIFFERING` / `FALSE NEGATIVES` in any `native:` line (must be 0; the check frames compare every 10th frame), and any line with `turned off` / `tripped`.
* `native: captures (5 s)` (should be 0 after the first seconds).
* `perf:` lines (only with `[debug] perf = true` in config.toml, see below) and the `hal: frame` fps lines (always).
* At the end `hal: device shutdown complete`. If the game crashed or hung: everything in `<Risen>\bin\rvk\dumps\`.

For CPU / GPU timing in the log as well (cheap, does not disturb RGP much): add to `config.toml`

```toml
[debug]
perf = true
```

## 5. If something looks wrong: switching single stages off

The owned stages are Debug settings: F11 -> System -> **Debug mode** on, then the **Debug** page, section "Native frame" / "Renderer internals". Each can be
switched off on its own (run-time ones at once; `native.clusters`, `native.cluster_cull`, `native.cluster_cull_shadows`, `native.material_table`,
`native.illum_table`, `native.lighting_images`, `native.mesh_parts`, `native.bone_motion`, `native.rigid_motion` after a restart). Or in `rvk\settings.toml` (game
closed), e.g.:

```toml
[native]
owned_trees = false      # trees composed again
own_post = "off"         # the post chain on the engine's translated shaders
[debug]
ubershader = false       # the translated material shaders instead of the HLSL ubershader
```

(Outside Debug mode these settings apply as their defaults, i.e. on: Debug mode must be on for a changed value to act.) Bisecting order when a picture is wrong:
`debug.ubershader`, `native.own_post`, `native.owned_trees`, `native.owned_vegetation_ops`, `native.owned_actors`, `native.owned_ordered`, `native.owned_instances`,
`native.owned_shadows`, then `native.owned_static` (turns off everything that depends on it). Everything at once = Render mode Wrapper. A photo / screenshot (F11 off,
the game's own screenshot key) with the rvk.log of that session helps more than a description.

The full list of the stages and what each does: CLAUDE.md "Frame ownership", frame-ownership.md 14.41.
