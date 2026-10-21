#include "core/png.h"

#include <vector>

// static: the tools link their own stb_image_write
#define STB_IMAGE_WRITE_STATIC
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include <stb_image_write.h>

namespace rvk {

bool writePngBgra(const std::filesystem::path& file, std::span<const std::uint8_t> bgra, std::uint32_t width, std::uint32_t height) {
    if (bgra.size() < static_cast<std::size_t>(width) * height * 4) {
        return false;
    }
    std::vector<std::uint8_t> rgb(static_cast<std::size_t>(width) * height * 3);
    for (std::size_t i = 0, o = 0; o < rgb.size(); i += 4, o += 3) {
        rgb[o] = bgra[i + 2];
        rgb[o + 1] = bgra[i + 1];
        rgb[o + 2] = bgra[i];
    }
    std::error_code ec;
    std::filesystem::create_directories(file.parent_path(), ec);
    return stbi_write_png(file.string().c_str(), static_cast<int>(width), static_cast<int>(height), 3, rgb.data(),
                          static_cast<int>(width * 3)) != 0;
}

}  // namespace rvk
