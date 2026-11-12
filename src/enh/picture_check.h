#pragma once

// The owned picture check (docs/research/frame-ownership.md 7.47; setting native.picture_check, Debug, default off). The owned path's own check frames compare CPU data
// (registers, records, plans) and never the PICTURE the owned draws make; 7.46 (trees past 50 instances read zeros) lived for weeks because of that. This pass compares the
// pictures on the GPU: at AfterOpaque of the frame's first main view it
//   * on a composed frame (the engine's own ops: the warm-up / check frames of native.owned_check) stores the opaque scene colour + the linear depth map as the REFERENCE
//     (two images at the scene size) and, when a reference of the same camera is at most kMaxAge frames old, first compares it with that reference = the CONTROL (what two
//     composed frames of one still scene, as far apart as the owned compares, differ by: the noise floor, not zero when something moves on its own);
//   * on an owned frame (the main view's ops left the lists, `Device::mainViewOwned`) with the camera of the reference compares the scene with it.
// The compare is a compute reduction (shaders/debug/picture_check.comp.hlsl): pixels whose colour / depth differ beyond a tolerance, the largest differences, the bounding box.
// The result is read back one frame later; rvk.log `native: owned picture check (5 s)`. The scene must stand still (time frozen, camera fixed: the rvkcompare spots) and no
// upscaling jitter; the check says so (`skipped`). Off = the pass is not registered, no image or buffer exists, nothing is drawn extra.

#include <array>
#include <cstdint>
#include <memory>
#include <vector>

#include "d3d9/device.h"
#include "shader/abi.h"
#include "rhi/rhi.h"

namespace rvk::enh {

// mirror of shaders/debug/picture_check.hlsli PictureCheckPush
struct PictureCheckPush {
    shader::abi::PassPushHeader h;
    std::uint32_t colorTex;
    std::uint32_t depthTex;
    std::uint32_t width;
    std::uint32_t height;
    std::uint32_t mode;
    float colorTol;
    float depthTol;
    std::uint32_t pad;
    std::uint32_t stats[2];
    std::uint32_t pad2[2];
};
static_assert(sizeof(PictureCheckPush) == 64 && sizeof(PictureCheckPush) <= shader::abi::kPushConstantBudget);

class PictureCheck {
public:
    explicit PictureCheck(d3d9::Device& device);
    ~PictureCheck();
    PictureCheck(const PictureCheck&) = delete;
    PictureCheck& operator=(const PictureCheck&) = delete;

    void setEnabled(bool on);  // render thread (the settings callbacks run at the frame boundary)
    bool enabled() const { return passId_ != 0; }

private:
    static constexpr std::uint32_t kSlots = 4;  // results in flight (read back once their frame completed)
    struct Slot {
        std::unique_ptr<rhi::Buffer> gpu, readback;
        std::uint64_t frame = 0;     // Device::frameValue() of the compare (0 = free)
        std::uint32_t pixels = 0;
        std::uint64_t refFrame = 0;  // frameValue of the reference it was compared with
        std::uint64_t refSkips = 0;  // drawSkips() before the reference's frame: a moved count at the read-back = a draw of the pair was skipped
        bool control = false;
        // RVK_PICTURE_DUMP: the reference + current colour / depth of this compare (read back; written when the view differs)
        std::unique_ptr<rhi::Buffer> dump;
        std::uint64_t dumpBytes = 0;
        bool dumped = false;
        std::uint32_t w = 0, h = 0;
    };
    struct Window {
        std::uint64_t views = 0, differingViews = 0, pixels = 0, colorPixels = 0, depthPixels = 0, worstPixels = 0;
        float maxColor = 0.0f, maxDepth = 0.0f;
        std::uint32_t box[4] = {0xFFFFu, 0xFFFFu, 0, 0};  // of the worst view
        std::uint32_t worstFrom = 0;
    };
    // the frames between a reference and an owned compare + the cycle's closing control (the first composed frame compared with the same reference): an owned
    // view is judged against the control of ITS reference, not against whatever control the 5 s window holds (a change of the scene during the cycle shows in both)
    struct Cycle {
        std::uint64_t ref = 0;
        std::uint64_t views = 0;
        std::uint32_t worstPixels = 0, worstFrom = 0;
        std::uint64_t worstFrame = 0;  // the worst owned view: its frame, colour / depth pixels, box
        std::uint32_t worstColor = 0, worstDepth = 0;
        std::uint32_t box[4] = {};
    };
    void record(d3d9::PassContext& ctx);
    void collect();
    void collectSlot(Slot& s);
    void judge(const Cycle& c, double controlShare, bool closed);
    std::uint64_t drawSkips() const;
    void report();
    bool ensure(rhi::Device& rdev);

    d3d9::Device& dev_;
    std::uint32_t passId_ = 0;
    std::unique_ptr<rhi::Pipeline> pipe_;
    bool failed_ = false;
    std::array<Slot, kSlots> slots_;
    // the reference
    bool refValid_ = false, refComposed_ = false;
    std::uint64_t refFrame_ = 0;
    std::uint64_t refSkips_ = 0;    // drawSkips() at the main view before the reference's
    std::uint64_t lastSkips_ = 0;   // drawSkips() at the last main view
    std::uint64_t lastFrame_ = 0;   // frameValue of the last main view
    std::uint64_t settleUntil_ = 0; // no compare / store before this frame (after a frame-value jump: a capture's mid-frame submit + wait, a gap)
    std::vector<Cycle> cycles_;     // open cycles (owned views compared, closing control not yet read back)
    int dumpLeft_ = 0;              // RVK_PICTURE_DUMP=<n>: at most n outlier views are written to bin/rvk/dumps
    bool dumpEver_ = false;         // RVK_PICTURE_DUMP was set (the images keep the copy usage once the dumps ran out: no re-creation)
    std::uint32_t detailLines_ = 0;  // per-view lines this window
    double stillShare_ = 1.0;         // the last age-1 control view's share (warm-up vs check frame): the scene stands still now when it is small
    double closingShare_ = 1.0;       // the last control view older than one frame (a cycle's closing control)
    std::uint32_t failLines_ = 0;     // failed-cycle lines this window
    std::uint32_t refW_ = 0, refH_ = 0;
    float refCamera_[16] = {};
    Window owned_, control_;
    std::uint64_t skippedNoRef_ = 0, skippedCamera_ = 0, skippedOther_ = 0, skippedSettle_ = 0, droppedSkips_ = 0, composedFrames_ = 0, ownedFrames_ = 0;
    std::uint64_t jumps_ = 0;                                       // frame-value jumps this window
    std::uint64_t cyclesJudged_ = 0, cyclesFailed_ = 0, cyclesOpenJudged_ = 0;  // this window
    double failWorst_ = 0.0, failControl_ = 0.0;                    // the worst failed cycle: its owned share and its control's
    std::uint64_t okWindows_ = 0, failedWindows_ = 0, ownedTotal_ = 0, ownedTotalDiffering_ = 0, windows_ = 0;
    double lastReport_ = 0.0;
};

}  // namespace rvk::enh
