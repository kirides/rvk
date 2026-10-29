#pragma once

// AMD FidelityFX frame generation through the signed FFX API DLLs (third_party/amd_fidelityfx: amd_fidelityfx_loader_vk.dll
// loads amd_fidelityfx_framegeneration_vk.dll from its own folder, bin\rvk). docs/research/frame-generation.md.
//
// Providers (ffxQueryDescGetVersions, per device):
//   * frame generation (optical flow + interpolation): "3.1.6" on every GPU that can do storage image loads; the ML provider
//     (FSR Frame Generation 4.x) only where the display driver / GPU generation offers it (RDNA 4 and newer). Offered or not
//     is decided by the query alone: nothing here guesses version ids, spoofs a GPU or forces an ML context.
//   * the frame generation swapchain (presentation + pacing, stage F3, not used yet): the DLL's "3.1.7"; a driver-provided
//     "4.0.0 *" is also listed and must never be picked by default (its destroy crashes: frame-generation.md 5.3).
//
// This wrapper is the manual model: the context is driven without a swapchain (FFX_FRAMEGENERATION_FLAG_NO_SWAPCHAIN_CONTEXT_NOTIFY on
// configure + prepare), prepare + dispatch are recorded into our own command buffer, and the interpolated frame lands in our own
// image. Resource states: the DLL's backend transitions each resource from the state given in the call and leaves it there
// (COMPUTE_READ = SHADER_READ_ONLY_OPTIMAL, UNORDERED_ACCESS = GENERAL), so the images' tracked layouts are set to those.
//
// Not thread safe (the FFX contexts are not): the render thread only. In the callback model the swapchain's present (another
// thread) dispatches the interpolation: FgSwapchain::contextMutex() covers configure / prepare / create / destroy as well as the present.

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

#include "vk/image.h"
#include "vk/vk.h"

namespace rvk::vk {

class Context;
struct FrameGenReport;

// Startup probe: loads the loader + (through it) the frame generation effect DLL, lists the providers for this device and unloads
// again. `loaderDll` = amd_fidelityfx_loader_vk.dll. A missing effect DLL is not an error (frame generation is a separate
// download): the report says why it is unavailable. Takes a few tens of ms when the DLL is there, nothing when it is not.
FrameGenReport probeFrameGeneration(Context& ctx, const std::filesystem::path& loaderDll);

// one provider the FFX API offers
struct FrameGenVersion {
    std::uint64_t id = 0;
    std::string name;       // "3.1.6", "4.0.0 *" (a trailing " *" = provided by the display driver)
    int major = 0, minor = 0, patch = 0;  // parsed from the first digits of the name (0 = none)
    bool fromDriver = false;
};

// pure helpers (unit tests: tests/framegen_tests.cpp)
FrameGenVersion parseFrameGenVersion(std::uint64_t id, std::string name);
// the newest frame generation provider of a family: ml = major >= 4 (FSR 4), else major 3 (FSR 3); nullptr when none is offered
const FrameGenVersion* pickFrameGenProvider(const std::vector<FrameGenVersion>& list, bool ml);
// the swapchain provider to pin (ffxOverrideVersion): the newest one the DLL itself provides, never a driver-provided one
const FrameGenVersion* pickSwapchainProvider(const std::vector<FrameGenVersion>& list);
// the version list as one line, "id: name, ..."
std::string describeVersions(const std::vector<FrameGenVersion>& list);

struct FrameGenCreateInfo {
    bool ml = false;                       // the ML provider (FSR 4); false = the newest FSR 3.x
    VkExtent2D displaySize{1, 1};          // rendered and generated frames
    VkExtent2D maxRenderSize{1, 1};        // depth / motion vector size (<= the display size)
    VkFormat backBufferFormat = VK_FORMAT_B8G8R8A8_UNORM;  // the colour the interpolation reads and writes
    VkFormat hudlessFormat = VK_FORMAT_UNDEFINED;          // != UNDEFINED: a HUDLessColor image of this format is passed to configure
    bool hdr = false;                      // the colour is linear HDR (the engine's back buffer is display-referred LDR: false)
    bool depthInverted = false;            // the depth input runs 1 (near) .. 0 (far)
    bool depthInfinite = false;
    bool asyncCompute = false;
    bool debugChecking = false;            // the DLL validates its inputs and reports through the log
    bool swapchain = false;                // the callback model: the AMD swapchain (vk/fg_swapchain.h) dispatches the interpolation
                                           // inside its present; configure / prepare then do not carry the NO_SWAPCHAIN flag
};

// ffxConfigure (per frame)
struct FrameGenConfigure {
    bool enabled = true;
    Image* hudless = nullptr;  // optional, the frame without the GUI
    std::uint64_t frameId = 0;
    VkExtent2D rect{0, 0};     // the area of the colour image that is interpolated (0 = the display size)
    std::uint32_t debugFlags = 0;  // FFX_FRAMEGENERATION_FLAG_DRAW_DEBUG_*
    // the callback model (FrameGenCreateInfo::swapchain): the swapchain's handle, its present callback + user (FgSwapchain), and
    // whether the interpolation of this frame must reset (a camera cut: OR-ed into the swapchain's own reset in the dispatch callback)
    VkSwapchainKHR swapchain = VK_NULL_HANDLE;
    const void* presentCallback = nullptr;
    void* presentUser = nullptr;
    bool reset = false;
};

// ffxDispatch(PrepareV2): the frame's depth + motion vectors + camera
struct FrameGenPrepare {
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    std::uint64_t frameId = 0;
    VkExtent2D renderSize{1, 1};
    float jitterX = 0.0f, jitterY = 0.0f;           // this frame's camera jitter in render pixels (0 without FSR)
    float motionScaleX = 1.0f, motionScaleY = 1.0f;  // pixel-space motion = motion * scale (uv-space motion: the render size)
    float frameTimeMs = 16.6f;
    bool reset = false;                              // the camera cut: drop the history
    float cameraNear = 1.0f, cameraFar = 100000.0f;
    float cameraFovY = 1.0f;                         // radians
    float viewSpaceToMeters = 1.0f;
    Image* depth = nullptr;                          // D3D convention 0 (near) .. 1 (far), render size
    Image* motion = nullptr;
    float position[3]{}, up[3]{0, 1, 0}, right[3]{1, 0, 0}, forward[3]{0, 0, 1};  // world space (required by the ML provider)
};

// ffxDispatch(frame generation): the interpolated frame between the previous colour image and this one
struct FrameGenDispatch {
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    std::uint64_t frameId = 0;
    Image* color = nullptr;   // this frame's final image (display size, the back buffer format)
    Image* output = nullptr;  // the generated frame (storage image)
    bool reset = false;
    VkExtent2D rect{0, 0};    // 0 = the display size
    std::uint32_t debugFlags = 0;
};

class FrameGen {
public:
    FrameGen() = default;
    FrameGen(const FrameGen&) = delete;
    FrameGen& operator=(const FrameGen&) = delete;
    ~FrameGen() { destroy(); }

