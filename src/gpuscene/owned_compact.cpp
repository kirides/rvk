#include "gpuscene/owned_compact.h"

#include <algorithm>
#include <array>
#include <unordered_map>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <format>
#include <future>
#include <string>

#include <spdlog/spdlog.h>

#include "d3d9/device.h"
#include "core/cluster_cull.h"
#include "gpuscene/cluster_table.h"
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
        for (Job& j : s.jobs) {
            for (CullItem& ci : j.culls) dev.cullCheckFinish(ci);  // (check frames: the exact verdicts from the end-of-frame twins)
        }
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
            std::uint32_t countsBase, argsBase, slotCount, viewSlots;
            std::uint32_t clusters[2], views[2];
            std::uint32_t clusterRows, pad[3];
        } push{};
        static_assert(sizeof(Push) == 80);
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
            // F5-2: the tables of a job with the cull (null otherwise: the shader drops the cull flag)
            const bool cull = (j.cullFlags & 1u) != 0 && !j.views.null() && !j.clusters.null();
            const auto cr = cull ? j.clusters.pair() : std::array<std::uint32_t, 2>{};
            const auto vr = cull ? j.views.pair() : std::array<std::uint32_t, 2>{};
            push.clusters[0] = cr[0];
            push.clusters[1] = cr[1];
            push.views[0] = vr[0];
            push.views[1] = vr[1];
            push.viewSlots = cull ? j.viewSlots : 0;
            push.clusterRows = cull ? ClusterStore::kRowCapacity : 0;
            if (cull) ++stats_.cullJobs;
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
            const std::uint32_t* meta = j.expect.data();  // per bucket: the first argument record (relative to the flush's), the expected plain records
            const std::uint32_t* recs = meta + 2 * static_cast<std::size_t>(j.buckets);
            std::unordered_map<std::uint32_t, const CullItem*> culled;
            for (const CullItem& ci : j.culls) culled.emplace(ci.pos, &ci);
            std::uint32_t done = 0;
            std::uint64_t after = 0;
            for (std::uint32_t b = 0; b < j.buckets; ++b) {
                const std::uint32_t argStart = meta[2 * b], want = meta[2 * b + 1];
                const std::uint32_t have = got[j.range.firstCount + b];
                ++stats_.checkedBuckets;
                const std::uint32_t* base = got + kArgsBase / 4 + static_cast<std::size_t>(j.range.firstArg + argStart) * 5;
                std::uint32_t plain = 0;
                std::vector<const CullItem*> seen;
                for (std::uint32_t r = 0; r < have;) {
                    const std::uint32_t* a = base + static_cast<std::size_t>(r) * 5;
                    const auto it = culled.find(a[4]);
                    if (it != culled.end() && it->second->bucket == b) {
                        const CullItem& ci = *it->second;
                        seen.push_back(&ci);
                        std::vector<std::pair<std::uint32_t, std::uint32_t>> runs;
                        for (; r < have; ++r) {
                            const std::uint32_t* q = base + static_cast<std::size_t>(r) * 5;
                            if (q[4] != ci.pos) break;
                            ++stats_.runs;
                            after += q[0];
                            const bool good = q[1] == 1 && static_cast<std::int32_t>(q[3]) == static_cast<std::int32_t>(ci.vertexOffset) && q[0] != 0 && q[2] >= ci.firstIndex &&
                                              static_cast<std::uint64_t>(q[2]) + q[0] <= static_cast<std::uint64_t>(ci.firstIndex) + ci.indexCount;
                            if (!good) {
                                ++stats_.badRuns;
                                ++stats_.differing;
                                if (differingLogged_++ < 8) {
                                    spdlog::error("native: owned compaction: DIFFERING cull run of entry {}: {{{}, {}, {}, {}, {}}} range [{}, +{}) vertex offset {}", ci.pos, q[0], q[1], q[2], q[3], q[4], ci.firstIndex,
                                                  ci.indexCount, ci.vertexOffset);
                                }
                            }
                            runs.emplace_back(q[2], q[0]);
                        }
                        evaluate(j, ci, runs);
                        continue;
                    }
                    ++stats_.checkedRecords;
                    const std::uint32_t* e = recs + static_cast<std::size_t>(done + plain) * 5;
                    if (plain >= want || std::memcmp(a, e, 20) != 0) {
                        ++stats_.differing;
                        if (differingLogged_++ < 8) {
                            if (plain < want) {
                                spdlog::error("native: owned compaction: DIFFERING record {} of bucket {}: gpu {{{}, {}, {}, {}, {}}} cpu {{{}, {}, {}, {}, {}}}", r, b, a[0], a[1], a[2], a[3], a[4], e[0], e[1], e[2],
                                              e[3], e[4]);
                            } else {
                                spdlog::error("native: owned compaction: DIFFERING extra record {} of bucket {}: gpu {{{}, {}, {}, {}, {}}}, {} expected", r, b, a[0], a[1], a[2], a[3], a[4], want);
                            }
                        }
                    }
                    after += a[0];
                    ++plain;
                    ++r;
                }
                if (plain != want) {
                    ++stats_.countDiffering;
                    ++stats_.differing;
                    if (differingLogged_++ < 8) spdlog::error("native: owned compaction: DIFFERING count of bucket {}: gpu {} plain records, cpu {} (total gpu {})", j.range.firstCount + b, plain, want, have);
                }
                // culled items the GPU wrote no record for: everything of them must be outside
                for (const CullItem& ci : j.culls) {
                    if (ci.bucket == b && std::find(seen.begin(), seen.end(), &ci) == seen.end()) {
                        ++stats_.emptyItems;
                        evaluate(j, ci, {});
                    }
                }
                done += want;
            }
            const std::uint32_t kind = std::min(j.viewKind, 2u);
            stats_.idxBefore[kind] += j.fullIndices;
            stats_.idxAfter[kind] += after;
            ++stats_.idxFlushes[kind];
        }
        s.checks.clear();
        s.checkFrame = 0;
    }
}

