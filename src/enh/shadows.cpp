#include "enh/shadows.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <format>
#include <string>

#include <spdlog/spdlog.h>

#include "enh/rt_shadows.h"
#include "shader/embedded.h"
#include "vk/pipeline.h"

namespace rvk::enh {

using d3d9::Semantic;

namespace {

std::uint64_t nameHash(const char* s) {
    std::uint64_t h = 1469598103934665603ull;
    for (; *s != 0; ++s) h = (h ^ static_cast<unsigned char>(*s)) * 1099511628211ull;
    return h;
}

float length3(const float* v) { return std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]); }

}  // namespace

bool viewDepthOfHardwareZ(const d3d9::scene::Mat4& projection, float z, float engineFar, float& depthMapUnits) {
    // perspective: clip.w = view z (m23 = 1, m33 = 0); clip.z = z * m22 + m32
    if (std::fabs(projection.at(2, 3) - 1.0f) > 1e-3f || std::fabs(projection.at(3, 3)) > 1e-3f || engineFar <= 0.0f) return false;
    const float den = z - projection.at(2, 2);
    if (std::fabs(den) < 1e-9f) return false;
    const float viewZ = projection.at(3, 2) / den;
    if (!(viewZ > 0.0f) || !std::isfinite(viewZ)) return false;
    depthMapUnits = viewZ / engineFar;
    return true;
}

Shadows::Shadows(d3d9::Device& device) : dev_(device) {}

Shadows::~Shadows() {
    if (registered_) dev_.removeDrawReplacer(this);
    destroyShaders();
}

void Shadows::destroyShaders() {
    vk::Context* ctx = &dev_.ctx();
    for (auto* s : {&vs_, &ps_}) {
        if (*s && (*s)->module != VK_NULL_HANDLE) {
            const VkShaderModule m = (*s)->module;
            dev_.frames().retire([ctx, m] { vkDestroyShaderModule(ctx->device(), m, nullptr); });
        }
        s->reset();
    }
}

bool Shadows::ensureShaders() {
    if (failed_) return false;
    if (vs_ && ps_) return true;
    const auto make = [&](const char* name, shader::Stage stage) {
        auto s = std::make_shared<d3d9::Shader>();
        s->stage = stage;
        s->module = vk::createShaderModule(dev_.ctx(), shader::embedded::spirv(name));
        s->hash = nameHash(name);
        return s;
    };
    vs_ = make("shadows/shadow_mask.vert", shader::Stage::Vertex);
    ps_ = make("shadows/shadow_mask.frag", shader::Stage::Pixel);
    ps_->reflection.colorOutputs = 1;
    if (vs_->module == VK_NULL_HANDLE || ps_->module == VK_NULL_HANDLE) {
        spdlog::error("enh: shadows: shader modules failed, enhanced shadows stay off this session");
        failed_ = true;
        destroyShaders();
        return false;
    }
    return true;
}

void Shadows::apply(const Settings& s) {
    settings_ = s;
    const bool want = s.enabled && ensureShaders();
    if (want && !registered_) {
        dev_.addDrawReplacer(this);
        loggedFirst_ = false;
    } else if (!want && registered_) {
        dev_.removeDrawReplacer(this);
    }
    registered_ = want;
    spdlog::info("enh: shadows: {} (taps {}x{}, softness {:.2f}, cascade blend {:.2f}, fade {:.2f}, bias {:.2f}, debug {})",
                 want ? "on" : "off", s.taps, s.taps, s.softness, s.blend, s.fade, s.bias, s.debug);
}

bool Shadows::handles(d3d9::draws::Kind, d3d9::scene::Phase phase, const d3d9::draws::Tag&) const {
    return settings_.enabled && phase.view == d3d9::scene::View::Main && phase.pass == d3d9::scene::Pass::ShadowMask;
}