    // loads `dll` (full path of amd_fidelityfx_loader_vk.dll) and binds the entry points; false (logged) when it is missing or
    // not an FFX API DLL. The effect DLL (amd_fidelityfx_framegeneration_vk.dll) is looked up next to it by the loader.
    bool load(const std::filesystem::path& dll);
    bool loaded() const { return module_ != nullptr; }

    // the providers the API offers for frame generation / for the frame generation swapchain on this device (empty without a
    // module, or when the effect DLL is missing)
    std::vector<FrameGenVersion> versionList(Context& ctx);
    std::vector<FrameGenVersion> swapchainVersionList(Context& ctx);

    bool createContext(Context& ctx, const FrameGenCreateInfo& info);
    bool hasContext() const { return context_ != nullptr; }
    // the GPU must be done with the context's dispatches
    void destroyContext();
    void destroy();  // context + module

    // callback model: configure the swapchain without frame generation and without our dispatch callback (the context is about to go)
    bool detachFromSwapchain();

    // the three calls of one frame, recorded into `cmd` (prepare, dispatch) in this order; the images' tracked layouts are set to
    // the states the DLL leaves them in
    bool configure(const FrameGenConfigure& c);
    bool prepare(const FrameGenPrepare& p);
    bool dispatch(const FrameGenDispatch& d);

    // the provider behind the context ("3.1.6" / "4.x"), empty without a context
    const std::string& providerName() const { return provider_; }
    bool swapchainModel() const { return info_.swapchain; }
    // GPU memory of the context (ffxQuery on the context), 0 when unknown
    std::uint64_t memoryBytes();

private:
    static long frameGenCallback(void* params, void* user);
    long onFrameGeneration(void* params);

    void* module_ = nullptr;   // HMODULE
    void* context_ = nullptr;  // ffxContext
    std::atomic<std::uint64_t> resetFrame_{0};  // the frame whose interpolation resets (callback model), 0 = none
    VkSwapchainKHR cfgSwapchain_ = VK_NULL_HANDLE;  // callback model: what the last configure attached (destroyContext detaches it)
    const void* cfgPresent_ = nullptr;
    void* cfgPresentUser_ = nullptr;
    struct Functions;
    Functions* fn_ = nullptr;
    FrameGenCreateInfo info_{};
    std::string provider_;
};

}  // namespace rvk::vk
