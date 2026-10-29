#include "vk/presenter.h"

#include <windows.h>

#include <chrono>

#include "core/stall_sampler.h"
#include "vk/swapchain.h"

namespace rvk::vk {

Presenter::~Presenter() { stop(); }

void Presenter::start(Swapchain& swapchain) {
    stop();
    swapchain_ = &swapchain;
    quit_ = false;
    pending_ = false;
    result_ = VK_SUCCESS;
    thread_ = std::thread([this] { run(); });
}

void Presenter::stop() {
    if (!thread_.joinable()) {
        return;
    }
    {
        const std::lock_guard lock(mutex_);
        quit_ = true;
    }
    cv_.notify_all();
    thread_.join();
}

void Presenter::present(std::uint32_t imageIndex) {
    std::unique_lock lock(mutex_);
    cv_.wait(lock, [&] { return !pending_; });
    image_ = imageIndex;
    pending_ = true;
    lock.unlock();
    cv_.notify_all();
}

VkResult Presenter::waitIdle() {
    std::unique_lock lock(mutex_);
    cv_.wait(lock, [&] { return !pending_; });
    const VkResult r = result_;
    result_ = VK_SUCCESS;
    return r;
}

void Presenter::waitDone() {
    std::unique_lock lock(mutex_);
    cv_.wait(lock, [&] { return !pending_; });
}

double Presenter::takePresentMs() {
    const std::lock_guard lock(mutex_);
    const double ms = presentMs_;
    presentMs_ = 0.0;
    return ms;
}

void Presenter::run() {
    SetThreadDescription(GetCurrentThread(), L"rvk present");
    std::unique_lock lock(mutex_);
    for (;;) {
        cv_.wait(lock, [&] { return pending_ || quit_; });
        if (!pending_) {
            return;  // quit with nothing queued
        }
        const std::uint32_t image = image_;
        lock.unlock();
        const auto t0 = std::chrono::steady_clock::now();
        diag::StallSampler* sampler = diag::presentSampler();  // [debug] present_stalls
        if (sampler != nullptr) sampler->enter();
        const VkResult r = swapchain_->present(image);
        if (sampler != nullptr) sampler->leave();
        const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        lock.lock();
        presentMs_ += ms;
        // keep the first non-success result until the render thread collects it
        if (result_ == VK_SUCCESS) {
            result_ = r;
        }
        pending_ = false;
        cv_.notify_all();
    }
}

}  // namespace rvk::vk
