// Borderless fullscreen decisions (src/engine/window_mode.*, docs/research/window-mode.md).

#include "engine/window_mode.h"
#include "test.h"

#include <vector>

using namespace rvk::engine::window;

TEST(window_mode_decide) {
    const Rect primary{0, 0, 2560, 1440};
    const Rect right{2560, 0, 1920, 1080};
    // fullscreen at the monitor's resolution: borderless, the engine's popup style stays, no resize
    Decision d = decide(false, 2560, 1440, primary);
    CHECK(d.borderless && d.engineFullscreen && !d.resize);
    // fullscreen at another resolution: borderless at the monitor's size, the engine must switch first
    d = decide(false, 1600, 900, primary);
    CHECK(d.borderless && d.engineFullscreen && d.resize);
    d = decide(false, 2560, 1440, right);
    CHECK(d.borderless && d.resize);
    // windowed at exactly the monitor's resolution: borderless as well, the engine stays windowed
    d = decide(true, 2560, 1440, primary);
    CHECK(d.borderless && !d.engineFullscreen && !d.resize);
    d = decide(true, 1920, 1080, right);
    CHECK(d.borderless && !d.engineFullscreen);
    // any other windowed size: the engine's bordered window
    d = decide(true, 1600, 900, primary);
    CHECK(!d.borderless && !d.engineFullscreen && !d.resize);
    d = decide(true, 2560, 1080, primary);  // only one side matching
    CHECK(!d.borderless);
    // no monitor information: the window is left to the engine
    d = decide(false, 1600, 900, Rect{});
    CHECK(!d.borderless && !d.resize);
    CHECK(kModePopup == 0x90000000u && kModeWindowed == 0x00c80000u);
}

TEST(window_mode_pick_display_mode) {
    const std::vector<DisplayMode> modes = {{2560, 1440, 60}, {2560, 1440, 120}, {2560, 1440, 360}, {1920, 1080, 60},
                                            {1920, 1080, 144}, {1600, 900, 75}};
    // the exact mode
    CHECK(pickDisplayMode(modes, 1920, 1080, 60, 120) == (DisplayMode{1920, 1080, 60}));
    CHECK(pickDisplayMode(modes, 2560, 1440, 360, 120) == (DisplayMode{2560, 1440, 360}));
    // refresh 0: the desktop's refresh if the size has it, else the highest
    CHECK(pickDisplayMode(modes, 2560, 1440, 0, 120) == (DisplayMode{2560, 1440, 120}));
    CHECK(pickDisplayMode(modes, 1920, 1080, 0, 120) == (DisplayMode{1920, 1080, 144}));
    // a refresh the size lacks: the nearest, the higher on a tie
    CHECK(pickDisplayMode(modes, 1920, 1080, 100, 120) == (DisplayMode{1920, 1080, 60}));
    CHECK(pickDisplayMode(modes, 2560, 1440, 90, 120) == (DisplayMode{2560, 1440, 120}));
    CHECK(pickDisplayMode(modes, 1600, 900, 60, 120) == (DisplayMode{1600, 900, 75}));
    // no mode of that size
    CHECK(!pickDisplayMode(modes, 1280, 720, 60, 120).has_value());
    CHECK(!pickDisplayMode({}, 1920, 1080, 60, 60).has_value());
}

TEST(window_mode_order_modes) {
    const std::vector<DisplayMode> in = {{2560, 1440, 60}, {1920, 1080, 60}, {2560, 1440, 120}, {2560, 1440, 360},
                                         {1920, 1080, 120}, {2560, 1440, 60}, {1920, 1080, 48}};
    const auto out = orderModes(in, 120);
    // sizes ascending, per size the desktop's refresh first (the engine's RefreshRate 0 takes the first), then descending
    const std::vector<DisplayMode> want = {{1920, 1080, 120}, {1920, 1080, 60}, {1920, 1080, 48},
                                           {2560, 1440, 120}, {2560, 1440, 360}, {2560, 1440, 60}};
    CHECK(out == want);
    // the desktop's refresh missing at a size: descending
    const auto out2 = orderModes({{1600, 900, 60}, {1600, 900, 75}}, 120);
    CHECK(out2 == (std::vector<DisplayMode>{{1600, 900, 75}, {1600, 900, 60}}));
}
