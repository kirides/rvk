risen-enhanced-graphics: a Vulkan renderer for Risen (64-bit version, not "Risen Classic")
Nightly build, work in progress. https://github.com/kirides/rvk

INSTALL
  Unpack the zip into <Risen>\bin (d3d9.dll goes next to Risen.exe).
  Needs a GPU + driver with Vulkan 1.3.
  Uninstall: delete <Risen>\bin\d3d9.dll and the <Risen>\bin\rvk folder.

IN GAME
  F11  opens / closes the settings menu (the key can be changed in System). While it is open, mouse and
       keyboard go to the menu. Hover over a setting for an explanation.
  Graphics tab: presets from Original (the game's own look) to Ultra. Enhanced water (with ray-traced
  reflections) and enhanced shadows are on by default ("High"); pick a lower preset if the frame rate is low.
  "Show advanced settings" (top right) adds fine tuning such as wave height and shadow distance.
  Settings save automatically to rvk\settings.toml. Settings marked "restart needed" apply at the next start.
  "Debug mode" (System tab) adds developer tools; it is not needed for playing.

QUIRKS
  - Fullscreen is a borderless window over the monitor. Real exclusive fullscreen: overlay, Display section.
  - Ray tracing options need a GPU with ray tracing support; otherwise they are greyed out.
  - Anti-aliasing / upscaling (Image): AMD FSR 3.1. With "Show advanced settings", Upscaler can be set to FSR 4 (a neural
    upscaler from AMD's FSR SDK 2.4; needs a Radeon RX 6000 series or newer, otherwise FSR 3.1 stays; experimental).
  - Frame generation (AMD FSR 3 / FSR 4, developer setting for now, System > Debug mode): the DLL is in this zip (rvk folder).
  - The first start (and the first visit to a coast with the enhanced ocean) can take longer: caches are built
    in rvk\cache.
  - Render mode (System tab): Wrapper (default: the game's rendering on Vulkan + the enhancements), Full
    (experimental: the renderer draws the whole frame itself; costs some processor time for now) or Original
    (the game's own D3D9 renderer, no enhancements, no F11 menu; after a restart). To leave Original, delete
    the [renderer] lines in rvk\settings.toml (or set mode = "wrapper" there).

CREDITS
  High fps gameplay fixes (falling, climbing, thunder; System tab, "High Fps compatibility (Vaana)"):
  ported from VaanaCZ's Risen FPS Patch (VaanaCZ and Auronen), https://github.com/VaanaCZ/Risen-FPS-Patch

PROBLEMS
  Attach rvk\rvk.log (written each start) to bug reports, plus the build line below.
