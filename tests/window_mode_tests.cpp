// Borderless fullscreen decisions (src/engine/window_mode.*, docs/research/window-mode.md).

#include "engine/window_mode.h"
#include "test.h"

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
