#include "gpuscene/owned_compact.h"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <future>

#include <spdlog/spdlog.h>

#include "d3d9/device.h"
#include "gpuscene/draw_record_table.h"
#include "shader/embedded.h"

extern "C" __declspec(dllimport) unsigned long __stdcall GetEnvironmentVariableA(const char* name, char* buffer, unsigned long size);

namespace rvk::gpuscene {

struct OwnedCompact::Pipeline {
    std::future<std::unique_ptr<rhi::Pipeline>> building;
    std::unique_ptr<rhi::Pipeline> pipe;
    bool failed = false;
};

OwnedCompact::OwnedCompact() = default;
OwnedCompact::~OwnedCompact() = default;

OwnedCompact& ownedcompact() {
    static OwnedCompact* c = new OwnedCompact();  // (leaked like the other tables: the device goes first)
    return *c;
}

void OwnedCompact::destroy() {
    for (SlotState& s : slots_) s = SlotState{};
    pipeline_.reset();
    failed_ = false;
}

std::uint64_t OwnedCompact::bytes() const {
    std::uint64_t b = 0;
    for (const SlotState& s : slots_) {
        if (s.buffer) b += s.buffer->desc().size;
        if (s.readback) b += s.readback->desc().size;
    }
    return b;
}

OwnedCompact::SlotState& OwnedCompact::slotFor(d3d9::Device& dev) {
    rhi::GameDevice& game = dev.game();
    SlotState& s = slots_[game.frameSlot() % rhi::kFramesInFlight];
    if (s.frameSeen != game.frameValue()) {  // the first use in this CPU frame: fresh cursors (the jobs of an unpresented frame are dropped)
        s.frameSeen = game.frameValue();
        s.items = s.buckets = 0;
        s.jobs.clear();
    }
    return s;
}

bool OwnedCompact::ensureBuffers(d3d9::Device& dev, SlotState& s) {
    if (s.buffer) return true;
    rhi::Device* rd = dev.rhi();
    if (rd == nullptr) return false;
    const std::uint64_t size = static_cast<std::uint64_t>(kArgsBase) + static_cast<std::uint64_t>(kMaxItems) * rhi::kIndirectStride;
    s.buffer = rd->createBuffer({size, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT, rhi::Memory::Device,
                                 "owned draws compacted arguments"});
    if (s.buffer) s.handle = dev.game().packetHandle(*s.buffer);
    return s.buffer != nullptr;
}

bool OwnedCompact::usable(d3d9::Device& dev) {
    if (failed_) return false;
    if (!envRead_) {
        envRead_ = true;
        char v[4] = {};
        if (GetEnvironmentVariableA("RVK_F2C4C_CPU", v, sizeof(v)) != 0 && v[0] != '0') {
            cpuOnly_ = true;
            spdlog::info("native: owned compaction: RVK_F2C4C_CPU = the CPU-written arguments");
        }
    }
    if (!checkEverySet_) {
        checkEverySet_ = true;
        char c[16] = {};
        if (GetEnvironmentVariableA("RVK_F2C4C_CHECK", c, sizeof(c)) != 0) checkEvery_ = static_cast<std::uint32_t>(std::strtoul(c, nullptr, 10));
    }
    if (cpuOnly_ || forceCpu_) return false;
    rhi::Device* rd = dev.rhi();
    if (rd == nullptr) return false;
    if (!rd->caps().drawIndirectCount || !rd->caps().drawIndirectFirstInstance) {
        cpuOnly_ = true;
        spdlog::info("native: owned compaction: the device has no indirect draw count / first instance: the CPU-written arguments stay");
        return false;
    }
    if (!pipeline_) {
        pipeline_ = std::make_unique<Pipeline>();
        pipeline_->building = std::async(std::launch::async, [rd]() {
            rhi::ComputePipelineDesc d;
            d.cs.spirv = shader::embedded::spirv("gpuscene/owned_compact.comp");
            d.cs.dxil = shader::embedded::dxilOnDemand("gpuscene/owned_compact.comp");
            d.name = "owned draws compact";
            return rd->createComputePipeline(d);
        });
    }
    if (!pipeline_->pipe) {
        if (pipeline_->failed || !pipeline_->building.valid() || pipeline_->building.wait_for(std::chrono::seconds(0)) != std::future_status::ready) return false;
        pipeline_->pipe = pipeline_->building.get();
        if (!pipeline_->pipe) {
            pipeline_->failed = true;
            failed_ = true;
            spdlog::warn("native: owned compaction: the compute pipeline could not be built (the CPU-written arguments stay)");
            return false;
        }
        spdlog::info("native: owned compaction on: argument records made by a compute pass from the persistent records (up to {} records / {} buckets per frame, {} KiB per frame slot)",
                     kMaxItems, kMaxBuckets, (static_cast<std::uint64_t>(kArgsBase) + static_cast<std::uint64_t>(kMaxItems) * rhi::kIndirectStride) / 1024);
    }
    SlotState& s = slotFor(dev);
    if (!ensureBuffers(dev, s)) {
        failed_ = true;
        spdlog::warn("native: owned compaction: the buffers could not be created (the CPU-written arguments stay)");
        return false;
    }
    return true;
}

bool OwnedCompact::checkThisFrame(d3d9::Device& dev) {
    if (checkEvery_ == 0) return false;
    SlotState& s = slotFor(dev);
    return s.checks.empty() && dev.game().frameValue() % checkEvery_ == 0;
}

bool OwnedCompact::reserve(d3d9::Device& dev, std::uint32_t items, std::uint32_t buckets, Range& r) {
    SlotState& s = slotFor(dev);
    if (s.items + items > kMaxItems || s.buckets + buckets > kMaxBuckets) {
        ++stats_.cpuFull;
        return false;
    }
    r.firstArg = s.items;
    r.firstCount = s.buckets;
    s.items += items;
    s.buckets += buckets;
    return true;
}

void OwnedCompact::queue(d3d9::Device& dev, Job&& job) {
    SlotState& s = slotFor(dev);
    ++stats_.flushes;
    stats_.items += job.items;
    stats_.buckets += job.buckets;
    s.jobs.push_back(std::move(job));
}

void OwnedCompact::fillPacket(d3d9::Device& dev, const Range& r, std::uint32_t firstItem, std::uint32_t bucket, std::uint32_t count, rhi::DrawPacket& m) {
    SlotState& s = slotFor(dev);
    m.op = rhi::DrawPacket::Op::DrawIndexedIndirectCount;
    m.count = count;  // (the upper bound: the GPU's count of the bucket decides)
    m.indirectBuffer = s.handle;
    m.indirectOffset = static_cast<std::uint64_t>(kArgsBase) + static_cast<std::uint64_t>(r.firstArg + firstItem) * rhi::kIndirectStride;
    m.first = (r.firstCount + bucket) * 4u;  // (the count's byte offset)
}

void OwnedCompact::flush(d3d9::Device& dev) {
    rhi::GameDevice& game = dev.game();
    SlotState& s = slots_[game.frameSlot() % rhi::kFramesInFlight];
    collect(dev);
    if (s.frameSeen == game.frameValue() && !s.jobs.empty() && s.buffer && pipeline_ && pipeline_->pipe) {
        rhi::CommandList& list = dev.uploadList();
        const bool timed = game.timestampsEnabled();
        if (timed) game.timestampMark(list, game.frameSlot(), rhi::TimestampStream::Upload, "uploads before the owned draws compaction");
        // the record table's uploads of this present (transfer) and everything before are visible to the compute pass; the buffer is written next
        list.memoryBarrier(VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
                           VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_READ_BIT);
        list.bufferBarrier(*s.buffer, rhi::BufferAccess::ShaderWrite);
        list.bindPipeline(*pipeline_->pipe);
        struct Push {
            std::uint32_t records[2], list[2], jobs[2], out[2];
            std::uint32_t countsBase, argsBase, slotCount, pad;
        } push{};
        static_assert(sizeof(Push) == 48);
        const auto rr = drawrecords().recordsRef().pair();
        const auto wr = s.buffer->shaderRefRw(0);
        for (const Job& j : s.jobs) {
            const auto lr = j.list.pair();
            const auto jr = j.jobs.pair();
            push.records[0] = rr[0];
            push.records[1] = rr[1];
            push.list[0] = lr[0];
            push.list[1] = lr[1];
            push.jobs[0] = jr[0];
            push.jobs[1] = jr[1];
            push.out[0] = wr[0];
            push.out[1] = wr[1];
            push.countsBase = 0;
            push.argsBase = kArgsBase;
            push.slotCount = DrawRecordTable::kCapacity;
            list.setPushConstants(&push, sizeof(push));
            list.dispatch(j.buckets, 1, 1);
            ++stats_.dispatches;
        }
        // the check frame: the arguments + counts of the frame are copied out (before the buffer becomes an indirect source)
        const bool check = std::any_of(s.jobs.begin(), s.jobs.end(), [](const Job& j) { return !j.expect.empty(); }) && s.checks.empty();
        if (check) {
            if (!s.readback) {
                s.readback = dev.rhi()->createBuffer({static_cast<std::uint64_t>(kArgsBase) + static_cast<std::uint64_t>(kMaxItems) * rhi::kIndirectStride, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                                                      rhi::Memory::Readback, "owned draws compaction verify"});
            }
            if (s.readback) {
                list.bufferBarrier(*s.buffer, rhi::BufferAccess::ShaderRead);  // (the dispatches' writes are visible to the copy)
                list.copyBuffer(*s.readback, 0, *s.buffer, 0, static_cast<std::uint64_t>(s.buckets) * 4);
                list.copyBuffer(*s.readback, kArgsBase, *s.buffer, kArgsBase, static_cast<std::uint64_t>(s.items) * rhi::kIndirectStride);
                s.checkItems = s.items;
                s.checkBuckets = s.buckets;
                s.checkFrame = game.frameValue();
                for (Job& j : s.jobs) {
                    if (!j.expect.empty()) s.checks.push_back(std::move(j));
                }
            } else {
                ++stats_.skipped;
            }
        }
        list.bufferBarrier(*s.buffer, rhi::BufferAccess::IndirectRead);
        if (timed) game.timestampMark(list, game.frameSlot(), rhi::TimestampStream::Upload, "owned draws compaction");
        ++stats_.frames;
        s.jobs.clear();
        s.frameSeen = ~0ull;  // (the frame is done: a stray use before the next frame begins fresh cursors)
    }
    report();
}

void OwnedCompact::collect(d3d9::Device& dev) {
    rhi::GameDevice& game = dev.game();
    for (SlotState& s : slots_) {
        if (s.checks.empty() || game.completedFrame() < s.checkFrame) continue;
        s.readback->invalidate();
        const auto* got = static_cast<const std::uint32_t*>(s.readback->map());
        ++stats_.checks;
        for (const Job& j : s.checks) {
            const std::uint32_t* meta = j.expect.data();  // per bucket: first entry, expected count
            const std::uint32_t* recs = meta + 2 * static_cast<std::size_t>(j.buckets);
            std::uint32_t done = 0;
            for (std::uint32_t b = 0; b < j.buckets; ++b) {
                const std::uint32_t firstEntry = meta[2 * b], want = meta[2 * b + 1];
                const std::uint32_t have = got[j.range.firstCount + b];
                ++stats_.checkedBuckets;
                if (have != want) {
                    ++stats_.countDiffering;
                    ++stats_.differing;
                    if (differingLogged_++ < 8) spdlog::error("native: owned compaction: DIFFERING count of bucket {}: gpu {} cpu {}", j.range.firstCount + b, have, want);
                    done += want;
                    continue;
                }
                for (std::uint32_t k = 0; k < want; ++k) {
                    ++stats_.checkedRecords;
                    const std::uint32_t* a = got + kArgsBase / 4 + static_cast<std::size_t>(j.range.firstArg + firstEntry + k) * 5;
                    const std::uint32_t* e = recs + static_cast<std::size_t>(done + k) * 5;
                    if (std::memcmp(a, e, 20) != 0) {
                        ++stats_.differing;
                        if (differingLogged_++ < 8) {
                            spdlog::error("native: owned compaction: DIFFERING record {} of bucket {}: gpu {{{}, {}, {}, {}, {}}} cpu {{{}, {}, {}, {}, {}}}", k, b, a[0], a[1], a[2], a[3], a[4], e[0], e[1],
                                          e[2], e[3], e[4]);
                        }
                    }
                }
                done += want;
            }
        }
        s.checks.clear();
        s.checkFrame = 0;
    }
}

void OwnedCompact::report() {
    const double now = std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
    if (now - lastReport_ < 5.0) return;
    lastReport_ = now;
    if (stats_.flushes == 0 && stats_.cpuFull == 0) return;
    const Stats& s = stats_;
    const Stats& l = last_;
    spdlog::info("native: owned compaction (5 s): {} frames, {} flushes ({} items, {} buckets, {} dispatches); CPU fallbacks: frame full {}, not ready {}, record args differ from the draw {}; "
                 "checks {} ({} buckets, {} records compared) DIFFERING {} (counts {}), skipped {}; {} KiB GPU",
                 s.frames - l.frames, s.flushes - l.flushes, s.items - l.items, s.buckets - l.buckets, s.dispatches - l.dispatches, s.cpuFull - l.cpuFull, s.cpuNotReady - l.cpuNotReady,
                 s.argsMismatch - l.argsMismatch, s.checks - l.checks, s.checkedBuckets - l.checkedBuckets, s.checkedRecords - l.checkedRecords, s.differing - l.differing,
                 s.countDiffering - l.countDiffering, s.skipped - l.skipped, bytes() / 1024);
    last_ = stats_;
}

}  // namespace rvk::gpuscene
