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
        bool control = false;
    };
    struct Window {
        std::uint64_t views = 0, differingViews = 0, pixels = 0, colorPixels = 0, depthPixels = 0, worstPixels = 0;
        float maxColor = 0.0f, maxDepth = 0.0f;
        std::uint32_t box[4] = {0xFFFFu, 0xFFFFu, 0, 0};  // of the worst view
        std::uint32_t worstFrom = 0;
    };
    void record(d3d9::PassContext& ctx);
    void collect();
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
    std::uint32_t refW_ = 0, refH_ = 0;
    float refCamera_[16] = {};
    Window owned_, control_;
    std::uint64_t skippedNoRef_ = 0, skippedCamera_ = 0, skippedOther_ = 0, composedFrames_ = 0, ownedFrames_ = 0;
    std::uint64_t okWindows_ = 0, failedWindows_ = 0, ownedTotal_ = 0, ownedTotalDiffering_ = 0, windows_ = 0;
    double lastReport_ = 0.0;
};

}  // namespace rvk::enh
