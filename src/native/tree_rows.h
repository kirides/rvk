#pragma once

// The row layout of the owned SpeedTree passes (docs/research/frame-ownership.md 7.46). Pure constants and arithmetic, shared by the pass (static_ops_trees_owned.cpp), the
// ramp and rvk_d3d9test.
//
// The engine's tree vertex shaders read the instance's rows by relative addressing, c32[a0.x] with a0 = 4 x the instance-rate ramp value, and the translated shader's relative
// read (`rvk_cRel`) returns ZERO from register 256 on (the register file ends there; registers 232..255 are another window). The engine therefore draws at most 50 instances
// per call (rows c32 .. c231); an owned pass that laid ALL its rows out as c32 + 4 p read zeros for every instance p >= 56 and the material window's bytes for 50 .. 55: most
// trees of a pass vanished or collapsed. The rows are laid out in CHUNKS of 50 instances instead: chunk k = 32 registers (c0 .. c31: the c31 header) + 4 x 50 registers of rows;
// the records of the positions of chunk k carry window 2's base shifted by k chunks, and the ramp holds `position mod 50` (the local instance index).

#include <cstdint>

namespace rvk::native::tro {

constexpr std::uint32_t kChunkInstances = 50;                            // the engine's batch limit: rows c32 .. c231
constexpr std::uint32_t kChunkPrefixRegs = 32;                           // c0 .. c31 (only the c31 header is read)
constexpr std::uint32_t kRowRegs = 4;                                    // c32 .. c35 per instance
constexpr std::uint32_t kChunkRegs = kChunkPrefixRegs + kRowRegs * kChunkInstances;  // 232 registers per chunk
constexpr std::uint32_t kChunkBytes = kChunkRegs * 16;

constexpr std::uint32_t chunkCount(std::uint32_t positions) { return (positions + kChunkInstances - 1) / kChunkInstances; }
constexpr std::uint32_t chunkOf(std::uint32_t position) { return position / kChunkInstances; }
// the byte offset of position p's row (c32 + 4 j of its chunk) in the rows block
constexpr std::uint64_t rowOffset(std::uint32_t position) {
    return static_cast<std::uint64_t>(chunkOf(position)) * kChunkBytes + static_cast<std::uint64_t>(kChunkPrefixRegs + kRowRegs * (position % kChunkInstances)) * 16;
}
// window 2's base of chunk k relative to the block's start (c[n] = base + 16 n: n = 31 is the chunk's header, n = 32 + 4 j its row j)
constexpr std::uint64_t chunkBase(std::uint32_t chunk) { return static_cast<std::uint64_t>(chunk) * kChunkBytes; }
// the identity ramp's element p (stream 1, instance rate): the local instance index
constexpr float rampValue(std::uint32_t position) { return static_cast<float>(position % kChunkInstances); }

}  // namespace rvk::native::tro
