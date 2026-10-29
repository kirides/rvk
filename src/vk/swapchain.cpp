#include "vk/swapchain.h"

#include <algorithm>
#include <format>
#include <string>

#include <windows.h>

#include <spdlog/spdlog.h>

#include "vk/context.h"
#include "vk/fg_swapchain.h"

namespace rvk::vk {

Swapchain::Swapchain() = default;
Swapchain::~Swapchain() = default;

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
    // exclusive fullscreen (docs/research/window-mode.md 8): the surface queries and the swapchain carry the exclusive mode
    // and its monitor
    VkSurfaceFullScreenExclusiveWin32InfoEXT fseWin32{VK_STRUCTURE_TYPE_SURFACE_FULL_SCREEN_EXCLUSIVE_WIN32_INFO_EXT};
    fseWin32.hmonitor = static_cast<HMONITOR>(fseMonitor_);
    VkSurfaceFullScreenExclusiveInfoEXT fse{VK_STRUCTURE_TYPE_SURFACE_FULL_SCREEN_EXCLUSIVE_INFO_EXT, &fseWin32};
    fse.fullScreenExclusive = VK_FULL_SCREEN_EXCLUSIVE_APPLICATION_CONTROLLED_EXT;
    VkPhysicalDeviceSurfaceInfo2KHR surfaceInfo{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SURFACE_INFO_2_KHR, &fse};
    surfaceInfo.surface = surface_;
    bool exclusive = fseMonitor_ != nullptr && ctx_->features().fullScreenExclusive && !fgWanted_;  // not with frame generation
    VkSurfaceCapabilitiesKHR caps{};
    if (exclusive) {
        VkSurfaceCapabilitiesFullScreenExclusiveEXT fseCaps{VK_STRUCTURE_TYPE_SURFACE_CAPABILITIES_FULL_SCREEN_EXCLUSIVE_EXT};
        VkSurfaceCapabilities2KHR caps2{VK_STRUCTURE_TYPE_SURFACE_CAPABILITIES_2_KHR, &fseCaps};
        const VkResult r = vkGetPhysicalDeviceSurfaceCapabilities2KHR(ctx_->physicalDevice(), &surfaceInfo, &caps2);
        if (r != VK_SUCCESS || !fseCaps.fullScreenExclusiveSupported) {
            spdlog::warn("vulkan: exclusive fullscreen not supported on this surface / monitor ({}, supported {}): normal swapchain",
                         resultString(r), fseCaps.fullScreenExclusiveSupported != 0);
            exclusive = false;
        } else {
            caps = caps2.surfaceCapabilities;
        }
    }
    if (!exclusive) {
        vkGetPhysicalDeviceSurfaceCapabilitiesKHR(ctx_->physicalDevice(), surface_, &caps);
    }
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

    std::vector<VkPresentModeKHR> modes;
    if (exclusive) {
        vkGetPhysicalDeviceSurfacePresentModes2EXT(ctx_->physicalDevice(), &surfaceInfo, &n, nullptr);
        modes.resize(n);
        vkGetPhysicalDeviceSurfacePresentModes2EXT(ctx_->physicalDevice(), &surfaceInfo, &n, modes.data());
    } else {
        vkGetPhysicalDeviceSurfacePresentModesKHR(ctx_->physicalDevice(), surface_, &n, nullptr);
        modes.resize(n);
        vkGetPhysicalDeviceSurfacePresentModesKHR(ctx_->physicalDevice(), surface_, &n, modes.data());
    }
    modes.resize(n);
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

    VkSwapchainCreateInfoKHR ci{VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR, exclusive ? &fse : nullptr};
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

    std::string offered;
    for (const auto m : modes) offered += std::format("{}{}", offered.empty() ? "" : ",", static_cast<int>(m));

    // ---- AMD's frame generation swapchain (vk/fg_swapchain.h) ----
    bool wantFg = fgWanted_;
    if (wantFg && !fg_) fg_ = std::make_unique<FgSwapchain>();
    if (wantFg && !fg_->loaded() && !fg_->load(fgLoader_)) {
        fgReason_ = "the FidelityFX loader does not load";
        wantFg = false;
    }
    if (fgActive_ && !wantFg) {
        destroySwapchain();  // leaving frame generation: the API's swapchain goes first (the surface takes one)
    }
    if (wantFg) {
        ci.minImageCount = 2;  // the app-facing replacement images ("always specify 2")
        ci.oldSwapchain = VK_NULL_HANDLE;
        bool ok = false;
        if (fgActive_) {
            destroyImageObjects();  // views over the replacement images (new ones when the extent changed)
            ok = fg_->recreate(ci) == VK_SUCCESS;
            if (!ok) {
                spdlog::error("vulkan: frame generation swapchain: recreate failed");
                destroySwapchain();
            }
        } else {
            destroySwapchain();  // our own swapchain first; the API creates one on the surface
            ok = fg_->create(*ctx_, ci, &fgReason_);
        }
        if (ok) {
            fgActive_ = true;
            swapchain_ = fg_->handle();
            format_ = chosen.format;
            extent_ = extent;
            presentMode_ = mode;
            fseActive_ = false;
            minImageCount_ = 2;
            fgReason_.clear();
            if (fgDone_ == VK_NULL_HANDLE) {
                VkSemaphoreTypeCreateInfo ti{VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO};
                ti.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE;
                VkSemaphoreCreateInfo si{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO, &ti};
                vkCreateSemaphore(ctx_->device(), &si, nullptr, &fgDone_);
                names::object(*ctx_, fgDone_, VK_OBJECT_TYPE_SEMAPHORE, "rvk frame generation presents done");
                fgPresents_ = 0;
            }
            collectImages(fg_->images());
            spdlog::info("vulkan: swapchain {}x{} (AMD frame generation swapchain '{}'), {} replacement images, format {}, present mode {} "
                         "(offered {})",
                         extent.width, extent.height, fg_->providerName(), images_.size(), static_cast<int>(format_), static_cast<int>(mode),
                         offered);
            return true;
        }
        spdlog::warn("vulkan: frame generation swapchain not available ({}): normal swapchain", fgReason_);
        fgWanted_ = false;
    }

