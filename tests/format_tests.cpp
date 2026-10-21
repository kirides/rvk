// D3D9 format table and DDS parser tests.

#include <cstring>
#include <vector>

#include "res/dds.h"
#include "test.h"
#include "vk/formats.h"

using namespace rvk;

namespace {

// a DDS file in memory: header for the given pixel format + `dataBytes` of pattern data
std::vector<std::uint8_t> makeDds(std::uint32_t w, std::uint32_t h, std::uint32_t mips, std::uint32_t pfFlags, std::uint32_t fourcc,
                                  std::uint32_t bits, std::uint32_t r, std::uint32_t g, std::uint32_t b, std::uint32_t a,
                                  std::size_t dataBytes, std::uint32_t caps2 = 0, std::uint32_t depth = 0) {
    std::vector<std::uint8_t> f(4 + 124 + dataBytes);
    auto put = [&](std::size_t off, std::uint32_t v) { std::memcpy(f.data() + off, &v, 4); };
    put(0, 0x20534444);
    put(4, 124);
    put(8, 0x1 | 0x2 | 0x4 | 0x1000 | (mips > 1 ? 0x20000u : 0u) | (depth > 0 ? 0x800000u : 0u));
    put(12, h);
    put(16, w);
    put(24, depth);
    put(28, mips);
    put(4 + 72, 32);
    put(4 + 76, pfFlags);
    put(4 + 80, fourcc);
    put(4 + 84, bits);
    put(4 + 88, r);
    put(4 + 92, g);
    put(4 + 96, b);
    put(4 + 100, a);
    put(4 + 108, caps2);
    for (std::size_t i = 0; i < dataBytes; ++i) {
        f[4 + 124 + i] = static_cast<std::uint8_t>(i);
    }
    return f;
}

}  // namespace

TEST(formats_table) {
    using namespace vk::d3dfmt;
    auto f = vk::formatInfo(A8R8G8B8);
    CHECK(f && f->format == VK_FORMAT_B8G8R8A8_UNORM && f->blockBytes == 4);
    f = vk::formatInfo(X8R8G8B8);
    CHECK(f && f->swizzle.a == VK_COMPONENT_SWIZZLE_ONE);
    f = vk::formatInfo(R32F);
    CHECK(f && f->format == VK_FORMAT_R32_SFLOAT && f->swizzle.g == VK_COMPONENT_SWIZZLE_ONE);
    f = vk::formatInfo(DXT5);
    CHECK(f && f->compressed && f->format == VK_FORMAT_BC3_UNORM_BLOCK && f->blockBytes == 16);
    f = vk::formatInfo(D24S8, VK_FORMAT_D32_SFLOAT_S8_UINT);
    CHECK(f && f->depth && f->stencil && f->format == VK_FORMAT_D32_SFLOAT_S8_UINT);
    f = vk::formatInfo(DF24);
    CHECK(f && f->depth && f->format == VK_FORMAT_D32_SFLOAT);
    CHECK(!vk::formatInfo(NULLFMT));
    CHECK(!vk::formatInfo(12345));
    const auto p = vk::d3dPitch(*vk::formatInfo(DXT1), 13, 7);  // 4 x 2 blocks
    CHECK(p.rowBytes == 32 && p.rows == 2);
}

TEST(formats_convert) {
    const auto rgb = *vk::formatInfo(vk::d3dfmt::R8G8B8);
    const std::uint8_t src[6] = {1, 2, 3, 4, 5, 6};  // two pixels B,G,R
    std::uint8_t dst[8]{};
    vk::convertLevel(rgb, src, 6, dst, 2, 1);
    CHECK(dst[0] == 1 && dst[1] == 2 && dst[2] == 3 && dst[3] == 255 && dst[4] == 4 && dst[7] == 255);
    const auto a4l4 = *vk::formatInfo(vk::d3dfmt::A4L4);
    const std::uint8_t s2[1] = {0xF3};
    std::uint8_t d2[2]{};
    vk::convertLevel(a4l4, s2, 1, d2, 1, 1);
    CHECK(d2[0] == 3 * 17 && d2[1] == 255);
}

TEST(dds_dxt1_mips) {
    // 64x64 DXT1 with 7 mips: 2048 + 512 + 128 + 32 + 8 + 8 + 8 bytes
    const auto file = makeDds(64, 64, 7, 0x4, vk::d3dfmt::DXT1, 0, 0, 0, 0, 0, 2744);
    std::string err;
    const auto img = res::parseDds(file, &err);
    CHECK_MSG(img.has_value(), err);
    if (!img) return;
    CHECK(img->d3dFormat == vk::d3dfmt::DXT1 && img->width == 64 && img->mips == 7 && !img->cube);
    const auto l0 = img->level(0, 0);
    const auto l1 = img->level(0, 1);
    const auto l6 = img->level(0, 6);
    CHECK(l0 && l0->bytes.size() == 2048 && l0->rowPitch == 128);
    CHECK(l1 && l1->bytes.size() == 512 && l1->bytes.data() == file.data() + 128 + 2048);
    CHECK(l6 && l6->width == 1 && l6->bytes.size() == 8);
    CHECK(!img->level(0, 7));
    // one byte short: truncated
    auto shorter = file;
    shorter.pop_back();
    CHECK(!res::parseDds(shorter));
}

TEST(dds_masks_and_cube) {
    auto img = res::parseDds(makeDds(4, 4, 1, 0x41, 0, 32, 0xff0000, 0xff00, 0xff, 0xff000000, 64));
    CHECK(img && img->d3dFormat == vk::d3dfmt::A8R8G8B8);
    img = res::parseDds(makeDds(4, 4, 1, 0x40, 0, 32, 0xff0000, 0xff00, 0xff, 0, 64));
    CHECK(img && img->d3dFormat == vk::d3dfmt::X8R8G8B8);
    img = res::parseDds(makeDds(4, 4, 1, 0x20000, 0, 8, 0xff, 0, 0, 0, 16));
    CHECK(img && img->d3dFormat == vk::d3dfmt::L8);
    img = res::parseDds(makeDds(4, 4, 1, 0x2, 0, 8, 0, 0, 0, 0xff, 16));
    CHECK(img && img->d3dFormat == vk::d3dfmt::A8);
    // numeric FOURCC: A16B16G16R16F
    img = res::parseDds(makeDds(2, 2, 1, 0x4, 113, 0, 0, 0, 0, 0, 32));
    CHECK(img && img->d3dFormat == 113);
    // cube map 8x8 A8R8G8B8, 1 mip: 6 faces of 256 bytes
    const auto cube = makeDds(8, 8, 1, 0x41, 0, 32, 0xff0000, 0xff00, 0xff, 0xff000000, 6 * 256, 0x200 | 0xFC00);
    img = res::parseDds(cube);
    CHECK(img && img->cube && img->faces == 6);
    if (img) {
        const auto f5 = img->level(5, 0);
        CHECK(f5 && f5->bytes.data() == cube.data() + 128 + 5 * 256);
    }
    CHECK(!res::parseDds(std::vector<std::uint8_t>(10)));
}
