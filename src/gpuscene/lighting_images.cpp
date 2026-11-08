#include "gpuscene/lighting_images.h"

#include <algorithm>
#include <cfloat>
#include <chrono>
#include <cmath>
#include <cstring>
#include <format>
#include <future>
#include <string>

#include <spdlog/spdlog.h>

#include "d3d9/device.h"
#include "gpuscene/verify.h"
#include "shader/embedded.h"

namespace rvk::gpuscene {

struct LightingImages::Pipeline {
    std::future<std::unique_ptr<rhi::Pipeline>> building;
    std::unique_ptr<rhi::Pipeline> pipe;
    bool failed = false;
};

LightingImages::LightingImages() = default;
LightingImages::~LightingImages() = default;

LightingImages& lightingimages() {
    static LightingImages t;
    return t;
}

namespace {
// distance in units in the last place between two floats (monotone integer mapping; NaN / sign flips count as far apart)
std::uint32_t ulpDistance(float a, float b) {
    std::uint32_t x, y;
    std::memcpy(&x, &a, 4);
    std::memcpy(&y, &b, 4);
    const auto key = [](std::uint32_t v) { return (v & 0x80000000u) != 0 ? 0x80000000u - (v & 0x7fffffffu) : 0x80000000u + v; };
    const std::uint32_t kx = key(x), ky = key(y);
    return kx > ky ? kx - ky : ky - kx;
}

// c14.w (register 0, component 3) is not written by the engine function: left out of every comparison
struct Diff {
    int reg = -1, comp = -1;
    std::uint32_t maxUlp = 0, denormalOnly = 0;
    bool differs = false;
};
Diff compare(const float a[24][4], const float b[24][4], const bool (*written)[4] = nullptr) {
    Diff d;
    for (int r = 0; r < 24; ++r) {
        for (int c = 0; c < 4; ++c) {
            if (written != nullptr && !written[r][c]) continue;
            if (written == nullptr && r == 0 && c == 3) continue;
            if (std::memcmp(&a[r][c], &b[r][c], 4) == 0) continue;
            // two values below the smallest normal float (a static SH word the engine's data holds as a denormal, 7e-44): the GPU's float ALU flushes denormals
            // to zero, so a shader sees 0 for both; not a difference of any register a shader can read
            if (std::fabs(a[r][c]) < FLT_MIN && std::fabs(b[r][c]) < FLT_MIN) {
                ++d.denormalOnly;
                continue;
            }
            const std::uint32_t u = ulpDistance(a[r][c], b[r][c]);
            if (!d.differs) {
                d.reg = r;
                d.comp = c;
            }
            d.differs = true;
            d.maxUlp = std::max(d.maxUlp, u);
        }
    }
    return d;
}
}  // namespace

std::uint32_t LightingImages::note(std::uint32_t illumSlot, const ComposeParams& p, const float drawn[24][4]) {
    if (!active_ || illumSlot == kNoSlot) return kNoEntry;
    std::lock_guard lock(mutex_);
    const std::uint32_t e = entryForLocked(illumSlot, p);
    if (e == kNoEntry) return kNoEntry;
    Entry& en = entries_[e];
    en.drawnFrame = frame_;
    std::memcpy(en.drawn, drawn, sizeof(en.drawn));
    return e;
}

// F2c-1: the entry of (illumination slot, parameters) without the composed registers (a record-derived draw has no drawn registers to check against; the
// image is composed on the GPU as for note()). Same key, same idle clock
std::uint32_t LightingImages::entryFor(std::uint32_t illumSlot, const ComposeParams& p) {
    if (!active_ || illumSlot == kNoSlot) return kNoEntry;
    std::lock_guard lock(mutex_);
    return entryForLocked(illumSlot, p);
}

bool LightingImages::cpuImage(std::uint32_t e, float out[24][4]) {
    std::uint32_t illum;
    ComposeParams p;
    {
        std::lock_guard lock(mutex_);
        if (!active_ || e >= entries_.size() || !entries_[e].live) return false;
        illum = entries_[e].illum;
        p = entries_[e].p;
    }
    return illumtable().compose(illum, p, out);  // (outside this table's lock: the illumination table has its own)
}

std::uint32_t LightingImages::entryForLocked(std::uint32_t illumSlot, const ComposeParams& p) {
    if (entries_.empty()) {
        entries_.resize(kCapacity);
        jobs_.init(kCapacity);
    }
    ++stats_.noted;
    const std::uint64_t key = lightingKey(illumSlot, p);
    std::uint32_t e;
    const auto it = byKey_.find(key);
    if (it != byKey_.end()) {
        e = it->second;
    } else {
        if (!free_.empty()) {
            e = free_.back();
            free_.pop_back();
        } else if (used_ < kCapacity) {
            e = used_++;
        } else {
            ++stats_.overflow;
            return kNoEntry;
        }
        Entry& n = entries_[e];
        n.key = key;
        n.illum = illumSlot;
        n.p = p;
        n.live = true;
        byKey_[key] = e;
        jobs_.shadow()[e] = lightingJob(illumSlot, p);
        jobs_.mark(e);
        ++live_;
        peak_ = std::max(peak_, live_);
        ++stats_.entriesNew;
    }
    entries_[e].seen = frame_;
    return e;
}

void LightingImages::flush(d3d9::Device& dev, bool verify) {
    if (active_) {
        std::lock_guard lock(mutex_);
        flushLocked(dev, verify);
    }
    ++frame_;
}

void LightingImages::flushLocked(d3d9::Device& dev, bool verify) {
    if (used_ == 0) return;
    IllumTable& illums = illumtable();
    if (!gpuFailed_ && !images_) {
        rhi::Device* rd = dev.rhi();
        if (rd == nullptr || illums.illumRef().null() || illums.setRef().null()) return;  // (the illumination table's buffers come first)
        images_ = rd->createBuffer({static_cast<std::uint64_t>(kLightingPad) + static_cast<std::uint64_t>(kCapacity) * kLightingStride,
                                    VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT, rhi::Memory::Device,
                                    "lighting register images"});
        if (!images_ || !jobs_.create(dev, kCapacity, "lighting jobs")) {
            gpuFailed_ = true;
            spdlog::warn("native: lighting images: the GPU buffers could not be created");
            return;
        }
        const auto w = images_->shaderRef(0);
        imagesRef_ = rhi::GpuRef::fromWords(w[0], w[1]);
        jobs_.markAll(used_);
        pipeline_ = std::make_unique<Pipeline>();
        pipeline_->building = std::async(std::launch::async, [rd]() {
            rhi::ComputePipelineDesc d;
            d.cs.spirv = shader::embedded::spirv("gpuscene/lighting_compose.comp");
            d.cs.dxil = shader::embedded::dxilOnDemand("gpuscene/lighting_compose.comp");
            d.name = "lighting images compose";
            return rd->createComputePipeline(d);
        });
        spdlog::info("native: lighting images: buffer {} KiB (pad {} B, {} entries x {} B), window base of entry 0 = entry address - {} B", images_->desc().size / 1024,
                     kLightingPad, kCapacity, kLightingStride, kLightingFirstReg * 16);
    }
    if (gpuFailed_ || !images_) return;
    if (!pipeline_->pipe && !pipeline_->failed && pipeline_->building.valid() &&
        pipeline_->building.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
        pipeline_->pipe = pipeline_->building.get();
        if (!pipeline_->pipe) {
            pipeline_->failed = true;
            spdlog::warn("native: lighting images: the compose pipeline could not be built");
        }
        pipelineReady_ = pipeline_->pipe != nullptr;
    }
    // idle sweep: entries not drawn for kIdleFrames (their jobs go free: the pass skips them)
    if (frame_ % 60 == 0) {
        for (std::uint32_t e = 0; e < used_; ++e) {
            Entry& en = entries_[e];
            if (!en.live || frame_ - en.seen < kIdleFrames) continue;
            en.live = false;
            byKey_.erase(en.key);
            jobs_.shadow()[e] = {0xffffffffu, 0, 0, 0};
            jobs_.mark(e);
            free_.push_back(e);
            --live_;
            ++stats_.entriesFreed;
        }
    }
    // 7.27: the read-back compare against the drawn registers needs this frame's draws to have noted them: a check frame, every 6th of them
    static std::uint64_t verifyTick = 0;
    const bool verifyNow = verify && verifyFrame() && verifyTick++ % 6 == 0;
    jobs_.flush(dev, verifyNow, used_);
    jobs_.collect(dev.game(), "lighting jobs");
    collect(dev);
    if (!pipelineReady_) {
        ++stats_.passSkipped;
        report();
        return;
    }
    // the compose pass: in the upload list, after the table flushes of this present (the same list, memory barrier between), before the frame's draws
    rhi::CommandList& list = dev.uploadList();
    rhi::GameDevice& game = dev.game();
    const bool timed = game.timestampsEnabled();
    if (timed) game.timestampMark(list, game.frameSlot(), rhi::TimestampStream::Upload, "uploads before the lighting images");
    list.memoryBarrier(VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
                       VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_READ_BIT);
    list.bufferBarrier(*images_, rhi::BufferAccess::ShaderWrite);
    struct Push {
        std::uint32_t illums[2], sets[2], jobs[2], images[2];
        std::uint32_t count, pad[3];
    } push{};
    static_assert(sizeof(Push) == 48);
    const auto ir = illums.illumRef().pair();
    const auto sr = illums.setRef().pair();
    const auto jr = jobs_.ref().pair();
    const auto wr = images_->shaderRefRw(0);
    push.illums[0] = ir[0];
    push.illums[1] = ir[1];
    push.sets[0] = sr[0];
    push.sets[1] = sr[1];
    push.jobs[0] = jr[0];
    push.jobs[1] = jr[1];
    push.images[0] = wr[0];
    push.images[1] = wr[1];
    push.count = used_;
    list.bindPipeline(*pipeline_->pipe);
    list.setPushConstants(&push, sizeof(push));
    list.dispatch((used_ + 63) / 64, 1, 1);
    list.bufferBarrier(*images_, rhi::BufferAccess::ShaderRead);
    if (timed) game.timestampMark(list, game.frameSlot(), rhi::TimestampStream::Upload, "lighting images compose");
    ++stats_.passes;
    stats_.composed += live_;
    if (verifyNow) startCheck(dev, used_);
    report();
}

void LightingImages::startCheck(d3d9::Device& dev, std::uint32_t used) {
    rhi::GameDevice& game = dev.game();
    Pending& p = pending_[game.frameSlot() % rhi::kFramesInFlight];
    if (p.busy) return;
    const std::uint64_t bytes = static_cast<std::uint64_t>(used) * kLightingStride;
    if (!p.buffer) {
        p.buffer = dev.rhi()->createBuffer({static_cast<std::uint64_t>(kCapacity) * kLightingStride, VK_BUFFER_USAGE_TRANSFER_DST_BIT, rhi::Memory::Readback,
                                            "lighting images verify"});
        if (!p.buffer) return;
    }
    rhi::CommandList& list = dev.uploadList();
    list.copyBuffer(*p.buffer, 0, *images_, kLightingPad, bytes);
    list.bufferBarrier(*images_, rhi::BufferAccess::ShaderRead);
    p.expect.clear();
    IllumTable& illums = illumtable();
    for (std::uint32_t e = 0; e < used; ++e) {
        const Entry& en = entries_[e];
        if (!en.live) continue;
        Pending::Expect x;
        x.entry = e;
        std::memset(x.cpu, 0, sizeof(x.cpu));
        int why = 0;
        x.hasCpu = illums.compose(en.illum, en.p, x.cpu, &why);
        if (!x.hasCpu) ++(why == 1 ? stats_.noRecord : stats_.noSet);
        // the registers the composition does not write (c14.w, the lights beyond the pass's count) differ between two different priors: the draw's register file
        // holds stale values there, the image zeros; they are left out of the comparison with the drawn registers
        float probe[24][4];
        for (auto& r : probe) r[0] = r[1] = r[2] = r[3] = 1.0f;
        if (x.hasCpu) illums.compose(en.illum, en.p, probe);
        for (int r = 0; r < 24; ++r) {
            for (int c = 0; c < 4; ++c) x.written[r][c] = x.hasCpu && std::memcmp(&x.cpu[r][c], &probe[r][c], 4) == 0;
        }
        x.hasDrawn = en.drawnFrame == frame_;  // (this CPU frame's draws carry the stamp frame_)
        if (x.hasDrawn) std::memcpy(x.drawn, en.drawn, sizeof(x.drawn));
        p.expect.push_back(x);
    }
    p.frameValue = game.frameValue();
    p.busy = true;
    ++stats_.checks;
}

void LightingImages::collect(d3d9::Device& dev) {
    rhi::GameDevice& game = dev.game();
    for (Pending& p : pending_) {
        if (!p.busy || game.completedFrame() < p.frameValue) continue;
        p.busy = false;
        p.buffer->invalidate();
        const auto* got = static_cast<const std::uint8_t*>(p.buffer->map());
        for (const Pending::Expect& x : p.expect) {
            float img[24][4];
            std::memcpy(img, got + static_cast<std::size_t>(x.entry) * kLightingStride, sizeof(img));
            ++stats_.entriesChecked;
            if (!x.hasCpu) {
                ++stats_.uncomparable;
                if (x.hasDrawn) ++stats_.drawnUncomparable;
                continue;
            }
            const Diff dc = compare(img, x.cpu);
            stats_.denormalOnly += dc.denormalOnly;
            if (dc.differs) {
                ++stats_.cpuDiff;
                stats_.maxUlpCpu = std::max(stats_.maxUlpCpu, dc.maxUlp);
                if (differingLogged_ < 8) {
                    ++differingLogged_;
                    spdlog::error("native: lighting images: DIFFERING entry {} (vs CPU compose): c{}.{} gpu {:g} cpu {:g} (max {} ulp)", x.entry, 14 + dc.reg, "xyzw"[dc.comp],
                                  img[dc.reg][dc.comp], x.cpu[dc.reg][dc.comp], dc.maxUlp);
                }
            }
            if (x.hasDrawn) {
                ++stats_.drawnCompared;
                const Diff dd = compare(img, x.drawn, x.written);
                stats_.denormalOnly += dd.denormalOnly;
                if (dd.differs) {
                    ++stats_.drawnDiff;
                    stats_.maxUlpDrawn = std::max(stats_.maxUlpDrawn, dd.maxUlp);
                    if (differingLogged_ < 8) {
                        ++differingLogged_;
                        spdlog::error("native: lighting images: DIFFERING entry {} (vs drawn registers): c{}.{} gpu {:g} drawn {:g} (max {} ulp)", x.entry, 14 + dd.reg,
                                      "xyzw"[dd.comp], img[dd.reg][dd.comp], x.drawn[dd.reg][dd.comp], dd.maxUlp);
                    }
                }
            }
        }
        p.expect.clear();
    }
}

void LightingImages::report() {
    const double now = std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
    if (now - lastReport_ < 5.0) return;
    lastReport_ = now;
    const std::uint64_t frames = std::max<std::uint64_t>(frame_ - lastFrame_, 1);
    spdlog::info(
        "native: lighting images (5 s): entries {} live (peak {}, slots {}), new {}, freed {}, overflow {}; compose passes {} ({:.1f} entries per pass; {} skipped, pipeline {}); "
        "draws noted {}; read-back checks {} ({} entries, {} not comparable: {} record freed, {} no light set, {} of them drawn this frame): DIFFERING {} (vs CPU compose, max {} ulp) / {} (vs drawn registers, {} compared, max {} ulp), denormal-only words {}; "
        "jobs: {} uploads, read-backs DIFFERING {}",
        live_, peak_, used_, stats_.entriesNew, stats_.entriesFreed, stats_.overflow, stats_.passes,
        static_cast<double>(stats_.composed - lastComposed_) / static_cast<double>(std::max<std::uint64_t>(stats_.passes - lastPasses_, 1)), stats_.passSkipped,
        pipelineReady_ ? "ready" : "pending", stats_.noted, stats_.checks, stats_.entriesChecked, stats_.uncomparable, stats_.noRecord, stats_.noSet, stats_.drawnUncomparable, stats_.cpuDiff, stats_.maxUlpCpu, stats_.drawnDiff,
        stats_.drawnCompared, stats_.maxUlpDrawn, stats_.denormalOnly, jobs_.stats().slotsUploaded, jobs_.stats().verifyBad);
    (void)frames;
    lastFrame_ = frame_;
    lastComposed_ = stats_.composed;
    lastPasses_ = stats_.passes;
}

}  // namespace rvk::gpuscene
