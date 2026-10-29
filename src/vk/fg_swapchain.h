#pragma once

// AMD's frame generation swapchain (FidelityFX API, effect "frame generation swapchain VK"; docs/research/frame-generation.md 2.1 (i),
// F3 result). It replaces our VkSwapchainKHR: the application renders into the context's replacement ("fake back buffer") images and
// presents through the context's replacement functions; the context interpolates (the frame generation callback model), composes
// the UI through the present callback and presents the generated and the real frame on its own threads with its own pacing.
//
// vk::Swapchain owns one of these while frame generation is wanted (Swapchain::setFrameGen). The frame generation context
// (vk::FrameGen) is configured with this swapchain's handle, callbacks and mutex (FrameGen::configure).
//
// Rules learned in the probe and the game (frame-generation.md 5.3, F3 result):
//  * the swapchain provider is pinned to the newest one the DLL itself provides ("3.1.7"); the driver-provided "4.0.0 *" crashes in
//    its destroy function and is never picked
//  * four distinct VkQueues (Context::frameGenQueues): the game queue is shared with us (vkQueueSubmit goes through
//    Context::queueSubmit, the queue lock, through the queue info's submit hook)
//  * teardown: frame generation context disabled / destroyed by its owner, then waitForPresents, then the swapchain context, then
//    the swapchain (destroy())
//  * the replacement images are handed over in VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL
//  * FFX contexts are not thread safe: contextMutex() covers every ffxConfigure / ffxDispatch(prepare) / create / destroy of the
//    frame generation context (render thread) and present() (the present thread: the interpolation dispatch runs inside it)

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include "vk/vk.h"

namespace rvk::vk {

class Context;

// pacing statistics of the real presents' gaps (ms): [debug] perf's sampler; pure (tests/framegen_tests.cpp). Gaps over `stallMs` are
// counted as stalls and kept out of the moments; even / odd = every other gap (generated -> real and real -> generated)
struct FgGapStats {
    std::size_t n = 0, stalls = 0;
    double mean = 0, stddev = 0, p1 = 0, p50 = 0, p99 = 0, max = 0;
    double evenMean = 0, evenStd = 0, oddMean = 0, oddStd = 0;
};
FgGapStats fgGapStats(const std::vector<double>& gapsMs, double stallMs = 100.0);

// pure: the image layout FFX's resource states (FfxApiResourceState bit values) stand for (tests/framegen_tests.cpp)
VkImageLayout fgLayoutOfState(std::uint32_t state);

// The UI composed on top of every presented frame (generated and real): called by the present callback on AMD's threads (or the
// present thread) with the frame already copied into `target`, which is in COLOR_ATTACHMENT_OPTIMAL
struct FgUiHook {
    std::function<bool()> wants;  // anything to draw? false skips the render pass
    std::function<void(VkCommandBuffer cmd, VkImageView target, VkExtent2D extent)> record;
};

struct FgPresentSample {
    double seconds = 0.0;        // QPC time the real present counter was seen to change
    std::uint64_t count = 0;     // ... its new value
};

class FgSwapchain {
public:
    FgSwapchain() = default;
    FgSwapchain(const FgSwapchain&) = delete;
    FgSwapchain& operator=(const FgSwapchain&) = delete;
    ~FgSwapchain();

    // loads the FidelityFX loader (and, through it, the frame generation DLL next to it); false (logged) when it cannot
    bool load(const std::filesystem::path& loaderDll);
    bool loaded() const { return module_ != nullptr; }

    // Creates the swapchain context and its swapchain from `ci` (surface, extent, format, present mode ...; the replacement
    // images are minImageCount = 2). `reason` says why not. The queues are Context::queue() and Context::frameGenQueues().
    bool create(Context& ctx, const VkSwapchainCreateInfoKHR& ci, std::string* reason);
    // in place (resize, present mode change): the context, its threads and semaphores stay; the replacement images are new when
    // the extent changed. The frame generation context of the same size must not be in use (its owner drops it first).
    VkResult recreate(const VkSwapchainCreateInfoKHR& ci);
    // WaitForPresents + the swapchain context + the swapchain (the frame generation context must be gone)
    void destroy();
    bool valid() const { return swapchain_ != VK_NULL_HANDLE; }
    VkSwapchainKHR handle() const { return swapchain_; }
    const std::string& providerName() const { return provider_; }

    std::vector<VkImage> images() const;  // the replacement images
    VkResult acquire(VkSemaphore signal, std::uint32_t& imageIndex);
    // `queue` = the game queue. Takes contextMutex(); returns the result of the previous real present (the API reports it late)
    VkResult present(VkQueue queue, const VkPresentInfoKHR& info);
    std::uint64_t realPresentCount() const;  // presents of the real swapchain since creation (generated frames included)
    // waits until every queued present was made (GPU work included); idempotent
    void waitForPresents();

    std::mutex& contextMutex() { return mutex_; }

    // ---- the present callback (FfxApiPresentCallbackFunc) ----
    // pass both to ffxConfigure (FrameGen::configure): they copy the frame into the real image and run the UI hook
    const void* presentCallback() const;
    void* presentUser() { return this; }
    void setUi(FgUiHook hook);

    // ---- statistics ----
    struct Counters {
        std::uint64_t callbacks = 0;       // present callback calls (real + generated frames)
        std::uint64_t generatedFrames = 0; // ... of them generated
        std::uint64_t uiFrames = 0;        // ... with a UI render
        double callbackMs = 0.0;           // summed CPU time in the callback
    };
    Counters takeCounters();
    // the real presents' times: a spinning thread watches the counter (diagnostics only: [debug] perf, a core's worth of CPU)
    void startSampler();
    void stopSampler();
    std::vector<FgPresentSample> takeSamples();

private:
    friend struct FgCallbacks;
    long onPresent(void* params);
    void destroyViews();
    VkImageView viewOf(VkImage image, VkFormat format);

    void* module_ = nullptr;  // HMODULE
    struct Functions;
    Functions* fn_ = nullptr;
    Context* ctx_ = nullptr;
    void* swapchainContext_ = nullptr;  // ffxContext
    VkSwapchainKHR swapchain_ = VK_NULL_HANDLE;
    VkFormat format_ = VK_FORMAT_B8G8R8A8_UNORM;
    std::string provider_;
    std::mutex mutex_;

    std::mutex uiMutex_;
    FgUiHook ui_;
    std::unordered_map<VkImage, VkImageView> views_;
    std::mutex counterMutex_;
    Counters counters_;

    std::atomic<bool> sampling_{false};
    std::thread sampler_;
    std::mutex sampleMutex_;
    std::vector<FgPresentSample> samples_;
};

}  // namespace rvk::vk
