#pragma once

// Which resolution the game uses when the one it was configured with is not available on the monitor it runs on
// (docs/research/window-mode.md 10). Pure logic (unit-tested): the monitor's facts come in as data, the engine's saved
// config is never rewritten (the caller keeps the preferred size and resolves again when the monitor changes).

#include <cstdint>
#include <string>
#include <vector>

#include "engine/window_mode.h"

namespace rvk::engine::window {

enum class Kind { Windowed, Borderless, Exclusive };

struct MonitorInfo {
    Rect rect;                       // rcMonitor (physical pixels, the process is per-monitor DPI aware)
    Rect work;                       // rcWork (without the taskbar)
    DisplayMode desktop;             // the monitor's current desktop mode (its size normally equals rect)
    std::vector<DisplayMode> modes;  // 32 bpp display modes of the monitor
    int frameWidth = 0;              // non-client overhead of the engine's windowed style
    int frameHeight = 0;
    std::string name;
};

struct Request {
    std::uint32_t width = 0, height = 0, refresh = 0;
    Kind kind = Kind::Windowed;
};

struct Resolved {
    std::uint32_t width = 0, height = 0, refresh = 0;
    Kind kind = Kind::Windowed;
    bool changed = false;   // differs from the request (size, refresh or kind)
    std::string reason;     // empty when unchanged
};

constexpr std::uint32_t kMinWidth = 640, kMinHeight = 480;

// borderless -> the monitor's desktop size; exclusive -> the mode when enumerated, else the best match (same aspect, nearest
// area not above the desktop, highest refresh <= requested), else the desktop mode; windowed -> the request when it fits the
// work area, shrunk (aspect kept) when it fits the monitor only, and a request that is as large as or larger than the monitor
// becomes borderless over the monitor (the player's intent: the whole screen).
Resolved resolve(const Request& request, const MonitorInfo& monitor);

// The refresh rate of the mode list for a size: the requested one if the size has it, else the highest one below it, else the
// lowest above it; 0 (any) stays 0. Pure.
std::uint32_t pickRefresh(const std::vector<DisplayMode>& modes, std::uint32_t width, std::uint32_t height, std::uint32_t refresh);

// The mode list the engine gets (CreateGraphicsSubsystem needs an exact width x height x refresh match, also windowed): the
// monitor's modes, plus the resolved mode itself when the list lacks it (a windowed size that is no display mode).
std::vector<DisplayMode> engineModes(std::vector<DisplayMode> monitorModes, const Resolved& resolved);

const char* kindName(Kind k);

}  // namespace rvk::engine::window
