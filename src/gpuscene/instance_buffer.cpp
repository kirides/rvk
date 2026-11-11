#include "gpuscene/instance_buffer.h"

#include <algorithm>
#include <chrono>
#include <cstring>

#include <spdlog/spdlog.h>

#include "d3d9/device.h"
#include "engine/scene_table.h"

namespace rvk::gpuscene {

namespace {

constexpr std::uint64_t kVerifyEvery = 240;

void fill(InstanceRecord& r, const engine::scenetable::Slot& s) {
    std::memcpy(r.world, s.world, sizeof(r.world));
    std::memcpy(r.prev, s.prev, sizeof(r.prev));
    r.generation = s.generation;
    r.version = s.version;
    r.flags = s.flags;
    r.live = 1;
    r.movedFrame = static_cast<std::uint32_t>(s.movedFrame);
    std::memset(r.pad, 0, sizeof(r.pad));
}

}  // namespace

InstanceBuffer& instances() {
    static InstanceBuffer b;
    return b;
}

InstanceBuffer::~InstanceBuffer() = default;

void InstanceBuffer::releaseGpu() {
    buffer_.reset();
    ref_ = {};
    for (Pending& p : pending_) p = Pending{};
}

bool InstanceBuffer::create(d3d9::Device& dev) {
    if (failed_) return false;
    rhi::Device* rd = dev.rhi();
    if (rd == nullptr) return false;
    buffer_ = rd->createBuffer({static_cast<std::uint64_t>(kCapacity) * sizeof(InstanceRecord),
                                VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT, rhi::Memory::Device,
                                "gpu scene instances"});
    if (!buffer_) {
        failed_ = true;
        spdlog::warn("gpu scene: the instance buffer could not be created");
        return false;
    }
    const auto w = buffer_->shaderRef(0);
    ref_ = rhi::GpuRef::fromWords(w[0], w[1]);
    shadow_.assign(kCapacity, InstanceRecord{});
    // slots that exist already (the table may have been filled before the buffer): all uploaded by the first flush
    spdlog::info("gpu scene: instance buffer {} slots x {} B = {} KiB (ref {:#x})", kCapacity, sizeof(InstanceRecord), kCapacity * sizeof(InstanceRecord) / 1024, ref_.bits);
    return true;
}

void InstanceBuffer::flush(d3d9::Device& dev, int mode) {
    ++frame_;
    if (mode <= 0) {
        if (buffer_) {
            engine::scenetable::trackFreed(false);
        }
        return;
    }
    const auto t0 = std::chrono::steady_clock::now();
    const bool first = !buffer_;
    if (first && !create(dev)) return;
    engine::scenetable::trackFreed(true);

    std::vector<std::uint32_t> idx;
    if (first) {
        const std::uint32_t cap = engine::scenetable::capacity();
        for (std::uint32_t i = 0; i < cap && i < kCapacity; ++i) {
            if (const auto* s = engine::scenetable::slot(i)) {
                fill(shadow_[i], *s);
                idx.push_back(i);
            }
        }
        engine::scenetable::drainFreed({});
        engine::scenetable::drainDirty({});
    } else {
        engine::scenetable::drainFreed([&](std::uint32_t i) {
            if (i >= kCapacity) {
                ++stats_.overflow;
                return;
            }
            ++stats_.freed;
            std::memset(&shadow_[i], 0, sizeof(InstanceRecord));
            idx.push_back(i);
        });
        engine::scenetable::drainDirty([&](std::uint32_t i, const engine::scenetable::Slot& s) {
            if (i >= kCapacity) {
                ++stats_.overflow;
                return;
            }
            fill(shadow_[i], s);
            idx.push_back(i);
        });
    }
    rhi::GameDevice& game = dev.game();
    rhi::CommandList* list = nullptr;
    if (!idx.empty()) {
        std::sort(idx.begin(), idx.end());
        idx.erase(std::unique(idx.begin(), idx.end()), idx.end());
        const std::uint64_t bytes = static_cast<std::uint64_t>(idx.size()) * sizeof(InstanceRecord);
        rhi::RingAlloc a = game.allocRing(bytes, 16);
        if (a.ptr == nullptr) return;
        auto* dst = static_cast<std::uint8_t*>(a.ptr);
        for (const std::uint32_t i : idx) {
            std::memcpy(dst, &shadow_[i], sizeof(InstanceRecord));
            dst += sizeof(InstanceRecord);
        }
        list = &dev.uploadList();
        rhi::Buffer& ring = game.ringBuffer(a);
        std::uint64_t pos = 0;
        for (std::size_t k = 0; k < idx.size();) {
            std::size_t e = k + 1;
            while (e < idx.size() && idx[e] == idx[e - 1] + 1) ++e;
            const std::uint64_t n = (e - k) * sizeof(InstanceRecord);
            list->copyBuffer(*buffer_, static_cast<std::uint64_t>(idx[k]) * sizeof(InstanceRecord), ring, a.offset + pos, n);
            pos += n;
            ++stats_.runs;
            k = e;
        }
        stats_.slotsUploaded += idx.size();
        stats_.bytes += bytes;
        ++stats_.flushes;
    }
    if (mode >= 2 && frame_ % kVerifyEvery == 0) {
        if (list == nullptr) list = &dev.uploadList();
        verify(dev);
    } else if (list != nullptr) {
        list->bufferBarrier(*buffer_, rhi::BufferAccess::ShaderRead);
    }
    // collect finished read-backs
    if (mode >= 2) {
        for (Pending& p : pending_) {
            if (!p.busy || game.completedFrame() < p.frameValue) continue;
            p.busy = false;
            p.buffer->invalidate();
            const auto* got = static_cast<const InstanceRecord*>(p.buffer->map());
            std::uint64_t bad = 0;
            for (std::size_t i = 0; i < p.expect.size(); ++i) {
                if (std::memcmp(&got[i], &p.expect[i], sizeof(InstanceRecord)) != 0) {
                    if (bad == 0) spdlog::error("gpu scene: slot {} differs between the GPU table and the uploaded copy (generation {} vs {}, version {} vs {})", i, got[i].generation,
                                                p.expect[i].generation, got[i].version, p.expect[i].version);
                    ++bad;
                }
            }
            ++stats_.verifyDone;
            stats_.verifySlots += p.expect.size();
            stats_.verifyBad += bad;
            p.expect.clear();
        }
    }
    stats_.flushMs += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    report();
}

void InstanceBuffer::verify(d3d9::Device& dev) {
    rhi::GameDevice& game = dev.game();
    Pending& p = pending_[game.frameSlot() % rhi::kFramesInFlight];
    if (p.busy) {
        buffer_ ? dev.uploadList().bufferBarrier(*buffer_, rhi::BufferAccess::ShaderRead) : void();
        return;
    }
    std::uint32_t used = 0;
    for (std::uint32_t i = 0; i < kCapacity; ++i) {
        if (shadow_[i].live != 0) used = i + 1;
    }
    if (used == 0) {  // nothing served yet: a read-back would compare never-written GPU memory (the bone table flake)
        if (buffer_) dev.uploadList().bufferBarrier(*buffer_, rhi::BufferAccess::ShaderRead);
        return;
    }
    const std::uint64_t bytes = static_cast<std::uint64_t>(used) * sizeof(InstanceRecord);
    if (!p.buffer || p.buffer->desc().size < bytes) {
        p.buffer = dev.rhi()->createBuffer({static_cast<std::uint64_t>(kCapacity) * sizeof(InstanceRecord), VK_BUFFER_USAGE_TRANSFER_DST_BIT, rhi::Memory::Readback,
                                            "gpu scene verify"});
        if (!p.buffer) return;
    }
    rhi::CommandList& list = dev.uploadList();
    list.bufferBarrier(*buffer_, rhi::BufferAccess::ShaderRead);  // the uploads' copies are complete before the read-back copies them
    list.copyBuffer(*p.buffer, 0, *buffer_, 0, bytes);
    list.bufferBarrier(*buffer_, rhi::BufferAccess::ShaderRead);
    p.expect.assign(shadow_.begin(), shadow_.begin() + used);
    p.frameValue = game.frameValue();
    p.busy = true;
    ++stats_.verifies;
}

void InstanceBuffer::report() {
    const double now = std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
    if (now - lastReport_ < 5.0) return;
    lastReport_ = now;
    spdlog::info("gpu scene: {} flushes, {} slots uploaded in {} runs ({} KiB), {} freed, {:.4f} ms per flush; verify: {} read-backs compared ({} slots) DIFFERING {}; overflow {}",
                 stats_.flushes, stats_.slotsUploaded, stats_.runs, stats_.bytes / 1024, stats_.freed, stats_.flushes != 0 ? stats_.flushMs / static_cast<double>(stats_.flushes) : 0.0,
                 stats_.verifyDone, stats_.verifySlots, stats_.verifyBad, stats_.overflow);
}

}  // namespace rvk::gpuscene
