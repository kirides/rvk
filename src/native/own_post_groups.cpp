#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <cstring>
#include <format>

#include <spdlog/spdlog.h>

#include "d3d9/defs.h"
#include "native/own_post.h"
#include "shader/embedded.h"

// S-7b (docs/research/frame-ownership.md 14.8b): the bloom chain and the DoF middle of the scene graph as compute kernels, on the mechanism of S-7a (own_post.cpp): the
// units are composed and held by native::EffectOps, their composed draws are the source of every parameter, a complete group runs here, a broken one is flushed as
// the engine's draws.

namespace rvk::native {

namespace {

constexpr std::string_view kBloomNames[kBloomUnits] = {"BrightPass BrightPassAlphaMask", "GlowH GlowH", "Streaks Streak", "Bloom GlowVAddBloom2"};
constexpr std::string_view kDofNames[kDofMidUnits] = {"BlurH DoFBlurCocH", "BlurV DoFBlurCocV", "CalcNearCoc DoFNearCoc", "SmallBlur DoFSmallBlur"};

d3d9::Texture* targetOf(const fxc::EffectDraw& d) {
    const auto* s = static_cast<const d3d9::Surface*>(d.rt0);
    return s != nullptr ? s->texture() : nullptr;
}
d3d9::Texture* texOf(const fxc::EffectDraw& d, std::size_t stage) { return const_cast<d3d9::Texture*>(static_cast<const d3d9::Texture*>(d.textures[stage])); }

bool nearTo(float a, double b, double rel = 2e-3, double absTol = 1e-6) { return std::abs(static_cast<double>(a) - b) <= std::max(absTol, rel * std::abs(b)); }

// the unit draws a plain full-target quad: no blending (or the bloom's SRCALPHA / INVSRCALPHA), no depth / stencil / scissor, all channels, the target's viewport
bool plain(const fxc::EffectDraw& d, bool blend, std::uint32_t w, std::uint32_t h, std::string& why) {
    using namespace d3d9;
    const auto& r = d.rs;
    if (d.depth != nullptr && r[RS_ZENABLE] != 0) {
        why = "depth test on";
        return false;
    }
    if (r[RS_STENCILENABLE] != 0 || r[RS_SCISSORTESTENABLE] != 0 || r[RS_SRGBWRITEENABLE] != 0 || (r[RS_COLORWRITEENABLE] & 15u) != 15u) {
        why = "stencil / scissor / sRGB write / colour mask";
        return false;
    }
    if (blend) {
        if (r[RS_ALPHABLENDENABLE] == 0 || r[RS_SRCBLEND] != BLEND_SRCALPHA || r[RS_DESTBLEND] != BLEND_INVSRCALPHA || r[RS_SEPARATEALPHABLENDENABLE] != 0 ||
            (r[RS_BLENDOP] != BLENDOP_ADD && r[RS_BLENDOP] != 0)) {
            why = "the blend is not SRCALPHA / INVSRCALPHA";
            return false;
        }
    } else if (r[RS_ALPHABLENDENABLE] != 0) {
        why = "blending on";
        return false;
    }
    if (d.viewport.x != 0 || d.viewport.y != 0 || d.viewport.width != w || d.viewport.height != h) {
        why = std::format("viewport {}x{} at ({}, {}), expected {}x{}", d.viewport.width, d.viewport.height, d.viewport.x, d.viewport.y, w, h);
        return false;
    }
    return true;
}

bool sampler(const fxc::EffectDraw& d, std::size_t stage, std::uint32_t filter, std::string& why) {
    using namespace d3d9;
    const auto& s = d.ss[stage];
    if (s[SAMP_MINFILTER] != filter || s[SAMP_MAGFILTER] != filter || s[SAMP_ADDRESSU] != TADDRESS_CLAMP || s[SAMP_ADDRESSV] != TADDRESS_CLAMP) {
        why = std::format("sampler {}: filter {}/{} address {}/{}, expected {} + clamp", stage, s[SAMP_MINFILTER], s[SAMP_MAGFILTER], s[SAMP_ADDRESSU], s[SAMP_ADDRESSV], filter);
        return false;
    }
    return true;
}

std::uint32_t groups8(std::uint32_t n) { return (n + 7) / 8; }

}  // namespace

bool OwnPost::groupUnit(std::string_view name, int& group, int& index) {
    const int l = lumUnit(name);
    if (l >= 0) {
        group = static_cast<int>(OwnGroup::Lum);
        index = l;
        return true;
    }
    for (int i = 0; i < kBloomUnits; ++i) {
        if (name == kBloomNames[i]) {
            group = static_cast<int>(OwnGroup::Bloom);
            index = i;
            return true;
        }
    }
    for (int i = 0; i < kDofMidUnits; ++i) {
        if (name == kDofNames[i]) {
            group = static_cast<int>(OwnGroup::DofMid);
            index = i;
            return true;
        }
    }
    return false;
}

int OwnPost::groupSize(int group) {
    switch (static_cast<OwnGroup>(group)) {
    case OwnGroup::Lum: return kLumUnits;
    case OwnGroup::Bloom: return kBloomUnits;
    case OwnGroup::DofMid: return kDofMidUnits;
    default: return 0;
    }
}

bool OwnPost::groupReady(int group) {
    if (static_cast<OwnGroup>(group) == OwnGroup::Lum) return lumReady();
    if (kernFailed_) return false;
    const int first = static_cast<OwnGroup>(group) == OwnGroup::Bloom ? 0 : 3;
    const int last = static_cast<OwnGroup>(group) == OwnGroup::Bloom ? 2 : 3;
    for (int i = first; i <= last; ++i) {
        auto& f = kernBuilding_[static_cast<std::size_t>(i)];
        if (kern_[static_cast<std::size_t>(i)]) continue;
        if (!f.valid()) return false;
        if (f.wait_for(std::chrono::seconds(0)) != std::future_status::ready) return false;
        kern_[static_cast<std::size_t>(i)] = f.get();
        if (!kern_[static_cast<std::size_t>(i)]) {
            spdlog::error("native: own post: a bloom / DoF compute pipeline failed: those units stay the engine's");
            kernFailed_ = true;
            return false;
        }
        spdlog::info("native: own post: kernel {} ready", i);
    }
    return true;
}

bool OwnPost::runGroup(int group, const std::array<const fxc::EffectDraw*, kMaxGroupUnits>& rec, bool write) {
    switch (static_cast<OwnGroup>(group)) {
    case OwnGroup::Lum: {
        std::array<const fxc::EffectDraw*, kLumUnits> r{};
        for (int i = 0; i < kLumUnits; ++i) r[static_cast<std::size_t>(i)] = rec[static_cast<std::size_t>(i)];
        return runLum(r, write);
    }
    case OwnGroup::Bloom: return runBloom(rec, write);
    case OwnGroup::DofMid: return runDofMid(rec, write);
    default: return false;
    }
}

void OwnPost::finishCompare(int group) {
    if (mode_ != Mode::Compare) return;
    rhi::Device* rdev = dev_.rhi();
    if (rdev == nullptr) return;
    if (static_cast<OwnGroup>(group) == OwnGroup::Bloom && bloomOurs_ != nullptr && bloomEngine_ != nullptr) {
        rhi::Texture* ours = bloomOurs_;
        d3d9::Texture* eng = bloomEngine_;
        bloomOurs_ = nullptr;
        bloomEngine_ = nullptr;
        dev_.runPassNow("post.bloom (compare 2)", [&](d3d9::PassContext& ctx) { recordCompare(ctx.cmd, "bloom", *ours, eng->rhi()); });
    } else if (static_cast<OwnGroup>(group) == OwnGroup::DofMid && dofOursS_ != nullptr && dofEngineS_ != nullptr) {
        rhi::Texture* ours = dofOursS_;
        d3d9::Texture* eng = dofEngineS_;
        dofOursS_ = nullptr;
        dofEngineS_ = nullptr;
        dev_.runPassNow("post.dofmid (compare 2)", [&](d3d9::PassContext& ctx) { recordCompare(ctx.cmd, "small blur", *ours, eng->rhi()); });
    }
}

// ---- the compare machinery ----

void OwnPost::recordCompare(rhi::CommandList& cmd, const char* name, rhi::Texture& ours, rhi::Texture& engine) {
    rhi::Device* rdev = dev_.rhi();
    if (rdev == nullptr) return;
    constexpr std::uint64_t kSlotBytes = 8ull << 20;
    if (!cmpBuf_) {
        cmpBuf_ = rdev->createBuffer({kSlotBytes * rhi::kFramesInFlight, VK_BUFFER_USAGE_TRANSFER_DST_BIT, rhi::Memory::Readback, "own post compare 2"});
        if (!cmpBuf_) return;
        std::memset(cmpBuf_->map(), 0, kSlotBytes * rhi::kFramesInFlight);
    }
    const std::uint32_t slot = dev_.frameSlot();
    // the entries of the last frame that used this slot are complete (the slot's fence was waited for at the frame's start)
    if (cmpFrame_[slot] != dev_.frameValue()) {
        evalCompares(slot);
        cmpFrame_[slot] = dev_.frameValue();
        cmpCursor_[slot] = 0;
    }
    const VkExtent3D e = ours.desc().extent;
    const VkExtent3D e2 = engine.desc().extent;
    if (e.width != e2.width || e.height != e2.height) return;
    const std::uint32_t pitch = rhi::bufferRowPitch(ours.desc().format, e.width);
    const std::uint32_t pitch2 = rhi::bufferRowPitch(engine.desc().format, e.width);
    const auto align = [](std::uint64_t v) { return (v + 511) & ~std::uint64_t{511}; };
    const std::uint64_t bytes = align(static_cast<std::uint64_t>(pitch) * e.height), bytes2 = align(static_cast<std::uint64_t>(pitch2) * e.height);
    if (cmpCursor_[slot] + bytes + bytes2 > kSlotBytes) return;
    const std::uint64_t base = kSlotBytes * slot + cmpCursor_[slot];
    cmd.transition(ours, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    cmd.copyTextureToBuffer(*cmpBuf_, base, ours);
    cmd.transition(engine, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    cmd.copyTextureToBuffer(*cmpBuf_, base + bytes, engine);
    Pending p{};
    p.name = name;
    p.w = e.width;
    p.h = e.height;
    p.pitch = pitch;
    p.offOurs = static_cast<std::uint32_t>(base);
    p.offEngine = static_cast<std::uint32_t>(base + bytes);
    p.engineBgra = engine.desc().format == VK_FORMAT_B8G8R8A8_UNORM;
    p.group = std::string_view(name) == "bloom" || std::string_view(name) == "bright" || std::string_view(name) == "glow" || std::string_view(name) == "streak"
                  ? static_cast<int>(OwnGroup::Bloom)
                  : static_cast<int>(OwnGroup::DofMid);
    cmpCursor_[slot] += static_cast<std::uint32_t>(bytes + bytes2);
    cmpPending_[slot].push_back(p);
}

void OwnPost::evalCompares(std::uint32_t slot) {
    if (!cmpBuf_ || cmpPending_[slot].empty()) return;
    cmpBuf_->invalidate();
    const auto* base = static_cast<const std::uint8_t*>(cmpBuf_->map());
    for (const Pending& p : cmpPending_[slot]) {
        GroupStats& g = gstats_[static_cast<std::size_t>(p.group)];
        double sumDiff = 0.0, sumEng = 0.0;
        std::uint32_t maxStep = 0;
        std::uint64_t ch1[4]{}, ch2[4]{};
        std::uint64_t off = 0;
        for (std::uint32_t y = 0; y < p.h; ++y) {
            const std::uint8_t* a = base + p.offOurs + static_cast<std::size_t>(y) * p.pitch;
            const std::uint8_t* b = base + p.offEngine + static_cast<std::size_t>(y) * p.pitch;
            for (std::uint32_t x = 0; x < p.w; ++x) {
                bool pixelOff = false;
                for (int c = 0; c < 4; ++c) {
                    const int ec = p.engineBgra ? (c == 0 ? 2 : c == 2 ? 0 : c) : c;  // the engine's memory order -> RGBA
                    const int va = a[x * 4 + static_cast<std::uint32_t>(c)], vb = b[x * 4 + static_cast<std::uint32_t>(ec)];
                    const std::uint32_t d = static_cast<std::uint32_t>(std::abs(va - vb));
                    sumDiff += d;
                    sumEng += vb;
                    if (d >= 1) ++ch1[c];
                    if (d >= 2) ++ch2[c];
                    maxStep = std::max(maxStep, d);
                    if (d > 1) pixelOff = true;
                    if (d > 1 && dumped_ < 24 && GetEnvironmentVariableA("RVK_OWNPOST_DUMP", nullptr, 0) != 0) {
                        ++dumped_;
                        spdlog::info("native: own post: dump {} ({}, {}) c{}: ours {} engine {}", p.name, x, y, c, va, vb);
                    }
                }
                if (pixelOff) ++off;
            }
        }
        const double rel = sumDiff / std::max(sumEng, 1.0);
        if (dumped_ < 24 && GetEnvironmentVariableA("RVK_OWNPOST_DUMP", nullptr, 0) != 0 && std::string_view(p.name) == "bright") {
            spdlog::info("native: own post: dump {}: channels differing >= 1 step: {} {} {} {}, >= 2: {} {} {} {} (of {})", p.name, ch1[0], ch1[1], ch1[2], ch1[3], ch2[0], ch2[1], ch2[2], ch2[3],
                         static_cast<std::uint64_t>(p.w) * p.h);
        }
        {
            GroupStats& n = named_[p.name];
            ++n.compared;
            n.sumRel += rel;
            n.maxRel = std::max(n.maxRel, rel);
            n.maxStep = std::max(n.maxStep, maxStep);
            n.pixels += static_cast<std::uint64_t>(p.w) * p.h;
            n.offPixels += off;
        }
        ++g.compared;
        g.sumRel += rel;
        g.maxRel = std::max(g.maxRel, rel);
        g.maxStep = std::max(g.maxStep, maxStep);
        g.pixels += static_cast<std::uint64_t>(p.w) * p.h;
        g.offPixels += off;
        if (maxStep > 3 || rel > 2e-3) {
            ++g.differing;
            if (g.differing <= 6) {
                spdlog::info("native: own post: {} differs: sum|d| / sum = {:.3g}, max {} steps, {} of {} pixels more than 1 step off", p.name, rel, maxStep, off,
                             static_cast<std::uint64_t>(p.w) * p.h);
            }
        }
    }
    cmpPending_[slot].clear();
}

// ---- the bloom chain ----

namespace {

struct BloomSet {
    PostBrightParams bright;
    PostGlowParams glow;
    PostBloomParams bloom;
    std::uint32_t w = 0, h = 0;
};

bool bloomParams(const std::array<const fxc::EffectDraw*, kMaxGroupUnits>& rec, bool sceneSubstituted, BloomSet& out, std::string& why) {
    std::memset(&out, 0, sizeof(out));
    d3d9::Texture* bp = targetOf(*rec[0]);
    d3d9::Texture* gh = targetOf(*rec[1]);
    d3d9::Texture* st = targetOf(*rec[2]);
    d3d9::Texture* bl = targetOf(*rec[3]);
    if (bp == nullptr || gh == nullptr || st == nullptr || bl == nullptr) {
        why = "a unit has no target";
        return false;
    }
    const std::uint32_t w = bp->desc().width, h = bp->desc().height;
    for (d3d9::Texture* t : {gh, st, bl}) {
        if (t->desc().width != w || t->desc().height != h) {
            why = "the bloom images differ in size";
            return false;
        }
    }
    out.w = w;
    out.h = h;
    float uv[4][4];
    for (int k = 0; k < 4; ++k) {
        if (!plain(*rec[static_cast<std::size_t>(k)], k == 3, w, h, why) || !OwnPost::quadUv(*rec[static_cast<std::size_t>(k)], w, h, uv[k], why)) {
            why = std::format("unit {}: {}", k, why);
            return false;
        }
    }
    // ---- bright pass ----
    {
        const fxc::EffectDraw& d = *rec[0];
        const d3d9::Texture* scene = texOf(d, 0);
        const d3d9::Texture* avg = texOf(d, 1);
        const d3d9::Texture* depth = texOf(d, 2);
        if (scene == nullptr || avg == nullptr || depth == nullptr || avg->desc().width != 1 || avg->desc().height != 1) {
            why = "bright pass: scene / adapted luminance / depth textures";
            return false;
        }
        if (!sampler(d, 0, d3d9::TEXF_POINT, why) || !sampler(d, 2, d3d9::TEXF_POINT, why)) return false;
        if (!sceneSubstituted && (!nearTo(d.vsF[0][0], 1.0 / scene->desc().width) || !nearTo(d.vsF[1][0], 1.0 / scene->desc().height))) {
            why = std::format("bright pass: tap offsets ({}, {}) are not 1 / {}x{}", d.vsF[0][0], d.vsF[1][0], scene->desc().width, scene->desc().height);
            return false;
        }
        out.bright.uv[0] = uv[0][0];
        out.bright.uv[1] = uv[0][1];
        out.bright.uv[2] = uv[0][2];
        out.bright.uv[3] = uv[0][3];
        out.bright.tap[0] = d.vsF[0][0];
        out.bright.tap[1] = d.vsF[1][0];
        out.bright.tap[2] = d.vsF[2][0];
        out.bright.tap[3] = d.vsF[2][1];
        out.bright.ps[0] = d.psF[0][0];  // ToneMapFactor
        out.bright.ps[1] = d.psF[1][0];  // BrightPassThreshold
        out.bright.ps[2] = d.psF[2][0];  // BrightPassOffset
        out.bright.ps[3] = d.psF[3][0];  // 1 / WhitePoint
        if (!(out.bright.ps[0] > 0.0f) || !(out.bright.ps[3] > 0.0f) || !std::isfinite(out.bright.ps[1]) || !std::isfinite(out.bright.ps[2])) {
            why = "bright pass: constants";
            return false;
        }
        out.bright.size[0] = w;
        out.bright.size[1] = h;
    }
    // ---- glow H + streak (both read the bright pass image through the linear / clamp sampler) ----
    const double cx = 0.5 / w, cy = 0.5 / h;
    {
        const fxc::EffectDraw& d = *rec[1];
        if (texOf(d, 0) != bp || !sampler(d, 0, d3d9::TEXF_LINEAR, why)) {
            why = "glow H: " + (why.empty() ? std::string("source is not the bright pass") : why);
            return false;
        }
        if (!nearTo(d.vsF[0][0], cx) || !nearTo(d.vsF[0][1], cy)) {
            why = "glow H: centre offset";
            return false;
        }
        for (int i = 0; i < 6; ++i) {
            if (!nearTo(d.vsF[1 + i][0], (0.5 + 2.0 * i) / w) || std::abs(d.vsF[1 + i][1]) > 1e-9f) {
                why = std::format("glow H: tap offset {} is ({}, {})", i + 1, d.vsF[1 + i][0], d.vsF[1 + i][1]);
                return false;
            }
        }
        for (int k = 0; k < 4; ++k) {
            out.glow.uvH[k] = uv[1][k];
            out.glow.centerH[k] = d.vsF[0][k];
        }
        for (int i = 0; i < 6; ++i) {
            out.glow.offH[i][0] = d.vsF[1 + i][0];
            out.glow.offH[i][1] = d.vsF[1 + i][1];
        }
    }
    {
        const fxc::EffectDraw& d = *rec[2];
        if (texOf(d, 0) != bp || !sampler(d, 0, d3d9::TEXF_LINEAR, why)) {
            why = "streak: " + (why.empty() ? std::string("source is not the bright pass") : why);
            return false;
        }
        if (!nearTo(d.vsF[0][0], cx) || !nearTo(d.vsF[0][1], cy)) {
            why = "streak: centre offset";
            return false;
        }
        for (int k = 0; k < 4; ++k) {
            out.glow.uvS[k] = uv[2][k];
            out.glow.centerS[k] = d.vsF[0][k];
        }
        out.glow.light[0] = d.psF[0][0];
        out.glow.light[1] = d.psF[0][1];
        out.glow.light[2] = d.psF[1][0];
        out.glow.light[3] = d.psF[2][0];
        out.glow.sw[0] = d.psF[3][0];
        out.glow.sw[1] = d.psF[4][0];
        // a Density / 64 and a plausible weight / decay: anything else is not post_streaks.fx's preshader
        if (!(out.glow.light[2] >= 0.0f) || !std::isfinite(out.glow.light[0]) || !std::isfinite(out.glow.light[1]) || !std::isfinite(out.glow.light[3]) ||
            !std::isfinite(out.glow.sw[0]) || !(out.glow.sw[1] >= 0.0f && out.glow.sw[1] <= 1.0f)) {
            why = "streak: constants";
            return false;
        }
    }
    out.glow.tex[2] = w;
    out.glow.tex[3] = h;
    // ---- GlowV + streak + blend ----
    {
        const fxc::EffectDraw& d = *rec[3];
        if (texOf(d, 0) != gh || texOf(d, 1) != st || !sampler(d, 0, d3d9::TEXF_LINEAR, why) || !sampler(d, 1, d3d9::TEXF_LINEAR, why)) {
            why = "bloom: " + (why.empty() ? std::string("sources are not the glow / streak images") : why);
            return false;
        }
        if (!nearTo(d.vsF[0][0], cx) || !nearTo(d.vsF[0][1], cy)) {
            why = "bloom: centre offset";
            return false;
        }
        for (int i = 0; i < 6; ++i) {
            if (std::abs(d.vsF[1 + i][0]) > 1e-9f || !nearTo(d.vsF[1 + i][1], (0.5 + 2.0 * i) / h)) {
                why = std::format("bloom: tap offset {} is ({}, {})", i + 1, d.vsF[1 + i][0], d.vsF[1 + i][1]);
                return false;
            }
        }
        for (int k = 0; k < 4; ++k) {
            out.bloom.uv[k] = uv[3][k];
            out.bloom.center[k] = d.vsF[0][k];
        }
        for (int i = 0; i < 6; ++i) {
            out.bloom.off[i][0] = d.vsF[1 + i][0];
            out.bloom.off[i][1] = d.vsF[1 + i][1];
        }
        out.bloom.blend[0] = d.psF[0][0];
        if (!(out.bloom.blend[0] >= 0.0f && out.bloom.blend[0] <= 1.0f)) {
            why = std::format("bloom: blend weight {}", out.bloom.blend[0]);
            return false;
        }
        out.bloom.size[0] = w;
        out.bloom.size[1] = h;
    }
    return true;
}

}  // namespace

bool OwnPost::runBloom(const std::array<const fxc::EffectDraw*, kMaxGroupUnits>& rec, bool write) {
    GroupStats& gs = gstats_[static_cast<std::size_t>(OwnGroup::Bloom)];
    const auto refuse = [&](const std::string& why) {
        ++gs.refused;
        std::string& last = groupWhy_[static_cast<int>(OwnGroup::Bloom)];
        std::uint32_t& n = groupWhyLogged_[static_cast<int>(OwnGroup::Bloom)];
        if (why != last || n < 3) {
            last = why;
            ++n;
            spdlog::info("native: own post: the bloom chain stays the engine's: {}", why);
        }
        return false;
    };
    if (!groupReady(static_cast<int>(OwnGroup::Bloom))) {
        ++gs.refused;
        return false;
    }
    d3d9::Texture* scene = texOf(*rec[0], 0);
    const bool substituted = scene == dev_.sceneSubstituteFrom() && dev_.sceneSubstituteSlot() != 0;
    BloomSet set;
    std::string why;
    if (!bloomParams(rec, substituted, set, why)) return refuse(why);
    d3d9::Texture* avg = texOf(*rec[0], 1);
    d3d9::Texture* depth = texOf(*rec[0], 2);
    d3d9::Texture* bpEng = targetOf(*rec[0]);
    d3d9::Texture* ghEng = targetOf(*rec[1]);
    d3d9::Texture* stEng = targetOf(*rec[2]);
    d3d9::Texture* blEng = targetOf(*rec[3]);
    if (write) {
        if (!blEng->storageCapable() || blEng->rhi().desc().format != VK_FORMAT_R8G8B8A8_UNORM) {
            return refuse("the persistent bloom image has no storage usage (created before the setting was on)");
        }
        if (!dev_.game().gameCaps().storageImageExtendedFormats) return refuse("the device cannot load RGBA8 storage images");
    }
    bool ok = false;
    rhi::Texture* oursBloom = nullptr;
    const std::uint32_t pointSampler = 0;
    (void)pointSampler;
    const auto record = [&](d3d9::PassContext& ctx) {
        rhi::CommandList& cmd = ctx.cmd;
        if (!substituted && !dev_.readable(scene, cmd)) return;
        if (!dev_.readable(avg, cmd) || !dev_.readable(depth, cmd)) return;
        if (!write && !dev_.readable(blEng, cmd)) return;
        const VkExtent2D ext{set.w, set.h};
        rhi::Texture* bp = dev_.transient("post.bright", {VK_FORMAT_R8G8B8A8_UNORM, ext});
        rhi::Texture* gh = dev_.transient("post.glowH", {VK_FORMAT_R8G8B8A8_UNORM, ext});
        rhi::Texture* st = dev_.transient("post.streak", {VK_FORMAT_R8G8B8A8_UNORM, ext});
        rhi::Texture* out = write ? &blEng->rhi() : dev_.transient("post.bloom.ours", {VK_FORMAT_R8G8B8A8_UNORM, ext});
        if (bp == nullptr || gh == nullptr || st == nullptr || out == nullptr) return;
        const rhi::RingAlloc pa = dev_.ring(sizeof(PostBrightParams), 16);
        const rhi::RingAlloc pb = dev_.ring(sizeof(PostGlowParams), 16);
        const rhi::RingAlloc pc = dev_.ring(sizeof(PostBloomParams), 16);
        if (pa.ptr == nullptr || pb.ptr == nullptr || pc.ptr == nullptr) return;
        const std::uint32_t linear = dev_.samplerFor(true, true);
        set.bright.tex[0] = substituted ? dev_.sceneSubstituteSlot() : scene->bindlessSlot();
        set.bright.tex[1] = avg->bindlessSlot();
        set.bright.tex[2] = depth->bindlessSlot();
        set.glow.tex[0] = bp->heapSampled();
        set.glow.tex[1] = linear;
        set.bloom.tex[0] = gh->heapSampled();
        set.bloom.tex[1] = st->heapSampled();
        set.bloom.tex[2] = linear;
        set.bloom.tex[3] = write ? 0u : blEng->bindlessSlot();  // Compare: the previous bloom is read from the engine's image, the result goes to the scratch
        std::memcpy(pa.ptr, &set.bright, sizeof(set.bright));
        std::memcpy(pb.ptr, &set.glow, sizeof(set.glow));
        std::memcpy(pc.ptr, &set.bloom, sizeof(set.bloom));
        const auto push = [&](const rhi::RingAlloc& a) {
            PostGroupPush p{};
            p.h.frame = ctx.frameBlock.bits;
            p.params[0] = a.ref.low();
            p.params[1] = a.ref.high();
            cmd.setPushConstants(&p, sizeof(p));
        };
        const std::uint32_t gx = groups8(set.w), gy = groups8(set.h);
        // 1: the bright pass
        cmd.transition(*bp, VK_IMAGE_LAYOUT_GENERAL);
        cmd.bindPipeline(*kern_[0]);
        rhi::Texture* const s1[] = {bp};
        cmd.setStorageTextures(s1);
        push(pa);
        cmd.dispatch(gx, gy, 1);
        cmd.transition(*bp, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        // 2: glow H + streak
        cmd.transition(*gh, VK_IMAGE_LAYOUT_GENERAL);
        cmd.transition(*st, VK_IMAGE_LAYOUT_GENERAL);
        cmd.bindPipeline(*kern_[1]);
        rhi::Texture* const s2[] = {gh, st};
        cmd.setStorageTextures(s2);
        push(pb);
        cmd.dispatch(gx, gy, 1);
        cmd.transition(*gh, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        cmd.transition(*st, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        // 3: glow V + streak, blended onto the persistent bloom image
        cmd.transition(*out, VK_IMAGE_LAYOUT_GENERAL);
        cmd.bindPipeline(*kern_[2]);
        rhi::Texture* const s3[] = {out};
        cmd.setStorageTextures(s3);
        push(pc);
        cmd.dispatch(gx, gy, 1);
        if (write) {
            cmd.transition(*out, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        } else {
            // Compare: the engine's bright / glow / streak units have been drawn: compare them now; the blended result after the engine's last unit
            recordCompare(cmd, "bright", *bp, bpEng->rhi());
            recordCompare(cmd, "glow", *gh, ghEng->rhi());
            recordCompare(cmd, "streak", *st, stEng->rhi());
            oursBloom = out;
        }
        ok = true;
    };
    if (!dev_.runPassNow(write ? "post.bloom" : "post.bloom (compare)", record) || !ok) {
        ++gs.refused;
        return false;
    }
    ++gs.runs;
    if (!write) {
        bloomOurs_ = oursBloom;
        bloomEngine_ = blEng;
    }
    return true;
}

// ---- the DoF middle ----

namespace {

struct DofSet {
    PostDofMidParams p;
    std::uint32_t w = 0, h = 0;
};

bool dofParams(const std::array<const fxc::EffectDraw*, kMaxGroupUnits>& rec, DofSet& out, std::string& why) {
    std::memset(&out, 0, sizeof(out));
    d3d9::Texture* t[4];
    for (int k = 0; k < 4; ++k) {
        t[k] = targetOf(*rec[static_cast<std::size_t>(k)]);
        if (t[k] == nullptr) {
            why = "a unit has no target";
            return false;
        }
    }
    d3d9::Texture* dsc = texOf(*rec[0], 0);
    if (dsc == nullptr) {
        why = "no DoFDown image";
        return false;
    }
    const std::uint32_t w = dsc->desc().width, h = dsc->desc().height;
    // the chain: DoFDown -> BlurH (t0) -> BlurV (t1) -> NearCoc (t2, with DoFDown as its second input) -> SmallBlur (t3)
    if (texOf(*rec[1], 0) != t[0] || texOf(*rec[2], 0) != t[1] || texOf(*rec[2], 1) != dsc || texOf(*rec[3], 0) != t[2]) {
        why = "the units are not the chain BlurH -> BlurV -> NearCoc -> SmallBlur";
        return false;
    }
    for (int k = 0; k < 4; ++k) {
        if (t[k]->desc().width != w || t[k]->desc().height != h) {
            why = "the DoF images differ in size";
            return false;
        }
        if (!plain(*rec[static_cast<std::size_t>(k)], false, w, h, why)) {
            why = std::format("unit {}: {}", k, why);
            return false;
        }
        if (!sampler(*rec[static_cast<std::size_t>(k)], 0, d3d9::TEXF_LINEAR, why)) return false;
        float uv[4];
        if (!OwnPost::quadUv(*rec[static_cast<std::size_t>(k)], w, h, uv, why)) return false;
        // texel corners: the quad's uv at pixel p is p / W (these units add no half texel)
        if (std::abs(uv[0]) > 1e-4f || std::abs(uv[1]) > 1e-4f || !nearTo(uv[2], 1.0 / w) || !nearTo(uv[3], 1.0 / h)) {
            why = std::format("unit {}: the quad's uv is ({}, {}) + p x ({}, {}), expected p / size", k, uv[0], uv[1], uv[2], uv[3]);
            return false;
        }
    }
    const double dx = 1.0 / w, dy = 1.0 / h;
    const auto& v0 = *rec[0];
    const auto& v1 = *rec[1];
    const auto& v2 = *rec[2];
    const auto& v3 = *rec[3];
    const bool h0 = nearTo(v0.vsF[0][0], dx) && std::abs(v0.vsF[0][1]) < 1e-9f && nearTo(v0.vsF[1][0], 3.0 * dx) && std::abs(v0.vsF[1][1]) < 1e-9f;
    const bool h1 = std::abs(v1.vsF[0][0]) < 1e-9f && nearTo(v1.vsF[0][1], dy) && std::abs(v1.vsF[1][0]) < 1e-9f && nearTo(v1.vsF[1][1], 3.0 * dy);
    const bool h2 = nearTo(v2.vsF[0][0], 0.5 * dx) && nearTo(v2.vsF[0][1], 0.5 * dy);
    const bool h3 = std::abs(v3.vsF[0][0]) < 1e-9f && nearTo(v3.vsF[0][1], dx) && std::abs(v3.vsF[0][2]) < 1e-9f && nearTo(v3.vsF[0][3], dy);
    if (!h0 || !h1 || !h2 || !h3) {
        why = std::format("tap offsets not as expected (H {} V {} near {} small {})", h0, h1, h2, h3);
        return false;
    }
    if (!sampler(v2, 1, d3d9::TEXF_LINEAR, why)) return false;
    out.w = w;
    out.h = h;
    out.p.size[0] = w;
    out.p.size[1] = h;
    return true;
}

}  // namespace

bool OwnPost::runDofMid(const std::array<const fxc::EffectDraw*, kMaxGroupUnits>& rec, bool write) {
    GroupStats& gs = gstats_[static_cast<std::size_t>(OwnGroup::DofMid)];
    const auto refuse = [&](const std::string& why) {
        ++gs.refused;
        std::string& last = groupWhy_[static_cast<int>(OwnGroup::DofMid)];
        std::uint32_t& n = groupWhyLogged_[static_cast<int>(OwnGroup::DofMid)];
        if (why != last || n < 3) {
            last = why;
            ++n;
            spdlog::info("native: own post: the DoF middle stays the engine's: {}", why);
        }
        return false;
    };
    if (!groupReady(static_cast<int>(OwnGroup::DofMid))) {
        ++gs.refused;
        return false;
    }
    DofSet set;
    std::string why;
    if (!dofParams(rec, set, why)) return refuse(why);
    d3d9::Texture* dsc = texOf(*rec[0], 0);
    d3d9::Texture* engV = targetOf(*rec[1]);
    d3d9::Texture* engS = targetOf(*rec[3]);
    if (write) {
        for (d3d9::Texture* t : {engV, engS}) {
            if (!t->storageCapable() || t->rhi().desc().format != VK_FORMAT_R8G8B8A8_UNORM) return refuse("a DoF image has no storage usage (created before the setting was on)");
        }
    }
    bool ok = false;
    rhi::Texture* oursS = nullptr;
    const auto record = [&](d3d9::PassContext& ctx) {
        rhi::CommandList& cmd = ctx.cmd;
        const VkExtent2D ext{set.w, set.h};
        // the engine's image pool shares textures between filters whose lifetimes do not overlap: the SmallBlur (or BlurV) target can be the DoFDown image itself, which
        // this kernel reads while it writes (a SRV / UAV alias: found by D3D12 GPU-based validation): the input is copied aside then
        rhi::Texture* in = &dsc->rhi();
        if (write && (dsc == engV || dsc == engS)) {
            rhi::Texture* copy = dev_.transient("post.dof.in", {VK_FORMAT_R8G8B8A8_UNORM, ext});
            if (copy == nullptr) return;
            cmd.transition(dsc->rhi(), VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
            cmd.transition(*copy, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
            rhi::TextureCopy region{};
            region.extent = {set.w, set.h, 1};
            cmd.copyTexture(*copy, dsc->rhi(), std::span<const rhi::TextureCopy>(&region, 1));
            cmd.transition(*copy, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
            in = copy;
            ++aliasedInputs_;
        } else if (!dev_.readable(dsc, cmd)) {
            return;
        }
        rhi::Texture* outV = write ? &engV->rhi() : dev_.transient("post.dof.v", {VK_FORMAT_R8G8B8A8_UNORM, ext});
        rhi::Texture* outS = write ? &engS->rhi() : dev_.transient("post.dof.s", {VK_FORMAT_R8G8B8A8_UNORM, ext});
        if (outV == nullptr || outS == nullptr) return;
        const rhi::RingAlloc pa = dev_.ring(sizeof(PostDofMidParams), 16);
        if (pa.ptr == nullptr) return;
        set.p.tex[0] = in->heapSampled();
        std::memcpy(pa.ptr, &set.p, sizeof(set.p));
        PostGroupPush push{};
        push.h.frame = ctx.frameBlock.bits;
        push.params[0] = pa.ref.low();
        push.params[1] = pa.ref.high();
        cmd.transition(*outV, VK_IMAGE_LAYOUT_GENERAL);
        cmd.transition(*outS, VK_IMAGE_LAYOUT_GENERAL);
        cmd.bindPipeline(*kern_[3]);
        rhi::Texture* const s[] = {outV, outS};
        cmd.setStorageTextures(s);
        cmd.setPushConstants(&push, sizeof(push));
        cmd.dispatch(groups8(set.w), groups8(set.h), 1);
        if (write) {
            cmd.transition(*outV, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
            cmd.transition(*outS, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        } else {
            recordCompare(cmd, "large blur", *outV, engV->rhi());
            oursS = outS;
        }
        ok = true;
    };
    if (!dev_.runPassNow(write ? "post.dofmid" : "post.dofmid (compare)", record) || !ok) {
        ++gs.refused;
        return false;
    }
    ++gs.runs;
    if (!write) {
        dofOursS_ = oursS;
        dofEngineS_ = engS;
    }
    return true;
}

}  // namespace rvk::native
