#pragma once

// Runs vkQueuePresentKHR on its own thread. In the game's (composited) window the AMD driver blocks inside vkQueuePresentKHR
// until the frame's GPU work has finished and its copy to the window is done (~6 ms per world frame, with FIFO and IMMEDIATE
// alike: docs/research/performance.md 2); on the render thread that serialised CPU and GPU work and capped the game at ~80
// fps. Here the render thread submits, hands the present over and records the next frame meanwhile.
//
// The swapchain is externally synchronised: the render thread calls waitIdle() before it touches the swapchain itself
// (acquire, recreate, destroy). The queue is shared through Context's queue lock.

#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <thread>

#include "vk/vk.h"

namespace rvk::vk {

class Swapchain;

class Presenter {
public:
    Presenter() = default;
    ~Presenter();
    Presenter(const Presenter&) = delete;
    Presenter& operator=(const Presenter&) = delete;

    void start(Swapchain& swapchain);
    void stop();
    bool running() const { return thread_.joinable(); }

    // render thread: queue the present of an acquired, submitted image (waits while an earlier present is still queued)
    void present(std::uint32_t imageIndex);
    // render thread: until the queued present has returned; then the swapchain may be used. Returns the latest present's
    // result since the last call (VK_SUCCESS when none).
    VkResult waitIdle();

    // statistics: summed ms inside vkQueuePresentKHR (present thread); taken and zeroed
    double takePresentMs();

private:
    void run();

    Swapchain* swapchain_ = nullptr;
    std::thread thread_;
    std::mutex mutex_;
    std::condition_variable cv_;
    bool quit_ = false;
    bool pending_ = false;  // a job is queued or running
    std::uint32_t image_ = 0;
    VkResult result_ = VK_SUCCESS;
    double presentMs_ = 0.0;
};

}  // namespace rvk::vk
