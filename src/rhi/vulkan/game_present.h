#pragma once

// The Vulkan implementation of rhi::GamePresenter (rhi/game_device.h): the game window's swapchain (vk::Swapchain), the present thread (vk::Presenter),
// AMD's frame generation swapchain (through vk::Swapchain::setFrameGen) and the per-frame-slot acquire semaphores. Owned by the Vulkan game device,
// which asks it for the semaphores of a submission (takeSubmitSync).

#include <memory>

#include "rhi/game_device.h"
#include "vk/presenter.h"
#include "vk/swapchain.h"

namespace rvk::vk {
class Context;
class Frames;
}  // namespace rvk::vk

namespace rvk::rhi::vulkan {

class VkGamePresenter final : public GamePresenter {
public:
    VkGamePresenter(GameDevice& device, vk::Context& ctx, vk::Frames& frames);
    ~VkGamePresenter() override;

    bool create(void* window, std::uint32_t width, std::uint32_t height, bool vsync) override;
    void destroy() override;
    PresentInfo info() const override;
    void setVsync(bool vsync) override { sc_.setVsync(vsync); }
    bool recreate(std::uint32_t width, std::uint32_t height) override { return sc_.recreate(width, height); }

    PresentStatus waitIdle(std::string* error) override;
    void waitDone() override { presenter_.waitDone(); }
    double takePresentMs() override { return presenter_.takePresentMs(); }

    void setFrameGen(bool on, const std::filesystem::path& loaderDll) override { sc_.setFrameGen(on, loaderDll); }
    bool frameGenWanted() const override { return sc_.frameGenWanted(); }
    bool frameGenActive() const override { return sc_.frameGenActive(); }
    const std::string& frameGenReason() const override { return sc_.frameGenReason(); }
    void releaseFrameGen() override;
    PresentWrapper* presentWrapper() override;
    bool waitFrameGenPresent() override;

    void setFullScreenExclusive(void* monitor) override { sc_.setFullScreenExclusive(monitor); }
    bool fullScreenExclusiveConfigured() const override { return sc_.fullScreenExclusiveConfigured(); }
    bool fullScreenExclusiveHeld() const override { return sc_.fullScreenExclusiveHeld(); }
    bool acquireFullScreenExclusive(std::string* error) override;
    void releaseFullScreenExclusive() override { sc_.releaseFullScreenExclusive(); }
    void noteExclusiveLost() override { sc_.noteExclusiveLost(); }
    bool takeExclusiveLost() override { return sc_.takeExclusiveLost(); }

    bool acquire() override;
    bool exclusiveLost() const override { return sc_.exclusiveLost(); }
    bool acquireNotReady() const override { return sc_.acquireNotReady(); }
    void blitFrame(Texture& src, VkExtent3D srcExtent) override;
    void drawOverlay(PresentOverlay& overlay) override;
    void finishFrame() override;
    void queuePresent() override;

    // the semaphores of the next submission: waits for the acquired image, signals "rendered" (null both when no image is acquired)
    void takeSubmitSync(VkSemaphore& waitAcquire, VkSemaphore& signalRendered) const;

private:
    // transitions the acquired image from the tracked state to the given one (one synchronization2 barrier)
    void barrier(VkImageLayout newLayout, VkPipelineStageFlags2 dstStage, VkAccessFlags2 dstAccess);

    GameDevice& dev_;
    vk::Context& ctx_;
    vk::Frames& frames_;
    vk::Swapchain sc_;
    vk::Presenter presenter_;
    VkSemaphore acquireSemaphores_[kFramesInFlight]{};

    // the acquired image of the frame being recorded
    bool haveImage_ = false;
    std::uint32_t imageIndex_ = 0;
    VkSemaphore acquired_ = VK_NULL_HANDLE;
    VkImageLayout layout_ = VK_IMAGE_LAYOUT_UNDEFINED;
    VkPipelineStageFlags2 stage_ = VK_PIPELINE_STAGE_2_NONE;
    VkAccessFlags2 access_ = VK_ACCESS_2_NONE;

    std::unique_ptr<PresentWrapper> fgWrapper_;  // over sc_'s AMD swapchain
    const vk::FgSwapchain* fgWrapperFor_ = nullptr;
};

}  // namespace rvk::rhi::vulkan
