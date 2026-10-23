#include "vk/generated_draws.h"

#include <algorithm>

#include <spdlog/spdlog.h>

#include "vk/context.h"

namespace rvk::vk {

bool GeneratedDraws::create(Context& ctx, Frames& frames, VkPipelineLayout layout) {
    ctx_ = &ctx;
    frames_ = &frames;
    if (!ctx.features().deviceGeneratedCommands || ctx.features().dgcMaxStride < kStride) return false;
    VkIndirectCommandsPushConstantTokenEXT push{};
    push.updateRange = {VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, kPushSize};
    VkIndirectCommandsLayoutTokenEXT tokens[2]{};
    tokens[0].sType = VK_STRUCTURE_TYPE_INDIRECT_COMMANDS_LAYOUT_TOKEN_EXT;
    tokens[0].type = VK_INDIRECT_COMMANDS_TOKEN_TYPE_PUSH_CONSTANT_EXT;
    tokens[0].data.pPushConstant = &push;
    tokens[0].offset = 0;
    tokens[1].sType = VK_STRUCTURE_TYPE_INDIRECT_COMMANDS_LAYOUT_TOKEN_EXT;
    tokens[1].type = VK_INDIRECT_COMMANDS_TOKEN_TYPE_DRAW_INDEXED_EXT;
    tokens[1].offset = kPushSize;
    VkIndirectCommandsLayoutCreateInfoEXT ci{VK_STRUCTURE_TYPE_INDIRECT_COMMANDS_LAYOUT_CREATE_INFO_EXT};
    ci.shaderStages = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
    ci.indirectStride = kStride;
    ci.pipelineLayout = layout;
    ci.tokenCount = 2;
    ci.pTokens = tokens;
    const VkResult r = vkCreateIndirectCommandsLayoutEXT(ctx.device(), &ci, nullptr, &layout_);
    if (r != VK_SUCCESS) {
        spdlog::warn("vulkan: generated draws: vkCreateIndirectCommandsLayoutEXT: {} (draws are recorded directly)", static_cast<int>(r));
        layout_ = VK_NULL_HANDLE;
        return false;
    }
    spdlog::info("vulkan: generated draws on (VK_EXT_device_generated_commands, up to {} sequences)", ctx.features().dgcMaxSequences);
    return true;
}

void GeneratedDraws::destroy() {
    for (Arena& a : arenas_) {
        if (a.buffer != VK_NULL_HANDLE) vmaDestroyBuffer(ctx_->allocator(), a.buffer, a.allocation);
        a = {};
    }
    if (layout_ != VK_NULL_HANDLE) vkDestroyIndirectCommandsLayoutEXT(ctx_->device(), layout_, nullptr);
    layout_ = VK_NULL_HANDLE;
}

// a bigger preprocess buffer for this slot; the old one stays alive until this frame has completed (earlier executions of
// the frame use it)
bool GeneratedDraws::grow(Arena& a, VkDeviceSize need) {
    const VkDeviceSize size = std::max<VkDeviceSize>({need, a.size * 2, 4ull << 20});
    VkBufferUsageFlags2CreateInfo usage2{VK_STRUCTURE_TYPE_BUFFER_USAGE_FLAGS_2_CREATE_INFO};
    usage2.usage = VK_BUFFER_USAGE_2_PREPROCESS_BUFFER_BIT_EXT | VK_BUFFER_USAGE_2_SHADER_DEVICE_ADDRESS_BIT;
    VkBufferCreateInfo bci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, &usage2};
    bci.size = size;
    VmaAllocationCreateInfo aci{};
    aci.usage = VMA_MEMORY_USAGE_UNKNOWN;
    aci.requiredFlags = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
    aci.memoryTypeBits = memoryTypeBits_;
    VkBuffer b = VK_NULL_HANDLE;
    VmaAllocation m = VK_NULL_HANDLE;
    if (vmaCreateBuffer(ctx_->allocator(), &bci, &aci, &b, &m, nullptr) != VK_SUCCESS) {
        aci.requiredFlags = 0;  // any type the requirements allow
        if (vmaCreateBuffer(ctx_->allocator(), &bci, &aci, &b, &m, nullptr) != VK_SUCCESS) return false;
    }
    if (a.buffer != VK_NULL_HANDLE) {
        Context* ctx = ctx_;
        frames_->retire([ctx, ob = a.buffer, oa = a.allocation] { vmaDestroyBuffer(ctx->allocator(), ob, oa); });
    }
    VkBufferDeviceAddressInfo ai{VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO};
    ai.buffer = b;
    a.buffer = b;
    a.allocation = m;
    a.address = vkGetBufferDeviceAddress(ctx_->device(), &ai);
    a.size = size;
    a.used = 0;
    spdlog::info("vulkan: generated draws: preprocess buffer {:.1f} MiB", static_cast<double>(size) / 1048576.0);
    return true;
}

bool GeneratedDraws::execute(VkCommandBuffer cmd, VkPipeline pipeline, VkDeviceAddress sequences, std::uint32_t count) {
    if (layout_ == VK_NULL_HANDLE || count == 0 || count > ctx_->features().dgcMaxSequences) return false;
    VkGeneratedCommandsPipelineInfoEXT pinfo{VK_STRUCTURE_TYPE_GENERATED_COMMANDS_PIPELINE_INFO_EXT};
    pinfo.pipeline = pipeline;
    VkGeneratedCommandsMemoryRequirementsInfoEXT ri{VK_STRUCTURE_TYPE_GENERATED_COMMANDS_MEMORY_REQUIREMENTS_INFO_EXT, &pinfo};
    ri.indirectCommandsLayout = layout_;
    ri.maxSequenceCount = count;
    VkMemoryRequirements2 mr{VK_STRUCTURE_TYPE_MEMORY_REQUIREMENTS_2};
    vkGetGeneratedCommandsMemoryRequirementsEXT(ctx_->device(), &ri, &mr);
    if (memoryTypeBits_ == 0) memoryTypeBits_ = mr.memoryRequirements.memoryTypeBits;
    // the slot's preprocess memory: a fresh frame value starts it over (its previous use has completed: Frames::begin)
    Arena& a = arenas_[frames_->currentSlot()];
    if (a.frame != frames_->currentValue()) {
        a.frame = frames_->currentValue();
        a.used = 0;
    }
    const VkDeviceSize align = std::max<VkDeviceSize>(mr.memoryRequirements.alignment, 256);
    const VkDeviceSize size = std::max<VkDeviceSize>(mr.memoryRequirements.size, 4);
    VkDeviceSize offset = (a.used + align - 1) / align * align;
    if (a.buffer == VK_NULL_HANDLE || offset + size > a.size) {
        if (!grow(a, size + align)) return false;
        offset = 0;
    }
    a.used = offset + size;
    VkGeneratedCommandsInfoEXT gi{VK_STRUCTURE_TYPE_GENERATED_COMMANDS_INFO_EXT, &pinfo};
    gi.shaderStages = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
    gi.indirectCommandsLayout = layout_;
    gi.indirectAddress = sequences;
    gi.indirectAddressSize = static_cast<VkDeviceSize>(count) * kStride;
    gi.preprocessAddress = a.address + offset;
    gi.preprocessSize = size;
    gi.maxSequenceCount = count;
    vkCmdExecuteGeneratedCommandsEXT(cmd, VK_FALSE, &gi);
    return true;
}

}  // namespace rvk::vk
