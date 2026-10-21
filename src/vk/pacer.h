#pragma once

// Present-rate limiter. FIFO should throttle to the display's refresh, but in the game's window the AMD driver returns every
// present at once (the menu ran at ~1500 fps with vsync on, while a plain test window is throttled). D3D9 was throttled by
// DWM, so with vsync the device paces itself to the refresh rate of the window's monitor; when FIFO does block, the frames
// already arrive at that rate and the pacer never sleeps.

#include <chrono>

struct HWND__;

namespace rvk::vk {

// refresh rate of the monitor that shows most of the window, 0 if unknown
double monitorRefreshRate(HWND__* window);

class FramePacer {
public:
    FramePacer();
    ~FramePacer();
    FramePacer(const FramePacer&) = delete;
    FramePacer& operator=(const FramePacer&) = delete;

    void setRate(double fps);  // 0 = off
    double rate() const { return rate_; }
    // sleeps until the next frame slot
    void wait();

private:
    void sleepUntil(std::chrono::steady_clock::time_point t);

    void* timer_ = nullptr;
    double rate_ = 0.0;
    std::chrono::steady_clock::duration interval_{};
    std::chrono::steady_clock::time_point next_{};
};

}  // namespace rvk::vk
