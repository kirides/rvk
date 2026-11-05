#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <format>
#include <fstream>
#include <string>

#include <spdlog/spdlog.h>

#include "core/paths.h"
#include "enh/debug_view.h"
#include "enh/gi.h"
#include "enh/rhi_scope.h"
#include "shader/embedded.h"

// The reference path tracer of the GI (gi.reference*; shaders/gi/gi_reference.comp.hlsl, docs/research/path-tracing.md): ground truth for
// the DDGI probes + screen probes, progressively accumulated while the camera and the light stand still, with the view pass that shows
// it in the game and the dumps (float16 .npy planes in bin\rvk\gi_ref) the comparison tool (tools/gi_reference.py) reads.

namespace rvk::enh {

namespace {

void split(rhi::GpuRef a, std::uint32_t* out) {
    out[0] = a.low();
    out[1] = a.high();
}
void split(const std::array<std::uint32_t, 2>& a, std::uint32_t* out) {
    out[0] = a[0];
    out[1] = a[1];
}

// the same on values quantised to 1 / scale (slowly drifting light: weather / time of day changes a hair every few frames)
std::uint64_t hashFloatsQ(const float* v, std::size_t n, float scale, std::uint64_t h = 1469598103934665603ull) {
    for (std::size_t i = 0; i < n; ++i) {
        const std::int32_t q = static_cast<std::int32_t>(std::floor(v[i] * scale));
        const auto* b = reinterpret_cast<const std::uint8_t*>(&q);
        for (std::size_t k = 0; k < sizeof(q); ++k) {
            h ^= b[k];
            h *= 1099511628211ull;
        }
    }
    return h;
}

std::uint64_t hashFloats(const float* v, std::size_t n, std::uint64_t h = 1469598103934665603ull) {
    const auto* b = reinterpret_cast<const std::uint8_t*>(v);
    for (std::size_t i = 0; i < n * sizeof(float); ++i) {
        h ^= b[i];
        h *= 1099511628211ull;
    }
    return h;
}

std::uint16_t toHalf(float f) {
    std::uint32_t x;
    std::memcpy(&x, &f, 4);
    const std::uint32_t sign = (x >> 16) & 0x8000u;
    const std::uint32_t ex = (x >> 23) & 0xffu;
    std::uint32_t m = x & 0x7fffffu;
    if (ex == 0xffu) return static_cast<std::uint16_t>(sign | 0x7c00u | (m != 0 ? 0x200u : 0u));
    const int e = static_cast<int>(ex) - 127 + 15;
    if (e >= 31) return static_cast<std::uint16_t>(sign | 0x7bffu);  // (beyond 65504: clamped)
    if (e <= 0) {
        if (e < -10) return static_cast<std::uint16_t>(sign);
        m |= 0x800000u;
        const int shift = 14 - e;
        std::uint32_t half = m >> shift;
        if (((m >> (shift - 1)) & 1u) != 0) ++half;
        return static_cast<std::uint16_t>(sign | half);
    }
    std::uint32_t h = sign | (static_cast<std::uint32_t>(e) << 10) | (m >> 13);
    if ((m & 0x1000u) != 0) ++h;
    return static_cast<std::uint16_t>(h);
}

// samples at which the accumulation is dumped (the luminance-only plane for the convergence test): 64, 128, ... below the maximum
constexpr std::uint32_t kFirstDump = 64;

}  // namespace

void Gi::flushDumps() {
    for (std::size_t i = 0; i < refDumps_.size();) {
        PendingDump& d = refDumps_[i];
        if (dev_.frameValue() < d.frame + 6) {
            ++i;
            continue;
        }
        d.buf->invalidate();
        const auto* src = static_cast<const float*>(d.buf->map());
        const std::uint64_t count = static_cast<std::uint64_t>(d.planes) * d.h * d.w * 4;
        std::error_code ec;
        const std::filesystem::path dir = paths::dataDir() / "gi_ref";
        std::filesystem::create_directories(dir, ec);
        const std::string name = d.full ? std::format("e{:03}_full_{:05}.npy", d.epoch, d.spp) : std::format("e{:03}_{:05}.npy", d.epoch, d.spp);
        std::string header = std::format("{{'descr': '<f2', 'fortran_order': False, 'shape': ({}, {}, {}, 4), }}", d.planes, d.h, d.w);
        header.append(63 - (10 + header.size()) % 64, ' ');
        header.push_back('\n');
        std::ofstream out(dir / name, std::ios::binary);
        if (out && src != nullptr) {
            const char magic[8] = {'\x93', 'N', 'U', 'M', 'P', 'Y', 1, 0};
            out.write(magic, 8);
            const std::uint16_t len = static_cast<std::uint16_t>(header.size());
            out.write(reinterpret_cast<const char*>(&len), 2);
            out.write(header.data(), static_cast<std::streamsize>(header.size()));
            std::vector<std::uint16_t> chunk(1 << 20);
            for (std::uint64_t at = 0; at < count; at += chunk.size()) {
                const std::size_t n = static_cast<std::size_t>(std::min<std::uint64_t>(chunk.size(), count - at));
                for (std::size_t k = 0; k < n; ++k) chunk[k] = toHalf(src[at + k]);
                out.write(reinterpret_cast<const char*>(chunk.data()), static_cast<std::streamsize>(n * 2));
            }
            spdlog::info("enh: gi.reference: dump {} ({} planes {}x{}, {} spp, epoch {}, camera ({:.0f}, {:.0f}, {:.0f}))", name, d.planes, d.w, d.h,
                         d.spp, d.epoch, d.camera[0], d.camera[1], d.camera[2]);
            if (d.full) {
                std::ofstream meta(dir / std::format("e{:03}_meta.json", d.epoch));
                meta << std::format("{{\"epoch\": {}, \"w\": {}, \"h\": {}, \"spp\": {}, \"camera\": [{:.2f}, {:.2f}, {:.2f}], \"bounces\": {}, "
                                    "\"sun\": [{:.4f}, {:.4f}, {:.4f}], \"sun_dir\": [{:.4f}, {:.4f}, {:.4f}], \"lights\": {}, "
                                    "\"planes\": [\"ref_indirect\", \"ref_direct\", \"ddgi_volume\", \"ddgi_screen\", \"albedo\", \"normal\", \"pixel_1spp\", \"pixel_4spp\"]}}\n",
                                    d.epoch, d.w, d.h, d.spp, d.camera[0], d.camera[1], d.camera[2], s_.referenceBounces, sunColor_[0], sunColor_[1],
                                    sunColor_[2], sunDir_[0], sunDir_[1], sunDir_[2], frameLights_.size());
            }
        } else {
            spdlog::warn("enh: gi.reference: cannot write {}", (dir / name).string());
        }
        refDumps_.erase(refDumps_.begin() + static_cast<std::ptrdiff_t>(i));
    }
}

void Gi::scheduleDump(rhi::CommandList& cl, std::uint32_t planes, std::uint32_t spp, bool full) {
    rhi::Device* rd = dev_.rhi();
    if (rd == nullptr || refBuf_ == nullptr) return;
    const std::uint64_t size = static_cast<std::uint64_t>(planes) * refW_ * refH_ * 16;
    PendingDump d;
    d.buf = rd->createBuffer({size, VK_BUFFER_USAGE_TRANSFER_DST_BIT, rhi::Memory::Readback, "gi reference dump"});
    if (d.buf == nullptr) return;
    cl.memoryBarrier(VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT, VK_PIPELINE_STAGE_2_COPY_BIT,
                     VK_ACCESS_2_TRANSFER_READ_BIT);
    cl.copyBuffer(*d.buf, 0, *refBuf_, 0, size);
    cl.memoryBarrier(VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_READ_BIT, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                     VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT);
    d.frame = dev_.frameValue();
    d.w = refW_;
    d.h = refH_;
    d.planes = planes;
    d.spp = spp;
    d.epoch = refEpoch_;
    d.full = full;
    const auto& cam = dev_.camera();
    for (int i = 0; i < 3; ++i) d.camera[i] = cam.position[static_cast<std::size_t>(i)];
    refDumps_.push_back(std::move(d));
}

void Gi::recordReference(d3d9::PassContext& ctx, rhi::CommandList& cl, GiPush push, std::uint32_t resolvedSlot, std::uint32_t w, std::uint32_t h) {
    (void)ctx;
    flushDumps();
    if (!refTrace_) {
        if (!refBuild_.valid() || refBuild_.wait_for(std::chrono::seconds(0)) != std::future_status::ready) return;
        refTrace_ = refBuild_.get();
        if (!refTrace_) {
            spdlog::error("enh: gi.reference: the compute pipeline failed");
            s_.reference = 0;
            return;
        }
        spdlog::info("enh: gi.reference: compute pipeline ready");
    }
    rhi::Device* rd = dev_.rhi();
    if (rd == nullptr) return;
    const std::uint64_t stride = static_cast<std::uint64_t>(w) * h * 16;
    if (refBuf_ == nullptr || refW_ != w || refH_ != h) {
        refBuf_ = rd->createBuffer({stride * 8, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                                    rhi::Memory::Device, "gi reference"});
        if (refBuf_ == nullptr) {
            spdlog::error("enh: gi.reference: the accumulation buffer failed ({} MiB)", stride * 8 / 1048576);
            s_.reference = 0;
            return;
        }
        cl.fillBuffer(*refBuf_, 0, stride * 8, 0);
        cl.memoryBarrier(VK_PIPELINE_STAGE_2_CLEAR_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                         VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT);
        refW_ = w;
        refH_ = h;
        refFirst_ = true;
        refAcc_ = 0;
        refHash_[0] = refHash_[1] = refHash_[2] = refHash_[3] = 0;
        spdlog::info("enh: gi.reference: {}x{}, {:.0f} MiB, {} samples per frame, up to {} samples, {} bounces, view {}", w, h,
                     static_cast<double>(stride * 8) / 1048576.0, s_.referenceSpp, s_.referenceMax, s_.referenceBounces, s_.reference);
    }
    rhi::Texture* view = dev_.transient("gi.ref.view", {VK_FORMAT_R16G16B16A16_SFLOAT, {w, h}});
    const rhi::RingAlloc ra = dev_.ring(sizeof(GiRefParams), 16);
    if (view == nullptr || ra.ptr == nullptr) return;

    // the accumulation restarts when the view, the light or the settings change: separate hashes to name the cause
    const auto& cam = dev_.camera().valid ? dev_.camera() : dev_.lastCamera();
    std::uint64_t hs[4];
    {
        float dims[2] = {static_cast<float>(w), static_cast<float>(h)};
        hs[0] = hashFloats(cam.viewProjection.m.data(), 16);
        hs[0] = hashFloats(cam.position.data(), 3, hs[0]);
        hs[0] = hashFloats(dims, 2, hs[0]);
        // (quantised: a slowly drifting sky would restart the accumulation before it converges, as in the crystal cave)
        hs[1] = hashFloatsQ(sunColor_, 4, 24.0f);
        hs[1] = hashFloatsQ(sunDir_, 4, 24.0f, hs[1]);
        hs[2] = hashFloatsQ(&sky_[0][0], 28, 48.0f);
        hs[2] = hashFloatsQ(indoor_, 4, 48.0f, hs[2]);
        hs[3] = 1469598103934665603ull;
        for (const auto& l : frameLights_) {
            // quantised: flickering lights (candles, crystals) would restart the accumulation every few frames and it would never
            // converge; the reference averages the flicker instead (colour in 41 % steps, position in 10 cm steps)
            float q[7] = {std::floor(l.pos[0] * 0.1f), std::floor(l.pos[1] * 0.1f), std::floor(l.pos[2] * 0.1f), 0.0f,
                          std::floor(std::log2(std::max(l.col[0], 1e-3f)) * 2.0f), std::floor(std::log2(std::max(l.col[1], 1e-3f)) * 2.0f),
                          std::floor(std::log2(std::max(l.col[2], 1e-3f)) * 2.0f)};
            hs[3] = hashFloats(q, 7, hs[3]);
        }
        const float opts[6] = {static_cast<float>(s_.referenceBounces), s_.emissive, static_cast<float>(s_.cutoutsTested), static_cast<float>(s_.characters),
                               static_cast<float>(dev_.rtScene().instances), static_cast<float>(dev_.rtScene().recordCount)};  // (streaming geometry)
        hs[3] = hashFloats(opts, 6, hs[3]);
    }
    bool reset = false;
    for (int i = 0; i < 4; ++i) {
        if (hs[i] != refHash_[i]) {
            reset = true;
            ++refResets_[i];
        }
        refHash_[i] = hs[i];
    }
    if (reset) {
        if (refAcc_ >= kFirstDump) {
            spdlog::info("enh: gi.reference: epoch {} ended after {} samples (resets so far: view {}, sun {}, sky {}, lights {})", refEpoch_, refAcc_,
                         refResets_[0], refResets_[1], refResets_[2], refResets_[3]);
        }
        ++refEpoch_;
        refAcc_ = 0;
        refFirst_ = true;
        refNextDump_ = 0;
        refFinalDumped_ = false;
    }

    const std::uint32_t maxSamples = static_cast<std::uint32_t>(s_.referenceMax);
    const std::uint32_t spp = refAcc_ >= maxSamples ? 0u : std::min<std::uint32_t>(static_cast<std::uint32_t>(s_.referenceSpp), maxSamples - refAcc_);
    GiRefParams rp{};
    split(refBuf_->shaderRefRw(), &rp.buf[0]);
    rp.buf[2] = spp;
    rp.buf[3] = refAcc_;
    rp.img[0] = resolvedSlot;
    rp.img[1] = static_cast<std::uint32_t>(s_.reference);
    rp.img[2] = static_cast<std::uint32_t>(s_.referenceBounces);
    rp.img[3] = refEpoch_ * 2654435761u + refAcc_ * 40503u + 12345u;
    rp.size[0] = w;
    rp.size[1] = h;
    rp.size[2] = refFirst_ ? 1u : 0u;
    std::memcpy(ra.ptr, &rp, sizeof(rp));
    split(ra.ref, push.pad);

    cl.transition(*view, VK_IMAGE_LAYOUT_GENERAL);
    cl.bindPipeline(*refTrace_);
    {
        rhi::Texture* const outs[1] = {view};
        cl.setStorageTextures(outs);
    }
    cl.setPushConstants(&push, sizeof(push));
    cl.dispatch((w + 7) / 8, (h + 7) / 8, 1);
    cl.transition(*view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    cl.computeBarrier();
    refAcc_ += spp;
    refFirst_ = false;
    refViewFrame_ = dev_.frameValue();
    ++refFrames_;

    // dumps: the luminance plane at 64, 128, ... samples (the convergence test), everything at the end
    std::uint32_t next = kFirstDump << refNextDump_;
    while (next < maxSamples && refAcc_ >= next) {
        scheduleDump(cl, 1, refAcc_, false);
        ++refNextDump_;
        next = kFirstDump << refNextDump_;
    }
    if (refAcc_ >= maxSamples && !refFinalDumped_) {
        refFinalDumped_ = true;
        scheduleDump(cl, 8, refAcc_, true);
    }
}

void Gi::recordReferenceView(d3d9::PassContext& ctx) {
    d3d9::Texture* scene = ctx.sceneHdr;
    if (ctx.mainView != 1 || scene == nullptr || ctx.target != scene || refViewFrame_ != dev_.frameValue()) return;
    rhi::Device* rdev = dev_.rhi();
    if (rdev == nullptr) return;
    rhi::Texture& target = scene->rhi();
    const std::uint32_t w = scene->desc().width, h = scene->desc().height;
    if (w != refW_ || h != refH_) return;
    rhi::Texture* view = dev_.transient("gi.ref.view", {VK_FORMAT_R16G16B16A16_SFLOAT, {w, h}});
    if (view == nullptr) return;
    const std::uint32_t fmt = static_cast<std::uint32_t>(target.desc().format);
    if (!refViewPipe_ || refViewFormat_ != fmt) {
        rhi::GraphicsPipelineDesc gd;
        gd.vs.spirv = shader::embedded::spirv("debug/fullscreen_rhi.vert");
        gd.ps.spirv = shader::embedded::spirv("debug/inject_view.frag");
        gd.vs.dxil = shader::embedded::dxilOnDemand("debug/fullscreen_rhi.vert");
        gd.ps.dxil = shader::embedded::dxilOnDemand("debug/inject_view.frag");
        gd.colorFormats[0] = target.desc().format;
        gd.blend.blendEnable = VK_FALSE;
        gd.name = "gi.reference view";
        refViewPipe_ = rdev->createGraphicsPipeline(gd);
        refViewFormat_ = fmt;
        if (!refViewPipe_) {
            spdlog::error("enh: gi.reference: the view pipeline failed");
            return;
        }
    }
    rhi::CommandList& cl = ctx.cmd;
    DebugViewPush push{};
    push.h.frame = ctx.frameBlock.bits;
    push.h.passId = 1;
    push.width = w;
    push.height = h;
    push.srcTex = view->heapSampled();
    push.srcSampler = 0;
    cl.transition(*view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    cl.transition(target, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    rhi::RenderingInfo ri;
    ri.color[0].texture = &target;
    ri.color[0].load = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    ri.color[0].store = VK_ATTACHMENT_STORE_OP_STORE;
    ri.colorCount = 1;
    ri.width = w;
    ri.height = h;
    cl.beginRendering(ri);
    cl.bindPipeline(*refViewPipe_);
    cl.setPushConstants(&push, sizeof(push));
    cl.draw(3, 1, 0, 0);
    cl.endRendering();
}

}  // namespace rvk::enh
