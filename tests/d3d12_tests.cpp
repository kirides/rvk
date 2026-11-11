// The pure parts of the D3D12 backend (rhi/d3d12/d3d12_test_hooks.h): the DXGI format tables and the PSO raster key.

#include <cstdint>

#include "rhi/d3d12/d3d12_test_hooks.h"
#include "rhi/d3d12/heap_alloc.h"
#include "rhi/d3d_formats.h"
#include "rhi/game_device.h"
#include "test.h"

using namespace rvk;
namespace t = rvk::rhi::d3d12test;
using rvk::rhi::DynamicStateValues;

namespace {
// DXGI_FORMAT values (dxgiformat.h)
constexpr std::uint32_t kR32G32B32A32_FLOAT = 2, kR16G16B16A16_FLOAT = 10, kR32_FLOAT = 41, kR32_TYPELESS = 39, kR8G8B8A8_UNORM = 28, kR8G8B8A8_UINT = 30,
                        kD32_FLOAT = 40, kD32_FLOAT_S8X24_UINT = 20, kR32G8X24_TYPELESS = 19, kR32_FLOAT_X8X24_TYPELESS = 21, kD24_UNORM_S8_UINT = 45,
                        kR24G8_TYPELESS = 44, kR24_UNORM_X8_TYPELESS = 46, kR16_TYPELESS = 53, kD16_UNORM = 55, kR16_UNORM = 56, kBC1_UNORM = 71,
                        kBC3_UNORM = 77, kBC5_UNORM = 83, kB8G8R8A8_UNORM = 87, kR10G10B10A2_UNORM = 24, kR10G10B10A2_UINT = 25, kR16G16_SINT = 38,
                        kR16G16B16A16_SINT = 14;
}  // namespace

TEST(d3d12_format_table_known_values) {
    CHECK(t::dxgiFormat(VK_FORMAT_R32G32B32A32_SFLOAT) == kR32G32B32A32_FLOAT);
    CHECK(t::dxgiFormat(VK_FORMAT_R16G16B16A16_SFLOAT) == kR16G16B16A16_FLOAT);
    CHECK(t::dxgiFormat(VK_FORMAT_R32_SFLOAT) == kR32_FLOAT);
    CHECK(t::dxgiFormat(VK_FORMAT_B8G8R8A8_UNORM) == kB8G8R8A8_UNORM);
    CHECK(t::dxgiFormat(VK_FORMAT_BC1_RGBA_UNORM_BLOCK) == kBC1_UNORM);
    CHECK(t::dxgiFormat(VK_FORMAT_BC3_UNORM_BLOCK) == kBC3_UNORM);
    CHECK(t::dxgiFormat(VK_FORMAT_BC5_UNORM_BLOCK) == kBC5_UNORM);
    CHECK(t::dxgiFormat(VK_FORMAT_D32_SFLOAT_S8_UINT) == kD32_FLOAT_S8X24_UINT);
    CHECK(t::dxgiFormat(VK_FORMAT_D16_UNORM) == kD16_UNORM);
    CHECK(t::dxgiFormat(VK_FORMAT_UNDEFINED) == 0);
    CHECK(t::dxgiFormat(VK_FORMAT_R8G8B8_UNORM) == 0);  // 24-bit RGB: the D3D9 format layer converts it to B8G8R8A8 before it gets here
}

