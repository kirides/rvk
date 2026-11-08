#include "native/own_post.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <format>

#include <spdlog/spdlog.h>

#include "core/settings.h"
#include "shader/embedded.h"

namespace rvk::native {

namespace {

// the luminance units' names as EffectOps::effectBegin composes them ("<filter name> <technique>"), in the engine's order
constexpr std::string_view kLumNames[kLumUnits] = {"Down64x64 LogLuminance", "Down32x32 DownSample", "Down16x16 DownSample", "Down8x8 DownSample",
                                                   "Down4x4 DownSample",     "Down2x2 DownSample",   "AdaptiveLiminance AdaptiveLiminance",
                                                   "AvgLuminance Copy"};

// D3DDECLUSAGE_POSITION / TEXCOORD, D3DDECLTYPE_FLOAT2 .. FLOAT4
constexpr std::uint8_t kUsagePosition = 0, kUsageTexcoord = 5, kTypeFloat2 = 1, kTypeFloat4 = 3;

}  // namespace

// the full-screen quad of a unit: position (x, y) and uv of its vertices -> the uv at the sample point of pixel (0, 0) and its step per pixel of a W x H target
bool OwnPost::quadUv(const fxc::EffectDraw& d, std::uint32_t w, std::uint32_t h, float out[4], std::string& why) {
    const auto* decl = static_cast<const d3d9::VertexDecl*>(d.decl);
    const auto* buf = static_cast<const d3d9::Buffer*>(d.streams[0].buffer);
    if (decl == nullptr || buf == nullptr || d.streams[0].stride == 0 || d.numVertices < 3 || d.numVertices > 16) {
        why = "no quad vertices";
        return false;
    }
    const d3d9::VertexElement* pos = nullptr;
    const d3d9::VertexElement* uv = nullptr;
    for (const d3d9::VertexElement& e : decl->elements()) {
        if (e.stream != 0) continue;
        if (e.usage == kUsagePosition && e.usageIndex == 0 && e.type >= kTypeFloat2 && e.type <= kTypeFloat4) pos = &e;
        if (e.usage == kUsageTexcoord && e.usageIndex == 0 && e.type == kTypeFloat2) uv = &e;
    }
    if (pos == nullptr || uv == nullptr) {
        why = "the quad's declaration has no float position / texcoord0";
        return false;
    }
    struct V {
        float x, y, u, v;
    } vert[16];
    for (std::uint32_t i = 0; i < d.numVertices; ++i) {
        const std::uint32_t base = d.streams[0].offset + static_cast<std::uint32_t>(d.baseVertex + static_cast<std::int32_t>(d.minIndex + i)) * d.streams[0].stride;
        float p[2], t[2];
        if (!const_cast<d3d9::Buffer*>(buf)->readCpu(base + pos->offset, sizeof(p), p) || !const_cast<d3d9::Buffer*>(buf)->readCpu(base + uv->offset, sizeof(t), t)) {
            why = "the quad's vertices are not readable";
            return false;
        }
        vert[i] = {p[0], p[1], t[0], t[1]};
    }
    const std::uint32_t n = d.numVertices;
    std::uint32_t xmin = 0, xmax = 0, ymin = 0, ymax = 0;
    for (std::uint32_t i = 1; i < n; ++i) {
        if (vert[i].x < vert[xmin].x) xmin = i;
        if (vert[i].x > vert[xmax].x) xmax = i;
        if (vert[i].y < vert[ymin].y) ymin = i;
        if (vert[i].y > vert[ymax].y) ymax = i;
    }
    const double dx = static_cast<double>(vert[xmax].x) - vert[xmin].x, dy = static_cast<double>(vert[ymax].y) - vert[ymin].y;
    if (!(dx > 0.0) || !(dy > 0.0)) {
        why = "the quad is degenerate";
        return false;
    }
    // (an axis-aligned quad: u follows x, v follows y; the corners with the same x have the same u)
    const double ux = (static_cast<double>(vert[xmax].u) - vert[xmin].u) / dx;
    const double vy = (static_cast<double>(vert[ymax].v) - vert[ymin].v) / dy;
    // (the renderer shifts every D3D9 draw by the half pixel, posFix = (+1 / W, -1 / H) in clip space: the D3D9 pixel p samples the quad where the viewport
    // transform puts screen x = p, not p + 0.5: NDC x = 2 p / W - 1, y = 1 - 2 p / H)
    const double ndcX0 = -1.0, ndcY0 = 1.0;
    out[0] = static_cast<float>(vert[xmin].u + (ndcX0 - vert[xmin].x) * ux);
    out[1] = static_cast<float>(vert[ymin].v + (ndcY0 - vert[ymin].y) * vy);
    out[2] = static_cast<float>(ux * (2.0 / w));
    out[3] = static_cast<float>(vy * (-2.0 / h));
    return true;
}

OwnPost::OwnPost(d3d9::Device& dev) : dev_(dev) {}

OwnPost::~OwnPost() {
    if (building_.valid()) building_.wait();
    if (buildingTile_.valid()) buildingTile_.wait();
    for (auto& f : kernBuilding_) {
        if (f.valid()) f.wait();
    }
    readback_.reset();
}

void OwnPost::setMode(Mode m) {
    if (m == mode_) return;
    mode_ = m;
    d3d9::Texture::setSmallStorage(m == Mode::On);
    if (m != Mode::Off && !lum_ && !building_.valid() && !failed_) {
        rhi::Device* rdev = dev_.rhi();
        if (rdev != nullptr) {
            building_ = std::async(std::launch::async, [rdev]() {
                rhi::ComputePipelineDesc d;
                d.cs.spirv = shader::embedded::spirv("post/post_lum.comp");
                d.cs.dxil = shader::embedded::dxilOnDemand("post/post_lum.comp");
                d.name = "post.lum";
                return rdev->createComputePipeline(d);
            });
            buildingTile_ = std::async(std::launch::async, [rdev]() {
                rhi::ComputePipelineDesc d;
                d.cs.spirv = shader::embedded::spirv("post/post_lum_tile.comp");
                d.cs.dxil = shader::embedded::dxilOnDemand("post/post_lum_tile.comp");
                d.name = "post.lum.tile";
                return rdev->createComputePipeline(d);
            });
        }
    }
    if (m != Mode::Off && !kernFailed_ && !kernBuilding_[0].valid() && !kern_[0]) {
        rhi::Device* rdev = dev_.rhi();
        static constexpr const char* kNames[4] = {"post/post_bright.comp", "post/post_glow.comp", "post/post_bloom.comp", "post/post_dofmid.comp"};
        if (rdev != nullptr) {
            for (int i = 0; i < 4; ++i) {
                const char* name = kNames[i];
                kernBuilding_[static_cast<std::size_t>(i)] = std::async(std::launch::async, [rdev, name]() {
                    rhi::ComputePipelineDesc d;
                    d.cs.spirv = shader::embedded::spirv(name);
                    d.cs.dxil = shader::embedded::dxilOnDemand(name);
                    d.name = name;
                    return rdev->createComputePipeline(d);
                });
            }
        }
    }
    spdlog::info("native: own post: {}", m == Mode::Off ? "off" : m == Mode::On ? "on" : "compare");
}

int OwnPost::lumUnit(std::string_view name) {
    for (int i = 0; i < kLumUnits; ++i) {
        if (name == kLumNames[i]) return i;
    }
    return -1;
}

bool OwnPost::lumReady() {
    if (lum_ && tile_) return true;
    if (failed_ || !building_.valid() || !buildingTile_.valid()) return false;
    if (building_.wait_for(std::chrono::seconds(0)) != std::future_status::ready || buildingTile_.wait_for(std::chrono::seconds(0)) != std::future_status::ready) return false;
    lum_ = building_.get();
    tile_ = buildingTile_.get();
    if (!lum_ || !tile_) {
        spdlog::error("native: own post: the luminance compute pipeline failed: the luminance units stay the engine's");
        failed_ = true;
        return false;
    }
    spdlog::info("native: own post: luminance pipeline ready");
    return true;
}

bool OwnPost::lumParams(const std::array<const fxc::EffectDraw*, kLumUnits>& rec, bool sceneSubstituted, PostLumParams& out, std::string& why) {
    std::memset(&out, 0, sizeof(out));
    // sizes: the targets are 64 .. 2 and 1 (viewport), the sources the scene (a texture, any size) and the previous level
    static constexpr std::uint32_t kDim[7] = {64, 32, 16, 8, 4, 2, 1};
    for (int s = 0; s < 7; ++s) {
        const fxc::EffectDraw& d = *rec[s];
        if (d.viewport.width != kDim[s] || d.viewport.height != kDim[s] || d.viewport.x != 0 || d.viewport.y != 0) {
            why = std::format("unit {}: viewport {}x{} at ({}, {}), expected {}x{}", s, d.viewport.width, d.viewport.height, d.viewport.x, d.viewport.y, kDim[s], kDim[s]);
            return false;
        }
        if (!quadUv(d, kDim[s], kDim[s], out.stage[s].uv, why)) {
            why = std::format("unit {}: {}", s, why);
            return false;
        }
        // the VS' offsets: c0.x = 1 / source width, c1.x = 1 / source height (post_hdr.fx's preshaders); checked against the source's size
        out.stage[s].tap[0] = d.vsF[0][0];
        out.stage[s].tap[1] = d.vsF[1][0];
        const auto* src = static_cast<const d3d9::Texture*>(d.textures[0]);
        if (src == nullptr) {
            why = std::format("unit {}: no source texture", s);
            return false;
        }
        const double w = s == 0 ? src->desc().width : 2.0 * kDim[s], h = s == 0 ? src->desc().height : 2.0 * kDim[s];
        if (!(s == 0 && sceneSubstituted) && (std::abs(out.stage[s].tap[0] * w - 1.0) > 2e-3 || std::abs(out.stage[s].tap[1] * h - 1.0) > 2e-3)) {
            why = std::format("unit {}: tap offsets ({}, {}) are not 1 / {}x{}", s, out.stage[s].tap[0], out.stage[s].tap[1], w, h);
            return false;
        }
        if (s >= 1 && src->desc().width != 2 * kDim[s] && s <= 5) {
            // (the sources' own sizes: the previous level's target)
            why = std::format("unit {}: source {}x{}", s, src->desc().width, src->desc().height);
            return false;
        }
    }
    // levels 1..3 are reduced as aligned 2x2 boxes inside the tiles: the engine's taps of those stages must be the blocks (2x, 2x + 1) of both axes
    for (int s = 1; s <= 3; ++s) {
        const int d = static_cast<int>(kDim[s]);
        for (int x = 0; x < d; ++x) {
            const float u = out.stage[s].uv[0] + out.stage[s].uv[2] * static_cast<float>(x), v = out.stage[s].uv[1] + out.stage[s].uv[3] * static_cast<float>(x);
            const int tu0 = static_cast<int>(std::floor(u * static_cast<float>(2 * d))), tu1 = static_cast<int>(std::floor((u + out.stage[s].tap[0]) * static_cast<float>(2 * d)));
            const int tv0 = static_cast<int>(std::floor(v * static_cast<float>(2 * d))), tv1 = static_cast<int>(std::floor((v + out.stage[s].tap[1]) * static_cast<float>(2 * d)));
            if (tu0 != 2 * x || tu1 != 2 * x + 1 || tv0 != 2 * x || tv1 != 2 * x + 1) {
                why = std::format("stage {}: the taps are not aligned 2x2 blocks (x {}: {} {} / {} {})", s, x, tu0, tu1, tv0, tv1);
                return false;
            }
        }
    }
    const fxc::EffectDraw& ad = *rec[6];
    out.adapt[0] = ad.psF[1][0];
    out.adapt[1] = ad.psF[0][0];
    out.adapt[2] = ad.psF[0][1];
    if (!(out.adapt[0] >= 0.0f && out.adapt[0] <= 1.0f) || !(out.adapt[1] > 0.0f) || !(out.adapt[2] >= out.adapt[1])) {
        why = std::format("adaptation constants {} / {} / {}", out.adapt[0], out.adapt[1], out.adapt[2]);
        return false;
    }
    // the targets: unit 6 (Adaptive) and unit 7 (the Copy), 1x1 R32F; unit 6 reads the previous value from the unit 7's target
    const auto* prev = static_cast<const d3d9::Texture*>(ad.textures[1]);
    const auto* adaptSurf = static_cast<const d3d9::Surface*>(rec[6]->rt0);
    const auto* avgSurf = static_cast<const d3d9::Surface*>(rec[7]->rt0);
    if (prev == nullptr || adaptSurf == nullptr || avgSurf == nullptr || prev != avgSurf->texture()) {
        why = "the adaptive unit's previous-luminance texture is not the copy unit's target";
        return false;
    }
    return true;
}

bool OwnPost::runLum(const std::array<const fxc::EffectDraw*, kLumUnits>& rec, bool write) {
    if (!lumReady()) {
        ++stats_.refused;
        return false;
    }
    PostLumParams p;
    std::string why;
    const auto* sceneTex = static_cast<const d3d9::Texture*>(rec[0]->textures[0]);
    const bool substituted = sceneTex == dev_.sceneSubstituteFrom() && dev_.sceneSubstituteSlot() != 0;
    if (!lumParams(rec, substituted, p, why)) {
        ++stats_.refused;
        if (why != lastWhy_ || whyLogged_ < 3) {
            lastWhy_ = why;
            ++whyLogged_;
            spdlog::info("native: own post: the luminance chain stays the engine's: {}", why);
        }
        return false;
    }
    d3d9::Texture* scene = const_cast<d3d9::Texture*>(static_cast<const d3d9::Texture*>(rec[0]->textures[0]));
    d3d9::Texture* prev = const_cast<d3d9::Texture*>(static_cast<const d3d9::Texture*>(rec[6]->textures[1]));
    d3d9::Texture* adaptOut = static_cast<d3d9::Surface*>(const_cast<void*>(rec[6]->rt0))->texture();
    d3d9::Texture* avgOut = static_cast<d3d9::Surface*>(const_cast<void*>(rec[7]->rt0))->texture();
    if (write && (!adaptOut->storageCapable() || !avgOut->storageCapable())) {
        ++stats_.refused;
        if (whyLogged_++ < 3) spdlog::info("native: own post: the 1x1 luminance images have no storage usage (created before the setting was on): the units stay the engine's");
        return false;
    }
    bool ok = false;
    const auto record = [&](d3d9::PassContext& ctx) {
        rhi::CommandList& cmd = ctx.cmd;
        rhi::Device* rdev = dev_.rhi();
        if (rdev == nullptr) return;
        if (!substituted && !dev_.readable(scene, cmd)) return;
        if (!dev_.readable(prev, cmd)) return;
        const rhi::RingAlloc pa = dev_.ring(sizeof(PostLumParams), 16);
        if (pa.ptr == nullptr) return;
        p.tex[0] = substituted ? dev_.sceneSubstituteSlot() : scene->bindlessSlot();
        p.tex[1] = prev->bindlessSlot();
        std::memcpy(pa.ptr, &p, sizeof(p));
        PostLumPush push{};
        push.h.frame = ctx.frameBlock.bits;
        push.params[0] = pa.ref.low();
        push.params[1] = pa.ref.high();
        rhi::Texture* outA = &adaptOut->rhi();
        rhi::Texture* outB = &avgOut->rhi();
        rhi::Texture* scratchA = nullptr;
        rhi::Texture* scratchB = nullptr;
        if (!write) {
            scratchA = dev_.transient("post.lum.ours", {VK_FORMAT_R32_SFLOAT, {1, 1}});
            scratchB = dev_.transient("post.lum.ours2", {VK_FORMAT_R32_SFLOAT, {1, 1}});
            if (scratchA == nullptr || scratchB == nullptr) return;
            outA = scratchA;
            outB = scratchB;
        }
        rhi::Texture* tileImg = dev_.transient("post.lum.tile", {VK_FORMAT_R32_SFLOAT, {8, 9}});
        if (tileImg == nullptr) return;
        cmd.transition(*tileImg, VK_IMAGE_LAYOUT_GENERAL);
        // A: 64 tiles (levels 0..3) from the scene
        cmd.bindPipeline(*tile_);
        rhi::Texture* const tileStorage[] = {tileImg};
        cmd.setStorageTextures(tileStorage);
        cmd.setPushConstants(&push, sizeof(push));
        cmd.dispatch(8, 8, 1);
        cmd.computeBarrier();
        // B: levels 4, 5 and the adaptation (u0 / u1 the two outputs, u2 the tile image); the outputs only now leave the layout they were read in
        cmd.transition(*outA, VK_IMAGE_LAYOUT_GENERAL);
        cmd.transition(*outB, VK_IMAGE_LAYOUT_GENERAL);
        cmd.bindPipeline(*lum_);
        rhi::Texture* const storage[] = {outA, outB, tileImg};
        cmd.setStorageTextures(storage);
        cmd.setPushConstants(&push, sizeof(push));
        cmd.dispatch(1, 1, 1);
        if (write) {
            cmd.transition(*outA, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
            cmd.transition(*outB, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        } else {
            // compare: ours and the engine's Adaptive unit's target (already drawn) -> the host-visible buffer of this frame slot
            if (!readback_) {
                readback_ = rdev->createBuffer({1024ull * rhi::kFramesInFlight, VK_BUFFER_USAGE_TRANSFER_DST_BIT, rhi::Memory::Readback, "own post compare"});
                if (readback_) std::memset(readback_->map(), 0, 1024ull * rhi::kFramesInFlight);
            }
            if (readback_) {
                const std::uint32_t slot = dev_.frameSlot();
                readCompare(slot);
                cmd.transition(*outA, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
                cmd.copyTextureToBuffer(*readback_, 1024ull * slot, *outA);
                rhi::Texture* eng = &adaptOut->rhi();
                cmd.transition(*eng, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
                cmd.copyTextureToBuffer(*readback_, 1024ull * slot + 512, *eng);
                pendingFrame_[slot] = dev_.frameValue();
            }
        }
        ok = true;
    };
    if (!dev_.runPassNow(write ? "post.lum" : "post.lum (compare)", record) || !ok) {
        ++stats_.refused;
        return false;
    }
    ++stats_.runs;
    return true;
}

// the compare result of the frame this slot last recorded (the GPU has finished it: the slot's fence was waited for at the frame's start)
void OwnPost::readCompare(std::uint32_t slot) {
    if (!readback_ || pendingFrame_[slot] == 0) return;
    readback_->invalidate();
    float v[2];
    std::memcpy(&v[0], static_cast<std::uint8_t*>(readback_->map()) + 1024ull * slot, 4);
    std::memcpy(&v[1], static_cast<std::uint8_t*>(readback_->map()) + 1024ull * slot + 512, 4);
    pendingFrame_[slot] = 0;
    const double ours = v[0], engine = v[1];
    const double rel = std::abs(ours - engine) / std::max(std::abs(engine), 1e-12);
    ++stats_.compared;
    stats_.sumRel += rel;
    stats_.maxRel = std::max(stats_.maxRel, rel);
    if (!(rel <= 1e-5)) {
        ++stats_.differing;
        if (stats_.differing <= 8) spdlog::info("native: own post: luminance differs: ours {:.9g} engine {:.9g} (rel {:.3g})", ours, engine, rel);
    }
}

void OwnPost::report() {
    const double now = dev_.secondsSinceCreate();
    if (now - lastLog_ < 5.0) return;
    lastLog_ = now;
    {
        // frame-ownership.md 7.47: the state line the A/B scripts assert (setting vs what the code runs)
        const settings::Setting* st = settings::renderer().nativeOwnPost;
        const int want = st != nullptr ? static_cast<int>(st->asInt()) : 0;
        const int have = static_cast<int>(mode_);
        spdlog::info("native: own post state (5 s): {} (setting {}, code {}){}", mode_ == Mode::Off ? "inactive" : "active", want, have, want != have ? " INCONSISTENT" : "");
    }
    if (mode_ == Mode::Off) return;
    spdlog::info("native: own post (5 s): luminance kernel runs {}, refused {}; compare {} frames, differing {} (rel > 1e-5), max rel {:.3g}, mean rel {:.3g}", stats_.runs,
                 stats_.refused, stats_.compared, stats_.differing, stats_.maxRel, stats_.compared != 0 ? stats_.sumRel / static_cast<double>(stats_.compared) : 0.0);
    static constexpr const char* kNames[kGroupCount] = {"luminance", "bloom chain", "DoF middle"};
    for (int g = 1; g < kGroupCount; ++g) {
        const GroupStats& gs = gstats_[static_cast<std::size_t>(g)];
        spdlog::info("native: own post (5 s): {} runs {}, refused {}; compare {} images, differing {} (max step > 3 or sum|d| / sum > 2e-3), max {} steps, max rel {:.3g}, mean rel {:.3g}, "
                     "{} of {} pixels more than 1 step off",
                     kNames[g], gs.runs, gs.refused, gs.compared, gs.differing, gs.maxStep, gs.maxRel, gs.compared != 0 ? gs.sumRel / static_cast<double>(gs.compared) : 0.0,
                     gs.offPixels, gs.pixels);
    }
    if (aliasedInputs_ != 0) spdlog::info("native: own post (5 s): DoF middle: {} runs read a copy of the DoFDown image (the pool shares it with a target)", aliasedInputs_);
    for (const auto& [name, n] : named_) {
        spdlog::info("native: own post (5 s):   image '{}': {} compared, max {} steps, max rel {:.3g}, mean rel {:.3g}, {:.4f} % of the pixels more than 1 step off", name, n.compared, n.maxStep,
                     n.maxRel, n.sumRel / static_cast<double>(std::max<std::uint64_t>(n.compared, 1)), 100.0 * static_cast<double>(n.offPixels) / static_cast<double>(std::max<std::uint64_t>(n.pixels, 1)));
    }
}

}  // namespace rvk::native
