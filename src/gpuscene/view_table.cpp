#include "gpuscene/view_table.h"

#include <algorithm>
#include <chrono>
#include <cstring>

#include <spdlog/spdlog.h>

#include "core/f2c4_measure.h"
#include "d3d9/device.h"

namespace rvk::gpuscene {

namespace {
constexpr std::uint64_t kVerifyEvery = 240;
}

ViewTable& viewtable() {
    static ViewTable t;
    return t;
}

void ViewTable::releaseGpu() {
    table_.release();
    created_ = gpuFailed_ = false;
}

bool ViewTable::ensure(d3d9::Device& dev) {
    if (table_.ready()) return true;
    if (gpuFailed_ || dev.rhi() == nullptr) return false;
    if (!created_) {
        created_ = true;
        table_.init(kCapacity);
        if (!table_.create(dev, kCapacity, "view table")) {
            gpuFailed_ = true;
            spdlog::warn("native: view table: the buffer could not be created (scene ABI v2 stays off)");
            return false;
        }
        table_.markAll(kCapacity);
    }
    dev.setSceneViewTable(table_.ref(), table_.shadow().data(), kCapacity);
    return table_.ready();
}

void ViewTable::build(shader::abi::ViewBlock2& b, const Regs& r) const {
    std::memset(&b, 0, sizeof(b));
    if (r.vs0 != nullptr) std::memcpy(b.vs0, r.vs0, sizeof(b.vs0));
    if (r.ps0 != nullptr) std::memcpy(b.ps0, r.ps0, sizeof(b.ps0));
    if (r.vs1 != nullptr) std::memcpy(b.vs1, r.vs1, sizeof(b.vs1));
    if (r.posFix != nullptr) std::memcpy(b.posFix, r.posFix, sizeof(b.posFix));
    if (r.c5 != nullptr) std::memcpy(b.c5, r.c5, 3 * sizeof(float));
}

bool ViewTable::set(std::uint32_t slot, const Regs& r) {
    if (!active_ || !table_.ready() || slot >= kCapacity) return false;
    ++stats_.sets;
    shader::abi::ViewBlock2 cand;
    build(cand, r);
    auto& shadow = table_.shadow();
    const bool differs = std::memcmp(&shadow[slot], &cand, sizeof(cand)) != 0;
    if (setFrame_[slot] == frame_) {
        if (differs) {
            ++stats_.conflicts;  // (the GPU sees ONE content of the slot per frame: the draws of the second view stay composed)
            return false;
        }
        ++stats_.same;
        return true;
    }
    if (f2c4::enabled() && slot < 2 && setFrame_[slot] != 0) {
        // F2c-4-0 item 2: this pass's block against the one of the last frame it was set in (the shadow holds it): which parts changed
        f2c4::Counters& m = f2c4::counters();
        const shader::abi::ViewBlock2& o = shadow[slot];
        const bool vs0 = std::memcmp(o.vs0, cand.vs0, sizeof(o.vs0)) != 0, vs1 = std::memcmp(o.vs1, cand.vs1, sizeof(o.vs1)) != 0, ps0 = std::memcmp(o.ps0, cand.ps0, sizeof(o.ps0)) != 0,
                   pxy = std::memcmp(o.posFix, cand.posFix, 2 * sizeof(float)) != 0, pzw = std::memcmp(o.posFix + 2, cand.posFix + 2, 2 * sizeof(float)) != 0, c5 = std::memcmp(o.c5, cand.c5, 3 * sizeof(float)) != 0;
        ++m.cmp[slot];
        m.chVs0[slot] += vs0;
        m.chVs1[slot] += vs1;
        m.chPs0[slot] += ps0;
        m.chPosFixXy[slot] += pxy;
        m.chPosFixZw[slot] += pzw;
        m.chC5[slot] += c5;
        m.chAny[slot] += differs;
        m.chOnlyC5Posfix[slot] += differs && !vs0 && !vs1 && !ps0;
    }
    setFrame_[slot] = frame_;
    if (differs) {
        shadow[slot] = cand;
        table_.mark(slot);
        ++stats_.written;
    } else {
        ++stats_.same;
    }
    return true;
}

std::uint32_t ViewTable::acquire(const Regs& r) {
    if (!active_ || !table_.ready()) return kNoSlot;
    shader::abi::ViewBlock2 cand;
    build(cand, r);
    auto& shadow = table_.shadow();
    for (std::uint32_t s = shader::abi::kViewSlotDynamicFirst; s < dynCursor_; ++s) {
        if (std::memcmp(&shadow[s], &cand, sizeof(cand)) == 0) {
            ++stats_.dynamicReused;
            return s;
        }
    }
    if (dynCursor_ >= kCapacity) {
        ++stats_.dynamicFull;
        return kNoSlot;
    }
    const std::uint32_t s = dynCursor_++;
    ++stats_.dynamic;
    setFrame_[s] = frame_;
    if (std::memcmp(&shadow[s], &cand, sizeof(cand)) != 0) {
        shadow[s] = cand;
        table_.mark(s);
        ++stats_.written;
    }
    return s;
}

std::uint64_t ViewTable::windowBase(rhi::GpuRef table, std::uint32_t slot, bool vertex, std::uint32_t window) {
    using namespace shader::abi;
    const std::int64_t at = static_cast<std::int64_t>(slot) * kViewBlock2Size;
    if (vertex && window == 0) return table.plus(at + kViewVs0Offset).bits;
    if (vertex && window == 1) return table.plus(at + static_cast<std::int64_t>(kViewVs1Offset) - static_cast<std::int64_t>(kVsWindowFirst[1]) * 16).bits;
    if (!vertex && window == 0) return table.plus(at + kViewPs0Offset).bits;
    return 0;
}

bool ViewTable::fillWindows(std::uint32_t slot, d3d9::OwnedWindows& ow) const {
    using d3d9::OwnedWindows;
    using namespace shader::abi;
    if (!table_.ready() || slot >= kCapacity) return false;
    const rhi::GpuRef t = table_.ref();
    const ViewBlock2& b = table_.shadow()[slot];
    OwnedWindows::Win& vs = ow.w[0][0];
    vs = OwnedWindows::Win{};
    vs.kind = OwnedWindows::Kind::View;
    vs.base = rhi::GpuRef{windowBase(t, slot, true, 0)};
    vs.covered = 12;
    vs.verifyMask = 0xFFFu;
    vs.image = b.vs0[0];
    OwnedWindows::Win& ps = ow.w[1][0];
    ps = OwnedWindows::Win{};
    ps.kind = OwnedWindows::Kind::View;
    ps.base = rhi::GpuRef{windowBase(t, slot, false, 0)};
    ps.covered = 5;
    ps.verifyMask = 0x1Fu;
    ps.image = b.ps0[0];
    OwnedWindows::Win& w1 = ow.w[0][1];
    w1 = OwnedWindows::Win{};
    w1.kind = OwnedWindows::Kind::View;
    w1.base = rhi::GpuRef{windowBase(t, slot, true, 1)};
    w1.covered = 19;
    w1.verifyMask = 0x7ffffu;
    w1.image = b.vs1[0];
    ow.viewSlot = slot;
    return true;
}

void ViewTable::flush(d3d9::Device& dev) {
    ++frame_;
    dynCursor_ = shader::abi::kViewSlotDynamicFirst;
    if (!active_) return;
    if (!table_.ready()) ensure(dev);
    if (table_.ready()) {
        table_.flush(dev, frame_ % kVerifyEvery == 0, kCapacity);
        table_.collect(dev.game(), "view table");
    }
    const double now = std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
    if (now - lastReport_ < 5.0) return;
    lastReport_ = now;
    const std::uint64_t frames = std::max<std::uint64_t>(frame_ - lastFrame_, 1);
    const auto& ts = table_.stats();
    spdlog::info("native: view table (5 s): {} slot sets ({} unchanged, {} rewritten, {} conflicts: a slot set twice in a frame with other contents), dynamic slots {} new / {} reused / {} full; "
                 "{:.2f} slots uploaded per frame; read-backs {} DIFFERING {}",
                 stats_.sets - last_.sets, stats_.same - last_.same, stats_.written - last_.written, stats_.conflicts - last_.conflicts, stats_.dynamic - last_.dynamic,
                 stats_.dynamicReused - last_.dynamicReused, stats_.dynamicFull - last_.dynamicFull, static_cast<double>(stats_.written - last_.written) / static_cast<double>(frames), ts.verifyDone,
                 ts.verifyBad);
    last_ = stats_;
    lastFrame_ = frame_;
}

}  // namespace rvk::gpuscene
