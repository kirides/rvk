#pragma once

// The object table's record (docs/research/frame-ownership.md 7.13, F2c-1; pure, tests `object_*`): the per-object registers of a NON-INSTANCED static
// mesh draw as the composition (StaticOps::subMeshDraws / meshZDraws: SetGlobalAlpha, SetWorldMatrix) writes them, indexed by the scene table's slot:
//   c31  (not meaningful for these draws: the engine keeps the actors' / trees' value there; a static draw whose vertex shader reads c31 is not served)
//   c32  .w = the draw's fade (z: 1, solid: the op's fade); .xyz = 0 (stale carry-over in the engine, never read by a static draw)
//   c33..c35 = columns 0..2 of the entity's world matrix (entity +0x60, row-vector convention): (m[k], m[4 + k], m[8 + k], m[12 + k]), exactly `writeWorld`
// VS register window 2 (c31..) is served from it: window base = record address - 31 x 16 (a 4 KiB pad in front of the table keeps D3D12's offset >= 0).

#include <cstddef>
#include <cstdint>
#include <cstring>

#include "gpuscene/window_compose.h"

namespace rvk::gpuscene {

inline constexpr std::uint32_t kObjectFirstReg = 31;
inline constexpr std::uint32_t kObjectRegs = 5;  // c31 .. c35

struct alignas(16) ObjectRecord {
    float regs[kObjectRegs][4];  // c31 .. c35
    std::uint32_t info[4];       // generation, version, flags (entity +0x28 at cache-in), live (1 = the slot holds an entity)
};
static_assert(sizeof(ObjectRecord) == 96);
static_assert(shader::abi::kVsWindowFirst[2] == kObjectFirstReg, "the object record starts at VS window 2");

// the registers c33..c35 writeWorld produces from an entity's matrix
inline void objectWorldRows(const float m[16], float rows[3][4]) {
    for (std::uint32_t k = 0; k < 3; ++k) {
        rows[k][0] = m[k];
        rows[k][1] = m[4 + k];
        rows[k][2] = m[8 + k];
        rows[k][3] = m[12 + k];
    }
}

// the record of a slot: world rows, fade (c32.w), info
inline ObjectRecord makeObjectRecord(const float world[16], float fade, std::uint32_t generation, std::uint32_t version, std::uint32_t flags) {
    ObjectRecord r{};
    r.regs[1][3] = fade;
    float rows[3][4];
    objectWorldRows(world, rows);
    std::memcpy(r.regs[2], rows, sizeof(rows));
    r.info[0] = generation;
    r.info[1] = version;
    r.info[2] = flags;
    r.info[3] = 1;
    return r;
}

// the record seen through VS window 2: biased base = record address - 31 x 16 (delta -496: inside the 4 KiB pad)
inline WindowImage objectWindow() {
    return windowOver(0, kObjectFirstReg, kObjectRegs, shader::abi::kVsWindowFirst[2], shader::abi::kVsWindowFirst[3] - shader::abi::kVsWindowFirst[2]);
}

// bit i = register (c31 + i) of the record that a draw's composition wrote (draw `vsSet` words): bits of c31..c35 out of the 256-bit mask
inline std::uint32_t objectComposedBits(const std::uint64_t vsSet[4]) {
    return static_cast<std::uint32_t>(vsSet[0] >> kObjectFirstReg) & ((1u << kObjectRegs) - 1u);  // (c31..c35 all lie in word 0)
}

// Why a draw's object window is not served (ObjectTable::serve)
enum class ObjectWhy : std::uint8_t {
    Served = 0,
    NotReady,       // the GPU table does not exist yet
    NoSlot,         // the entity has no scene-table slot (not cached in)
    SlotRange,      // slot >= table capacity
    C31Live,        // the vertex shader reads c31 (carry-over of the actors / trees): the old upload keeps it
    MatrixDiffers,  // the slot's matrix is not the one the draw was composed with (a missed signal)
    FadeConflict,   // another draw of this frame was served from the record with a different fade / matrix: the record cannot hold both
    Count
};

}  // namespace rvk::gpuscene
