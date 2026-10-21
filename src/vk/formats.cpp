#include "vk/formats.h"

#include <algorithm>
#include <cstring>

namespace rvk::vk {
namespace {

constexpr VkComponentSwizzle R = VK_COMPONENT_SWIZZLE_R, G = VK_COMPONENT_SWIZZLE_G, B = VK_COMPONENT_SWIZZLE_B,
                             A = VK_COMPONENT_SWIZZLE_A, ONE = VK_COMPONENT_SWIZZLE_ONE, ZERO = VK_COMPONENT_SWIZZLE_ZERO,
                             ID = VK_COMPONENT_SWIZZLE_IDENTITY;

FormatInfo make(std::uint32_t d3d, VkFormat f, std::uint32_t bytes, const char* name, VkComponentMapping sw = {ID, ID, ID, ID}) {
    FormatInfo i;
    i.d3d = d3d;
    i.format = f;
    i.blockBytes = bytes;
    i.srcBlockBytes = bytes;
    i.name = name;
    i.swizzle = sw;
    return i;
}

FormatInfo compressed(std::uint32_t d3d, VkFormat f, std::uint32_t bytes, const char* name) {
    FormatInfo i = make(d3d, f, bytes, name);
    i.compressed = true;
    return i;
}

FormatInfo depth(std::uint32_t d3d, VkFormat f, std::uint32_t bytes, const char* name) {
    FormatInfo i = make(d3d, f, bytes, name);
    i.depth = true;
    i.stencil = f == VK_FORMAT_D24_UNORM_S8_UINT || f == VK_FORMAT_D32_SFLOAT_S8_UINT || f == VK_FORMAT_D16_UNORM_S8_UINT;
    return i;
}

}  // namespace

std::optional<FormatInfo> formatInfo(std::uint32_t d, VkFormat d24s8) {
    using namespace d3dfmt;
    switch (d) {
    // D3D9 ARGB formats are stored little-endian, so A8R8G8B8 is B,G,R,A in memory
    case A8R8G8B8: return make(d, VK_FORMAT_B8G8R8A8_UNORM, 4, "A8R8G8B8");
    case X8R8G8B8: return make(d, VK_FORMAT_B8G8R8A8_UNORM, 4, "X8R8G8B8", {ID, ID, ID, ONE});
    case A8B8G8R8: return make(d, VK_FORMAT_R8G8B8A8_UNORM, 4, "A8B8G8R8");
    case X8B8G8R8: return make(d, VK_FORMAT_R8G8B8A8_UNORM, 4, "X8B8G8R8", {ID, ID, ID, ONE});
    case R5G6B5: return make(d, VK_FORMAT_R5G6B5_UNORM_PACK16, 2, "R5G6B5");
    case A1R5G5B5: return make(d, VK_FORMAT_A1R5G5B5_UNORM_PACK16, 2, "A1R5G5B5");
    case X1R5G5B5: return make(d, VK_FORMAT_A1R5G5B5_UNORM_PACK16, 2, "X1R5G5B5", {ID, ID, ID, ONE});
    // D3D A4R4G4B4 = A[15:12] R[11:8] G[7:4] B[3:0]; Vulkan B4G4R4A4 (mandatory) has B[15:12] G[11:8] R[7:4] A[3:0]
    case A4R4G4B4: return make(d, VK_FORMAT_B4G4R4A4_UNORM_PACK16, 2, "A4R4G4B4", {G, R, A, B});
    case X4R4G4B4: return make(d, VK_FORMAT_B4G4R4A4_UNORM_PACK16, 2, "X4R4G4B4", {G, R, A, ONE});
    case A2R10G10B10: return make(d, VK_FORMAT_A2R10G10B10_UNORM_PACK32, 4, "A2R10G10B10");
    case A2B10G10R10: return make(d, VK_FORMAT_A2B10G10R10_UNORM_PACK32, 4, "A2B10G10R10");
    case G16R16: return make(d, VK_FORMAT_R16G16_UNORM, 4, "G16R16", {ID, ID, ONE, ONE});
    case A16B16G16R16: return make(d, VK_FORMAT_R16G16B16A16_UNORM, 8, "A16B16G16R16");
    // D3D9 returns 1 for the components a format does not have
    case R16F: return make(d, VK_FORMAT_R16_SFLOAT, 2, "R16F", {ID, ONE, ONE, ONE});
    case G16R16F: return make(d, VK_FORMAT_R16G16_SFLOAT, 4, "G16R16F", {ID, ID, ONE, ONE});
    case A16B16G16R16F: return make(d, VK_FORMAT_R16G16B16A16_SFLOAT, 8, "A16B16G16R16F");
    case R32F: return make(d, VK_FORMAT_R32_SFLOAT, 4, "R32F", {ID, ONE, ONE, ONE});
    case G32R32F: return make(d, VK_FORMAT_R32G32_SFLOAT, 8, "G32R32F", {ID, ID, ONE, ONE});
    case A32B32G32R32F: return make(d, VK_FORMAT_R32G32B32A32_SFLOAT, 16, "A32B32G32R32F");
    case L8: return make(d, VK_FORMAT_R8_UNORM, 1, "L8", {R, R, R, ONE});
    case L16: return make(d, VK_FORMAT_R16_UNORM, 2, "L16", {R, R, R, ONE});
    case A8: return make(d, VK_FORMAT_R8_UNORM, 1, "A8", {ZERO, ZERO, ZERO, R});
    case A8L8: return make(d, VK_FORMAT_R8G8_UNORM, 2, "A8L8", {R, R, R, G});
    case A4L4: {
        FormatInfo i = make(d, VK_FORMAT_R8G8_UNORM, 2, "A4L4", {R, R, R, G});
        i.convert = Convert::A4L4ToR8G8;
        i.srcBlockBytes = 1;
        return i;
    }
    case V8U8: return make(d, VK_FORMAT_R8G8_SNORM, 2, "V8U8", {ID, ID, ONE, ONE});
    case Q8W8V8U8: return make(d, VK_FORMAT_R8G8B8A8_SNORM, 4, "Q8W8V8U8");
    case V16U16: return make(d, VK_FORMAT_R16G16_SNORM, 4, "V16U16", {ID, ID, ONE, ONE});
    case R8G8B8: {
        FormatInfo i = make(d, VK_FORMAT_B8G8R8A8_UNORM, 4, "R8G8B8", {ID, ID, ID, ONE});
        i.convert = Convert::R8G8B8ToB8G8R8A8;
        i.srcBlockBytes = 3;
        return i;
    }
    case DXT1: return compressed(d, VK_FORMAT_BC1_RGBA_UNORM_BLOCK, 8, "DXT1");
    case DXT2:
    case DXT3: return compressed(d, VK_FORMAT_BC2_UNORM_BLOCK, 16, d == DXT2 ? "DXT2" : "DXT3");
    case DXT4:
    case DXT5: return compressed(d, VK_FORMAT_BC3_UNORM_BLOCK, 16, d == DXT4 ? "DXT4" : "DXT5");
    case ATI1: return compressed(d, VK_FORMAT_BC4_UNORM_BLOCK, 8, "ATI1");
    case ATI2: return compressed(d, VK_FORMAT_BC5_UNORM_BLOCK, 16, "ATI2");
    case D16:
    case D16_LOCKABLE: return depth(d, VK_FORMAT_D16_UNORM, 2, "D16");
    case D24S8:
    case D24X8:
    case D24X4S4:
    case D24FS8:
    case INTZ: return depth(d, d24s8, 4, "D24S8");
    case D32:
    case D32F_LOCKABLE: return depth(d, VK_FORMAT_D32_SFLOAT, 4, "D32F");
    // ATI fetch4-era depth textures: sampled as a plain depth value
    case DF16: return depth(d, VK_FORMAT_D16_UNORM, 2, "DF16");
    case DF24: return depth(d, VK_FORMAT_D32_SFLOAT, 4, "DF24");
    default: return std::nullopt;
    }
}

const char* d3dFormatName(std::uint32_t d) {
    if (d == d3dfmt::NULLFMT) {
        return "NULL";
    }
    const auto f = formatInfo(d);
    return f ? f->name : "?";
}

Pitch d3dPitch(const FormatInfo& f, std::uint32_t width, std::uint32_t height) {
    if (f.compressed) {
        return {std::max(1u, (width + 3) / 4) * f.blockBytes, std::max(1u, (height + 3) / 4)};
    }
    return {width * f.srcBlockBytes, height};
}

std::uint64_t storageSize(const FormatInfo& f, std::uint32_t width, std::uint32_t height, std::uint32_t depth) {
    if (f.compressed) {
        return static_cast<std::uint64_t>(std::max(1u, (width + 3) / 4)) * std::max(1u, (height + 3) / 4) * f.blockBytes * depth;
    }
    return static_cast<std::uint64_t>(width) * height * depth * f.blockBytes;
}

void convertLevel(const FormatInfo& f, const void* src, std::uint32_t srcRowPitch, void* dst, std::uint32_t width,
                  std::uint32_t height) {
    const auto* s = static_cast<const std::uint8_t*>(src);
    auto* o = static_cast<std::uint8_t*>(dst);
    const Pitch p = d3dPitch(f, width, height);
    const std::uint32_t dstRow = f.compressed ? p.rowBytes : width * f.blockBytes;
    switch (f.convert) {
    case Convert::None:
        for (std::uint32_t y = 0; y < p.rows; ++y) {
            std::memcpy(o + static_cast<std::size_t>(y) * dstRow, s + static_cast<std::size_t>(y) * srcRowPitch, dstRow);
        }
        break;
    case Convert::R8G8B8ToB8G8R8A8:
        for (std::uint32_t y = 0; y < height; ++y) {
            const std::uint8_t* row = s + static_cast<std::size_t>(y) * srcRowPitch;
            std::uint8_t* out = o + static_cast<std::size_t>(y) * dstRow;
            for (std::uint32_t x = 0; x < width; ++x) {
                out[x * 4 + 0] = row[x * 3 + 0];
                out[x * 4 + 1] = row[x * 3 + 1];
                out[x * 4 + 2] = row[x * 3 + 2];
                out[x * 4 + 3] = 255;
            }
        }
        break;
    case Convert::A4L4ToR8G8:
        for (std::uint32_t y = 0; y < height; ++y) {
            const std::uint8_t* row = s + static_cast<std::size_t>(y) * srcRowPitch;
            std::uint8_t* out = o + static_cast<std::size_t>(y) * dstRow;
            for (std::uint32_t x = 0; x < width; ++x) {
                const std::uint8_t v = row[x];
                out[x * 2 + 0] = static_cast<std::uint8_t>((v & 0x0F) * 17);
                out[x * 2 + 1] = static_cast<std::uint8_t>((v >> 4) * 17);
            }
        }
        break;
    }
}

}  // namespace rvk::vk
