#include "engine/window_mode.h"

#include <windows.h>

#include <charconv>
#include <chrono>
#include <cstdint>
#include <format>
#include <string>
#include <string_view>

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
};
State g;

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
        if (mode == kModePopup && monitorRect(monitor, mr, &name) && mr.width > 0 && mr.height > 0 &&
            (width != static_cast<unsigned long>(mr.width) || height != static_cast<unsigned long>(mr.height))) {
            spdlog::info("engine: window: fullscreen request {}x{} -> the monitor's {}x{} ({})", width, height, mr.width, mr.height, name);
            width = static_cast<unsigned long>(mr.width);
            height = static_cast<unsigned long>(mr.height);
        }
    }
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

bool install() {
    const auto& s = sym();
    if (s.appSetResolutionEx == nullptr) {
        spdlog::warn("engine: window: eCApplication::SetResolutionEx not found: fullscreen keeps the requested resolution");
        return false;
    }
    return g_setResolutionEx.install("eCApplication::SetResolutionEx", reinterpret_cast<SetResolutionExFn>(s.appSetResolutionEx),
                                     &detourSetResolutionEx);
}

void onModeSet(HWND__* window, bool windowed, std::uint32_t width, std::uint32_t height) {
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
    g.window = window;
    g.windowed = windowed;
    g.width = width;
    g.height = height;
    g.pending = true;
    spdlog::info("engine: window: mode {}x{} {} (placed at the next present)", width, height, windowed ? "windowed" : "fullscreen");
}

bool onPresent() {
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
    return fail("window fullscreen | toggle | <W>x<H> | state");
}

std::string describe() {
    if (g.window == nullptr) return "no window";
    RECT wr{}, cr{};
    GetWindowRect(g.window, &wr);
    GetClientRect(g.window, &cr);
    Rect mr;
    std::string name;
    monitorRect(MonitorFromWindow(g.window, MONITOR_DEFAULTTONEAREST), mr, &name);
    return std::format("{} ({} {}x{}), window {}, client {}x{}, style 0x{:08x}, ex 0x{:08x}, monitor {} {}",
                       g.borderless ? "borderless" : "bordered", g.windowed ? "windowed" : "fullscreen", g.width, g.height,
                       rectText(wr), cr.right, cr.bottom, static_cast<std::uint32_t>(GetWindowLongW(g.window, GWL_STYLE)),
                       static_cast<std::uint32_t>(GetWindowLongW(g.window, GWL_EXSTYLE)), name, rectText(mr));
}

}  // namespace rvk::engine::window
