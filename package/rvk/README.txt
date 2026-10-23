risen-enhanced-graphics: a Vulkan renderer for Risen (64-bit version, not "Risen Classic")
Nightly build, work in progress. https://github.com/kirides/rvk

INSTALL
  Unpack the zip into <Risen>\bin (d3d9.dll goes next to Risen.exe).
  Needs a GPU + driver with Vulkan 1.3.
  Uninstall: delete <Risen>\bin\d3d9.dll and the <Risen>\bin\rvk folder.

IN GAME
  F10  opens / closes the settings overlay. While it is open, mouse and keyboard go to the overlay.
  F11  A/B: flips between your settings and the defaults (the game's original look), to compare.
  Both keys can be changed in the overlay (Overlay section).
  Enhanced water and shadows are off by default: turn them on in the overlay.
  Settings save automatically to rvk\settings.toml. Settings marked "restart needed" apply at the next start.

QUIRKS
  - Fullscreen is a borderless window over the monitor. Real exclusive fullscreen: overlay, Display section.
  - Ray tracing options need a GPU with ray tracing support; otherwise they are greyed out.
  - The first start (and the first visit to a coast with the enhanced ocean) can take longer: caches are built
    in rvk\cache.
  - To play with the game's own D3D9 renderer, set mode = "passthrough" in rvk\config.toml.

PROBLEMS
  Attach rvk\rvk.log (written each start) to bug reports, plus the build line below.
