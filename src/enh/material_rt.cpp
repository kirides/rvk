#include "enh/material_rt.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>

#include <spdlog/spdlog.h>

#include "d3d9/semantics.h"
#include "enh/gbuffer.h"
#include "engine/hemisphere.h"
#include "engine/weather.h"
#include "enh/rain_wetness.h"
#include "enh/rhi_scope.h"
#include "enh/rt_scene.h"
#include "shader/embedded.h"

namespace rvk::enh {

namespace {

using d3d9::Semantic;

// the tiers (docs/research/material-rt.md 3)
struct Tier {
    std::uint32_t scale;       // screen pixels per trace pixel (per axis)
    std::uint32_t traceFlags;  // RVK_RT_TRACE_OPAQUE (1): cut-outs solid
    std::uint32_t shadows;     // 0 none, 1 cut-outs solid, 2 alpha-tested
};
constexpr Tier kTiers[3] = {
    {2, 1, 0},  // low
    {2, 0, 1},  // medium
    {1, 0, 2},  // high
};

constexpr VkFormat kImageFormat = VK_FORMAT_R16G16B16A16_SFLOAT;

}  // namespace

MaterialRt::MaterialRt(d3d9::Device& device, RtScene* scene) : dev_(device), scene_(scene) {
    if (scene_ == nullptr || !scene_->supported()) return;
    rhi::Device* rdev = dev_.rhi();
    if (rdev == nullptr) {
        failed_ = true;
        return;
    }
    // the compute pipelines in the background from the start: turning the tier on later costs no frame
    const auto build = [rdev](const char* file, const char* name) {
        return std::async(std::launch::async, [rdev, file, name]() {
            rhi::ComputePipelineDesc d;
            d.cs.spirv = shader::embedded::spirv(file);
            d.cs.dxil = shader::embedded::dxilOnDemand(file);
            d.name = name;
            return rdev->createComputePipeline(d);
        });
    };
    traceBuild_ = build("materials/material_rt.comp", "materials.rt trace");
    filterBuild_ = build("materials/material_rt_filter.comp", "materials.rt filter");
}

MaterialRt::~MaterialRt() {
    disable();
    for (std::future<std::unique_ptr<rhi::Pipeline>>* f : {&traceBuild_, &filterBuild_}) {
        if (f->valid()) f->get();
    }
}

bool MaterialRt::available() const { return scene_ != nullptr && scene_->supported() && !failed_; }

void MaterialRt::apply(const Settings& s) {
    s_ = s;
    s_.strength = std::clamp(s_.strength, 0.0f, 2.0f);
    s_.rangeMetres = std::clamp(s_.rangeMetres, 10.0f, 1000.0f);
    const int want = available() ? std::clamp(s.tier, 0, 3) : 0;
    if (want > 0 && !on_) enable();
    if (want == 0 && on_) disable();
    tier_ = want;
    if (on_) {
        spdlog::info("enh: materials.rt: tier {} (1 ray per {}x{} pixels), range {:.0f} m, strength {:.2f}, rougher than {:.2f} keep the raster", tier_,
                     kTiers[tier_ - 1].scale, kTiers[tier_ - 1].scale, s_.rangeMetres, s_.strength, s_.roughnessLimit);
    }
}

void MaterialRt::enable() {
    on_ = true;
    historyValid_ = false;
    scene_->acquire(this);
    dev_.addDrawObserver(this);
    dev_.addPass(d3d9::inject::Point::AfterOpaque, "materials.rt", [this](d3d9::PassContext& ctx) { record(ctx); }, this, 30);
}

void MaterialRt::disable() {
    if (!on_) return;
    on_ = false;
    tier_ = 0;
    dev_.removePasses(this);
    dev_.removeDrawObserver(this);
    scene_->release(this);
    for (const char* k : {"matrt.raw", "matrt.hist0", "matrt.hist1"}) dev_.releaseTransient(k);
    historyValid_ = false;
}

// ---- the frame's light: the sun and the fog the lit draws carry ----

bool MaterialRt::observes(d3d9::draws::Kind, d3d9::scene::Phase phase) const {
    return on_ && phase.view == d3d9::scene::View::Main && phase.pass == d3d9::scene::Pass::Solid;
}

void MaterialRt::observe(d3d9::Device& dev, const d3d9::ReplaceQuery& q) {
    const d3d9::Shader* ps = q.enginePs;
    if (ps == nullptr) return;
    const std::uint64_t frame = dev.frameValue();
    if (lightFrame_ != frame) {
        lightFrame_ = frame;
        // (the sun of the last frame stays until a draw shows a new one: indoors no draw carries one)
    }
    const auto& dir = ps->semantics[Semantic::DirLight];
    if (dir.present() && dir.reg + 3 <= static_cast<int>(d3d9::kPsFloatConsts)) {
        const float* c = dev.psConstant(static_cast<std::uint32_t>(dir.reg));
        if (c[0] + c[1] + c[2] > 1e-4f) {
            std::memcpy(sunColor_, c, 16);
            std::memcpy(sunDir_, dev.psConstant(static_cast<std::uint32_t>(dir.reg + 2)), 16);
        }
    }
    const auto& fc = ps->semantics[Semantic::FogColor];
    const auto& fs = ps->semantics[Semantic::FogSettings];
    if (fc.present() && fs.present()) {
        std::memcpy(fogColor_, dev.psConstant(static_cast<std::uint32_t>(fc.reg)), 16);
        std::memcpy(fogParams_, dev.psConstant(static_cast<std::uint32_t>(fs.reg)), 16);
        haveFog_ = true;
    }
}

// ---- the pass ----

bool MaterialRt::pipelinesReady(VkFormat target) {
    if (failed_) return false;
    for (auto [future, out] : {std::pair{&traceBuild_, &trace_}, std::pair{&filterBuild_, &filter_}}) {
        if (*out) continue;
        if (!future->valid() || future->wait_for(std::chrono::seconds(0)) != std::future_status::ready) return false;
        *out = future->get();
        if (!*out) {
            spdlog::error("enh: materials.rt: a compute pipeline failed: the traced reflections stay off this session");
            failed_ = true;
            return false;
        }
    }
    rhi::Device* rdev = dev_.rhi();
    if (composite_ && compositeFormat_ != target) composite_.reset();
    if (!composite_ && rdev != nullptr) {
        rhi::GraphicsPipelineDesc gd;
        gd.vs.spirv = shader::embedded::spirv("debug/fullscreen_rhi.vert");
        gd.vs.dxil = shader::embedded::dxilOnDemand("debug/fullscreen_rhi.vert");
        gd.ps.spirv = shader::embedded::spirv("materials/material_rt.frag");
        gd.ps.dxil = shader::embedded::dxilOnDemand("materials/material_rt.frag");
        gd.colorFormats[0] = target;
        // result = src.rgb + dst * src.a (the reflection's light added, the surface under a puddle darkened); the target's alpha kept
        gd.blend.blendEnable = VK_TRUE;
        gd.blend.srcColorBlendFactor = VK_BLEND_FACTOR_ONE;
        gd.blend.dstColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
        gd.blend.colorBlendOp = VK_BLEND_OP_ADD;
        gd.blend.srcAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
        gd.blend.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
        gd.blend.alphaBlendOp = VK_BLEND_OP_ADD;
        gd.name = "materials.rt composite";
        composite_ = rdev->createGraphicsPipeline(gd);
        compositeFormat_ = target;
        if (!composite_) {
            spdlog::error("enh: materials.rt: the composite pipeline failed: the traced reflections stay off this session");
            failed_ = true;
            return false;
        }
    }
    return trace_ && filter_ && composite_;
}

void MaterialRt::record(d3d9::PassContext& ctx) {
    if (ctx.mainView != 1 || tier_ == 0 || ctx.sceneHdr == nullptr || ctx.target != ctx.sceneHdr) return;
    rhi::CommandList& cmd = ctx.cmd;
    d3d9::Texture* scene = ctx.sceneHdr;
    if (!dev_.rtSceneValid() || ctx.depthMap == nullptr || !dev_.gbufferWritten() || !pipelinesReady(scene->rhi().desc().format)) {
        ++skipped_;
        historyValid_ = false;
        return;
    }
    const std::uint32_t w = scene->desc().width, h = scene->desc().height;
    rhi::Texture* normal = dev_.transient("gbuffer.normal", {kGbufferNormalFormat, {w, h}, kGbufferNormalUsage});
    rhi::Texture* motion = dev_.transient("gbuffer.motion", {kGbufferMotionFormat, {w, h}, kGbufferMotionUsage});
    const Tier& t = kTiers[tier_ - 1];
    const std::uint32_t tw = (w + t.scale - 1) / t.scale, th = (h + t.scale - 1) / t.scale;
    rhi::Texture* raw = dev_.transient("matrt.raw", {kImageFormat, {tw, th}});
    rhi::Texture* hist[2] = {dev_.transient("matrt.hist0", {kImageFormat, {w, h}}), dev_.transient("matrt.hist1", {kImageFormat, {w, h}})};
    if (normal == nullptr || motion == nullptr || raw == nullptr || hist[0] == nullptr || hist[1] == nullptr || !dev_.readable(ctx.depthMap, cmd)) {
        ++skipped_;
        historyValid_ = false;
        return;
    }
    if (historyW_ != w || historyH_ != h) {
        historyValid_ = false;
        historyW_ = w;
        historyH_ = h;
    }
    const std::uint32_t cur = history_ ^ 1u;
    const rhi::RingAlloc pa = dev_.ring(sizeof(MaterialRtParams), 16);
    if (pa.ptr == nullptr) {
        ++skipped_;
        return;
    }
    MaterialRtParams p{};
    p.tex[0] = ctx.depthMap->bindlessSlot();
    p.tex[1] = normal->heapSampled();
    p.tex[2] = scene->bindlessSlot();
    p.tex[3] = dev_.samplerFor(true, true);
    p.tex2[0] = dev_.samplerFor(false, true);
    p.tex2[1] = motion->heapSampled();
    p.tex2[2] = historyValid_ ? hist[history_]->heapSampled() : 0u;
    p.tex2[3] = raw->heapSampled();
    p.info[0] = tw;
    p.info[1] = th;
    p.info[2] = t.scale;
    p.info[3] = static_cast<std::uint32_t>(dev_.frameValue());
    p.trace[0] = t.traceFlags;
    p.trace[1] = t.shadows;
    p.trace[2] = static_cast<std::uint32_t>(s_.tweaks);
    p.trace[3] = static_cast<std::uint32_t>(s_.debug);
    p.reach[0] = s_.rangeMetres * 100.0f;
    p.reach[1] = 0.0015f;
    p.reach[2] = 20000.0f;
    p.reach[3] = s_.roughnessLimit;
    p.strength[0] = s_.environment;
    p.strength[1] = s_.strength;
    updateRain();
    p.strength[2] = wet_ * s_.rain;
    p.strength[3] = s_.puddles;
    std::memcpy(p.fogColor, fogColor_, 16);
    std::memcpy(p.fogParams, fogParams_, 16);
    if (!haveFog_) p.fogParams[2] = 0.0f;  // no fog seen yet: none
    engine::hemisphere::Light hemi;
    if (engine::hemisphere::latest(hemi)) {
        std::memcpy(p.sh, hemi.regs, sizeof(p.sh));
        std::memcpy(p.indoor, hemi.indoor, sizeof(hemi.indoor));
    }
    std::memcpy(p.sunColor, sunColor_, 16);
    std::memcpy(p.sunDir, sunDir_, 16);
    std::memcpy(pa.ptr, &p, sizeof(p));
    MaterialRtPush push{};
    push.h.frame = ctx.frameBlock.bits;
    push.h.passId = 40;
    push.params[0] = pa.ref.low();
    push.params[1] = pa.ref.high();

    RhiScope rs(dev_, cmd);
    if (!rs) {
        ++skipped_;
        return;
    }
    rhi::CommandList& cl = rs.cl();
    rhi::Texture* sceneT = rs.wrap(*scene);
    rhi::Texture* rawT = rs.wrap(*raw);
    rhi::Texture* histRead = rs.wrap(*hist[history_]);
    rhi::Texture* histWrite = rs.wrap(*hist[cur]);
    cl.transition(*rs.wrap(*normal), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    cl.transition(*rs.wrap(*motion), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    cl.transition(*sceneT, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);

    // the traced image (a quarter of the pixels at low / medium)
    cl.transition(*rawT, VK_IMAGE_LAYOUT_GENERAL);
    cl.bindPipeline(*trace_);
    rhi::Texture* const storage1[] = {rawT};
    cl.setStorageTextures(storage1);
    cl.setPushConstants(&push, sizeof(push));
    cl.dispatch((tw + 7) / 8, (th + 7) / 8, 1);

    // the gather + temporal filter into this frame's history
    cl.transition(*rawT, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    if (historyValid_) cl.transition(*histRead, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    cl.transition(*histWrite, VK_IMAGE_LAYOUT_GENERAL);
    cl.bindPipeline(*filter_);
    rhi::Texture* const storage2[] = {histWrite};
    cl.setStorageTextures(storage2);
    cl.setPushConstants(&push, sizeof(push));
    cl.dispatch((w + 7) / 8, (h + 7) / 8, 1);
    cl.transition(*histWrite, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);

    // onto the scene
    MaterialRtCompositePush cp{};
    cp.h.frame = ctx.frameBlock.bits;
    cp.h.passId = 41;
    cp.image = hist[cur]->heapSampled();
    cp.sampler = dev_.samplerFor(false, true);
    cl.transition(*sceneT, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    rhi::RenderingInfo ri;
    ri.color[0].texture = sceneT;
    ri.color[0].load = VK_ATTACHMENT_LOAD_OP_LOAD;
    ri.color[0].store = VK_ATTACHMENT_STORE_OP_STORE;
    ri.colorCount = 1;
    ri.width = w;
    ri.height = h;
    cl.beginRendering(ri);
    cl.bindPipeline(*composite_);
    cl.setPushConstants(&cp, sizeof(cp));
    cl.draw(3, 1, 0, 0);
    cl.endRendering();

    history_ = cur;
    historyValid_ = true;
    lastTraceW_ = tw;
    lastTraceH_ = th;
    if (passes_++ == 0) {
        spdlog::info("enh: materials.rt: first reflection frame: trace {}x{} (tier {}), image {}x{}", tw, th, tier_, w, h);
    }
    logStats();
}

// the rain share of the weather (about once a second: the weather admin's states, SEH-guarded engine reads) -> the wetness level
void MaterialRt::updateRain() {
    const double now = dev_.secondsSinceCreate();
    if (s_.forceRain > 0.0f) {
        wet_ = rainShare_ = std::clamp(s_.forceRain, 0.0f, 1.0f);  // (a test: at once)
        lastWetTime_ = now;
        return;
    }
    if (now - lastWeather_ >= 1.0) {
        lastWeather_ = now;
        const std::vector<engine::WeatherState> states = engine::weatherStates();
        rainShare_ = engine::rainOfWeather(states);
    }
    const float dt = lastWetTime_ < 0.0 ? 0.0f : static_cast<float>(now - lastWetTime_);
    lastWetTime_ = now;
    const float before = wet_;
    wet_ = rain::wetnessStep(wet_, rainShare_, dt);
    if ((before < 0.02f) != (wet_ < 0.02f)) {
        spdlog::info("enh: materials.rt: the world is {} (rain share {:.2f}, wetness {:.2f})", wet_ < 0.02f ? "dry" : "getting wet", rainShare_, wet_);
    }
}

void MaterialRt::logStats() {
    const double now = dev_.secondsSinceCreate();
    if (now - lastLog_ < 5.0) return;
    lastLog_ = now;
    spdlog::info("enh: materials.rt: tier {}, trace {}x{}, {} passes ({} skipped: no RT scene / G-buffer / pipelines yet), sun ({:.2f}, {:.2f}, {:.2f}), rain {:.2f} wetness {:.2f}",
                 tier_, lastTraceW_, lastTraceH_, passes_, skipped_, sunColor_[0], sunColor_[1], sunColor_[2], rainShare_, wet_);
}

}  // namespace rvk::enh
