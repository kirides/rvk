#pragma once

// The persistent DrawRecord / DrawBlock tables of F2c-4a (docs/research/frame-ownership.md 7.21 / 11, setting native.owned_persistent): one slot = one owned
// static draw. A slot holds the draw's scene record (shader/abi.h DrawRecord: the material / object / lighting window bases, the DrawBlock address, flags, the
// c5.w bits) and its DrawBlock (textures, samplers, alpha test, pack word; the v2 shaders read posFix / c5.xyz from the view block, so those bytes are zero).
// A slot is written when its draw is first owned or when one of its derivation inputs changed (the caller decides: native/static_ops_owned.cpp), and stays;
// the CPU shadow decides what is uploaded (only the changed slots, at the present, into the frame's upload list: the GpuTable recipe of the other tables).
//
// Slot reuse is deferred: a released slot becomes allocatable again kFramesInFlight + 1 presents later, so a draw recorded in an earlier frame that still
// reads the old content is never overtaken by a new owner's upload. A full table is a refusal (`alloc` = kNoSlot, counted): the caller composes the op
// the old way. Singleton, leaked on purpose (op records release their slots from destructors at any time, also at shutdown).

#include <cstdint>
#include <deque>
#include <memory>
#include <vector>

#include "gpuscene/illumination_table.h"
#include "shader/abi.h"

namespace rvk::d3d9 {
class Device;
}

namespace rvk::gpuscene {

// a zeroed block the windows a shader does not read point at (biased bases reach 232 x 16 bytes below it: the table keeps a pad in front, D3D12 offsets stay >= 0)
struct ZeroBlock {
    std::uint8_t b[4096];
};

class DrawRecordTable {
public:
    static constexpr std::uint32_t kCapacity = 16384;  // slots: 2 MiB of records + 3.3 MiB of blocks (F-INT, 7.43: 4,096 filled while turning with everything owned)

    void setActive(bool on) { active_ = on; }
    bool active() const { return active_; }
    // creates the two buffers (once; false = they could not be created: the persistent path stays off)
    bool ensure(d3d9::Device& dev);
    bool ready() const { return records_ && records_->ready() && blocks_->ready() && zero_->ready(); }
    // the buffers (tests: before the device goes; the game's tables live as long as the process like the other persistent tables)
    void destroy();
    // the zero block's address (a window base = this - first register x 16)
    rhi::GpuRef zeroRef() const { return zero_ ? zero_->ref() : rhi::GpuRef{}; }
    rhi::GpuRef recordsRef() const { return records_ ? records_->ref() : rhi::GpuRef{}; }
    rhi::GpuRef blocksRef() const { return blocks_ ? blocks_->ref() : rhi::GpuRef{}; }
    // S-4 (native.vertex_pull, frame-ownership.md 14.12): the VertexFetch table parallel to the records (slot i at + i * kVertexFetchSize), created on the first
    // ensureFetch (only while pulling is on: no table nobody reads); null / zero until then
    bool ensureFetch(d3d9::Device& dev);
    rhi::GpuRef fetchRef() const { return fetch_ && fetch_->ready() ? fetch_->ref() : rhi::GpuRef{}; }
    const shader::abi::VertexFetch* fetchShadow() const { return fetch_ && fetch_->ready() ? fetch_->shadow().data() : nullptr; }
    // the DrawBlock address a record of `slot` holds
    rhi::GpuRef blockRef(std::uint32_t slot) const { return blocks_->ref().plus(static_cast<std::int64_t>(slot) * static_cast<std::int64_t>(sizeof(shader::abi::DrawBlock))); }

    // a free slot (kNoSlot: none left, counted)
    std::uint32_t alloc();
    // the slot goes back after the deferral (idempotent per slot is the caller's duty: a slot is released once)
    void release(std::uint32_t slot);
    // writes the slot's record + block (+ its vertex fetch block: null = zeros; ignored while the fetch table does not exist); true = the bytes differ from what the
    // slot held (marked for upload)
    bool write(std::uint32_t slot, const shader::abi::DrawRecord& rec, const shader::abi::DrawBlock& blk, const shader::abi::VertexFetch* fetch = nullptr);
    // the slot holds exactly these bytes (record, block, and the fetch block where the table exists; null = zeros)
    bool holds(std::uint32_t slot, const shader::abi::DrawRecord& rec, const shader::abi::DrawBlock& blk, const shader::abi::VertexFetch* fetch = nullptr) const;
    // the CPU twins of a slot (what the GPU reads once uploaded)
    const shader::abi::DrawRecord& record(std::uint32_t slot) const { return records_->shadow()[slot]; }
    const shader::abi::DrawRecord* recordsShadow() const { return records_ ? records_->shadow().data() : nullptr; }
    const shader::abi::DrawBlock& block(std::uint32_t slot) const { return blocks_->shadow()[slot]; }
    bool live(std::uint32_t slot) const { return slot < kCapacity && slot < liveFlags_.size() && liveFlags_[slot] != 0; }

    // once per present (before the upload list is closed): the deferred releases, the uploads, the read-back check, the 5 s log
    void flush(d3d9::Device& dev);

    struct Stats {
        std::uint64_t allocs = 0, releases = 0, full = 0, writes = 0, same = 0, reused = 0;
    };
    const Stats& stats() const { return stats_; }
    std::uint32_t liveCount() const { return liveCount_; }
    std::uint32_t peak() const { return peak_; }
    TableStats recordStats() const { return records_ ? records_->stats() : TableStats{}; }
    TableStats blockStats() const { return blocks_ ? blocks_->stats() : TableStats{}; }

private:
    bool active_ = false, created_ = false, gpuFailed_ = false;
    std::unique_ptr<GpuTable<shader::abi::DrawRecord>> records_;
    std::unique_ptr<GpuTable<shader::abi::DrawBlock>> blocks_;
    std::unique_ptr<GpuTable<ZeroBlock>> zero_;
    std::unique_ptr<GpuTable<shader::abi::VertexFetch>> fetch_;
    bool fetchFailed_ = false;
    std::vector<std::uint8_t> liveFlags_;
    std::vector<std::uint32_t> free_;
    struct Pending {
        std::uint32_t slot;
        std::uint64_t dueFrame;
    };
    std::deque<Pending> pending_;
    std::uint32_t used_ = 0, liveCount_ = 0, peak_ = 0, windowPeak_ = 0;
    std::uint64_t frame_ = 0;
    Stats stats_{}, last_{};
    double lastReport_ = 0.0;
};

DrawRecordTable& drawrecords();

}  // namespace rvk::gpuscene
