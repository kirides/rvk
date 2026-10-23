#include "engine/window_mode.h"

#include <windows.h>

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cstdint>
#include <format>
#include <string>
#include <string_view>
#include <tuple>
#include <vector>

#include <spdlog/spdlog.h>

#include "core/hooks.h"
#include "engine/symbols.h"

namespace rvk::engine::window {
namespace {

// bool eCApplication::SetResolutionEx(adapter, width, height, refresh, vsync, fov?, eEWindowMode) (0x4c02e0): every mode
// change of the engine (SetResolution, SetFullscreen, ToggleFullscreen, ToggleDebugResolution, the options menu's
// gCHudResolutionManager2) ends here: window title, eCWindow::SetWindowMode (style), SetClientSize (centred on the primary
// desktop), then the reset flag (+0xc48) -> the next DoRender's eCApplication::Reset -> device ToggleNewMode
using SetResolutionExFn = bool (*)(void* self, unsigned long adapter, unsigned long width, unsigned long height, unsigned long refresh,
                                   bool vsync, float value, std::uint32_t mode);
// bool eCApplication::SetResolution(width, height, eEWindowMode) (0x4c0430): SetResolutionEx with the current rest
using SetResolutionFn = bool (*)(void* self, unsigned long width, unsigned long height, std::uint32_t mode);
// bool eCApplication::ToggleFullscreen() (0x4c01f0): the debug key F2
using ToggleFullscreenFn = bool (*)(void* self);
using AppGetInstanceFn = void* (*)();

Hook<SetResolutionExFn> g_setResolutionEx;

// engine thread only (the HAL's create / ToggleNewMode / SwapScreen and the engine's SetResolutionEx run there): no lock,
// and onPresent may re-enter through the detour
struct State {
    HWND window = nullptr;
    bool windowed = true;
    std::uint32_t width = 0, height = 0;  // the engine's back buffer
    bool pending = false;                  // a mode was set: place the window at the next present
    bool borderless = false;               // we placed it borderless
    HMONITOR target = nullptr;             // the monitor the last engine mode change started on (it centres on the primary)
    int corrections = 0;                   // resolution requests of ours in a row (loop guard)
    std::chrono::steady_clock::time_point lastCheck{};
    std::uint32_t refresh = 0;             // the engine's refresh rate (its matched mode; every mode, docs/research/window-mode.md 8.1)
    // exclusive fullscreen (docs/research/window-mode.md 8)
    bool exclusive = false;                // the setting vulkan.fullscreen = exclusive (effective value)
    bool exPending = false;                // re-evaluate (setting changed)
    bool exActive = false;                 // the display mode is set and the window covers the monitor
    HMONITOR exMonitor = nullptr;          // the monitor of the exclusive swapchain (kept while minimised)
    DisplayMode exMode{};
    bool displayChanged = false;           // we changed a display mode (CDS_FULLSCREEN): restoreDisplayMode undoes it
    bool modeEvent = false;                // a display mode was changed / restored since the last onPresent
    // SetResolutionEx ran: the engine's Reset -> ToggleNewMode -> onModeSet follows (its window style already changed);
    // no placement until then (at most 3 s: an unchanged request never resets)
    std::chrono::steady_clock::time_point changing{};
};
State g;

// GDI device names (DISPLAYn) are not stable across display mode changes on this machine (two monitors swapped their
// names after a mode change was undone): always derive them from the monitor handle when needed
std::wstring monitorDevice(HMONITOR monitor) {
    MONITORINFOEXW mi{};
    mi.cbSize = sizeof(mi);
    if (monitor == nullptr || !GetMonitorInfoW(monitor, &mi)) return {};
    return mi.szDevice;
}

std::string narrow(const std::wstring& w) {
    std::string out;
    for (const wchar_t c : w) out.push_back(c < 128 ? static_cast<char>(c) : '?');
    return out;
}

DisplayMode modeOf(const DEVMODEW& dm) { return {dm.dmPelsWidth, dm.dmPelsHeight, dm.dmDisplayFrequency}; }
struct MonitorModes;
std::string summaryText(const MonitorModes& mm);
std::string modeText(const DisplayMode& m) { return std::format("{}x{}@{}", m.width, m.height, m.refresh); }

struct MonitorModes {
    std::vector<DisplayMode> modes;  // 32 bpp
    DisplayMode desktop;             // the registry mode (what a restore returns to)
    DisplayMode current;
};
MonitorModes monitorModes(const std::wstring& device) {
    MonitorModes mm;
    DEVMODEW dm{};
    dm.dmSize = sizeof(dm);
    for (DWORD i = 0; EnumDisplaySettingsW(device.c_str(), i, &dm); ++i) {
        if (dm.dmBitsPerPel == 32) mm.modes.push_back(modeOf(dm));
    }
    if (EnumDisplaySettingsW(device.c_str(), ENUM_REGISTRY_SETTINGS, &dm)) mm.desktop = modeOf(dm);
    if (EnumDisplaySettingsW(device.c_str(), ENUM_CURRENT_SETTINGS, &dm)) mm.current = modeOf(dm);
    return mm;
}

bool monitorRect(HMONITOR monitor, Rect& r, std::string* name = nullptr) {
    MONITORINFOEXW mi{};
    mi.cbSize = sizeof(mi);
    if (monitor == nullptr || !GetMonitorInfoW(monitor, &mi)) return false;
    r = {mi.rcMonitor.left, mi.rcMonitor.top, mi.rcMonitor.right - mi.rcMonitor.left, mi.rcMonitor.bottom - mi.rcMonitor.top};
    if (name != nullptr) {
        name->clear();
        for (const wchar_t* c = mi.szDevice; *c != 0; ++c) name->push_back(*c < 128 ? static_cast<char>(*c) : '?');
    }
    return true;
}

std::string rectText(const RECT& r) { return std::format("({},{} {}x{})", r.left, r.top, r.right - r.left, r.bottom - r.top); }
std::string rectText(const Rect& r) { return std::format("({},{} {}x{})", r.x, r.y, r.width, r.height); }

UINT asyncFlag(HWND window) {
    // never block on another thread's message loop (the engine creates and renders on one thread: normally 0)
    return GetWindowThreadProcessId(window, nullptr) == GetCurrentThreadId() ? 0u : SWP_ASYNCWINDOWPOS;
}

void* app() {
    const auto& s = sym();
    return s.appGetInstance != nullptr ? reinterpret_cast<AppGetInstanceFn>(s.appGetInstance)() : nullptr;
}

// the engine's own mode change (window, back buffer, GUI): applied by its next Reset -> ToggleNewMode -> onModeSet
bool requestResolution(std::uint32_t width, std::uint32_t height, std::uint32_t mode, std::string_view why) {
    const auto& s = sym();
    void* a = app();
    if (a == nullptr || s.appSetResolution == nullptr) {
        spdlog::warn("engine: window: eCApplication::SetResolution not found: {} skipped", why);
        return false;
    }
    spdlog::info("engine: window: SetResolution({}x{}, {}) ({})", width, height, mode == kModePopup ? "fullscreen" : "windowed", why);
    return reinterpret_cast<SetResolutionFn>(s.appSetResolution)(a, width, height, mode);
}

void place(HMONITOR monitor, const Rect& mr, const std::string& monitorName, bool engineFullscreen) {
    HWND w = g.window;
    const LONG style = GetWindowLongW(w, GWL_STYLE);
    const LONG ex = GetWindowLongW(w, GWL_EXSTYLE);
    // no caption, frame or sizing border; the engine's fullscreen keeps WS_POPUP, its windowed mode stays a non-popup
    // window (eCWindow::GetWindowMode then still answers "windowed" from its stored mode)
    LONG newStyle = style & ~static_cast<LONG>(WS_CAPTION | WS_THICKFRAME | WS_MINIMIZEBOX | WS_MAXIMIZEBOX);
    if (engineFullscreen) newStyle |= static_cast<LONG>(WS_POPUP);
    const LONG newEx = ex & ~static_cast<LONG>(WS_EX_DLGMODALFRAME | WS_EX_WINDOWEDGE | WS_EX_CLIENTEDGE | WS_EX_STATICEDGE);
    if (newStyle != style) SetWindowLongW(w, GWL_STYLE, newStyle);
    if (newEx != ex) SetWindowLongW(w, GWL_EXSTYLE, newEx);
    // not topmost (the engine's fullscreen sets WS_EX_TOPMOST for D3D9's exclusive mode): alt-tab and other windows work
    // as with any window; a game in the background stays behind the foreground window
    const UINT async = asyncFlag(w);
    const bool topmost = (ex & WS_EX_TOPMOST) != 0;
    UINT flags = SWP_FRAMECHANGED | SWP_NOACTIVATE | SWP_NOOWNERZORDER | async;
    if (!topmost) flags |= SWP_NOZORDER;
    SetWindowPos(w, topmost ? HWND_NOTOPMOST : nullptr, mr.x, mr.y, mr.width, mr.height, flags);
    if (topmost) {
        const HWND fg = GetForegroundWindow();
        if (fg != nullptr && fg != w && (GetWindowLongW(fg, GWL_EXSTYLE) & WS_EX_TOPMOST) == 0) {
            SetWindowPos(w, fg, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOOWNERZORDER | async);
        }
    }
    RECT wr{}, cr{};
    GetWindowRect(w, &wr);
    GetClientRect(w, &cr);
    spdlog::info("engine: window: borderless over monitor {} {} ({}): style 0x{:08x} -> 0x{:08x}, ex 0x{:08x} -> 0x{:08x}, "
                 "window {}, client {}x{}, back buffer {}x{}",
                 monitorName, rectText(mr), engineFullscreen ? "the engine's fullscreen" : "windowed at the monitor's resolution",
                 static_cast<std::uint32_t>(style), static_cast<std::uint32_t>(GetWindowLongW(w, GWL_STYLE)),
                 static_cast<std::uint32_t>(ex), static_cast<std::uint32_t>(GetWindowLongW(w, GWL_EXSTYLE)), rectText(wr),
                 cr.right, cr.bottom, g.width, g.height);
    g.borderless = true;
    (void)monitor;
}

// a window we made borderless that is windowed again without the engine restoring its style (it normally does, in
// SetWindowMode): the engine's windowed style back, the client at the back buffer's size, centred on the monitor
bool restoreBordered(const Rect& mr) {
    HWND w = g.window;
    const LONG style = GetWindowLongW(w, GWL_STYLE);
    if ((style & WS_CAPTION) == WS_CAPTION || (style & WS_POPUP) != 0) return false;
    const LONG newStyle = style | static_cast<LONG>(kModeWindowed);
    SetWindowLongW(w, GWL_STYLE, newStyle);
    RECT r{0, 0, static_cast<LONG>(g.width), static_cast<LONG>(g.height)};
    AdjustWindowRectEx(&r, static_cast<DWORD>(newStyle), FALSE, static_cast<DWORD>(GetWindowLongW(w, GWL_EXSTYLE)));
    const int ww = r.right - r.left, wh = r.bottom - r.top;
    SetWindowPos(w, nullptr, mr.x + (mr.width - ww) / 2, mr.y + (mr.height - wh) / 2, ww, wh,
                 SWP_FRAMECHANGED | SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOOWNERZORDER | asyncFlag(w));
    spdlog::info("engine: window: the engine's windowed style restored (0x{:08x} -> 0x{:08x})", static_cast<std::uint32_t>(style),
                 static_cast<std::uint32_t>(newStyle));
    return true;
}

bool detourSetResolutionEx(void* self, unsigned long adapter, unsigned long width, unsigned long height, unsigned long refresh,
                           bool vsync, float value, std::uint32_t mode) {
    if (g.window != nullptr) {
        // the monitor the player is on (SetClientSize centres the window on the primary desktop afterwards)
        const HMONITOR monitor = MonitorFromWindow(g.window, MONITOR_DEFAULTTONEAREST);
        g.target = monitor;
        Rect mr;
        std::string name;
        if (mode != kModePopup && g.displayChanged) {
            // leaving exclusive fullscreen: the desktop's mode back before the engine sizes and centres its window
            restoreDisplayMode("the game leaves fullscreen");
            g.exActive = false;
        }
        if (mode == kModePopup && g.exclusive) {
            // exclusive fullscreen: the requested size when the monitor has such a mode (the display switches to it), else
            // the monitor's desktop size
            const MonitorModes mm = monitorModes(monitorDevice(monitor));
            const bool has = std::ranges::any_of(mm.modes, [&](const DisplayMode& m) { return m.width == width && m.height == height; });
            if (!has && mm.desktop.width != 0) {
                spdlog::info("engine: window: exclusive fullscreen request {}x{}: the monitor has no such mode, its desktop {}x{} instead",
                             width, height, mm.desktop.width, mm.desktop.height);
                width = mm.desktop.width;
                height = mm.desktop.height;
            } else {
                spdlog::info("engine: window: exclusive fullscreen request {}x{} @ {} Hz", width, height, refresh);
            }
        } else if (mode == kModePopup && monitorRect(monitor, mr, &name) && mr.width > 0 && mr.height > 0 &&
                   (width != static_cast<unsigned long>(mr.width) || height != static_cast<unsigned long>(mr.height))) {
            spdlog::info("engine: window: fullscreen request {}x{} -> the monitor's {}x{} ({})", width, height, mr.width, mr.height, name);
            width = static_cast<unsigned long>(mr.width);
            height = static_cast<unsigned long>(mr.height);
        }
    }
    if (g.window != nullptr) g.changing = std::chrono::steady_clock::now();
    return g_setResolutionEx.original(self, adapter, width, height, refresh, vsync, value, mode);
}

}  // namespace

Decision decide(bool windowed, std::uint32_t width, std::uint32_t height, const Rect& monitor) {
    Decision d;
    if (monitor.width <= 0 || monitor.height <= 0) return d;  // no monitor: leave the window to the engine
    const bool matches = width == static_cast<std::uint32_t>(monitor.width) && height == static_cast<std::uint32_t>(monitor.height);
    if (!windowed) {
        d.borderless = true;
        d.engineFullscreen = true;
        d.resize = !matches;
    } else if (matches) {
        d.borderless = true;
    }
    return d;
}

void enableDpiAwareness() {
    using SetContextFn = BOOL(WINAPI*)(HANDLE);
    const HMODULE user32 = GetModuleHandleW(L"user32.dll");
    const auto setContext = user32 != nullptr ? reinterpret_cast<SetContextFn>(GetProcAddress(user32, "SetProcessDpiAwarenessContext"))
                                              : nullptr;
    if (setContext == nullptr) {
        // before Windows 10 1703: system DPI aware at least
        spdlog::info("engine: window: SetProcessDpiAwarenessContext unavailable, SetProcessDPIAware: {}", SetProcessDPIAware() != 0);
        return;
    }
    // DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2
    if (setContext(reinterpret_cast<HANDLE>(static_cast<LONG_PTR>(-4))) != 0) {
        spdlog::info("engine: window: process DPI awareness: per-monitor v2 (window and monitor sizes in physical pixels)");
    } else {
        const DWORD e = GetLastError();
        spdlog::info("engine: window: process DPI awareness unchanged (SetProcessDpiAwarenessContext error {}{})", e,
                     e == ERROR_ACCESS_DENIED ? ": already set by the manifest or an earlier call" : "");
    }
}

// The OS cursor over the client area. The engine draws its own GUI cursor (eCGuiCursor2, a sprite) and never hides the OS
// one in a window: it has no WM_SETCURSOR handler, so DefWindowProc sets the class cursor (IDC_ARROW); its ShowCursor(FALSE)
// (popup mode only) just lowers the display counter. In D3D9 mode the system d3d9.dll's window subclass answers
// WM_SETCURSOR with SetCursor(NULL) while the device cursor is hidden (the engine never shows it). Without a D3D9 device we
// do the same; caption and frame keep their cursors (docs/research/window-mode.md 9).
namespace {
WNDPROC g_cursorOriginal = nullptr;
HWND g_cursorWindow = nullptr;
bool g_cursorUnicode = false;

LRESULT CALLBACK cursorWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_SETCURSOR && LOWORD(lp) == HTCLIENT) {
        SetCursor(nullptr);
        return TRUE;
    }
    return g_cursorUnicode ? CallWindowProcW(g_cursorOriginal, hwnd, msg, wp, lp) : CallWindowProcA(g_cursorOriginal, hwnd, msg, wp, lp);
}

