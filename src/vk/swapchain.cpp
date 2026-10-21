#include "vk/swapchain.h"

#include <algorithm>

#include <windows.h>

#include <spdlog/spdlog.h>

#include "vk/context.h"

namespace rvk::vk {

bool Swapchain::create(Context& ctx, HWND__* window, std::uint32_t width, std::uint32_t height, bool vsync) {
    ctx_ = &ctx;
    window_ = window;
    vsync_ = vsync;
    VkWin32SurfaceCreateInfoKHR sci{VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR};
    sci.hinstance = GetModuleHandleW(nullptr);
    sci.hwnd = window;
    if (const VkResult r = vkCreateWin32SurfaceKHR(ctx.instance(), &sci, nullptr, &surface_); r != VK_SUCCESS) {
        spdlog::error("vulkan: vkCreateWin32SurfaceKHR: {}", resultString(r));
        return false;
    }
    VkBool32 supported = VK_FALSE;
    vkGetPhysicalDeviceSurfaceSupportKHR(ctx.physicalDevice(), ctx.queueFamily(), surface_, &supported);
    if (!supported) {
        spdlog::error("vulkan: the queue family cannot present to the game window");
        return false;
    }
    return build(width, height);
}

bool Swapchain::build(std::uint32_t width, std::uint32_t height) {
    VkSurfaceCapabilitiesKHR caps{};
    vkGetPhysicalDeviceSurfaceCapabilitiesKHR(ctx_->physicalDevice(), surface_, &caps);
    if (caps.currentExtent.width == 0 || caps.currentExtent.height == 0) {
        return false;  // minimised
    }
    VkExtent2D extent = caps.currentExtent;
    if (extent.width == 0xFFFFFFFFu) {
        extent = {std::clamp(width, caps.minImageExtent.width, caps.maxImageExtent.width),
                  std::clamp(height, caps.minImageExtent.height, caps.maxImageExtent.height)};
    }

    std::uint32_t n = 0;
    vkGetPhysicalDeviceSurfaceFormatsKHR(ctx_->physicalDevice(), surface_, &n, nullptr);
    std::vector<VkSurfaceFormatKHR> formats(n);
    vkGetPhysicalDeviceSurfaceFormatsKHR(ctx_->physicalDevice(), surface_, &n, formats.data());
    VkSurfaceFormatKHR chosen = formats.empty() ? VkSurfaceFormatKHR{VK_FORMAT_B8G8R8A8_UNORM, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR}
                                                : formats[0];
    for (const auto& f : formats) {
        // D3D9's X8R8G8B8 back buffer is UNORM (no sRGB conversion on write)
        if ((f.format == VK_FORMAT_B8G8R8A8_UNORM || f.format == VK_FORMAT_R8G8B8A8_UNORM) &&
            f.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) {
            chosen = f;
            break;
        }
    }

    vkGetPhysicalDeviceSurfacePresentModesKHR(ctx_->physicalDevice(), surface_, &n, nullptr);
    std::vector<VkPresentModeKHR> modes(n);
    vkGetPhysicalDeviceSurfacePresentModesKHR(ctx_->physicalDevice(), surface_, &n, modes.data());
    VkPresentModeKHR mode = VK_PRESENT_MODE_FIFO_KHR;
    if (!vsync_) {
        for (auto m : {VK_PRESENT_MODE_MAILBOX_KHR, VK_PRESENT_MODE_IMMEDIATE_KHR}) {
            if (std::ranges::find(modes, m) != modes.end()) {
                mode = m;
                break;
            }
        }
    }

    std::uint32_t imageCount = std::max(caps.minImageCount + 1, 3u);
    if (caps.maxImageCount != 0) {
        imageCount = std::min(imageCount, caps.maxImageCount);
    }

    VkSwapchainKHR old = swapchain_;
    VkSwapchainCreateInfoKHR ci{VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR};
    ci.surface = surface_;
    ci.minImageCount = imageCount;
    ci.imageFormat = chosen.format;
    ci.imageColorSpace = chosen.colorSpace;
    ci.imageExtent = extent;
    ci.imageArrayLayers = 1;
    ci.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    ci.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    ci.preTransform = caps.currentTransform;
    ci.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    ci.presentMode = mode;
    ci.clipped = VK_TRUE;
    ci.oldSwapchain = old;
    VkSwapchainKHR created = VK_NULL_HANDLE;
    if (const VkResult r = vkCreateSwapchainKHR(ctx_->device(), &ci, nullptr, &created); r != VK_SUCCESS) {
        spdlog::error("vulkan: vkCreateSwapchainKHR: {}", resultString(r));
        return false;
    }
    destroySwapchain();
    swapchain_ = created;
    format_ = chosen.format;
    extent_ = extent;

    vkGetSwapchainImagesKHR(ctx_->device(), swapchain_, &n, nullptr);
    images_.resize(n);
    vkGetSwapchainImagesKHR(ctx_->device(), swapchain_, &n, images_.data());
    rendered_.resize(n);
    for (auto& s : rendered_) {
        VkSemaphoreCreateInfo si{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
        vkCreateSemaphore(ctx_->device(), &si, nullptr, &s);
    }
    spdlog::info("vulkan: swapchain {}x{}, {} images, format {}, present mode {}", extent.width, extent.height, n,
                 static_cast<int>(format_), static_cast<int>(mode));
    return true;
}

bool Swapchain::recreate(std::uint32_t width, std::uint32_t height) {
    ctx_->waitIdle();
    return build(width, height);
}

void Swapchain::destroySwapchain() {
    if (ctx_ == nullptr) {
        return;
    }
    for (auto s : rendered_) {
        vkDestroySemaphore(ctx_->device(), s, nullptr);
    }
    rendered_.clear();
    images_.clear();
    if (swapchain_ != VK_NULL_HANDLE) {
        vkDestroySwapchainKHR(ctx_->device(), swapchain_, nullptr);
        swapchain_ = VK_NULL_HANDLE;
    }
}

void Swapchain::destroy() {
    if (ctx_ == nullptr) {
        return;
    }
    ctx_->waitIdle();
    destroySwapchain();
    if (surface_ != VK_NULL_HANDLE) {
        vkDestroySurfaceKHR(ctx_->instance(), surface_, nullptr);
        surface_ = VK_NULL_HANDLE;
    }
}

bool Swapchain::acquire(VkSemaphore acquired, std::uint32_t& imageIndex) {
    if (swapchain_ == VK_NULL_HANDLE) {
        return false;
    }
    const VkResult r = vkAcquireNextImageKHR(ctx_->device(), swapchain_, UINT64_MAX, acquired, VK_NULL_HANDLE, &imageIndex);
    if (r == VK_SUCCESS || r == VK_SUBOPTIMAL_KHR) {
        return true;
    }
    if (r != VK_ERROR_OUT_OF_DATE_KHR) {
        spdlog::error("vulkan: vkAcquireNextImageKHR: {}", resultString(r));
    }
    return false;
}

VkResult Swapchain::present(std::uint32_t imageIndex) {
    VkPresentInfoKHR pi{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
    pi.waitSemaphoreCount = 1;
    pi.pWaitSemaphores = &rendered_[imageIndex];
    pi.swapchainCount = 1;
    pi.pSwapchains = &swapchain_;
    pi.pImageIndices = &imageIndex;
    return ctx_->queuePresent(pi);
}

}  // namespace rvk::vk