TEST(d3d12_format_table_covers_the_d3d9_formats) {
    // every format the D3D9 layer produces for a surface (rhi::formatInfo) has a DXGI twin, except the ones the game never creates; the
    // depth formats follow the device's D24S8 choice (D32S8 like AMD's Vulkan)
    using namespace rhi::d3dfmt;
    const std::uint32_t formats[] = {A8R8G8B8, X8R8G8B8, R5G6B5, X1R5G5B5, A1R5G5B5, A4R4G4B4, X4R4G4B4, A8, L8, A8L8, A2B10G10R10, A2R10G10B10, G16R16,
                                     A16B16G16R16, V8U8, Q8W8V8U8, V16U16, L16, R16F, G16R16F, A16B16G16R16F, R32F, G32R32F, A32B32G32R32F, DXT1,
                                     DXT3, DXT5, ATI1, ATI2, D16, D24S8, D32F_LOCKABLE, R8G8B8, A4L4};
    for (const std::uint32_t d3d : formats) {
        const auto info = rhi::formatInfo(d3d, VK_FORMAT_D32_SFLOAT_S8_UINT);
        if (!info) continue;  // (unsupported on both backends)
        const std::uint32_t dxgi = t::dxgiFormat(info->format);
        if (dxgi == 0) std::printf("    d3d9 format %u (%s): Vulkan format %d has no DXGI twin\n", d3d, info->name, static_cast<int>(info->format));
        CHECK(dxgi != 0);
    }
}

TEST(d3d12_depth_formats_typeless_when_sampled) {
    CHECK(t::dxgiResourceFormat(VK_FORMAT_D32_SFLOAT, false) == kD32_FLOAT);
    CHECK(t::dxgiResourceFormat(VK_FORMAT_D32_SFLOAT, true) == kR32_TYPELESS);
    CHECK(t::dxgiSrvFormat(VK_FORMAT_D32_SFLOAT) == kR32_FLOAT);
    CHECK(t::dxgiResourceFormat(VK_FORMAT_D32_SFLOAT_S8_UINT, true) == kR32G8X24_TYPELESS);
    CHECK(t::dxgiSrvFormat(VK_FORMAT_D32_SFLOAT_S8_UINT) == kR32_FLOAT_X8X24_TYPELESS);
    CHECK(t::dxgiResourceFormat(VK_FORMAT_D24_UNORM_S8_UINT, true) == kR24G8_TYPELESS);
    CHECK(t::dxgiSrvFormat(VK_FORMAT_D24_UNORM_S8_UINT) == kR24_UNORM_X8_TYPELESS);
    CHECK(t::dxgiResourceFormat(VK_FORMAT_D16_UNORM, true) == kR16_TYPELESS);
    CHECK(t::dxgiSrvFormat(VK_FORMAT_D16_UNORM) == kR16_UNORM);
    CHECK(t::dxgiResourceFormat(VK_FORMAT_R32_SFLOAT, true) == kR32_FLOAT);  // colour formats are typed
    CHECK(t::dxgiFormat(VK_FORMAT_D24_UNORM_S8_UINT) == kD24_UNORM_S8_UINT);
}

TEST(d3d12_vertex_formats_scaled_integers) {
    // D3D9 UBYTE4 (blend indices): read as UNORM, the vertex shader multiplies by 255 (translator flags); SHORT2 / SHORT4 / UDEC3: integers
    CHECK(t::dxgiVertexFormat(VK_FORMAT_R8G8B8A8_USCALED) == kR8G8B8A8_UNORM);
    CHECK(t::dxgiVertexFormat(VK_FORMAT_R16G16_SSCALED) == kR16G16_SINT);
    CHECK(t::dxgiVertexFormat(VK_FORMAT_R16G16B16A16_SSCALED) == kR16G16B16A16_SINT);
    CHECK(t::dxgiVertexFormat(VK_FORMAT_A2B10G10R10_USCALED_PACK32) == kR10G10B10A2_UINT);
    CHECK(t::dxgiVertexFormat(VK_FORMAT_R32G32B32_SFLOAT) == 6);  // R32G32B32_FLOAT: plain formats fall through to the typed table
    CHECK(t::dxgiVertexFormat(VK_FORMAT_R8G8B8A8_UNORM) == kR8G8B8A8_UNORM);
    (void)kR8G8B8A8_UINT;
    (void)kR10G10B10A2_UNORM;
}