void hideOsCursor(HWND window) {
    if (window == nullptr || window == g_cursorWindow) return;
    if (g_cursorWindow != nullptr) {
        spdlog::warn("engine: window: a new game window: the OS cursor stays hidden only over the first one");
        return;
    }
    g_cursorUnicode = IsWindowUnicode(window) != FALSE;
    const auto proc = reinterpret_cast<LONG_PTR>(&cursorWndProc);
    const LONG_PTR previous = g_cursorUnicode ? SetWindowLongPtrW(window, GWLP_WNDPROC, proc) : SetWindowLongPtrA(window, GWLP_WNDPROC, proc);
    if (previous == 0) {
        spdlog::warn("engine: window: subclassing the game window failed (error {}): the OS cursor stays visible", GetLastError());
        return;
    }
    g_cursorOriginal = reinterpret_cast<WNDPROC>(previous);
    g_cursorWindow = window;
    // the cursor already over the window keeps its shape until the next mouse move: hide it now
    POINT p{};
    if (GetCursorPos(&p) && WindowFromPoint(p) == window) SetCursor(nullptr);
    spdlog::info("engine: window: OS cursor hidden over the client area (the game draws its own)");
}
}  // namespace

bool install() {
    const auto& s = sym();
    if (s.appSetResolutionEx == nullptr) {
        spdlog::warn("engine: window: eCApplication::SetResolutionEx not found: fullscreen keeps the requested resolution");
        return false;
    }
    return g_setResolutionEx.install("eCApplication::SetResolutionEx", reinterpret_cast<SetResolutionExFn>(s.appSetResolutionEx),
                                     &detourSetResolutionEx);
}

