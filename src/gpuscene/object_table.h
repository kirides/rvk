#pragma once

// The object table (docs/research/frame-ownership.md 7.13, F2c-1; setting `native.owned_objects`, Debug, Full + native.owned_windows + native.material_table):
// one 96-byte record (c31..c35 + info, object_compose.h) per scene-table slot in a persistent device buffer, from which the static ops' non-instanced mesh
// draws read VS register window 2 instead of a per-draw ring upload (d3d9::Device::setOwnedWindows; StaticOps::setOwnedFor).
//   * Authority: `serve` (called per draw, composition already done): the slot's signal-fed matrix must equal the one the draw was composed with (else the
//     draw keeps its upload: `MatrixDiffers`), the record is updated NOW in the CPU shadow (so debug.split_verify's CPU twin is exactly what the present
//     uploads) and marked; a record that an earlier draw of THIS frame was served from can no longer change (the GPU sees one value per frame): a draw with
//     another fade / matrix keeps its upload (`FadeConflict`). Rewrites of an existing record's fade count as `fade rewrites`.
//   * Drain (`flush`, once per present): every dirty slot of the scene table (the matrix signal) whose record exists is re-composed from the slot's matrix;
//     a slot served this frame whose matrix changed AFTER the serve (version differs) counts `changed after serve` (a draw read a stale matrix: must stay 0).
//     Slots are not freed on cache-out (a reused slot's draws rewrite the record; an unserved record is never read).
//   * Ordering as the other tables: the present flush uploads the CPU shadow into the frame's UPLOAD list, which runs before all of the frame's draws.
//   * Verification: GPU read-back every 240th frame (0 differing), debug.split_verify (the image's CPU twin vs the register file at the composed registers),
//     the per-draw matrix check (slot matrix vs the draw's composed rows), the 5 s log `native: owned objects (5 s)`.

#include <cstdint>
#include <vector>

#include "d3d9/owned_windows.h"
#include "gpuscene/illumination_table.h"
#include "gpuscene/object_compose.h"

namespace rvk::d3d9 {
class Device;
}

namespace rvk::gpuscene {

struct ObjectStats {
    std::uint64_t requests = 0, served = 0, fadeRewrites = 0, matrixRewrites = 0, newRecords = 0, fast = 0;
    std::uint64_t why[static_cast<std::size_t>(ObjectWhy::Count)] = {};
    std::uint64_t matrixChecks = 0, matrixBad = 0, changedAfterServe = 0, drained = 0;
    std::uint64_t instanced = 0, vs1Offered = 0, vs1Differs = 0, notCandidate = 0;  // StaticOps::setOwnedFor's own counts
};

class ObjectTable {
public:
    static constexpr std::uint32_t kCapacity = 32768;  // scene-table slots (InstanceBuffer::kCapacity; 3 MiB)

    void setActive(bool on) { active_ = on; }
    bool active() const { return active_; }

    // One draw: the entity's record for the draw whose VS registers c31..c35 were composed as `composed[5][4]` (bit i of `composedBits` = c(31 + i) was composed;
    // c32 .w = the fade). Fills `out` (kind Object, base, covered, masks, CPU twin) and returns Served, else the reason (out untouched).
    ObjectWhy serve(const void* entity, const float composed[kObjectRegs][4], std::uint32_t composedBits, d3d9::OwnedWindows::Win& out);
    // 7.27, off the check frames: the slot's record is served WITHOUT the per-draw matrix check / record rebuild when it is current by the scene table's own
    // signals (generation, matrix version, flags equal the record's) and the fade (`fade`, used when c32 is composed: bit 1 of composedBits) is the record's;
    // false = not applicable, the caller runs `serve`. The check frames run `serve` for every draw, which compares the signal-fed matrix with the entity's.
    bool serveFast(const void* entity, std::uint32_t composedBits, float fade, d3d9::OwnedWindows::Win& out);

    // F2c-2: the window of slot `slot`'s record without serving anything (a pure function of the slot: kind Object, base, covered); false: not ready / out of range
    bool windowFor(std::uint32_t slot, d3d9::OwnedWindows::Win& out) const;

    // once per present (engine thread, before the frame's draws are submitted): the scene table's drain, upload, read-back, 5 s log
    void flush(d3d9::Device& dev);

    bool ready() const { return table_.ready(); }
    ObjectStats& mutableStats() { return stats_; }  // (StaticOps::setOwnedFor counts its pre-checks: instanced draws, window 1 offers / composed-differs, non-candidates)
    const ObjectStats& stats() const { return stats_; }
    const TableStats& tableStats() const { return table_.stats(); }

private:
    void ensureCpu();
    void report(d3d9::Device& dev);

    bool active_ = false;
    GpuTable<ObjectRecord> table_;
    bool cpuReady_ = false, created_ = false, gpuFailed_ = false;
    std::vector<std::uint64_t> servedFrame_;     // per slot: the frame the record was last served in (frame_ + 1; 0 = never)
    std::vector<std::uint32_t> servedVersion_;   // per slot: the scene-table slot's matrix version at that serve
    std::vector<float> servedFade_;              // (the record's c32.w at that serve)
    std::uint32_t used_ = 0;                     // highest slot written + 1
    std::uint64_t frame_ = 0;
    ObjectStats stats_, last_;
    d3d9::OwnedStats lastDev_;
    std::uint64_t lastFrame_ = 0, lastBytes_ = 0, lastSlots_ = 0;
    double lastReport_ = 0.0;
    int changedLogged_ = 0;
};

ObjectTable& objecttable();

}  // namespace rvk::gpuscene
