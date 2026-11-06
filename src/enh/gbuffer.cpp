#include "enh/gbuffer.h"

#include <algorithm>
#include <bit>
#include <chrono>
#include <cstring>
#include <format>

#include <spdlog/spdlog.h>

#include "enh/material_variant.h"
#include "enh/rhi_scope.h"
#include "enh/rt_scene.h"
#include "shader/embedded.h"

namespace rvk::enh {

namespace {

constexpr VkFormat kNormalFormat = kGbufferNormalFormat;
constexpr VkFormat kMotionFormat = kGbufferMotionFormat;
constexpr VkImageUsageFlags kNormalUsage = kGbufferNormalUsage;
constexpr VkImageUsageFlags kMotionUsage = kGbufferMotionUsage;

// one variant slot per target layout (d3d9::Device::gbufferSlot): a layout change at run time takes other modules, none is rebuilt
template <MaterialVariant K, GbufferOutputs O>
bool gbufferVariantOf(const d3d9::Shader& s, shader::TranslateOptions& o) {
    return materialVariantOptions(s, K, o, O);
}

// 1x1 stand-ins for storage images a pass binds but does not write in its mode (no full-size image without a reader)
constexpr const char* kDummyAo = "gtao.unused_ao";
constexpr const char* kDummyMotion = "gtao.unused_motion";

}  // namespace

Gbuffer::Gbuffer(d3d9::Device& device, RtScene* scene) : dev_(device), scene_(scene) {
    rhi::Device* rdev = dev_.rhi();
    if (rdev == nullptr) {
        failed_ = true;
        return;
    }
    const auto build = [rdev](const char* file, const char* name) {
        return [rdev, file, name]() {
            rhi::ComputePipelineDesc d;
            d.cs.spirv = shader::embedded::spirv(file);
            d.cs.dxil = shader::embedded::dxilOnDemand(file);
            d.name = name;
            return rdev->createComputePipeline(d);
        };
    };
    gtaoBuild_ = std::async(std::launch::async, build("gbuffer/gtao.comp", "gbuffer.gtao"));
    filterBuild_ = std::async(std::launch::async, build("gbuffer/gtao_filter.comp", "gbuffer.gtao filter"));
    using GO = GbufferOutputs;
    dev_.setVariantProvider(d3d9::VariantSlot::Gbuffer, {materialVariantTag("gbuf"), gbufferVariantOf<MaterialVariant::Gbuffer, GO::NormalMotion>});
    dev_.setVariantProvider(d3d9::VariantSlot::GbufferM, {materialVariantTag("gbuf_m"), gbufferVariantOf<MaterialVariant::Gbuffer, GO::Motion>});
    dev_.setVariantProvider(d3d9::VariantSlot::GbufferA, {materialVariantTag("gbuf_a"), gbufferVariantOf<MaterialVariant::Gbuffer, GO::None>});
    // the replacement of the game's SSAO draws (ao.mode = replace)
    const auto make = [&](const char* name, shader::Stage stage) {
        auto s = std::make_shared<d3d9::Shader>();
        s->stage = stage;
        s->module = dev_.game().createShaderModule({.spirv = shader::embedded::spirv(name), .dxil = shader::embedded::dxilOnDemand(name)});
        return s;
    };
    whiteVs_ = make("debug/fullscreen.vert", shader::Stage::Vertex);
    whitePs_ = make("gbuffer/ao_white.frag", shader::Stage::Pixel);
    whitePs_->reflection.colorOutputs = 1;
    dev_.setVariantProvider(d3d9::VariantSlot::GbufferMotion,
                            {materialVariantTag("gbufm"), gbufferVariantOf<MaterialVariant::GbufferMotion, GO::NormalMotion>});
    dev_.setVariantProvider(d3d9::VariantSlot::GbufferMotionM,
                            {materialVariantTag("gbufm_m"), gbufferVariantOf<MaterialVariant::GbufferMotion, GO::Motion>});
}

Gbuffer::~Gbuffer() {
    disable();
    if (replacing_) dev_.removeDrawReplacer(this);
    for (auto* s : {&whiteVs_, &whitePs_}) {
        if (*s && (*s)->module != rhi::ShaderModule::None) dev_.game().destroyShaderModule((*s)->module, true);
        s->reset();
    }
    // (a pipeline still being built is waited for, then retired with the others)
    for (std::future<std::unique_ptr<rhi::Pipeline>>* f : {&gtaoBuild_, &filterBuild_}) {
        if (f->valid()) f->get();
    }
}

GbufferOutputs Gbuffer::outputs() const {
    // (the debug views read both; normal without motion does not exist: the only normal reader also reads motion)
    if (s_.needNormal || s_.debug != 0) return GbufferOutputs::NormalMotion;
    return s_.needMotion ? GbufferOutputs::Motion : GbufferOutputs::None;
}

void Gbuffer::apply(const Settings& s) {
    const GbufferOutputs before = outputs();
    s_ = s;
    s_.aoStrength = std::clamp(s_.aoStrength, 0.0f, 1.0f);
    s_.aoRadius = std::clamp(s_.aoRadius, 10.0f, 1000.0f);
    if (s.ao && !dev_.game().gameCaps().storageImageExtendedFormats && !failed_) {
        spdlog::warn("enh: gbuffer: the GPU lacks shaderStorageImageExtendedFormats (RG16F storage images): AO is not available");
        failed_ = true;
    }
    const bool want = (s.ao || s.debug != 0 || s.forced) && !failed_;
    if (want && !on_) enable();
    if (!want && on_) disable();
    if (on_ && outputs() != before) {
        // targets nobody reads any more are released; the device takes the new layout at the next ensureTargets
        dev_.setGbuffer({});
        if (!writesNormal(outputs())) dev_.releaseTransient("gbuffer.normal");
        if (!writesMotion(outputs())) dev_.releaseTransient("gbuffer.motion");
    }
    if (on_ && scene_ != nullptr) scene_->setDynamic(this, writesMotion(outputs()));
    if (on_ && !s_.ao) {
        dev_.setFrameAo({});
        for (const char* k : {"gtao.raw", "gtao.history0", "gtao.history1"}) dev_.releaseTransient(k);
        historyValid_ = false;
    }
    // the game's SSAO off while ours replaces it
    const bool replace = on_ && s_.ao && s_.replaceEngineSsao && whiteVs_ && whitePs_ && whiteVs_->module != rhi::ShaderModule::None &&
                         whitePs_->module != rhi::ShaderModule::None;
    if (replace && !replacing_) dev_.addDrawReplacer(this);
    if (!replace && replacing_) dev_.removeDrawReplacer(this);
    replacing_ = replace;
    const GbufferOutputs out = outputs();
    spdlog::info("enh: gbuffer: {} (targets {}, ao {}, game SSAO {}, strength {:.2f}, radius {:.0f}, debug {})", on_ ? "on" : "off",
                 out == GbufferOutputs::NormalMotion ? "normal + motion" : out == GbufferOutputs::Motion ? "motion" : "none",
                 s_.ao ? "on" : "off", replace ? "replaced" : "kept", s_.aoStrength, s_.aoRadius, s_.debug);
}

void Gbuffer::enable() {
    on_ = true;
    historyValid_ = false;
    // the targets for the frame's solid pass (after the z pre-pass), and for the next frame (after the opaque scene)
    dev_.addPass(d3d9::inject::Point::AfterDepthPrepass, "gbuffer.targets", [this](d3d9::PassContext& ctx) { ensureTargets(ctx); }, this);
    // the AO right after the z pre-pass (the solid pass reads it this frame), the motion fill after the opaque scene
    dev_.addPass(d3d9::inject::Point::AfterDepthPrepass, "gbuffer.ao", [this](d3d9::PassContext& ctx) { recordAo(ctx); }, this, 20);
    dev_.addPass(d3d9::inject::Point::AfterOpaque, "gbuffer.motion", [this](d3d9::PassContext& ctx) { recordMotionFill(ctx); }, this, 20);
    dev_.addPass(d3d9::inject::Point::BeforeGui, "gbuffer.debug", [this](d3d9::PassContext& ctx) { recordDebug(ctx); }, this);
    // characters' own motion (the vertex capture runs while the RT scene is active: GI, RT reflections): only with a motion target (apply)
}

void Gbuffer::disable() {
    if (!on_) return;
    on_ = false;
    dev_.removePasses(this);
    dev_.setGbuffer({});
    dev_.setFrameAo({});
    if (scene_ != nullptr) scene_->setDynamic(this, false);
    for (const char* k : {"gbuffer.normal", "gbuffer.motion", "gtao.raw", "gtao.history0", "gtao.history1", kDummyAo, kDummyMotion}) {
        dev_.releaseTransient(k);
    }
    historyValid_ = false;
}

// ---- the game's SSAO (ao.mode = replace) ----

bool Gbuffer::handles(d3d9::draws::Kind, d3d9::scene::Phase phase, const d3d9::draws::Tag&) const {
    return replacing_ && phase.view == d3d9::scene::View::Main && phase.pass == d3d9::scene::Pass::ShadowMask;
}

// post_SSM.fx's SSAO and its two blurs render into an A8R8G8B8 target smaller than the scene (shadows.md 2: 800 x 450 at
// 1600 x 900); the depth downscale before them is R32F, the mask itself has the scene's size. Those draws write 1
// (unoccluded): the upscale puts 1 into the mask's red channel.
void Gbuffer::replace(d3d9::Device& device, const d3d9::ReplaceQuery&, d3d9::DrawReplacement& out) {
    out.action = d3d9::DrawReplacement::Action::Engine;
    const d3d9::Surface* rt = device.renderTarget(0);
    const d3d9::Texture* hdr = device.imageWithRole(d3d9::scene::ImageRole::SceneHdr);
    if (rt == nullptr || rt->texture() == nullptr || hdr == nullptr) return;
    constexpr std::uint32_t kA8R8G8B8 = 21;
    if (rt->texture()->desc().format != kA8R8G8B8 || rt->width() >= hdr->desc().width) return;
    out.action = d3d9::DrawReplacement::Action::Procedural;
    out.vs = whiteVs_;
    out.ps = whitePs_;
    out.vertices = 3;
    out.cullNone = true;
    out.opaque = true;
    if (ssaoReplaced_++ == 0) {
        spdlog::info("enh: gbuffer: the game's SSAO draws replaced ({}x{} target): only the GTAO shades", rt->width(), rt->height());
    }
}

// the G-buffer images at the scene target's size (Device::setGbuffer every frame: the transients keep their images while the
// size holds)
bool Gbuffer::ensureTargets(d3d9::PassContext& ctx) {
    if (ctx.mainView != 1 || ctx.sceneHdr == nullptr) return false;
    const std::uint32_t w = ctx.sceneHdr->desc().width, h = ctx.sceneHdr->desc().height;
    const GbufferOutputs out = outputs();
    rhi::Texture* n = writesNormal(out) ? dev_.transient("gbuffer.normal", {kNormalFormat, {w, h}, kNormalUsage}) : nullptr;
    rhi::Texture* m = writesMotion(out) ? dev_.transient("gbuffer.motion", {kMotionFormat, {w, h}, kMotionUsage}) : nullptr;
    if ((writesNormal(out) && n == nullptr) || (writesMotion(out) && m == nullptr)) {
        dev_.setGbuffer({});
        return false;
    }
    d3d9::Device::GbufferTargets t{n, m, w, h, true};
    // where no material writes, the fill pass puts the camera's motion; before its pipeline exists those pixels stay zero
    t.motionClear = pipelinesReady() ? d3d9::Device::kGbufferMotionUnwritten : 0.0f;
    dev_.setGbuffer(t);
    return true;
}

bool Gbuffer::pipelinesReady() {
    if (gtao_ && filter_) return true;
    if (failed_) return false;
    for (auto [future, out] : {std::pair{&gtaoBuild_, &gtao_}, std::pair{&filterBuild_, &filter_}}) {
        if (*out) continue;
        if (!future->valid() || future->wait_for(std::chrono::seconds(0)) != std::future_status::ready) return false;
        *out = future->get();
        if (!*out) {
            spdlog::error("enh: gbuffer: a compute pipeline failed: AO stays off this session");
            failed_ = true;
            return false;
        }
    }
    return true;
}

// the camera's motion in the G-buffer's motion image where no material wrote one (after the opaque scene)
void Gbuffer::recordMotionFill(d3d9::PassContext& ctx) {
    if (ctx.mainView != 1 || !on_) return;
    ensureTargets(ctx);  // for the next frame's solid pass (the z pre-pass point may not fire)
    if (!writesMotion(outputs())) return;  // (no motion target: no reader)
    d3d9::Surface* ds = dev_.depthStencil();
    rhi::CommandList& cmd = ctx.cmd;
    if (!dev_.gbufferWritten() || ctx.sceneHdr == nullptr) return;
    const std::uint32_t w = ctx.sceneHdr->desc().width, h = ctx.sceneHdr->desc().height;
    rhi::Texture* motion = dev_.transient("gbuffer.motion", {kMotionFormat, {w, h}, kMotionUsage});
    if (motion == nullptr) return;
    // (the target is cleared to the "unwritten" marker only once the fill pipeline exists: ensureTargets)
    if (ds == nullptr || ds->texture() == nullptr || !pipelinesReady() || !dev_.readable(ds->texture(), cmd)) return;
    rhi::Texture* raw = dev_.transient(kDummyAo, {VK_FORMAT_R32_SFLOAT, {1, 1}});  // (bound, not written in this mode)
    if (raw == nullptr) return;
    GbufferPush push{};
    push.h.frame = ctx.frameBlock.bits;
    push.h.passId = 2;
    push.depthTex = ds->texture()->bindlessSlot();
    push.width = std::min(w, ds->width());
    push.height = std::min(h, ds->height());
    push.flags = 2u;  // gtao.comp: the motion fill only
    RhiScope rs(dev_, cmd);
    if (!rs) return;
    rhi::CommandList& cl = rs.cl();
    rhi::Texture* motionT = rs.wrap(*motion);
    rhi::Texture* rawT = rs.wrap(*raw);
    cl.transition(*motionT, VK_IMAGE_LAYOUT_GENERAL);
    cl.transition(*rawT, VK_IMAGE_LAYOUT_GENERAL);
    cl.bindPipeline(*gtao_);
    rhi::Texture* const storage[] = {rawT, motionT};
    cl.setStorageTextures(storage);
    cl.setPushConstants(&push, sizeof(push));
    cl.dispatch((push.width + 7) / 8, (push.height + 7) / 8, 1);
    cl.transition(*motionT, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
}

// the AO of this frame from the depth after the z pre-pass (the material shaders of the solid pass read it at their pixel)
void Gbuffer::recordAo(d3d9::PassContext& ctx) {
    if (ctx.mainView != 1 || !on_) return;
    ensureTargets(ctx);
    if (!s_.ao) return;
    d3d9::Surface* ds = dev_.depthStencil();
    rhi::CommandList& cmd = ctx.cmd;
    if (ds == nullptr || ds->texture() == nullptr || ctx.sceneHdr == nullptr || !pipelinesReady() ||
        !dev_.readable(ds->texture(), cmd)) {
        ++skipped_;
        dev_.setFrameAo({});
        historyValid_ = false;
        return;
    }
    const std::uint32_t w = ctx.sceneHdr->desc().width, h = ctx.sceneHdr->desc().height;
    // (the AO reads neither G-buffer target: normals and reprojection come from the depth; the motion slot of gtao.comp is bound, not
    // written in this mode: a 1x1 stand-in)
    rhi::Texture* motion = dev_.transient(kDummyMotion, {kMotionFormat, {1, 1}, VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT});
    rhi::Texture* raw = dev_.transient("gtao.raw", {VK_FORMAT_R32_SFLOAT, {w, h}});
    const std::uint32_t cur = history_ ^ 1u;
    rhi::Texture* hist[2] = {dev_.transient("gtao.history0", {VK_FORMAT_R16G16_SFLOAT, {w, h}}),
                                      dev_.transient("gtao.history1", {VK_FORMAT_R16G16_SFLOAT, {w, h}})};
    if (motion == nullptr || raw == nullptr || hist[0] == nullptr || hist[1] == nullptr) {
        ++skipped_;
        return;
    }
    GbufferPush push{};
    push.h.frame = ctx.frameBlock.bits;
    push.h.passId = 1;
    push.depthTex = ds->texture()->bindlessSlot();
    push.linearSampler = dev_.samplerFor(true, true);
    push.width = std::min(w, ds->width());
    push.height = std::min(h, ds->height());
    push.radius = s_.aoRadius;
    push.falloff = 0.6f;
    push.strength = s_.aoStrength;
    push.blend = 0.85f;
    push.frameIndex = static_cast<std::uint32_t>(dev_.frameValue());
    push.flags = 1u;  // gtao.comp: the AO

    RhiScope rs(dev_, cmd);
    if (!rs) return;
    rhi::CommandList& cl = rs.cl();
    rhi::Texture* motionT = rs.wrap(*motion);
    rhi::Texture* rawT = rs.wrap(*raw);
    rhi::Texture* histRead = rs.wrap(*hist[history_]);
    rhi::Texture* histWrite = rs.wrap(*hist[cur]);

    // the raw AO (the motion image is bound but not written in this mode)
    cl.transition(*motionT, VK_IMAGE_LAYOUT_GENERAL);
    cl.transition(*rawT, VK_IMAGE_LAYOUT_GENERAL);
    cl.bindPipeline(*gtao_);
    rhi::Texture* const storage1[] = {rawT, motionT};
    cl.setStorageTextures(storage1);
    cl.setPushConstants(&push, sizeof(push));
    cl.dispatch((push.width + 7) / 8, (push.height + 7) / 8, 1);

    // filter + temporal into this frame's history
    cl.transition(*rawT, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    cl.transition(*histRead, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    cl.transition(*histWrite, VK_IMAGE_LAYOUT_GENERAL);
    push.aoTex = raw->heapSampled();
    push.historyTex = historyValid_ ? hist[history_]->heapSampled() : 0u;
    cl.bindPipeline(*filter_);
    rhi::Texture* const storage2[] = {histWrite};
    cl.setStorageTextures(storage2);
    cl.setPushConstants(&push, sizeof(push));
    cl.dispatch((push.width + 7) / 8, (push.height + 7) / 8, 1);
    cl.transition(*histWrite, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    history_ = cur;
    historyValid_ = true;
    // -> this frame's material shaders (Gbuffer variants: the AO at their pixel scales their ambient)
    dev_.setFrameAo({hist[cur]->heapSampled(), dev_.samplerFor(true, true), shader::abi::kFrameAoValid, std::bit_cast<std::uint32_t>(s_.aoStrength)});
    if (aoFrames_++ == 0) {
        spdlog::info("enh: gbuffer: first AO frame {}x{} (radius {:.0f}, strength {:.2f})", push.width, push.height, s_.aoRadius, s_.aoStrength);
    }
    const double now = dev_.secondsSinceCreate();
    if (now - lastLog_ >= 5.0) {
        lastLog_ = now;
        spdlog::info("enh: gbuffer: {} AO frames ({} skipped: no G-buffer this frame / inputs / pipelines), g-buffer {}", aoFrames_, skipped_,
                     dev_.gbufferArmed() ? "armed" : "building pipelines");
    }
}

void Gbuffer::recordDebug(d3d9::PassContext& ctx) {
    if (s_.debug == 0 || !dev_.gbufferWritten()) return;
    d3d9::Surface* bb = dev_.backBuffer();
    if (bb == nullptr || bb->texture() == nullptr) return;
    rhi::Texture& target = bb->texture()->rhi();
    RhiScope rs(dev_, ctx.cmd);
    if (!rs) return;
    if (debug_ && debugFormat_ != target.desc().format) debug_.reset();
    if (!debug_) {
        rhi::GraphicsPipelineDesc gd;
        gd.vs.spirv = shader::embedded::spirv("debug/fullscreen_rhi.vert");
        gd.vs.dxil = shader::embedded::dxilOnDemand("debug/fullscreen_rhi.vert");
        gd.ps.spirv = shader::embedded::spirv("gbuffer/gbuffer_debug.frag");
        gd.ps.dxil = shader::embedded::dxilOnDemand("gbuffer/gbuffer_debug.frag");
        gd.colorFormats[0] = target.desc().format;
        gd.name = "gbuffer.debug";
        debug_ = rs.device().createGraphicsPipeline(gd);
        debugFormat_ = target.desc().format;
        if (!debug_) return;
    }
    rhi::Texture* normal = nullptr;
    rhi::Texture* motion = nullptr;
    if (ctx.sceneHdr != nullptr) {
        const std::uint32_t w = ctx.sceneHdr->desc().width, h = ctx.sceneHdr->desc().height;
        normal = dev_.transient("gbuffer.normal", {kNormalFormat, {w, h}, kNormalUsage});
        motion = dev_.transient("gbuffer.motion", {kMotionFormat, {w, h}, kMotionUsage});
    }
    if (normal == nullptr || motion == nullptr) return;
    rhi::CommandList& cl = rs.cl();
    cl.transition(*rs.wrap(*normal), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    cl.transition(*rs.wrap(*motion), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    GbufferPush push{};
    push.h.frame = ctx.frameBlock.bits;
    push.h.passId = 3;
    push.normalTex = normal->heapSampled();
    push.motionTex = motion->heapSampled();
    push.linearSampler = dev_.samplerFor(false, true);
    push.mode = static_cast<std::uint32_t>(s_.debug);
    if (s_.debug == 4) push.aoTex = dev_.frameSgi()[0];  // the screen probes' factor (enh/gi)
    if (s_.debug != 4 && s_.ao && historyValid_) {
        rhi::Texture* hst = dev_.transient(history_ == 0 ? "gtao.history0" : "gtao.history1",
                                                           {VK_FORMAT_R16G16_SFLOAT, {normal->desc().extent.width, normal->desc().extent.height}});
        if (hst != nullptr) push.aoTex = hst->heapSampled();
    }
    const std::uint32_t w = bb->width(), h = bb->height();
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
    cl.bindPipeline(*debug_);
    cl.setPushConstants(&push, sizeof(push));
    cl.draw(3, 1, 0, 0);
    cl.endRendering();
    if (debugFrames_++ == 0) spdlog::info("enh: gbuffer: debug view {} ({}x{})", s_.debug, w, h);
}

}  // namespace rvk::enh