    VkSwapchainKHR old = swapchain_;
    releaseFullScreenExclusive();  // the old swapchain's exclusive mode: the new one acquires its own
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
    presentMode_ = mode;
    fseActive_ = exclusive;

    std::vector<VkImage> got;
    vkGetSwapchainImagesKHR(ctx_->device(), swapchain_, &n, nullptr);
    got.resize(n);
    vkGetSwapchainImagesKHR(ctx_->device(), swapchain_, &n, got.data());
    names::object(*ctx_, swapchain_, VK_OBJECT_TYPE_SWAPCHAIN_KHR, "rvk swapchain");
    minImageCount_ = imageCount;
    collectImages(std::move(got));
    spdlog::info("vulkan: swapchain {}x{}, {} images, format {}, present mode {} (offered {}){}", extent.width, extent.height, images_.size(),
                 static_cast<int>(format_), static_cast<int>(mode), offered,
                 exclusive ? std::format(", exclusive fullscreen (application controlled, monitor {})", fseMonitor_) : std::string());
    return true;
}

bool Swapchain::recreate(std::uint32_t width, std::uint32_t height) {
    ctx_->waitIdle();
    return build(width, height);
}

void Swapchain::destroyImageObjects() {
    if (ctx_ == nullptr) {
        return;
    }
    for (auto s : rendered_) {
        vkDestroySemaphore(ctx_->device(), s, nullptr);
    }
    rendered_.clear();
    for (auto v : views_) {
        if (v != VK_NULL_HANDLE) vkDestroyImageView(ctx_->device(), v, nullptr);
    }
    views_.clear();
    images_.clear();
}

