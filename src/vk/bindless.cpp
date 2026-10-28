#include "vk/bindless.h"

#include <cstring>
#include <string>

#include <spdlog/spdlog.h>

#include "shader/abi.h"
#include "vk/context.h"
#include "vk/frames.h"

namespace rvk::vk {

bool Bindless::create(Context& ctx, Frames& frames) {
    ctx_ = &ctx;
    frames_ = &frames;
    arrays_[0] = {shader::abi::kBindingTex2D, kMax2D};
    arrays_[1] = {shader::abi::kBindingTexCube, kMaxCube};
    arrays_[2] = {shader::abi::kBindingTex3D, kMax3D};

    // compute too: injected passes (vk/passes.h) bind this set in their own layouts
    const VkShaderStageFlags stages = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT | VK_SHADER_STAGE_COMPUTE_BIT;
    VkDescriptorSetLayoutBinding b[4]{};
    b[0] = {shader::abi::kBindingTex2D, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, kMax2D, stages, nullptr};
    b[1] = {shader::abi::kBindingTexCube, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, kMaxCube, stages, nullptr};
    b[2] = {shader::abi::kBindingTex3D, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, kMax3D, stages, nullptr};
    b[3] = {shader::abi::kBindingSamplers, VK_DESCRIPTOR_TYPE_SAMPLER, kMaxSamplers, stages, nullptr};
    const VkDescriptorBindingFlags flag = VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT | VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT |
                                          VK_DESCRIPTOR_BINDING_UPDATE_UNUSED_WHILE_PENDING_BIT;
    VkDescriptorBindingFlags flags[4] = {flag, flag, flag, flag};
    VkDescriptorSetLayoutBindingFlagsCreateInfo bfi{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO};
    bfi.bindingCount = 4;
    bfi.pBindingFlags = flags;
    VkDescriptorSetLayoutCreateInfo lci{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, &bfi};
    lci.flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_UPDATE_AFTER_BIND_POOL_BIT;
    lci.bindingCount = 4;
    lci.pBindings = b;
    if (vkCreateDescriptorSetLayout(ctx.device(), &lci, nullptr, &layout_) != VK_SUCCESS) {
        spdlog::error("vulkan: bindless descriptor set layout failed (descriptor limits?)");
        return false;
    }
    names::object(ctx, layout_, VK_OBJECT_TYPE_DESCRIPTOR_SET_LAYOUT, "rvk bindless set layout");
    VkDescriptorPoolSize sizes[2] = {{VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, kMax2D + kMaxCube + kMax3D},
                                     {VK_DESCRIPTOR_TYPE_SAMPLER, kMaxSamplers}};
    VkDescriptorPoolCreateInfo pci{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    pci.flags = VK_DESCRIPTOR_POOL_CREATE_UPDATE_AFTER_BIND_BIT;
    pci.maxSets = 1;
    pci.poolSizeCount = 2;
    pci.pPoolSizes = sizes;
    if (vkCreateDescriptorPool(ctx.device(), &pci, nullptr, &pool_) != VK_SUCCESS) {
        return false;
    }
    VkDescriptorSetAllocateInfo ai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    ai.descriptorPool = pool_;
    ai.descriptorSetCount = 1;
    ai.pSetLayouts = &layout_;
    if (vkAllocateDescriptorSets(ctx.device(), &ai, &set_) != VK_SUCCESS) {
        return false;
    }
    // sampled images by kind (the DrawBlock's tex[] entries index these arrays) and samplers
    names::object(ctx, pool_, VK_OBJECT_TYPE_DESCRIPTOR_POOL, "rvk bindless pool");
    names::object(ctx, set_, VK_OBJECT_TYPE_DESCRIPTOR_SET, "rvk bindless set (2D / cube / 3D textures + samplers)");
    VkPushConstantRange pcr{VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(shader::abi::PushConstants)};
    VkPipelineLayoutCreateInfo plci{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    plci.setLayoutCount = 1;
    plci.pSetLayouts = &layout_;
    plci.pushConstantRangeCount = 1;
    plci.pPushConstantRanges = &pcr;
    if (vkCreatePipelineLayout(ctx.device(), &plci, nullptr, &pipelineLayout_) != VK_SUCCESS) {
        return false;
    }
    names::object(ctx, pipelineLayout_, VK_OBJECT_TYPE_PIPELINE_LAYOUT, "rvk bindless pipeline layout");
    return createDefaults();
}

bool Bindless::createDefaults() {
    // 1x1 (0,0,0,1) in every texture kind; RGBA8 is supported for sampling everywhere
    for (int k = 0; k < 3; ++k) {
        VkImageCreateInfo ici{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
        ici.imageType = k == 2 ? VK_IMAGE_TYPE_3D : VK_IMAGE_TYPE_2D;
        ici.format = VK_FORMAT_R8G8B8A8_UNORM;
        ici.extent = {1, 1, 1};
        ici.mipLevels = 1;
        ici.arrayLayers = k == 1 ? 6 : 1;
        ici.flags = k == 1 ? VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT : 0;
        ici.samples = VK_SAMPLE_COUNT_1_BIT;
        ici.tiling = VK_IMAGE_TILING_OPTIMAL;
        ici.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        VmaAllocationCreateInfo aci{};
        aci.usage = VMA_MEMORY_USAGE_AUTO;
        if (vmaCreateImage(ctx_->allocator(), &ici, &aci, &defaultImages_[k], &defaultAllocs_[k], nullptr) != VK_SUCCESS) {
            return false;
        }
        VkImageViewCreateInfo vci{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        vci.image = defaultImages_[k];
        vci.viewType = k == 0 ? VK_IMAGE_VIEW_TYPE_2D : k == 1 ? VK_IMAGE_VIEW_TYPE_CUBE : VK_IMAGE_VIEW_TYPE_3D;
        vci.format = VK_FORMAT_R8G8B8A8_UNORM;
        vci.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, k == 1 ? 6u : 1u};
        if (vkCreateImageView(ctx_->device(), &vci, nullptr, &defaultViews_[k]) != VK_SUCCESS) {
            return false;
        }
        {
            static constexpr const char* kKind[3] = {"2D", "cube", "3D"};
            names::object(*ctx_, defaultImages_[k], VK_OBJECT_TYPE_IMAGE, std::string("bindless default ") + kKind[k] + " texture (slot 0)");
            names::object(*ctx_, defaultViews_[k], VK_OBJECT_TYPE_IMAGE_VIEW, std::string("bindless default ") + kKind[k] + " texture (view)");
        }
        write(static_cast<TextureKind>(k), 0, defaultViews_[k]);
    }
    VkSamplerCreateInfo sci{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    sci.magFilter = sci.minFilter = VK_FILTER_NEAREST;
    sci.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    sci.addressModeU = sci.addressModeV = sci.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sci.maxLod = VK_LOD_CLAMP_NONE;
    vkCreateSampler(ctx_->device(), &sci, nullptr, &defaultSampler_);
    names::object(*ctx_, defaultSampler_, VK_OBJECT_TYPE_SAMPLER, "bindless default sampler (point, clamp)");
    addSampler(defaultSampler_);

    VkBufferCreateInfo bci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bci.size = 4;
    bci.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    VmaAllocationCreateInfo aci{};
    aci.usage = VMA_MEMORY_USAGE_AUTO;
    aci.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;
    VmaAllocationInfo info{};
    if (vmaCreateBuffer(ctx_->allocator(), &bci, &aci, &defaultStaging_, &defaultStagingAlloc_, &info) != VK_SUCCESS) {
        return false;
    }
    names::object(*ctx_, defaultStaging_, VK_OBJECT_TYPE_BUFFER, "bindless default texel staging");
    const std::uint8_t pixel[4] = {0, 0, 0, 255};
    std::memcpy(info.pMappedData, pixel, 4);
    return true;
}

void Bindless::recordInit(VkCommandBuffer cmd) {
    for (int k = 0; k < 3; ++k) {
        const std::uint32_t layers = k == 1 ? 6 : 1;
        VkImageMemoryBarrier2 b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
        b.srcStageMask = VK_PIPELINE_STAGE_2_NONE;
        b.dstStageMask = VK_PIPELINE_STAGE_2_COPY_BIT;
        b.dstAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
        b.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        b.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        b.image = defaultImages_[k];
        b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, layers};
        VkDependencyInfo di{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
        di.imageMemoryBarrierCount = 1;
        di.pImageMemoryBarriers = &b;
        vkCmdPipelineBarrier2(cmd, &di);
        for (std::uint32_t l = 0; l < layers; ++l) {
            VkBufferImageCopy region{};
            region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, l, 1};
            region.imageExtent = {1, 1, 1};
            vkCmdCopyBufferToImage(cmd, defaultStaging_, defaultImages_[k], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
        }
        b.srcStageMask = VK_PIPELINE_STAGE_2_COPY_BIT;
        b.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
        b.dstStageMask = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT;
        b.dstAccessMask = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT;
        b.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        b.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        vkCmdPipelineBarrier2(cmd, &di);
    }
}

void Bindless::destroy() {
    if (ctx_ == nullptr) {
        return;
    }
    VkDevice d = ctx_->device();
    for (int k = 0; k < 3; ++k) {
        if (defaultViews_[k] != VK_NULL_HANDLE) vkDestroyImageView(d, defaultViews_[k], nullptr);
        if (defaultImages_[k] != VK_NULL_HANDLE) vmaDestroyImage(ctx_->allocator(), defaultImages_[k], defaultAllocs_[k]);
        defaultViews_[k] = VK_NULL_HANDLE;
        defaultImages_[k] = VK_NULL_HANDLE;
    }
    if (defaultStaging_ != VK_NULL_HANDLE) vmaDestroyBuffer(ctx_->allocator(), defaultStaging_, defaultStagingAlloc_);
    if (defaultSampler_ != VK_NULL_HANDLE) vkDestroySampler(d, defaultSampler_, nullptr);
    if (pipelineLayout_ != VK_NULL_HANDLE) vkDestroyPipelineLayout(d, pipelineLayout_, nullptr);
    if (pool_ != VK_NULL_HANDLE) vkDestroyDescriptorPool(d, pool_, nullptr);
    if (layout_ != VK_NULL_HANDLE) vkDestroyDescriptorSetLayout(d, layout_, nullptr);
    defaultStaging_ = VK_NULL_HANDLE;
    defaultSampler_ = VK_NULL_HANDLE;
    pipelineLayout_ = VK_NULL_HANDLE;
    pool_ = VK_NULL_HANDLE;
    layout_ = VK_NULL_HANDLE;
    ctx_ = nullptr;
}

void Bindless::write(TextureKind kind, std::uint32_t slot, VkImageView view) {
    VkDescriptorImageInfo ii{VK_NULL_HANDLE, view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    VkWriteDescriptorSet w{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    w.dstSet = set_;
    w.dstBinding = array(kind).binding;
    w.dstArrayElement = slot;
    w.descriptorCount = 1;
    w.descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
    w.pImageInfo = &ii;
    vkUpdateDescriptorSets(ctx_->device(), 1, &w, 0, nullptr);
}

std::uint32_t Bindless::addTexture(TextureKind kind, VkImageView view) {
    const std::lock_guard lock(mutex_);
    Array& a = array(kind);
    std::uint32_t slot = 0;
    if (!a.freeList.empty()) {
        slot = a.freeList.back();
        a.freeList.pop_back();
    } else if (a.next < a.capacity) {
        slot = a.next++;
    } else {
        spdlog::error("vulkan: bindless {} array full ({} slots)", static_cast<int>(kind), a.capacity);
        return 0;
    }
    write(kind, slot, view);
    return slot;
}

void Bindless::updateTexture(TextureKind kind, std::uint32_t slot, VkImageView view) {
    if (slot == 0) {
        return;
    }
    const std::lock_guard lock(mutex_);
    write(kind, slot, view);
}

void Bindless::releaseTexture(TextureKind kind, std::uint32_t slot) {
    if (slot == 0) {
        return;
    }
    frames_->retire([this, kind, slot] {
        const std::lock_guard lock(mutex_);
        write(kind, slot, defaultViews_[static_cast<int>(kind)]);
        array(kind).freeList.push_back(slot);
    });
}

std::uint32_t Bindless::addSampler(VkSampler sampler) {
    const std::lock_guard lock(mutex_);
    if (nextSampler_ >= kMaxSamplers) {
        spdlog::error("vulkan: bindless sampler array full");
        return 0;
    }
    const std::uint32_t slot = nextSampler_++;
    VkDescriptorImageInfo ii{sampler, VK_NULL_HANDLE, VK_IMAGE_LAYOUT_UNDEFINED};
    VkWriteDescriptorSet w{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    w.dstSet = set_;
    w.dstBinding = shader::abi::kBindingSamplers;
    w.dstArrayElement = slot;
    w.descriptorCount = 1;
    w.descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER;
    w.pImageInfo = &ii;
    vkUpdateDescriptorSets(ctx_->device(), 1, &w, 0, nullptr);
    return slot;
}

}  // namespace rvk::vk
