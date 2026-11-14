#include "enh/sdsm.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>

#include <spdlog/spdlog.h>

#include "shader/embedded.h"

namespace rvk::enh {

namespace {

namespace sm = engine::shadowmaps;

Sdsm* g_instance = nullptr;

// the reduction's order-preserving encoding back to a float (shaders/shadows/sdsm_reduce.comp.hlsl sdsmEnc)
float decode(std::uint32_t u) {
    const std::uint32_t b = (u & 0x80000000u) != 0u ? (u & 0x7fffffffu) : ~u;
    float f;
    std::memcpy(&f, &b, sizeof f);
    return f;
}

double steadySeconds() { return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count(); }

}  // namespace

Sdsm::Sdsm(d3d9::Device& device) : dev_(device) {}

Sdsm::~Sdsm() { setEnabled(false); }

void Sdsm::setEnabled(bool on) {
    if (on == on_) return;
    on_ = on;
    if (!on) {
        if (passId_ != 0) dev_.removePass(passId_);
        passId_ = 0;
        sm::setSdsmPoll(nullptr);
        g_instance = nullptr;
        if (build_.valid()) build_.get();
        pipeline_.reset();
        for (Slot& s : slots_) s = {};
        spdlog::info("enh: shadows: sdsm reduction off");
        return;
    }
    failed_ = false;
    g_instance = this;
    sm::setSdsmPoll(&Sdsm::pollTrampoline);
    passId_ = dev_.addPass(d3d9::inject::Point::AfterDepthPrepass, "shadows.sdsm", [this](d3d9::PassContext& ctx) { record(ctx); }, this);
    spdlog::info("enh: shadows: sdsm reduction on ({} depth bins, light-space bounds of the visible pixels)", sm::SdsmFrame::kBins);
}

void Sdsm::pollTrampoline() {
    if (g_instance != nullptr) g_instance->poll();
}

bool Sdsm::ensureResources(rhi::Device& rdev) {
    if (failed_) return false;
    if (!pipeline_) {
        if (!build_.valid()) {
            build_ = std::async(std::launch::async, [dev = &rdev]() {
                rhi::ComputePipelineDesc cd;
                cd.cs.spirv = shader::embedded::spirv("shadows/sdsm_reduce.comp");
                cd.cs.dxil = shader::embedded::dxilOnDemand("shadows/sdsm_reduce.comp");
                cd.name = "shadows.sdsm reduce";
                return dev->createComputePipeline(cd);
            });
            return false;
        }
        if (build_.wait_for(std::chrono::seconds(0)) != std::future_status::ready) return false;
        pipeline_ = build_.get();
        if (!pipeline_) {
            spdlog::error("enh: shadows: the sdsm reduction pipeline failed: the fit stays the sphere's this session");
            failed_ = true;
            return false;
        }
    }
    for (Slot& s : slots_) {
        if (s.gpu) continue;
        s.gpu = rdev.createBuffer({kBytes, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT, rhi::Memory::Device,
                                   "sdsm bins"});
        s.readback = rdev.createBuffer({kBytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT, rhi::Memory::Readback, "sdsm bins read-back"});
        if (!s.gpu || !s.readback) {
            spdlog::error("enh: shadows: the sdsm buffers could not be created: the fit stays the sphere's this session");
            failed_ = true;
            return false;
        }
    }
    return true;
}

void Sdsm::record(d3d9::PassContext& ctx) {
    if (ctx.mainView != 1 || !on_ || failed_) return;
    rhi::Device* rdev = dev_.rhi();
    d3d9::Surface* ds = dev_.depthStencil();
    const auto& cam = dev_.camera();
    sm::SdsmSetup su;
    if (rdev == nullptr || ctx.sceneHdr == nullptr || ds == nullptr || ds->texture() == nullptr || ds->texture()->isNull() || !cam.valid || !sm::sdsmSetup(su) ||
        !ensureResources(*rdev)) {
        ++skipped_;
        return;
    }
    rhi::CommandList& cl = ctx.cmd;
    if (!dev_.readable(ds->texture(), cl)) {
        ++skipped_;
        return;
    }
    Slot* slot = nullptr;
    for (Slot& s : slots_) {
        if (s.frame == 0) {
            slot = &s;
            break;
        }
    }
    if (slot == nullptr) {
        ++skipped_;
        return;
    }
    const std::uint32_t w = std::min(ctx.sceneHdr->desc().width, ds->width()), h = std::min(ctx.sceneHdr->desc().height, ds->height());
    const float p00 = std::fabs(cam.projection.at(0, 0)), p11 = std::fabs(cam.projection.at(1, 1));
    if (!(p00 > 1e-6f && p11 > 1e-6f) || !(su.range > 20.0f)) {
        ++skipped_;
        return;
    }
    sm::SdsmFrame& m = slot->meta;
    m = sm::SdsmFrame{};
    m.counter = su.counter;
    m.time = steadySeconds();
    m.zMin = 10.0f;
    m.zMax = su.range;
    m.tanX = 1.0f / p00;
    m.tanY = 1.0f / p11;
    std::memcpy(m.lightA, su.lightA, sizeof m.lightA);
    std::memcpy(m.lightT, su.lightT, sizeof m.lightT);
    for (int i = 0; i < 3; ++i) {
        m.pos[i] = cam.position[static_cast<std::size_t>(i)];
        for (int j = 0; j < 3; ++j) m.axes[i * 3 + j] = cam.inverseView.at(i, j);
    }

    cl.fillBuffer(*slot->gpu, 0, kBytes, 0);
    cl.bufferBarrier(*slot->gpu, rhi::BufferAccess::ShaderWrite);
    SdsmPush push{};
    push.h.frame = ctx.frameBlock.bits;
    push.h.passId = 1;
    push.depthTex = ds->texture()->bindlessSlot();
    push.width = w;
    push.height = h;
    push.stride = 2;
    const auto sr = slot->gpu->shaderRefRw(0);
    push.bins[0] = sr[0];
    push.bins[1] = sr[1];
    push.range[0] = m.zMin;
    push.range[1] = static_cast<float>(sm::SdsmFrame::kBins) / std::log(m.zMax / m.zMin);
    push.range[2] = m.zMax;
    for (int k = 0; k < 3; ++k) {
        push.lx[k] = su.lightA[k * 3 + 0];
        push.ly[k] = su.lightA[k * 3 + 1];
    }
    push.lx[3] = su.lightT[0];
    push.ly[3] = su.lightT[1];
    cl.bindPipeline(*pipeline_);
    cl.setPushConstants(&push, sizeof push);
    const std::uint32_t tile = 16 * push.stride;
    cl.dispatch((w + tile - 1) / tile, (h + tile - 1) / tile, 1);
    cl.bufferBarrier(*slot->gpu, rhi::BufferAccess::ShaderRead);
    cl.copyBuffer(*slot->readback, 0, *slot->gpu, 0, kBytes);
    slot->frame = dev_.game().frameValue();
    ++recorded_;
}

// the reductions whose frame the GPU completed: the newest goes to the cascade fit, the older ones are dropped (never waits)
void Sdsm::poll() {
    rhi::GameDevice& game = dev_.game();
    Slot* newest = nullptr;
    for (Slot& s : slots_) {
        if (s.frame == 0 || game.completedFrame() < s.frame) continue;
        if (newest == nullptr || s.meta.counter > newest->meta.counter) {
            if (newest != nullptr) newest->frame = 0;
            newest = &s;
        } else {
            s.frame = 0;
        }
    }
    if (newest == nullptr) return;
    newest->readback->invalidate();
    const auto* words = static_cast<const std::uint32_t*>(newest->readback->map());
    if (words != nullptr) {
        sm::SdsmFrame& m = newest->meta;
        for (int i = 0; i < sm::SdsmFrame::kBins; ++i) {
            const std::uint32_t* w = words + i * 4;
            float* b = m.bounds[i];
            if (w[0] == 0 || w[1] == 0 || w[2] == 0 || w[3] == 0) {
                b[0] = 1.0f;  // empty: min > max
                b[1] = -1.0f;
                b[2] = 1.0f;
                b[3] = -1.0f;
            } else {
                b[0] = -decode(w[1]);
                b[1] = decode(w[0]);
                b[2] = -decode(w[3]);
                b[3] = decode(w[2]);
            }
        }
        sm::submitSdsm(m);
    }
    newest->frame = 0;
}

}  // namespace rvk::enh
