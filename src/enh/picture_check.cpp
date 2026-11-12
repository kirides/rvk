#include "enh/picture_check.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <format>
#include <fstream>
#include <string>

#include <windows.h>

#include <spdlog/spdlog.h>

#include "core/paths.h"
#include "engine/hemisphere.h"
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
constexpr std::uint64_t kSettleFrames = 32;  // frames without compares after a frame-value jump (the post-capture transient decays in ~20)

std::uint64_t dumpAlign(std::uint64_t v) { return (v + 511) & ~std::uint64_t{511}; }

float bitsToFloat(std::uint32_t v) {
    float f;
    std::memcpy(&f, &v, 4);
    return f;
}

}  // namespace

PictureCheck::PictureCheck(d3d9::Device& device) : dev_(device) {
    // (the Win32 environment: run.ps1 -Environment sets it from DllMain, after the CRT copied its own)
    char buf[16] = {};
    if (GetEnvironmentVariableA("RVK_PICTURE_DUMP", buf, sizeof(buf)) != 0) dumpLeft_ = std::max(0, std::atoi(buf));
    char range[64] = {};
    if (GetEnvironmentVariableA("RVK_PICTURE_DUMP_RANGE", range, sizeof(range)) != 0) {
        char* end = nullptr;
        dumpFrom_ = std::strtoull(range, &end, 10);
        dumpTo_ = (end != nullptr && *end == '-') ? std::strtoull(end + 1, nullptr, 10) : dumpFrom_;
    }
    if (GetEnvironmentVariableA("RVK_PICTURE_DUMP_MIN", buf, sizeof(buf)) != 0) dumpMin_ = std::atof(buf);
    ignoreLight_ = GetEnvironmentVariableA("RVK_PICTURE_IGNORE_LIGHT", nullptr, 0) != 0;
    if (GetEnvironmentVariableA("RVK_PICTURE_FAKE_OWNED", buf, sizeof(buf)) != 0) fakeOwned_ = static_cast<std::uint32_t>(std::max(0, std::atoi(buf)));
    dumpEver_ = dumpLeft_ > 0 || dumpTo_ != 0;
}

PictureCheck::~PictureCheck() { setEnabled(false); }

