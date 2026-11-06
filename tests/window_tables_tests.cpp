// The register images of the window override (gpuscene/window_compose.h; frame-ownership.md 7.8): window arithmetic on both backends' references.

#include <cstdint>
#include <cstring>

#include "gpuscene/object_compose.h"
#include "gpuscene/window_compose.h"
#include "rhi/game_device.h"
#include "test.h"

using namespace rvk;

namespace {
constexpr std::uint32_t kShiftVulkan = 0;
constexpr std::uint32_t kShiftD3D12 = 32;

gpuscene::MaterialRecord record(std::uint32_t vsFirst, std::uint32_t vsCount, std::uint32_t psFirst, std::uint32_t psCount) {
    gpuscene::MaterialRecord r{};
    r.info[0] = 1;
    r.counts[0] = vsFirst;
    r.counts[1] = vsCount;
    r.counts[2] = psFirst;
    r.counts[3] = psCount;
    return r;
}
}  // namespace

TEST(window_layout_constants) {
    CHECK(gpuscene::kMatVsRegsOffset == 80);
    CHECK(gpuscene::kMatPsRegsOffset == 80 + 32 * 16);
    CHECK(sizeof(gpuscene::ViewRecord) == 576);
    CHECK(offsetof(gpuscene::ViewRecord, vs1) == 17 * 16);
    CHECK(offsetof(gpuscene::ViewRecord, ps) == 12 * 16);
    // the pad covers the highest window start: VS window 3 at c232
    CHECK(gpuscene::kTablePad >= shader::abi::kVsWindowFirst[3] * 16);
}

TEST(window_material_images_cover_their_pools) {
    // the default class: VS pool c232.. (a material with 8 registers), PS pool c6..c13 (8)
    const auto r = record(232, 8, 6, 8);
    const gpuscene::WindowImage vs = gpuscene::materialVsWindow(r);
    CHECK(vs.covered == 8 && vs.imageIndex == 0);
    CHECK(vs.baseDelta == 80 - 232 * 16);
    const gpuscene::WindowImage ps = gpuscene::materialPsWindow(r);
    CHECK(ps.covered == 8 && ps.imageIndex == 0);
    CHECK(ps.baseDelta == 80 + 512 - 6 * 16);
    // a pool longer than the window: only the window's registers
    CHECK(gpuscene::materialVsWindow(record(232, 32, 6, 16)).covered == 24);
    CHECK(gpuscene::materialPsWindow(record(232, 32, 6, 16)).covered == 8);
    // a pool shorter than the shader's need: covered says how many
    CHECK(gpuscene::materialVsWindow(record(232, 3, 6, 4)).covered == 3);
    CHECK(gpuscene::materialPsWindow(record(232, 3, 6, 4)).covered == 4);
    // a pool that starts below the window's first register: the image index points into it
    const gpuscene::WindowImage low = gpuscene::materialPsWindow(record(232, 8, 4, 10));
    CHECK(low.covered == 8 && low.imageIndex == 2 && low.baseDelta == 80 + 512 - 4 * 16);
}

TEST(window_material_images_refuse_what_they_do_not_hold) {
    // the pool starts after the window's first register: c232 / c6 would read other bytes
    CHECK(gpuscene::materialVsWindow(record(233, 8, 6, 8)).covered == 0);
    CHECK(gpuscene::materialPsWindow(record(232, 8, 7, 7)).covered == 0);
    // an empty pool, a pool entirely below the window
    CHECK(gpuscene::materialVsWindow(record(232, 0, 6, 0)).covered == 0);
    CHECK(gpuscene::materialPsWindow(record(232, 8, 0, 4)).covered == 0);
    // a pool whose bias would reach below the pad
    CHECK(gpuscene::windowOver(80, 300, 4, 232, 24).covered == 0);
}

TEST(window_switch_masks_are_per_stage) {
    auto r = record(232, 8, 6, 8);
    CHECK(!gpuscene::materialVsSwitched(r) && !gpuscene::materialPsSwitched(r));
    r.sw[0] = 1u << 3;
    CHECK(gpuscene::materialVsSwitched(r) && !gpuscene::materialPsSwitched(r));
    r.sw[0] = 0;
    r.sw[1] = 1u << 2;
    CHECK(!gpuscene::materialVsSwitched(r) && gpuscene::materialPsSwitched(r));
    r.sw[1] = 1u << 18;  // a sampler slot, not a register
    CHECK(!gpuscene::materialPsSwitched(r));
}

TEST(window_register_addresses_vulkan) {
    // table at a device address, record 3: c235 of VS window 3 = the 4th pool register
    const std::uint64_t table = 0x0000'7fff'1000'0000ull + gpuscene::kTablePad;
    const rhi::GpuRef rec{table + 3ull * sizeof(gpuscene::MaterialRecord)};
    const auto r = record(232, 8, 6, 8);
    const gpuscene::WindowImage vs = gpuscene::materialVsWindow(r);
    const rhi::GpuRef base = rec.plusAt(vs.baseDelta, kShiftVulkan);
    CHECK(base.bits + 235ull * 16 == rec.bits + gpuscene::kMatVsRegsOffset + 3ull * 16);
    const gpuscene::WindowImage ps = gpuscene::materialPsWindow(r);
    const rhi::GpuRef pbase = rec.plusAt(ps.baseDelta, kShiftVulkan);
    CHECK(pbase.bits + 9ull * 16 == rec.bits + gpuscene::kMatPsRegsOffset + 3ull * 16);
}

