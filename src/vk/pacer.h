#pragma once

// Present-rate limiter. FIFO should throttle to the display's refresh, but in the game's window the AMD driver returns every
// present at once (the menu ran at ~1500 fps with vsync on, while a plain test window is throttled). D3D9 was throttled by
// DWM, so with vsync the device paces itself to the refresh rate of the window's monitor; [vulkan] max_fps caps the rate.
//
// Schedule: frame k is released at t0 + k * interval (a fixed cadence: a frame that is a little late is made up by the next
// one, so the average rate is exact); a frame later than one whole interval restarts the cadence instead of bursting. With
// vsync the interval is DWM's measured refresh period (119.96 Hz, not the nominal 120) and the cadence is phase-locked to
// DWM's vblank, so the frames do not drift through the refresh cycle.
//
// Waiting adapted from GD3D11's fpslimiter.h (same author): QPC time base, a high-resolution waitable timer for the bulk of
// the wait and a short YieldProcessor spin for the rest. The spin margin adapts to the measured timer lateness (at most
// 0.5 ms). Without high-resolution timers (before Windows 10 1803) a normal waitable timer with timeBeginPeriod(1) is used;
// the period is restored in the destructor.

#include <cstdint>

struct HWND__;

namespace rvk::vk {

// refresh rate of the monitor that shows most of the window, 0 if unknown
double monitorRefreshRate(HWND__* window);

// DWM's composition clock: refresh period and the QPC time of the last vblank (false without DWM timing)
struct VblankClock {
    std::int64_t lastVblank = 0;  // QPC ticks
    std::int64_t period = 0;      // QPC ticks
    double rate() const;          // Hz
};
bool dwmVblankClock(VblankClock& out);

class FramePacer {
public:
    FramePacer();
    ~FramePacer();
    FramePacer(const FramePacer&) = delete;
    FramePacer& operator=(const FramePacer&) = delete;

    // fps 0 = off. refreshHz (vsync: the window monitor's nominal refresh, else 0): lock the cadence to DWM's vblank when fps
    // is the refresh rate or an integer fraction of it (the rate then comes from DWM's measured period)
    void setRate(double fps, double refreshHz = 0.0);
    double rate() const { return rate_; }
    bool vblankLocked() const { return vblank_; }
    // sleeps until the next frame slot
    void wait();

    struct Stats {
        std::uint32_t waits = 0;    // frames that waited
        std::uint32_t restarts = 0; // cadence restarts (a frame later than one interval)
        double sleptMs = 0.0;       // in the timer
        double spunMs = 0.0;        // in the final spin
        double lateMs = 0.0;        // summed timer wake-up lateness past the planned wake-up
        double maxLateMs = 0.0;
        double releaseErrorMs = 0.0;  // summed |release time - slot| of the waits (spin overshoot)
    };
    Stats takeStats();

private:
    void sleepUntil(std::int64_t t);
    void resyncPhase(std::int64_t now);

    void* timer_ = nullptr;
    bool highResolution_ = false;
    bool periodRaised_ = false;  // timeBeginPeriod(1) in effect (fallback timer)
    double freq_ = 0.0;          // QPC ticks per ms
    double rate_ = 0.0;
    bool vblank_ = false;
    int divisor_ = 1;            // vblanks per frame when locked
    double nominalHz_ = 0.0;     // the monitor's refresh rate the DWM clock is checked against
    double interval_ = 0.0;      // QPC ticks (fractional: the cadence keeps the exact rate)
    double next_ = 0.0;          // QPC ticks of the next slot, 0 = not started
    double marginTicks_ = 0.0;   // spin margin
    double lateEma_ = 0.0;       // QPC ticks
    std::int64_t lastPhaseCheck_ = 0;
    Stats stats_;
};

}  // namespace rvk::vk
