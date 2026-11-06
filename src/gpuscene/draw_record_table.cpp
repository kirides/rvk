#include "gpuscene/draw_record_table.h"

#include <algorithm>
#include <chrono>
#include <cstring>

#include <spdlog/spdlog.h>

#include "d3d9/device.h"

namespace rvk::gpuscene {

namespace {
constexpr std::uint64_t kVerifyEvery = 240;
}

void DrawRecordTable::destroy() {
    records_.reset();
    blocks_.reset();
    zero_.reset();
    created_ = false;
    gpuFailed_ = false;
    used_ = liveCount_ = peak_ = 0;
    free_.clear();
    pending_.clear();
    liveFlags_.assign(kCapacity, 0);
}

DrawRecordTable& drawrecords() {
    static DrawRecordTable* t = new DrawRecordTable();  // (leaked: op records release their slots from destructors, also at shutdown)
    return *t;
}

bool DrawRecordTable::ensure(d3d9::Device& dev) {
    if (ready()) return true;
    if (gpuFailed_ || dev.rhi() == nullptr) return false;
    if (!created_) {
        created_ = true;
        records_ = std::make_unique<GpuTable<shader::abi::DrawRecord>>();
        blocks_ = std::make_unique<GpuTable<shader::abi::DrawBlock>>();
        zero_ = std::make_unique<GpuTable<ZeroBlock>>();
        records_->init(kCapacity);
        blocks_->init(kCapacity);
        liveFlags_.assign(kCapacity, 0);
        zero_->init(1);
        if (!records_->create(dev, kCapacity, "draw records") || !blocks_->create(dev, kCapacity, "draw blocks") || !zero_->create(dev, 1, "draw zero block", sizeof(ZeroBlock))) {
            gpuFailed_ = true;
            spdlog::warn("native: draw record table: the buffers could not be created (the persistent owned path stays off)");
            return false;
        }
        zero_->mark(0);  // (zeros: the device memory is not cleared)
    }
    return ready();
}

std::uint32_t DrawRecordTable::alloc() {
    if (!records_ || !blocks_) return kNoSlot;
    std::uint32_t s;
    if (!free_.empty()) {
        s = free_.back();
        free_.pop_back();
        ++stats_.reused;
    } else if (used_ < kCapacity) {
        s = used_++;
    } else {
        ++stats_.full;
        return kNoSlot;
    }
    liveFlags_[s] = 1;
    // (a new owner starts from zeros, uploaded: the GPU never holds the previous owner's record, an unwritten slot is recognisably empty, the read-back check compares like with like)
    records_->shadow()[s] = {};
    blocks_->shadow()[s] = {};
    records_->mark(s);
    blocks_->mark(s);
    ++liveCount_;
    peak_ = std::max(peak_, liveCount_);
    ++stats_.allocs;
    return s;
}

void DrawRecordTable::release(std::uint32_t slot) {
    if (slot >= kCapacity || slot >= liveFlags_.size() || liveFlags_[slot] == 0) return;
    liveFlags_[slot] = 0;
    --liveCount_;
    ++stats_.releases;
    pending_.push_back({slot, frame_ + rhi::kFramesInFlight + 1});
}

bool DrawRecordTable::write(std::uint32_t slot, const shader::abi::DrawRecord& rec, const shader::abi::DrawBlock& blk) {
    if (slot >= kCapacity || !live(slot)) return false;
    auto& rs = records_->shadow()[slot];
    auto& bs = blocks_->shadow()[slot];
    bool changed = false;
    if (std::memcmp(&rs, &rec, sizeof(rec)) != 0) {
        rs = rec;
        records_->mark(slot);
        changed = true;
    }
    if (std::memcmp(&bs, &blk, sizeof(blk)) != 0) {
        bs = blk;
        blocks_->mark(slot);
        changed = true;
    }
    if (changed) ++stats_.writes;
    else ++stats_.same;
    return changed;
}

void DrawRecordTable::flush(d3d9::Device& dev) {
    ++frame_;
    while (!pending_.empty() && pending_.front().dueFrame <= frame_) {
        free_.push_back(pending_.front().slot);
        pending_.pop_front();
    }
    if (!active_) return;
    if (!ready()) ensure(dev);
    if (ready()) {
        const bool verify = frame_ % kVerifyEvery == 0;
        records_->flush(dev, verify, std::max(used_, 1u));
        blocks_->flush(dev, verify, std::max(used_, 1u));
        zero_->flush(dev, false, 1);
        records_->collect(dev.game(), "draw records");
        blocks_->collect(dev.game(), "draw blocks");
    }
    const double now = std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
    if (now - lastReport_ < 5.0) return;
    lastReport_ = now;
    const auto& rs = records_->stats();
    const auto& bs = blocks_->stats();
    spdlog::info("native: draw record table (5 s): {} slots live (peak {}, cursor {} of {}), {} allocated, {} released, {} reused, FULL refusals {}; writes {} changed / {} unchanged; "
                 "uploaded records {} / blocks {} in total; read-backs {} / {} DIFFERING {} / {}",
                 liveCount_, peak_, used_, kCapacity, stats_.allocs - last_.allocs, stats_.releases - last_.releases, stats_.reused - last_.reused, stats_.full - last_.full, stats_.writes - last_.writes,
                 stats_.same - last_.same, rs.slotsUploaded, bs.slotsUploaded, rs.verifyDone, bs.verifyDone, rs.verifyBad, bs.verifyBad);
    last_ = stats_;
}

}  // namespace rvk::gpuscene
