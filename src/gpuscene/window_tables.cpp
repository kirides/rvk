#include "gpuscene/window_tables.h"
#include "gpuscene/verify.h"

#include <algorithm>
#include <chrono>
#include <cstring>

#include <spdlog/spdlog.h>

#include "d3d9/device.h"

namespace rvk::gpuscene {

namespace {
constexpr std::uint64_t kVerifyEvery = 240;
constexpr std::uint32_t kSearch = 8;  // view blocks of this frame compared before a new one is written
}  // namespace

WindowTables& windowtables() {
    static WindowTables t;
    return t;
}

void WindowTables::releaseGpu() {
    views_.release();
    created_ = gpuFailed_ = false;
}

bool WindowTables::fillViews(d3d9::Device& dev, d3d9::OwnedWindows& ow, const float (*vs1)[4], std::uint32_t vs1Composed, const float (*vs0)[4], const float (*ps0)[4]) {
    using d3d9::OwnedWindows;
    ow.w[0][0] = OwnedWindows::Win{};
    ow.w[1][0] = OwnedWindows::Win{};
    ow.w[0][1] = OwnedWindows::Win{};
    if (!active_ || !views_.ready()) return false;
    ++viewLookups_;
    auto& shadow = views_.shadow();
    const float(*src0)[4] = vs0 != nullptr ? vs0 : dev.vsConstants();
    const float(*srcP)[4] = ps0 != nullptr ? ps0 : dev.psConstants();
    std::uint32_t slot = kViewCapacity;
    // 7.27: the block just used (consecutive draws of one view: nearly always) is compared with the sources directly, no candidate is built
    if (cursor_ > 0) {
        static const float kZero[kViewVs1Regs][4] = {};
        const ViewRecord& last = shadow[lastSlot_];
        if (std::memcmp(last.vs, src0, sizeof(last.vs)) == 0 && std::memcmp(last.ps, srcP, sizeof(last.ps)) == 0 &&
            std::memcmp(last.vs1, vs1 != nullptr ? vs1 : &kZero[0], sizeof(last.vs1)) == 0) {
            slot = lastSlot_;
        }
    }
    ViewRecord cand;
    if (slot == kViewCapacity) {
        std::memcpy(cand.vs, src0, sizeof(cand.vs));
        std::memcpy(cand.ps, srcP, sizeof(cand.ps));
        if (vs1 != nullptr) std::memcpy(cand.vs1, vs1, sizeof(cand.vs1));
        else std::memset(cand.vs1, 0, sizeof(cand.vs1));
    }
    // the block just used first (consecutive draws of one view), then the few before it
    for (std::uint32_t k = 0; k < kSearch && k < cursor_ && slot == kViewCapacity; ++k) {
        const std::uint32_t s = k == 0 ? lastSlot_ : (lastSlot_ + cursor_ - k) % cursor_;
        if (std::memcmp(&shadow[s], &cand, sizeof(cand)) == 0) {
            slot = s;
            break;
        }
    }
    if (slot != kViewCapacity) {
        ++viewReused_;
    } else if (cursor_ < kViewCapacity) {
        slot = cursor_++;
        peak_ = std::max(peak_, cursor_);
        if (std::memcmp(&shadow[slot], &cand, sizeof(cand)) != 0) {
            // (statistics: is this block a variant of one of this frame's other blocks in c0..c3 only (depth-bias view-projection) / c12..c30 only?)
            for (std::uint32_t s = 0; s < slot; ++s) {
                const ViewRecord& o = shadow[s];
                const bool restSame = std::memcmp(&o.vs[4], &cand.vs[4], sizeof(cand.vs) - 4 * 16) == 0 && std::memcmp(o.ps, cand.ps, sizeof(cand.ps)) == 0;
                if (restSame && std::memcmp(o.vs1, cand.vs1, sizeof(cand.vs1)) == 0) {
                    ++viewDbvp_;
                    break;
                }
                if (std::memcmp(o.vs, cand.vs, sizeof(cand.vs)) == 0 && std::memcmp(o.ps, cand.ps, sizeof(cand.ps)) == 0) {
                    ++viewVs1Split_;
                    break;
                }
            }
            shadow[slot] = cand;
            views_.mark(slot);
            ++viewNew_;
        } else {
            ++viewSame_;  // (the GPU already holds these bytes from an earlier frame)
        }
    } else {
        ++viewFull_;
        return false;
    }
    lastSlot_ = slot;
    const rhi::GpuRef rec = views_.ref().plus(static_cast<std::int64_t>(slot) * static_cast<std::int64_t>(sizeof(ViewRecord)));
    OwnedWindows::Win& vs = ow.w[0][0];
    vs.kind = OwnedWindows::Kind::View;
    vs.base = rec;  // (window 0 starts at register 0: no bias)
    vs.covered = 12;
    vs.verifyMask = 0xFFFu;
    vs.image = shadow[slot].vs[0];
    OwnedWindows::Win& ps = ow.w[1][0];
    ps.kind = OwnedWindows::Kind::View;
    ps.base = rec.plus(static_cast<std::int64_t>(offsetof(ViewRecord, ps)));
    ps.covered = 5;
    ps.verifyMask = 0x1Fu;
    ps.image = shadow[slot].ps[0];
    if (vs1 != nullptr) {
        // VS window 1 (c12..c30): the block's vs1 registers; the biased base = block + 272 - 12 x 16
        OwnedWindows::Win& w1 = ow.w[0][1];
        w1.kind = OwnedWindows::Kind::View;
        w1.base = rec.plus(static_cast<std::int64_t>(offsetof(ViewRecord, vs1)) - 12 * 16);
        w1.covered = kViewVs1Regs;
        w1.verifyMask = vs1Composed & ((1u << kViewVs1Regs) - 1u);
        w1.ignoreMask = ~vs1Composed & ((1u << kViewVs1Regs) - 1u);  // (an uncomposed register holds the admin pool's value; the file holds the stale one of an earlier draw)
        w1.image = shadow[slot].vs1[0];
    }
    return true;
}

void WindowTables::flush(d3d9::Device& dev) {
    ++frame_;
    cursor_ = 0;
    lastSlot_ = 0;
    if (!active_) return;
    if (!created_ && !gpuFailed_) {
        created_ = true;
        views_.init(kViewCapacity);
        if (!views_.create(dev, kViewCapacity, "view blocks")) {
            gpuFailed_ = true;
            spdlog::warn("native: owned windows: the view block buffer could not be created (the view windows stay uploaded)");
        } else {
            views_.markAll(kViewCapacity);
        }
    }
    if (views_.ready()) {
        views_.flush(dev, periodic(frame_, kVerifyEvery), kViewCapacity);
        views_.collect(dev.game(), "view blocks");
    }
    report(dev);
}

void WindowTables::report(d3d9::Device& dev) {
    const double now = std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
    if (now - lastReport_ < 5.0) return;
    lastReport_ = now;
    const d3d9::OwnedStats& s = dev.ownedStats();
    const d3d9::OwnedStats& p = last_;
    const auto& ts = views_.stats();
    const std::uint64_t frames = std::max<std::uint64_t>(frame_ - lastFrame_, 1);
    const std::uint64_t draws = s.draws - p.draws;
    const std::uint64_t fromMat = s.material[0] - p.material[0] + s.material[1] - p.material[1];
    const std::uint64_t fromView = s.view[0] - p.view[0] + s.view[1] - p.view[1];
    const std::uint64_t fromLight = s.lighting - p.lighting;
    spdlog::info(
        "native: owned windows (5 s): draws {} ({:.0f} per frame), windows from tables: material VS {} PS {}, view VS {} PS {}, lighting {} ({} + {} + {} = {} of {} candidate windows); uploaded {}, reused {} "
        "(the draws' other windows); fallback: switch materials {}, other {} (no record: sky / decals / FX / unread), image too short {}; views: {} lookups, {} reused, {} new blocks, "
        "{} rewritten-equal, {} full (peak {} per frame; {:.2f} blocks / {:.2f} KiB uploaded per frame); verify: {} windows compared against the register file, MISMATCHES {}, "
        "read-backs {} DIFFERING {}",
        draws, static_cast<double>(draws) / static_cast<double>(frames), s.material[0] - p.material[0], s.material[1] - p.material[1], s.view[0] - p.view[0], s.view[1] - p.view[1], fromLight, fromMat,
        fromView, fromLight, fromMat + fromView + fromLight, fromMat + fromView + fromLight + (s.fallbackSwitch - p.fallbackSwitch) + (s.fallbackOther - p.fallbackOther) + (s.fallbackShort - p.fallbackShort),
        s.uploaded - p.uploaded, s.reused - p.reused, s.fallbackSwitch - p.fallbackSwitch, s.fallbackOther - p.fallbackOther, s.fallbackShort - p.fallbackShort, viewLookups_ - lastLookups_,
        viewReused_ - lastReused_, viewNew_ - lastNew_, viewSame_, viewFull_, peak_, static_cast<double>(ts.slotsUploaded - lastSlots_) / static_cast<double>(frames),
        static_cast<double>(ts.bytes - lastBytes_) / 1024.0 / static_cast<double>(frames), s.verified - p.verified, s.mismatches, ts.verifyDone, ts.verifyBad);
    last_ = s;
    lastLookups_ = viewLookups_;
    lastReused_ = viewReused_;
    lastNew_ = viewNew_;
    lastFrame_ = frame_;
    lastBytes_ = ts.bytes;
    lastSlots_ = ts.slotsUploaded;
    peak_ = 0;
}

}  // namespace rvk::gpuscene
