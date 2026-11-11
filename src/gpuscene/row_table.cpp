#include "gpuscene/row_table.h"

#include <algorithm>
#include <chrono>
#include <cstring>

#include <spdlog/spdlog.h>

#include "d3d9/device.h"
#include "gpuscene/window_compose.h"

namespace rvk::gpuscene {

namespace {
constexpr std::uint64_t kVerifyEvery = 240;
}  // namespace

RowTable& rowtable() {
    static RowTable* t = new RowTable();  // (leaked: the buffer must not be released after the device at shutdown)
    return *t;
}

std::uint32_t RowTable::alloc(const float regs[kObjectRegs][4], std::uint32_t composedBits) {
    if (!active_ || !table_.ready() || table_.ref().null()) return kNoSlot;
    if ((composedBits & 1u) != 0) {
        ++stats_.c31Live;
        return kNoSlot;
    }
    std::uint32_t slot = kNoSlot;
    if (!free_.empty()) {
        slot = free_.back();
        free_.pop_back();
    } else if (top_ < kCapacity) {
        slot = top_++;
    } else {
        ++stats_.full;
        return kNoSlot;
    }
    ObjectRecord& r = table_.shadow()[slot];
    std::memset(&r, 0, sizeof(r));
    for (std::uint32_t i = 1; i < kObjectRegs; ++i) {
        if ((composedBits >> i & 1u) != 0) std::memcpy(r.regs[i], regs[i], 16);
    }
    r.regs[1][0] = r.regs[1][1] = r.regs[1][2] = 0.0f;  // (c32 .xyz: carry-over in the engine's register file, never read by a static draw)
    r.info[3] = 1;
    table_.mark(slot);
    used_[slot] = 1;
    ++live_;
    peak_ = std::max(peak_, live_);
    ++stats_.allocs;
    return slot;
}

void RowTable::release(std::uint32_t slot) {
    if (slot >= kCapacity || slot >= used_.size() || used_[slot] == 0) return;
    used_[slot] = 0;
    --live_;
    ++stats_.releases;
    pending_.push_back({slot, frame_ + rhi::kFramesInFlight + 1});
}

bool RowTable::windowFor(std::uint32_t slot, std::uint32_t composedBits, d3d9::OwnedWindows::Win& out) const {
    if (!active_ || !table_.ready() || table_.ref().null() || slot >= kCapacity || slot >= used_.size() || used_[slot] == 0) return false;
    out = d3d9::OwnedWindows::Win{};
    out.kind = d3d9::OwnedWindows::Kind::Object;
    out.base = table_.ref().plus(static_cast<std::int64_t>(slot) * static_cast<std::int64_t>(sizeof(ObjectRecord)) - static_cast<std::int64_t>(kObjectFirstReg) * 16);
    out.covered = kObjectRegs;
    out.verifyMask = composedBits & ~1u;
    out.wOnlyMask = 2u;
    out.ignoreMask = ((~composedBits) & ((1u << kObjectRegs) - 1u)) | 1u;
    out.image = table_.shadow()[slot].regs[0];
    return true;
}

bool RowTable::same(std::uint32_t slot, const float regs[kObjectRegs][4], std::uint32_t composedBits) const {
    if (!table_.ready() || slot >= kCapacity || slot >= used_.size() || used_[slot] == 0 || (composedBits & 1u) != 0) return false;
    const ObjectRecord& r = table_.shadow()[slot];
    if ((composedBits & 2u) != 0 && std::memcmp(&r.regs[1][3], &regs[1][3], 4) != 0) return false;
    for (std::uint32_t i = 2; i < kObjectRegs; ++i) {
        if ((composedBits >> i & 1u) != 0 && std::memcmp(r.regs[i], regs[i], 16) != 0) return false;
    }
    return true;
}

bool RowTable::worldRowsAt(std::uint64_t baseBits, float world[12]) const {
    if (!active_ || !table_.ready() || table_.ref().null()) return false;
    const std::int64_t delta = (static_cast<std::int64_t>(baseBits - table_.ref().bits) >> rhi::detail::g_refShift) + static_cast<std::int64_t>(kObjectFirstReg) * 16;
    if (delta < 0 || delta % static_cast<std::int64_t>(sizeof(ObjectRecord)) != 0) return false;
    const std::uint64_t slot = static_cast<std::uint64_t>(delta) / sizeof(ObjectRecord);
    if (slot >= kCapacity || slot >= used_.size() || used_[slot] == 0) return false;
    std::memcpy(world, table_.shadow()[slot].regs[2], 48);
    return true;
}

void RowTable::flush(d3d9::Device& dev) {
    ++frame_;
    if (!active_) return;
    if (!created_ && !failed_) {
        created_ = true;
        if (!table_.create(dev, kCapacity, "instance rows", kTablePad)) {
            failed_ = true;
            spdlog::warn("native: owned instances: the instance row table could not be created (the instance lists stay composed)");
        } else {
            used_.assign(kCapacity, 0);
            table_.zero(dev);  // (the read-back covers slots that were never written)
        }
    }
    if (table_.ready()) {
        for (std::size_t i = 0; i < pending_.size();) {
            if (pending_[i].at <= frame_) {
                free_.push_back(pending_[i].slot);
                pending_[i] = pending_.back();
                pending_.pop_back();
            } else {
                ++i;
            }
        }
        table_.flush(dev, frame_ % kVerifyEvery == 0, top_);
        table_.collect(dev.game(), "instance rows");
    }
    const double now = std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
    if (now - lastReport_ < 5.0) return;
    lastReport_ = now;
    const auto& ts = table_.stats();
    spdlog::info("native: instance rows (5 s): {} live images (peak {}, top {} of {}), new {}, released {}, refused: full {}, c31 read {}; read-backs {} DIFFERING {}", live_, peak_, top_, kCapacity,
                 stats_.allocs - last_.allocs, stats_.releases - last_.releases, stats_.full - last_.full, stats_.c31Live - last_.c31Live, ts.verifyDone, ts.verifyBad);
    last_ = stats_;
}

void RowTable::releaseGpu() {
    table_.release();
    std::fill(used_.begin(), used_.end(), 0);
    free_.clear();
    pending_.clear();
    top_ = live_ = 0;
    created_ = failed_ = false;
}

}  // namespace rvk::gpuscene
