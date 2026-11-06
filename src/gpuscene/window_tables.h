#pragma once

// Owned register windows (docs/research/frame-ownership.md 7.8, F1a; setting `native.owned_windows`, Debug, Full + native.material_table): the static
// composition's draws read their Material windows (PS window 2, VS window 3) from the Material table's records and their view windows (VS / PS window 0)
// from a per-view block, instead of per-draw ring uploads of the register file (d3d9::Device::setOwnedWindows).
//   * view blocks: a persistent GPU table of 272-byte records (c0..c11 of the VS + c0..c4 of the PS) written from the register file: a draw looks for a
//     record of this frame with the same 17 registers (the last 8 are searched) and writes a new one only when none matches. Slots are the frame's:
//     the cursor restarts at every present; a slot is re-uploaded only when its bytes differ from what the GPU holds.
//   * ordering: both tables are uploaded by `flush` at the present into the frame's upload list, which the GPU runs before the frame's draws, so a draw
//     sees the value the CPU shadow has at the present. Material records change only at their first draw of a frame (the poll) and view slots are written
//     once per frame, so that value is the one the draw was composed with; `debug.split_verify` compares every served window with the register file.

#include <bitset>
#include <cstdint>

#include "d3d9/owned_windows.h"
#include "gpuscene/illumination_table.h"
#include "gpuscene/window_compose.h"

namespace rvk::d3d9 {
class Device;
}

namespace rvk::gpuscene {

class WindowTables {
public:
    static constexpr std::uint32_t kViewCapacity = 512;  // view blocks per frame (measured: tens)

    // native.owned_windows effective (Full, the material table on, the setting on)
    void setActive(bool on) { active_ = on; }
    bool active() const { return active_; }

    // the view windows of a draw (VS / PS window 0) from the device's register file as it is now, into `ow`; false: no view table yet / full
    // `vs1` (optional, 19 registers c12..c30: the admin pool's values as the composition holds them): written into the block too and VS window 1 served
    // from it (F2c-1, native.owned_objects); null = the block's c12..c30 stay zero and window 1 is not offered
    bool fillViews(d3d9::Device& dev, d3d9::OwnedWindows& ow, const float (*vs1)[4] = nullptr, std::uint32_t vs1Composed = 0);
    // blocks of this frame that differ from another block only in c0..c3 (the depth-bias view-projection variant) / only in c12..c30 (per-draw variation of window 1)
    std::uint64_t dbvpBlocks() const { return viewDbvp_; }
    std::uint64_t vs1SplitBlocks() const { return viewVs1Split_; }

    // once per present (before the upload list is closed): uploads the frame's view blocks, restarts the cursor, the 5 s log
    void flush(d3d9::Device& dev);

private:
    void report(d3d9::Device& dev);

    bool active_ = false;
    GpuTable<ViewRecord> views_;
    bool gpuFailed_ = false, created_ = false;
    std::uint32_t cursor_ = 0;          // view blocks of this frame
    std::uint32_t lastSlot_ = 0;
    std::uint64_t frame_ = 0;
    std::uint64_t viewDbvp_ = 0, viewVs1Split_ = 0;
    std::uint64_t viewLookups_ = 0, viewReused_ = 0, viewNew_ = 0, viewFull_ = 0, viewSame_ = 0;  // totals
    std::uint64_t lastLookups_ = 0, lastReused_ = 0, lastNew_ = 0, lastFrame_ = 0, lastBytes_ = 0, lastSlots_ = 0;
    std::uint32_t peak_ = 0;
    d3d9::OwnedStats last_;
    double lastReport_ = 0.0;
    std::uint64_t lastMatDraws_ = 0;
};

WindowTables& windowtables();

}  // namespace rvk::gpuscene
