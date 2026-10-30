#pragma once

// The optional modules of the game device (docs/research/dx12-game-port.md WP2b): what only some backends / some GPUs offer and what lives in AMD's DLLs.
// Each is an interface over the vendor's API in backend neutral terms (rhi::Texture, rhi::CommandList); the Vulkan implementations wrap vk::Fsr, vk::FrameGen,
// vk::FgSwapchain and the ImGui Vulkan backend (rhi/vulkan/modules.cpp, rhi/vulkan/imgui_backend.cpp). Made by GameDevice::createUpscaler() /
// createFrameGenerator() / createImGuiBackend(); the frame generation swapchain (PresentWrapper) belongs to the presentation and is handed out by the device
// that owns the swapchain.
//
// Threading: like the vendor APIs they wrap: the render thread, except where noted (the present callback of the PresentWrapper runs on AMD's threads).

#include <array>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "rhi/rhi.h"

struct ImDrawData;
struct ImTextureData;

namespace rvk::rhi {

// ---- upscaler (FidelityFX Super Resolution) --------------------------------------------------------------------------------------

// one upscaler provider the API offers
struct UpscalerVersion {
    std::uint64_t id = 0;
    std::string name;       // "4.1.2", "3.1.6 *" (a trailing " *" = provided by the display driver)
    int major = 0, minor = 0, patch = 0;
    bool fromDriver = false;
};

struct UpscalerCreateInfo {
    bool ml = false;                   // the ML upscaler (FSR 4.x); false = the newest FSR 3.x
    VkExtent2D maxRenderSize{1, 1};
    VkExtent2D maxUpscaleSize{1, 1};
    bool hdr = false;                  // the colour input is linear HDR (else display-referred)
    bool invertedDepth = false;        // depth runs 1 (near) .. 0 (far)
    bool infiniteDepth = false;
    bool autoExposure = true;          // no exposure texture: the upscaler derives it
    bool motionJitterCancelled = false;
    bool debugChecking = false;
    bool nonLinearColor = false;
};

// One upscale (Upscaler::dispatch). The textures keep the layouts the vendor leaves them in (their tracked layouts are updated).
struct UpscalerDispatch {
    CommandList* cmd = nullptr;
    Texture* color = nullptr;
    Texture* depth = nullptr;
    Texture* motion = nullptr;      // previous - current position in pixels of the render size (scaled by motionScale)
    Texture* exposure = nullptr;    // optional 1x1 R32F: the scene exposure (only read by a context made without autoExposure)
    Texture* reactive = nullptr;    // optional
    Texture* transparency = nullptr;  // optional
    Texture* output = nullptr;
    float jitterX = 0.0f, jitterY = 0.0f;  // this frame's camera jitter in render pixels
    float motionScaleX = 1.0f, motionScaleY = 1.0f;
    VkExtent2D renderSize{1, 1};
    VkExtent2D upscaleSize{1, 1};
    float sharpness = 0.0f;         // 0 = off
    float frameTimeMs = 16.6f;
    float preExposure = 1.0f;
    bool reset = false;             // the camera cut: drop the history
    float cameraNear = 1.0f, cameraFar = 100000.0f;
    float cameraFovY = 1.0f;        // radians
    float viewSpaceToMeters = 1.0f;
    bool nonLinearSrgb = false;
};

class Upscaler {
public:
    virtual ~Upscaler() = default;
    // loads the vendor's loader DLL (full path) and binds its entry points; false (logged) when it is missing / not usable on this backend
    virtual bool load(const std::filesystem::path& dll) = 0;
    virtual bool loaded() const = 0;
    virtual bool createContext(const UpscalerCreateInfo& info) = 0;
    virtual bool hasContext() const = 0;
    // the GPU must be done with the context's dispatches (retire the whole object through GameDevice::retire)
    virtual void destroyContext() = 0;
    virtual void destroy() = 0;
    virtual bool dispatch(const UpscalerDispatch& d) = 0;
    // the automatic reactive mask: where `color` (opaque + translucent) differs from `opaque` (the opaque scene alone) the temporal history is trusted
    // less; `out` is a storage image (R8 / R16F), left readable
    virtual bool generateReactiveMask(CommandList& cmd, Texture& opaque, Texture& color, Texture& out, VkExtent2D renderSize, float scale = 1.0f,
                                      float cutoff = 0.2f, float binaryValue = 0.9f) = 0;
    virtual const std::string& providerName() const = 0;
    // whether the provider uses a reactive / transparency mask (the resource requirements query); true while unknown
    virtual bool consumesReactiveMask() const = 0;
    virtual int jitterPhaseCount(std::uint32_t renderWidth, std::uint32_t displayWidth) = 0;
    virtual bool jitterOffset(int index, int phaseCount, float& x, float& y) = 0;
    virtual bool renderSizeForMode(VkExtent2D display, std::uint32_t mode, VkExtent2D& render) = 0;
    virtual std::vector<UpscalerVersion> versionList() = 0;
    virtual std::string versions() = 0;
};

// ---- frame generation -----------------------------------------------------------------------------------------------------------

class PresentWrapper;

struct FrameGeneratorCreateInfo {
    bool ml = false;                       // the ML provider (FSR 4); false = the newest FSR 3.x
    VkExtent2D displaySize{1, 1};          // rendered and generated frames
    VkExtent2D maxRenderSize{1, 1};        // depth / motion vector size (<= the display size)
    VkFormat backBufferFormat = VK_FORMAT_B8G8R8A8_UNORM;  // the colour the interpolation reads and writes
    VkFormat hudlessFormat = VK_FORMAT_UNDEFINED;          // != UNDEFINED: a HUDLessColor image of this format is passed to configure
    bool hdr = false;
    bool depthInverted = false;
    bool depthInfinite = false;
    bool asyncCompute = false;
    bool debugChecking = false;
    bool swapchain = false;                // the callback model: the PresentWrapper dispatches the interpolation inside its present
};

// FrameGenerator::configure (per frame)
struct FrameGeneratorConfigure {
    bool enabled = true;
    Texture* hudless = nullptr;      // optional, the frame without the GUI
    std::uint64_t frameId = 0;
    VkExtent2D rect{0, 0};           // the area of the colour image that is interpolated (0 = the display size)
    std::uint32_t debugFlags = 0;
    PresentWrapper* present = nullptr;  // the callback model: the swapchain whose present callback / handle the context is configured with
    bool reset = false;              // the interpolation of this frame resets (a camera cut)
};

// FrameGenerator::prepare: the frame's depth + motion vectors + camera
struct FrameGeneratorPrepare {
    CommandList* cmd = nullptr;
    std::uint64_t frameId = 0;
    VkExtent2D renderSize{1, 1};
    float jitterX = 0.0f, jitterY = 0.0f;
    float motionScaleX = 1.0f, motionScaleY = 1.0f;
    float frameTimeMs = 16.6f;
    bool reset = false;
    float cameraNear = 1.0f, cameraFar = 100000.0f;
    float cameraFovY = 1.0f;
    float viewSpaceToMeters = 1.0f;
    Texture* depth = nullptr;        // D3D convention 0 (near) .. 1 (far), render size
    Texture* motion = nullptr;
    float position[3]{}, up[3]{0, 1, 0}, right[3]{1, 0, 0}, forward[3]{0, 0, 1};
};

// FrameGenerator::dispatch: the interpolated frame between the previous colour image and this one (manual model)
struct FrameGeneratorDispatch {
    CommandList* cmd = nullptr;
    std::uint64_t frameId = 0;
    Texture* color = nullptr;        // this frame's final image (display size, the back buffer format)
    Texture* output = nullptr;       // the generated frame (storage image)
    bool reset = false;
    VkExtent2D rect{0, 0};
    std::uint32_t debugFlags = 0;
};

class FrameGenerator {
public:
    virtual ~FrameGenerator() = default;
    virtual bool load(const std::filesystem::path& dll) = 0;
    virtual bool loaded() const = 0;
    virtual bool createContext(const FrameGeneratorCreateInfo& info) = 0;
    virtual bool hasContext() const = 0;
    virtual void destroyContext() = 0;
    virtual void destroy() = 0;  // context + module
    // the callback model: configure the swapchain without frame generation and without our dispatch callback (the context is about to go)
    virtual bool detachFromSwapchain() = 0;
    // the calls of one frame, prepare and dispatch recorded into their command list in this order
    virtual bool configure(const FrameGeneratorConfigure& c) = 0;
    virtual bool prepare(const FrameGeneratorPrepare& p) = 0;
    virtual bool dispatch(const FrameGeneratorDispatch& d) = 0;
    virtual const std::string& providerName() const = 0;
    virtual bool swapchainModel() const = 0;
    virtual std::uint64_t memoryBytes() = 0;
};

// What draws on top of the presented frame (the overlay): called by the swapchain's present callback on AMD's threads with the frame already in the
// image the commands record into (a rendering scope of one colour attachment of `extent` is open: the same contract as PresentOverlay::record)
struct PresentUi {
    std::function<bool()> wants;  // anything to draw? false skips the render pass
    std::function<void(CommandList& cmd, VkExtent2D extent)> record;
};

struct PresentSample {
    double seconds = 0.0;      // QPC time the real present counter was seen to change
    std::uint64_t count = 0;   // ... its new value
};

// AMD's frame generation swapchain as the presentation of the game device (it replaces the device's own swapchain while frame generation is wanted): the
// interpolation runs inside its present, it composes the UI through the present callback and paces the generated and the real frame.
class PresentWrapper {
public:
    virtual ~PresentWrapper() = default;
    virtual const std::string& providerName() const = 0;
    // presents of the real swapchain since creation (generated frames included)
    virtual std::uint64_t realPresentCount() const = 0;
    // waits until every queued present was made (GPU work included); idempotent
    virtual void waitForPresents() = 0;
    // covers every configure / prepare / create / destroy of the generator (render thread) and the present (the interpolation dispatch runs inside it)
    virtual std::mutex& contextMutex() = 0;
    virtual void setUi(PresentUi ui) = 0;
    struct Counters {
        std::uint64_t callbacks = 0;        // present callback calls (real + generated frames)
        std::uint64_t generatedFrames = 0;  // ... of them generated
        std::uint64_t uiFrames = 0;         // ... with a UI render
        double callbackMs = 0.0;            // summed CPU time in the callback
    };
    virtual Counters takeCounters() = 0;
    // the real presents' times: a spinning thread watches the counter (diagnostics only: [debug] perf, a core's worth of CPU)
    virtual void startSampler() = 0;
    virtual void stopSampler() = 0;
    virtual std::vector<PresentSample> takeSamples() = 0;
};

// Statistics of the real presents' gaps (ms; the sampler's output): gaps over `stallMs` count as stalls and are kept out of the moments; even / odd =
// every other gap (generated -> real and real -> generated). Pure.
struct PresentGapStats {
    std::size_t n = 0, stalls = 0;
    double mean = 0, stddev = 0, p1 = 0, p50 = 0, p99 = 0, max = 0;
    double evenMean = 0, evenStd = 0, oddMean = 0, oddStd = 0;
};
PresentGapStats presentGapStats(const std::vector<double>& gapsMs, double stallMs = 100.0);

// ---- the overlay's UI renderer ----------------------------------------------------------------------------------------------------

struct ImGuiBackendDesc {
    VkFormat colorFormat = VK_FORMAT_B8G8R8A8_UNORM;  // the attachment the UI is drawn into
    std::uint32_t minImageCount = 2;                  // the swapchain's images (frames the UI resources are buffered for)
    std::uint32_t imageCount = 2;
};

// Dear ImGui's renderer backend of the game device (Vulkan: imgui_impl_vulkan over the renderer's device and queue). The overlay owns the ImGui context and
// the platform backend; this draws ImDrawData.
class ImGuiBackend {
public:
    virtual ~ImGuiBackend() = default;
    virtual bool init(const ImGuiBackendDesc& desc) = 0;
    virtual void shutdown() = 0;
    virtual void newFrame() = 0;
    virtual void updateTexture(ImTextureData* texture) = 0;
    // records the draw lists into `cmd`, inside an open rendering scope of one colour attachment (viewport / scissor / pipeline are the backend's own)
    virtual void renderDrawData(ImDrawData* data, CommandList& cmd) = 0;
};

}  // namespace rvk::rhi
