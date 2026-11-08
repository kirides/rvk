#include "gpuscene/billboard_table.h"

#include <cstring>

#include <spdlog/spdlog.h>

#include "d3d9/device.h"
#include "native/static_ops_retained.h"

namespace rvk::gpuscene {

BbTable& bbtable() {
    static BbTable t;
    return t;
}

std::uint32_t BbTable::intern(const float* consts, std::uint32_t n) {
    if (!active_ || buffer_ == nullptr || n == 0 || n > kBbRegs - 1) return kNone;
    const std::uint64_t h = native::ret::hashBytes(consts, static_cast<std::size_t>(n) * 16);
    auto& bucket = byHash_[h];
    for (const std::uint32_t s : bucket) {
        if (std::memcmp(shadow_[s].regs[1], consts, static_cast<std::size_t>(n) * 16) == 0 && n * 16 <= sizeof(shadow_[s].regs) - 16) {
            // (the rest of the record must be zero: equal prefix and equal length = equal image)
            bool tailZero = true;
            const float* t = shadow_[s].regs[1 + n];
            for (std::uint32_t i = 0; i < (kBbRegs - 1 - n) * 4 && tailZero; ++i) tailZero = t[i] == 0.0f;
            if (tailZero) return s;
        }
    }
    if (used_ >= kCapacity) {
        ++full_;
        return kNone;
    }
    const std::uint32_t slot = used_++;
    BbConstRecord& r = shadow_[slot];
    std::memset(&r, 0, sizeof(r));
    std::memcpy(r.regs[1], consts, static_cast<std::size_t>(n) * 16);
    bucket.push_back(slot);
    return slot;
}

bool BbTable::windowFor(std::uint32_t slot, d3d9::OwnedWindows::Win& out) const {
    if (!active_ || buffer_ == nullptr || ref_.null() || slot >= used_) return false;
    out = d3d9::OwnedWindows::Win{};
    out.kind = d3d9::OwnedWindows::Kind::Object;  // (a persistent VS window 2 image; the kind only names the counter)
    out.base = ref_.plus(static_cast<std::int64_t>(slot) * static_cast<std::int64_t>(sizeof(BbConstRecord)) - static_cast<std::int64_t>(kBbFirstReg) * 16);
    out.covered = kBbRegs;
    out.image = shadow_[slot].regs[0];
    return true;
}

void BbTable::flush(d3d9::Device& dev) {
    if (!active_ || failed_) return;
    if (buffer_ == nullptr) {
        rhi::Device* rd = dev.rhi();
        if (rd == nullptr) return;
        buffer_ = rd->createBuffer({static_cast<std::uint64_t>(kCapacity) * sizeof(BbConstRecord) + kTablePad,
                                    VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT, rhi::Memory::Device, "billboard constants"});
        if (!buffer_) {
            failed_ = true;
            spdlog::warn("native: owned billboards: the constants table buffer could not be created (the billboards stay composed)");
            return;
        }
        const auto w = buffer_->shaderRef(kTablePad);
        ref_ = rhi::GpuRef::fromWords(w[0], w[1]);
        spdlog::info("native: gpu table: billboard constants buffer {} slots x {} B = {} KiB (ref {:#x})", kCapacity, sizeof(BbConstRecord), kCapacity * sizeof(BbConstRecord) / 1024, ref_.bits);
        return;
    }
    if (uploaded_ == used_) return;
    rhi::GameDevice& game = dev.game();
    const std::uint64_t bytes = static_cast<std::uint64_t>(used_ - uploaded_) * sizeof(BbConstRecord);
    rhi::RingAlloc a = game.allocRing(bytes, 16);
    if (a.ptr == nullptr) return;  // (the next present)
    std::memcpy(a.ptr, &shadow_[uploaded_], bytes);
    rhi::CommandList& list = dev.uploadList();
    list.copyBuffer(*buffer_, kTablePad + static_cast<std::uint64_t>(uploaded_) * sizeof(BbConstRecord), game.ringBuffer(a), a.offset, bytes);
    list.bufferBarrier(*buffer_, rhi::BufferAccess::ShaderRead);
    uploaded_ = used_;
    ++uploads_;
}

}  // namespace rvk::gpuscene