void onModeSet(HWND__* window, bool windowed, std::uint32_t width, std::uint32_t height, std::uint32_t refresh) {
    // the engine's fullscreen window is topmost from its creation on (eCWindow::Create / SetWindowMode): not topmost at
    // once (a z-order change only, no WM_SIZE: safe inside the device creation and Reset), the placement follows at the
    // first present (the loading screen's first frame can be seconds away)
    if (window != nullptr && !windowed && (GetWindowLongW(window, GWL_EXSTYLE) & WS_EX_TOPMOST) != 0) {
        const UINT async = asyncFlag(window);
        SetWindowPos(window, HWND_NOTOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOOWNERZORDER | async);
        const HWND fg = GetForegroundWindow();
        if (fg != nullptr && fg != window && (GetWindowLongW(fg, GWL_EXSTYLE) & WS_EX_TOPMOST) == 0) {
            SetWindowPos(window, fg, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOOWNERZORDER | async);
        }
        spdlog::info("engine: window: the engine's fullscreen window is no longer topmost");
    }
    hideOsCursor(window);
    g.window = window;
    g.windowed = windowed;
    g.width = width;
    g.height = height;
    g.refresh = refresh;
    g.pending = true;
    g.changing = {};
    spdlog::info("engine: window: mode {}x{} @ {} Hz {} (placed at the next present)", width, height, refresh,
                 windowed ? "windowed" : "fullscreen");
}