// the images the swapchain gave (or the frame generation swapchain's replacement images): a view each (the overlay draws into
// the normal swapchain's images; with frame generation the present callback draws into the real ones) and a "rendered" semaphore
bool Swapchain::collectImages(std::vector<VkImage> images) {
    images_ = std::move(images);
    const std::uint32_t n = static_cast<std::uint32_t>(images_.size());
    views_.assign(n, VK_NULL_HANDLE);
    for (std::uint32_t i = 0; i < n; ++i) {
        if (!fgActive_) {
            VkImageViewCreateInfo vci{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
            vci.image = images_[i];
            vci.viewType = VK_IMAGE_VIEW_TYPE_2D;
            vci.format = format_;
            vci.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
            if (vkCreateImageView(ctx_->device(), &vci, nullptr, &views_[i]) != VK_SUCCESS) {
                views_[i] = VK_NULL_HANDLE;
            }
        }
        if (ctx_->namesEnabled()) {
            names::object(*ctx_, images_[i], VK_OBJECT_TYPE_IMAGE, std::format("swapchain image {}", i));
            if (views_[i] != VK_NULL_HANDLE) names::object(*ctx_, views_[i], VK_OBJECT_TYPE_IMAGE_VIEW, std::format("swapchain image {} (view)", i));
        }
    }
    rendered_.resize(n);
    for (auto& s : rendered_) {
        VkSemaphoreCreateInfo si{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
        vkCreateSemaphore(ctx_->device(), &si, nullptr, &s);
        names::object(*ctx_, s, VK_OBJECT_TYPE_SEMAPHORE, "rvk swapchain rendered");
    }
    return true;
}

void Swapchain::destroySwapchain() {
    if (ctx_ == nullptr) {
        return;
    }
    destroyImageObjects();
    if (fgActive_) {
        fg_->destroy();  // waits for the presents, the swapchain context, the swapchain (the generator's context is gone by now)
        fgActive_ = false;
        swapchain_ = VK_NULL_HANDLE;
    }
    if (swapchain_ != VK_NULL_HANDLE) {
        vkDestroySwapchainKHR(ctx_->device(), swapchain_, nullptr);  // releases a held exclusive mode
        swapchain_ = VK_NULL_HANDLE;
    }
    fseHeld_ = false;
    fseActive_ = false;
}

void Swapchain::releaseFrameGen() {
    fgWanted_ = false;
    if (ctx_ != nullptr && fgActive_) {
        ctx_->waitIdle();
        destroySwapchain();
    }
}

void Swapchain::destroy() {
    if (ctx_ == nullptr) {
        return;
    }
    ctx_->waitIdle();
    destroySwapchain();
    if (fgDone_ != VK_NULL_HANDLE) {
        vkDestroySemaphore(ctx_->device(), fgDone_, nullptr);
        fgDone_ = VK_NULL_HANDLE;
    }
    fg_.reset();
    if (surface_ != VK_NULL_HANDLE) {
        vkDestroySurfaceKHR(ctx_->instance(), surface_, nullptr);
        surface_ = VK_NULL_HANDLE;
    }
}

bool Swapchain::acquire(VkSemaphore acquired, std::uint32_t& imageIndex) {
    acquireNotReady_ = false;
    if (swapchain_ == VK_NULL_HANDLE) {
        return false;
    }
    if (fgActive_) {
        // the replacement images: the API signals `acquired` through its acquire queue once the image is free
        const VkResult r = fg_->acquire(acquired, imageIndex);
        if (r == VK_SUCCESS || r == VK_SUBOPTIMAL_KHR) return true;
        if (r == VK_NOT_READY) {
            acquireNotReady_ = true;
        } else {
            spdlog::error("vulkan: frame generation swapchain acquire: {}", resultString(r));
        }
        return false;
    }
    const VkResult r = vkAcquireNextImageKHR(ctx_->device(), swapchain_, UINT64_MAX, acquired, VK_NULL_HANDLE, &imageIndex);
    if (r == VK_SUCCESS || r == VK_SUBOPTIMAL_KHR) {
        return true;
    }
    if (r == VK_ERROR_FULL_SCREEN_EXCLUSIVE_MODE_LOST_EXT) {
        fseHeld_ = false;
        fseLost_ = true;
        return false;
    }
    if (r != VK_ERROR_OUT_OF_DATE_KHR) {
        spdlog::error("vulkan: vkAcquireNextImageKHR: {}", resultString(r));
    }
    return false;
}

VkResult Swapchain::acquireFullScreenExclusive() {
    if (swapchain_ == VK_NULL_HANDLE || !fseActive_) return VK_ERROR_INITIALIZATION_FAILED;
    if (fseHeld_) return VK_SUCCESS;
    const VkResult r = vkAcquireFullScreenExclusiveModeEXT(ctx_->device(), swapchain_);
    fseHeld_ = r == VK_SUCCESS;
    return r;
}

void Swapchain::releaseFullScreenExclusive() {
    if (swapchain_ != VK_NULL_HANDLE && fseHeld_) {
        vkReleaseFullScreenExclusiveModeEXT(ctx_->device(), swapchain_);
    }
    fseHeld_ = false;
}

VkResult Swapchain::present(std::uint32_t imageIndex) {
    VkPresentInfoKHR pi{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
    pi.waitSemaphoreCount = 1;
    pi.pWaitSemaphores = &rendered_[imageIndex];
    pi.swapchainCount = 1;
    pi.pSwapchains = &swapchain_;
    pi.pImageIndices = &imageIndex;
    if (fgActive_) {
        // the game queue's submits (the API's, ours) go through the queue lock; the present itself needs none (it presents on
        // its own queue) but the interpolation it dispatches is queued on the game queue behind us
        const VkResult r = fg_->present(ctx_->queue(), pi);
        // everything this present queued (interpolation, composition hand-over) sits before this signal in the game queue
        const std::uint64_t v = fgPresents_.load() + 1;
        VkSemaphoreSubmitInfo sig{VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO};
        sig.semaphore = fgDone_;
        sig.value = v;
        sig.stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
        VkSubmitInfo2 si{VK_STRUCTURE_TYPE_SUBMIT_INFO_2};
        si.signalSemaphoreInfoCount = 1;
        si.pSignalSemaphoreInfos = &sig;
        ctx_->queueSubmit(si);
        fgPresents_.store(v);
        return r;
    }
    return ctx_->queuePresent(pi);
}

std::uint64_t Swapchain::frameGenPresentsDone() const {
    if (fgDone_ == VK_NULL_HANDLE || ctx_ == nullptr) return 0;
    std::uint64_t v = 0;
    vkGetSemaphoreCounterValue(ctx_->device(), fgDone_, &v);
    return v;
}

bool Swapchain::waitFrameGenDone(std::uint64_t n, std::uint64_t timeoutNs) {
    if (fgDone_ == VK_NULL_HANDLE || n == 0) return true;
    VkSemaphoreWaitInfo wi{VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO};
    wi.semaphoreCount = 1;
    wi.pSemaphores = &fgDone_;
    wi.pValues = &n;
    return vkWaitSemaphores(ctx_->device(), &wi, timeoutNs) == VK_SUCCESS;
}

}  // namespace rvk::vk
