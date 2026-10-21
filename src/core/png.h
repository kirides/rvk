#pragma once

#include <cstdint>
#include <span>
#include <vector>

namespace rvk {

// tightly packed BGRA8 pixels -> an opaque RGB PNG in memory (empty on failure); files: api::writeImage
std::vector<std::uint8_t> encodePngBgra(std::span<const std::uint8_t> bgra, std::uint32_t width, std::uint32_t height);

}  // namespace rvk
