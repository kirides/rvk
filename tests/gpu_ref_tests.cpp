// rhi::GpuRef / GpuAddr / RingAlloc (rhi/game_device.h): the shader-visible 64-bit reference of both backends.

#include <cstdint>

#include "rhi/game_device.h"
#include "test.h"

using namespace rvk;

namespace {

// the biased constant-window arithmetic of d3d9::Device::splitStage: windows[w] = base.plus(-first * 16); a shader reads register n at
// (the window's offset + n * 16) with 32-bit arithmetic on the offset word (D3D12) / 64-bit address arithmetic (Vulkan)
constexpr std::uint32_t kShiftVulkan = 0;
constexpr std::uint32_t kShiftD3D12 = 32;

// the byte offset a shader would compute for register `reg` of a window biased by `first` registers over a block at `blockOffset`
constexpr std::uint32_t registerOffset(std::uint32_t blockOffset, std::uint32_t first, std::uint32_t reg) {
    const rhi::GpuRef base = rhi::GpuRef::fromWords(7, blockOffset);  // D3D12 form: slot 7, offset in the high word
    const rhi::GpuRef window = base.plusAt(-static_cast<std::int64_t>(first) * 16, kShiftD3D12);
    return window.high() + reg * 16;  // 32-bit arithmetic on the offset word
}

}  // namespace

TEST(gpu_ref_words_and_pair) {
    const rhi::GpuRef r = rhi::GpuRef::fromWords(0x11223344u, 0x55667788u);
    CHECK(r.bits == 0x5566778811223344ull);
    CHECK(r.low() == 0x11223344u);
    CHECK(r.high() == 0x55667788u);
    CHECK(r.pair()[0] == 0x11223344u && r.pair()[1] == 0x55667788u);
    CHECK(static_cast<bool>(r));
    CHECK(!rhi::GpuRef{}.operator bool());
    CHECK(rhi::GpuRef{}.null());
    CHECK(rhi::GpuRef{5} == rhi::GpuRef::fromWords(5, 0));
}

