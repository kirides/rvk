risen-enhanced-graphics: a Vulkan renderer for Risen (64-bit version, not "Risen Classic")
Nightly build, work in progress. https://github.com/kirides/rvk

INSTALL
  Unpack the zip into <Risen>\bin (d3d9.dll goes next to Risen.exe).
  Needs a GPU + driver with Vulkan 1.3.
  Uninstall: delete <Risen>\bin\d3d9.dll and the <Risen>\bin\rvk folder.

IN GAME
  F11  opens / closes the settings overlay (the key can be changed there). While it is open, mouse and
       keyboard go to the overlay.
  Enhanced water (with ray-traced reflections) and enhanced shadows are on by default; each "Enhanced ..."
  checkbox off gives the game's original look. Lower water / shadow quality if the frame rate is low.
  Settings save automatically to rvk\settings.toml. Settings marked "restart needed" apply at the next start.
  "Debug mode" (Overlay section) shows developer tools: debug views and an A/B comparison (F10).

QUIRKS
  - Fullscreen is a borderless window over the monitor. Real exclusive fullscreen: overlay, Display section.
  - Ray tracing options need a GPU with ray tracing support; otherwise they are greyed out.
  - The first start (and the first visit to a coast with the enhanced ocean) can take longer: caches are built
    in rvk\cache.
  - To play with the game's own D3D9 renderer, set mode = "passthrough" in rvk\config.toml.

PROBLEMS
  Attach rvk\rvk.log (written each start) to bug reports, plus the build line below.
