#include "res/dds.h"

#include <algorithm>
#include <cstring>
#include <format>

#include "vk/formats.h"

namespace rvk::res {
namespace {

constexpr std::uint32_t kMagic = 0x20534444;  // "DDS "
constexpr std::uint32_t DDSD_MIPMAPCOUNT = 0x20000;
constexpr std::uint32_t DDSD_DEPTH = 0x800000;
constexpr std::uint32_t DDPF_ALPHAPIXELS = 0x1, DDPF_ALPHA = 0x2, DDPF_FOURCC = 0x4, DDPF_RGB = 0x40, DDPF_LUMINANCE = 0x20000,
                        DDPF_BUMPDUDV = 0x80000;
constexpr std::uint32_t DDSCAPS2_CUBEMAP = 0x200, DDSCAPS2_VOLUME = 0x200000;

struct PixelFormat {
    std::uint32_t size, flags, fourCC, bitCount, rMask, gMask, bMask, aMask;
};
struct Header {
    std::uint32_t size, flags, height, width, pitchOrLinearSize, depth, mipMapCount, reserved1[11];
    PixelFormat pf;
    std::uint32_t caps, caps2, caps3, caps4, reserved2;
};
static_assert(sizeof(Header) == 124);

std::uint32_t formatFromMasks(const PixelFormat& pf) {
    using namespace vk::d3dfmt;
    const bool alpha = (pf.flags & DDPF_ALPHAPIXELS) != 0;
    const std::uint32_t a = alpha ? pf.aMask : 0;
    if ((pf.flags & DDPF_RGB) != 0) {
        switch (pf.bitCount) {
        case 32:
            if (pf.rMask == 0xff0000 && pf.gMask == 0xff00 && pf.bMask == 0xff) return a == 0xff000000 ? A8R8G8B8 : X8R8G8B8;
            if (pf.rMask == 0xff && pf.gMask == 0xff00 && pf.bMask == 0xff0000) return a == 0xff000000 ? A8B8G8R8 : X8B8G8R8;
            if (pf.rMask == 0xffff && pf.gMask == 0xffff0000) return G16R16;
            if (pf.rMask == 0x3ff00000 && pf.gMask == 0xffc00 && pf.bMask == 0x3ff) return A2R10G10B10;
            if (pf.rMask == 0x3ff && pf.gMask == 0xffc00 && pf.bMask == 0x3ff00000) return A2B10G10R10;
            break;
        case 24:
            if (pf.rMask == 0xff0000 && pf.gMask == 0xff00 && pf.bMask == 0xff) return R8G8B8;
            break;
        case 16:
            if (pf.rMask == 0xf800 && pf.gMask == 0x7e0 && pf.bMask == 0x1f) return R5G6B5;
            if (pf.rMask == 0x7c00 && pf.gMask == 0x3e0 && pf.bMask == 0x1f) return a == 0x8000 ? A1R5G5B5 : X1R5G5B5;
            if (pf.rMask == 0xf00 && pf.gMask == 0xf0 && pf.bMask == 0xf) return a == 0xf000 ? A4R4G4B4 : X4R4G4B4;
            break;
        default: break;
        }
    } else if ((pf.flags & DDPF_LUMINANCE) != 0) {
        if (pf.bitCount == 8 && pf.rMask == 0xff) return L8;
        if (pf.bitCount == 8 && pf.rMask == 0xf && a == 0xf0) return A4L4;
        if (pf.bitCount == 16 && pf.rMask == 0xff && a == 0xff00) return A8L8;
        if (pf.bitCount == 16 && pf.rMask == 0xffff) return L16;
    } else if ((pf.flags & DDPF_ALPHA) != 0) {
        if (pf.bitCount == 8 && pf.aMask == 0xff) return A8;
    } else if ((pf.flags & DDPF_BUMPDUDV) != 0) {
        if (pf.bitCount == 16 && pf.rMask == 0xff && pf.gMask == 0xff00) return V8U8;
        if (pf.bitCount == 32 && pf.rMask == 0xff && pf.gMask == 0xff00 && pf.bMask == 0xff0000) return Q8W8V8U8;
        if (pf.bitCount == 32 && pf.rMask == 0xffff && pf.gMask == 0xffff0000) return V16U16;
    }
    return 0;
}

std::uint64_t levelBytes(std::uint32_t fmt, std::uint32_t w, std::uint32_t h, std::uint32_t d, std::uint32_t& rowPitch,
                         std::uint32_t& slicePitch) {
    bool compressed = false;
    const std::uint32_t bb = d3dFormatBlockBytes(fmt, &compressed);
    if (compressed) {
        const std::uint32_t bw = std::max(1u, (w + 3) / 4), bh = std::max(1u, (h + 3) / 4);
        rowPitch = bw * bb;
        slicePitch = rowPitch * bh;
    } else {
        rowPitch = w * bb;
        slicePitch = rowPitch * h;
    }
    return static_cast<std::uint64_t>(slicePitch) * d;
}

}  // namespace

std::uint32_t d3dFormatBlockBytes(std::uint32_t fmt, bool* compressed) {
    const auto f = vk::formatInfo(fmt);
    if (!f) {
        return 0;
    }
    if (compressed != nullptr) {
        *compressed = f->compressed;
    }
    return f->compressed ? f->blockBytes : f->srcBlockBytes;
}

std::optional<DdsImage::Level> DdsImage::level(std::uint32_t face, std::uint32_t mip) const {
    if (face >= faces || mip >= mips) {
        return std::nullopt;
    }
    std::uint64_t offset = 0;
    for (std::uint32_t f = 0; f <= face; ++f) {
        std::uint32_t w = width, h = height, d = depth;
        for (std::uint32_t m = 0; m < mips; ++m) {
            std::uint32_t row = 0, slice = 0;
            const std::uint64_t size = levelBytes(d3dFormat, w, h, d, row, slice);
            if (f == face && m == mip) {
                if (offset + size > data.size()) {
                    return std::nullopt;
                }
                return Level{data.subspan(static_cast<std::size_t>(offset), static_cast<std::size_t>(size)), w, h, d, row, slice};
            }
            offset += size;
            w = std::max(1u, w / 2);
            h = std::max(1u, h / 2);
            d = std::max(1u, d / 2);
        }
    }
    return std::nullopt;
}

std::optional<DdsImage> parseDds(std::span<const std::uint8_t> file, std::string* error) {
    auto fail = [&](std::string msg) -> std::optional<DdsImage> {
        if (error != nullptr) {
            *error = std::move(msg);
        }
        return std::nullopt;
    };
    if (file.size() < 4 + sizeof(Header)) {
        return fail("file too small for a DDS header");
    }
    std::uint32_t magic = 0;
    std::memcpy(&magic, file.data(), 4);
    if (magic != kMagic) {
        return fail("no DDS magic");
    }
    Header h{};
    std::memcpy(&h, file.data() + 4, sizeof(Header));
    if (h.size != 124 || h.pf.size != 32) {
        return fail(std::format("bad header sizes {} / {}", h.size, h.pf.size));
    }
    std::size_t dataOffset = 4 + sizeof(Header);
    DdsImage img;
    if ((h.pf.flags & DDPF_FOURCC) != 0) {
        if (h.pf.fourCC == vk::d3dfmt::fourcc('D', 'X', '1', '0')) {
            return fail("DX10 DDS header not supported");
        }
        img.d3dFormat = h.pf.fourCC;  // DXTn FOURCC or a numeric D3DFORMAT (e.g. 113 = A16B16G16R16F)
    } else {
        img.d3dFormat = formatFromMasks(h.pf);
    }
    if (img.d3dFormat == 0 || d3dFormatBlockBytes(img.d3dFormat) == 0) {
        return fail(std::format("unsupported pixel format (flags 0x{:x}, fourcc 0x{:08x}, {} bits)", h.pf.flags, h.pf.fourCC,
                                h.pf.bitCount));
    }
    img.width = std::max(1u, h.width);
    img.height = std::max(1u, h.height);
    img.mips = (h.flags & DDSD_MIPMAPCOUNT) != 0 ? std::max(1u, h.mipMapCount) : 1;
    img.cube = (h.caps2 & DDSCAPS2_CUBEMAP) != 0;
    img.volume = (h.caps2 & DDSCAPS2_VOLUME) != 0 && (h.flags & DDSD_DEPTH) != 0;
    img.faces = img.cube ? 6 : 1;
    img.depth = img.volume ? std::max(1u, h.depth) : 1;
    img.data = file.subspan(dataOffset);
    if (!img.level(img.faces - 1, img.mips - 1)) {
        return fail("truncated DDS data");
    }
    return img;
}

}  // namespace rvk::res
