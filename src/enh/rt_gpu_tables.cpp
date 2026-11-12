#include "enh/rt_gpu_tables.h"

#include <algorithm>
#include <cstring>
#include <format>

#include <spdlog/spdlog.h>

#include "d3d9/device.h"
#include "gpuscene/verify.h"
#include "shader/embedded.h"
#include "shader/pass_push.h"

namespace rvk::enh::rtbuild {

namespace abi = shader::abi;

namespace {
constexpr std::uint64_t kVerifyEvery = 240;  // frames between the tables' read-backs (GpuTable verify)
}  // namespace

// ---- SlotPool ----

std::uint32_t SlotPool::alloc() {
    std::uint32_t s = kNone;
    if (!free_.empty()) {
        s = free_.back();
        free_.pop_back();
    } else if (top_ < cap_) {
        s = top_++;
    } else {
        ++full;
        return kNone;
    }
    ++live_;
    return s;
}

void SlotPool::release(std::uint32_t slot, std::uint64_t frame) {
    if (slot >= cap_) return;
    if (live_ > 0) --live_;
    pending_.push_back({slot, frame + rhi::kFramesInFlight + 1});
}

void SlotPool::tick(std::uint64_t frame) {
    for (std::size_t i = 0; i < pending_.size();) {
        if (pending_[i].at <= frame) {
            free_.push_back(pending_[i].slot);
            pending_[i] = pending_.back();
            pending_.pop_back();
        } else {
            ++i;
        }
    }
}

// ---- GpuBuild ----

bool GpuBuild::ready(d3d9::Device& dev, std::uint32_t tlasCapacity) {
    if (failed_) return false;
    rhi::Device* rd = dev.rhi();
    if (rd == nullptr) return false;
    if (!created_) {
        created_ = true;
        bool ok = inputs.create(dev, kInputCap, "rt build inputs") && blas.create(dev, kBlasCap, "rt build blas rows") && materials.create(dev, kMatCap, "rt build material rows");
        outCap_ = std::max(tlasCapacity, 1u);
        for (std::uint32_t s = 0; s < rhi::kFramesInFlight && ok; ++s) {
            outInst_[s] = rd->createBuffer({static_cast<std::uint64_t>(outCap_) * sizeof(VkAccelerationStructureInstanceKHR),
                                            VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT, rhi::Memory::Device, "rt build instances"});
            outRec_[s] = rd->createBuffer({static_cast<std::uint64_t>(outCap_) * sizeof(abi::RtGeometryRecord), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                                           rhi::Memory::Device, "rt build records"});
            ok = outInst_[s] != nullptr && outRec_[s] != nullptr;
        }
        if (!ok) {
            spdlog::error("rt: gpu build: the tables / output buffers could not be created: the instances stay CPU-written this session");
            failed_ = true;
            release();
            failed_ = true;
            return false;
        }
        inputSlots.reset(kInputCap);
        blasSlots.reset(kBlasCap);
        matSlots.reset(kMatCap);
        // the CPU twins start zero like the new buffers (zeroTo below): a twin left from an earlier session (setting off -> on) would hide every first write
        std::fill(inputs.shadow().begin(), inputs.shadow().end(), InstanceInput{});
        std::fill(blas.shadow().begin(), blas.shadow().end(), BlasRow{});
        std::fill(materials.shadow().begin(), materials.shadow().end(), MaterialRow{});
        zeroed_ = false;
        spdlog::info("rt: gpu build: tables created (inputs {} x {} B, BLAS rows {} x {} B, material rows {} x {} B), outputs {} instances x {} frame slots", kInputCap,
                     sizeof(InstanceInput), kBlasCap, sizeof(BlasRow), kMatCap, sizeof(MaterialRow), outCap_, rhi::kFramesInFlight);
    }
    if (tlasCapacity > outCap_) return false;  // (the TLAS cap never grows: a bound)
    if (pipe_) return true;
    if (pipeFailed_) return false;
    if (!pipeStarted_) {
        pipeStarted_ = true;
        pipeBuild_ = std::async(std::launch::async, [rd]() {
            rhi::ComputePipelineDesc d;
            d.cs.spirv = shader::embedded::spirv("rt/rt_instance_build.comp");
            d.cs.dxil = shader::embedded::dxilOnDemand("rt/rt_instance_build.comp");
            d.name = "rt.instance_build";
            return rd->createComputePipeline(d);
        });
        return false;
    }
    if (!pipeBuild_.valid() || pipeBuild_.wait_for(std::chrono::seconds(0)) != std::future_status::ready) return false;
    pipe_ = pipeBuild_.get();
    if (!pipe_) {
        spdlog::error("rt: gpu build: the compute pipeline failed: the instances stay CPU-written");
        pipeFailed_ = true;
        return false;
    }
    spdlog::info("rt: gpu build: compute pipeline ready");
    return true;
}

void GpuBuild::release() {
    // (the pipeline stays: a command list being recorded / in flight may reference it, and D3D12 does not defer a pipeline's release; it goes with the scene)
    inputs.release();
    blas.release();
    materials.release();
    for (std::uint32_t s = 0; s < rhi::kFramesInFlight; ++s) {
        outInst_[s].reset();
        outRec_[s].reset();
        cmp_[s] = Cmp{};
    }
    inputSlots.reset(kInputCap);
    blasSlots.reset(kBlasCap);
    matSlots.reset(kMatCap);
    created_ = failed_ = zeroed_ = false;
    outCap_ = 0;
}

bool GpuBuild::upload(d3d9::Device& dev, rhi::CommandList& cl, std::uint64_t frame) {
    if (!zeroed_) {
        // (the read-backs cover slots that were never written: their CPU shadow is zero)
        inputs.zeroTo(cl);
        blas.zeroTo(cl);
        materials.zeroTo(cl);
        zeroed_ = true;
    }
    const bool verify = gpuscene::periodic(frame, kVerifyEvery);
    bool ok = inputs.flushTo(dev, cl, verify, inputSlots.top());
    ok = blas.flushTo(dev, cl, verify, blasSlots.top()) && ok;
    ok = materials.flushTo(dev, cl, verify, matSlots.top()) && ok;
    inputs.collect(dev.game(), "rt build inputs");
    blas.collect(dev.game(), "rt build blas rows");
    materials.collect(dev.game(), "rt build material rows");
    return ok;
}

bool GpuBuild::dispatch(d3d9::Device& dev, rhi::CommandList& cl, std::uint32_t slot, const Header& hIn) {
    if (!pipe_ || slot >= rhi::kFramesInFlight || !outInst_[slot] || !outRec_[slot] || hIn.count > outCap_) return false;
    if (hIn.count == 0) return true;  // (nothing to write: no zero-size dispatch; the TLAS is built over 0 instances)
    const rhi::RingAlloc t = dev.ring(sizeof(Header), 16);
    if (t.ptr == nullptr) return false;
    Header h = hIn;
    const auto oi = outInst_[slot]->shaderRefRw(0);
    const auto orr = outRec_[slot]->shaderRefRw(0);
    h.outInst[0] = oi[0];
    h.outInst[1] = oi[1];
    h.outRec[0] = orr[0];
    h.outRec[1] = orr[1];
    const rhi::GpuRef ir = inputs.ref(), br = blas.ref(), mr = materials.ref();
    h.inputs[0] = ir.low();
    h.inputs[1] = ir.high();
    h.blas[0] = br.low();
    h.blas[1] = br.high();
    h.materials[0] = mr.low();
    h.materials[1] = mr.high();
    h.inputCap = kInputCap;
    h.blasCap = kBlasCap;
    h.matCap = kMatCap;
    std::memcpy(t.ptr, &h, sizeof(h));
    // the previous users of the slot's outputs (the TLAS build and the ray queries of kFramesInFlight frames ago) and the table copies above -> the writes
    cl.memoryBarrier(VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                     VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT);
    cl.bufferBarrier(*outInst_[slot], rhi::BufferAccess::ShaderWrite);
    cl.bufferBarrier(*outRec_[slot], rhi::BufferAccess::ShaderWrite);
    cl.bindPipeline(*pipe_);
    abi::SkinPush push{};
    push.h.frame = 0;
    push.table[0] = t.ref.low();
    push.table[1] = t.ref.high();
    cl.setPushConstants(&push, sizeof(push));
    cl.dispatch((h.count + 63) / 64, 1, 1);
    // the outputs -> the TLAS build (instances) and every ray query / hit shading of the frame (records)
    cl.memoryBarrier(VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
                     VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_BUILD_BIT_KHR | VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                     VK_ACCESS_2_SHADER_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_TRANSFER_READ_BIT);
    cl.bufferBarrier(*outRec_[slot], rhi::BufferAccess::ShaderRead);
    ++stats.frames;
    stats.instances += h.count;
    stats.literals += h.literals;
    totalInstances += h.count;
    return true;
}

void GpuBuild::queueCompare(d3d9::Device& dev, rhi::CommandList& cl, std::uint32_t slot, std::vector<VkAccelerationStructureInstanceKHR>&& refInst,
                            std::vector<abi::RtGeometryRecord>&& refRec) {
    if (slot >= rhi::kFramesInFlight || !outInst_[slot] || !outRec_[slot]) return;
    Cmp& c = cmp_[slot];
    if (c.busy || refInst.size() != refRec.size() || refInst.empty() || refInst.size() > outCap_) {
        ++stats.compareSkipped;
        return;
    }
    const std::uint64_t ib = refInst.size() * sizeof(VkAccelerationStructureInstanceKHR), rb = refRec.size() * sizeof(abi::RtGeometryRecord);
    if (!c.readback) {
        c.readback = dev.rhi()->createBuffer({static_cast<std::uint64_t>(outCap_) * (sizeof(VkAccelerationStructureInstanceKHR) + sizeof(abi::RtGeometryRecord)),
                                              VK_BUFFER_USAGE_TRANSFER_DST_BIT, rhi::Memory::Readback, "rt build compare"});
        if (!c.readback) return;
    }
    // (after the TLAS build: the instances were read by it; both go back to the state their readers need)
    cl.memoryBarrier(VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_BUILD_BIT_KHR, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
                     VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_READ_BIT);
    cl.copyBuffer(*c.readback, 0, *outInst_[slot], 0, ib);
    cl.copyBuffer(*c.readback, static_cast<std::uint64_t>(outCap_) * sizeof(VkAccelerationStructureInstanceKHR), *outRec_[slot], 0, rb);
    cl.bufferBarrier(*outRec_[slot], rhi::BufferAccess::ShaderRead);
    cl.bufferBarrier(*outInst_[slot], rhi::BufferAccess::ShaderRead);
    c.inst = std::move(refInst);
    c.rec = std::move(refRec);
    c.frameValue = dev.frameValue();
    c.busy = true;
}

void GpuBuild::collect(d3d9::Device& dev) {
    for (Cmp& c : cmp_) {
        if (!c.busy || dev.game().completedFrame() < c.frameValue) continue;
        c.busy = false;
        c.readback->invalidate();
        const auto* base = static_cast<const std::uint8_t*>(c.readback->map());
        if (base == nullptr) continue;
        const auto* gi = reinterpret_cast<const VkAccelerationStructureInstanceKHR*>(base);
        const auto* gr = reinterpret_cast<const abi::RtGeometryRecord*>(base + static_cast<std::uint64_t>(outCap_) * sizeof(VkAccelerationStructureInstanceKHR));
        std::uint64_t badI = 0, badR = 0;
        for (std::size_t k = 0; k < c.inst.size(); ++k) {
            const bool di = std::memcmp(&gi[k], &c.inst[k], sizeof(VkAccelerationStructureInstanceKHR)) != 0;
            const bool dr = std::memcmp(&gr[k], &c.rec[k], sizeof(abi::RtGeometryRecord)) != 0;
            if ((di || dr) && badI + badR < 3) {
                const auto* a = reinterpret_cast<const std::uint32_t*>(di ? static_cast<const void*>(&gi[k]) : static_cast<const void*>(&gr[k]));
                const auto* b = reinterpret_cast<const std::uint32_t*>(di ? static_cast<const void*>(&c.inst[k]) : static_cast<const void*>(&c.rec[k]));
                const std::size_t words = di ? 16 : 28;
                std::size_t w = 0;
                while (w < words && a[w] == b[w]) ++w;
                spdlog::error("rt: gpu build: frame {} instance {} of {}: the GPU's {} differs from the CPU path's at word {} (gpu {:#010x}, cpu {:#010x})", c.frameValue, k,
                              c.inst.size(), di ? "instance" : "record", w, w < words ? a[w] : 0u, w < words ? b[w] : 0u);
            }
            badI += di ? 1 : 0;
            badR += dr ? 1 : 0;
        }
        ++stats.compares;
        stats.comparedInstances += c.inst.size();
        stats.differingInstances += badI;
        stats.differingRecords += badR;
        totalDiffering += badI + badR;
        c.inst.clear();
        c.rec.clear();
    }
}

void GpuBuild::log(double frames) {
    const BuildStats& s = stats;
    const BuildStats& l = last_;
    const double f = std::max(1.0, frames);
    const auto& it = inputs.stats();
    const auto& bt = blas.stats();
    const auto& mt = materials.stats();
    spdlog::info("rt: gpu build (5 s): {} GPU-built frames ({:.0f} instances / frame, {:.1f} literal), {} CPU-path frames, {} fallback frames (upload failed {}, table full {}, not ready "
                 "{}); writes per frame: inputs {:.1f}, BLAS rows {:.1f}, material rows {:.1f} (material evaluations {:.1f}); slots live: inputs {} (top {}), BLAS {} (top {}), "
                 "materials {} (top {}); instance + record CPU {:.3f} ms / GPU-built frame, {:.3f} ms / CPU-path frame; check frames: {} compared ({} instances), DIFFERING {} instances {} records (skipped {}); "
                 "material rows checked {} STALE {}; table read-backs {} / {} / {} DIFFERING {} / {} / {}",
                 s.frames - l.frames, static_cast<double>(s.instances - l.instances) / std::max<double>(1.0, static_cast<double>(s.frames - l.frames)),
                 static_cast<double>(s.literals - l.literals) / std::max<double>(1.0, static_cast<double>(s.frames - l.frames)), s.cpuFrames - l.cpuFrames,
                 s.fallbackFrames - l.fallbackFrames, s.uploadFailed - l.uploadFailed, s.refusedFull - l.refusedFull, s.notReady - l.notReady,
                 static_cast<double>(s.inputWrites - l.inputWrites) / f, static_cast<double>(s.blasWrites - l.blasWrites) / f, static_cast<double>(s.matWrites - l.matWrites) / f,
                 static_cast<double>(s.matEvals - l.matEvals) / f, inputSlots.live(), inputSlots.top(), blasSlots.live(), blasSlots.top(), matSlots.live(), matSlots.top(),
                 (s.cpuMs - l.cpuMs) / std::max<double>(1.0, static_cast<double>(s.frames - l.frames)), (s.cpuMsCpuArm - l.cpuMsCpuArm) / std::max<double>(1.0, static_cast<double>(s.cpuFrames - l.cpuFrames)), s.compares - l.compares, s.comparedInstances - l.comparedInstances, s.differingInstances - l.differingInstances,
                 s.differingRecords - l.differingRecords, s.compareSkipped - l.compareSkipped, s.matChecked - l.matChecked, s.matStale - l.matStale, it.verifyDone, bt.verifyDone,
                 mt.verifyDone, it.verifyBad, bt.verifyBad, mt.verifyBad);
    last_ = s;
}

}  // namespace rvk::enh::rtbuild
