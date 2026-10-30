// The optional modules of the game device (rhi/modules.h) on Vulkan: thin adapters from the backend neutral interfaces to the vendor DLL wrappers
// (vk/fsr.h, vk/framegen.h, vk/fg_swapchain.h). The wrappers keep their code; textures are the Vulkan images behind the RHI textures (their tracked
// layouts are the ones the wrappers update), command lists are the VkCommandBuffer the DLLs record into.

#include <spdlog/spdlog.h>

#include "rhi/modules.h"
#include "rhi/vulkan/wrap.h"
#include "vk/context.h"
#include "vk/fg_swapchain.h"
#include "vk/framegen.h"
#include "vk/fsr.h"
#include "vk/image.h"

namespace rvk::rhi::vulkan {
namespace {

vk::Image* imageOrNull(Texture* t) { return t != nullptr ? &imageOf(*t) : nullptr; }

// ---- FSR ------------------------------------------------------------------------------------------------------------------------

class FsrUpscaler final : public Upscaler {
public:
    explicit FsrUpscaler(GameDevice& device) : ctx_(contextOf(device)) {}

    bool load(const std::filesystem::path& dll) override { return fsr_.load(dll); }
    bool loaded() const override { return fsr_.loaded(); }
    bool createContext(const UpscalerCreateInfo& info) override {
        vk::FsrCreateInfo ci;
        ci.ml = info.ml;
        ci.maxRenderSize = info.maxRenderSize;
        ci.maxUpscaleSize = info.maxUpscaleSize;
        ci.hdr = info.hdr;
        ci.invertedDepth = info.invertedDepth;
        ci.infiniteDepth = info.infiniteDepth;
        ci.autoExposure = info.autoExposure;
        ci.motionJitterCancelled = info.motionJitterCancelled;
        ci.debugChecking = info.debugChecking;
        ci.nonLinearColor = info.nonLinearColor;
        return fsr_.createContext(ctx_, ci);
    }
    bool hasContext() const override { return fsr_.hasContext(); }
    void destroyContext() override { fsr_.destroyContext(); }
    void destroy() override { fsr_.destroy(); }
    bool dispatch(const UpscalerDispatch& d) override {
        vk::FsrDispatch v;
        v.cmd = commandBufferOf(*d.cmd);
        v.color = imageOrNull(d.color);
        v.depth = imageOrNull(d.depth);
        v.motion = imageOrNull(d.motion);
        v.exposure = imageOrNull(d.exposure);
        v.reactive = imageOrNull(d.reactive);
        v.transparency = imageOrNull(d.transparency);
        v.output = imageOrNull(d.output);
        v.jitterX = d.jitterX;
        v.jitterY = d.jitterY;
        v.motionScaleX = d.motionScaleX;
        v.motionScaleY = d.motionScaleY;
        v.renderSize = d.renderSize;
        v.upscaleSize = d.upscaleSize;
        v.sharpness = d.sharpness;
        v.frameTimeMs = d.frameTimeMs;
        v.preExposure = d.preExposure;
        v.reset = d.reset;
        v.cameraNear = d.cameraNear;
        v.cameraFar = d.cameraFar;
        v.cameraFovY = d.cameraFovY;
        v.viewSpaceToMeters = d.viewSpaceToMeters;
        v.nonLinearSrgb = d.nonLinearSrgb;
        return fsr_.dispatch(v);
    }
    bool generateReactiveMask(CommandList& cmd, Texture& opaque, Texture& color, Texture& out, VkExtent2D renderSize, float scale, float cutoff,
                              float binaryValue) override {
        return fsr_.generateReactiveMask(commandBufferOf(cmd), imageOf(opaque), imageOf(color), imageOf(out), renderSize, scale, cutoff, binaryValue);
    }
    const std::string& providerName() const override { return fsr_.providerName(); }
    bool consumesReactiveMask() const override { return fsr_.consumesReactiveMask(); }
    int jitterPhaseCount(std::uint32_t renderWidth, std::uint32_t displayWidth) override { return fsr_.jitterPhaseCount(renderWidth, displayWidth); }
    bool jitterOffset(int index, int phaseCount, float& x, float& y) override { return fsr_.jitterOffset(index, phaseCount, x, y); }
    bool renderSizeForMode(VkExtent2D display, std::uint32_t mode, VkExtent2D& render) override { return fsr_.renderSizeForMode(display, mode, render); }
    std::vector<UpscalerVersion> versionList() override {
        std::vector<UpscalerVersion> out;
        for (const vk::FsrVersion& v : fsr_.versionList(ctx_)) out.push_back({v.id, v.name, v.major, v.minor, v.patch, v.fromDriver});
        return out;
    }
    std::string versions() override { return fsr_.versions(ctx_); }

private:
    vk::Context& ctx_;
    vk::Fsr fsr_;
};

// ---- frame generation swapchain -------------------------------------------------------------------------------------------------

class FgPresentWrapper final : public PresentWrapper {
public:
    FgPresentWrapper(GameDevice& device, vk::FgSwapchain& swapchain) : device_(device), sc_(swapchain) {}
    vk::FgSwapchain& swapchain() { return sc_; }

