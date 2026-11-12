#include "enh/gi.h"

#include <windows.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <format>
#include <fstream>
#include <string>

#include <spdlog/spdlog.h>

#include "core/caps.h"
#include "core/paths.h"
#include "engine/hemisphere.h"
#include "engine/symbols.h"
#include "enh/gbuffer.h"
#include "enh/material_variant.h"
#include "enh/debug_view.h"
#include "enh/rhi_scope.h"
#include "enh/rt_scene.h"
#include "shader/embedded.h"

namespace rvk::enh {

namespace {

using d3d9::Semantic;
using d3d9::draws::Kind;
using d3d9::scene::Pass;
using d3d9::scene::View;

constexpr std::int32_t kDims[3] = {32, 16, 32};  // probes per cascade along x, y (up), z
constexpr std::uint32_t kProbesPerCascade = 32 * 16 * 32;
constexpr float kSpacing[kGiCascades] = {150.0f, 300.0f, 600.0f};  // world units (cm)
constexpr float kReach[kGiCascades] = {4000.0f, 8000.0f, 16000.0f};
constexpr float kWorldLightRadius = 16000.0f;  // the world's lights whose reach touches 160 m around the camera (the outer cascade)
constexpr float kShare[kGiCascades] = {0.5f, 0.3f, 0.2f};           // of the per-frame budget
constexpr std::uint32_t kBudget[3] = {1024, 2048, 4096};            // probes per frame by gi.quality
constexpr float kDepthCap = 4.0f;                                   // depth moments: distances clamped (cells)

std::int32_t floorDiv(float v, float s) { return static_cast<std::int32_t>(std::floor(v / s)); }
std::int32_t posMod(std::int32_t a, std::int32_t m) { return ((a % m) + m) % m; }

// rvk_gi.glsl rvkGiCellOfSlot
std::array<std::int32_t, 3> cellOfSlot(const std::array<std::int32_t, 3>& origin, std::uint32_t slot) {
    const std::int32_t s = static_cast<std::int32_t>(slot);
    const std::int32_t m[3] = {s % kDims[0], s / (kDims[0] * kDims[2]), (s / kDims[0]) % kDims[2]};
    std::array<std::int32_t, 3> c{};
    for (int a = 0; a < 3; ++a) c[a] = origin[a] + posMod(m[a] - posMod(origin[a], kDims[a]), kDims[a]);
    return c;
}

std::uint64_t hashFloats(const float* v, std::size_t n) {
    std::uint64_t h = 1469598103934665603ull;
    const auto* b = reinterpret_cast<const std::uint8_t*>(v);
    for (std::size_t i = 0; i < n * sizeof(float); ++i) {
        h ^= b[i];
        h *= 1099511628211ull;
    }
    return h;
}

void split(rhi::GpuRef a, std::uint32_t* out) {
    out[0] = a.low();
    out[1] = a.high();
}
// the shader reference of an rhi::Buffer (Buffer::shaderRef: the device address on Vulkan, {heap index, offset} on D3D12)
void split(const std::array<std::uint32_t, 2>& a, std::uint32_t* out) {
    out[0] = a[0];
    out[1] = a[1];
}

}  // namespace

Gi::Gi(d3d9::Device& device, RtScene* scene) : dev_(device), scene_(scene) {
    for (Cascade& c : cascades_) c.queued.assign(kProbesPerCascade, 0);
    if (scene_ == nullptr || !scene_->supported()) return;
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
    // both compute pipelines in the background from the start: turning GI on later costs no frame
    traceBuild_ = std::async(std::launch::async, build("gi/gi_trace.comp", "gi.trace"));
    blendBuild_ = std::async(std::launch::async, build("gi/gi_blend.comp", "gi.blend"));
    dev_.setVariantProvider(d3d9::VariantSlot::Gi, {materialVariantTag("gi"), variantOptions});
    // the screen probes' passes (stage C)
    // three storage images: the trace writes the probes' ratio / correction / geometry images, the resolve the first two
    // (resolved ratio, correction + count) of them
    spTraceBuild_ = std::async(std::launch::async, build("gi/sp_trace.comp", "gi.screen trace"));
    spResolveBuild_ = std::async(std::launch::async, build("gi/sp_resolve.comp", "gi.screen resolve"));
}

Gi::~Gi() {
    disable();
    // (a pipeline still being built is waited for, then retired with the others)
    for (PipelineFuture* f : {&traceBuild_, &blendBuild_, &spTraceBuild_, &spResolveBuild_, &refBuild_}) {
        if (f->valid()) f->get();
    }
}

bool Gi::available() const { return scene_ != nullptr && scene_->supported() && !failed_; }

void Gi::apply(const Settings& s) {
    s_ = s;
    s_.quality = std::clamp(s_.quality, 0, 2);
    s_.strength = std::clamp(s_.strength, 0.0f, 1.0f);
    s_.bleed = std::clamp(s_.bleed, 0.0f, 1.0f);
    s_.directional = std::clamp(s_.directional, 0.0f, 1.0f);
    s_.shadowFill = std::clamp(s_.shadowFill, 0.0f, 1.0f);
    const bool want = s.enabled && available();
    if (want && !on_) enable();
    if (!want && on_) disable();
    s_.foliage = std::clamp(s_.foliage, 0, 2);
    if (on_) {
        scene_->setDynamic(this, s_.characters || s_.foliage > 0);
        // the wind-animated SpeedTrees (leaf cards included) within 40 / 80 m: the canopy shades and bounces (rtgi.md 22)
        scene_->setAnimated(this, s_.foliage > 0, s_.foliage == 2 ? 8000.0f : 4000.0f);
        dev_.setGiDraws({true, s_.replaceLightmaps});
    }
    if (!on_ || s_.screen == 0) {
        dev_.setFrameSgi({});
        sgiValid_ = false;
    }
    // the reference path tracer (Debug): its pipeline is built on first use, its view pass exists while a view is selected
    s_.reference = std::clamp(s_.reference, 0, 7);
    s_.referenceSpp = std::clamp(s_.referenceSpp, 1, 64);
    s_.referenceMax = std::clamp(s_.referenceMax, 1, 1000000);
    s_.referenceBounces = std::clamp(s_.referenceBounces, 1, 16);
    if (on_ && s_.reference > 0 && !refTrace_ && !refBuild_.valid()) {
        rhi::Device* rdev = dev_.rhi();
        if (rdev != nullptr) {
            refBuild_ = std::async(std::launch::async, [rdev]() {
                rhi::ComputePipelineDesc d;
                d.cs.spirv = shader::embedded::spirv("gi/gi_reference.comp");
                d.cs.dxil = shader::embedded::dxilOnDemand("gi/gi_reference.comp");
                d.name = "gi.reference";
                return rdev->createComputePipeline(d);
            });
        }
    }
    if (on_ && s_.reference > 0 && refViewPass_ == 0) {
        refViewPass_ = dev_.addPass(d3d9::inject::Point::AfterMainView, "gi.reference view", [this](d3d9::PassContext& ctx) { recordReferenceView(ctx); }, this, 10);
        refFirst_ = true;
        refAcc_ = 0;
    } else if ((!on_ || s_.reference == 0) && refViewPass_ != 0) {
        dev_.removePass(refViewPass_);
        refViewPass_ = 0;
        dev_.releaseTransient("gi.ref.view");
        refBuf_.reset();
        refW_ = refH_ = 0;
    }
    spdlog::info("enh: gi: {} (quality {}, strength {:.2f}, lightmaps {}, characters {}, screen probes {}, bleed {:.2f}, debug {})", on_ ? "on" : "off",
                 s_.quality, s_.strength, s_.replaceLightmaps ? "replaced" : "kept", s_.characters ? "on" : "off", s_.screen, s_.bleed, s_.debug);
}

void Gi::enable() {
    on_ = true;
    scene_->acquire(this);
    dev_.addDrawObserver(this);
    // after the RT scene's pass (order -100): its TLAS of this frame
    passId_ = dev_.addPass(d3d9::inject::Point::AfterOpaque, "gi", [this](d3d9::PassContext& ctx) { record(ctx); }, this, 10);
    // the screen probes right after the z pre-pass (after the RT scene's build there): the solid pass reads them this frame
    dev_.addPass(d3d9::inject::Point::AfterDepthPrepass, "gi.screen", [this](d3d9::PassContext& ctx) { recordScreen(ctx); }, this, 10);
    for (Cascade& c : cascades_) {
        c.valid = false;
        c.priority.clear();
        std::fill(c.queued.begin(), c.queued.end(), 0);
        c.cursor = 0;
    }
    haveSky_ = false;
    updatesTotal_ = 0;
    lastLog_ = -1e9;
    giDrawsBase_ = dev_.giDrawsThisInterval();
}

void Gi::disable() {
    if (!on_) return;
    on_ = false;
    dev_.setGiDraws({});
    dev_.setGiHeader({});
    dev_.setFrameSgi({});
    sgiValid_ = false;
    for (const char* k : {"sgi.probes", "sgi.resolved0", "sgi.resolved1"}) dev_.releaseTransient(k);
    dev_.removeDrawObserver(this);
    dev_.removePasses(this);
    passId_ = 0;
    refViewPass_ = 0;
    refBuf_.reset();
    refW_ = refH_ = 0;
    dev_.releaseTransient("gi.ref.view");
    haveLast_ = false;
    scene_->setAnimated(this, false, 0.0f);
    scene_->setDynamic(this, false);
    scene_->release(this);
    destroyResources();
}

// ---- P2: the material pixel shaders' GI variant -----------------------------------------------------------------------------

bool Gi::variantOptions(const d3d9::Shader& s, shader::TranslateOptions& o) { return materialVariantOptions(s, MaterialVariant::Gi, o); }

// ---- P0: the frame's light ----------------------------------------------------------------------------------------------------

bool Gi::observes(Kind kind, d3d9::scene::Phase phase) const {
    (void)kind;
    return on_ && phase.view == View::Main && phase.pass == Pass::Solid;
}

void Gi::observe(d3d9::Device& dev, const d3d9::ReplaceQuery& q) {
    const d3d9::Shader* ps = q.enginePs;
    if (ps == nullptr) return;
    const std::uint64_t frame = dev.game().memoryEpoch();  // (the frame's identity: a mid-frame flush moves the value, not the epoch; 14.39)
    if (tallyFrame_ != frame) {
        tallyFrame_ = frame;
        ambient_.clear();
        sun_.clear();
        lights_.clear();
        ambientDraws_ = 0;
    }
    const auto& amb = ps->semantics[Semantic::AmbientLight];
    if (amb.present() && amb.count >= 7 && amb.reg + 7 <= static_cast<int>(d3d9::kPsFloatConsts) && !(q.blending && q.dstBlend == 2)) {
        float regs[8][4]{};
        const auto& ind = ps->semantics[Semantic::IndoorAmbient];
        if (ind.present()) std::memcpy(regs[0], dev.psConstant(static_cast<std::uint32_t>(ind.reg)), 16);
        bool any = false;
        for (int i = 0; i < 7; ++i) {
            const float* c = dev.psConstant(static_cast<std::uint32_t>(amb.reg + i));
            std::memcpy(regs[1 + i], c, 16);
            any = any || c[0] != 0.0f || c[1] != 0.0f || c[2] != 0.0f || c[3] != 0.0f;
        }
        if (any) {
            ++ambientDraws_;
            const std::uint64_t h = hashFloats(&regs[1][0], 28);
            auto it = std::find_if(ambient_.begin(), ambient_.end(), [&](const AmbientTally& t) { return t.hash == h; });
            if (it != ambient_.end()) {
                ++it->count;
            } else if (ambient_.size() < 32) {
                AmbientTally t;
                t.hash = h;
                t.count = 1;
                std::memcpy(t.regs, regs, sizeof(regs));
                ambient_.push_back(t);
            }
        }
    }
    const auto& dir = ps->semantics[Semantic::DirLight];
    if (dir.present() && dir.reg + 3 <= static_cast<int>(d3d9::kPsFloatConsts)) {
        float regs[3][4]{};
        for (int i = 0; i < 3; ++i) std::memcpy(regs[i], dev.psConstant(static_cast<std::uint32_t>(dir.reg + i)), 16);
        if (regs[0][0] + regs[0][1] + regs[0][2] > 1e-4f) {
            const std::uint64_t h = hashFloats(&regs[0][0], 12);
            auto it = std::find_if(sun_.begin(), sun_.end(), [&](const SunTally& t) { return t.hash == h; });
            if (it != sun_.end()) {
                ++it->count;
            } else if (sun_.size() < 16) {
                SunTally t;
                t.hash = h;
                t.count = 1;
                std::memcpy(t.regs, regs, sizeof(regs));
                sun_.push_back(t);
            }
        }
    }
    const auto& pl = ps->semantics[Semantic::PointLights];
    if (pl.present() && lights_.size() < kGiMaxLights * 2) {
        const int n = std::min(pl.count / 4, 4);
        for (int i = 0; i < n; ++i) {
            const int r = pl.reg + 4 * i;
            if (r + 2 > static_cast<int>(d3d9::kPsFloatConsts)) break;
            const float* p = dev.psConstant(static_cast<std::uint32_t>(r));
            const float* c = dev.psConstant(static_cast<std::uint32_t>(r + 1));
            if (p[3] <= 0.0f || c[0] + c[1] + c[2] <= 1e-4f) continue;
            const bool known = std::any_of(lights_.begin(), lights_.end(), [&](const Light& l) {
                return std::abs(l.pos[0] - p[0]) < 4.0f && std::abs(l.pos[1] - p[1]) < 4.0f && std::abs(l.pos[2] - p[2]) < 4.0f;
            });
            if (known) continue;
            Light l;
            std::memcpy(l.pos, p, 16);
            std::memcpy(l.col, c, 16);
            lights_.push_back(l);
        }
    }
}

// the light of the frame from the tallies: the most frequent ambient set (the outdoor one when it covers a good share of
// the draws; indoors the previous outdoor sky stays), the most frequent sun, the point lights nearest the camera
void Gi::pickLight() {
    if (tallyFrame_ != dev_.game().memoryEpoch()) return;
    const auto best = std::max_element(ambient_.begin(), ambient_.end(), [](const auto& a, const auto& b) { return a.count < b.count; });
    if (best != ambient_.end() && (!haveSky_ || best->count * 10 >= ambientDraws_ * 3)) {
        std::memcpy(drawSky_, best->regs[1], sizeof(drawSky_));
        std::memcpy(drawIndoor_, best->regs[0], sizeof(drawIndoor_));
        if (!trueSky_) {
            std::memcpy(sky_, drawSky_, sizeof(sky_));
            std::memcpy(indoor_, drawIndoor_, sizeof(indoor_));
            haveSky_ = true;
        }
    }
    // the engine's own hemisphere light (rtgi.md 17): the sky of every outdoor object, whatever this frame's draws are
    engine::hemisphere::Light hemi;
    if (engine::hemisphere::latest(hemi)) {
        std::memcpy(sky_, hemi.regs, sizeof(sky_));
        std::memcpy(indoor_, hemi.indoor, sizeof(hemi.indoor));
        indoor_[3] = 0.0f;
        trueSky_ = haveSky_ = true;
    }
    const auto sun = std::max_element(sun_.begin(), sun_.end(), [](const auto& a, const auto& b) { return a.count < b.count; });
    if (sun != sun_.end()) {
        std::memcpy(sunColor_, sun->regs[0], 16);
        std::memcpy(sunDir_, sun->regs[2], 16);
    } else if (ambientDraws_ > 0) {
        std::memset(sunColor_, 0, sizeof(sunColor_));
    }
    frameLights_ = lights_;
    const auto& cam = dev_.camera().valid ? dev_.camera() : dev_.lastCamera();
    const auto dist2 = [&](const Light& l) {
        const float dx = l.pos[0] - cam.position[0], dy = l.pos[1] - cam.position[1], dz = l.pos[2] - cam.position[2];
        return dx * dx + dy * dy + dz * dz;
    };
    worldLightsUsed_ = 0;
    if (s_.lights == 1 && engine::lights::collect(cam.position.data(), kWorldLightRadius, worldScratch_, lightStats_)) {
        // every light of the cached-in world (static + dynamic) within reach of the camera; the drawn objects' lights that
        // are none of them (a light the registries do not know) are kept
        std::vector<Light> merged;
        merged.reserve(worldScratch_.size() + frameLights_.size());
        for (const auto& w : worldScratch_) {
            Light l;
            l.pos[0] = w.pos[0];
            l.pos[1] = w.pos[1];
            l.pos[2] = w.pos[2];
            l.pos[3] = 1.0f / (w.range * w.range);
            l.col[0] = w.col[0];
            l.col[1] = w.col[1];
            l.col[2] = w.col[2];
            l.col[3] = w.range;  // (the shaders read rgb only)
            merged.push_back(l);
        }
        worldLightsUsed_ = static_cast<std::uint32_t>(merged.size());
        // check of the colour units: the draws' constants of the same light (within 8 units) over the registry's
        for (const Light& d : frameLights_) {
            for (std::uint32_t i = 0; i < worldLightsUsed_; ++i) {
                const Light& l = merged[i];
                if (std::abs(l.pos[0] - d.pos[0]) < 8.0f && std::abs(l.pos[1] - d.pos[1]) < 8.0f && std::abs(l.pos[2] - d.pos[2]) < 8.0f) {
                    const float a = d.col[0] + d.col[1] + d.col[2], b = l.col[0] + l.col[1] + l.col[2];
                    if (a > 1e-4f && b > 1e-4f) {
                        ratioSum_ += a / b;
                        ++ratioCount_;
                    }
                    break;
                }
            }
        }
        for (const Light& d : frameLights_) {
            const bool known = std::any_of(merged.begin(), merged.begin() + worldLightsUsed_, [&](const Light& l) {
                return std::abs(l.pos[0] - d.pos[0]) < 8.0f && std::abs(l.pos[1] - d.pos[1]) < 8.0f && std::abs(l.pos[2] - d.pos[2]) < 8.0f;
            });
            if (!known) merged.push_back(d);
        }
        frameLights_ = std::move(merged);
    }
    // the most relevant first: luminance x the share of the light's reach that the camera is inside of (a far light with a
    // small range matters little to the cascades around the camera; a big one still lights a wall near it)
    const auto score = [&](const Light& l) {
        const float lum = (l.col[0] + l.col[1] + l.col[2]) * (1.0f / 3.0f);
        const float r2 = l.pos[3] > 0.0f ? 1.0f / l.pos[3] : 1.0f;
        return lum * r2 / (dist2(l) + r2);
    };
    std::sort(frameLights_.begin(), frameLights_.end(), [&](const Light& a, const Light& b) { return score(a) > score(b); });
    const std::size_t cap = std::clamp<std::size_t>(static_cast<std::size_t>(s_.lightCount), 4, kGiMaxLights);
    if (frameLights_.size() > cap) frameLights_.resize(cap);
    spdlog::debug("enh: gi: turn: drawn objects' lights {} world {} in use {}", lights_.size(), worldLightsUsed_, frameLights_.size());
}

// ---- P1: the probes -----------------------------------------------------------------------------------------------------------

void Gi::queue(Cascade& c, std::uint32_t slot) {
    if (c.queued[slot] != 0) return;
    c.queued[slot] = 1;
    c.priority.push_back(slot);
}

// the grids follow the camera: cells that scrolled in are traced first (their slots still hold another cell's probe:
// lookups ignore it until then)
void Gi::updateCascades(const float camera[3]) {
    for (std::uint32_t ci = 0; ci < kGiCascades; ++ci) {
        Cascade& c = cascades_[ci];
        std::array<std::int32_t, 3> o{};
        for (int a = 0; a < 3; ++a) o[a] = floorDiv(camera[a], kSpacing[ci]) - kDims[a] / 2;
        if (c.valid && o == c.origin) continue;
        bool all = !c.valid;
        for (int a = 0; a < 3 && !all; ++a) all = std::abs(o[a] - c.origin[a]) >= kDims[a];
        if (all) {
            c.priority.clear();
            std::fill(c.queued.begin(), c.queued.end(), 0);
            // nearest to the camera first (the grid's middle)
            std::vector<std::pair<std::int32_t, std::uint32_t>> order;
            order.reserve(kProbesPerCascade);
            for (std::uint32_t s = 0; s < kProbesPerCascade; ++s) {
                const auto cell = cellOfSlot(o, s);
                std::int32_t d = 0;
                for (int a = 0; a < 3; ++a) d += std::abs(cell[a] - (o[a] + kDims[a] / 2));
                order.emplace_back(d, s);
            }
            std::sort(order.begin(), order.end());
            for (const auto& [d, s] : order) queue(c, s);
        } else {
            for (std::uint32_t s = 0; s < kProbesPerCascade; ++s) {
                const auto cell = cellOfSlot(o, s);
                bool inside = true;
                for (int a = 0; a < 3; ++a) inside = inside && cell[a] >= c.origin[a] && cell[a] < c.origin[a] + kDims[a];
                if (!inside) queue(c, s);
            }
        }
        c.origin = o;
        c.valid = true;
    }
}

void Gi::selectProbes(std::vector<std::uint32_t>& out, std::uint32_t budget) {
    out.clear();
    for (std::uint32_t ci = 0; ci < kGiCascades; ++ci) {
        Cascade& c = cascades_[ci];
        const std::uint32_t share = static_cast<std::uint32_t>(static_cast<float>(budget) * kShare[ci]);
        std::uint32_t taken = 0;
        while (taken < share && !c.priority.empty()) {
            const std::uint32_t s = c.priority.front();
            c.priority.pop_front();
            c.queued[s] = 0;
            out.push_back((ci << 24) | s);
            ++taken;
        }
        // the rest round robin (the light changes: time of day, weather, characters)
        for (std::uint32_t tries = 0; taken < share && tries < kProbesPerCascade; ++tries) {
            const std::uint32_t s = c.cursor;
            c.cursor = (c.cursor + 1) % kProbesPerCascade;
            if (c.queued[s] != 0) continue;
            out.push_back((ci << 24) | s);
            ++taken;
        }
    }
}

bool Gi::pipelinesReady() {
    if (trace_ && blend_) return true;
    if (failed_) return false;
    for (auto [future, out] : {std::pair{&traceBuild_, &trace_}, std::pair{&blendBuild_, &blend_}}) {
        if (*out) continue;
        if (!future->valid() || future->wait_for(std::chrono::seconds(0)) != std::future_status::ready) return false;
        *out = future->get();
        if (!*out) {
            spdlog::error("enh: gi: a compute pipeline failed: GI stays off this session");
            failed_ = true;
            return false;
        }
    }
    spdlog::info("enh: gi: compute pipelines ready");
    return true;
}

bool Gi::ensureResources(rhi::CommandList& cmd) {
    if (probes_ != nullptr) return true;
    rhi::Device* rd = dev_.rhi();
    const std::uint64_t probes = static_cast<std::uint64_t>(kGiCascades) * kProbesPerCascade;
    const VkBufferUsageFlags usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    bool ok = rd != nullptr;
    if (ok) {
        probes_ = rd->createBuffer({probes * 128, usage, rhi::Memory::Device, "gi probes"});
        depth_ = rd->createBuffer({probes * kGiDepthTexels * 8, usage, rhi::Memory::Device, "gi depth moments"});
        rays_ = rd->createBuffer({static_cast<std::uint64_t>(kBudget[2]) * (kGiRays + 1) * 16, usage, rhi::Memory::Device, "gi rays"});
        ok = probes_ != nullptr && depth_ != nullptr && rays_ != nullptr;
    }
    if (!ok) {
        spdlog::error("enh: gi: GPU buffers failed: GI stays off this session");
        destroyResources();
        failed_ = true;
        return false;
    }
    // zero keys: every probe invalid until traced
    {
        RhiScope rs(dev_, cmd);
        if (!rs) return false;
        rs.cl().fillBuffer(*probes_, 0, probes_->desc().size, 0);
        rs.cl().fillBuffer(*depth_, 0, depth_->desc().size, 0);
        rs.cl().memoryBarrier(VK_PIPELINE_STAGE_2_CLEAR_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                              VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                              VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT);
        rs.cl().bufferBarrier(*probes_, rhi::BufferAccess::ShaderRead);
        rs.cl().bufferBarrier(*depth_, rhi::BufferAccess::ShaderRead);
    }
    spdlog::info("enh: gi: {} cascades x {} x {} x {} probes ({:.0f} / {:.0f} / {:.0f} units apart), {:.1f} MiB", kGiCascades, kDims[0], kDims[1],
                 kDims[2], kSpacing[0], kSpacing[1], kSpacing[2],
                 static_cast<double>(probes_->desc().size + depth_->desc().size + rays_->desc().size) / 1048576.0);
    return true;
}

void Gi::destroyResources() {
    // (the RHI buffers are destroyed with the frame being recorded: the GPU may still use them)
    probes_.reset();
    depth_.reset();
    rays_.reset();
    for (Cascade& c : cascades_) c.valid = false;
    diag_.reset();
}

void Gi::record(d3d9::PassContext& ctx) {
    if (ctx.mainView != 1 || !on_) return;
    const auto& cam = dev_.camera();
    if (!cam.valid || !pipelinesReady()) {
        ++skipped_;
        return;
    }
    rhi::CommandList& cmd = ctx.cmd;
    if (!ensureResources(cmd)) return;
    pickLight();
    if (!haveSky_) {
        ++skipped_;
        return;
    }
    const float camera[3] = {cam.position[0], cam.position[1], cam.position[2]};
    updateCascades(camera);
    std::vector<std::uint32_t> list;
    selectProbes(list, kBudget[s_.quality]);
    const std::uint32_t n = static_cast<std::uint32_t>(list.size());
    d3d9::Device& frames = dev_;
    const std::uint64_t frame = dev_.frameValue();

    // this frame's header (the trace / blend passes and, from now on, every FrameBlock: the material shaders' lookups)
    GiVolumeHeader hd{};
    hd.dims[0] = kDims[0];
    hd.dims[1] = kDims[1];
    hd.dims[2] = kDims[2];
    hd.dims[3] = kGiCascades;
    // the frame's header (the lookups of the material shaders and passes: read access) holds the read references; the passes that write the probes get their own copy with the writable ones
    split(probes_->shaderRef(), &hd.data[0]);
    split(depth_->shaderRef(), &hd.data[2]);
    hd.params[0] = s_.strength;
    hd.params[1] = 0.1f;  // normal bias (cells)
    hd.params[2] = 0.3f;  // view bias (cells)
    hd.params[3] = 2.0f;  // cascade blend band (cells)
    hd.info[0] = (s_.replaceLightmaps ? 0u : 1u) |  // RVK_GI_FLAG_KEEP_STATIC: gi.lightmaps = keep
                 (static_cast<std::uint32_t>(std::clamp(s_.shadowFill, 0.0f, 1.0f) * 255.0f + 0.5f) << 8);  // bits 8-15: gi.shadow_fill
    std::memcpy(hd.sky, sky_, sizeof(hd.sky));
    hd.info[1] = static_cast<std::uint32_t>(s_.debug);
    hd.info[2] = kProbesPerCascade;
    hd.info[3] = static_cast<std::uint32_t>(frame);
    for (std::uint32_t c = 0; c < kGiCascades; ++c) {
        for (int a = 0; a < 3; ++a) hd.cascade[c].origin[a] = cascades_[c].origin[a];
        hd.cascade[c].spacing[0] = kSpacing[c];
        hd.cascade[c].spacing[1] = 1.0f / kSpacing[c];
        hd.cascade[c].spacing[2] = kDepthCap;
    }
    {
        // developer experiments (-Environment 'RVK_GI_EXP=<bit mask>'): the shaders read the mask from cascade 0's spacing.w (path-tracing.md 10)
        static const std::uint32_t exp = [] {
            char buf[16] = {};
            return GetEnvironmentVariableA("RVK_GI_EXP", buf, sizeof(buf)) > 0 ? static_cast<std::uint32_t>(std::strtoul(buf, nullptr, 0)) : 0u;
        }();
        std::memcpy(&hd.cascade[0].spacing[3], &exp, sizeof(exp));
    }
    const rhi::RingAlloc ha = frames.ring(sizeof(hd), 16);
    const rhi::RingAlloc la = frames.ring(std::max<std::uint64_t>(n, 1) * 4, 16);
    const rhi::RingAlloc pa = frames.ring(sizeof(GiPassParams), 16);
    if (ha.ptr == nullptr || la.ptr == nullptr || pa.ptr == nullptr) {
        ++skipped_;
        return;
    }
    std::memcpy(ha.ptr, &hd, sizeof(hd));
    // the blend pass writes the probes: its own copy of the header with the writable references (D3D12: the UAV slots)
    const rhi::RingAlloc haRw = frames.ring(sizeof(hd), 16);
    const rhi::RingAlloc paRw = frames.ring(sizeof(GiPassParams), 16);
    if (haRw.ptr == nullptr || paRw.ptr == nullptr) {
        ++skipped_;
        return;
    }
    {
        GiVolumeHeader hp = hd;
        split(probes_->shaderRefRw(), &hp.data[0]);
        split(depth_->shaderRefRw(), &hp.data[2]);
        std::memcpy(haRw.ptr, &hp, sizeof(hp));
    }
    std::memcpy(la.ptr, list.data(), static_cast<std::size_t>(n) * 4);

    GiPassParams p{};
    split(la.ref, &p.lists[0]);
    split(rays_->shaderRefRw(), &p.lists[2]);
    split(ha.ref, &p.volume[0]);
    p.volume[2] = n;
    p.volume[3] = static_cast<std::uint32_t>(frame);
    // cut-outs solid (cheap); characters only with gi.characters (the scene holds them for the other consumers too)
    p.flags[0] = (s_.cutoutsTested ? 0u : 1u) | (s_.characters ? 0u : 4u);
    p.flags[1] = static_cast<std::uint32_t>(frameLights_.size());
    std::memcpy(&p.flags[3], &s_.emissive, sizeof(float));
    // the lights' direct light enters the probes where the baked lighting that carried it is replaced (rtgi.md 19)
    p.flags[2] = (s_.lights == 1 && s_.replaceLightmaps && worldLightsUsed_ > 0) ? 1u : 0u;
    // a uniformly random rotation of the ray set per frame (a random unit quaternion)
    {
        std::uniform_real_distribution<float> u(0.0f, 1.0f);
        const float u1 = u(rng_), u2 = u(rng_) * 6.2831853f, u3 = u(rng_) * 6.2831853f;
        const float a = std::sqrt(1.0f - u1), b = std::sqrt(u1);
        const float qx = a * std::sin(u2), qy = a * std::cos(u2), qz = b * std::sin(u3), qw = b * std::cos(u3);
        const float m[3][3] = {{1 - 2 * (qy * qy + qz * qz), 2 * (qx * qy - qz * qw), 2 * (qx * qz + qy * qw)},
                               {2 * (qx * qy + qz * qw), 1 - 2 * (qx * qx + qz * qz), 2 * (qy * qz - qx * qw)},
                               {2 * (qx * qz - qy * qw), 2 * (qy * qz + qx * qw), 1 - 2 * (qx * qx + qy * qy)}};
        for (int r = 0; r < 3; ++r) {
            for (int c = 0; c < 3; ++c) p.rot[r][c] = m[r][c];
        }
    }
    for (std::uint32_t c = 0; c < kGiCascades; ++c) p.reach[c] = kReach[c];
    p.blend[0] = 0.98f;  // irradiance hysteresis of a steady probe (the shader raises the weight of new rays where the light changed)
    p.blend[1] = 0.95f;  // depth hysteresis
    p.blend[2] = 10.0f;  // depth lobe sharpness
    p.blend[3] = 0.25f;  // back-face share: inside geometry
    p.shade[0] = 1.0f;   // bounce
    p.shade[1] = 1.0f;   // albedo scale
    p.shade[2] = 12000.0f;  // sun shadow rays (120 m)
    p.shade[3] = 0.25f;  // ray cone spread (a low mip: the mean albedo)
    std::memcpy(p.sunColor, sunColor_, 16);
    std::memcpy(p.sunDir, sunDir_, 16);
    std::memcpy(p.sky, sky_, sizeof(p.sky));
    for (std::size_t i = 0; i < frameLights_.size(); ++i) {
        std::memcpy(p.lights[i * 2], frameLights_[i].pos, 16);
        std::memcpy(p.lights[i * 2 + 1], frameLights_[i].col, 16);
    }
    std::memcpy(pa.ptr, &p, sizeof(p));
    GiPush push{};
    push.h.frame = ctx.frameBlock.bits;
    push.h.passId = passId_;
    split(pa.ref, push.params);
    GiPassParams pRw = p;
    split(haRw.ref, &pRw.volume[0]);
    std::memcpy(paRw.ptr, &pRw, sizeof(pRw));
    GiPush pushRw = push;
    split(paRw.ref, pushRw.params);

    if (n > 0) {
        RhiScope rs(dev_, cmd);
        if (!rs) return;
        rhi::CommandList& cl = rs.cl();
        // this frame's lookups (the solid pass) and last frame's passes before the probe / ray writes
        cl.computeBarrier();
        cl.bindPipeline(*trace_);
        cl.setPushConstants(&push, sizeof(push));
        cl.dispatch(n, 1, 1);
        // rays -> the blend; the trace's probe reads before the blend's writes
        cl.computeBarrier();
        // the probes and their depth moments change state for the blend (D3D12: a buffer is a shader resource for the lookups and a UAV for the blend)
        cl.bufferBarrier(*probes_, rhi::BufferAccess::ShaderWrite);
        cl.bufferBarrier(*depth_, rhi::BufferAccess::ShaderWrite);
        cl.bindPipeline(*blend_);
        cl.setPushConstants(&pushRw, sizeof(pushRw));
        cl.dispatch(n, 1, 1);
        // the probes -> the lookups of the later draws (alpha) and of the next frames, the next trace
        cl.computeBarrier();
        cl.bufferBarrier(*probes_, rhi::BufferAccess::ShaderRead);
        cl.bufferBarrier(*depth_, rhi::BufferAccess::ShaderRead);
    }
    dev_.setGiHeader({reinterpret_cast<const std::uint8_t*>(&hd), sizeof(hd)});
    // the screen probes of the next frame trace with this update's light and volume (they run before the solid pass)
    lastParams_ = p;
    lastHeader_ = hd;
    haveLast_ = true;
    ++passes_;
    probesTraced_ += n;
    if (updatesTotal_++ == 0) {
        spdlog::info("enh: gi: first update: {} probes x {} rays; light: sky L0 ({:.2f}, {:.2f}, {:.2f}), sun ({:.2f}, {:.2f}, {:.2f}) towards "
                     "({:.2f}, {:.2f}, {:.2f}), {} point lights",
                     n, kGiRays, sky_[0][3] + sky_[3][2] / 3.0f, sky_[1][3] + sky_[4][2] / 3.0f, sky_[2][3] + sky_[5][2] / 3.0f, sunColor_[0],
                     sunColor_[1], sunColor_[2], sunDir_[0], sunDir_[1], sunDir_[2], frameLights_.size());
    }
    logStats(n);
}

// ---- stage C: the screen probes (rtgi.md 11) ----

void Gi::recordScreen(d3d9::PassContext& ctx) {
    if (ctx.mainView != 1 || !on_ || s_.screen == 0 || !haveLast_) return;
    // the pipelines, the main view's depth after the z pre-pass, the RT scene built there
    if (!spTrace_ || !spResolve_) {
        for (auto [future, out] : {std::pair{&spTraceBuild_, &spTrace_}, std::pair{&spResolveBuild_, &spResolve_}}) {
            if (*out || !future->valid() || future->wait_for(std::chrono::seconds(0)) != std::future_status::ready) continue;
            *out = future->get();
        }
        if (!spTrace_ || !spResolve_) {
            ++screenSkipped_;
            return;
        }
    }
    d3d9::Surface* ds = dev_.depthStencil();
    rhi::CommandList& cmd = ctx.cmd;
    if (ctx.sceneHdr == nullptr || ds == nullptr || ds->texture() == nullptr || !dev_.rtSceneValid() || !dev_.readable(ds->texture(), cmd)) {
        ++screenSkipped_;
        dev_.setFrameSgi({});
        sgiValid_ = false;
        return;
    }
    const std::uint32_t w = std::min(ctx.sceneHdr->desc().width, ds->width()), h = std::min(ctx.sceneHdr->desc().height, ds->height());
    const std::uint32_t sceneW = ctx.sceneHdr->desc().width, sceneH = ctx.sceneHdr->desc().height;
    // low bandwidth mode: the resolved / stats images at half the scene's size (4 x 16 bytes -> 1 / 4 of it per pixel and frame; the
    // material shaders take the 2 x 2 texels around their pixel, depth-aware). The reference path tracer reads the images at its pixel: full size.
    const bool halfRes = caps::lowBandwidth(2) && s_.reference == 0;
    const std::uint32_t sw = halfRes ? (sceneW + 1) / 2 : sceneW, sh = halfRes ? (sceneH + 1) / 2 : sceneH;
    if (halfRes != sgiHalf_) {  // the images are re-created at the other size (transients are keyed by description): no history
        sgiHalf_ = halfRes;
        sgiValid_ = false;
        spdlog::info("enh: gi: screen probe images at {} size ({}x{})", halfRes ? "half" : "full", sw, sh);
    }
    // low: 16 x 16 tiles x 8 rays, medium: 16 x 16 x 16, high: 8 x 8 x 16 (1600 x 900: 5.7k / 5.7k / 22.5k probes)
    const std::uint32_t tile = s_.screen == 3 ? 8u : 16u;
    const std::uint32_t rays = s_.screen == 1 ? 8u : 16u;
    const std::uint32_t gw = (w + tile - 1) / tile, gh = (h + tile - 1) / tile;
    rhi::Texture* probes = dev_.transient("sgi.probes", {VK_FORMAT_R16G16B16A16_SFLOAT, {gw, gh}});
    rhi::Texture* probes2 = dev_.transient("sgi.probes2", {VK_FORMAT_R16G16B16A16_SFLOAT, {gw, gh}});
    rhi::Texture* geom = dev_.transient("sgi.geometry", {VK_FORMAT_R32G32B32A32_SFLOAT, {gw, gh}});
    const std::uint32_t cur = sgiHistory_ ^ 1u;
    rhi::Texture* res[2] = {dev_.transient("sgi.resolved0", {VK_FORMAT_R16G16B16A16_SFLOAT, {sw, sh}}),
                                     dev_.transient("sgi.resolved1", {VK_FORMAT_R16G16B16A16_SFLOAT, {sw, sh}})};
    // the per-pixel correction + history count, ping-pong like the resolved ratio
    rhi::Texture* stats[2] = {dev_.transient("sgi.stats0", {VK_FORMAT_R16G16B16A16_SFLOAT, {sw, sh}}),
                                       dev_.transient("sgi.stats1", {VK_FORMAT_R16G16B16A16_SFLOAT, {sw, sh}})};
    const rhi::RingAlloc pa = dev_.ring(sizeof(GiPassParams), 16);
    const rhi::RingAlloc ha = dev_.ring(sizeof(GiVolumeHeader), 16);
    if (probes == nullptr || probes2 == nullptr || geom == nullptr || res[0] == nullptr || res[1] == nullptr || stats[0] == nullptr ||
        stats[1] == nullptr || pa.ptr == nullptr || ha.ptr == nullptr) {
        ++screenSkipped_;
        return;
    }
    // the last update's light and volume header (a copy in this frame's ring)
    std::memcpy(ha.ptr, &lastHeader_, sizeof(lastHeader_));
    GiPassParams p = lastParams_;
    split(ha.ref, &p.volume[0]);
    p.volume[3] = static_cast<std::uint32_t>(dev_.frameValue());
    p.screen[0] = ds->texture()->bindlessSlot();
    p.screen[1] = geom->heapSampled();
    p.screen[2] = gw;
    p.screen[3] = gh;
    p.screen2[0] = tile;
    p.screen2[1] = rays;
    p.screen2[2] = w;
    p.screen2[3] = h;
    p.screen3[0] = probes->heapSampled();
    p.screen3[1] = sgiValid_ ? res[sgiHistory_]->heapSampled() : 0u;
    p.screen3[2] = sgiValid_ ? stats[sgiHistory_]->heapSampled() : 0u;
    p.screen3[3] = dev_.samplerFor(true, true);
    p.screen4[0] = 4000.0f;  // ray reach (world units); a ray that runs out takes the probe volume's light at its end
    p.screen4[1] = 2.0f;     // ratio cap
    p.screen4[2] = 0.95f;    // history weight
    {
        // diagnostics (-Environment 'RVK_GI_HISTORY=0'): scale of the screen probes' history weight; 0 = every frame stands alone
        // (the per-frame noise of the sampling and the spatial filter, tools/gi_noise.py; docs/research/rtgi.md 20)
        static const float scale = [] {
            char buf[16] = {};
            return GetEnvironmentVariableA("RVK_GI_HISTORY", buf, sizeof(buf)) > 0 ? std::clamp(static_cast<float>(std::atof(buf)), 0.0f, 1.0f) : 1.0f;
        }();
        p.screen4[2] *= scale;
    }
    p.screen4[3] = s_.bleed;  // colour bleeding
    p.screen5[0] = probes2->heapSampled();
    {
        static const bool diag = [] {
            char buf[8] = {};
            return GetEnvironmentVariableA("RVK_GI_DIAG", buf, sizeof(buf)) > 0 && buf[0] != '0';
        }();
        if (diag) {
            // the sky cube is sampled by the diagnostics only (rtgi.md 27): ready for sampling
            if (d3d9::Texture* cube = dev_.imageWithRole(d3d9::scene::ImageRole::SkyEnvCube); cube != nullptr && !cube->isNull()) dev_.readable(cube, cmd);
            if (diag_ == nullptr && dev_.rhi() != nullptr) {
                diag_ = dev_.rhi()->createBuffer({64 * 4, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, rhi::Memory::Readback, "gi diag"});
                if (diag_ != nullptr) std::memset(diag_->map(), 0, 64 * 4);
            }
            if (diag_ != nullptr) {
                diag_->invalidate();
                const auto* dcur = static_cast<const volatile std::uint32_t*>(diag_->map());
                for (int i = 0; i < 64; ++i) {
                    const std::uint32_t c = dcur[i];
                    diagSum_[i] += static_cast<double>(static_cast<std::uint32_t>(c - diagLast_[i]));
                    diagLast_[i] = c;
                }
                split(diag_->shaderRef(), &p.screen5[1]);
                p.screen5[3] = 1;
            }
        }
    }
    p.screen6[0] = s_.directional;
    p.screen6[1] = halfRes ? 1.0f : 0.0f;
    std::memcpy(pa.ptr, &p, sizeof(p));
    GiPush push{};
    push.h.frame = ctx.frameBlock.bits;
    push.h.passId = passId_;
    split(pa.ref, push.params);

    // the DDGI probes (last frame's update) -> these reads: ordered by the update's own end barrier
    RhiScope rs(dev_, cmd);
    if (!rs) return;
    rhi::CommandList& cl = rs.cl();
    rhi::Texture* probesT = rs.wrap(*probes);
    rhi::Texture* probes2T = rs.wrap(*probes2);
    rhi::Texture* geomT = rs.wrap(*geom);
    rhi::Texture* resRead = rs.wrap(*res[sgiHistory_]);
    rhi::Texture* statsRead = rs.wrap(*stats[sgiHistory_]);
    rhi::Texture* resWrite = rs.wrap(*res[cur]);
    rhi::Texture* statsWrite = rs.wrap(*stats[cur]);
    cl.transition(*probesT, VK_IMAGE_LAYOUT_GENERAL);
    cl.transition(*probes2T, VK_IMAGE_LAYOUT_GENERAL);
    cl.transition(*geomT, VK_IMAGE_LAYOUT_GENERAL);
    cl.bindPipeline(*spTrace_);
    {
        rhi::Texture* const outs[3] = {probesT, probes2T, geomT};
        cl.setStorageTextures(outs);
    }
    cl.setPushConstants(&push, sizeof(push));
    const std::uint32_t perGroup = 64u / rays;  // one ray per invocation
    cl.dispatch((gw * gh + perGroup - 1) / perGroup, 1, 1);
    // resolve + temporal into this frame's image
    cl.transition(*probesT, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    cl.transition(*probes2T, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    cl.transition(*geomT, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    cl.transition(*resRead, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    cl.transition(*statsRead, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    cl.transition(*resWrite, VK_IMAGE_LAYOUT_GENERAL);
    cl.transition(*statsWrite, VK_IMAGE_LAYOUT_GENERAL);
    cl.bindPipeline(*spResolve_);
    {
        rhi::Texture* const outs[2] = {resWrite, statsWrite};
        cl.setStorageTextures(outs);
    }
    cl.setPushConstants(&push, sizeof(push));
    cl.dispatch((sw + 7) / 8, (sh + 7) / 8, 1);  // (the output's size: half the scene's in low bandwidth mode)
    cl.transition(*resWrite, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    cl.transition(*statsWrite, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    sgiHistory_ = cur;
    sgiValid_ = true;
    // -> this frame's material shaders (Gbuffer variants: the factor on their DDGI lookup at their pixel)
    dev_.setFrameSgi({res[cur]->heapSampled(), dev_.samplerFor(true, true), shader::abi::kFrameSgiValid | (halfRes ? shader::abi::kFrameSgiHalf : 0u),
                      stats[cur]->heapSampled()});
    if (s_.reference > 0) recordReference(ctx, cl, push, res[cur]->heapSampled(), w, h);
    if (screenFrames_++ == 0) {
        spdlog::info("enh: gi: screen probes: {}x{} probes ({} px tiles, {} rays each) over {}x{}", gw, gh, tile, rays, w, h);
    }
}

void Gi::logDiag() {
    if (diag_ == nullptr) return;
    for (int cls = 0; cls < 2; ++cls) {
        const double* d = &diagSum_[cls * 32];
        const double n = d[0] / 1024.0;
        if (n < 1.0) continue;
        const auto m = [&](int i) { return d[i] / 1024.0 / n; };
        const double hits = std::max(m(11), 1e-9);
        spdlog::info("enh: gi diag [{}] {} probes: engine ambient {:.3f}, ddgi {:.3f}, traced {:.3f} (uncapped {:.3f}) = {:.0f}% of the engine's; "
                     "far field {:.3f} ({:.0f}% of rays; the engine's sky on them {:.3f}, far field towards +dir {:.3f}, as radiance {:.3f}); hits {:.0f}% of rays: sun {:.3f} "
                     "point {:.3f} bounce {:.3f} emissive {:.3f} (hit rays' mean radiance {:.3f}, mean albedo^2.2 {:.3f}); back faces {:.0f}%; hits "
                     "< 2 m {:.0f}%, < 10 m {:.0f}%; all rays as the engine's sky {:.3f}; sky cube: far rays (per far ray) raw {:.3f} ^2.2 {:.3f}, all rays ^2.2 {:.3f}; cube / engine sky on far rays: mean {:.3f}, rms {:.3f}",
                     cls == 0 ? "shaded" : "sun-lit", static_cast<long long>(n), m(1), m(2), m(3), m(4), m(1) > 1e-6 ? 100.0 * m(3) / m(1) : 0.0, m(5),
                     100.0 * m(10), m(15), m(16), m(20), 100.0 * m(11), m(6), m(7), m(8), m(9), m(19) / hits,
                     m(18) / hits, 100.0 * m(12), 100.0 * m(13), 100.0 * m(14), m(17), m(21) / std::max(m(10), 1e-9), m(22) / std::max(m(10), 1e-9), m(23), m(24) / std::max(m(10), 1e-9),
                     std::sqrt(m(25) / std::max(m(10), 1e-9)));
    }
    std::fill(std::begin(diagSum_), std::end(diagSum_), 0.0);
}

void Gi::logStats(std::uint32_t probes) {
    const double now = dev_.secondsSinceCreate();
    if (now - lastLog_ < 5.0) return;
    logDiag();
    const double span = lastLog_ < 0.0 ? 0.0 : now - lastLog_;
    lastLog_ = now;
    const std::uint64_t draws = dev_.giDrawsThisInterval();
    const std::uint64_t giDraws = draws - giDrawsBase_;
    giDrawsBase_ = draws;
    std::size_t pending = 0;
    for (const Cascade& c : cascades_) pending += c.priority.size();
    spdlog::info("enh: gi: {} updates ({} skipped), {} probes per frame ({} traced in the interval, {} new cells pending), GI draws {:.0f} / s; "
                 "light: sky L0 ({:.2f}, {:.2f}, {:.2f}) from {} of {} ambient draws, sun ({:.2f}, {:.2f}, {:.2f}), {} point lights; cascade 0 "
                 "origin ({}, {}, {}); sky source {}",
                 passes_, skipped_, probes, probesTraced_, pending, span > 0.0 ? static_cast<double>(giDraws) / span : 0.0,
                 sky_[0][3] + sky_[3][2] / 3.0f, sky_[1][3] + sky_[4][2] / 3.0f, sky_[2][3] + sky_[5][2] / 3.0f,
                 ambient_.empty() ? 0u : std::max_element(ambient_.begin(), ambient_.end(), [](const auto& a, const auto& b) { return a.count < b.count; })->count,
                 ambientDraws_, sunColor_[0], sunColor_[1], sunColor_[2], frameLights_.size(), cascades_[0].origin[0], cascades_[0].origin[1],
                 cascades_[0].origin[2], trueSky_ ? "engine hemisphere" : "draws");
    if (trueSky_) {
        // the hemisphere's registers against the draws' most frequent set (an outdoor object: f = 1, plus its static lights)
        double dMax = 0.0;
        for (int i = 0; i < 7; ++i) {
            for (int c = 0; c < 4; ++c) dMax = std::max(dMax, static_cast<double>(std::abs(sky_[i][c] - drawSky_[i][c])));
        }
        spdlog::info("enh: gi: hemisphere vs draws: max register difference {:.4f}; indoor ({:.3f}, {:.3f}, {:.3f}) vs c14 ({:.3f}, {:.3f}, {:.3f})",
                     dMax, indoor_[0], indoor_[1], indoor_[2], drawIndoor_[0], drawIndoor_[1], drawIndoor_[2]);
    }
    {
        spdlog::info("enh: gi: world lights: {} used of {} static ({} in the tree, {} nodes, {} unknown) + {} dynamic ({} registered); draws / registry colour ratio {:.3f} ({} matches); {} drawn lights",
                     worldLightsUsed_, lightStats_.staticNear, lightStats_.staticTotal, lightStats_.nodes, lightStats_.unknown,
                     lightStats_.dynamicNear, lightStats_.dynamicTotal, ratioCount_ > 0 ? ratioSum_ / static_cast<double>(ratioCount_) : 0.0,
                     ratioCount_, lights_.size());
        ratioSum_ = 0.0;
        ratioCount_ = 0;
        const auto& cam = dev_.camera().valid ? dev_.camera() : dev_.lastCamera();
        std::string nearest;
        std::vector<const Light*> sorted;
        for (const Light& l : frameLights_) sorted.push_back(&l);
        const auto d2 = [&](const Light* l) {
            const float dx = l->pos[0] - cam.position[0], dy = l->pos[1] - cam.position[1], dz = l->pos[2] - cam.position[2];
            return dx * dx + dy * dy + dz * dz;
        };
        std::sort(sorted.begin(), sorted.end(), [&](const Light* a, const Light* b) { return d2(a) < d2(b); });
        for (std::size_t i = 0; i < sorted.size() && i < 10; ++i) {
            nearest += std::format(" [{:.0f} u at ({:.0f} {:.0f} {:.0f}), R {:.0f}, rgb {:.2f} {:.2f} {:.2f}]", std::sqrt(d2(sorted[i])), sorted[i]->pos[0], sorted[i]->pos[1], sorted[i]->pos[2], sorted[i]->pos[3] > 0.0f ? 1.0f / std::sqrt(sorted[i]->pos[3]) : 0.0f,
                                sorted[i]->col[0], sorted[i]->col[1], sorted[i]->col[2]);
        }
        {
            float farthest = 0.0f;
            for (const Light& d : lights_) farthest = std::max(farthest, std::sqrt(d2(&d)));
            spdlog::info("enh: gi: the drawn objects' lights reach {:.0f} units from the camera (farthest of {})", farthest, lights_.size());
        }
        {
            const auto base = reinterpret_cast<std::uintptr_t>(GetModuleHandleW(L"Engine.dll"));
            spdlog::info("enh: gi: dynamic light vtables (rva): {:x} {:x} {:x} {:x}; eCPointLight_PS {:x}", lightStats_.dynamicVt[0] - base,
                         lightStats_.dynamicVt[1] - base, lightStats_.dynamicVt[2] - base, lightStats_.dynamicVt[3] - base,
                         reinterpret_cast<std::uintptr_t>(engine::sym().pointLightVftable) - base);
        }
        spdlog::info("enh: gi: lights in use {} (dynamic registered {}); camera ({:.0f}, {:.0f}, {:.0f}); nearest:{}", frameLights_.size(),
                     lightStats_.dynamicRaw, cam.position[0], cam.position[1], cam.position[2], nearest);
    }
    dev_.setStatus("gi", std::format("GI: {} probes / frame, {} new pending", probes, pending));
    if (s_.screen > 0) {
        spdlog::info("enh: gi: screen probes: {} frames ({} skipped: no G-buffer / RT scene / pipelines this frame)", screenFrames_,
                     screenSkipped_);
    }
    passes_ = skipped_ = probesTraced_ = 0;
}

}  // namespace rvk::enh
