#pragma once

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include "vk/vk.h"

struct HWND__;

namespace rvk::vk {

class Context;
class FgSwapchain;

// Swapchain on the game's window. The renderer draws into its own back buffer and copies it into the acquired image at
// present (Swapchain::recordCopy), so swapchain recreation never touches engine-visible resources.
class Swapchain {
public:
    Swapchain();
    ~Swapchain();
    bool create(Context& ctx, HWND__* window, std::uint32_t width, std::uint32_t height, bool vsync);
    void destroy();
    bool recreate(std::uint32_t width, std::uint32_t height);

    // acquire: signals `acquired`; returns false when the swapchain must be recreated (out of date / minimised) or exclusive
    // fullscreen was lost (exclusiveLost() is then set: no recreation, re-acquire the exclusive mode later)
    bool acquire(VkSemaphore acquired, std::uint32_t& imageIndex);

    // Exclusive fullscreen (VK_EXT_full_screen_exclusive, docs/research/window-mode.md 8): `monitor` != null = the next
    // build creates the swapchain with VkSurfaceFullScreenExclusiveInfoEXT APPLICATION_CONTROLLED on that monitor (needs
    // Context::features().fullScreenExclusive); null = a normal swapchain. Takes effect at the next recreate.
    void setFullScreenExclusive(void* monitor) { fseMonitor_ = monitor; }
    bool fullScreenExclusiveConfigured() const { return fseActive_; }  // the current swapchain was built with it
    // vkAcquire/ReleaseFullScreenExclusiveModeEXT (render thread, the present thread idle)
    VkResult acquireFullScreenExclusive();
    void releaseFullScreenExclusive();
    bool fullScreenExclusiveHeld() const { return fseHeld_; }
    // a present / acquire returned VK_ERROR_FULL_SCREEN_EXCLUSIVE_MODE_LOST_EXT: the exclusive mode is no longer held
    void noteExclusiveLost() { fseHeld_ = false; }
    bool exclusiveLost() const { return fseLost_; }
    bool takeExclusiveLost() {
        const bool r = fseLost_;
        fseLost_ = false;
        return r;
    }
    // Frame generation (vk/fg_swapchain.h, docs/research/frame-generation.md F3): the next build creates AMD's frame generation
    // swapchain instead of ours (a normal swapchain when `on` is false). Exclusive fullscreen is not possible with it. The caller
    // recreates (Device::setFrameGenSwapchain); a build that cannot create it falls back to a normal swapchain (frameGenReason()).
    // `loaderDll` = the FidelityFX loader.
    void setFrameGen(bool on, const std::filesystem::path& loaderDll = {}) {
        fgWanted_ = on;
        if (on) fgLoader_ = loaderDll;
    }
    bool frameGenWanted() const { return fgWanted_; }
    // tears the frame generation swapchain down without building another (shutdown): the generator's context must be gone
    void releaseFrameGen();
    bool frameGenActive() const { return fg_ != nullptr && fgActive_; }
    FgSwapchain* frameGen() { return fgActive_ ? fg_.get() : nullptr; }
    const std::string& frameGenReason() const { return fgReason_; }
    VkSwapchainKHR handle() const { return swapchain_; }  // the proxy handle in frame generation mode
    // the last acquire returned VK_NOT_READY (frame generation: too many images acquired): drop the frame, no recreation
    bool acquireNotReady() const { return acquireNotReady_; }
    // frame generation: the present thread finished presenting `n` frames, and so did the GPU work (interpolation) they queued
    std::uint64_t frameGenPresentsDone() const;
    bool waitFrameGenDone(std::uint64_t n, std::uint64_t timeoutNs);
    std::uint64_t frameGenPresentsQueued() const { return fgPresents_.load(); }
    VkPresentModeKHR presentMode() const { return presentMode_; }
    // present: waits on the per-image "rendered" semaphore (renderedSemaphore(imageIndex)). Thread-safe against submits, but
    // not against the other swapchain functions (Presenter::waitIdle before those)
    VkResult present(std::uint32_t imageIndex);

    VkSemaphore renderedSemaphore(std::uint32_t imageIndex) const { return rendered_[imageIndex]; }
    VkImage image(std::uint32_t index) const { return images_[index]; }
    VkImageView view(std::uint32_t index) const { return views_[index]; }
    std::uint32_t imageCount() const { return static_cast<std::uint32_t>(images_.size()); }
    std::uint32_t minImageCount() const { return minImageCount_; }
    VkFormat format() const { return format_; }
    VkExtent2D extent() const { return extent_; }
    bool valid() const { return swapchain_ != VK_NULL_HANDLE; }
    void setVsync(bool vsync) { vsync_ = vsync; }

private:
    bool build(std::uint32_t width, std::uint32_t height);
    void destroySwapchain();
    void destroyImageObjects();  // views and semaphores over the current images
    bool collectImages(std::vector<VkImage> images);

    Context* ctx_ = nullptr;
    HWND__* window_ = nullptr;
    VkSurfaceKHR surface_ = VK_NULL_HANDLE;
    VkSwapchainKHR swapchain_ = VK_NULL_HANDLE;
    VkFormat format_ = VK_FORMAT_B8G8R8A8_UNORM;
    VkExtent2D extent_{};
    bool vsync_ = true;
    VkPresentModeKHR presentMode_ = VK_PRESENT_MODE_FIFO_KHR;
    void* fseMonitor_ = nullptr;  // HMONITOR of the exclusive swapchain to build, null = none
    bool fseActive_ = false;      // the current swapchain has the exclusive structures
    bool fseHeld_ = false;        // vkAcquireFullScreenExclusiveModeEXT succeeded and was not lost / released since
    bool fseLost_ = false;        // acquire reported the exclusive mode lost (taken by the device)
    std::vector<VkImage> images_;
    std::vector<VkImageView> views_;
    std::uint32_t minImageCount_ = 2;
    std::vector<VkSemaphore> rendered_;  // one per image (present may hold it until the image is re-acquired)

    // frame generation backend
    std::unique_ptr<FgSwapchain> fg_;
    bool fgWanted_ = false;
    bool fgActive_ = false;       // swapchain_ is the frame generation swapchain's proxy handle
    std::filesystem::path fgLoader_;
    std::string fgReason_;
    bool acquireNotReady_ = false;
    std::atomic<std::uint64_t> fgPresents_{0};  // presents handed to the API (present thread)
    VkSemaphore fgDone_ = VK_NULL_HANDLE; // timeline: signalled after each present returned (everything it queued is done)
};

}  // namespace rvk::vk