TEST(gpu_ref_plus_vulkan_is_address_arithmetic) {
    // refShift 0: the reference is the device address
    const rhi::GpuRef a{0x0000'7fff'0000'1000ull};
    CHECK(a.plusAt(0x20, kShiftVulkan).bits == 0x0000'7fff'0000'1020ull);
    CHECK(a.plusAt(-0x1000, kShiftVulkan).bits == 0x0000'7fff'0000'0000ull);
    // the biased window: base - first * 16 (first = 256 registers) lands below the block and wraps like the old unsigned arithmetic
    const rhi::GpuRef w = a.plusAt(-256 * 16, kShiftVulkan);
    CHECK(w.bits == a.bits - 256ull * 16);
    CHECK(w.plusAt(256 * 16, kShiftVulkan) == a);
    // a delta below the base's low bits borrows from the high word as plain 64-bit arithmetic does
    const rhi::GpuRef b = rhi::GpuRef::fromWords(0x10, 0x2);
    CHECK(b.plusAt(-0x20, kShiftVulkan) == rhi::GpuRef::fromWords(0xFFFFFFF0u, 0x1));
}

TEST(gpu_ref_plus_d3d12_moves_the_offset_word_only) {
    // refShift 32: slot in the low word, byte offset in the high word
    const rhi::GpuRef r = rhi::GpuRef::fromWords(42, 1000);
    const rhi::GpuRef up = r.plusAt(24, kShiftD3D12);
    CHECK(up.low() == 42 && up.high() == 1024);
    const rhi::GpuRef down = r.plusAt(-24, kShiftD3D12);
    CHECK(down.low() == 42 && down.high() == 976);
    // the offset word wraps modulo 2^32 without a carry into the slot (and without touching bit 63 + 1)
    const rhi::GpuRef low = rhi::GpuRef::fromWords(42, 16);
    const rhi::GpuRef wrapped = low.plusAt(-256 * 16, kShiftD3D12);
    CHECK(wrapped.low() == 42);
    CHECK(wrapped.high() == 16u - 256u * 16u);  // 0xFFFFF010
    const rhi::GpuRef back = wrapped.plusAt(256 * 16, kShiftD3D12);
    CHECK(back == low);
    // a delta beyond 2^32 keeps only its low 32 bits (offsets are 32-bit on this backend)
    const rhi::GpuRef big = r.plusAt(static_cast<std::int64_t>(1) << 32, kShiftD3D12);
    CHECK(big.low() == 42 && big.high() == 1000);
    // the largest slot value is not disturbed by a negative delta
    const rhi::GpuRef top = rhi::GpuRef::fromWords(0xFFFFFFFFu, 0);
    CHECK(top.plusAt(-16, kShiftD3D12).low() == 0xFFFFFFFFu);
    CHECK(top.plusAt(-16, kShiftD3D12).high() == 0xFFFFFFF0u);
}

TEST(gpu_ref_biased_window_registers) {
    // c[first + i] of a window biased by `first` reads the i-th register of the block, whatever the block's offset: the VS windows start at 0 / 64 / 128 / 224
    // (shader/abi.h kVsWindowFirst), the PS windows at 0 / 5 / 16 / 32 (kPsWindowFirst); the block sits at a low offset, so base - first * 16 underflows
    for (const std::uint32_t first : {0u, 5u, 16u, 32u, 64u, 128u, 224u}) {
        for (const std::uint32_t blockOffset : {0u, 16u, 256u, 4096u, 0x1000000u}) {
            for (const std::uint32_t i : {0u, 1u, 7u, 100u}) {
                CHECK(registerOffset(blockOffset, first, first + i) == blockOffset + i * 16);
            }
        }
    }
}

TEST(gpu_ref_plus_uses_the_active_shift) {
    const std::uint32_t saved = rhi::refShift();
    const rhi::GpuRef r = rhi::GpuRef::fromWords(9, 64);
    rhi::setRefShift(kShiftVulkan);
    CHECK(r.plus(16) == r.plusAt(16, kShiftVulkan));
    CHECK(r.plus(16).bits == r.bits + 16);
    rhi::setRefShift(kShiftD3D12);
    CHECK(r.plus(16) == r.plusAt(16, kShiftD3D12));
    CHECK(r.plus(16) == rhi::GpuRef::fromWords(9, 80));
    rhi::setRefShift(saved);
}

TEST(gpu_addr_plus_moves_both_forms) {
    const std::uint32_t saved = rhi::refShift();
    rhi::setRefShift(kShiftD3D12);
    const rhi::GpuAddr a{rhi::GpuRef::fromWords(3, 128), 0x1'0000'0000'0000ull};
    const rhi::GpuAddr b = a.plus(48);
    CHECK(b.ref == rhi::GpuRef::fromWords(3, 176));
    CHECK(b.va == 0x1'0000'0000'0000ull + 48);
    CHECK(b.plus(-48) == a);
    CHECK(rhi::GpuAddr{}.null());
    CHECK(!a.null());
    rhi::setRefShift(saved);
}

TEST(ring_alloc_addr) {
    rhi::RingAlloc a;
    CHECK(a.addr().null());
    a.ref = rhi::GpuRef{0x1234};
    a.gpuVa = 0x5678;
    CHECK(a.addr().ref.bits == 0x1234 && a.addr().va == 0x5678);
}

TEST(shader_module_is_an_opaque_handle) {
    CHECK(rhi::ShaderModule{} == rhi::ShaderModule::None);
    const rhi::ShaderModule m = static_cast<rhi::ShaderModule>(0xABCDull);
    CHECK(m != rhi::ShaderModule::None);
    CHECK(static_cast<std::uint64_t>(m) == 0xABCDull);
}