// one culled item: the GPU's runs against the CPU's exact verdict
void OwnedCompact::evaluate(const Job& j, const CullItem& ci, const std::vector<std::pair<std::uint32_t, std::uint32_t>>& runs) {
    ++stats_.cullChecked;
    if (!ci.exact) {
        ++stats_.inexact;
        return;
    }
    if (runs.size() == 1 && runs[0].first == ci.firstIndex && runs[0].second == ci.indexCount) ++stats_.wholeRange;
    std::uint32_t falseNeg = 0;
    for (std::size_t c = 0; c < ci.visible.size(); ++c) {
        bool covered = false;
        for (const auto& r : runs) {
            covered = covered || (ci.clusterFirst[c] >= r.first && static_cast<std::uint64_t>(ci.clusterFirst[c]) + ci.clusterCount[c] <= static_cast<std::uint64_t>(r.first) + r.second);
        }
        ++stats_.cullClusters;
        stats_.cullExactVisible += ci.visible[c];
        stats_.cullCovered += covered ? 1 : 0;
        if (ci.visible[c] != 0 && !covered) {
            ++stats_.falseNegatives;
            ++falseNeg;
            if (differingLogged_ < 8) {
                clustercull::View cv;
                clustercull::makeView(reinterpret_cast<const float(*)[4]>(ci.refRegs), ci.refWorld, clustercull::kMargin, true, cv);
                const bool refOut = clustercull::sphereOutside(cv, &ci.sphere[4 * c], ci.sphere[4 * c + 3]);
                float d[6];
                for (int p = 0; p < 6; ++p) d[p] = (cv.plane[p][0] * ci.sphere[4 * c] + cv.plane[p][1] * ci.sphere[4 * c + 1] + cv.plane[p][2] * ci.sphere[4 * c + 2] + cv.plane[p][3]) / std::max(cv.len[p], 1e-9f) + ci.sphere[4 * c + 3];
                spdlog::error("native: owned compaction: false negative cluster {} of entry {} view {}: sphere ({:.1f} {:.1f} {:.1f}) r {:.1f}, exact slack {:.3f}, reference (the shader's formula) says {}, its slacks {:.2f} {:.2f} {:.2f} {:.2f} {:.2f}; "
                              "world rows [{:.4g} {:.4g} {:.4g} {:.4g} | {:.4g} {:.4g} {:.4g} {:.4g} | {:.4g} {:.4g} {:.4g} {:.4g}]; c3 row {:.5g} {:.5g} {:.5g} {:.5g}",
                              c, ci.pos, ci.viewSlot, ci.sphere[4 * c], ci.sphere[4 * c + 1], ci.sphere[4 * c + 2], ci.sphere[4 * c + 3], ci.slack[c], refOut ? "OUTSIDE" : "inside", d[0], d[1], d[2], d[3], d[4],
                              ci.refWorld[0], ci.refWorld[1], ci.refWorld[2], ci.refWorld[3], ci.refWorld[4], ci.refWorld[5], ci.refWorld[6], ci.refWorld[7], ci.refWorld[8], ci.refWorld[9], ci.refWorld[10], ci.refWorld[11],
                              ci.refRegs[12], ci.refRegs[13], ci.refRegs[14], ci.refRegs[15]);
            }
        } else if (ci.visible[c] == 0 && covered) {
            ++stats_.extras;
        }
    }
    if (falseNeg != 0 && differingLogged_++ < 8) {
        spdlog::error("native: owned compaction: FALSE NEGATIVE cull of entry {} (view kind {}): {} of {} clusters are inside the frustum but were not drawn ({} runs)", ci.pos, j.viewKind, falseNeg,
                      ci.visible.size(), runs.size());
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
    if (s.cullItems != 0 || s.cullChecked != 0) {
        const auto pct = [](std::uint64_t a, std::uint64_t b) { return b != 0 ? 100.0 * static_cast<double>(a) / static_cast<double>(b) : 0.0; };
        static const char* kKind[3] = {"main z", "main solid", "other"};
        std::string tri;
        for (int k = 0; k < 3; ++k) {
            const std::uint64_t f = s.idxFlushes[k] - l.idxFlushes[k];
            if (f == 0) continue;
            const std::uint64_t b = s.idxBefore[k] - l.idxBefore[k], a = s.idxAfter[k] - l.idxAfter[k];
            tri += std::format(" [{}: {} flushes, {:.0f} -> {:.0f} triangles per flush ({:.1f} %)]", kKind[k], f, static_cast<double>(b) / 3.0 / static_cast<double>(f), static_cast<double>(a) / 3.0 / static_cast<double>(f),
                               pct(a, b));
        }
        spdlog::info("native: cluster cull (5 s): {} culled-capable items in {} flushes; checks: {} items, {} clusters ({} inside by the exact test, {} drawn), FALSE NEGATIVES {}, extras {} ({:.1f} % of the clusters), "
                     "whole-range fallbacks {}, items with no record {}, runs {} ({} bad), exact evaluation impossible {};{}",
                     s.cullItems - l.cullItems, s.cullJobs - l.cullJobs, s.cullChecked - l.cullChecked, s.cullClusters - l.cullClusters, s.cullExactVisible - l.cullExactVisible, s.cullCovered - l.cullCovered,
                     s.falseNegatives - l.falseNegatives, s.extras - l.extras, pct(s.extras - l.extras, s.cullClusters - l.cullClusters), s.wholeRange - l.wholeRange, s.emptyItems - l.emptyItems,
                     s.runs - l.runs, s.badRuns - l.badRuns, s.inexact - l.inexact, tri);
    }
    last_ = stats_;
}

}  // namespace rvk::gpuscene
