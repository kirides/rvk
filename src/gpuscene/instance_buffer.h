#pragma once

// The GPU scene's instance table on the GPU (docs/research/gpu-scene.md 2.1, M1b): a persistent device-memory buffer with one 160-byte
// record per slot of engine/scene_table, updated in batches (one ring staging block + one copy per contiguous run) from the table's
// dirty / freed lists once per frame, on both backends (rhi::Device buffers; the shader address is a GpuRef: Vulkan device address,
// D3D12 heap slot + offset, `rvkPtr<RvkInstance, 16>(pair)` in HLSL: shaders/include/rvk_instance.hlsli). Verification
// (`native.gpu_scene` >= 2): every 240th frame the whole table is copied back and compared with the CPU copy that was uploaded.

#include <cstdint>
#include <memory>
#include <vector>

#include "rhi/game_device.h"

namespace rvk::d3d9 {
class Device;
}

namespace rvk::gpuscene {

// std430 / HLSL RvkInstance (shaders/include/rvk_instance.hlsli): keep in sync
struct InstanceRecord {
    float world[16];          // the entity's world matrix as the engine stores it (row-major, row vectors)
    float prev[16];           // the matrix before its last change
    std::uint32_t generation;
    std::uint32_t version;
    std::uint32_t flags;      // entity +0x28 at cache-in
    std::uint32_t live;       // 1 = cached in, 0 = free
    std::uint32_t movedFrame; // low 32 bits of the frame of the last change
    std::uint32_t pad[3];
};
static_assert(sizeof(InstanceRecord) == 160);

struct Stats {
    std::uint64_t flushes = 0, slotsUploaded = 0, runs = 0, bytes = 0, freed = 0;
    std::uint64_t verifies = 0, verifyDone = 0, verifySlots = 0, verifyBad = 0, overflow = 0;
    double flushMs = 0.0;
};

class InstanceBuffer {
public:
    static constexpr std::uint32_t kCapacity = 32768;  // slots (5 MiB)
    ~InstanceBuffer();
    // `mode`: 0 = off, 1 = upload, 2 = upload + read-back verification. Call once per frame on the engine thread, before the frame's draws.
    void flush(d3d9::Device& dev, int mode);
    bool ready() const { return buffer_ != nullptr; }
    // the table's shader reference (record i at + i * sizeof(InstanceRecord))
    rhi::GpuRef ref() const { return ref_; }
    const Stats& stats() const { return stats_; }
    void report();

private:
    bool create(d3d9::Device& dev);
    void verify(d3d9::Device& dev);

    std::unique_ptr<rhi::Buffer> buffer_;
    rhi::GpuRef ref_;
    std::vector<InstanceRecord> shadow_;  // what the GPU table holds (after the latest flush)
    struct Pending {
        std::unique_ptr<rhi::Buffer> buffer;
        std::uint64_t frameValue = 0;
        std::vector<InstanceRecord> expect;
        bool busy = false;
    };
    Pending pending_[rhi::kFramesInFlight];
    std::uint64_t frame_ = 0;
    Stats stats_;
    double lastReport_ = 0.0;
    bool failed_ = false;
};

InstanceBuffer& instances();

}  // namespace rvk::gpuscene