void setExclusive(bool on) {
    if (on == g.exclusive) return;
    g.exclusive = on;
    g.exPending = true;
    g.corrections = 0;
    spdlog::info("engine: window: fullscreen mode {}", on ? "exclusive (display mode change)" : "borderless");
}

namespace {

void restoreDisplayModeImpl(const char* why);

// borderless: pending placements, corrections and the keep-on-monitor check; true when the client size may have changed
bool borderlessStep() {
    if (g.window == nullptr) return false;
    if (g.pending) {
        g.pending = false;
        const HMONITOR monitor = g.target != nullptr ? g.target : MonitorFromWindow(g.window, MONITOR_DEFAULTTONEAREST);
        g.target = nullptr;
        Rect mr;
        std::string name;
        if (!monitorRect(monitor, mr, &name)) return false;
        const Decision d = decide(g.windowed, g.width, g.height, mr);
        if (d.resize) {
            // fullscreen at another size (the start from the config / -fullscreen; a request without the detour): the
            // engine switches to the monitor's size itself, its Reset brings the next onModeSet
            if (g.corrections < 2 && requestResolution(static_cast<std::uint32_t>(mr.width), static_cast<std::uint32_t>(mr.height),
                                                       kModePopup, "fullscreen at the monitor's resolution")) {
                ++g.corrections;
                g.target = monitor;
                return false;
            }
            spdlog::warn("engine: window: fullscreen stays at {}x{} (scaled to the monitor's {}x{})", g.width, g.height, mr.width,
                         mr.height);
        } else {
            g.corrections = 0;
        }
        if (d.borderless) {
            place(monitor, mr, name, d.engineFullscreen);
            return true;
        }
        bool changed = false;
        if (g.borderless) {
            g.borderless = false;
            changed = restoreBordered(mr);
        }
        RECT wr{}, cr{};
        GetWindowRect(g.window, &wr);
        GetClientRect(g.window, &cr);
        spdlog::info("engine: window: bordered (the engine's window): window {}, client {}x{}, style 0x{:08x}, monitor {} {}",
                     rectText(wr), cr.right, cr.bottom, static_cast<std::uint32_t>(GetWindowLongW(g.window, GWL_STYLE)), name,
                     rectText(mr));
        return changed;  // the engine's own window: its swapchain follows as before (pixel-identical windowed runs)
    }
    if (!g.borderless) return false;
    // keep the borderless window on its monitor (moved by a tool, Win+Shift+arrow, a monitor change), a few times a second
    const auto now = std::chrono::steady_clock::now();
    if (now - g.lastCheck < std::chrono::milliseconds(250)) return false;
    g.lastCheck = now;
    if (IsIconic(g.window)) return false;
    const LONG style = GetWindowLongW(g.window, GWL_STYLE);
    if ((style & WS_CAPTION) == WS_CAPTION) return false;  // the engine is changing its mode: its ToggleNewMode follows
    const HMONITOR monitor = MonitorFromWindow(g.window, MONITOR_DEFAULTTONEAREST);
    Rect mr;
    std::string name;
    if (!monitorRect(monitor, mr, &name) || mr.width <= 0 || mr.height <= 0) return false;
    if (static_cast<std::uint32_t>(mr.width) != g.width || static_cast<std::uint32_t>(mr.height) != g.height) {
        // on a monitor of another size: the engine changes to it (the same mode)
        if (g.corrections < 2) {
            const bool ok = requestResolution(static_cast<std::uint32_t>(mr.width), static_cast<std::uint32_t>(mr.height),
                                              g.windowed ? kModeWindowed : kModePopup, "the window moved to monitor " + name);
            if (ok) {
                ++g.corrections;
                g.target = monitor;
            }
        }
        return false;
    }
    RECT wr{};
    GetWindowRect(g.window, &wr);
    const bool topmost = (GetWindowLongW(g.window, GWL_EXSTYLE) & WS_EX_TOPMOST) != 0;
    if (wr.left != mr.x || wr.top != mr.y || wr.right - wr.left != mr.width || wr.bottom - wr.top != mr.height || topmost) {
        spdlog::info("engine: window: borderless window at {}{}: placed over its monitor again", rectText(wr), topmost ? " (topmost)" : "");
        place(monitor, mr, name, !g.windowed);
        return true;
    }
    return false;
}

// exclusive fullscreen on `monitor`: its display mode switched to the game's (if needed), the window over it
bool activateExclusive(HMONITOR monitor) {
    const std::wstring device = monitorDevice(monitor);
    if (device.empty()) return false;
    const MonitorModes mm = monitorModes(device);
    const auto pick = pickDisplayMode(mm.modes, g.width, g.height, g.refresh, mm.desktop.refresh);
    if (!pick) {
        spdlog::warn("engine: window: exclusive fullscreen: {} has no {}x{} mode", narrow(device), g.width, g.height);
        return false;
    }
    if (mm.current != *pick) {
        DEVMODEW dm{};
        dm.dmSize = sizeof(dm);
        dm.dmPelsWidth = pick->width;
        dm.dmPelsHeight = pick->height;
        dm.dmDisplayFrequency = pick->refresh;
        dm.dmBitsPerPel = 32;
        dm.dmFields = DM_PELSWIDTH | DM_PELSHEIGHT | DM_DISPLAYFREQUENCY | DM_BITSPERPEL;
        const auto t0 = std::chrono::steady_clock::now();
        const LONG r = ChangeDisplaySettingsExW(device.c_str(), &dm, nullptr, CDS_FULLSCREEN, nullptr);
        const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        if (r != DISP_CHANGE_SUCCESSFUL) {
            spdlog::warn("engine: window: exclusive fullscreen: ChangeDisplaySettingsEx({}, {}) failed: {}", narrow(device),
                         modeText(*pick), r);
            return false;
        }
        g.displayChanged = true;
        g.modeEvent = true;
        spdlog::info("engine: window: exclusive fullscreen: {} switched {} -> {} (desktop {}, game {}x{} @ {} Hz) in {:.0f} ms",
                     narrow(device), modeText(mm.current), modeText(*pick), modeText(mm.desktop), g.width, g.height, g.refresh, ms);
    } else {
        spdlog::info("engine: window: exclusive fullscreen: {} already at {} (game {}x{} @ {} Hz)", narrow(device), modeText(*pick),
                     g.width, g.height, g.refresh);
    }
    Rect mr;
    std::string name;
    if (!monitorRect(monitor, mr, &name)) {
        monitor = MonitorFromWindow(g.window, MONITOR_DEFAULTTONEAREST);
        if (!monitorRect(monitor, mr, &name)) return false;
    }
    place(monitor, mr, name, true);
    g.exActive = true;
    g.exMonitor = monitor;
    g.exMode = *pick;
    g.corrections = 0;
    return true;
}

// exclusive fullscreen (the engine in fullscreen): true when the client size may have changed
bool exclusiveStep() {
    HWND w = g.window;
    const bool front = GetForegroundWindow() == w && !IsIconic(w);
    const bool redo = g.pending || g.exPending;
    g.pending = false;
    g.exPending = false;
    if (front && (redo || !g.exActive)) {
        const HMONITOR monitor = g.target != nullptr ? g.target : MonitorFromWindow(w, MONITOR_DEFAULTTONEAREST);
        g.target = nullptr;
        if (activateExclusive(monitor)) return true;
        // no such mode here / the switch failed: the monitor's desktop size, as borderless fullscreen does
        restoreDisplayModeImpl("the exclusive mode could not be set");
        g.exActive = false;
        const MonitorModes mm = monitorModes(monitorDevice(monitor));
        if (g.corrections < 2 && mm.desktop.width != 0 && (mm.desktop.width != g.width || mm.desktop.height != g.height)) {
            ++g.corrections;
            g.target = monitor;
            requestResolution(mm.desktop.width, mm.desktop.height, kModePopup, "exclusive fullscreen at the desktop's resolution");
            return false;
        }
        Rect mr;
        std::string name;
        if (monitorRect(monitor, mr, &name)) place(monitor, mr, name, true);
        g.exActive = true;  // no further attempts until the next mode change / focus change
        g.exMonitor = nullptr;
        return true;
    }
    if (!front && g.exActive) {
        // focus lost (alt-tab, another window, minimised): the desktop's mode back, the window minimised (as D3D9's
        // exclusive mode does); in front again -> switched again
        g.exActive = false;
        restoreDisplayModeImpl("the game window lost focus");
        if (!IsIconic(w)) ShowWindow(w, SW_SHOWMINNOACTIVE);
        spdlog::info("engine: window: exclusive fullscreen suspended: the window lost focus (minimised)");
        return true;
    }
    if (redo && !front && !IsIconic(w)) {
        // not in front (yet: a start in the background): no display mode change; the window at the back buffer's size in
        // its monitor's corner until it comes to the front
        const HMONITOR monitor = g.target != nullptr ? g.target : MonitorFromWindow(w, MONITOR_DEFAULTTONEAREST);
        g.target = nullptr;
        Rect mr;
        std::string name;
        if (!monitorRect(monitor, mr, &name)) return false;
        mr.width = static_cast<int>(g.width);
        mr.height = static_cast<int>(g.height);
        place(monitor, mr, name, true);
        g.exMonitor = monitor;
        spdlog::info("engine: window: exclusive fullscreen waits for the window to come to the front");
        return true;
    }
    if (!g.exActive || !front) return false;
    // keep the window over its (switched) monitor
    const auto now = std::chrono::steady_clock::now();
    if (now - g.lastCheck < std::chrono::milliseconds(250)) return false;
    g.lastCheck = now;
    const HMONITOR monitor = MonitorFromWindow(w, MONITOR_DEFAULTTONEAREST);
    Rect mr;
    std::string name;
    if (!monitorRect(monitor, mr, &name)) return false;
    RECT wr{};
    GetWindowRect(w, &wr);
    const bool topmost = (GetWindowLongW(w, GWL_EXSTYLE) & WS_EX_TOPMOST) != 0;
    if (wr.left != mr.x || wr.top != mr.y || wr.right - wr.left != mr.width || wr.bottom - wr.top != mr.height || topmost) {
        spdlog::info("engine: window: exclusive window at {}{}: placed over its monitor again", rectText(wr), topmost ? " (topmost)" : "");
        place(monitor, mr, name, true);
        return true;
    }
    return false;
}

void restoreDisplayModeImpl(const char* why) {
    if (!g.displayChanged) return;
    g.displayChanged = false;
    g.modeEvent = true;
    // every attached display whose mode differs from its registry (desktop) mode goes back to it: robust against the
    // device names changing after a mode switch
    std::string done;
    DISPLAY_DEVICEW dd{};
    dd.cb = sizeof(dd);
    for (DWORD i = 0; EnumDisplayDevicesW(nullptr, i, &dd, 0); ++i, dd.cb = sizeof(dd)) {
        if ((dd.StateFlags & DISPLAY_DEVICE_ATTACHED_TO_DESKTOP) == 0) continue;
        const MonitorModes mm = monitorModes(dd.DeviceName);
        if (mm.current == mm.desktop || mm.desktop.width == 0) continue;
        const LONG r = ChangeDisplaySettingsExW(dd.DeviceName, nullptr, nullptr, 0, nullptr);
        done += std::format("{}{} {} -> {} ({})", done.empty() ? "" : ", ", narrow(dd.DeviceName), modeText(mm.current),
                            modeText(mm.desktop), r == DISP_CHANGE_SUCCESSFUL ? std::string("ok") : std::format("error {}", r));
    }
    spdlog::info("engine: window: desktop display mode restored ({}): {}", why, done.empty() ? "nothing to restore" : done);
}

}  // namespace

