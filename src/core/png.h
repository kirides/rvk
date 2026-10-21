#pragma once

#include <cstdint>
#include <filesystem>
#include <span>

namespace rvk {

// writes tightly packed BGRA8 pixels as an opaque RGB PNG; creates the directory
bool writePngBgra(const std::filesystem::path& file, std::span<const std::uint8_t> bgra, std::uint32_t width, std::uint32_t height);

}  // namespace rvk
