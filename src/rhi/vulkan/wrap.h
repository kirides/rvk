#pragma once

// The pass migration's adapter (docs/research/hlsl-rhi.md 8): the Vulkan RHI backend over the renderer's own vk:: objects, so that passes recorded
// inside the game's frame loop (d3d9::Device::addPass) use rhi::Device / rhi::CommandList / rhi::Texture without a second VkDevice.
//   wrap(...)             an rhi::Device that shares the renderer's VkDevice, queue, Frames (deferred destruction) and the bindless set. Pipelines,
//                         buffers and samplers created through it are the RHI's usual objects, destroyed through Frames::retire (no GPU wait);
//                         submitAndWait / swapchains are not available (the renderer owns submission and presentation), no acceleration structures.
//   wrapImage(...)        a non-owning rhi::Texture over a vk::Image (a transient, an engine texture's image). The image's tracked layout
//                         (vk::Image::layout / transition) stays the single source of truth: CommandList::transition on the wrapper calls it,
//                         the renderer's own barriers and the RHI's agree. The bindless slot of the sampled view is passed in (0 = none).
//   wrapCommandBuffer(..) a non-owning rhi::CommandList recording into the frame's VkCommandBuffer (no begin / end / submit). Create one per pass call
//                         (it holds only the bound pipeline and pending bindings: nothing carries over from the renderer's own recording state).
// Only the Vulkan backend has these; code above the RHI includes this header in the one place that sets a pass up (d3d9::Device::rhi()).

#include <cstdint>
#include <memory>

#include "rhi/game_device.h"
#include "rhi/rhi.h"

namespace rvk::vk {
class Bindless;
class Context;
class FgSwapchain;
class Frames;
class Image;
class MeshArena;
}  // namespace rvk::vk

namespace rvk::rhi::vulkan {

std::unique_ptr<Device> wrap(vk::Context& ctx, vk::Frames& frames, vk::Bindless& bindless);
// The same device as a GameDevice (rhi/game_device.h): what d3d9::Device holds (shader modules of the game's pipelines, the GpuRef shift, ...). `device`
// arguments of the functions below accept it.
// `arena` = the renderer's mesh arena (GameDevice::allocMesh; null: none)
std::unique_ptr<GameDevice> wrapGame(vk::Context& ctx, vk::Frames& frames, vk::Bindless& bindless, vk::MeshArena* arena = nullptr);
// `device` must come from wrap(). `name` = debug name (default: the image's).
std::unique_ptr<Texture> wrapImage(Device& device, vk::Image& image, std::uint32_t sampledSlot = 0, const char* name = nullptr);
std::unique_ptr<CommandList> wrapCommandBuffer(Device& device, VkCommandBuffer cmd);
// A non-owning rhi::Buffer over a VkBuffer owned elsewhere (the frame ring, an engine arena): `address` = the buffer's device address (offset 0),
// `mapped` its host pointer or null. Used as the source / destination of copies and as a build input; the usage the buffer was created with is the
// caller's business. `device` must come from wrap().
std::unique_ptr<Buffer> wrapBuffer(Device& device, VkBuffer buffer, std::uint64_t address, std::uint64_t size, void* mapped = nullptr, const char* name = nullptr);


// ---- the Vulkan objects behind the RHI's (the backend's own users: the vendor modules, the renderer's not yet moved recording code) ----
// The texture / list must come from this backend.
vk::Image& imageOf(Texture& texture);
VkCommandBuffer commandBufferOf(CommandList& list);
vk::Context& contextOf(Device& device);

// ---- the optional modules (rhi/modules.h) over the vendor's Vulkan DLL wrappers (vk/fsr.h, vk/framegen.h, vk/fg_swapchain.h) and ImGui's Vulkan backend ----
std::unique_ptr<Upscaler> createUpscaler(GameDevice& device);
std::unique_ptr<FrameGenerator> createFrameGenerator(GameDevice& device);
std::unique_ptr<ImGuiBackend> createImGuiBackend(GameDevice& device);
// The frame generation swapchain `swapchain` as a PresentWrapper (non-owning; the swapchain outlives it). The wrapper's UI hook records into a list on the
// swapchain's present command buffer.
std::unique_ptr<PresentWrapper> wrapFgSwapchain(GameDevice& device, vk::FgSwapchain& swapchain);

}  // namespace rvk::rhi::vulkan
