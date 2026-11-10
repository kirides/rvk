#include "enh/depth_z.h"

#include <algorithm>
#include <cstring>

#include <spdlog/spdlog.h>

#include "shader/embedded.h"

namespace rvk::enh {

namespace {

float bitsToFloat(std::uint32_t v) {
    float f;
    std::memcpy(&f, &v, 4);
    return f;
}

}  // namespace

DepthZ::DepthZ(d3d9::Device& device) : dev_(device) {}

DepthZ::~DepthZ() { setMode(Mode::Off); }

void DepthZ::setMode(Mode mode) {
    if (mode == mode_) return;
    if (passId_ != 0) dev_.removePass(passId_);
    passId_ = 0;
    armed_ = false;
    mode_ = mode;
    if (mode == Mode::Off) {
        dev_.setDepthZ(d3d9::Device::DepthZ::Off);
        for (Slot& s : slots_) s = {};
        spdlog::info("native: depth_z off");
        return;
    }
    failed_ = false;
    // (the order: before every other pass of the point, which read the depth map: rt.scene -100, the G-buffer, the lights, the RT shadows, ...)
    passId_ = dev_.addPass(d3d9::inject::Point::AfterDepthPrepass, "native.depth_z", [this](d3d9::PassContext& ctx) { record(ctx); }, this, -200);
    // the cascades' pixel shaders go at once; the main z pass keeps its attachment until the derive pipeline exists (record)
    dev_.setDepthZ(d3d9::Device::DepthZ::PsLess);
    spdlog::info("native: depth_z {}: colour-less z passes draw without a pixel shader{}", mode == Mode::On ? "on" : "compare",
                 mode == Mode::On ? "; the main z pass loses its R32F attachment once the derive pass is ready (the depth map comes from the hardware depth)"
                                  : "; the derivation of the depth map is checked against the engine's z pass on the GPU");
}

bool DepthZ::ensureDerive(rhi::Device& rdev, VkFormat target) {
    if (failed_) return false;
    if (derive_ && deriveFormat_ != target) derive_.reset();
    if (!derive_) {
        rhi::GraphicsPipelineDesc gd;
        gd.vs.spirv = shader::embedded::spirv("debug/fullscreen_rhi.vert");
        gd.vs.dxil = shader::embedded::dxilOnDemand("debug/fullscreen_rhi.vert");
        gd.ps.spirv = shader::embedded::spirv("depth/depth_z.frag");
        gd.ps.dxil = shader::embedded::dxilOnDemand("depth/depth_z.frag");
        gd.colorFormats[0] = target;
        gd.name = "native.depth_z derive";
        derive_ = rdev.createGraphicsPipeline(gd);
        deriveFormat_ = target;
    }
    if (!derive_) {
        spdlog::error("native: depth_z: the derive pipeline failed, the setting stays off this session (the engine's depth map is used)");
        failed_ = true;
        dev_.setDepthZ(d3d9::Device::DepthZ::PsLess);
        armed_ = false;
        return false;
    }
    return true;
}

bool DepthZ::ensureCompare(rhi::Device& rdev) {
    if (failed_) return false;
    if (!compare_) {
        rhi::ComputePipelineDesc cd;
        cd.cs.spirv = shader::embedded::spirv("depth/depth_z_compare.comp");
        cd.cs.dxil = shader::embedded::dxilOnDemand("depth/depth_z_compare.comp");
        cd.name = "native.depth_z compare";
        compare_ = rdev.createComputePipeline(cd);
    }
    if (!compare_) {
        spdlog::error("native: depth_z compare: the pipeline failed, the check stays off this session");
        failed_ = true;
        return false;
    }
    for (Slot& s : slots_) {
        if (!s.gpu) {
            s.gpu = rdev.createBuffer({kStatsBytes, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT, rhi::Memory::Device,
                                       "depth_z compare stats"});
            s.readback = rdev.createBuffer({kStatsBytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT, rhi::Memory::Readback, "depth_z compare readback"});
            if (!s.gpu || !s.readback) {
                spdlog::error("native: depth_z compare: the buffers could not be created, the check stays off this session");
                failed_ = true;
                return false;
            }
        }
    }
    return true;
}

void DepthZ::record(d3d9::PassContext& ctx) {
    collect();
    report();
    if (mode_ == Mode::On) {
        recordDerive(ctx);
    } else if (mode_ == Mode::Compare) {
        recordCompare(ctx);
    }
}

void DepthZ::recordDerive(d3d9::PassContext& ctx) {
    rhi::Device* rdev = dev_.rhi();
    d3d9::Texture* map = ctx.depthMap;
    d3d9::Surface* ds = dev_.depthStencil();
    if (rdev == nullptr || map == nullptr) return;
    if (!armed_) {
        // the first view: build the pipeline, then let the next z pass drop its attachment (this view's map is the engine's)
        if (map->rhi().desc().format != VK_FORMAT_R32_SFLOAT) {
            if (!failed_) spdlog::warn("native: depth_z: the depth map is not R32F ({}): the engine's z pass keeps writing it", static_cast<int>(map->rhi().desc().format));
            failed_ = true;
            return;
        }
        if (!ensureDerive(*rdev, map->rhi().desc().format)) return;
        armed_ = true;
        dev_.setDepthZ(d3d9::Device::DepthZ::Main);
        spdlog::info("native: depth_z: derive pipeline ready, the main z pass drops the depth map attachment from the next scope on");
        return;
    }
    if (!dev_.takeDepthMapDropped()) {
        ++skippedKept_;  // the scope kept the engine's attachment (conditions did not hold): the map is the engine's
        return;
    }
    const std::uint32_t w = map->desc().width, h = map->desc().height;
    if (ds == nullptr || ds->texture() == nullptr || ds->texture()->isNull() || ds->width() < w || ds->height() < h || !ensureDerive(*rdev, map->rhi().desc().format)) {
        ++skipped_;
        return;
    }
    rhi::CommandList& cl = ctx.cmd;
    if (!dev_.readable(ds->texture(), cl)) {
        ++skipped_;
        return;
    }
    DepthZPush push{};
    push.h.frame = ctx.frameBlock.bits;
    push.h.passId = 1;
    push.depthTex = ds->texture()->bindlessSlot();
    push.width = w;
    push.height = h;
    cl.transition(map->rhi(), VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    rhi::RenderingInfo ri;
    ri.color[0].texture = &map->rhi();
    ri.color[0].load = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    ri.color[0].store = VK_ATTACHMENT_STORE_OP_STORE;
    ri.colorCount = 1;
    ri.width = w;
    ri.height = h;
    cl.beginRendering(ri);
    cl.bindPipeline(*derive_);
    cl.setPushConstants(&push, sizeof(push));
    cl.draw(3, 1, 0, 0);
    cl.endRendering();
    ++derived_;
    ++derivedTotal_;
}

void DepthZ::recordCompare(d3d9::PassContext& ctx) {
    rhi::Device* rdev = dev_.rhi();
    d3d9::Texture* map = ctx.depthMap;
    d3d9::Surface* ds = dev_.depthStencil();
    if (rdev == nullptr || map == nullptr || ds == nullptr || ds->texture() == nullptr || ds->texture()->isNull() || !ensureCompare(*rdev)) return;
    const std::uint32_t w = map->desc().width, h = map->desc().height;
    if (ds->width() < w || ds->height() < h) {
        ++skipped_;
        return;
    }
    rhi::CommandList& cl = ctx.cmd;
    if (!dev_.readable(ds->texture(), cl) || !dev_.readable(map, cl)) return;
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
    cl.fillBuffer(*slot->gpu, 0, kStatsBytes, 0);
    cl.bufferBarrier(*slot->gpu, rhi::BufferAccess::ShaderWrite);
    DepthZPush push{};
    push.h.frame = ctx.frameBlock.bits;
    push.h.passId = 1;
    push.depthTex = ds->texture()->bindlessSlot();
    push.mapTex = map->bindlessSlot();
    push.width = w;
    push.height = h;
    const auto sr = slot->gpu->shaderRefRw(0);
    push.stats[0] = sr[0];
    push.stats[1] = sr[1];
    cl.bindPipeline(*compare_);
    cl.setPushConstants(&push, sizeof(push));
    cl.dispatch((w + 7) / 8, (h + 7) / 8, 1);
    cl.bufferBarrier(*slot->gpu, rhi::BufferAccess::ShaderRead);
    cl.copyBuffer(*slot->readback, 0, *slot->gpu, 0, kStatsBytes);
    slot->frame = dev_.game().frameValue();
    slot->pixels = w * h;
}

void DepthZ::collect() {
    rhi::GameDevice& game = dev_.game();
    for (Slot& s : slots_) {
        if (s.frame == 0 || game.completedFrame() < s.frame) continue;
        s.readback->invalidate();
        const auto* got = static_cast<const std::uint32_t*>(s.readback->map());
        ++win_.views;
        win_.pixels += s.pixels;
        win_.geometry += got[7];
        for (int i = 0; i < 5; ++i) win_.over[i] += got[i];
        win_.skyDiff += got[6];
        win_.maxRel = std::max(win_.maxRel, bitsToFloat(got[5]));
        win_.nearMax = std::max(win_.nearMax, bitsToFloat(got[8]));
        s.frame = 0;
    }
}

void DepthZ::report() {
    const double now = dev_.secondsSinceCreate();
    if (now - lastReport_ < 5.0) return;
    if (lastReport_ == 0.0) {
        lastReport_ = now;
        return;
    }
    lastReport_ = now;
    const d3d9::Device::DepthZStats st = dev_.takeDepthZStats();
    if (mode_ == Mode::Compare) {
        const double g = static_cast<double>(std::max<std::uint64_t>(win_.geometry, 1));
        spdlog::info("native: depth_z compare (5 s): views {}, geometry pixels {}, relative error over 1e-6 / 1e-5 / 1e-4 / 1e-3 / 4e-3: {} / {} / {} / {} / {} ({:.4f} % / {:.4f} % / {:.4f} % / "
                     "{:.4f} % / {:.4f} % of the geometry pixels), max {:.3e}, max below map 0.02 {:.3e}; sky classification differs {}; pixel shaders left out {} (kept {}); skipped {}",
                     win_.views, win_.geometry, win_.over[0], win_.over[1], win_.over[2], win_.over[3], win_.over[4], 100.0 * win_.over[0] / g, 100.0 * win_.over[1] / g,
                     100.0 * win_.over[2] / g, 100.0 * win_.over[3] / g, 100.0 * win_.over[4] / g, win_.maxRel, win_.nearMax, win_.skyDiff, st.psDropped, st.psKept, skipped_);
        win_ = {};
    } else {
        spdlog::info("native: depth_z (5 s): depth map derived {} times (total {}), z scopes without the attachment {} (kept the engine's {}), pixel shaders left out {} (kept {}: texkill / "
                     "depth write / alpha test; main view {} / {}, shadow views {} / {}, draws not replayed from a retained packet); skipped {}, scope kept the engine's map {}",
                     derived_, derivedTotal_, st.scopesDropped, st.scopesRefused, st.psDropped, st.psKept, st.psDroppedMain, st.psKeptMain, st.psDropped - st.psDroppedMain,
                     st.psKept - st.psKeptMain, skipped_, skippedKept_);
    }
    derived_ = skipped_ = skippedKept_ = 0;
}

}  // namespace rvk::enh