TEST(window_register_addresses_d3d12_never_go_below_zero) {
    // D3D12: {heap slot, byte offset}; the table's ref points kTablePad bytes into the buffer, so record 0's biased VS base is still >= 0
    const rhi::GpuRef rec0 = rhi::GpuRef::fromWords(17, gpuscene::kTablePad);
    const auto r = record(232, 24, 6, 8);
    const gpuscene::WindowImage vs = gpuscene::materialVsWindow(r);
    const rhi::GpuRef base = rec0.plusAt(vs.baseDelta, kShiftD3D12);
    CHECK(base.low() == 17);
    CHECK(static_cast<std::int64_t>(base.high()) >= 0 && base.high() < 0x80000000u);
    CHECK(base.high() == gpuscene::kTablePad + 80 - 232 * 16);
    // c250 reads the 19th register; the 32-bit offset arithmetic of the shader
    CHECK(base.high() + 250u * 16 == gpuscene::kTablePad + gpuscene::kMatVsRegsOffset + 18u * 16);
    // the highest record keeps its offset word monotone (no wrap)
    const rhi::GpuRef recN = rhi::GpuRef::fromWords(17, gpuscene::kTablePad + 4095u * static_cast<std::uint32_t>(sizeof(gpuscene::MaterialRecord)));
    const rhi::GpuRef baseN = recN.plusAt(vs.baseDelta, kShiftD3D12);
    CHECK(baseN.high() + 232u * 16 == recN.high() + gpuscene::kMatVsRegsOffset);
    // the view block: window 0 starts at register 0, no bias; the PS part sits after the 12 VS registers
    const rhi::GpuRef view = rhi::GpuRef::fromWords(18, 272u * 5u);
    const rhi::GpuRef psBase = view.plusAt(static_cast<std::int64_t>(offsetof(gpuscene::ViewRecord, ps)), kShiftD3D12);
    CHECK(psBase.high() + 4u * 16 == 272u * 5u + 12u * 16 + 4u * 16);
}

// F2c-1: the object record (c31..c35) and its window
TEST(object_record_layout_and_rows) {
    CHECK(sizeof(gpuscene::ObjectRecord) == 96);
    CHECK(offsetof(gpuscene::ObjectRecord, info) == 5 * 16);
    float m[16];
    for (int i = 0; i < 16; ++i) m[i] = static_cast<float>(i + 1);
    const gpuscene::ObjectRecord r = gpuscene::makeObjectRecord(m, 0.75f, 3, 9, 0x200000);
    // c33 + k = column k of the row-vector matrix, exactly what StaticOps::writeWorld writes
    CHECK(r.regs[2][0] == m[0] && r.regs[2][1] == m[4] && r.regs[2][2] == m[8] && r.regs[2][3] == m[12]);
    CHECK(r.regs[3][0] == m[1] && r.regs[3][1] == m[5] && r.regs[3][2] == m[9] && r.regs[3][3] == m[13]);
    CHECK(r.regs[4][0] == m[2] && r.regs[4][1] == m[6] && r.regs[4][2] == m[10] && r.regs[4][3] == m[14]);
    CHECK(r.regs[1][3] == 0.75f && r.regs[1][0] == 0.0f && r.regs[0][0] == 0.0f);
    CHECK(r.info[0] == 3 && r.info[1] == 9 && r.info[2] == 0x200000 && r.info[3] == 1);
}

TEST(object_window_covers_c31_to_c35_inside_the_pad) {
    const gpuscene::WindowImage w = gpuscene::objectWindow();
    CHECK(w.covered == 5 && w.imageIndex == 0);
    CHECK(w.baseDelta == -31 * 16);
    CHECK(-w.baseDelta <= static_cast<std::int64_t>(gpuscene::kTablePad));
    // D3D12: record 0's biased offset (heap offset after the pad + delta) stays >= 0
    CHECK(static_cast<std::int64_t>(gpuscene::kTablePad) + w.baseDelta >= 0);
    // a shader reading c33..c35 (need 5 from c31) is covered, an instanced one (c32 + 4 i, windowEnd 256 -> need 225) is not
    CHECK(w.covered >= 36 - 31);
    CHECK(w.covered < 256 - 31);
}

TEST(object_composed_bits_pick_c31_to_c35) {
    std::uint64_t set[4] = {};
    set[0] = (1ull << 32) | (1ull << 33) | (1ull << 35) | (1ull << 20) | (1ull << 36);
    CHECK(gpuscene::objectComposedBits(set) == 0b10110u);  // c32, c33, c35 (c20 / c36 are outside)
    set[0] |= 1ull << 31;
    CHECK((gpuscene::objectComposedBits(set) & 1u) == 1u);
}
