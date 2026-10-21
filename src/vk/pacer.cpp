#include "vk/pacer.h"

#include <windows.h>

#include <thread>

namespace rvk::vk {

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

FramePacer::FramePacer() {
    timer_ = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
}

FramePacer::~FramePacer() {
    if (timer_ != nullptr) {
        CloseHandle(timer_);
    }
}

void FramePacer::setRate(double fps) {
    rate_ = fps > 0.0 ? fps : 0.0;
    interval_ = rate_ > 0.0 ? std::chrono::duration_cast<std::chrono::steady_clock::duration>(std::chrono::duration<double>(1.0 / rate_))
                            : std::chrono::steady_clock::duration{};
}

void FramePacer::sleepUntil(std::chrono::steady_clock::time_point t) {
    using namespace std::chrono;
    // the timer for all but the last half millisecond, then spin: timer wake-ups are late by up to ~0.5 ms
    const auto coarse = t - microseconds(500);
    auto now = steady_clock::now();
    if (coarse > now) {
        const auto ticks = duration_cast<nanoseconds>(coarse - now).count() / 100;
        LARGE_INTEGER due{};
        due.QuadPart = -static_cast<LONGLONG>(ticks);  // relative, 100 ns units
        if (timer_ != nullptr && SetWaitableTimerEx(timer_, &due, 0, nullptr, nullptr, nullptr, 0)) {
            WaitForSingleObject(timer_, INFINITE);
        } else {
            Sleep(static_cast<DWORD>(duration_cast<milliseconds>(coarse - now).count()));
        }
    }
    while (steady_clock::now() < t) {
        std::this_thread::yield();
    }
}

void FramePacer::wait() {
    if (rate_ <= 0.0) {
        return;
    }
    const auto target = next_;
    auto now = std::chrono::steady_clock::now();
    if (target > now) {
        sleepUntil(target);
        now = std::chrono::steady_clock::now();
    }
    // keep the cadence when slightly late; after a long frame (loading, a hitch) start over instead of catching up
    next_ = now - target > interval_ ? now + interval_ : target + interval_;
}

}  // namespace rvk::vk
