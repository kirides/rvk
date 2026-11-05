// Resolution fallback (src/engine/display_resolve.*, docs/research/window-mode.md 10).

#include "engine/display_resolve.h"
#include "test.h"

#include <vector>

using namespace rvk::engine::window;

namespace {
MonitorInfo virtualDisplay() {  // the 1920x1200 streaming display
    MonitorInfo m;
    m.rect = {0, 0, 1920, 1200};
    m.work = {0, 0, 1920, 1152};
    m.desktop = {1920, 1200, 60};
    m.modes = {{1280, 800, 60}, {1600, 900, 60}, {1920, 1080, 60}, {1920, 1200, 60}, {1920, 1200, 30}};
    m.frameWidth = 2;
    m.frameHeight = 33;
    m.name = "DISPLAY9";
    return m;
}
MonitorInfo bigMonitor() {
    MonitorInfo m;
    m.rect = {0, 0, 2560, 1440};
    m.work = {0, 0, 2560, 1392};
    m.desktop = {2560, 1440, 120};
    m.modes = {{1600, 900, 60}, {1920, 1080, 120}, {1920, 1080, 60}, {2560, 1440, 120}, {2560, 1440, 60}, {3840, 2160, 60}};
    m.frameWidth = 2;
    m.frameHeight = 33;
    return m;
}
}  // namespace

TEST(display_resolve_borderless) {
    // config 2560x1440 on the 1920x1200 display: the monitor's desktop size
    Resolved r = resolve({2560, 1440, 120, Kind::Borderless}, virtualDisplay());
    CHECK(r.changed && r.kind == Kind::Borderless && r.width == 1920 && r.height == 1200 && r.refresh == 60);
    CHECK(!r.reason.empty());
    // already the desktop size: unchanged
    r = resolve({2560, 1440, 120, Kind::Borderless}, bigMonitor());
    CHECK(!r.changed && r.width == 2560 && r.refresh == 120 && r.reason.empty());
    // refresh 0 (any) stays any
    r = resolve({1600, 900, 0, Kind::Borderless}, bigMonitor());
    CHECK(r.width == 2560 && r.height == 1440 && r.refresh == 0);
}

TEST(display_resolve_exclusive) {
    const MonitorInfo vd = virtualDisplay();
    // enumerated mode: kept; refresh the size lacks: the highest below
    Resolved r = resolve({1920, 1080, 60, Kind::Exclusive}, vd);
    CHECK(!r.changed);
    r = resolve({1920, 1200, 144, Kind::Exclusive}, vd);
    CHECK(r.changed && r.width == 1920 && r.height == 1200 && r.refresh == 60 && r.kind == Kind::Exclusive);
    // 2560x1440 is not offered: the nearest of its aspect not above the desktop = 1920x1080
    r = resolve({2560, 1440, 120, Kind::Exclusive}, vd);
    CHECK(r.changed && r.width == 1920 && r.height == 1080 && r.refresh == 60);
    // 3840x2160 likewise
    r = resolve({3840, 2160, 60, Kind::Exclusive}, vd);
    CHECK(r.width == 1920 && r.height == 1080);
    // an odd aspect (21:9 3440x1440): no mode of that aspect -> nearest area (1920x1080 vs 1920x1200: 1920x1080 is closer)
    r = resolve({3440, 1440, 60, Kind::Exclusive}, vd);
    CHECK(r.width <= 1920 && r.height <= 1200 && r.changed);
    // no usable mode at all: the desktop mode
    MonitorInfo empty = vd;
    empty.modes.clear();
    r = resolve({2560, 1440, 60, Kind::Exclusive}, empty);
    CHECK(r.width == 1920 && r.height == 1200);
}

TEST(display_resolve_windowed) {
    const MonitorInfo vd = virtualDisplay();
    // fits: kept (also a size that is no display mode)
    Resolved r = resolve({1600, 900, 60, Kind::Windowed}, vd);
    CHECK(!r.changed);
    r = resolve({1366, 768, 60, Kind::Windowed}, vd);
    CHECK(!r.changed && r.width == 1366);
    // larger than the monitor (the user's case): borderless over the monitor, not a window with a caption
    r = resolve({2560, 1440, 60, Kind::Windowed}, vd);
    CHECK(r.changed && r.kind == Kind::Borderless && r.width == 1920 && r.height == 1200);
    r = resolve({3840, 2160, 60, Kind::Windowed}, vd);
    CHECK(r.kind == Kind::Borderless);
    // exactly the monitor: borderless (the existing rule)
    r = resolve({1920, 1200, 60, Kind::Windowed}, vd);
    CHECK(r.kind == Kind::Borderless && r.width == 1920);
    // fits the monitor but not the work area: shrunk, aspect kept, the frame included
    r = resolve({1920, 1150, 60, Kind::Windowed}, vd);
    CHECK(r.changed && r.kind == Kind::Windowed && r.height + 33 <= 1152 && r.width <= 1918);
    CHECK(r.width * 1150 / 1920 - r.height <= 2 && r.height - r.width * 1150 / 1920 <= 2);
    // wider than the monitor in one dimension only: borderless
    r = resolve({2560, 1080, 60, Kind::Windowed}, vd);
    CHECK(r.kind == Kind::Borderless);
    // too small: the minimum
    r = resolve({320, 200, 60, Kind::Windowed}, vd);
    CHECK(r.width == kMinWidth && r.height == kMinHeight);
    // no monitor information: unchanged
    r = resolve({2560, 1440, 60, Kind::Borderless}, MonitorInfo{});
    CHECK(!r.changed && r.width == 2560);
}

TEST(display_resolve_engine_modes) {
    const MonitorInfo vd = virtualDisplay();
    Resolved r = resolve({1366, 768, 60, Kind::Windowed}, vd);
    const auto modes = engineModes(vd.modes, r);
    CHECK(modes.size() == vd.modes.size() + 1);
    CHECK(modes.back().width == 1366 && modes.back().refresh == 60);
    // a listed mode adds nothing; none of the monitor's absent sizes appears
    r = resolve({1600, 900, 60, Kind::Windowed}, vd);
    CHECK(engineModes(vd.modes, r).size() == vd.modes.size());
    // 1920x1080 with a caption does not fit 1920 wide: the largest listed mode of its aspect that fits
    r = resolve({1920, 1080, 60, Kind::Windowed}, vd);
    CHECK(r.changed && r.kind == Kind::Windowed && r.width == 1600 && r.height == 900);
    CHECK(pickRefresh(vd.modes, 1920, 1200, 120) == 60);
    CHECK(pickRefresh(vd.modes, 1920, 1200, 45) == 30);
    CHECK(pickRefresh(vd.modes, 1920, 1200, 0) == 0);
}
