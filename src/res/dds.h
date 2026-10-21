#pragma once

// DDS files (the payload of Risen's ._ximg images): header -> D3DFORMAT, size, mips, cube faces / volume depth, and where each
// subresource's data starts. Replaces D3DXGetImageInfoFromFileInMemory / D3DXCreateTextureFromFileInMemoryEx.

#include <cstdint>
#include <optional>
#include <span>
#include <string>

namespace rvk::res {

struct DdsImage {
    std::uint32_t d3dFormat = 0;  // D3DFORMAT (FOURCC for DXTn)
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::uint32_t depth = 1;      // > 1 for volume textures
    std::uint32_t mips = 1;
    std::uint32_t faces = 1;      // 6 for cube maps
    bool cube = false;
    bool volume = false;
    std::span<const std::uint8_t> data;  // every subresource, DDS order: face-major, then mip

    struct Level {
        std::span<const std::uint8_t> bytes;
        std::uint32_t width, height, depth;
        std::uint32_t rowPitch;   // D3D9 row pitch (bytes per row of pixels, or of 4x4 blocks)
        std::uint32_t slicePitch;
    };
    // data of one face/mip; nullopt for an index out of range or a truncated file
    std::optional<Level> level(std::uint32_t face, std::uint32_t mip) const;
};

std::optional<DdsImage> parseDds(std::span<const std::uint8_t> file, std::string* error = nullptr);

// bytes per pixel of uncompressed D3D formats / bytes per 4x4 block of DXTn; 0 if unknown
std::uint32_t d3dFormatBlockBytes(std::uint32_t d3dFormat, bool* compressed = nullptr);

}  // namespace rvk::res