    const std::string& providerName() const override { return sc_.providerName(); }
    std::uint64_t realPresentCount() const override { return sc_.realPresentCount(); }
    void waitForPresents() override { sc_.waitForPresents(); }
    std::mutex& contextMutex() override { return sc_.contextMutex(); }
    void setUi(PresentUi ui) override {
        vk::FgUiHook hook;
        if (ui.wants || ui.record) {
            hook.wants = std::move(ui.wants);
            if (ui.record) {
                // on AMD's threads, inside its present callback: a list over the callback's command buffer for the duration of the call
                hook.record = [this, rec = std::move(ui.record)](VkCommandBuffer cmd, VkImageView, VkExtent2D extent) {
                    const std::unique_ptr<CommandList> list = wrapCommandBuffer(device_, cmd);
                    rec(*list, extent);
                };
            }
        }
        sc_.setUi(std::move(hook));
    }
    Counters takeCounters() override {
        const vk::FgSwapchain::Counters c = sc_.takeCounters();
        return {c.callbacks, c.generatedFrames, c.uiFrames, c.callbackMs};
    }
    void startSampler() override { sc_.startSampler(); }
    void stopSampler() override { sc_.stopSampler(); }
    std::vector<PresentSample> takeSamples() override {
        std::vector<PresentSample> out;
        for (const vk::FgPresentSample& s : sc_.takeSamples()) out.push_back({s.seconds, s.count});
        return out;
    }

private:
    GameDevice& device_;
    vk::FgSwapchain& sc_;
};

// ---- frame generator ------------------------------------------------------------------------------------------------------------

class FsrFrameGenerator final : public FrameGenerator {
public:
    explicit FsrFrameGenerator(GameDevice& device) : ctx_(contextOf(device)) {}

    bool load(const std::filesystem::path& dll) override { return fg_.load(dll); }
    bool loaded() const override { return fg_.loaded(); }
    bool createContext(const FrameGeneratorCreateInfo& info) override {
        vk::FrameGenCreateInfo ci;
        ci.ml = info.ml;
        ci.displaySize = info.displaySize;
        ci.maxRenderSize = info.maxRenderSize;
        ci.backBufferFormat = info.backBufferFormat;
        ci.hudlessFormat = info.hudlessFormat;
        ci.hdr = info.hdr;
        ci.depthInverted = info.depthInverted;
        ci.depthInfinite = info.depthInfinite;
        ci.asyncCompute = info.asyncCompute;
        ci.debugChecking = info.debugChecking;
        ci.swapchain = info.swapchain;
        return fg_.createContext(ctx_, ci);
    }
    bool hasContext() const override { return fg_.hasContext(); }
    void destroyContext() override { fg_.destroyContext(); }
    void destroy() override { fg_.destroy(); }
    bool detachFromSwapchain() override { return fg_.detachFromSwapchain(); }
    bool configure(const FrameGeneratorConfigure& c) override {
        vk::FrameGenConfigure v;
        v.enabled = c.enabled;
        v.hudless = imageOrNull(c.hudless);
        v.frameId = c.frameId;
        v.rect = c.rect;
        v.debugFlags = c.debugFlags;
        v.reset = c.reset;
        if (c.present != nullptr) {
            vk::FgSwapchain& sc = static_cast<FgPresentWrapper*>(c.present)->swapchain();
            v.swapchain = sc.handle();
            v.presentCallback = sc.presentCallback();
            v.presentUser = sc.presentUser();
        }
        return fg_.configure(v);
    }
    bool prepare(const FrameGeneratorPrepare& p) override {
        vk::FrameGenPrepare v;
        v.cmd = commandBufferOf(*p.cmd);
        v.frameId = p.frameId;
        v.renderSize = p.renderSize;
        v.jitterX = p.jitterX;
        v.jitterY = p.jitterY;
        v.motionScaleX = p.motionScaleX;
        v.motionScaleY = p.motionScaleY;
        v.frameTimeMs = p.frameTimeMs;
        v.reset = p.reset;
        v.cameraNear = p.cameraNear;
        v.cameraFar = p.cameraFar;
        v.cameraFovY = p.cameraFovY;
        v.viewSpaceToMeters = p.viewSpaceToMeters;
        v.depth = imageOrNull(p.depth);
        v.motion = imageOrNull(p.motion);
        for (int i = 0; i < 3; ++i) {
            v.position[i] = p.position[i];
            v.up[i] = p.up[i];
            v.right[i] = p.right[i];
            v.forward[i] = p.forward[i];
        }
        return fg_.prepare(v);
    }
    bool dispatch(const FrameGeneratorDispatch& d) override {
        vk::FrameGenDispatch v;
        v.cmd = commandBufferOf(*d.cmd);
        v.frameId = d.frameId;
        v.color = imageOrNull(d.color);
        v.output = imageOrNull(d.output);
        v.reset = d.reset;
        v.rect = d.rect;
        v.debugFlags = d.debugFlags;
        return fg_.dispatch(v);
    }
    const std::string& providerName() const override { return fg_.providerName(); }
    bool swapchainModel() const override { return fg_.swapchainModel(); }
    std::uint64_t memoryBytes() override { return fg_.memoryBytes(); }

private:
    vk::Context& ctx_;
    vk::FrameGen fg_;
};

}  // namespace

std::unique_ptr<Upscaler> createUpscaler(GameDevice& device) { return std::make_unique<FsrUpscaler>(device); }
std::unique_ptr<FrameGenerator> createFrameGenerator(GameDevice& device) { return std::make_unique<FsrFrameGenerator>(device); }
std::unique_ptr<PresentWrapper> wrapFgSwapchain(GameDevice& device, vk::FgSwapchain& swapchain) {
    return std::make_unique<FgPresentWrapper>(device, swapchain);
}

}  // namespace rvk::rhi::vulkan

namespace rvk::rhi {

PresentGapStats presentGapStats(const std::vector<double>& gapsMs, double stallMs) {
    const vk::FgGapStats g = vk::fgGapStats(gapsMs, stallMs);
    return {g.n, g.stalls, g.mean, g.stddev, g.p1, g.p50, g.p99, g.max, g.evenMean, g.evenStd, g.oddMean, g.oddStd};
}

}  // namespace rvk::rhi
