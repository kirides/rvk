#pragma once

// A VkImage with its allocation, default view and tracked layout (whole-image granularity: every mip and layer is in the same
// layout between commands; transitions are synchronization2 barriers).

#include <cstdint>

#include "vk/vk.h"

namespace rvk::vk {

class Context;

struct ImageDesc {
    VkImageType type = VK_IMAGE_TYPE_2D;
    VkFormat format = VK_FORMAT_R8G8B8A8_UNORM;
    VkExtent3D extent{1, 1, 1};
    std::uint32_t mips = 1;
    std::uint32_t layers = 1;  // 6 for cube maps
    bool cube = false;
    VkImageUsageFlags usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    VkComponentMapping swizzle{};  // view swizzle (L8 -> rrr1 etc.)
    const char* name = nullptr;
};

class Image {
public:
    bool create(Context& ctx, const ImageDesc& desc);
    // destroys at once (caller makes sure the GPU no longer uses it, e.g. through Frames::retire)
    void destroy(Context& ctx);

    VkImage image() const { return image_; }
    VkImageView view() const { return view_; }
    VkFormat format() const { return desc_.format; }
    const ImageDesc& desc() const { return desc_; }
    VkImageAspectFlags aspect() const { return aspect_; }
    VkImageLayout layout() const { return layout_; }
    bool valid() const { return image_ != VK_NULL_HANDLE; }

    // records a barrier to `layout` (no-op if already there unless `force`); the source stage/access derive from the current
    // layout, so writes in the old layout are made visible
    void transition(VkCommandBuffer cmd, VkImageLayout layout, bool force = false);
    // forget the contents (next transition uses UNDEFINED as the old layout)
    void discard() { layout_ = VK_IMAGE_LAYOUT_UNDEFINED; }

    // view of one mip level / layer as a 2D attachment (created on demand, owned by the image)
    VkImageView attachmentView(Context& ctx, std::uint32_t mip, std::uint32_t layer);

private:
    ImageDesc desc_;
    VkImage image_ = VK_NULL_HANDLE;
    VmaAllocation alloc_ = VK_NULL_HANDLE;
    VkImageView view_ = VK_NULL_HANDLE;
    VkImageView attachmentViews_[16 * 6]{};  // [mip * 6 + layer] for mip < 16
    VkImageAspectFlags aspect_ = VK_IMAGE_ASPECT_COLOR_BIT;
    VkImageLayout layout_ = VK_IMAGE_LAYOUT_UNDEFINED;
};

bool isDepthFormat(VkFormat f);
bool hasStencil(VkFormat f);
// stage and access masks that cover every use of an image in `layout`
void layoutSync(VkImageLayout layout, VkPipelineStageFlags2& stages, VkAccessFlags2& access);

}  // namespace rvk::vk