void restoreDisplayMode(const char* why) { restoreDisplayModeImpl(why); }

Presentation onPresent() {
    Presentation out;
    if (g.window == nullptr) return out;
    const bool engaged = g.exclusive && !g.windowed;
    if (g.changing != std::chrono::steady_clock::time_point{} &&
        std::chrono::steady_clock::now() - g.changing < std::chrono::seconds(3)) {
        // the engine is between SetResolutionEx and its ToggleNewMode: leave the window to it
    } else if (engaged) {
        g.changing = {};
        out.resized = exclusiveStep();
    } else {
        g.changing = {};
        if (g.displayChanged || g.exActive || g.exPending) {
            // exclusive fullscreen ended (setting off / the engine windowed): the desktop's mode back, placed as borderless
            restoreDisplayModeImpl(g.exclusive ? "the game is windowed" : "fullscreen mode borderless");
            if (g.exActive || g.exPending) g.pending = true;
            g.exActive = false;
            g.exPending = false;
        }
        g.exMonitor = nullptr;
        out.resized = borderlessStep();
    }
    // the exclusive swapchain only while the display mode is set and the window in front: on AMD an application-
    // controlled exclusive swapchain that is not held presented at ~4 fps in the background (window-mode.md 8.3)
    out.exclusiveMonitor = engaged && g.exActive ? g.exMonitor : nullptr;
    out.holdExclusive = engaged && g.exActive && g.exMonitor != nullptr;
    out.refreshCap = !engaged && !g.windowed && g.borderless ? static_cast<double>(g.refresh) : 0.0;
    out.displayModeChanged = g.modeEvent;
    g.modeEvent = false;
    return out;
}

