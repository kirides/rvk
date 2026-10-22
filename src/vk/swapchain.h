#pragma once

#include <cstdint>
#include <vector>

#include "vk/vk.h"

struct HWND__;

namespace rvk::vk {

class Context;

// Swapchain on the game's window. The renderer draws into its own back buffer and copies it into the acquired image at
// present (Swapchain::recordCopy), so swapchain recreation never touches engine-visible resources.
class Swapchain {
public:
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
};

}  // namespace rvk::vk
