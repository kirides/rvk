#pragma once

// The GPU side of the RT scene's instance build (enh/rt_gpu_build.h, docs/research/frame-ownership.md 14.43): the three persistent tables with their slot
// pools (deferred release: a slot comes back kFramesInFlight + 1 frames after its release), the per-frame-slot output buffers (instances: the TLAS build input;
// records: the frame's RT scene records), the compute pipeline (built in the background on first use) and the check frames' read-back compare of the GPU output
// against the CPU path's bytes. Render thread (RtScene::record).

#include <cstdint>
#include <future>
#include <memory>
#include <string>
#include <vector>

#include "enh/rt_gpu_build.h"
#include "gpuscene/illumination_table.h"
#include "rhi/rhi.h"

namespace rvk::d3d9 {
class Device;
}

namespace rvk::enh::rtbuild {

// a slot allocator: free list + top, released slots wait `delay` frames
class SlotPool {
public:
    void reset(std::uint32_t capacity) {
        cap_ = capacity;
        top_ = live_ = 0;
        free_.clear();
        pending_.clear();
    }
    std::uint32_t alloc();
    void release(std::uint32_t slot, std::uint64_t frame);
    void tick(std::uint64_t frame);  // pending slots whose time came go back to the free list
    std::uint32_t top() const { return top_; }
    std::uint32_t live() const { return live_; }
    std::uint32_t capacity() const { return cap_; }
    std::uint64_t full = 0;

private:
    struct Pending {
        std::uint32_t slot;
        std::uint64_t at;
    };
    std::uint32_t cap_ = 0, top_ = 0, live_ = 0;
    std::vector<std::uint32_t> free_;
    std::vector<Pending> pending_;
};

struct BuildStats {
    std::uint64_t frames = 0, instances = 0, literals = 0, cpuFrames = 0, fallbackFrames = 0;
    std::uint64_t inputWrites = 0, blasWrites = 0, matWrites = 0, matEvals = 0, matChecked = 0, matStale = 0;
    std::uint64_t refusedFull = 0, uploadFailed = 0, notReady = 0;
    std::uint64_t compares = 0, comparedInstances = 0, differingInstances = 0, differingRecords = 0, compareSkipped = 0;
    double cpuMs = 0.0;        // the instance + record part of the RT pass, GPU arm (entries, tables, uploads, dispatch)
    double cpuMsCpuArm = 0.0;  // the same part, CPU arm
};

class GpuBuild {
public:
    static constexpr std::uint32_t kInputCap = 40960;
    static constexpr std::uint32_t kBlasCap = 32768;
    static constexpr std::uint32_t kMatCap = 16384;

    // tables, outputs and the pipeline (first call starts the pipeline build); true = usable this frame
    bool ready(d3d9::Device& dev, std::uint32_t tlasCapacity);
    // drops everything (GPU + slots); the caller forgets the slots it held
    void release();
    bool created() const { return created_; }

    SlotPool inputSlots, blasSlots, matSlots;
    gpuscene::GpuTable<InstanceInput> inputs;
    gpuscene::GpuTable<BlasRow> blas;
    gpuscene::GpuTable<MaterialRow> materials;

    // the three tables' uploads into `cl` (+ every kVerifyEvery-th frame their read-back); false = an upload failed (the GPU copy is not current)
    bool upload(d3d9::Device& dev, rhi::CommandList& cl, std::uint64_t frame);
    // the dispatch over `count` entries (header + entries + literals in the frame's ring) into the slot's outputs, then the barriers for the TLAS build and the
    // records' readers. False = not recorded.
    bool dispatch(d3d9::Device& dev, rhi::CommandList& cl, std::uint32_t slot, const Header& h);
    rhi::Buffer* outInstances(std::uint32_t slot) { return outInst_[slot].get(); }
    rhi::Buffer* outRecords(std::uint32_t slot) { return outRec_[slot].get(); }
    // check frames: the GPU output of `slot` copied back and compared with `ref` (the CPU path's bytes) once the frame completed
    void queueCompare(d3d9::Device& dev, rhi::CommandList& cl, std::uint32_t slot, std::vector<VkAccelerationStructureInstanceKHR>&& refInst,
                      std::vector<shader::abi::RtGeometryRecord>&& refRec);
    void collect(d3d9::Device& dev);

    BuildStats stats;
    // the 5 s line (`rt: gpu build (5 s)`), with the process totals for the stages line
    void log(double frames);
    std::uint64_t totalInstances = 0, totalDiffering = 0;

private:
    bool created_ = false, failed_ = false;
    std::future<std::unique_ptr<rhi::Pipeline>> pipeBuild_;
    std::unique_ptr<rhi::Pipeline> pipe_;
    bool pipeStarted_ = false, pipeFailed_ = false;
    std::unique_ptr<rhi::Buffer> outInst_[rhi::kFramesInFlight], outRec_[rhi::kFramesInFlight];
    std::uint32_t outCap_ = 0;
    struct Cmp {
        std::unique_ptr<rhi::Buffer> readback;
        std::vector<VkAccelerationStructureInstanceKHR> inst;
        std::vector<shader::abi::RtGeometryRecord> rec;
        std::uint64_t frameValue = 0;
        bool busy = false;
    } cmp_[rhi::kFramesInFlight];
    bool zeroed_ = false;
    std::uint64_t logged_ = 0;
    BuildStats last_;
};

}  // namespace rvk::enh::rtbuild
