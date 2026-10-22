#pragma once

// Borderless fullscreen (Vulkan mode only; docs/research/window-mode.md). The engine owns the game window (eCWindow /
// eCApplication: style 0x90000000 = WS_POPUP | WS_VISIBLE + WS_EX_TOPMOST for fullscreen, 0xc80000 = caption + system menu
// for windowed); with D3D9 the runtime then switches the display mode. Our HAL never takes exclusive fullscreen:
//   * fullscreen requested (start / mode parameters windowed = false): the window becomes a borderless, non-topmost popup
//     exactly over its monitor (rcMonitor), at the monitor's resolution. A different requested resolution is replaced by
//     the monitor's through the engine's own eCApplication::SetResolutionEx (a pre-hook rewrites the size of fullscreen
//     requests; a fullscreen start at another size is corrected once with SetResolution), so the back buffer, the
//     viewports, the GUI layout (eCApplication::OnSize) and the mouse all agree with the window.
//   * windowed at exactly the monitor's resolution: borderless over the monitor as well (the engine keeps believing it is
//     windowed: the caption bits are removed, not replaced by WS_POPUP, so eCWindow::GetWindowMode still says windowed).
//   * any other windowed size: the engine's own bordered window, untouched.
// A borderless window is kept on its monitor (moved away -> placed again; moved to a monitor of another size -> the engine
// switches to that size). The process is made per-monitor DPI aware (V2) before the game window exists, so the window
// covers physical pixels on scaled displays too.
//
// Exclusive fullscreen (player setting vulkan.fullscreen = exclusive, docs/research/window-mode.md 8): while the engine is
// in fullscreen and the window is in front, the monitor is switched to the game's resolution + refresh rate
// (ChangeDisplaySettingsEx CDS_FULLSCREEN), the window covers it and the device takes Vulkan's exclusive mode
// (VK_EXT_full_screen_exclusive, application controlled). Focus lost -> desktop mode restored, window minimised; back in
// front -> switched again. In borderless fullscreen a refresh rate below the desktop's becomes a frame cap with vsync.

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

struct HWND__;

namespace rvk::engine::window {

// eCWindow::eEWindowMode (the window style the engine sets)
constexpr std::uint32_t kModePopup = 0x90000000u;     // WS_POPUP | WS_VISIBLE: the engine's fullscreen
constexpr std::uint32_t kModeWindowed = 0x00c80000u;  // WS_CAPTION | WS_SYSMENU: the engine's windowed mode

struct Rect {
    int x = 0, y = 0, width = 0, height = 0;
    bool operator==(const Rect&) const = default;
};

// What a mode of the engine (windowed flag, back buffer size) becomes on a monitor. Pure (unit-tested).
struct Decision {
    bool borderless = false;       // borderless over the monitor's rectangle
    bool engineFullscreen = false;  // the engine asked for fullscreen (its WS_POPUP style stays)
    bool resize = false;            // the back buffer must become the monitor's size first (fullscreen at another size)
};
Decision decide(bool windowed, std::uint32_t width, std::uint32_t height, const Rect& monitor);

// A display mode of a monitor (32 bits per pixel).
struct DisplayMode {
    std::uint32_t width = 0, height = 0, refresh = 0;
    bool operator==(const DisplayMode&) const = default;
};
// The mode exclusive fullscreen switches to for the game's width x height x refresh. Pure (unit-tested): that exact mode;
// refresh 0 -> the desktop's refresh if the size has it, else the highest; a refresh the size lacks -> the nearest one
// (the higher on a tie); no mode of that size -> none.
std::optional<DisplayMode> pickDisplayMode(std::span<const DisplayMode> modes, std::uint32_t width, std::uint32_t height,
                                           std::uint32_t refresh, std::uint32_t desktopRefresh);
// The HAL's mode list order (the engine takes the first mode of the configured size when its RefreshRate is 0): sizes
// ascending, per size the desktop's refresh rate first, then the others descending. Pure (unit-tested), removes duplicates.
std::vector<DisplayMode> orderModes(std::vector<DisplayMode> modes, std::uint32_t desktopRefresh);

// Makes the process per-monitor DPI aware (V2) unless the manifest / an earlier call decided; call before the game window
// is created (the sEnumAdapterDesc detour). Logs the result.
void enableDpiAwareness();

// Creates the SetResolutionEx detour (enabled with the others by hooks::enableAll); false when the symbols are missing
// (borderless placement still works, a fullscreen request at another size then keeps its size, scaled to the monitor).
bool install();

// The HAL's device creation and ToggleNewMode: the engine's mode (refresh: the engine's matched mode, passed in every
// mode); the window is placed at the next onPresent.
void onModeSet(HWND__* window, bool windowed, std::uint32_t width, std::uint32_t height, std::uint32_t refresh);

// the setting vulkan.fullscreen (its effective value): exclusive fullscreen while the engine is in fullscreen. Engine thread.
void setExclusive(bool on);

// What the device does at this present.
struct Presentation {
    bool resized = false;             // the window's client size may have changed: re-create the swapchain
    bool displayModeChanged = false;  // a monitor's display mode was changed / restored (the pacer re-reads the refresh)
    void* exclusiveMonitor = nullptr; // exclusive fullscreen engaged on this monitor (HMONITOR): the swapchain's exclusive info
    bool holdExclusive = false;       // engaged, display mode set and the window in front: hold Vulkan's exclusive mode
    double refreshCap = 0.0;          // borderless fullscreen: the game's refresh rate as a frame cap with vsync (0 = none)
};

// SwapScreen, before the present (the engine's thread): pending placements, corrections, the keep-on-monitor check and
// exclusive fullscreen's display mode / focus handling.
Presentation onPresent();

// restores a display mode we changed (process detach; harmless when none was changed)
void restoreDisplayMode(const char* why);

// "[debug] commands" / overlay: "window fullscreen" | "window toggle" (the debug key F2's ToggleFullscreen) |
// "window <W>x<H>" (windowed at that size) | "window fullscreen <W>x<H>[@<Hz>]" (fullscreen at that mode: exclusive keeps it,
// borderless takes the monitor's size) | "window state" | "window minimize" / "window restore" (focus loss / return) |
// "window focus" (to the front: test runs start in the background) | "window display" (the monitors' current display modes). Calls the engine's own resolution functions.
bool runCommand(std::string_view args, std::string* out);

// one line about the current state (log / overlay)
std::string describe();
// one short line for the overlay: "exclusive fullscreen 1920x1080 @ 60 Hz" / "borderless fullscreen 2560x1440 (game
// refresh 60 Hz)" / "window 1600x900"
std::string displaySummary();
// the monitors' current display modes ("DISPLAY1 2560x1440@120 at 0,0; ...")
std::string currentDisplayModes();

}  // namespace rvk::engine::window
