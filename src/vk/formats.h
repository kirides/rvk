#pragma once

// D3D9 surface formats (D3DFORMAT, = the engine's eEColorFormat values) -> Vulkan formats.

#include <cstdint>
#include <optional>

#include "vk/vk.h"

namespace rvk::vk {

namespace d3dfmt {
constexpr std::uint32_t fourcc(char a, char b, char c, char d) {
    return static_cast<std::uint32_t>(static_cast<unsigned char>(a)) | (static_cast<std::uint32_t>(static_cast<unsigned char>(b)) << 8) |
           (static_cast<std::uint32_t>(static_cast<unsigned char>(c)) << 16) |
           (static_cast<std::uint32_t>(static_cast<unsigned char>(d)) << 24);
}
inline constexpr std::uint32_t R8G8B8 = 20, A8R8G8B8 = 21, X8R8G8B8 = 22, R5G6B5 = 23, X1R5G5B5 = 24, A1R5G5B5 = 25,
                               A4R4G4B4 = 26, R3G3B2 = 27, A8 = 28, A8R3G3B2 = 29, X4R4G4B4 = 30, A2B10G10R10 = 31,
                               A8B8G8R8 = 32, X8B8G8R8 = 33, G16R16 = 34, A2R10G10B10 = 35, A16B16G16R16 = 36, L8 = 50,
                               A8L8 = 51, A4L4 = 52, V8U8 = 60, L6V5U5 = 61, X8L8V8U8 = 62, Q8W8V8U8 = 63, V16U16 = 64,
                               A2W10V10U10 = 67, D16_LOCKABLE = 70, D32 = 71, D15S1 = 73, D24S8 = 75, D24X8 = 77,
                               D24X4S4 = 79, D16 = 80, D32F_LOCKABLE = 82, D24FS8 = 83, L16 = 81, R16F = 111,
                               G16R16F = 112, A16B16G16R16F = 113, R32F = 114, G32R32F = 115, A32B32G32R32F = 116;
inline constexpr std::uint32_t DXT1 = fourcc('D', 'X', 'T', '1'), DXT2 = fourcc('D', 'X', 'T', '2'),
                               DXT3 = fourcc('D', 'X', 'T', '3'), DXT4 = fourcc('D', 'X', 'T', '4'),
                               DXT5 = fourcc('D', 'X', 'T', '5'), ATI1 = fourcc('A', 'T', 'I', '1'),
                               ATI2 = fourcc('A', 'T', 'I', '2'), DF16 = fourcc('D', 'F', '1', '6'),
                               DF24 = fourcc('D', 'F', '2', '4'), INTZ = fourcc('I', 'N', 'T', 'Z'),
                               NULLFMT = fourcc('N', 'U', 'L', 'L');
}  // namespace d3dfmt

enum class Convert : std::uint8_t {
    None,
    R8G8B8ToB8G8R8A8,  // 24-bit, no Vulkan equivalent with mandatory support
    A4L4ToR8G8,
};

struct FormatInfo {
    std::uint32_t d3d = 0;
    VkFormat format = VK_FORMAT_UNDEFINED;   // storage format
    VkComponentMapping swizzle{};            // view swizzle giving D3D9's sampled values
    std::uint32_t blockBytes = 0;            // bytes per pixel, or per 4x4 block when compressed
    bool compressed = false;
    bool depth = false;
    bool stencil = false;
    Convert convert = Convert::None;
    std::uint32_t srcBlockBytes = 0;         // bytes per pixel of the D3D9 data when converted (else = blockBytes)
    const char* name = "";
};

// nullopt for formats the renderer does not support. `depthFormat` is the device's choice for D24S8 (D24_UNORM_S8_UINT is
// optional, AMD lacks it: D32_SFLOAT_S8_UINT then).
std::optional<FormatInfo> formatInfo(std::uint32_t d3dFormat, VkFormat d24s8 = VK_FORMAT_D24_UNORM_S8_UINT);

const char* d3dFormatName(std::uint32_t d3dFormat);

// D3D9 data layout of one mip level: row pitch in bytes and number of rows (blocks for compressed formats)
struct Pitch {
    std::uint32_t rowBytes = 0;
    std::uint32_t rows = 0;
};
Pitch d3dPitch(const FormatInfo& f, std::uint32_t width, std::uint32_t height);
// bytes of one mip level of the storage (Vulkan) format, tightly packed
std::uint64_t storageSize(const FormatInfo& f, std::uint32_t width, std::uint32_t height, std::uint32_t depth = 1);

// copies one tightly packed D3D9 mip level (`src`, d3dPitch rows) into storage layout (`dst`), converting if needed
void convertLevel(const FormatInfo& f, const void* src, std::uint32_t srcRowPitch, void* dst, std::uint32_t width,
                  std::uint32_t height);

}  // namespace rvk::vk