TEST(d3d12_raster_key_is_a_plain_byte_key) {
    // the key is compared (memcmp) and hashed as bytes: no padding
    CHECK(t::rasterKeySize() == t::rasterKeyFieldBytes());
}

TEST(d3d12_raster_key_equality) {
    DynamicStateValues a{};
    a.cull = VK_CULL_MODE_BACK_BIT;
    a.depthTest = 1;
    a.depthWrite = 1;
    a.depthCompare = VK_COMPARE_OP_LESS_OR_EQUAL;
    DynamicStateValues b = a;
    CHECK(t::rasterKeysEqual(a, b));
    // state that does not enter the PSO (viewport, scissor, blend constants, stencil reference with stencil off, depth compare with depth off) does not split the key
    b.viewport.width = 640.0f;
    b.scissor.extent.width = 640;
    b.blendConstants[0] = 0.5f;
    b.stencilMasks[2] = 7;
    CHECK(t::rasterKeysEqual(a, b));
    DynamicStateValues c = a;
    c.depthTest = 0;
    c.depthCompare = VK_COMPARE_OP_ALWAYS;
    DynamicStateValues d = c;
    d.depthCompare = VK_COMPARE_OP_NEVER;
    CHECK(t::rasterKeysEqual(c, d));  // (depth test off: the compare function is irrelevant)
    CHECK(!t::rasterKeysEqual(a, c));
    DynamicStateValues e = a;
    e.cull = VK_CULL_MODE_FRONT_BIT;
    CHECK(!t::rasterKeysEqual(a, e));
    DynamicStateValues f = a;
    f.depthWrite = 0;
    CHECK(!t::rasterKeysEqual(a, f));
    // stencil state only counts with the stencil test on
    DynamicStateValues g = a;
    g.stencilFrontCompare = VK_COMPARE_OP_EQUAL;
    CHECK(t::rasterKeysEqual(a, g));
    g.stencilTest = 1;
    CHECK(!t::rasterKeysEqual(a, g));
    DynamicStateValues h = g;
    h.stencilMasks[0] = 0x0f;
    CHECK(!t::rasterKeysEqual(g, h));
}

TEST(d3d12_raster_key_depth_bias_rounds_away_from_zero) {
    DynamicStateValues d{};
    d.depthBiasEnable = 1;
    d.depthBias[0] = 0.3f;
    CHECK(t::rasterKeyBias(d) == 1);  // a bias below one unit must not vanish
    d.depthBias[0] = -0.3f;
    CHECK(t::rasterKeyBias(d) == -1);
    d.depthBias[0] = 2.0f;
    CHECK(t::rasterKeyBias(d) == 2);
    d.depthBias[0] = 0.0f;
    CHECK(t::rasterKeyBias(d) == 0);
    d.depthBias[0] = 5.0f;
    d.depthBiasEnable = 0;
    CHECK(t::rasterKeyBias(d) == 0);  // off: no bias in the key
    DynamicStateValues e = d;
    e.depthBias[0] = 9.0f;
    CHECK(t::rasterKeysEqual(d, e));
}

TEST(d3d12_heap_allocator_low_range) {
    rvk::rhi::HeapAllocator a;
    a.next = 3;
    a.limit = 100;
    a.reserveLow(10, 14);
    for (int i = 0; i < 7; ++i) CHECK(a.alloc() < 10);  // 3..9
    CHECK(a.alloc() == 14);                              // hops over [10, 14)
    for (std::uint32_t want = 10; want < 14; ++want) CHECK(a.allocLow() == want);
    CHECK(a.allocLow() == 0);  // full
    a.release(12);             // back to the low list
    CHECK(a.allocLow() == 12);
    a.release(5);
    CHECK(a.alloc() == 5);
    CHECK(a.allocLow() == 0);
    std::uint32_t n = 0;
    while (a.alloc() != 0) ++n;
    CHECK(n == 85);  // 15..99
}
