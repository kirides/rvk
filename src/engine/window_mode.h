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

#include <cstdint>
#include <string>
#include <string_view>

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

// Makes the process per-monitor DPI aware (V2) unless the manifest / an earlier call decided; call before the game window
// is created (the sEnumAdapterDesc detour). Logs the result.
void enableDpiAwareness();

// Creates the SetResolutionEx detour (enabled with the others by hooks::enableAll); false when the symbols are missing
// (borderless placement still works, a fullscreen request at another size then keeps its size, scaled to the monitor).
bool install();

// The HAL's device creation and ToggleNewMode: the engine's mode; the window is placed at the next onPresent.
void onModeSet(HWND__* window, bool windowed, std::uint32_t width, std::uint32_t height);

// SwapScreen, before the present (the engine's thread): pending placements, corrections and the keep-on-monitor check.
// True when the window's client size may have changed (the caller re-creates the swapchain).
bool onPresent();

// "[debug] commands" / overlay: "window fullscreen" | "window toggle" (the debug key F2's ToggleFullscreen) |
// "window <W>x<H>" (windowed at that size) | "window state". Calls the engine's own resolution functions.
bool runCommand(std::string_view args, std::string* out);

// one line about the current state (log / overlay)
std::string describe();

}  // namespace rvk::engine::window
