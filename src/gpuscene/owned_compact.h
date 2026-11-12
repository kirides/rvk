#pragma once

// F2c-4c: GPU compaction of the persistent owned draws (docs/research/frame-ownership.md 7.32; env RVK_F2C4C_CPU=1 = the CPU-written arguments of 7.21).
// A flush of the owned path (d3d9::Device::ownedFlush) used to write, per item, an indirect argument record (20 bytes) into a ring block. Now it writes only the ref
// list (slot, view slot) and one 16-byte job per bucket; the argument records are made ON THE GPU from the persistent DrawRecord table (DrawRecord::args) by
// shaders/gpuscene/owned_compact.comp.hlsl, which also writes each bucket's count: the packet is a DrawIndexedIndirectCount (Vulkan 1.2 drawIndirectCount,
// D3D12 ExecuteIndirect with a count buffer; without DeviceCaps::drawIndirectCount the CPU path stays).
//
// Ordering: the jobs of a CPU frame are queued while the frame is composed; at the present (after the record table's own upload) the dispatches are recorded into the
// UPLOAD list (one per flush, one barrier before and after), so every draw of the frame finds its arguments done, from the end-of-frame state of the records.
// One arguments + counts buffer per frame in flight (a device buffer: arguments / counts need UAV writes, the ring is an upload heap on D3D12), counts first.
// Check frames (gpuscene::verifyFrame): the GPU's arguments + counts are copied to a read-back buffer and compared with the arguments the CPU path would have
// written (counts, then every record): `DIFFERING` must stay 0.
// Nothing exists until a flush asks: the buffers / the pipeline are made on the first use.

#include <cstdint>
#include <memory>
#include <vector>

#include "rhi/game_device.h"
#include "rhi/rhi.h"

namespace rvk::d3d9 {
class Device;
}

namespace rvk::gpuscene {

class OwnedCompact {
public:
    static constexpr std::uint32_t kMaxItems = 16384;   // argument records per frame
    static constexpr std::uint32_t kMaxBuckets = 4096;  // counts per frame
    static constexpr std::uint32_t kArgsBase = kMaxBuckets * 4;

    OwnedCompact();
    ~OwnedCompact();

    // GPU compaction possible now: the cap, the buffers, the built pipeline (the first call starts making them: false until they are there). RVK_F2C4C_CPU=1 = never
    bool usable(d3d9::Device& dev);
    // a flush's range in this frame's buffer: false = the frame's capacity is spent (the flush takes the CPU path)
    struct Range {
        std::uint32_t firstArg = 0;    // argument index of the flush's first record
        std::uint32_t firstCount = 0;  // count index of the flush's first bucket
    };
    // `items` = argument records to reserve (every item one, a culled-capable one kRunCap)
    bool reserve(d3d9::Device& dev, std::uint32_t items, std::uint32_t buckets, Range& r);
    // F5-2 (check frames): an item the GPU may cull, with the CPU's exact verdict per cluster (no margin) to compare the runs the GPU wrote against
    struct CullItem {
        std::uint32_t pos = 0, bucket = 0;           // the entry's position in the flush's list (= the records' firstInstance), its bucket
        std::uint32_t firstIndex = 0, indexCount = 0, vertexOffset = 0;  // the record's own args (the whole range)
        std::uint32_t viewSlot = 0, rowArgs = 0, entryId = 0;  // what the evaluation needs (it runs at the present, from the end-of-frame state the GPU reads)
        std::uint64_t win2 = 0;
        bool exact = false;                          // the exact evaluation was possible (a regular world matrix)
        std::vector<std::uint32_t> clusterFirst, clusterCount;  // per cluster (copied: the rows may be freed by the time the read-back is collected)
        std::vector<std::uint8_t> visible;           // per cluster: the exact test says not outside
        float refRegs[16] = {}, refWorld[12] = {};   // diagnostics: the inputs of the exact evaluation
        std::vector<float> sphere;                   // diagnostics: per cluster centre xyz + radius
        std::vector<float> slack;                    // per cluster: the least over the planes of (distance / |n| + radius): how far inside (world units; < 0 = outside)
    };
    struct Job {
        rhi::GpuRef list, jobs;  // the flush's ref list block, its jobs block (32 bytes per bucket)
        std::uint32_t items = 0, buckets = 0;
        Range range;
        std::uint32_t viewKind = 2;         // statistics: 0 main z, 1 main solid, 2 other, 3 sun cascades
        std::uint32_t cullFlags = 0;        // bit 0: cull the clustered records (the tables below are set), bit 1: side planes only
        rhi::GpuRef views, clusters;        // the view table, the cluster table
        std::uint32_t viewSlots = 0;
        std::uint64_t fullIndices = 0;      // check frames: the indices of every item's whole range
        // check frames: per bucket (the bucket's first argument record relative to range.firstArg, the plain records expected), then 5 words per expected argument record of
        // the items that are not culled, in order (empty = not checked)
        std::vector<std::uint32_t> expect;
        std::vector<CullItem> culls;
    };
    void queue(d3d9::Device& dev, Job&& job);
    // the fields of a DrawIndexedIndirectCount packet over a bucket (`firstItem` = the bucket's first entry in the flush, `bucket` its index in the flush)
    void fillPacket(d3d9::Device& dev, const Range& r, std::uint32_t firstItem, std::uint32_t bucket, std::uint32_t count, rhi::DrawPacket& m);
    // once per present, after the record table's flush: the dispatches, the barriers, the check frame's copy; collects finished checks, the 5 s line
    void flush(d3d9::Device& dev);
    void destroy();

