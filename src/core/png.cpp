#include "core/png.h"

#include <vector>

// static: the tools link their own stb_image_write
#define STB_IMAGE_WRITE_STATIC
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include <stb_image_write.h>

namespace rvk {

std::vector<std::uint8_t> encodePngBgra(std::span<const std::uint8_t> bgra, std::uint32_t width, std::uint32_t height) {
    std::vector<std::uint8_t> out;
    if (width == 0 || height == 0 || bgra.size() < static_cast<std::size_t>(width) * height * 4) {
        return out;
    }
    std::vector<std::uint8_t> rgb(static_cast<std::size_t>(width) * height * 3);
    for (std::size_t i = 0, o = 0; o < rgb.size(); i += 4, o += 3) {
        rgb[o] = bgra[i + 2];
        rgb[o + 1] = bgra[i + 1];
        rgb[o + 2] = bgra[i];
    }
    const auto append = [](void* ctx, void* data, int size) {
        auto& v = *static_cast<std::vector<std::uint8_t>*>(ctx);
        const auto* p = static_cast<const std::uint8_t*>(data);
        v.insert(v.end(), p, p + size);
    };
    if (stbi_write_png_to_func(append, &out, static_cast<int>(width), static_cast<int>(height), 3, rgb.data(),
                               static_cast<int>(width * 3)) == 0) {
        out.clear();
    }
    return out;
}

}  // namespace rvk