std::optional<DisplayMode> pickDisplayMode(std::span<const DisplayMode> modes, std::uint32_t width, std::uint32_t height,
                                           std::uint32_t refresh, std::uint32_t desktopRefresh) {
    std::optional<DisplayMode> best;
    const auto distance = [&](const DisplayMode& m) { return m.refresh > refresh ? m.refresh - refresh : refresh - m.refresh; };
    for (const DisplayMode& m : modes) {
        if (m.width != width || m.height != height) continue;
        if (refresh == 0) {
            if (m.refresh == desktopRefresh) return m;
            if (!best || m.refresh > best->refresh) best = m;
        } else {
            if (m.refresh == refresh) return m;
            if (!best || distance(m) < distance(*best) || (distance(m) == distance(*best) && m.refresh > best->refresh)) best = m;
        }
    }
    return best;
}

std::vector<DisplayMode> orderModes(std::vector<DisplayMode> modes, std::uint32_t desktopRefresh) {
    const auto key = [&](const DisplayMode& m) {
        // per size: the desktop's refresh first, then descending
        return std::tuple(m.width, m.height, m.refresh == desktopRefresh ? 0u : 1u, ~m.refresh);
    };
    std::ranges::sort(modes, [&](const DisplayMode& a, const DisplayMode& b) { return key(a) < key(b); });
    const auto dup = std::ranges::unique(modes);
    modes.erase(dup.begin(), dup.end());
    return modes;
}

