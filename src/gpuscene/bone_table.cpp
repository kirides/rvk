#include "gpuscene/bone_table.h"

#include <algorithm>
#include <chrono>
#include <cstring>

#include <spdlog/spdlog.h>

#include "d3d9/device.h"

namespace rvk::gpuscene {

namespace {
constexpr std::uint64_t kVerifyEvery = 240;
}  // namespace

BoneTable& bonetable() {
    static BoneTable* t = new BoneTable();  // (leaked: the buffer must not be released after the device at shutdown)
    return *t;
}

void BoneTable::destroy() {
    table_.release();
    std::memset(table_.shadow().data(), 0, table_.shadow().size() * sizeof(BoneRecord));
    map_.clear();
    free_.clear();
    pending_.clear();
    std::fill(servedFrame_.begin(), servedFrame_.end(), 0);
    std::fill(usedRegs_.begin(), usedRegs_.end(), 0);
    created_ = gpuFailed_ = false;
    used_ = 0;
    frame_ = 0;
}

void BoneTable::window(std::uint32_t slot, d3d9::OwnedWindows::Win& out) const {
    out = d3d9::OwnedWindows::Win{};
    out.kind = d3d9::OwnedWindows::Kind::Bones;
    out.base = table_.ref().plus(static_cast<std::int64_t>(slot) * static_cast<std::int64_t>(sizeof(BoneRecord)) - static_cast<std::int64_t>(kBoneFirstReg) * 16);
    out.covered = kBoneRegs;
    // split_verify (the composed draws' check): c32 .w and the bone rows inside the first 32 registers; c31 (the morph weights: .w is carry-over in the register file)
    // and c32 .xyz are not compared (the image holds zero there), the owned path's own check frames compare c31 .xyz
    out.verifyMask = 0xfffffffeu;
    out.wOnlyMask = 2u;
    out.image = table_.shadow()[slot].regs[0];
}

BoneWhy BoneTable::serve(const BoneKey& key, const float (*regs)[4], std::uint32_t count, d3d9::OwnedWindows::Win& out) {
    ++stats_.requests;
    const auto fail = [&](BoneWhy why) {
        ++stats_.why[static_cast<std::size_t>(why)];
        return why;
    };
    if (!active_ || !table_.ready() || table_.ref().null() || key.wrapper == nullptr || regs == nullptr) return fail(BoneWhy::NotReady);
    if (count < 2 || count > kBoneRegs) return fail(BoneWhy::TooLong);
    auto it = map_.find(key);
    if (it == map_.end()) {
        // (a released slot comes back after kFramesInFlight + 1 presents)
        std::uint32_t slot = UINT32_MAX;
        if (!free_.empty()) {
            slot = free_.back();
            free_.pop_back();
        } else if (used_ < kCapacity - 1) {  // (the last record stays zero: the tail pad a window read past its slot lands in)
            slot = used_++;
        } else {
            return fail(BoneWhy::Full);
        }
        it = map_.emplace(key, Entry{slot, frame_ + 1}).first;
        std::memset(&table_.shadow()[slot], 0, sizeof(BoneRecord));
        usedRegs_[slot] = 0;
        table_.mark(slot);
        servedFrame_[slot] = 0;
        ++stats_.newSlots;
    }
    Entry& e = it->second;
    e.lastUse = frame_ + 1;
    // (c31 .w and c32 .xyz are carry-over of the engine's register pool: never read by the skinned vertex shader, zero in the image so that every writer agrees)
    float head[2][4];
    std::memcpy(head, regs, sizeof(head));
    head[0][3] = 0.0f;
    head[1][0] = head[1][1] = head[1][2] = 0.0f;
    BoneRecord& rec = table_.shadow()[e.slot];
    // equal = the same register count and the same bytes (the registers past the count are zero: a shorter image zeroes them when it is written)
    const bool differs = usedRegs_[e.slot] != count || std::memcmp(rec.regs, head, sizeof(head)) != 0 || (count > 2 && std::memcmp(rec.regs[2], regs[2], static_cast<std::size_t>(count - 2) * 16) != 0);
    const bool servedNow = servedFrame_[e.slot] == frame_ + 1;
    if (differs && servedNow) {
        if (conflictLogged_ < 8) {
            ++conflictLogged_;
            std::uint32_t reg = 0;
            while (reg < count && reg < kBoneRegs && std::memcmp(rec.regs[reg], reg < 2 ? head[reg] : regs[reg], 16) == 0) ++reg;
            spdlog::info("native: bone table: conflict: wrapper {} prim {} fade {:#x} slot {} differs at c{} (count {} vs {}), {} registers offered", key.wrapper, key.prim, key.fadeBits, e.slot, kBoneFirstReg + reg,
                         usedRegs_[e.slot], count, count);
        }
        return fail(BoneWhy::Conflict);  // (the GPU sees one value per frame)
    }
    if (differs) {
        if (usedRegs_[e.slot] > count) std::memset(rec.regs[count], 0, static_cast<std::size_t>(usedRegs_[e.slot] - count) * 16);
        std::memcpy(rec.regs, head, sizeof(head));
        if (count > 2) std::memcpy(rec.regs[2], regs[2], static_cast<std::size_t>(count - 2) * 16);
        usedRegs_[e.slot] = count;
        table_.mark(e.slot);
        ++stats_.rewrites;
    } else {
        ++stats_.unchanged;
    }
    servedFrame_[e.slot] = frame_ + 1;
    window(e.slot, out);
    ++stats_.served;
    ++stats_.why[static_cast<std::size_t>(BoneWhy::Served)];
    return BoneWhy::Served;
}

bool BoneTable::windowFor(const BoneKey& key, d3d9::OwnedWindows::Win& out) const {
    if (!active_ || !table_.ready() || table_.ref().null()) return false;
    const auto it = map_.find(key);
    if (it == map_.end()) return false;
    window(it->second.slot, out);
    return true;
}

bool BoneTable::servedNow(const BoneKey& key, d3d9::OwnedWindows::Win& out) const {
    if (!active_ || !table_.ready() || table_.ref().null()) return false;
    const auto it = map_.find(key);
    if (it == map_.end() || servedFrame_[it->second.slot] != frame_ + 1) return false;
    window(it->second.slot, out);
    return true;
}

const BoneRecord* BoneTable::image(const BoneKey& key) const {
    const auto it = map_.find(key);
    return it == map_.end() || !table_.ready() ? nullptr : &table_.shadow()[it->second.slot];
}

void BoneTable::flush(d3d9::Device& dev) {
    if (!active_) {
        ++frame_;
        return;
    }
    if (!created_ && !gpuFailed_) {
        created_ = true;
        if (!table_.create(dev, kCapacity, "bone table", kTablePad)) {
            gpuFailed_ = true;
            spdlog::warn("native: owned actors: the bone table buffer could not be created (the actors stay composed)");
        } else {
            servedFrame_.assign(kCapacity, 0);
            usedRegs_.assign(kCapacity, 0);
        }
    }
    if (table_.ready()) {
        // idle slots go back (deferred), released slots whose time has come are free again
        for (auto it = map_.begin(); it != map_.end();) {
            if (it->second.lastUse + kIdleFrames < frame_ + 1) {
                pending_.push_back({it->second.slot, frame_ + rhi::kFramesInFlight + 1});
                ++stats_.released;
                it = map_.erase(it);
            } else {
                ++it;
            }
        }
        for (std::size_t i = 0; i < pending_.size();) {
            if (pending_[i].at <= frame_) {
                free_.push_back(pending_[i].slot);
                pending_[i] = pending_.back();
                pending_.pop_back();
            } else {
                ++i;
            }
        }
        table_.flush(dev, frame_ % kVerifyEvery == 0, used_);
        table_.collect(dev.game(), "bone table");
    }
    report(dev);
    ++frame_;
}

void BoneTable::report(d3d9::Device& dev) {
    const double now = std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
    if (now - lastReport_ < 5.0) return;
    lastReport_ = now;
    const BoneStats& s = stats_;
    const BoneStats& p = last_;
    const d3d9::OwnedStats& o = dev.ownedStats();
    const auto d = [&](std::uint64_t a, std::uint64_t b) { return a - b; };
    const auto why = [&](BoneWhy w) { return d(s.why[static_cast<std::size_t>(w)], p.why[static_cast<std::size_t>(w)]); };
    const auto& ts = table_.stats();
    const std::uint64_t frames = std::max<std::uint64_t>(frame_ - lastFrame_, 1);
    spdlog::info("native: bone table (5 s): {} live slots (peak {}), requests {}: served {} ({} unchanged, {} rewritten), new slots {}, released {}, refused: not ready {}, full {}, conflict (another content in the same frame) {}, too long {}; "
                 "device served window 2 from it {} draws; {:.2f} slots / {:.2f} KiB uploaded per frame; read-backs {} DIFFERING {}",
                 map_.size(), used_, d(s.requests, p.requests), why(BoneWhy::Served), d(s.unchanged, p.unchanged), d(s.rewrites, p.rewrites), d(s.newSlots, p.newSlots), d(s.released, p.released), why(BoneWhy::NotReady),
                 why(BoneWhy::Full), why(BoneWhy::Conflict), why(BoneWhy::TooLong), o.bones - lastDeviceBones_, static_cast<double>(ts.slotsUploaded - lastSlots_) / static_cast<double>(frames),
                 static_cast<double>(ts.bytes - lastBytes_) / 1024.0 / static_cast<double>(frames), ts.verifyDone, ts.verifyBad);
    last_ = s;
    lastDeviceBones_ = o.bones;
    lastFrame_ = frame_;
    lastBytes_ = ts.bytes;
    lastSlots_ = ts.slotsUploaded;
}

}  // namespace rvk::gpuscene