    struct Stats {
        std::uint64_t flushes = 0, items = 0, buckets = 0, dispatches = 0, frames = 0, cpuFull = 0, cpuNotReady = 0, argsMismatch = 0, peakItems = 0, peakBuckets = 0;  // (peaks: the most argument records / counts one frame reserved; reset per 5 s window)
        std::uint64_t checks = 0, checkedBuckets = 0, checkedRecords = 0, differing = 0, countDiffering = 0, skipped = 0;
        // F5-2: culled-capable items / jobs with the cull; check frames: items compared, clusters (all / the exact test's visible / covered by the GPU's runs), false negatives, extras,
        // whole-range fallbacks, items with no records, runs, bad run records, exact evaluations impossible; indices of the checked flushes per view kind before / after
        std::uint64_t cullItems = 0, cullJobs = 0, cullChecked = 0, cullClusters = 0, cullExactVisible = 0, cullCovered = 0, falseNegatives = 0, extras = 0, wholeRange = 0, emptyItems = 0,
                      runs = 0, badRuns = 0, inexact = 0;
        std::uint64_t idxBefore[4] = {}, idxAfter[4] = {}, idxFlushes[4] = {};
    };
    Stats& stats() { return stats_; }
    bool failed() const { return failed_; }
    // run-time A/B (overlay command `debug compact cpu|gpu`): true = the flushes write the arguments on the CPU as before F2c-4c
    void setForceCpu(bool on) { forceCpu_ = on; }
    // the check frames' period in frames (default 30; env RVK_F2C4C_CHECK=n, 0 = never, 1 = every frame; the test sets it): this frame's flushes carry the CPU's expected arguments
    void setCheckEvery(std::uint32_t n) { checkEvery_ = n; checkEverySet_ = true; }
    bool checkThisFrame(d3d9::Device& dev);
    // the GPU memory held (diagnostics)
    std::uint64_t bytes() const;

private:
    struct SlotState {
        std::unique_ptr<rhi::Buffer> buffer;    // counts + arguments (device, STORAGE | INDIRECT | TRANSFER_SRC)
        std::uint64_t handle = 0;               // the buffer's DrawPacket handle
        std::unique_ptr<rhi::Buffer> readback;  // check frames
        std::vector<Job> jobs;
        std::uint32_t items = 0, buckets = 0;   // the cursors of the frame being recorded
        std::uint64_t frameSeen = ~0ull;        // game.frameValue() the cursors belong to
        std::vector<Job> checks;                // the jobs whose `expect` waits for the read-back
        std::uint32_t checkItems = 0, checkBuckets = 0;
        std::uint64_t checkFrame = 0;           // the frame value the read-back belongs to (0 = none)
    };
    SlotState& slotFor(d3d9::Device& dev);
    bool ensureBuffers(d3d9::Device& dev, SlotState& s);
    void collect(d3d9::Device& dev);
    void evaluate(const Job& j, const CullItem& ci, const std::vector<std::pair<std::uint32_t, std::uint32_t>>& runs);
    void report();

    struct Pipeline;
    std::unique_ptr<Pipeline> pipeline_;
    SlotState slots_[rhi::kFramesInFlight];
    bool forceCpu_ = false;
    bool envRead_ = false, cpuOnly_ = false, failed_ = false, checkEverySet_ = false, checkEnv_ = false;
    std::uint32_t checkEvery_ = 30;
    Stats stats_{}, last_{};
    double lastReport_ = 0.0;
    std::uint64_t differingLogged_ = 0;
};

OwnedCompact& ownedcompact();

}  // namespace rvk::gpuscene
