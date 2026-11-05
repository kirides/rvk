#include "rhi/vulkan/game_present.h"

#include <spdlog/spdlog.h>

#include "rhi/vulkan/wrap.h"
#include "vk/context.h"
#include "vk/debug_names.h"
#include "vk/fg_swapchain.h"
#include "vk/frames.h"
#include "vk/image.h"

namespace rvk::rhi::vulkan {

VkGamePresenter::VkGamePresenter(GameDevice& device, vk::Context& ctx, vk::Frames& frames) : dev_(device), ctx_(ctx), frames_(frames) {}

VkGamePresenter::~VkGamePresenter() { destroy(); }

bool VkGamePresenter::create(void* window, std::uint32_t width, std::uint32_t height, bool vsync) {
    if (!sc_.create(ctx_, static_cast<HWND__*>(window), width, height, vsync)) return false;
    presenter_.start(sc_);
    for (VkSemaphore& s : acquireSemaphores_) {
        VkSemaphoreCreateInfo sci{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
        vkCreateSemaphore(ctx_.device(), &sci, nullptr, &s);
        vk::names::object(ctx_, s, VK_OBJECT_TYPE_SEMAPHORE, "rvk swapchain acquire");
    }
    return true;
}

void VkGamePresenter::destroy() {
    presenter_.stop();
    fgWrapper_.reset();
    fgWrapperFor_ = nullptr;
    for (VkSemaphore& s : acquireSemaphores_) {
        if (s != VK_NULL_HANDLE) {
            vkDestroySemaphore(ctx_.device(), s, nullptr);
            s = VK_NULL_HANDLE;
        }
    }
    sc_.destroy();
    haveImage_ = false;
}

PresentInfo VkGamePresenter::info() const {
    PresentInfo i;
    i.valid = sc_.valid();
    i.extent = sc_.extent();
    i.format = sc_.format();
    i.minImageCount = sc_.minImageCount();
    i.imageCount = sc_.imageCount();
    i.presentMode = static_cast<int>(sc_.presentMode());
    return i;
}

PresentStatus VkGamePresenter::waitIdle(std::string* error) {
    const VkResult r = presenter_.waitIdle();
    switch (r) {
    case VK_SUCCESS: return PresentStatus::Ok;
    case VK_SUBOPTIMAL_KHR: return PresentStatus::Suboptimal;
    case VK_ERROR_OUT_OF_DATE_KHR: return PresentStatus::OutOfDate;
    case VK_ERROR_FULL_SCREEN_EXCLUSIVE_MODE_LOST_EXT: return PresentStatus::ExclusiveLost;
    default:
        if (error != nullptr) *error = vk::resultString(r);
        return PresentStatus::Error;
    }
}

void VkGamePresenter::releaseFrameGen() {
    fgWrapper_.reset();
    fgWrapperFor_ = nullptr;
    sc_.releaseFrameGen();
}

// AMD's swapchain as the RHI's PresentWrapper (made on first use, dropped when the swapchain goes)
PresentWrapper* VkGamePresenter::presentWrapper() {
    vk::FgSwapchain* fg = sc_.frameGen();
    if (fg == nullptr) {
        fgWrapper_.reset();
        fgWrapperFor_ = nullptr;
        return nullptr;
    }
    if (!fgWrapper_ || fgWrapperFor_ != fg) {
        fgWrapper_ = wrapFgSwapchain(dev_, *fg);
        fgWrapperFor_ = fg;
    }
    return fgWrapper_.get();
}

bool VkGamePresenter::waitFrameGenPresent() {
    if (!sc_.frameGenActive()) return true;
    presenter_.waitDone();
    return sc_.waitFrameGenDone(sc_.frameGenPresentsQueued(), 2'000'000'000ull);
}

bool VkGamePresenter::acquireFullScreenExclusive(std::string* error) {
    const VkResult r = sc_.acquireFullScreenExclusive();
    if (r == VK_SUCCESS) return true;
    if (error != nullptr) *error = std::string("vkAcquireFullScreenExclusiveModeEXT: ") + vk::resultString(r);
    return false;
}

bool VkGamePresenter::acquire() {
    acquired_ = acquireSemaphores_[frames_.currentValue() % vk::Frames::kInFlight];
    haveImage_ = sc_.acquire(acquired_, imageIndex_);
    layout_ = VK_IMAGE_LAYOUT_UNDEFINED;
    stage_ = VK_PIPELINE_STAGE_2_NONE;
    access_ = VK_ACCESS_2_NONE;
    return haveImage_;
}

void VkGamePresenter::barrier(VkImageLayout newLayout, VkPipelineStageFlags2 dstStage, VkAccessFlags2 dstAccess) {
    VkImageMemoryBarrier2 b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
    b.srcStageMask = stage_;
    b.srcAccessMask = access_;
    b.dstStageMask = dstStage;
    b.dstAccessMask = dstAccess;
    b.oldLayout = layout_;
    b.newLayout = newLayout;
    b.image = sc_.image(imageIndex_);
    b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    VkDependencyInfo di{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    di.imageMemoryBarrierCount = 1;
    di.pImageMemoryBarriers = &b;
    vkCmdPipelineBarrier2(frames_.cmd(), &di);
    layout_ = newLayout;
    stage_ = dstStage;
    access_ = dstAccess;
}

void VkGamePresenter::blitFrame(Texture& src, VkExtent3D srcExtent) {
    VkCommandBuffer cmd = frames_.cmd();
    vk::Image& bb = imageOf(src);
    bb.transition(cmd, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    // the swapchain image: contents undefined -> transfer destination (earlier transfers of the frame ordered before the blit)
    stage_ = VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT;
    access_ = VK_ACCESS_2_NONE;
    barrier(VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT);
    const VkExtent2D ext = sc_.extent();
    VkImageBlit2 region{VK_STRUCTURE_TYPE_IMAGE_BLIT_2};
    region.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    region.srcOffsets[1] = {static_cast<std::int32_t>(srcExtent.width), static_cast<std::int32_t>(srcExtent.height), 1};
    region.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    region.dstOffsets[1] = {static_cast<std::int32_t>(ext.width), static_cast<std::int32_t>(ext.height), 1};
    VkBlitImageInfo2 bi{VK_STRUCTURE_TYPE_BLIT_IMAGE_INFO_2};
    bi.srcImage = bb.image();
    bi.srcImageLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    bi.dstImage = sc_.image(imageIndex_);
    bi.dstImageLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    bi.regionCount = 1;
    bi.pRegions = &region;
    bi.filter = VK_FILTER_LINEAR;
    vkCmdBlitImage2(cmd, &bi);
    stage_ = VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT;
    access_ = VK_ACCESS_2_TRANSFER_WRITE_BIT;
}

void VkGamePresenter::drawOverlay(PresentOverlay& overlay) {
    const VkExtent2D ext = sc_.extent();
    if (sc_.frameGenActive()) {
        // built here (the render thread), drawn by the swapchain's present callback on every frame it shows
        overlay.prepare(ext);
        return;
    }
    if (sc_.view(imageIndex_) == VK_NULL_HANDLE) return;
    VkCommandBuffer cmd = frames_.cmd();
    barrier(VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
            VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT);
    VkRenderingAttachmentInfo att{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
    att.imageView = sc_.view(imageIndex_);
    att.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    att.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
    att.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    VkRenderingInfo ri{VK_STRUCTURE_TYPE_RENDERING_INFO};
    ri.renderArea = {{0, 0}, ext};
    ri.layerCount = 1;
    ri.colorAttachmentCount = 1;
    ri.pColorAttachments = &att;
    vkCmdBeginRendering(cmd, &ri);
    overlay.record(dev_.frameCommands(), ext);
    vkCmdEndRendering(cmd);
    stage_ = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
    access_ = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;
}

void VkGamePresenter::finishFrame() {
    if (sc_.frameGenActive()) {
        // the replacement image goes to the API in the layout it reads (frame-generation.md F3: ReplacementBufferTransferState)
        barrier(VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_MEMORY_READ_BIT);
    } else {
        barrier(VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, VK_PIPELINE_STAGE_2_NONE, VK_ACCESS_2_NONE);
    }
}

void VkGamePresenter::queuePresent() {
    presenter_.present(imageIndex_);
    haveImage_ = false;
}

void VkGamePresenter::takeSubmitSync(VkSemaphore& waitAcquire, VkSemaphore& signalRendered) const {
    waitAcquire = haveImage_ ? acquired_ : VK_NULL_HANDLE;
    signalRendered = haveImage_ ? sc_.renderedSemaphore(imageIndex_) : VK_NULL_HANDLE;
}

}  // namespace rvk::rhi::vulkan
