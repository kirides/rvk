#pragma once

// Runs of indexed draws that share a pipeline and every other state, recorded as one VK_EXT_device_generated_commands
// execution (docs/research/performance.md 12): per draw a sequence of the 48-byte push constants and a
// VkDrawIndexedIndirectCommand, written by the CPU into the frame's ring; the driver turns them into draws on the GPU
// instead of the CPU recording each one. Pipeline, vertex input, dynamic state and index buffer are bound before (no
// execution set: the device offers no pipeline switching inside generated commands). Optional (DeviceFeatures::
// deviceGeneratedCommands); the caller records the draws directly otherwise.

#include <cstdint>

#include "vk/frames.h"
#include "vk/vk.h"

namespace rvk::vk {

class Context;

class GeneratedDraws {
public:
    static constexpr std::uint32_t kPushSize = 48;
    static constexpr std::uint32_t kStride = kPushSize + sizeof(VkDrawIndexedIndirectCommand);  // 68

    bool create(Context& ctx, Frames& frames, VkPipelineLayout layout);
    void destroy();
    bool available() const { return layout_ != VK_NULL_HANDLE; }

    // records `count` sequences (kStride bytes each at `sequences`, a device address in the ring) with the bound pipeline
    // `pipeline` and the bound state; false (nothing recorded) when no preprocess memory could be had
    bool execute(VkCommandBuffer cmd, VkPipeline pipeline, VkDeviceAddress sequences, std::uint32_t count);

private:
    struct Arena {
        VkBuffer buffer = VK_NULL_HANDLE;
        VmaAllocation allocation = VK_NULL_HANDLE;
        VkDeviceAddress address = 0;
        VkDeviceSize size = 0;
        VkDeviceSize used = 0;
        std::uint64_t frame = 0;  // the frame value its `used` belongs to
    };
    bool grow(Arena& a, VkDeviceSize need);

    Context* ctx_ = nullptr;
    Frames* frames_ = nullptr;
    VkIndirectCommandsLayoutEXT layout_ = VK_NULL_HANDLE;
    std::uint32_t memoryTypeBits_ = 0;
    Arena arenas_[Frames::kInFlight];
};

}  // namespace rvk::vk
