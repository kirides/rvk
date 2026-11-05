#include "vk/image.h"

#include <algorithm>
#include <vector>

#include <spdlog/spdlog.h>

#include "vk/context.h"
#include "vk/debug_names.h"

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
        // compute: injected passes sample engine images (d3d9/inject.h)
        stages = VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
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
    aci.priority = 0.5f;  // VK_EXT_memory_priority: the default (a zero-initialised priority is the lowest)
    aci.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
    if ((desc.usage & (VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT)) != 0) {
        aci.flags = VMA_ALLOCATION_CREATE_DEDICATED_MEMORY_BIT;
        // render targets stay resident when VRAM is oversubscribed (the driver evicts textures first); DXVK does the same
        aci.priority = 1.0f;
    }
    if (const VkResult r = vmaCreateImage(ctx.allocator(), &ici, &aci, &image_, &alloc_, nullptr); r != VK_SUCCESS) {
        spdlog::error("vulkan: vmaCreateImage {}x{} format {}: {}", desc.extent.width, desc.extent.height,
                      static_cast<int>(desc.format), resultString(r));
        image_ = VK_NULL_HANDLE;
        return false;
    }
    VkImageViewCreateInfo vci{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    vci.image = image_;
    vci.viewType = desc.cube                       ? VK_IMAGE_VIEW_TYPE_CUBE
                   : desc.type == VK_IMAGE_TYPE_3D ? VK_IMAGE_VIEW_TYPE_3D
                   : desc.layers > 1               ? VK_IMAGE_VIEW_TYPE_2D_ARRAY  // (was an invalid 2D view of several layers)
                                                   : VK_IMAGE_VIEW_TYPE_2D;
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
        names::image(ctx, *this, desc.name);  // the image and its view
    }
    layout_ = VK_IMAGE_LAYOUT_UNDEFINED;
    split_ = false;
    return true;
}

void Image::destroy(Context& ctx) {
    if (adopted_) {
        for (auto& v : attachmentViews_) {
            if (v != VK_NULL_HANDLE) vkDestroyImageView(ctx.device(), v, nullptr);
            v = VK_NULL_HANDLE;
        }
        image_ = VK_NULL_HANDLE;
        view_ = VK_NULL_HANDLE;
        adopted_ = false;
        return;
    }
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
    const VkComponentMapping& s = desc_.swizzle;
    const bool identity = (s.r == VK_COMPONENT_SWIZZLE_IDENTITY || s.r == VK_COMPONENT_SWIZZLE_R) &&
                          (s.g == VK_COMPONENT_SWIZZLE_IDENTITY || s.g == VK_COMPONENT_SWIZZLE_G) &&
                          (s.b == VK_COMPONENT_SWIZZLE_IDENTITY || s.b == VK_COMPONENT_SWIZZLE_B) &&
                          (s.a == VK_COMPONENT_SWIZZLE_IDENTITY || s.a == VK_COMPONENT_SWIZZLE_A);
    // attachment views must not swizzle (the sampled view of X8R8G8B8 forces alpha to 1)
    if (identity && desc_.mips == 1 && desc_.layers == 1 && desc_.type == VK_IMAGE_TYPE_2D && !desc_.cube &&
        aspect_ == VK_IMAGE_ASPECT_COLOR_BIT) {
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
    if (split_) {
        transitionMips(cmd, layout, 0, desc_.mips, force);
        return;
    }
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



// the barriers of a mip range transition (whole-image bookkeeping while the levels agree, per level after a partial range); the tracked layouts are
// updated, nothing is recorded
void Image::appendBarriers(std::vector<VkImageMemoryBarrier2>& out, VkImageLayout layout, std::uint32_t baseMip, std::uint32_t mipCount, bool force) {
    const std::uint32_t mips = std::min(desc_.mips, 16u);
    if (baseMip >= mips || mipCount == 0) return;
    mipCount = std::min(mipCount, mips - baseMip);
    const auto make = [&](VkImageLayout old, std::uint32_t first, std::uint32_t count) {
        VkImageMemoryBarrier2 b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
        layoutSync(old, b.srcStageMask, b.srcAccessMask);
        layoutSync(layout, b.dstStageMask, b.dstAccessMask);
        if (b.srcStageMask == VK_PIPELINE_STAGE_2_NONE) b.srcStageMask = VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT;
        if (b.dstStageMask == VK_PIPELINE_STAGE_2_NONE) b.dstStageMask = VK_PIPELINE_STAGE_2_BOTTOM_OF_PIPE_BIT;
        b.oldLayout = old;
        b.newLayout = layout;
        b.image = image_;
        b.subresourceRange = {aspect_, first, count, 0, VK_REMAINING_ARRAY_LAYERS};
        out.push_back(b);
    };
    if (!split_ && baseMip == 0 && mipCount >= desc_.mips) {
        if (layout_ != layout || force) make(layout_, 0, VK_REMAINING_MIP_LEVELS);
        layout_ = layout;
        return;
    }
    if (!split_) {
        for (std::uint32_t m = 0; m < 16; ++m) mipLayout_[m] = layout_;
        split_ = true;
    }
    for (std::uint32_t m = baseMip; m < baseMip + mipCount;) {
        const VkImageLayout old = mipLayout_[m];
        std::uint32_t e = m + 1;
        while (e < baseMip + mipCount && mipLayout_[e] == old) ++e;
        if (old != layout || force) make(old, m, e - m);
        m = e;
    }
    for (std::uint32_t m = baseMip; m < baseMip + mipCount; ++m) mipLayout_[m] = layout;
    // all levels in one layout again: back to the whole-image bookkeeping
    bool same = true;
    for (std::uint32_t m = 1; m < mips; ++m) same = same && mipLayout_[m] == mipLayout_[0];
    if (same) {
        layout_ = mipLayout_[0];
        split_ = false;
    }
}

void Image::transitionMips(VkCommandBuffer cmd, VkImageLayout layout, std::uint32_t baseMip, std::uint32_t mipCount, bool force) {
    std::vector<VkImageMemoryBarrier2> barriers;
    appendBarriers(barriers, layout, baseMip, mipCount, force);
    if (barriers.empty()) return;
    VkDependencyInfo di{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    di.imageMemoryBarrierCount = static_cast<std::uint32_t>(barriers.size());
    di.pImageMemoryBarriers = barriers.data();
    vkCmdPipelineBarrier2(cmd, &di);
}

}  // namespace rvk::vk
