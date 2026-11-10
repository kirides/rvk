#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <windows.h>
#include <cstring>
#include <format>

#include <spdlog/spdlog.h>

#include "d3d9/defs.h"
#include "native/own_post.h"
#include "rhi/d3d_formats.h"
#include "shader/embedded.h"

// S-7c (docs/research/frame-ownership.md 14.16): the post chain's single draw units on our own pixel shaders. A unit's composed draw (native::EffectOps) is drawn
// with the engine's vertex shader, quad, target, states, constants and textures and our port of its pixel shader (shaders/post/post_*.frag.hlsl): the port's
// d3d9::Shader carries the engine pixel shader's reflection, so the device uploads the same registers and binds the same stages. The fused DoFDown / DoF ports
// take the ToneMap draw's inputs in two free stages and five free registers (post_units.hlsli) and the ToneMap unit is not drawn.

namespace rvk::native {

namespace {

struct PortInfo {
    const char* shader;          // embedded name
    std::uint32_t samplers;      // the engine pixel shader's sampler stages (checked)
    std::uint32_t inputs;        // the varying locations the port declares (the engine's vertex shader writes them)
};
// index = PostUnit - 1, then the fused DoFDown / DoF
constexpr PortInfo kPorts[kPortCount] = {
    {"post/post_tonemap.frag", 0x3, 0x1},          {"post/post_hdri.frag", 0x7, 0x3},          {"post/post_dofdown.frag", 0x3, 0x7f},
    {"post/post_dof.frag", 0x1f, 0x3f},            {"post/post_ov_color.frag", 0x0, 0x7},      {"post/post_ov_radialcolor.frag", 0x0, 0x7},
    {"post/post_ov_radialblur.frag", 0x1, 0x7},    {"post/post_ov_image.frag", 0x1, 0x7},      {"post/post_ov_strips.frag", 0x0, 0x7},
    {"post/post_dofdown_tm.frag", 0x3, 0x7f},      {"post/post_dof_tm.frag", 0x1f, 0x3f},
};
static_assert(kPortDofDownTm == 9 && kPortDofTm == 10 && kPortCount == 11);

constexpr std::uint32_t kTmScene = 5, kTmAvg = 6;  // the fused ports' stages (post_units.hlsli RVK_POST_TM_SCENE / _AVG)
constexpr std::uint32_t kTmRegs = 16;               // c16..c20

d3d9::Texture* targetTex(const fxc::EffectDraw& d) {
    const auto* s = static_cast<const d3d9::Surface*>(d.rt0);
    return s != nullptr ? s->texture() : nullptr;
}
bool nearRel(float a, double b) { return std::abs(static_cast<double>(a) - b) <= std::max(1e-7, 2e-3 * std::abs(b)); }
bool pointClamp(const std::array<std::uint32_t, fxc::kSamplerStates>& s) {
    using namespace d3d9;
    return s[SAMP_MINFILTER] == TEXF_POINT && s[SAMP_MAGFILTER] == TEXF_POINT && s[SAMP_ADDRESSU] == TADDRESS_CLAMP && s[SAMP_ADDRESSV] == TADDRESS_CLAMP;
}

}  // namespace

PostUnit OwnPost::unitOf(std::string_view t) {
    if (t == "ToneMap") return PostUnit::ToneMap;
    if (t == "HDRI") return PostUnit::Hdri;
    if (t == "DoFDown") return PostUnit::DofDown;
    if (t == "DoF") return PostUnit::Dof;
    if (t == "TOverlayColor") return PostUnit::OvColor;
    if (t == "OverlayRadialColor") return PostUnit::OvRadialColor;
    if (t == "OverlayRadialBlur") return PostUnit::OvRadialBlur;
    if (t == "OverlayImage") return PostUnit::OvImage;
    if (t == "OverlayMovieStrips" || t == "OverlayMovieStripsAbs") return PostUnit::OvStrips;
    return PostUnit::None;
}

const char* OwnPost::unitName(PostUnit u) {
    static constexpr const char* kNames[kPostUnitCount] = {"-",        "ToneMap",      "HDRI",       "DoFDown",           "DoF",
                                                           "TOverlayColor", "OverlayRadialColor", "OverlayRadialBlur", "OverlayImage", "OverlayMovieStrips"};
    return kNames[static_cast<std::size_t>(u)];
}

void OwnPost::buildPorts() {
    // the ToneMap fold is opt-in (RVK_OWNPOST_FUSE=1): measured slower on the RX 7900 XTX (frame-ownership.md 14.16: the fused DoFDown tone-maps 16 FP16 texels per
    // output pixel, +0.02 .. 0.03 ms, more than the ToneMap pass it saves)
    noFuse_ = GetEnvironmentVariableA("RVK_OWNPOST_FUSE", nullptr, 0) == 0;
    {
        char v[8] = {};
        noUnits_ = GetEnvironmentVariableA("RVK_OWNPOST_UNITS", v, sizeof(v)) != 0 && v[0] == '0';  // (A/B: the S-7 / S-7b kernels only)
    }
    for (int i = 0; i < kPortCount; ++i) {
        const auto k = static_cast<std::size_t>(i);
        if (portState_[k] != 0) continue;
        portState_[k] = 1;
        const char* name = kPorts[k].shader;
        rhi::GameDevice* game = &dev_.game();
        portBuilding_[k] = std::async(std::launch::async, [game, name]() {
            return game->createShaderModule({.spirv = shader::embedded::spirv(name), .dxil = shader::embedded::dxilOnDemand(name)}, name);
        });
    }
}

bool OwnPost::portReady(int port) {
    const auto k = static_cast<std::size_t>(port);
    if (portState_[k] == 2) return true;
    if (portState_[k] != 1 || !portBuilding_[k].valid() || portBuilding_[k].wait_for(std::chrono::seconds(0)) != std::future_status::ready) return false;
    port_[k] = portBuilding_[k].get();
    portState_[k] = port_[k] != rhi::ShaderModule::None ? 2 : 3;
    if (portState_[k] == 3) spdlog::error("native: own post: the pixel shader {} failed: that unit stays the engine's", kPorts[k].shader);
    return portState_[k] == 2;
}

bool OwnPost::fuseReady() { return !noFuse_ && portReady(kPortDofDownTm) && portReady(kPortDofTm) && portReady(0); }

// our d3d9::Shader for `port` next to the engine's pixel shader `engine` (its reflection: registers, stages, outputs); fused: + s5 / s6 and c16..c20, no s0
d3d9::Shader* OwnPost::portShader(int port, const d3d9::Shader* engine, bool fused) {
    auto& list = portShaders_[static_cast<std::size_t>(port)];
    for (const PortShader& p : list) {
        if (p.engine == engine) return p.ours.get();
    }
    if (list.size() >= 8) return nullptr;  // (a handful of engine shaders per technique at most; more = not the expected effects)
    auto s = std::make_shared<d3d9::Shader>();
    s->stage = shader::Stage::Pixel;
    s->module.store(port_[static_cast<std::size_t>(port)], std::memory_order_release);
    s->reflection = engine->reflection;
    s->semantics = engine->semantics;
    s->hash = engine->hash ^ (0x5c7c000000000000ull + (static_cast<std::uint64_t>(port) << 40));
    s->tintFailed = true;  // (no translated variants: the bytecode is not ours)
    s->lodFailed = true;
    for (auto& v : s->variantState) v.store(3, std::memory_order_relaxed);
    if (fused) {
        s->reflection.samplers[0] = shader::TextureType::Unknown;
        s->reflection.samplers[kTmScene] = shader::TextureType::Tex2D;
        s->reflection.samplers[kTmAvg] = shader::TextureType::Tex2D;
        s->reflection.floatConstCount = std::max<std::uint32_t>(s->reflection.floatConstCount, kTmRegs + 5);
        s->reflection.windowEnd[3] = std::max<std::uint16_t>(s->reflection.windowEnd[3], static_cast<std::uint16_t>(kTmRegs + 5));
    }
    s->samplerMask = 0;
    for (std::uint32_t slot = 0; slot < s->reflection.samplers.size(); ++slot) {
        if (s->reflection.samplers[slot] != shader::TextureType::Unknown) s->samplerMask |= 1u << slot;
    }
    list.push_back({engine, s});
    return s.get();
}

bool OwnPost::toneMapFusable(const fxc::EffectDraw& tm, std::string& why) {
    using namespace d3d9;
    const Texture* t = targetTex(tm);
    const auto* scene = static_cast<const Texture*>(tm.textures[0]);
    const auto* avg = static_cast<const Texture*>(tm.textures[1]);
    if (t == nullptr || scene == nullptr || avg == nullptr || avg->desc().width != 1 || avg->desc().height != 1) {
        why = "ToneMap: target / scene / 1x1 luminance";
        return false;
    }
    const std::uint32_t w = t->desc().width, h = t->desc().height;
    if (tm.viewport.x != 0 || tm.viewport.y != 0 || tm.viewport.width != w || tm.viewport.height != h || tm.rs[RS_ALPHABLENDENABLE] != 0 ||
        (tm.rs[RS_COLORWRITEENABLE] & 15u) != 15u) {
        why = "ToneMap: not a plain full-target draw";
        return false;
    }
    if (!pointClamp(tm.ss[0]) || !pointClamp(tm.ss[1])) {
        why = "ToneMap: samplers are not point / clamp";
        return false;
    }
    // 1:1 on the scene: the taps' half texel (the VS c0 = 0.5 / SrcTextureSize) is half a texel of the target, and the quad's uv at pixel p is p / size
    float uv[4];
    if (!quadUv(tm, w, h, uv, why)) return false;
    if (!nearRel(tm.vsF[0][0], 0.5 / w) || !nearRel(tm.vsF[0][1], 0.5 / h) || std::abs(uv[0]) > 1e-4f || std::abs(uv[1]) > 1e-4f || !nearRel(uv[2], 1.0 / w) ||
        !nearRel(uv[3], 1.0 / h)) {
        why = std::format("ToneMap: not 1:1 on its {}x{} target (half texel ({}, {}), uv {} {} + p x ({}, {}))", w, h, tm.vsF[0][0], tm.vsF[0][1], uv[0], uv[1], uv[2], uv[3]);
        return false;
    }
    return true;
}

bool OwnPost::unitDraw(PostUnit u, const fxc::EffectDraw& d, const fxc::EffectDraw* tm, fxc::EffectDraw& out) {
    const auto ui = static_cast<std::size_t>(u);
    const auto refuse = [&](std::string why) {
        if (unitWhy_[ui] != why) {
            spdlog::info("native: own post: {} stays on the engine's pixel shader: {}", unitName(u), why);
            unitWhy_[ui] = std::move(why);
        }
        return false;
    };
    if (u == PostUnit::None) return false;
    if (noUnits_) return refuse("RVK_OWNPOST_UNITS=0");
    const bool fused = tm != nullptr && (u == PostUnit::DofDown || u == PostUnit::Dof);
    if (tm != nullptr && !fused) return false;
    const int port = fused ? (u == PostUnit::DofDown ? kPortDofDownTm : kPortDofTm) : static_cast<int>(u) - 1;
    if (!portReady(port)) return refuse("its port is not built yet");
    const auto* engine = static_cast<const d3d9::Shader*>(d.ps);
    if (engine == nullptr) return refuse("no pixel shader");
    const PortInfo& pi = kPorts[static_cast<std::size_t>(port)];
    if (engine->stage != shader::Stage::Pixel || (engine->samplerMask & 0xffffu) != pi.samplers || (engine->reflection.colorOutputs & 1u) == 0 || engine->reflection.writesDepth) {
        return refuse(std::format("the engine's pixel shader is not the expected one (samplers {:#x}, outputs {:#x})", engine->samplerMask & 0xffffu, engine->reflection.colorOutputs));
    }
    if (fused) {
        // the consumer samples the ToneMap image (s0) at the scene's texel grid: DoFDown's VS c0 = one texel, DoF's = half a texel of it
        const d3d9::Texture* tmImage = targetTex(*tm);
        if (tmImage == nullptr || d.textures[0] != tmImage) return refuse("the fused consumer does not sample the ToneMap image");
        const double w = tmImage->desc().width, h = tmImage->desc().height;
        const double s = u == PostUnit::DofDown ? 1.0 : 0.5;
        if (!nearRel(d.vsF[0][0], s / w) || !nearRel(d.vsF[0][1], s / h)) {
            return refuse(std::format("fused: the taps ({}, {}) are not on the {}x{} ToneMap grid", d.vsF[0][0], d.vsF[0][1], w, h));
        }
    }
    d3d9::Shader* ours = portShader(port, engine, fused);
    if (ours == nullptr) return refuse("too many engine pixel shaders for one technique");
    out = d;
    out.ps = ours;
    if (fused) {
        const d3d9::Texture* tmImage = targetTex(*tm);
        out.textures[0] = nullptr;
        out.textures[kTmScene] = tm->textures[0];
        out.ss[kTmScene] = tm->ss[0];
        out.textures[kTmAvg] = tm->textures[1];
        out.ss[kTmAvg] = tm->ss[1];
        for (int r = 0; r < 3; ++r) std::memcpy(out.psF[kTmRegs + r], tm->psF[r], 16);
        const float c19[4] = {tm->psF[3][0], tm->psF[4][0], 0.0f, 0.0f};
        const float c20[4] = {0.5f / static_cast<float>(tmImage->desc().width), 0.5f / static_cast<float>(tmImage->desc().height), 0.0f, 0.0f};
        std::memcpy(out.psF[kTmRegs + 3], c19, 16);
        std::memcpy(out.psF[kTmRegs + 4], c20, 16);
    }
    return true;
}

d3d9::Surface* OwnPost::scratchFor(d3d9::Surface* target, bool precopy) {
    if (target == nullptr || target->texture() == nullptr) return nullptr;
    d3d9::Texture* t = target->texture();
    // (by size + format: ToneMap, DoFDown, the back buffer, an overlay; the one matching is reused)
    std::unique_ptr<d3d9::Texture>* slot = nullptr;
    for (auto& s : scratch_) {
        if (s && s->desc().width == t->desc().width && s->desc().height == t->desc().height && s->desc().format == t->desc().format) slot = &s;
    }
    if (slot == nullptr) {
        for (auto& s : scratch_) {
            if (!s && slot == nullptr) slot = &s;
        }
        if (slot == nullptr) slot = &scratch_[bigCompareFrame_++ % std::size(scratch_)];
        if (*slot) dev_.release(std::move(*slot));
        d3d9::TextureDesc desc;
        desc.width = t->desc().width;
        desc.height = t->desc().height;
        desc.format = t->desc().format;
        desc.usage = d3d9::USAGE_RENDERTARGET;
        desc.pool = d3d9::POOL_DEFAULT;
        *slot = dev_.createTexture(desc);
        if (!*slot) return nullptr;
    }
    d3d9::Texture* s = slot->get();
    if (precopy) {
        bool ok = false;
        dev_.runPassNow("post.unit (compare copy)", [&](d3d9::PassContext& ctx) {
            rhi::CommandList& cmd = ctx.cmd;
            cmd.transition(t->rhi(), VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
            cmd.transition(s->rhi(), VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
            rhi::TextureCopy region{};
            region.extent = {t->desc().width, t->desc().height, 1};
            cmd.copyTexture(s->rhi(), t->rhi(), std::span<const rhi::TextureCopy>(&region, 1));
            ok = true;
        });
        if (!ok) return nullptr;
    }
    return s->surface(0, 0);
}

void OwnPost::compareUnit(const char* name, d3d9::Surface* ours, d3d9::Surface* engine) {
    if (ours == nullptr || engine == nullptr || ours->texture() == nullptr || engine->texture() == nullptr) return;
    d3d9::Texture* a = ours->texture();
    d3d9::Texture* b = engine->texture();
    // (full-resolution images every 4th frame: two 1600 x 900 compares per frame cost the CPU ~10 ms)
    if (static_cast<std::uint64_t>(b->desc().width) * b->desc().height > (1u << 20) && dev_.frameValue() % 4 != 0) return;
    // X8R8G8B8 (the back buffer): the alpha byte is not part of the picture
    const int channels = b->desc().format == rhi::d3dfmt::X8R8G8B8 ? 3 : 4;
    dev_.runPassNow("post.unit (compare)", [&](d3d9::PassContext& ctx) { recordCompare(ctx.cmd, name, a->rhi(), b->rhi(), static_cast<int>(OwnGroup::Units), channels); });
}

}  // namespace rvk::native
