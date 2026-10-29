#pragma once

// Frame generation with AMD FidelityFX (FSR 3 frame generation; the ML provider of FSR 4 where the API offers it): stages F2 / F3
// of docs/research/frame-generation.md. Two ways to show the frames:
//   * the AMD frame generation swapchain (default, F3): vk::Swapchain creates AMD's swapchain (vk/fg_swapchain.h), the generator's
//     context is configured with its callbacks, the interpolation runs inside the swapchain's present and AMD paces the generated
//     and the real frame; the overlay is drawn by the present callback on both. This file records the Prepare (depth, motion,
//     camera) and configures the generator every frame;
//   * the manual model (framegen.manual, also the fallback when the swapchain cannot be created): the generated frame is computed
//     into an image of ours, Device::present shows it first and the real frame half a base interval later.
//
// Per rendered frame (a frame is only generated while the game is in its gameplay state):
//   AfterMainView   framegen.depth     the main view's hardware depth -> an R32F image in the D3D convention 0 (near) .. 1 (far)
//                                      (shaders/upscale/prep.comp, the upscaler's own conversion: reversed-Z handled there)
//   BeforeGui       framegen.hudless   a copy of the back buffer before the GUI is drawn (FG's HUDLessColor)
//   BeforePresent   framegen.generate  swapchain model: ffxConfigure (frame id, HUDLessColor, callbacks, enabled) + Prepare (depth, the
//                                      G-buffer's motion vectors, the camera, jitter, frame time, reset); the swapchain interpolates
//                                      manual model: a copy of the finished frame (the interpolation's colour input), the same
//                                      Configure + Prepare and the interpolation into an image of ours
//                   framegen.debug     framegen.debug = motion | depth | hudless | generated replaces the shown frame by an input
//
// Manual model: Device::present() shows the generated frame first and the real frame half a base interval later. HalDevice::SwapScreen
// captures the generated frame next to the real one (screenshots frame_<n>_gen.png; swapchain model: real frames only). Nothing
// exists (no passes, no transients, no DLL) while framegen.mode is off.
//
// Motion vectors, depth and colour are the ones the upscaler uses: the G-buffer's motion (uv units, previous - current, jitter
// free; camera motion everywhere, per-object motion for characters with upscale.object_motion) and the hardware depth. The G-buffer
// is forced on while frame generation is (like FSR does). With the upscaler on, the camera jitter is the one FSR was given.

#include <array>
#include <chrono>
#include <cstdint>
#include <memory>

#include "d3d9/device.h"
#include "vk/framegen.h"
#include "vk/passes.h"

namespace rvk::enh {

enum class FrameGenMode : std::uint8_t { Off, Fsr3, Fsr4 };
enum class FrameGenView : std::uint8_t { Off, Motion, Depth, Hudless, Generated };

// ---- the rules (pure; tests/framegen_tests.cpp) --------------------------------------------------------------------------

// the facts of one frame at its end
struct FrameGenFacts {
    bool providerReady = true;   // the DLL / context is usable (false: failed to load or create this session)
    bool gameplay = true;        // the engine shows the world (not a menu page, not the loading screen)
    bool paused = false;         // the game is paused (identical frames)
    bool mainView = true;        // the frame had a main view (the camera is valid)
    bool inputs = true;          // depth prepared and G-buffer motion written for this frame
    bool sizeOk = true;          // the render size fits the display
    bool presentSizeOk = true;   // swapchain model: the back buffer is the swapchain's size (the generator interpolates the swapchain's images)
    bool unpaced = false;        // a loading-screen frame
};
enum class FrameGenBlock : std::uint8_t { None, NoProvider, Menu, Loading, Paused, NoMainView, NoInputs, Size, PresentSize };
FrameGenBlock frameGenBlocker(const FrameGenFacts& f);
const char* frameGenBlockName(FrameGenBlock b);

// a camera pose for cut detection (engine units: centimetres; forward = the view direction)
struct FrameGenPose {
    std::array<float, 3> position{};
    std::array<float, 3> forward{0.0f, 0.0f, 1.0f};
    float fovY = 1.0f;
    bool valid = false;
};
struct FrameGenCutLimits {
    float teleport = 5000.0f;  // cm moved in one frame (50 m: a teleport, a dialogue camera cut; the hero runs ~0.1 m per frame)
    float turnDegrees = 60.0f; // view direction change in one frame
    float fovRatio = 1.25f;    // field of view change (zoom)
};
// the camera moved discontinuously between two consecutive frames: the generator must not interpolate across it
bool frameGenCameraCut(const FrameGenPose& prev, const FrameGenPose& cur, const FrameGenCutLimits& limits = {});

// What happens this frame, over time: the first generated frame after any interruption resets the generator (and shows no
// generated frame: the provider passes the frame through on a reset), so does a camera cut.
class FrameGenState {
public:
    struct Step {
        bool active = false;   // generate (configure, prepare, dispatch) this frame
        bool reset = false;    // ... with the reset flag
        bool show = false;     // the generated frame is a usable interpolation (not the pass-through of a reset)
        std::uint64_t frameId = 0;  // increases by exactly 1 per active frame (any other step resets the provider)
    };
    Step advance(FrameGenBlock blocker, bool cameraCut);
    // a context was (re)created or the mode changed: the next active frame resets
    void invalidate() { wasActive_ = false; }
    std::uint64_t activeFrames() const { return frameId_; }

private:
    bool wasActive_ = false;
    std::uint64_t frameId_ = 0;
};

// ---- the feature ----------------------------------------------------------------------------------------------------------

class FrameGeneration final : public d3d9::Device::FrameGenClient {
public:
    struct Settings {
        FrameGenMode mode = FrameGenMode::Off;
        bool manual = false;      // framegen.manual: generate and present ourselves (without it nothing is generated before stage F3)
        FrameGenView view = FrameGenView::Off;
    };