void Shadows::replace(d3d9::Device& device, const d3d9::ReplaceQuery& q, d3d9::DrawReplacement& out) {
    out.action = d3d9::DrawReplacement::Action::Engine;
    const d3d9::Shader* ps = q.enginePs;
    if (ps == nullptr || q.textures == nullptr) return;
    const auto& sem = ps->semantics;
    if (const std::uint64_t f = device.stats().frames; f != frame_) {
        frame_ = f;
        captured_ = 0;
        drawn_ = false;
        skipFrame_ = false;
    }
    if (skipFrame_) return;
    const bool cascadeDraw = sem[Semantic::ShadowMatrix].present() && sem[Semantic::ShadowMatrix].count >= 4 &&
                             sem[Semantic::ShadowSampler].present() && sem[Semantic::MaskDepth].present();
    const bool varianceDraw = !sem[Semantic::ShadowMatrix].present() && sem[Semantic::VarianceMatrix].present() &&
                              sem[Semantic::VarianceSampler].present() && sem[Semantic::MaskDepth].present();
    const auto reg = [&](Semantic s) { return sem[s].present() ? static_cast<int>(sem[s].reg) : -1; };
    if (cascadeDraw) {
        if (drawn_) {
            out.action = d3d9::DrawReplacement::Action::Skip;  // more cascades than expected: the mask is complete
            return;
        }
        d3d9::Texture* map = q.textures[sem[Semantic::ShadowSampler].reg & 15];
        if (captured_ == 0) {
            // take the frame over only when the pass can be drawn: a perspective main camera, the depth map, a cascade map
            float probe = 0.0f;
            const auto& cam = device.camera();
            if (!cam.valid || !viewDepthOfHardwareZ(cam.projection, 0.99f, cam.engineFar, probe) || map == nullptr ||
                map->isNull() || q.textures[sem[Semantic::MaskDepth].reg & 15] == nullptr) {
                skipFrame_ = true;
                return;
            }
        }
        if (map == nullptr || captured_ >= static_cast<int>(cascades_.size())) {
            out.action = d3d9::DrawReplacement::Action::Skip;
            return;
        }
        Cascade& c = cascades_[static_cast<std::size_t>(captured_++)];
        for (int r = 0; r < 4; ++r) std::memcpy(c.rows[r], device.psConstant(static_cast<std::uint32_t>(reg(Semantic::ShadowMatrix) + r)), 16);
        c.quadZ = device.vsConstant(2)[0];  // post_SSM.fx VS: o0.z = c2.x
        c.map = map;
        const auto expected = std::clamp<std::uint32_t>(device.shadowDepthMaps(), 1u, static_cast<std::uint32_t>(cascades_.size()));
        if (static_cast<std::uint32_t>(captured_) < expected) {
            out.action = d3d9::DrawReplacement::Action::Skip;
            return;
        }
        drawMask(device, q, reg(Semantic::VarianceMatrix), reg(Semantic::MinVariance), reg(Semantic::MinLightBleeding),
                 reg(Semantic::VarianceSampler), reg(Semantic::MaskDepth), out);
        return;
    }
    if (varianceDraw && captured_ > 0 && !drawn_) {
        // fewer cascade draws than cascade maps: the mask is drawn here, over the stencil the skipped draws left at 0
        drawMask(device, q, reg(Semantic::VarianceMatrix), reg(Semantic::MinVariance), reg(Semantic::MinLightBleeding),
                 reg(Semantic::VarianceSampler), reg(Semantic::MaskDepth), out);
    }
}

