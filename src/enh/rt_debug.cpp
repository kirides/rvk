#include "enh/rt_debug.h"

#include <cstring>
#include <string>

#include <spdlog/spdlog.h>

#include "enh/rhi_scope.h"
#include "enh/rt_scene.h"
#include "shader/embedded.h"

namespace rvk::enh {

RtDebug::Mode RtDebug::parse(std::string_view option) {
    if (option == "instances") return Mode::Instances;
    if (option == "depth") return Mode::Depth;
    if (option == "normals") return Mode::Normals;
    if (option == "albedo" || option == "albedo_split" || option == "albedo_opaque") return Mode::Albedo;
    if (option == "uv") return Mode::Uv;
    return Mode::Off;
}

std::uint32_t RtDebug::optionFlags(std::string_view option) {
    return option == "albedo_split" ? kRtDebugSplit : option == "albedo_opaque" ? kRtDebugOpaque : 0u;
}

RtDebug::RtDebug(d3d9::Device& device, RtScene& scene) : dev_(device), scene_(scene) {}

RtDebug::~RtDebug() {
    setMode(Mode::Off);
    destroyPipelines();
}

void RtDebug::readProbes(std::uint32_t slot) {
    if (probeFrame_[slot] == 0 || probes_[slot] == nullptr) return;
    const std::uint64_t frame = probeFrame_[slot];
    probeFrame_[slot] = 0;
    probes_[slot]->invalidate();
    const auto* r = static_cast<const RtDebugProbe*>(probes_[slot]->map());
    const auto now = std::chrono::steady_clock::now();
    // the material views every second (the probe pixels of a capture's frozen frames get logged)
    const bool materialView = mode_ == Mode::Albedo || mode_ == Mode::Uv;
    if (now - probeLogged_ >= std::chrono::seconds(materialView ? 1 : 5)) {
        probeLogged_ = now;
        static constexpr const char* kCodes[] = {"nothing", "match", "traced farther", "traced closer", "missed (raster has geometry)",
                                                  "hit (raster has none)"};
        int counts[6]{};
        for (std::uint32_t k = 0; k < kRtDebugProbes; ++k) counts[std::min<std::uint32_t>(r[k].code, 5)]++;
        spdlog::info("enh: rt.debug probes of frame {} (5 x 5 grid): {} match, {} traced farther, {} traced closer, {} missed, {} hit sky",
                     frame, counts[1], counts[2], counts[3], counts[4], counts[5]);
        for (std::uint32_t k = 0; k < kRtDebugProbes; ++k) {
            // the material views name every probe's surface
            if (r[k].code == 0 || (r[k].code == 1 && !materialView)) continue;
            spdlog::info("enh: rt.debug   probe {},{}: {}: t {:.1f}, raster {:.1f}{}", k % 5, k / 5, kCodes[std::min<std::uint32_t>(r[k].code, 5)],
                         r[k].t, r[k].rasterT, r[k].record != 0 ? "; " + scene_.describe(frame, r[k].record - 1) : std::string());
        }
    }
    std::memset(probes_[slot]->map(), 0, sizeof(RtDebugProbe) * kRtDebugProbes);
}

void RtDebug::setMode(Mode mode, std::uint32_t flags) {
    flags_ = flags;
    if (mode == mode_) return;
    mode_ = mode;
    if (mode == Mode::Off) {
        if (passId_ != 0) dev_.removePass(passId_);
        if (depthPassId_ != 0) dev_.removePass(depthPassId_);
        passId_ = depthPassId_ = 0;
        dev_.releaseTransient("rt.debug");
        dev_.releaseTransient("rt.debug.depth");
        scene_.keepContents(false);
        scene_.release(this);
        for (std::uint32_t i = 0; i < rhi::kFramesInFlight; ++i) {
            probes_[i].reset();  // (destroyed with the frame being recorded)
            probeFrame_[i] = 0;
        }
        return;
    }
    if (!scene_.supported()) {
        spdlog::warn("enh: rt.debug: the ray tracing tier is off: nothing to show");
        return;
    }
    scene_.acquire(this);
    scene_.keepContents(true);
    if (passId_ == 0) {
        passId_ = dev_.addPass(d3d9::inject::Point::BeforeGui, "rt.debug", [this](d3d9::PassContext& ctx) { record(ctx); }, this);
    }
    if (mode == Mode::Depth && depthPassId_ == 0) {
        // after the RT scene's pass (order -100) at the same point
        depthPassId_ = dev_.addPass(d3d9::inject::Point::AfterOpaque, "rt.debug.depth", [this](d3d9::PassContext& ctx) { captureDepth(ctx); },
                                    this, 10);
    } else if (mode != Mode::Depth && depthPassId_ != 0) {
        dev_.removePass(depthPassId_);
        depthPassId_ = 0;
        dev_.releaseTransient("rt.debug.depth");
    }
    frames_ = 0;
}

void RtDebug::destroyPipelines() {
    compute_.reset();  // (retired with the device's frames)
    composite_.reset();
    depthCompute_.reset();
    compositeFormat_ = VK_FORMAT_UNDEFINED;
}

bool RtDebug::ensurePipelines(VkFormat target) {
    if (failed_) return false;
    rhi::Device* rdev = dev_.rhi();
    if (rdev == nullptr) {
        failed_ = true;
        return false;
    }
    if (!compute_) {
        rhi::ComputePipelineDesc cd;
        cd.cs.spirv = shader::embedded::spirv("rt/rt_debug.comp");
        cd.cs.dxil = shader::embedded::dxilOnDemand("rt/rt_debug.comp");
        cd.name = "rt.debug compute";
        compute_ = rdev->createComputePipeline(cd);
    }
    if (!depthCompute_) {
        rhi::ComputePipelineDesc cd;
        cd.cs.spirv = shader::embedded::spirv("rt/rt_debug_depth.comp");
        cd.cs.dxil = shader::embedded::dxilOnDemand("rt/rt_debug_depth.comp");
        cd.name = "rt.debug depth";
        depthCompute_ = rdev->createComputePipeline(cd);
    }
    if (composite_ && compositeFormat_ != target) composite_.reset();
    if (!composite_) {
        rhi::GraphicsPipelineDesc gd;
        gd.vs.spirv = shader::embedded::spirv("debug/fullscreen_rhi.vert");
        gd.vs.dxil = shader::embedded::dxilOnDemand("debug/fullscreen_rhi.vert");
        gd.ps.spirv = shader::embedded::spirv("rt/rt_debug.frag");
        gd.ps.dxil = shader::embedded::dxilOnDemand("rt/rt_debug.frag");
        gd.colorFormats[0] = target;
        gd.blend.blendEnable = VK_TRUE;  // src alpha over; the target's alpha kept
        gd.blend.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
        gd.blend.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
        gd.blend.colorBlendOp = VK_BLEND_OP_ADD;
        gd.blend.srcAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
        gd.blend.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
        gd.blend.alphaBlendOp = VK_BLEND_OP_ADD;
        gd.name = "rt.debug composite";
        composite_ = rdev->createGraphicsPipeline(gd);
        compositeFormat_ = target;
    }
    if (!compute_ || !composite_ || !depthCompute_) {
        spdlog::error("enh: rt.debug: pipelines failed, the view stays off this session");
        failed_ = true;
        return false;
    }
    return true;
}

void RtDebug::captureDepth(d3d9::PassContext& ctx) {
    if (ctx.mainView != 1 || !dev_.rtSceneValid() || failed_) return;
    d3d9::Surface* ds = dev_.depthStencil();
    d3d9::Texture* scene = ctx.sceneHdr;
    if (ds == nullptr || ds->texture() == nullptr || scene == nullptr || ctx.target != scene) return;
    d3d9::Surface* bb = dev_.backBuffer();
    if (bb == nullptr || !ensurePipelines(bb->texture()->rhi().desc().format)) return;
    const std::uint32_t w = std::min(scene->desc().width, ds->width()), h = std::min(scene->desc().height, ds->height());
    rhi::Texture* out = dev_.transient("rt.debug.depth", {VK_FORMAT_R32_SFLOAT, {w, h}});
    if (out == nullptr || !dev_.readable(ds->texture(), ctx.cmd)) return;
    RtDebugPush push{};
    push.h.frame = ctx.frameBlock.bits;
    push.h.passId = 2;
    push.depthTex = ds->texture()->bindlessSlot();
    push.width = w;
    push.height = h;
    RhiScope rs(dev_, ctx.cmd);
    if (!rs) return;
    rhi::CommandList& cl = rs.cl();
    rhi::Texture* outT = rs.wrap(*out);
    cl.transition(*outT, VK_IMAGE_LAYOUT_GENERAL);
    cl.bindPipeline(*depthCompute_);
    rhi::Texture* const storage[] = {outT};
    cl.setStorageTextures(storage);
    cl.setPushConstants(&push, sizeof(push));
    cl.dispatch((w + 7) / 8, (h + 7) / 8, 1);
    cl.transition(*outT, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    depthFrame_ = dev_.frameValue();
    depthExtent_ = {w, h};
}

void RtDebug::record(d3d9::PassContext& ctx) {
    // only frames whose RT scene was built in this submission (a frame without a main view has none)
    if (!dev_.rtSceneValid()) return;
    d3d9::Surface* bb = dev_.backBuffer();
    if (bb == nullptr || bb->texture() == nullptr) return;
    d3d9::Texture* targetTex = bb->texture();
    rhi::Texture& target = targetTex->rhi();
    if (!ensurePipelines(target.desc().format)) return;
    const std::uint32_t w = bb->width(), h = bb->height();
    rhi::Texture* color = dev_.transient("rt.debug", {VK_FORMAT_R16G16B16A16_SFLOAT, {w, h}});
    if (color == nullptr) return;
    rhi::CommandList& cmd = ctx.cmd;

    RtDebugPush push{};
    push.h.frame = ctx.frameBlock.bits;
    push.h.passId = 1;
    push.width = w;
    push.height = h;
    push.mode = static_cast<std::uint32_t>(mode_) | flags_;
    // the raster's depth: this frame's hardware depth capture (depth mode), else the engine's linear depth map (z-pass only)
    rhi::Texture* captured = mode_ == Mode::Depth && depthFrame_ == dev_.frameValue()
                                          ? dev_.transient("rt.debug.depth", {VK_FORMAT_R32_SFLOAT, depthExtent_})
                                          : nullptr;
    if (captured != nullptr) {
        push.depthTex = captured->heapSampled();
        push.mode |= kRtDebugLinearDepth;
    } else if (ctx.depthMap != nullptr && dev_.readable(ctx.depthMap, cmd)) {
        push.depthTex = ctx.depthMap->bindlessSlot();
        if (mode_ == Mode::Depth && fallbacks_++ % 600 == 0) {
            spdlog::info("enh: rt.debug: no hardware depth capture this frame: the engine's depth map (z-pass draws only) is the "
                         "reference ({} frames so far)", fallbacks_);
        }
    }
    push.srcTex = color->heapSampled();
    push.srcSampler = 0;  // point / clamp (same size: one texel per pixel)
    // probe results: the slot's previous ones are complete (Frames::begin waited for that submission)
    const std::uint32_t slot = dev_.frameSlot();
    readProbes(slot);
    if (probes_[slot] == nullptr && dev_.rhi() != nullptr) {
        probes_[slot] = dev_.rhi()->createBuffer(
            {sizeof(RtDebugProbe) * kRtDebugProbes, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, rhi::Memory::Readback, "rt.debug probes"});
        if (probes_[slot] != nullptr) std::memset(probes_[slot]->map(), 0, sizeof(RtDebugProbe) * kRtDebugProbes);
    }
    if (probes_[slot] != nullptr) {
        probes_[slot]->flush();
        const auto ref = probes_[slot]->shaderRefRw(0);
        push.probes[0] = ref[0];
        push.probes[1] = ref[1];
        probeFrame_[slot] = dev_.frameValue();
    }

    RhiScope rs(dev_, cmd);
    if (!rs) return;
    rhi::CommandList& cl = rs.cl();
    rhi::Texture* colorT = rs.wrap(*color);
    cl.transition(*colorT, VK_IMAGE_LAYOUT_GENERAL);
    cl.bindPipeline(*compute_);
    rhi::Texture* const storage[] = {colorT};
    cl.setStorageTextures(storage);
    cl.setPushConstants(&push, sizeof(push));
    cl.dispatch((w + 7) / 8, (h + 7) / 8, 1);

    cl.transition(*colorT, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    rhi::Texture* targetT = rs.wrap(target);
    cl.transition(*targetT, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    rhi::RenderingInfo ri;
    ri.color[0].texture = targetT;
    ri.color[0].load = VK_ATTACHMENT_LOAD_OP_LOAD;
    ri.color[0].store = VK_ATTACHMENT_STORE_OP_STORE;
    ri.colorCount = 1;
    ri.width = w;
    ri.height = h;
    cl.beginRendering(ri);
    cl.bindPipeline(*composite_);
    cl.setPushConstants(&push, sizeof(push));
    cl.draw(3, 1, 0, 0);
    cl.endRendering();
    if (frames_++ == 0) {
        spdlog::info("enh: rt.debug mode {} at {}: {}x{}, depth map {}, TLAS {} instances", static_cast<int>(mode_),
                     d3d9::inject::name(ctx.point), w, h, push.depthTex != 0 ? "slot " + std::to_string(push.depthTex) : "none",
                     dev_.rtScene().instances);
    }
}

}  // namespace rvk::enh
