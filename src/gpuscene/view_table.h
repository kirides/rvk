#pragma once

// The view table of scene ABI v2 (docs/research/frame-ownership.md 7.19 / 11, F2c-4-0): a persistent GPU table of ViewBlock2 (640 bytes, shader/abi.h) with FIXED
// slots (0 main z pass, 1 main solid pass, 2-3 the depth-bias view-projection variants, 4-7 reserved for the sun cascades, then dynamic slots from 8). A slot is
// written once per view and frame from the view's registers (VS c0..c30, PS c0..c4, posFix with the jitter and the depth remap, PS c5.xyz); the shadow copy
// decides what is uploaded, so an unchanged view costs no upload at all. The table is uploaded by `flush` at the present into the frame's upload list (the
// GPU runs it before the frame's draws), so a slot must hold ONE content for the whole frame: a second `set` of a slot with different bytes in the same frame is
// refused (the caller composes its draws the old way) and counted.
//   A v2 draw reads its slot through the ref stream's y (ScenePush::views + slot * 640); `fillWindows` builds the OwnedWindows entries (VS 0 / 1, PS 0) that
//   point into a slot, which the device checks against the table's own bases (Device::replayOwned) and the debug.split_verify machinery compares with the
//   register file.

#include <array>
#include <cstdint>

#include "d3d9/owned_windows.h"
#include "gpuscene/illumination_table.h"
#include "shader/abi.h"

namespace rvk::d3d9 {
class Device;
}

namespace rvk::gpuscene {

class ViewTable {
public:
    static constexpr std::uint32_t kCapacity = 64;  // slots (40 KiB); fixed 0..7, dynamic 8..63

    // the registers of one view (the owner's copies): VS c0..c11 + c12..c30, PS c0..c4, posFix, pixel c5 (xyz)
    struct Regs {
        const float (*vs0)[4] = nullptr;  // 12
        const float (*vs1)[4] = nullptr;  // 19
        const float (*ps0)[4] = nullptr;  // 5
        const float* posFix = nullptr;    // 4
        const float* c5 = nullptr;        // 3 or 4 (xyz used)
    };

    void setActive(bool on) { active_ = on; }
    bool active() const { return active_; }
    bool ready() const { return table_.ready(); }
    // creates the buffer (once; false = it could not be created: v2 stays off) and tells the device the table's base
    bool ensure(d3d9::Device& dev);
    rhi::GpuRef ref() const { return table_.ref(); }
    const shader::abi::ViewBlock2* shadow() const { return table_.shadow().data(); }

    // writes `slot` for this frame; false = refused (not ready, a slot out of range, or this frame's content of the slot differs: counted in conflicts())
    bool set(std::uint32_t slot, const Regs& r);
    // the first dynamic slot of this frame holding exactly these registers, or a new one; kNoSlot = none left / not ready
    std::uint32_t acquire(const Regs& r);
    // the windows (VS 0 / 1, PS 0) of `slot` as OwnedWindows entries (kind View, bases = the slot's block + offset - first x 16, images = the table's CPU twin)
    bool fillWindows(std::uint32_t slot, d3d9::OwnedWindows& ow) const;
    // the base of window `w` of the stage as the shader computes it (views + slot * 640 + offset - first * 16): the device's consistency check
    static std::uint64_t windowBase(rhi::GpuRef table, std::uint32_t slot, bool vertex, std::uint32_t window);

    // once per present (before the upload list is closed): uploads the changed slots, restarts the frame's bookkeeping, the 5 s log
    void flush(d3d9::Device& dev);

    struct Stats {
        std::uint64_t sets = 0, same = 0, written = 0, conflicts = 0, dynamic = 0, dynamicReused = 0, dynamicFull = 0;
    };
    const Stats& stats() const { return stats_; }

private:
    void build(shader::abi::ViewBlock2& b, const Regs& r) const;
    bool active_ = false, gpuFailed_ = false, created_ = false;
    GpuTable<shader::abi::ViewBlock2> table_;
    std::array<std::uint64_t, kCapacity> setFrame_{};  // the frame (1-based) a slot was last set in
    std::uint64_t frame_ = 1;
    std::uint32_t dynCursor_ = shader::abi::kViewSlotDynamicFirst;
    Stats stats_{};
    Stats last_{};
    std::uint64_t lastFrame_ = 1;
    double lastReport_ = 0.0;
};

ViewTable& viewtable();

}  // namespace rvk::gpuscene
