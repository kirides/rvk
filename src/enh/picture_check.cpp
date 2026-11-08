#include "enh/picture_check.h"

#include <algorithm>
#include <cstring>
#include <format>
#include <string>

#include <spdlog/spdlog.h>

#include "shader/embedded.h"

namespace rvk::enh {

namespace {

constexpr float kColorTol = 0.04f;   // a pixel differs in colour at 4 % of max(a, b) + 0.05 (HDR values: the sun / sky are bright)
constexpr float kDepthTol = 0.004f;  // ... in depth at 0.4 % of the nearer surface
constexpr std::uint32_t kStatsBytes = 64;
constexpr std::uint64_t kMaxAge = 16;  // an owned frame is compared with a reference at most this many frames old (native.owned_check = 10 renews it)
// the tolerance of the verdict: an owned view fails when its differing pixels exceed this share of the picture AND 3 x the worst control view
constexpr double kShareTol = 2.0e-4;
constexpr double kMovingShare = 0.01;  // control views differing by more than this share of the picture: the scene is not still

float bitsToFloat(std::uint32_t v) {
    float f;
    std::memcpy(&f, &v, 4);
    return f;
}

}  // namespace

PictureCheck::PictureCheck(d3d9::Device& device) : dev_(device) {}

PictureCheck::~PictureCheck() { setEnabled(false); }

void PictureCheck::setEnabled(bool on) {
    if (on == enabled()) return;
    if (!on) {
        if (passId_ != 0) dev_.removePass(passId_);
        passId_ = 0;
        dev_.releaseTransient("picture_check.color");
        dev_.releaseTransient("picture_check.depth");
        for (Slot& s : slots_) s = {};
        refValid_ = false;
        spdlog::info("native: owned picture check off");
        return;
    }
    failed_ = false;
    passId_ = dev_.addPass(d3d9::inject::Point::AfterOpaque, "native.picture_check", [this](d3d9::PassContext& ctx) { record(ctx); }, this, 90);
    spdlog::info("native: owned picture check on: the opaque scene (colour + linear depth) of an owned main view is compared on the GPU with the last composed frame's "
                 "(same camera; colour tolerance {}, depth tolerance {}); control = two composed frames; the scene must stand still", kColorTol, kDepthTol);
}

bool PictureCheck::ensure(rhi::Device& rdev) {
    if (failed_) return false;
    if (!pipe_) {
        rhi::ComputePipelineDesc cd;
        cd.cs.spirv = shader::embedded::spirv("debug/picture_check.comp");
        cd.cs.dxil = shader::embedded::dxilOnDemand("debug/picture_check.comp");
        cd.name = "native.picture_check";
        pipe_ = rdev.createComputePipeline(cd);
    }
    if (!pipe_) {
        spdlog::error("native: owned picture check: the pipeline failed, the check stays off this session");
        failed_ = true;
        return false;
    }
    for (Slot& s : slots_) {
        if (!s.gpu) {
            s.gpu = rdev.createBuffer({kStatsBytes, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT, rhi::Memory::Device,
                                       "owned picture check stats"});
            s.readback = rdev.createBuffer({kStatsBytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT, rhi::Memory::Readback, "owned picture check readback"});
            if (!s.gpu || !s.readback) {
                spdlog::error("native: owned picture check: the buffers could not be created, the check stays off this session");
                failed_ = true;
                return false;
            }
        }
    }
    return true;
}

void PictureCheck::record(d3d9::PassContext& ctx) {
    collect();
    report();
    d3d9::Texture* scene = ctx.sceneHdr;
    d3d9::Texture* depth = ctx.depthMap;
    if (ctx.mainView != 1 || scene == nullptr || depth == nullptr || ctx.target != scene) return;
    rhi::Device* rdev = dev_.rhi();
    if (rdev == nullptr || !ensure(*rdev)) return;
    const std::uint32_t w = scene->desc().width, h = scene->desc().height;
    if (w == 0 || h == 0 || depth->desc().width != w || depth->desc().height != h) {
        ++skippedOther_;
        refValid_ = false;
        return;
    }
    const std::uint64_t frame = dev_.game().frameValue();
    const bool owned = dev_.mainViewOwned();
    const auto& cam = dev_.camera().viewProjection.m;
    static_assert(sizeof(cam) == sizeof(refCamera_));
    const bool sameCamera = refValid_ && refW_ == w && refH_ == h && std::memcmp(cam.data(), refCamera_, sizeof(refCamera_)) == 0;

    bool compare = false, control = false;
    if (owned) {
        ++ownedFrames_;
        if (!refValid_ || !refComposed_) ++skippedNoRef_;
        else if (!sameCamera) ++skippedCamera_;
        else if (frame - refFrame_ > kMaxAge) ++skippedNoRef_;
        else compare = true;
    } else {
        ++composedFrames_;
        // (the control is as old as the owned compares: the first composed frame of a cycle is compared with the reference of the cycle before, up to kMaxAge frames old, so
        // anything that moves with time on its own (torch flicker, sea sparkle) is in the noise floor the same way)
        if (refValid_ && sameCamera && refComposed_ && frame - refFrame_ <= kMaxAge) {
            compare = true;
            control = true;
        }
    }
    const bool store = !owned;
    if (!compare && !store) return;
    if (!dev_.readable(scene, ctx.cmd) || !dev_.readable(depth, ctx.cmd)) return;
    rhi::Texture* refColor = dev_.transient("picture_check.color", {VK_FORMAT_R16G16B16A16_SFLOAT, {w, h}});
    rhi::Texture* refDepth = dev_.transient("picture_check.depth", {VK_FORMAT_R32_SFLOAT, {w, h}});
    if (refColor == nullptr || refDepth == nullptr) return;
    if (store && !compare && (refW_ != w || refH_ != h)) refValid_ = false;
    rhi::CommandList& cl = ctx.cmd;
    cl.transition(*refColor, VK_IMAGE_LAYOUT_GENERAL);
    cl.transition(*refDepth, VK_IMAGE_LAYOUT_GENERAL);
    cl.bindPipeline(*pipe_);
    rhi::Texture* const storage[] = {refColor, refDepth};
    cl.setStorageTextures(storage);

    PictureCheckPush push{};
    push.h.frame = ctx.frameBlock.bits;
    push.h.passId = 1;
    push.colorTex = scene->bindlessSlot();
    push.depthTex = depth->bindlessSlot();
    push.width = w;
    push.height = h;
    push.colorTol = kColorTol;
    push.depthTol = kDepthTol;

    if (compare) {
        Slot* slot = nullptr;
        for (Slot& s : slots_) {
            if (s.frame == 0) {
                slot = &s;
                break;
            }
        }
        if (slot == nullptr) {
            ++skippedOther_;
        } else {
            cl.fillBuffer(*slot->gpu, 0, kStatsBytes, 0);
            cl.bufferBarrier(*slot->gpu, rhi::BufferAccess::ShaderWrite);
            const auto sr = slot->gpu->shaderRefRw(0);
            push.mode = 1;
            push.stats[0] = sr[0];
            push.stats[1] = sr[1];
            cl.setPushConstants(&push, sizeof(push));
            cl.dispatch((w + 7) / 8, (h + 7) / 8, 1);
            cl.bufferBarrier(*slot->gpu, rhi::BufferAccess::ShaderRead);
            cl.copyBuffer(*slot->readback, 0, *slot->gpu, 0, kStatsBytes);
            slot->frame = frame;
            slot->pixels = w * h;
            slot->control = control;
        }
    }
    if (store) {
        // (the compare above has read the old reference; the store writes the new one: a barrier orders them)
        if (compare) cl.memoryBarrier(VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT | VK_ACCESS_2_SHADER_STORAGE_READ_BIT,
                                      VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT | VK_ACCESS_2_SHADER_STORAGE_READ_BIT);
        push.mode = 0;
        push.stats[0] = push.stats[1] = 0;
        cl.setPushConstants(&push, sizeof(push));
        cl.dispatch((w + 7) / 8, (h + 7) / 8, 1);
        refValid_ = true;
        refComposed_ = true;
        refFrame_ = frame;
        refW_ = w;
        refH_ = h;
        std::memcpy(refCamera_, cam.data(), sizeof(refCamera_));
    }
}

void PictureCheck::collect() {
    rhi::GameDevice& game = dev_.game();
    for (Slot& s : slots_) {
        if (s.frame == 0 || game.completedFrame() < s.frame) continue;
        s.readback->invalidate();
        const auto* got = static_cast<const std::uint32_t*>(s.readback->map());
        Window& win = s.control ? control_ : owned_;
        ++win.views;
        const std::uint32_t any = got[2];
        win.colorPixels += got[0];
        win.depthPixels += got[1];
        win.pixels += any;
        win.maxColor = std::max(win.maxColor, bitsToFloat(got[3]));
        win.maxDepth = std::max(win.maxDepth, bitsToFloat(got[4]));
        if (any != 0) ++win.differingViews;
        if (any > win.worstPixels) {
            win.worstPixels = any;
            win.worstFrom = s.pixels;
            win.box[0] = 0xFFFFu - got[7];
            win.box[1] = 0xFFFFu - got[8];
            win.box[2] = got[5];
            win.box[3] = got[6];
        }
        if (!s.control) {
            ++ownedTotal_;
            if (any != 0) ++ownedTotalDiffering_;
        }
        s.frame = 0;
    }
}

void PictureCheck::report() {
    const double now = dev_.secondsSinceCreate();
    if (now - lastReport_ < 5.0) return;
    if (lastReport_ == 0.0) {
        lastReport_ = now;
        return;
    }
    lastReport_ = now;
    ++windows_;
    const auto share = [](const Window& w) { return w.worstFrom != 0 ? static_cast<double>(w.worstPixels) / static_cast<double>(w.worstFrom) : 0.0; };
    const double ownedShare = share(owned_), controlShare = share(control_);
    // the verdict needs owned views compared; a window without them is `no data`
    // (a window where the scene moved is no verdict: the camera changed during it, or two composed frames of one camera already differ by > 1 % of the picture)
    const bool moving = skippedCamera_ != 0 || controlShare > kMovingShare;
    const char* verdict = owned_.views == 0 ? "NO DATA" : moving ? "UNRELIABLE (the scene moves)" : (ownedShare > kShareTol && ownedShare > 3.0 * controlShare) ? "FAILED" : "ok";
    if (owned_.views != 0 && verdict[0] == 'F') ++failedWindows_;
    if (owned_.views != 0 && verdict[0] == 'o') ++okWindows_;
    const auto box = [](const Window& w) {
        return w.worstPixels == 0 ? std::string("-") : std::format("[{},{}..{},{}]", w.box[0], w.box[1], w.box[2], w.box[3]);
    };
    spdlog::info("native: owned picture check (5 s): {}: owned views {} compared ({} differing; skipped: camera moved {}, no reference {}, other {}), differing pixels {} (colour {}, depth {}), "
                 "worst view {} of {} = {:.4f} %, max colour {:.3f}, max depth {:.4f}, box {}; control (composed vs composed) views {}, differing {}, worst {} pixels = {:.4f} %, max colour {:.3f}, "
                 "max depth {:.4f}; frames: {} owned, {} composed; windows ok {}, FAILED {} of {}; total owned views {} ({} differing)",
                 verdict, owned_.views, owned_.differingViews, skippedCamera_, skippedNoRef_, skippedOther_, owned_.pixels, owned_.colorPixels, owned_.depthPixels, owned_.worstPixels, owned_.worstFrom,
                 100.0 * ownedShare, owned_.maxColor, owned_.maxDepth, box(owned_), control_.views, control_.differingViews, control_.worstPixels, 100.0 * controlShare, control_.maxColor,
                 control_.maxDepth, ownedFrames_, composedFrames_, okWindows_, failedWindows_, windows_, ownedTotal_, ownedTotalDiffering_);
    owned_ = {};
    control_ = {};
    skippedNoRef_ = skippedCamera_ = skippedOther_ = ownedFrames_ = composedFrames_ = 0;
}

}  // namespace rvk::enh
