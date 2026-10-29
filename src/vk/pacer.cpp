#include "vk/pacer.h"

#include <windows.h>

#include <dwmapi.h>
#include <timeapi.h>

#include <algorithm>
#include <cmath>

#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "winmm.lib")

namespace rvk::vk {
namespace {

std::int64_t qpc() {
    LARGE_INTEGER li;
    QueryPerformanceCounter(&li);
    return li.QuadPart;
}

}  // namespace

double monitorRefreshRate(HWND__* window) {
    HMONITOR monitor = MonitorFromWindow(window, MONITOR_DEFAULTTONEAREST);
    MONITORINFOEXW mi{};
    mi.cbSize = sizeof(mi);
    if (monitor == nullptr || !GetMonitorInfoW(monitor, &mi)) {
        return 0.0;
    }
    DEVMODEW dm{};
    dm.dmSize = sizeof(dm);
    if (!EnumDisplaySettingsW(mi.szDevice, ENUM_CURRENT_SETTINGS, &dm)) {
        return 0.0;
    }
    // 0 and 1 mean "the hardware's default rate"
    return dm.dmDisplayFrequency > 1 ? static_cast<double>(dm.dmDisplayFrequency) : 0.0;
}

double VblankClock::rate() const {
    LARGE_INTEGER f;
    QueryPerformanceFrequency(&f);
    return period > 0 ? static_cast<double>(f.QuadPart) / static_cast<double>(period) : 0.0;
}

bool dwmVblankClock(VblankClock& out) {
    DWM_TIMING_INFO info{};
    info.cbSize = sizeof(info);
    // since Windows 8.1 only the null window is accepted: the timing of DWM's composition clock
    if (FAILED(DwmGetCompositionTimingInfo(nullptr, &info)) || info.qpcRefreshPeriod == 0 || info.qpcVBlank == 0) {
        return false;
    }
    out.lastVblank = static_cast<std::int64_t>(info.qpcVBlank);
    out.period = static_cast<std::int64_t>(info.qpcRefreshPeriod);
    return true;
}

FramePacer::FramePacer() {
    LARGE_INTEGER f;
    QueryPerformanceFrequency(&f);
    freq_ = static_cast<double>(f.QuadPart) / 1000.0;
    timer_ = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
    highResolution_ = timer_ != nullptr;
    if (timer_ == nullptr) {
        timer_ = CreateWaitableTimerExW(nullptr, nullptr, 0, TIMER_ALL_ACCESS);
    }
    marginTicks_ = 0.5 * freq_;
}

FramePacer::~FramePacer() {
    if (periodRaised_) {
        timeEndPeriod(1);
    }
    if (timer_ != nullptr) {
        CloseHandle(timer_);
    }
}

void FramePacer::setRate(double fps, double refreshHz) {
    fps = fps > 0.0 ? fps : 0.0;
    VblankClock clock;
    // locked when fps is the refresh rate or an integer fraction of it (60 on 120 Hz: every second vblank), and DWM's clock
    // runs at the window monitor's nominal refresh (it is the primary display's clock; under load its measured period
    // also strays)
    divisor_ = 1;
    nominalHz_ = refreshHz;
    bool locked = false;
    if (refreshHz > 0.0 && fps > 0.0 && dwmVblankClock(clock) && std::abs(clock.rate() - refreshHz) < 0.005 * refreshHz) {
        const double n = std::round(clock.rate() / fps);
        if (n >= 1.0 && n <= 8.0 && std::abs(clock.rate() / n - fps) < 0.01 * fps) {
            locked = true;
            divisor_ = static_cast<int>(n);
        }
    }
    rate_ = locked ? clock.rate() / divisor_ : fps;
    vblank_ = locked;
    interval_ = locked ? static_cast<double>(clock.period) * divisor_ : (rate_ > 0.0 ? 1000.0 * freq_ / rate_ : 0.0);
    next_ = 0.0;
    if (rate_ > 0.0 && !highResolution_ && !periodRaised_) {
        // the fallback timer wakes on the system tick (15.6 ms by default)
        periodRaised_ = timeBeginPeriod(1) == TIMERR_NOERROR;
    }
}

void FramePacer::resyncPhase(std::int64_t now) {
    lastPhaseCheck_ = now;
    VblankClock clock;
    if (!dwmVblankClock(clock)) {
        return;
    }
    if (std::abs(clock.rate() - nominalHz_) >= 0.005 * nominalHz_) {
        return;  // a stray measurement: keep the current period and phase
    }
    const double period = static_cast<double>(clock.period);
    interval_ = period * divisor_;  // follows DWM's measured period
    if (next_ == 0.0) {
        // the first vblank after now
        const double since = std::fmod(static_cast<double>(now - clock.lastVblank), period);
        next_ = static_cast<double>(now) + (period - since);
        return;
    }
    // slot phase relative to the vblank grid, in (-period/2, period/2]: pull it toward 0 by at most a tenth of a period
    double phase = std::fmod(next_ - static_cast<double>(clock.lastVblank), period);
    if (phase < 0.0) phase += period;
    if (phase > period / 2) phase -= period;
    next_ -= std::clamp(phase, -0.1 * period, 0.1 * period);
}

void FramePacer::sleepUntil(std::int64_t t) {
    std::int64_t now = qpc();
    const std::int64_t wake = t - static_cast<std::int64_t>(marginTicks_);
    if (wake > now && timer_ != nullptr) {
        LARGE_INTEGER due{};
        due.QuadPart = -static_cast<LONGLONG>(static_cast<double>(wake - now) / freq_ * 10000.0);  // relative, 100 ns units
        if (SetWaitableTimerEx(timer_, &due, 0, nullptr, nullptr, nullptr, 0)) {
            WaitForSingleObject(timer_, INFINITE);
        }
        const std::int64_t woke = qpc();
        stats_.sleptMs += static_cast<double>(woke - now) / freq_;
        const double late = static_cast<double>(std::max<std::int64_t>(0, woke - wake));
        stats_.lateMs += late / freq_;
        stats_.maxLateMs = std::max(stats_.maxLateMs, late / freq_);
        // spin margin: 1.5x the typical lateness + 50 us, within [0.1, 0.5] ms
        lateEma_ = lateEma_ == 0.0 ? late : 0.9 * lateEma_ + 0.1 * late;
        marginTicks_ = std::clamp(1.5 * lateEma_ + 0.05 * freq_, 0.1 * freq_, 0.5 * freq_);
        now = woke;
    }
    const std::int64_t spin0 = now;
    while (now < t) {
        YieldProcessor();
        now = qpc();
    }
    stats_.spunMs += static_cast<double>(now - spin0) / freq_;
}

void FramePacer::sleepFor(double ms) {
    if (ms <= 0.0 || freq_ <= 0.0) return;
    sleepUntil(qpc() + static_cast<std::int64_t>(ms * freq_));
}

void FramePacer::wait() {
    if (interval_ <= 0.0) {
        return;
    }
    std::int64_t now = qpc();
    if (vblank_ && (next_ == 0.0 || static_cast<double>(now - lastPhaseCheck_) > 500.0 * freq_)) {
        resyncPhase(now);
    }
    if (next_ == 0.0) {
        next_ = static_cast<double>(now);
    }
    const auto slot = static_cast<std::int64_t>(next_);
    if (now < slot) {
        ++stats_.waits;
        sleepUntil(slot);
        now = qpc();
        stats_.releaseErrorMs += static_cast<double>(now - slot) / freq_;
    }
    if (static_cast<double>(now) - next_ > interval_) {
        // more than a whole frame late (loading, a hitch): restart the cadence instead of catching up with a burst
        ++stats_.restarts;
        next_ = static_cast<double>(now) + interval_;
        if (vblank_ && static_cast<double>(now - lastPhaseCheck_) > 50.0 * freq_) {
            lastPhaseCheck_ = 0;  // re-align to the vblank grid at the next frame (DWM is asked at most every 50 ms)
        }
    } else {
        next_ += interval_;
    }
}

FramePacer::Stats FramePacer::takeStats() {
    const Stats s = stats_;
    stats_ = {};
    return s;
}

}  // namespace rvk::vk
