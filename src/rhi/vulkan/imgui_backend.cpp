// rhi::ImGuiBackend over Dear ImGui's Vulkan backend (third_party/imgui_vulkan, imgui_vulkan_impl.cpp): dynamic rendering, its own descriptor pool, the
// renderer's device and queue (the setup follows GD3D11's ImGuiShim::InitVulkan, same author as this repository).

#include <algorithm>

#include <imgui.h>
#include <spdlog/spdlog.h>

#define IMGUI_IMPL_VULKAN_USE_VOLK
#include "imgui_impl_vulkan.h"

#include "rhi/vulkan/wrap.h"
#include "vk/context.h"
#include "vk/vk.h"

namespace rvk::rhi::vulkan {
namespace {

void checkVk(VkResult r) {
    if (r < 0) spdlog::error("overlay: imgui_impl_vulkan: {}", vk::resultString(r));
}

class VkImGuiBackend final : public ImGuiBackend {
public:
    explicit VkImGuiBackend(GameDevice& device) : device_(device) {}
    ~VkImGuiBackend() override { shutdown(); }

    bool init(const ImGuiBackendDesc& desc) override {
        if (initialised_) return true;
        vk::Context& ctx = contextOf(device_);
        format_ = desc.colorFormat;
        ImGui_ImplVulkan_InitInfo ii{};
        ii.ApiVersion = VK_API_VERSION_1_3;
        ii.Instance = ctx.instance();
        ii.PhysicalDevice = ctx.physicalDevice();
        ii.Device = ctx.device();
        ii.QueueFamily = ctx.queueFamily();
        ii.Queue = ctx.queue();
        ii.DescriptorPoolSize = IMGUI_IMPL_VULKAN_MINIMUM_SAMPLED_IMAGE_POOL_SIZE + 8;
        ii.MinImageCount = std::max(2u, desc.minImageCount);
        ii.ImageCount = std::max(ii.MinImageCount, desc.imageCount);
        ii.UseDynamicRendering = true;
        ii.PipelineInfoMain.PipelineRenderingCreateInfo = {VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
        ii.PipelineInfoMain.PipelineRenderingCreateInfo.colorAttachmentCount = 1;
        ii.PipelineInfoMain.PipelineRenderingCreateInfo.pColorAttachmentFormats = &format_;  // (read inside ImGui_ImplVulkan_Init only)
        ii.CheckVkResultFn = &checkVk;
        initialised_ = ImGui_ImplVulkan_Init(&ii);
        return initialised_;
    }
    void shutdown() override {
        if (!initialised_) return;
        // (ImGui's shutdown waits for the device: vkDeviceWaitIdle touches the queue the present thread uses)
        auto lock = contextOf(device_).queueLock();
        ImGui_ImplVulkan_Shutdown();
        initialised_ = false;
    }
    void newFrame() override { ImGui_ImplVulkan_NewFrame(); }
    void updateTexture(ImTextureData* texture) override {
        auto lock = contextOf(device_).queueLock();  // (the upload submits to the queue and waits for it; the present thread presents meanwhile)
        ImGui_ImplVulkan_UpdateTexture(texture);
    }
    void renderDrawData(ImDrawData* data, CommandList& cmd) override { ImGui_ImplVulkan_RenderDrawData(data, commandBufferOf(cmd)); }

private:
    GameDevice& device_;
    VkFormat format_ = VK_FORMAT_B8G8R8A8_UNORM;
    bool initialised_ = false;
};

}  // namespace

std::unique_ptr<ImGuiBackend> createImGuiBackend(GameDevice& device) { return std::make_unique<VkImGuiBackend>(device); }

}  // namespace rvk::rhi::vulkan