bool runCommand(std::string_view args, std::string* out) {
    const auto fail = [&](std::string why) {
        if (out != nullptr) *out = std::move(why);
        return false;
    };
    while (!args.empty() && args.front() == ' ') args.remove_prefix(1);
    while (!args.empty() && args.back() == ' ') args.remove_suffix(1);
    if (args == "state") {
        if (out != nullptr) *out = describe();
        return true;
    }
    if (g.window == nullptr) return fail("no device yet");
    if (args == "fullscreen") {
        Rect mr;
        if (!monitorRect(MonitorFromWindow(g.window, MONITOR_DEFAULTTONEAREST), mr)) return fail("no monitor");
        g.corrections = 0;
        if (!requestResolution(static_cast<std::uint32_t>(mr.width), static_cast<std::uint32_t>(mr.height), kModePopup, "command")) {
            return fail("SetResolution failed");
        }
        if (out != nullptr) *out = std::format("fullscreen {}x{} requested", mr.width, mr.height);
        return true;
    }
    if (args.starts_with("fullscreen ")) {
        // the options menu's path: eCApplication::SetResolutionEx(adapter, w, h, refresh, vsync, fov, current mode)
        std::string_view m = args.substr(11);
        std::uint32_t w = 0, h = 0, hz = 0;
        const auto x = m.find_first_of("xX");
        const auto at = m.find('@');
        const char* end = m.data() + (at == std::string_view::npos ? m.size() : at);
        if (x == std::string_view::npos || std::from_chars(m.data(), m.data() + x, w).ec != std::errc{} ||
            std::from_chars(m.data() + x + 1, end, h).ec != std::errc{} ||
            (at != std::string_view::npos && std::from_chars(m.data() + at + 1, m.data() + m.size(), hz).ec != std::errc{})) {
            return fail("window fullscreen <W>x<H>[@<Hz>]");
        }
        const auto& s = sym();
        auto* a = static_cast<std::uint8_t*>(app());
        if (a == nullptr || s.appSetResolutionEx == nullptr) return fail("eCApplication::SetResolutionEx not found");
        // app +0x170 adapter (the current context desc), +0x6cc refresh, +0x6d0 vsync, +0x6d4 FOV (window-mode.md 8.1)
        const auto adapter = *reinterpret_cast<const unsigned long*>(a + 0x170);
        if (at == std::string_view::npos) hz = *reinterpret_cast<const std::uint32_t*>(a + 0x6cc);
        const bool vsync = *reinterpret_cast<const bool*>(a + 0x6d0);
        const float fov = *reinterpret_cast<const float*>(a + 0x6d4);
        g.corrections = 0;
        spdlog::info("engine: window: SetResolutionEx({}x{} @ {} Hz, fullscreen) (command)", w, h, hz);
        const bool r = reinterpret_cast<SetResolutionExFn>(s.appSetResolutionEx)(a, adapter, w, h, hz, vsync, fov, kModePopup);
        if (out != nullptr) *out = std::format("fullscreen {}x{} @ {} Hz requested -> {}", w, h, hz, r);
        return true;
    }
    if (args == "minimize") {
        // focus loss as alt-tab gives it (exclusive fullscreen: desktop mode back)
        ShowWindow(g.window, SW_MINIMIZE);
        if (out != nullptr) *out = "minimised";
        return true;
    }
    if (args == "restore" || args == "focus") {
        // test tooling: back in front (a background test run cannot take the foreground by itself: Windows' foreground
        // lock; attaching to the foreground window's input queue lifts it for SetForegroundWindow)
        if (IsIconic(g.window)) ShowWindow(g.window, SW_RESTORE);
        bool fg = SetForegroundWindow(g.window) != 0 && GetForegroundWindow() == g.window;
        if (!fg) {
            const HWND current = GetForegroundWindow();
            const DWORD other = current != nullptr ? GetWindowThreadProcessId(current, nullptr) : 0;
            const DWORD self = GetCurrentThreadId();
            const bool attached = other != 0 && other != self && AttachThreadInput(self, other, TRUE) != 0;
            BringWindowToTop(g.window);
            SetForegroundWindow(g.window);
            SetFocus(g.window);
            if (attached) AttachThreadInput(self, other, FALSE);
            fg = GetForegroundWindow() == g.window;
        }
        if (out != nullptr) *out = std::format("{}, foreground {}", args, fg);
        return true;
    }
    if (args == "display") {
        if (out != nullptr) *out = currentDisplayModes();
        return true;
    }
    if (args == "toggle") {
        const auto& s = sym();
        void* a = app();
        if (a == nullptr || s.appToggleFullscreen == nullptr) return fail("eCApplication::ToggleFullscreen not found");
        g.corrections = 0;
        const bool r = reinterpret_cast<ToggleFullscreenFn>(s.appToggleFullscreen)(a);
        if (out != nullptr) *out = std::format("ToggleFullscreen -> {}", r);
        return true;
    }
    const auto x = args.find_first_of("xX");
    if (x != std::string_view::npos) {
        std::uint32_t w = 0, h = 0;
        const auto rw = std::from_chars(args.data(), args.data() + x, w);
        const auto rh = std::from_chars(args.data() + x + 1, args.data() + args.size(), h);
        if (rw.ec == std::errc{} && rh.ec == std::errc{} && rh.ptr == args.data() + args.size() && w >= 320 && h >= 200) {
            g.corrections = 0;
            if (!requestResolution(w, h, kModeWindowed, "command")) return fail("SetResolution failed");
            if (out != nullptr) *out = std::format("windowed {}x{} requested", w, h);
            return true;
        }
    }
    return fail("window fullscreen [<W>x<H>[@<Hz>]] | toggle | <W>x<H> | state | minimize | restore | focus | display");
}

