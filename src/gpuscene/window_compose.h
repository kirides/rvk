#pragma once

// Register images for the window override (frame-ownership.md 7.8, F1a; pure, tests `window_*`): where a persistent record's registers sit relative to
// the record and which of a register window's registers an image covers. A window base is "image address - imageFirst * 16", so `c[n]` of the
// shaders (RVK_VSC / RVK_PSC: window base + n * 16) is the record's register n - imageFirst.
//   Vulkan: a GpuRef is the device address, base arithmetic is plain 64-bit.
//   D3D12: a GpuRef is {heap slot, byte offset}; the offset word wraps modulo 2^32 (rhi::GpuRef::plus), which the shaders' 32-bit offset arithmetic
//          undoes, but the tables start with kTablePad unused bytes (GpuTable::create padBytes) so the biased offset never goes below 0 at all.

#include <algorithm>
#include <cstddef>
#include <cstdint>

#include "gpuscene/material_compose.h"
#include "shader/abi.h"

namespace rvk::gpuscene {

inline constexpr std::uint32_t kTablePad = 4096;  // >= first register of any window x 16 (VS window 3: 232 x 16 = 3,712)

// the per-view block: the VS registers c0..c11 (window 0) + the PS registers c0..c4 (window 0), written from the register file
struct alignas(16) ViewRecord {
    float vs[12][4];
    float ps[5][4];
};
static_assert(sizeof(ViewRecord) == 272);
static_assert(shader::abi::kVsWindowFirst[1] == 12 && shader::abi::kPsWindowFirst[1] == 5, "ViewRecord holds exactly windows 0");

// an image's use for one window
struct WindowImage {
    std::int64_t baseDelta = 0;    // bytes from the record's address to the biased base (negative for images whose first register is high)
    std::uint32_t covered = 0;     // registers from the window's first one the image holds (<= the window's size), 0 = none
    std::uint32_t imageIndex = 0;  // index in the image of the window's first register (valid when covered > 0)
};

// An image of `count` registers starting at register `imageFirst`, stored at byte `regsOffset` of its record, seen through the window [first, first + size).
// Not usable (covered 0) when the image starts after the window's first register (the registers below it would read other records' bytes) or when
// the biased base would lie more than the table's pad below the record start.
inline WindowImage windowOver(std::uint32_t regsOffset, std::uint32_t imageFirst, std::uint32_t count, std::uint32_t first, std::uint32_t size) {
    WindowImage w;
    const std::int64_t delta = static_cast<std::int64_t>(regsOffset) - static_cast<std::int64_t>(imageFirst) * 16;
    if (imageFirst > first || count == 0) return w;
    if (delta < -static_cast<std::int64_t>(kTablePad)) return w;
    const std::uint32_t end = imageFirst + count;
    if (end <= first) return w;
    w.covered = std::min(end - first, size);
    w.baseDelta = delta;
    w.imageIndex = first - imageFirst;
    return w;
}

inline constexpr std::uint32_t kMatVsRegsOffset = static_cast<std::uint32_t>(offsetof(MaterialRecord, vs));
inline constexpr std::uint32_t kMatPsRegsOffset = static_cast<std::uint32_t>(offsetof(MaterialRecord, ps));

// the Material record's VS pool seen through VS window 3 (c232..) / PS pool through PS window 2 (c6..c13)
inline WindowImage materialVsWindow(const MaterialRecord& r) {
    return windowOver(kMatVsRegsOffset, r.counts[0], r.counts[1], shader::abi::kVsWindowFirst[3], shader::abi::kVsWindowFirst[4] - shader::abi::kVsWindowFirst[3]);
}
inline WindowImage materialPsWindow(const MaterialRecord& r) {
    return windowOver(kMatPsRegsOffset, r.counts[2], r.counts[3], shader::abi::kPsWindowFirst[2], shader::abi::kPsWindowFirst[3] - shader::abi::kPsWindowFirst[2]);
}

// whether the record's registers of a stage are history dependent (a SetSwitch element / the sky wrote the pool natively; the engine's memory, which the
// record is read from, does not hold them: those draws keep the normal upload)
inline bool materialVsSwitched(const MaterialRecord& r) { return r.sw[0] != 0; }
inline bool materialPsSwitched(const MaterialRecord& r) { return (r.sw[1] & 0xffffu) != 0; }

}  // namespace rvk::gpuscene
