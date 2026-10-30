#include "rhi/rhi.h"

namespace rvk::rhi {

const char* backendName(Backend b) { return b == Backend::Vulkan ? "Vulkan" : "D3D12"; }

std::uint32_t formatBytes(VkFormat f) {
    switch (f) {
    case VK_FORMAT_R8_UNORM:
    case VK_FORMAT_R8_UINT: return 1;
    case VK_FORMAT_R8G8_UNORM:
    case VK_FORMAT_R16_SFLOAT:
    case VK_FORMAT_R16_UNORM:
    case VK_FORMAT_R16_UINT: return 2;
    case VK_FORMAT_R8G8B8A8_UNORM:
    case VK_FORMAT_R8G8B8A8_SRGB:
    case VK_FORMAT_B8G8R8A8_UNORM:
    case VK_FORMAT_B8G8R8A8_SRGB:
    case VK_FORMAT_A2B10G10R10_UNORM_PACK32:
    case VK_FORMAT_R16G16_SFLOAT:
    case VK_FORMAT_R32_SFLOAT:
    case VK_FORMAT_R32_UINT:
    case VK_FORMAT_D32_SFLOAT: return 4;
    case VK_FORMAT_R16G16B16A16_SFLOAT:
    case VK_FORMAT_R32G32_SFLOAT: return 8;
    case VK_FORMAT_R32G32B32A32_SFLOAT: return 16;
    default: return 0;
    }
}

std::uint32_t bufferRowPitch(VkFormat format, std::uint32_t width) { return (formatBytes(format) * width + 255u) & ~255u; }

}  // namespace rvk::rhi
