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

    // acquire: signals `acquired`; returns false when the swapchain must be recreated (out of date / minimised)
    bool acquire(VkSemaphore acquired, std::uint32_t& imageIndex);
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
    std::vector<VkImage> images_;
    std::vector<VkImageView> views_;
    std::uint32_t minImageCount_ = 2;
    std::vector<VkSemaphore> rendered_;  // one per image (present may hold it until the image is re-acquired)
};

}  // namespace rvk::vk