void Shadows::drawMask(d3d9::Device& device, const d3d9::ReplaceQuery& q, int varianceReg, int minVarianceReg, int minBleedingReg,
                       int varianceSampler, int depthSampler, d3d9::DrawReplacement& out) {
    drawn_ = true;
    const auto& cam = device.camera();
    const d3d9::scene::Mat4& proj = cam.projection;
    ShadowBlock b{};
    const int n = captured_;
    float texelWorld[3]{};
    float prev = 0.0f;
    for (int i = 0; i < n; ++i) {
        const Cascade& c = cascades_[static_cast<std::size_t>(i)];
        std::memcpy(b.cascade[i * 4], c.rows, sizeof(c.rows));
        float split = 0.0f;
        if (!viewDepthOfHardwareZ(proj, c.quadZ, cam.engineFar, split)) split = 1.0f;
        split = std::max(split, prev + 1e-6f);
        b.split[i] = split;
        prev = split;
        const float size = static_cast<float>(std::max(1u, c.map->desc().width));
        b.texel[i][0] = size;
        b.texel[i][1] = 1.0f / size;
        // the rows act on p = (d * ndc.x, d * ndc.y, d, 1); camera space = (d F ndc.x / m00, d F ndc.y / m11, d F): per world
        // unit the texture coordinate changes by |row0 (m00, m11, 1) / F|, the shadow depth by |row2 ...|
        const float scale[3] = {proj.at(0, 0) / cam.engineFar, proj.at(1, 1) / cam.engineFar, 1.0f / cam.engineFar};
        float r0[3], r2[3];
        for (int k = 0; k < 3; ++k) {
            r0[k] = c.rows[0][k] * scale[k];
            r2[k] = c.rows[2][k] * scale[k];
        }
        const float uvPerWorld = std::max(length3(r0), 1e-12f);
        texelWorld[i] = 1.0f / (uvPerWorld * size);
        b.texel[i][3] = length3(r2) * texelWorld[i];
        // the filter spans the same world width in every cascade where it can: the coarser cascades take fewer texels (down
        // to half a texel between taps), so the far cascades' edges do not grow into blobs
        b.texel[i][2] = settings_.softness * std::clamp(texelWorld[0] / texelWorld[i], 0.5f, 1.0f);
        b.maps[i] = c.map->bindlessSlot();
        out.sampled[static_cast<std::size_t>(i)] = c.map;
    }
    b.maps[3] = static_cast<std::uint32_t>(n);
    d3d9::Texture* depth = depthSampler >= 0 ? q.textures[depthSampler & 15] : nullptr;
    d3d9::Texture* variance = varianceSampler >= 0 ? q.textures[varianceSampler & 15] : nullptr;
    b.res[0] = depth != nullptr ? depth->bindlessSlot() : 0;
    b.res[1] = device.samplerFor(false, true);
    b.res[2] = variance != nullptr && varianceReg >= 0 && !variance->isNull() ? variance->bindlessSlot() : 0;
    b.res[3] = device.samplerFor(true, true);
    out.sampled[3] = depth;
    out.sampled[4] = variance;
    if (varianceReg >= 0) {
        for (int r = 0; r < 3; ++r) std::memcpy(b.vsm[r], device.psConstant(static_cast<std::uint32_t>(varianceReg + r)), 16);
    }
    b.vsmParams[0] = minVarianceReg >= 0 ? device.psConstant(static_cast<std::uint32_t>(minVarianceReg))[0] : 1e-5f;
    b.vsmParams[1] = minBleedingReg >= 0 ? device.psConstant(static_cast<std::uint32_t>(minBleedingReg))[0] : 0.0f;
    b.vsmParams[2] = 1.0f / std::max(1e-3f, 1.0f - b.vsmParams[1]);
    b.params[0] = settings_.blend;
    b.params[1] = settings_.fade;
    b.params[2] = settings_.bias;
    b.params[3] = settings_.maxSlope;
    const d3d9::Surface* rt = device.renderTarget(0);
    const float w = rt != nullptr ? static_cast<float>(rt->width()) : 1.0f;
    const float h = rt != nullptr ? static_cast<float>(rt->height()) : 1.0f;
    b.target[0] = 1.0f / w;
    b.target[1] = 1.0f / h;
    b.target[2] = w;
    b.target[3] = h;
    device.depthRemap(b.depthRemap[0], b.depthRemap[1]);  // the vertex shader's z += x + y (reversed depth; 0 otherwise)
    b.mode[0] = static_cast<std::uint32_t>(std::clamp(settings_.taps, 1, 4));
    b.mode[1] = static_cast<std::uint32_t>(settings_.debug);
    // the ray-traced shadows of this frame (their image has the mask target's size; the engine's depth map pixel = the mask's)
    if (rt_ != nullptr) {
        const RtShadows::Frame rf = rt_->frame(device.frameValue());
        if (rf.slot != 0 && rf.width == static_cast<std::uint32_t>(w) && rf.height == static_cast<std::uint32_t>(h) && cam.engineFar > 0.0f) {
            b.rt[0] = rf.slot;
            b.rtParams[0] = rf.range / cam.engineFar;
            b.rtParams[1] = settings_.rtBlend;
        }
    }

    const vk::RingAlloc a = device.frames().allocate(sizeof(b), 16);
    std::memcpy(a.ptr, &b, sizeof(b));
    out.action = d3d9::DrawReplacement::Action::Procedural;
    out.vs = vs_;
    out.ps = ps_;
    out.vertices = 3;
    out.block = a.address;
    out.cullNone = true;
    out.opaque = false;  // the engine's states: alpha writes only, its stencil and depth test

    if (!loggedFirst_) {
        loggedFirst_ = true;
        std::string splits;
        for (int i = 0; i < n; ++i) {
            splits += std::format("{}{:.0f}", i ? ", " : "", b.split[i] * cam.engineFar);
        }
        if (b.rt[0] != 0) spdlog::info("enh: shadows: ray-traced term inside {:.0f} m (blend band {:.2f})", b.rtParams[0] * cam.engineFar / 100.0f, b.rtParams[1]);
        spdlog::info("enh: shadows: mask pass with {} cascades (map {}^2), splits {} (engine far {:.0f}), texels {:.1f} / {:.1f} / {:.1f} "
                     "units, variance map {}",
                     n, b.texel[0][0], splits, cam.engineFar, texelWorld[0], texelWorld[1], texelWorld[2], b.res[2] != 0 ? "yes" : "no");
    }
}

}  // namespace rvk::enh