std::string describe() {
    if (g.window == nullptr) return "no window";
    RECT wr{}, cr{};
    GetWindowRect(g.window, &wr);
    GetClientRect(g.window, &cr);
    Rect mr;
    std::string name;
    monitorRect(MonitorFromWindow(g.window, MONITOR_DEFAULTTONEAREST), mr, &name);
    return std::format("{}{} ({} {}x{} @ {} Hz), window {}, client {}x{}, style 0x{:08x}, ex 0x{:08x}, monitor {} {}",
                       g.borderless ? "borderless" : "bordered",
                       g.exclusive && !g.windowed ? (g.exActive ? ", exclusive (display mode set)" : ", exclusive (suspended)") : "",
                       g.windowed ? "windowed" : "fullscreen", g.width, g.height, g.refresh,
                       rectText(wr), cr.right, cr.bottom, static_cast<std::uint32_t>(GetWindowLongW(g.window, GWL_STYLE)),
                       static_cast<std::uint32_t>(GetWindowLongW(g.window, GWL_EXSTYLE)), name, rectText(mr));
}

std::string displaySummary() {
    if (g.window == nullptr) return "no window";
    // the overlay asks every frame: EnumDisplaySettings is slow, refreshed twice a second
    static std::string cached;
    static std::chrono::steady_clock::time_point at{};
    const auto now = std::chrono::steady_clock::now();
    if (!cached.empty() && now - at < std::chrono::milliseconds(500)) return cached;
    at = now;
    const std::wstring device = monitorDevice(MonitorFromWindow(g.window, MONITOR_DEFAULTTONEAREST));
    MonitorModes mm;
    DEVMODEW dm{};
    dm.dmSize = sizeof(dm);
    if (EnumDisplaySettingsW(device.c_str(), ENUM_REGISTRY_SETTINGS, &dm)) mm.desktop = modeOf(dm);
    if (EnumDisplaySettingsW(device.c_str(), ENUM_CURRENT_SETTINGS, &dm)) mm.current = modeOf(dm);
    cached = summaryText(mm);
    return cached;
}

namespace {
std::string summaryText(const MonitorModes& mm) {
    const std::string display = std::format("display {} (desktop {})", modeText(mm.current), modeText(mm.desktop));
    if (g.windowed) {
        return std::format("{} window {}x{}, {}", g.borderless ? "borderless" : "bordered", g.width, g.height, display);
    }
    if (g.exclusive) {
        return std::format("exclusive fullscreen {}x{} @ {} Hz{}, {}", g.width, g.height, g.refresh,
                           g.exActive ? "" : " (suspended: not in front)", display);
    }
    return std::format("borderless fullscreen {}x{}, game refresh {} Hz{}, {}", g.width, g.height, g.refresh,
                       g.refresh != 0 && g.refresh + 1 < mm.current.refresh ? " (frame cap with vsync)" : "", display);
}
}  // namespace

std::string currentDisplayModes() {
    std::string out;
    DISPLAY_DEVICEW dd{};
    dd.cb = sizeof(dd);
    for (DWORD i = 0; EnumDisplayDevicesW(nullptr, i, &dd, 0); ++i, dd.cb = sizeof(dd)) {
        if ((dd.StateFlags & DISPLAY_DEVICE_ATTACHED_TO_DESKTOP) == 0) continue;
        DEVMODEW dm{};
        dm.dmSize = sizeof(dm);
        if (!EnumDisplaySettingsW(dd.DeviceName, ENUM_CURRENT_SETTINGS, &dm)) continue;
        const MonitorModes mm = monitorModes(dd.DeviceName);
        out += std::format("{}{} {} at {},{} (desktop {}){}", out.empty() ? "" : "; ", narrow(dd.DeviceName), modeText(modeOf(dm)),
                           dm.dmPosition.x, dm.dmPosition.y, modeText(mm.desktop),
                           (dd.StateFlags & DISPLAY_DEVICE_PRIMARY_DEVICE) != 0 ? " primary" : "");
    }
    return out;
}

}  // namespace rvk::engine::window
