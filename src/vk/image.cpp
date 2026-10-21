#include "vk/image.h"

#include <spdlog/spdlog.h>

#include "vk/context.h"

namespace rvk::vk {

bool isDepthFormat(VkFormat f) {
    switch (f) {
    case VK_FORMAT_D16_UNORM:
    case VK_FORMAT_X8_D24_UNORM_PACK32:
    case VK_FORMAT_D32_SFLOAT:
    case VK_FORMAT_D16_UNORM_S8_UINT:
    case VK_FORMAT_D24_UNORM_S8_UINT:
    case VK_FORMAT_D32_SFLOAT_S8_UINT: return true;
    default: return false;
    }
}

bool hasStencil(VkFormat f) {
    return f == VK_FORMAT_D16_UNORM_S8_UINT || f == VK_FORMAT_D24_UNORM_S8_UINT || f == VK_FORMAT_D32_SFLOAT_S8_UINT;
}

void layoutSync(VkImageLayout layout, VkPipelineStageFlags2& stages, VkAccessFlags2& access) {
    switch (layout) {
    case VK_IMAGE_LAYOUT_UNDEFINED:
        stages = VK_PIPELINE_STAGE_2_NONE;
        access = VK_ACCESS_2_NONE;
        break;
    case VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL:
        stages = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
        access = VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;
        break;
    case VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL:
        stages = VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT;
        access = VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
        break;
    case VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL:
    case VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL:
        stages = VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
        access = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT;
        break;
    case VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL:
        stages = VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT;
        access = VK_ACCESS_2_TRANSFER_READ_BIT;
        break;
    case VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL:
        stages = VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT;
        access = VK_ACCESS_2_TRANSFER_WRITE_BIT;
        break;
    case VK_IMAGE_LAYOUT_PRESENT_SRC_KHR:
        stages = VK_PIPELINE_STAGE_2_NONE;
        access = VK_ACCESS_2_NONE;
        break;
    default:
        stages = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
        access = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT;
        break;
    }
}

bool Image::create(Context& ctx, const ImageDesc& desc) {
    desc_ = desc;
    aspect_ = isDepthFormat(desc.format) ? (VK_IMAGE_ASPECT_DEPTH_BIT | (hasStencil(desc.format) ? VK_IMAGE_ASPECT_STENCIL_BIT : 0))
                                         : VK_IMAGE_ASPECT_COLOR_BIT;
    VkImageCreateInfo ici{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ici.imageType = desc.type;
    ici.format = desc.format;
    ici.extent = desc.extent;
    ici.mipLevels = desc.mips;
    ici.arrayLayers = desc.layers;
    ici.flags = desc.cube ? VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT : 0;
    ici.samples = VK_SAMPLE_COUNT_1_BIT;
    ici.tiling = VK_IMAGE_TILING_OPTIMAL;
    ici.usage = desc.usage;
    ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    VmaAllocationCreateInfo aci{};
    aci.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
    if ((desc.usage & (VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT)) != 0) {
        aci.flags = VMA_ALLOCATION_CREATE_DEDICATED_MEMORY_BIT;
    }
    if (const VkResult r = vmaCreateImage(ctx.allocator(), &ici, &aci, &image_, &alloc_, nullptr); r != VK_SUCCESS) {
        spdlog::error("vulkan: vmaCreateImage {}x{} format {}: {}", desc.extent.width, desc.extent.height,
                      static_cast<int>(desc.format), resultString(r));
        image_ = VK_NULL_HANDLE;
        return false;
    }
    VkImageViewCreateInfo vci{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    vci.image = image_;
    vci.viewType = desc.cube ? VK_IMAGE_VIEW_TYPE_CUBE : desc.type == VK_IMAGE_TYPE_3D ? VK_IMAGE_VIEW_TYPE_3D : VK_IMAGE_VIEW_TYPE_2D;
    vci.format = desc.format;
    vci.components = desc.swizzle;
    // a sampled view of a depth/stencil image may only have one aspect
    vci.subresourceRange = {isDepthFormat(desc.format) ? VkImageAspectFlags(VK_IMAGE_ASPECT_DEPTH_BIT) : aspect_, 0, desc.mips, 0,
                            desc.layers};
    if (vkCreateImageView(ctx.device(), &vci, nullptr, &view_) != VK_SUCCESS) {
        destroy(ctx);
        return false;
    }
    if (desc.name != nullptr) {
        ctx.setName(image_, VK_OBJECT_TYPE_IMAGE, desc.name);
    }
    layout_ = VK_IMAGE_LAYOUT_UNDEFINED;
    return true;
}

void Image::destroy(Context& ctx) {
    for (auto& v : attachmentViews_) {
        if (v != VK_NULL_HANDLE) {
            vkDestroyImageView(ctx.device(), v, nullptr);
            v = VK_NULL_HANDLE;
        }
    }
    if (view_ != VK_NULL_HANDLE) {
        vkDestroyImageView(ctx.device(), view_, nullptr);
        view_ = VK_NULL_HANDLE;
    }
    if (image_ != VK_NULL_HANDLE) {
        vmaDestroyImage(ctx.allocator(), image_, alloc_);
        image_ = VK_NULL_HANDLE;
        alloc_ = VK_NULL_HANDLE;
    }
}

VkImageView Image::attachmentView(Context& ctx, std::uint32_t mip, std::uint32_t layer) {
    if (desc_.mips == 1 && desc_.layers == 1 && desc_.type == VK_IMAGE_TYPE_2D && !desc_.cube && aspect_ == VK_IMAGE_ASPECT_COLOR_BIT) {
        return view_;
    }
    const std::uint32_t idx = mip * 6 + layer;
    if (mip >= 16 || layer >= 6) {
        return VK_NULL_HANDLE;
    }
    if (attachmentViews_[idx] == VK_NULL_HANDLE) {
        VkImageViewCreateInfo vci{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        vci.image = image_;
        vci.viewType = VK_IMAGE_VIEW_TYPE_2D;
        vci.format = desc_.format;
        vci.subresourceRange = {aspect_, mip, 1, layer, 1};
        vkCreateImageView(ctx.device(), &vci, nullptr, &attachmentViews_[idx]);
    }
    return attachmentViews_[idx];
}

void Image::transition(VkCommandBuffer cmd, VkImageLayout layout, bool force) {
    if (layout_ == layout && !force) {
        return;
    }
    VkImageMemoryBarrier2 b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
    layoutSync(layout_, b.srcStageMask, b.srcAccessMask);
    layoutSync(layout, b.dstStageMask, b.dstAccessMask);
    if (b.srcStageMask == VK_PIPELINE_STAGE_2_NONE) {
        b.srcStageMask = VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT;
    }
    if (b.dstStageMask == VK_PIPELINE_STAGE_2_NONE) {
        b.dstStageMask = VK_PIPELINE_STAGE_2_BOTTOM_OF_PIPE_BIT;
    }
    b.oldLayout = layout_;
    b.newLayout = layout;
    b.image = image_;
    b.subresourceRange = {aspect_, 0, VK_REMAINING_MIP_LEVELS, 0, VK_REMAINING_ARRAY_LAYERS};
    VkDependencyInfo di{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    di.imageMemoryBarrierCount = 1;
    di.pImageMemoryBarriers = &b;
    vkCmdPipelineBarrier2(cmd, &di);
    layout_ = layout;
}

}  // namespace rvk::vk
