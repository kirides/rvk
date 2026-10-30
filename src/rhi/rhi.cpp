#include "rhi/rhi.h"

namespace rvk::rhi {

const char* backendName(Backend b) { return b == Backend::Vulkan ? "Vulkan" : "D3D12"; }

std::uint32_t formatBytes(VkFormat f) {
    switch (f) {
    case VK_FORMAT_R8_UNORM:
    case VK_FORMAT_R8_UINT:
    case VK_FORMAT_R8_SNORM: return 1;
    case VK_FORMAT_R8G8_UNORM:
    case VK_FORMAT_R8G8_SNORM:
    case VK_FORMAT_R16_SFLOAT:
    case VK_FORMAT_R16_UNORM:
    case VK_FORMAT_R16_UINT:
    case VK_FORMAT_D16_UNORM:
    case VK_FORMAT_R5G6B5_UNORM_PACK16:
    case VK_FORMAT_A1R5G5B5_UNORM_PACK16:
    case VK_FORMAT_B4G4R4A4_UNORM_PACK16: return 2;
    case VK_FORMAT_R8G8B8A8_UNORM:
    case VK_FORMAT_R8G8B8A8_SNORM:
    case VK_FORMAT_R8G8B8A8_SRGB:
    case VK_FORMAT_B8G8R8A8_UNORM:
    case VK_FORMAT_B8G8R8A8_SRGB:
    case VK_FORMAT_A2B10G10R10_UNORM_PACK32:
    case VK_FORMAT_A2R10G10B10_UNORM_PACK32:
    case VK_FORMAT_R16G16_SFLOAT:
    case VK_FORMAT_R16G16_UNORM:
    case VK_FORMAT_R16G16_SNORM:
    case VK_FORMAT_R32_SFLOAT:
    case VK_FORMAT_R32_UINT:
    case VK_FORMAT_D32_SFLOAT: return 4;
    case VK_FORMAT_R16G16B16A16_SFLOAT:
    case VK_FORMAT_R16G16B16A16_UNORM:
    case VK_FORMAT_R32G32_SFLOAT: return 8;
    case VK_FORMAT_R32G32B32_SFLOAT: return 12;
    case VK_FORMAT_R32G32B32A32_SFLOAT: return 16;
    default: return 0;
    }
}

FormatBlock formatBlock(VkFormat f) {
    switch (f) {
    case VK_FORMAT_BC1_RGB_UNORM_BLOCK:
    case VK_FORMAT_BC1_RGBA_UNORM_BLOCK:
    case VK_FORMAT_BC4_UNORM_BLOCK: return {4, 4, 8};
    case VK_FORMAT_BC2_UNORM_BLOCK:
    case VK_FORMAT_BC3_UNORM_BLOCK:
    case VK_FORMAT_BC5_UNORM_BLOCK: return {4, 4, 16};
    default: return {1, 1, formatBytes(f)};
    }
}

// a row of `width` texels in bytes: blocks for the compressed formats
static std::uint32_t rowBytes(VkFormat format, std::uint32_t width) {
    const FormatBlock b = formatBlock(format);
    return ((width + b.width - 1) / b.width) * b.bytes;
}

std::uint32_t bufferRowPitch(VkFormat format, std::uint32_t width) { return (rowBytes(format, width) + 255u) & ~255u; }

VkExtent3D mipExtent(const TextureDesc& desc, std::uint32_t mip) {
    return {std::max(desc.extent.width >> mip, 1u), std::max(desc.extent.height >> mip, 1u),
            desc.type == VK_IMAGE_TYPE_3D ? std::max(desc.extent.depth >> mip, 1u) : 1u};
}

std::uint64_t copyBytes(const TextureDesc& desc, std::uint32_t mip) {
    const VkExtent3D e = mipExtent(desc, mip);
    const FormatBlock b = formatBlock(desc.format);
    return static_cast<std::uint64_t>(bufferRowPitch(desc.format, e.width)) * ((e.height + b.height - 1) / b.height) * e.depth;
}

}  // namespace rvk::rhi