    explicit FrameGeneration(d3d9::Device& device);
    ~FrameGeneration() override;
    FrameGeneration(const FrameGeneration&) = delete;
    FrameGeneration& operator=(const FrameGeneration&) = delete;

    void apply(const Settings& s);
    // the generator's context goes (synchronously) and the swapchain backend is released: before the overlay and the device are torn down
    void shutdown();
    // frame generation is meant to present through AMD's swapchain (mode on, not manual, not failed): the HAL keeps exclusive
    // fullscreen off then
    bool active() const { return on_ && !failed_; }
    bool wantsSwapchain() const { return on_ && !failed_ && !manualEffective(); }
    // the swapchain is about to be re-created or destroyed (d3d9::Device::FrameGenClient)
    void swapchainWillChange() override;
    // base / output numbers for the log lines (frames generated / shown, resets ...)
    struct Counters {
        std::uint64_t generated = 0, shown = 0, resets = 0, cuts = 0, blocked = 0;
    };
    Counters counters() const { return {frames_, shown_, resets_, cuts_, blocked_}; }
    // frame generation needs the G-buffer's motion vectors
    bool wantsGbuffer() const { return s_.mode != FrameGenMode::Off && !failed_; }
    // once per present, before the passes of that frame run (engine thread): the engine's menu / loading / pause state
    void setGameState(bool gameplay, bool paused) { gameplay_ = gameplay; paused_ = paused; }
    // the frame generated this frame as tightly packed BGRA8 (after the BeforePresent pass ran); false when there is none
    bool readGenerated(std::vector<std::uint8_t>& bgra, std::uint32_t& width, std::uint32_t& height);

private:
    void recordDepth(d3d9::PassContext& ctx);
    void recordHudless(d3d9::PassContext& ctx);
    void recordGenerate(d3d9::PassContext& ctx);
    void recordDebug(d3d9::PassContext& ctx);
    bool ensureContext(VkExtent2D display, VkFormat format, bool swapchain, VkFormat hudlessFormat);
    bool ensurePipelines(VkFormat target);
    void dropContext();
    bool manualEffective() const { return s_.manual || fallbackManual_; }
    void updateBackend();           // the swapchain backend follows the mode (create / release; fall back to the manual model)
    void recordSwapchain(d3d9::PassContext& ctx, FrameGenBlock blocker, bool cut, const FrameGenPose& pose, VkExtent2D display);
    const char* hudlessKey() const { return hudlessIndex_ == 0 ? "framegen.hudless.0" : "framegen.hudless.1"; }
    void enable();
    void disable();
    void logBlock(FrameGenBlock b);

    d3d9::Device& dev_;
    Settings s_;
    bool on_ = false;          // passes registered
    bool failed_ = false;      // the DLL / the context / a pipeline is not available: off for the session
    bool gameplay_ = true, paused_ = false;
    std::shared_ptr<vk::FrameGen> fg_;  // retired with the frame that last used it
    VkExtent2D ctxDisplay_{};
    bool mlActive_ = false;
    vk::PassLayout layout_;             // set 1: one storage image (the R32F depth)
    VkPipeline prep_ = VK_NULL_HANDLE;  // shaders/upscale/prep.comp
    vk::PassLayout debugLayout_;        // no set 1 bindings
    VkPipeline debug_ = VK_NULL_HANDLE;
    VkFormat debugFormat_ = VK_FORMAT_UNDEFINED;
    FrameGenState state_;
    // this frame
    std::uint64_t depthFrame_ = 0;      // the frame (device frame value) whose depth was prepared
    VkExtent2D renderSize_{};
    bool hudlessFrame_ = false;         // a hudless copy of this frame exists
    vk::Image* generated_ = nullptr;    // the interpolated frame of this frame (null: none), valid until the next BeforePresent
    bool generatedShown_ = false;       // ... a usable one
    std::uint32_t generatedWidth_ = 0, generatedHeight_ = 0;
    // the previous frame's camera (cut detection)
    FrameGenPose lastPose_;
    // log state
    FrameGenBlock lastBlock_ = FrameGenBlock::None;
    bool manualNoticed_ = false;
    bool fallbackManual_ = false;       // the swapchain could not be created: the manual model instead
    std::chrono::steady_clock::time_point retryAt_{};  // swapchain model pending (minimised window ...): next attempt
    int hudlessIndex_ = 0;              // the HUD-less copy alternates: the swapchain's interpolation reads the previous one meanwhile
    bool configured_ = false;           // swapchain model: the generator is configured (callbacks attached)
    bool lastEnabled_ = false;
    std::uint64_t lastFrameId_ = 0;
    std::uint64_t lastLogSwapchain_ = 0;
    std::uint64_t frames_ = 0, shown_ = 0, resets_ = 0, cuts_ = 0, blocked_ = 0, ringWaits_ = 0;
    double ringWaitMs_ = 0.0;
    double lastLog_ = -1e9;
};

}  // namespace rvk::enh