void PictureCheck::setEnabled(bool on) {
    if (on == enabled()) return;
    if (!on) {
        if (passId_ != 0) dev_.removePass(passId_);
        passId_ = 0;
        dev_.releaseTransient("picture_check.color");
        dev_.releaseTransient("picture_check.depth");
        dev_.releaseTransient("picture_check.cur_color");
        dev_.releaseTransient("picture_check.cur_depth");
        for (Slot& s : slots_) s = {};
        refValid_ = false;
        cycles_.clear();
        lastFrame_ = 0;
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
    const bool owned = fakeOwned_ >= 3 ? frame % fakeOwned_ >= 2 : dev_.mainViewOwned();
    // draws skipped while their shader / pipeline was built (a D3D12 PSO of a new dynamic-state combination, a Vulkan shader still compiling): the picture of such a
    // frame lacks them; the count before this frame's main view goes with a stored reference, a compare whose count moved by its read-back is dropped (collectSlot)
    const std::uint64_t skipsBefore = lastSkips_;
    lastSkips_ = drawSkips();
    // a frame-value jump = a submit + wait in the middle of a frame (a capture's back-buffer read-back: rvk_screenshot, -Screenshots) or frames without a main view:
    // the frame after it is no steady frame (frame-ownership.md 14.37: the post-capture transient, a 13 % colour change that decays over ~20 frames in owned AND
    // composed frames); the reference is dropped and nothing is compared or stored for kSettleFrames
    if (lastFrame_ != 0 && frame != lastFrame_ + 1) {
        ++jumps_;
        settleUntil_ = frame + kSettleFrames;
        refValid_ = false;
    }
    lastFrame_ = frame;
    if (frame < settleUntil_) {
        ++(owned ? ownedFrames_ : composedFrames_);
        ++skippedSettle_;
        return;
    }
    const auto& cam = dev_.camera().viewProjection.m;
    static_assert(sizeof(cam) == sizeof(refCamera_));
    const bool sameCamera = refValid_ && refW_ == w && refH_ == h && std::memcmp(cam.data(), refCamera_, sizeof(refCamera_)) == 0;
    // the global lighting (frame-ownership.md 14.39): the weather's sky light (the hemisphere's SH, intensity, indoor colour) moves while the weather changes and with
    // every lightning flash; two frames of one camera under different global light are no pair (the whole picture changes, owned or composed)
    const std::uint64_t light = lightStamp();
    const bool sameLight = light == refLight_;

    bool compare = false, control = false;
    if (owned) {
        ++ownedFrames_;
        if (!refValid_ || !refComposed_) ++skippedNoRef_;
        else if (!sameCamera) ++skippedCamera_;
        else if (!sameLight) ++skippedLight_;
        else if (frame - refFrame_ > kMaxAge) ++skippedNoRef_;
        else compare = true;
    } else {
        ++composedFrames_;
        // (the control is as old as the owned compares: the first composed frame of a cycle is compared with the reference of the cycle before, up to kMaxAge frames old, so
        // anything that moves with time on its own (torch flicker, sea sparkle) is in the noise floor the same way)
        if (refValid_ && sameCamera && refComposed_ && frame - refFrame_ <= kMaxAge) {
            if (sameLight) {
                compare = true;
                control = true;
            } else {
                ++skippedLight_;
            }
        }
    }
    const bool store = !owned;
    if (!compare && !store) return;
    if (!dev_.readable(scene, ctx.cmd) || !dev_.readable(depth, ctx.cmd)) return;
    // (the dump diagnostics copy the images into a read-back buffer)
    const VkImageUsageFlags usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | (dumpEver_ ? VK_IMAGE_USAGE_TRANSFER_SRC_BIT : 0u);
    rhi::Texture* refColor = dev_.transient("picture_check.color", {VK_FORMAT_R16G16B16A16_SFLOAT, {w, h}, usage});
    rhi::Texture* refDepth = dev_.transient("picture_check.depth", {VK_FORMAT_R32_SFLOAT, {w, h}, usage});
    if (refColor == nullptr || refDepth == nullptr) return;
    if (store && !compare && (refW_ != w || refH_ != h)) refValid_ = false;
    rhi::CommandList& cl = ctx.cmd;
    const bool inRange = dumpTo_ != 0 && frame >= dumpFrom_ && frame <= dumpTo_;
    const bool dump = compare && (dumpLeft_ > 0 || inRange);
    rhi::Texture* curColor = dump ? dev_.transient("picture_check.cur_color", {VK_FORMAT_R16G16B16A16_SFLOAT, {w, h}, usage}) : nullptr;
    rhi::Texture* curDepth = dump ? dev_.transient("picture_check.cur_depth", {VK_FORMAT_R32_SFLOAT, {w, h}, usage}) : nullptr;
    const bool dumping = dump && curColor != nullptr && curDepth != nullptr;
    cl.transition(*refColor, VK_IMAGE_LAYOUT_GENERAL);
    cl.transition(*refDepth, VK_IMAGE_LAYOUT_GENERAL);
    if (dumping) {
        cl.transition(*curColor, VK_IMAGE_LAYOUT_GENERAL);
        cl.transition(*curDepth, VK_IMAGE_LAYOUT_GENERAL);
    }
    cl.bindPipeline(*pipe_);
    // (u2 / u3 are written in mode 2 only; otherwise the reference images stand in for the bindings)
    rhi::Texture* const storage[] = {refColor, refDepth, dumping ? curColor : refColor, dumping ? curDepth : refDepth};
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
            push.mode = dumping ? 2u : 1u;
            push.stats[0] = sr[0];
            push.stats[1] = sr[1];
            cl.setPushConstants(&push, sizeof(push));
            cl.dispatch((w + 7) / 8, (h + 7) / 8, 1);
            cl.bufferBarrier(*slot->gpu, rhi::BufferAccess::ShaderRead);
            cl.copyBuffer(*slot->readback, 0, *slot->gpu, 0, kStatsBytes);
            slot->frame = frame;
            slot->refFrame = refFrame_;
            slot->refSkips = refSkips_;
            slot->pixels = w * h;
            slot->control = control;
            slot->dumped = false;
            if (dumping) {
                // the four images into the slot's read-back buffer: ref colour, current colour, ref depth, current depth (rows bufferRowPitch apart, 512-aligned parts)
                const std::uint64_t cb = dumpAlign(std::uint64_t{rhi::bufferRowPitch(VK_FORMAT_R16G16B16A16_SFLOAT, w)} * h);
                const std::uint64_t db = dumpAlign(std::uint64_t{rhi::bufferRowPitch(VK_FORMAT_R32_SFLOAT, w)} * h);
                const std::uint64_t need = 2 * cb + 2 * db;
                if (!slot->dump || slot->dumpBytes < need) {
                    slot->dump = rdev->createBuffer({need, VK_BUFFER_USAGE_TRANSFER_DST_BIT, rhi::Memory::Readback, "owned picture check dump"});
                    slot->dumpBytes = slot->dump ? need : 0;
                }
                if (slot->dump) {
                    cl.memoryBarrier(VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT, VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_READ_BIT);
                    rhi::Texture* const parts[] = {refColor, curColor, refDepth, curDepth};
                    const std::uint64_t offs[] = {0, cb, 2 * cb, 2 * cb + db};
                    for (int i = 0; i < 4; ++i) {
                        cl.transition(*parts[i], VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
                        cl.copyTextureToBuffer(*slot->dump, offs[i], *parts[i]);
                    }
                    for (rhi::Texture* t : parts) cl.transition(*t, VK_IMAGE_LAYOUT_GENERAL);
                    cl.bindPipeline(*pipe_);
                    cl.setStorageTextures(storage);
                    slot->dumped = true;
                    slot->w = w;
                    slot->h = h;
                }
            }
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
        refSkips_ = skipsBefore;
        refW_ = w;
        refH_ = h;
        std::memcpy(refCamera_, cam.data(), sizeof(refCamera_));
        refLight_ = light;
    }
}

std::uint64_t PictureCheck::lightStamp() const {
    if (ignoreLight_) return 0;
    engine::hemisphere::Light l;
    if (!engine::hemisphere::latest(l)) return 0;
    std::uint64_t h = 1469598103934665603ull;
    const auto mix = [&](const void* p, std::size_t n) {
        for (std::size_t i = 0; i < n; ++i) {
            h ^= static_cast<const std::uint8_t*>(p)[i];
            h *= 1099511628211ull;
        }
    };
    mix(l.regs, sizeof(l.regs));
    mix(l.indoor, sizeof(l.indoor));
    mix(&l.intensity, sizeof(l.intensity));
    return h;
}

std::uint64_t PictureCheck::drawSkips() const {
    const auto& st = dev_.stats();
    return st.skippedCompiling + st.skippedNoShader + st.skippedPipeline + dev_.game().pipelineSkips();
}

void PictureCheck::collect() {
    // in frame order: an owned view's cycle is open before its closing control is read back
    std::array<Slot*, kSlots> ready{};
    std::size_t n = 0;
    const std::uint64_t done = dev_.game().completedFrame();
    for (Slot& s : slots_) {
        if (s.frame != 0 && done >= s.frame) ready[n++] = &s;
    }
    std::sort(ready.begin(), ready.begin() + static_cast<std::ptrdiff_t>(n), [](const Slot* a, const Slot* b) { return a->frame < b->frame; });
    for (std::size_t i = 0; i < n; ++i) collectSlot(*ready[i]);
}

void PictureCheck::judge(const Cycle& c, double controlShare, bool closed) {
    const double share = c.worstFrom != 0 ? static_cast<double>(c.worstPixels) / static_cast<double>(c.worstFrom) : 0.0;
    ++(closed ? cyclesJudged_ : cyclesOpenJudged_);
    if (share > kShareTol && share > 3.0 * controlShare) {
        ++cyclesFailed_;
        if (failLines_++ < 8) {
            spdlog::info("native: owned picture check: FAILED cycle of reference frame {}: worst owned view frame {} (age {}) {} pixels = {:.4f} % (colour {}, depth {}), box [{},{}..{},{}], "
                         "{} owned views; its control {:.4f} % ({})",
                         c.ref, c.worstFrame, c.worstFrame - c.ref, c.worstPixels, 100.0 * share, c.worstColor, c.worstDepth, c.box[0], c.box[1], c.box[2], c.box[3], c.views,
                         100.0 * controlShare, closed ? "the closing control" : "the window's worst control");
        }
        if (share > failWorst_) {
            failWorst_ = share;
            failControl_ = controlShare;
        }
    }
}

void PictureCheck::collectSlot(Slot& s) {
    s.readback->invalidate();
    const auto* got = static_cast<const std::uint32_t*>(s.readback->map());
    const std::uint32_t any = got[2];
    const double share = s.pixels != 0 ? static_cast<double>(any) / static_cast<double>(s.pixels) : 0.0;
    const std::uint64_t frame = s.frame, ref = s.refFrame;
    const bool control = s.control, dumped = s.dumped;
    s.frame = 0;
    s.dumped = false;
    // a draw of the reference's frame, of this frame or of a frame between was skipped (its shader / pipeline was being built): no valid pair. The count is read now,
    // when every draw of this frame has been recorded (D3D12 records the packets on worker threads), so later frames' skips drop it too (conservative)
    if (drawSkips() != s.refSkips) {
        ++droppedSkips_;
        if (!control) std::erase_if(cycles_, [&](const Cycle& c) { return c.ref == ref; });
        return;
    }
    Window& win = control ? control_ : owned_;
    ++win.views;
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
    if (!control) {
        ++ownedTotal_;
        if (any != 0) ++ownedTotalDiffering_;
        auto it = std::ranges::find_if(cycles_, [&](const Cycle& c) { return c.ref == ref; });
        if (it == cycles_.end()) it = cycles_.insert(cycles_.end(), Cycle{ref});
        ++it->views;
        if (any >= it->worstPixels) {
            it->worstPixels = any;
            it->worstFrom = s.pixels;
            it->worstFrame = frame;
            it->worstColor = got[0];
            it->worstDepth = got[1];
            it->box[0] = 0xFFFFu - got[7];
            it->box[1] = 0xFFFFu - got[8];
            it->box[2] = got[5];
            it->box[3] = got[6];
        }
    } else if (auto it = std::ranges::find_if(cycles_, [&](const Cycle& c) { return c.ref == ref; }); it != cycles_.end()) {
        // the closing control of a cycle: the first composed frame compared with the reference its owned views were compared with
        judge(*it, share, true);
        cycles_.erase(it);
    }
    // per-view detail (outliers only: the scene stands still (the cycle's age-1 control below 0.1 %), the view differs on more than 0.1 % of the picture and 3 x
    // that control; at most 24 lines per window)
    if (control && frame - ref == 1) stillShare_ = share;
    if (control && frame - ref > 1) closingShare_ = share;  // (an old control: the noise floor at the age of the owned views)
    const bool outlier = stillShare_ < 0.001 && share > 0.0005 && share > 3.0 * std::max(stillShare_, closingShare_);
    if (outlier && detailLines_ < 24) {
        ++detailLines_;
        spdlog::info("native: owned picture check: {} view of frame {} (reference frame {}, age {}): {} pixels = {:.4f} % (colour {}, depth {}), max colour {:.3f}, max depth {:.4f}, "
                     "box [{},{}..{},{}]",
                     control ? "control" : "owned", frame, ref, frame - ref, any, 100.0 * share, got[0], got[1], bitsToFloat(got[3]), bitsToFloat(got[4]), 0xFFFFu - got[7],
                     0xFFFFu - got[8], got[5], got[6]);
    }
    // (not the views of a scene that moves: a camera cut / the unfreeze differ on most of the picture)
    const bool inRange = dumpTo_ != 0 && frame >= dumpFrom_ && frame <= dumpTo_ && share >= dumpMin_ && rangeLeft_ > 0;
    if (dumped && s.dump && (inRange || (dumpLeft_ > 0 && outlier && share < 0.2 && okWindows_ > 0))) {
        if (inRange) --rangeLeft_;
        else --dumpLeft_;
        s.dump->invalidate();
        const auto* bytes = static_cast<const char*>(s.dump->map());
        const std::uint32_t cp = rhi::bufferRowPitch(VK_FORMAT_R16G16B16A16_SFLOAT, s.w), dp = rhi::bufferRowPitch(VK_FORMAT_R32_SFLOAT, s.w);
        const std::uint64_t cb = dumpAlign(std::uint64_t{cp} * s.h), db = dumpAlign(std::uint64_t{dp} * s.h);
        std::error_code ec;
        const auto dir = paths::dataDir() / L"dumps";
        std::filesystem::create_directories(dir, ec);
        const auto file = dir / std::format("picture_check_{}_{}_ref{}.bin", control ? "control" : "owned", frame, ref);
        std::ofstream out(file, std::ios::binary);
        // header: magic, w, h, colour pitch, depth pitch, part offsets (4 x u64: ref colour, current colour, ref depth, current depth), then the parts
        const std::uint32_t hdr[5] = {0x31435052u, s.w, s.h, cp, dp};
        const std::uint64_t offs[4] = {0, cb, 2 * cb, 2 * cb + db};
        out.write(reinterpret_cast<const char*>(hdr), sizeof(hdr));
        out.write(reinterpret_cast<const char*>(offs), sizeof(offs));
        out.write(bytes, static_cast<std::streamsize>(2 * cb + 2 * db));
        spdlog::info("native: owned picture check: dumped {} ({} left)", file.string(), dumpLeft_);
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
    detailLines_ = 0;
    ++windows_;
    const auto share = [](const Window& w) { return w.worstFrom != 0 ? static_cast<double>(w.worstPixels) / static_cast<double>(w.worstFrom) : 0.0; };
    const double ownedShare = share(owned_), controlShare = share(control_);
    // cycles whose closing control will never come (the reference was replaced without a compare against it: a camera change, a frame jump, an age limit) are judged
    // against the window's worst control view; the cycle of the current reference stays open into the next window
    std::erase_if(cycles_, [&](const Cycle& c) {
        if (refValid_ && c.ref == refFrame_) return false;
        judge(c, controlShare, false);
        return true;
    });
    // the verdict needs owned views compared; a window without them is `no data`
    // (a window where the scene moved is no verdict: the camera changed during it, or two composed frames of one camera already differ by > 1 % of the picture)
    // FAILED = a cycle whose worst owned view differs by more than kShareTol of the picture AND 3 x its own closing control (frame-ownership.md 14.37: the window's
    // control was the wrong yardstick when the scene changed during a cycle whose closing control fell into the next window)
    const bool moving = skippedCamera_ != 0 || skippedLight_ != 0 || controlShare > kMovingShare;
    const std::uint64_t judged = cyclesJudged_ + cyclesOpenJudged_;
    const char* verdict = owned_.views == 0 && judged == 0 ? "NO DATA" : moving ? "UNRELIABLE (the scene moves)" : cyclesFailed_ != 0 ? "FAILED" : "ok";
    if (verdict[0] == 'F') ++failedWindows_;
    if (verdict[0] == 'o') ++okWindows_;
    failLines_ = 0;
    const auto box = [](const Window& w) {
        return w.worstPixels == 0 ? std::string("-") : std::format("[{},{}..{},{}]", w.box[0], w.box[1], w.box[2], w.box[3]);
    };
    spdlog::info("native: owned picture check (5 s): {}: owned views {} compared ({} differing; skipped: camera moved {}, global light changed {}, no reference {}, other {}, settling after a frame jump {} "
                 "({} jumps), draw skips {}), differing pixels {} (colour {}, depth {}), "
                 "worst view {} of {} = {:.4f} %, max colour {:.3f}, max depth {:.4f}, box {}; control (composed vs composed) views {}, differing {}, worst {} pixels = {:.4f} %, max colour {:.3f}, "
                 "max depth {:.4f}; cycles judged {} by their closing control + {} by the window's, failed {}{}; frames: {} owned, {} composed; windows ok {}, FAILED {} of {}; "
                 "total owned views {} ({} differing)",
                 verdict, owned_.views, owned_.differingViews, skippedCamera_, skippedLight_, skippedNoRef_, skippedOther_, skippedSettle_, jumps_, droppedSkips_, owned_.pixels, owned_.colorPixels,
                 owned_.depthPixels, owned_.worstPixels, owned_.worstFrom, 100.0 * ownedShare, owned_.maxColor, owned_.maxDepth, box(owned_), control_.views, control_.differingViews,
                 control_.worstPixels, 100.0 * controlShare, control_.maxColor, control_.maxDepth, cyclesJudged_, cyclesOpenJudged_, cyclesFailed_,
                 cyclesFailed_ != 0 ? std::format(" (worst {:.4f} % against its control {:.4f} %)", 100.0 * failWorst_, 100.0 * failControl_) : std::string(), ownedFrames_,
                 composedFrames_, okWindows_, failedWindows_, windows_, ownedTotal_, ownedTotalDiffering_);
    owned_ = {};
    control_ = {};
    skippedNoRef_ = skippedCamera_ = skippedLight_ = skippedOther_ = skippedSettle_ = droppedSkips_ = jumps_ = ownedFrames_ = composedFrames_ = 0;
    cyclesJudged_ = cyclesOpenJudged_ = cyclesFailed_ = 0;
    failWorst_ = failControl_ = 0.0;
}

}  // namespace rvk::enh
