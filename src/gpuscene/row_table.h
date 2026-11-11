#pragma once

// The instance row table (docs/research/frame-ownership.md 14.19, F2f; setting `native.owned_instances`, Debug, Full + native.owned_persistent): the VS register window 2
// images (c31..c35, the object record's layout: object_compose.h) of the instance-list draws of a mesh resource (render-op node 0xffffff: one draw per render instance, the
// instance's own 0x58-byte record at `res +0xb0` carries matrix / switch / illumination / flags, so the object table, one record per scene-table entity, cannot serve them).
// One image per owned draw, written ONCE when the op record is built from a composition (the composed registers c32 .w / c33..c35 of that draw) and immutable while the record
// lives: the record's stamp holds every instance's matrix, so a moved instance remakes the record (and with it the images). Released with the draw record; a released slot is
// handed out again only after kFramesInFlight + 1 presents (frames in flight still read it).
//   * `flush` (once per present, with the other tables: before the frame's draws) uploads the new images into the frame's upload list, a GPU read-back compares the used range
//     every 240th frame (0 differing), the 5 s log `native: instance rows (5 s)`.
//   * The window: kind Object (the device counts it like an object record: same layout, c31 never served), base = image - 31 x 16 (kTablePad in front: D3D12 offset >= 0).

#include <cstdint>
#include <vector>

#include "d3d9/owned_windows.h"
#include "gpuscene/illumination_table.h"
#include "gpuscene/object_compose.h"

namespace rvk::d3d9 {
class Device;
}

namespace rvk::gpuscene {

struct RowStats {
    std::uint64_t allocs = 0, releases = 0, full = 0, c31Live = 0;
};

class RowTable {
public:
    static constexpr std::uint32_t kCapacity = 16384;  // images (1.5 MiB)

    void setActive(bool on) { active_ = on; }
    bool active() const { return active_; }
    // a new image := c31..c35 as composed (`composedBits` bit i = c(31 + i) composed; c31 must not be: a shader reading it keeps its upload): the slot, kNoSlot = not ready /
    // full / c31 read. The image's registers that were not composed are zero (never read by the draw: they would be carry-over of the register file)
    std::uint32_t alloc(const float regs[kObjectRegs][4], std::uint32_t composedBits);
    // the slot goes back (deferred by kFramesInFlight + 1 presents)
    void release(std::uint32_t slot);
    // the VS window 2 of `slot` (kind Object, covered 5, CPU twin, the split_verify masks of `composedBits`); false = not ready / not a live slot
    bool windowFor(std::uint32_t slot, std::uint32_t composedBits, d3d9::OwnedWindows::Win& out) const;
    // the image of `slot` equals `regs` in the composed registers (c32 .w only)
    bool same(std::uint32_t slot, const float regs[kObjectRegs][4], std::uint32_t composedBits) const;
    // F5-2 (check frames): the world rows c33..c35 of the image a window base points at; false = not one of this table's bases
    bool worldRowsAt(std::uint64_t baseBits, float world[12]) const;

    void flush(d3d9::Device& dev);  // once per present (engine thread)
    void releaseGpu();
    bool ready() const { return table_.ready(); }
    std::uint32_t live() const { return live_; }
    const RowStats& stats() const { return stats_; }

private:
    bool active_ = false, created_ = false, failed_ = false;
    GpuTable<ObjectRecord> table_;
    std::vector<std::uint8_t> used_;      // per slot: handed out
    std::vector<std::uint32_t> free_;
    struct Pending {
        std::uint32_t slot;
        std::uint64_t at;
    };
    std::vector<Pending> pending_;
    std::uint32_t top_ = 0, live_ = 0, peak_ = 0;
    std::uint64_t frame_ = 0;
    RowStats stats_, last_;
    double lastReport_ = 0.0;
};

RowTable& rowtable();

// RAII: an owned draw's image (released when the draw record goes away)
struct RowHandle {
    std::uint32_t slot = kNoSlot;
    std::uint32_t bits = 0;  // the composed registers of the image
    RowHandle() = default;
    RowHandle(const RowHandle&) = delete;
    RowHandle& operator=(const RowHandle&) = delete;
    RowHandle(RowHandle&& o) noexcept : slot(o.slot), bits(o.bits) { o.slot = kNoSlot; }
    RowHandle& operator=(RowHandle&& o) noexcept {
        if (this != &o) {
            reset();
            slot = o.slot;
            bits = o.bits;
            o.slot = kNoSlot;
        }
        return *this;
    }
    ~RowHandle() { reset(); }
    void reset() {
        if (slot != kNoSlot) rowtable().release(slot);
        slot = kNoSlot;
    }
    bool has() const { return slot != kNoSlot; }
};

}  // namespace rvk::gpuscene
