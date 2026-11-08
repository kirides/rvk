#pragma once

// The billboard constants table (docs/research/frame-ownership.md 7.17, F4-1; setting `native.owned_billboards`): the tree billboards' VS constants c32..c36 (the engine's fixed
// tex-coord table) + c37 + j (the PS's tex-coord list entries of the sub-mesh) as persistent register images, VS window 2 (c31..c231) of the owned billboard draws. Records are
// interned by content (the ops of one billboard area share one image), immutable once written (a frame in flight reads them) and uploaded once, by `flush` at the present into the
// frame's upload list (which runs before the frame's draws). The window base is record - 31 x 16, as the object table's (kTablePad keeps the biased offset >= 0 on D3D12).

#include <cstdint>
#include <memory>
#include <unordered_map>
#include <vector>

#include "d3d9/owned_windows.h"
#include "gpuscene/window_compose.h"
#include "rhi/game_device.h"

namespace rvk::d3d9 {
class Device;
}

namespace rvk::gpuscene {

inline constexpr std::uint32_t kBbFirstReg = 31;
inline constexpr std::uint32_t kBbRegs = 201;  // c31 .. c231: the whole VS window 2
struct alignas(16) BbConstRecord {
    float regs[kBbRegs][4];  // [0] = c31 (zero: no billboard shader reads it), [1 + i] = c32 + i, the rest zero
};

class BbTable {
public:
    static constexpr std::uint32_t kCapacity = 256;  // images (0.8 MiB; never freed, interned by content: 2 used in every test scene; F-INT 7.43: refusals are counted now, a full table leaves the rest composed)
    static constexpr std::uint32_t kNone = 0xffffffffu;

    void setActive(bool on) { active_ = on; }
    bool active() const { return active_; }
    // the slot of an image holding `n` registers c32.. = `consts` (n <= 200): found by content or appended; kNone: not ready / full / too long
    std::uint32_t intern(const float* consts, std::uint32_t n);
    // the VS window 2 of slot `slot` (kind Object-like image offer, covered = kBbRegs); false: not ready
    bool windowFor(std::uint32_t slot, d3d9::OwnedWindows::Win& out) const;
    // the CPU twin of a slot's registers (c31 first)
    const BbConstRecord* record(std::uint32_t slot) const { return slot < used_ ? &shadow_[slot] : nullptr; }
    // once per present: creates the buffer, uploads the records appended since the last flush
    void flush(d3d9::Device& dev);
    bool ready() const { return buffer_ != nullptr; }
    std::uint32_t used() const { return used_; }
    std::uint64_t fullRefusals() const { return full_; }
    std::uint64_t uploads() const { return uploads_; }

private:
    bool active_ = false, failed_ = false;
    std::unique_ptr<rhi::Buffer> buffer_;
    rhi::GpuRef ref_;  // record 0 (past the pad)
    std::vector<BbConstRecord> shadow_ = std::vector<BbConstRecord>(kCapacity);
    std::unordered_map<std::uint64_t, std::vector<std::uint32_t>> byHash_;
    std::uint32_t used_ = 0, uploaded_ = 0;
    std::uint64_t uploads_ = 0, full_ = 0;
};

BbTable& bbtable();

}  // namespace rvk::gpuscene
