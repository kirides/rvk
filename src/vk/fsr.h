#pragma once

// FidelityFX Super Resolution 3.1 through AMD's signed FFX API DLL (third_party/amd_fidelityfx: amd_fidelityfx_vk.dll, loaded
// at run time from bin\rvk). The DLL's provider is FSR 3.1 on every GPU; a driver with FSR 4 support (RDNA4) swaps it in behind
// the same calls. The wrapper owns the module, one upscale context and the Vulkan resource descriptions of a dispatch.
//
// Resource states: the DLL's Vulkan backend transitions each resource from the state given in the dispatch (COMPUTE_READ =
// SHADER_READ_ONLY_OPTIMAL, UNORDERED_ACCESS = GENERAL) and leaves it there, so the images' tracked layouts are set to those.
// Depth must be a 32-bit float / depth image; colour, motion and output must have the usage bits the effect needs (inputs
// SAMPLED, output STORAGE).

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

#include "vk/image.h"
#include "vk/vk.h"

namespace rvk::vk {

class Context;

// one upscaler provider the FFX API offers (ffxQueryDescGetVersions)
struct FsrVersion {
    std::uint64_t id = 0;
    std::string name;       // "4.1.2", "3.1.6 *" (a trailing " *" = provided by the display driver)
    int major = 0, minor = 0, patch = 0;  // parsed from the first digits of the name (0 = none)
    bool fromDriver = false;
};

struct FsrCreateInfo {
    bool ml = false;                   // the ML upscaler (FSR 4.x); false = the newest FSR 3.x
    VkExtent2D maxRenderSize{1, 1};
    VkExtent2D maxUpscaleSize{1, 1};
    bool hdr = false;                  // the colour input is linear HDR (else display-referred)
    bool invertedDepth = false;        // depth runs 1 (near) .. 0 (far)
    bool infiniteDepth = false;
    bool autoExposure = true;          // no exposure texture: FSR derives it
    bool motionJitterCancelled = false;  // the motion vectors carry the jitter (rare; false = they do not)
    bool debugChecking = false;        // the DLL validates its inputs and reports through the log
    bool nonLinearColor = false;       // the colour input is gamma encoded (sRGB)
};

struct FsrDispatch {
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    Image* color = nullptr;
    Image* depth = nullptr;
    Image* motion = nullptr;     // previous - current position in pixels of the render size (scaled by motionScale)
    Image* exposure = nullptr;   // optional 1x1 R32F: the scene exposure (colour x exposure = what is displayed); only read by a context made without autoExposure
    Image* reactive = nullptr;   // optional
    Image* transparency = nullptr;  // optional
    Image* output = nullptr;
    float jitterX = 0.0f, jitterY = 0.0f;  // this frame's camera jitter in render pixels (FSR's own sequence)
    float motionScaleX = 1.0f, motionScaleY = 1.0f;
    VkExtent2D renderSize{1, 1};
    VkExtent2D upscaleSize{1, 1};
    float sharpness = 0.0f;      // 0 = off
    float frameTimeMs = 16.6f;
    float preExposure = 1.0f;
    bool reset = false;          // the camera cut: drop the history
    float cameraNear = 1.0f, cameraFar = 100000.0f;
    float cameraFovY = 1.0f;     // radians
    float viewSpaceToMeters = 1.0f;
    bool nonLinearSrgb = false;
};

class Fsr {
public:
    Fsr() = default;
    Fsr(const Fsr&) = delete;
    Fsr& operator=(const Fsr&) = delete;
    ~Fsr() { destroy(); }

    // loads `dll` (full path) and binds the entry points; false (logged) when it is missing or not an FFX API DLL
    // (the loader DLL, amd_fidelityfx_loader_vk.dll; the effect DLL is looked up next to it)
    bool load(const std::filesystem::path& dll);
    bool loaded() const { return module_ != nullptr; }
    bool createContext(Context& ctx, const FsrCreateInfo& info);
    bool hasContext() const { return context_ != nullptr; }
    // destroys the context (the GPU must be done with its dispatches)
    void destroyContext();
    void destroy();  // context + module

    // recorded into `d.cmd`; the images' tracked layouts must be their stable ones (transitions are recorded here)
    bool dispatch(const FsrDispatch& d);
    // FSR's automatic reactive mask (R8 / R16F storage image `out`, left readable): where `color` (opaque + translucent) differs
    // from `opaque` (the opaque scene alone, same size) the temporal history is trusted less
    // (own context, created on first use)
    bool generateReactiveMask(VkCommandBuffer cmd, Image& opaque, Image& color, Image& out, VkExtent2D renderSize, float scale = 1.0f,
                              float cutoff = 0.2f, float binaryValue = 0.9f);

    // the provider behind the context ("3.1.x" / "4.x"), empty without a context
    const std::string& providerName() const { return provider_; }
    // ffxQueryDescUpscaleGetResourceRequirements of the context (FfxApiQueryResourceIdentifiers bitmaps, read at creation): FSR 4
    // needs no reactive / transparency mask, so the work to build them is only done when the provider consumes them
    bool consumesReactiveMask() const { return !requirementsKnown_ || ((requiredResources_ | optionalResources_) & kResourceReactiveMask) != 0; }
    std::uint64_t requiredResources() const { return requiredResources_; }
    std::uint64_t optionalResources() const { return optionalResources_; }
    static constexpr std::uint64_t kResourceReactiveMask = 1u << 4;  // FFX_API_QUERY_RESOURCE_INPUT_REACTIVEMASK
    // the jitter sequence for a render / display width (FSR 3.1: 8 * (display / render)^2 phases), and its offset at `index`
    int jitterPhaseCount(std::uint32_t renderWidth, std::uint32_t displayWidth);
    bool jitterOffset(int index, int phaseCount, float& x, float& y);
    // the render size for a quality mode (FfxApiUpscaleQualityMode: 0 = native AA .. 4 = ultra performance)
    bool renderSizeForMode(VkExtent2D display, std::uint32_t mode, VkExtent2D& render);
    // every provider version the API offers for the upscaler on this device (empty without a module)
    std::vector<FsrVersion> versionList(Context& ctx);
    // the same as one line, "id: name, ..."
    std::string versions(Context& ctx);
    // the newest version of the family (ml: major >= 4, else major 3), nullptr when there is none
    static const FsrVersion* pick(const std::vector<FsrVersion>& list, bool ml);

private:
    void* module_ = nullptr;  // HMODULE
    void* context_ = nullptr;  // ffxContext
    // The DLL's Vulkan backend advances a ring of 4 dynamic resource view slots per ffxDispatch and destroys the views of the
    // slot it enters: two dispatches per frame on one context (reactive mask + upscale) cycle it faster than our frames in flight
    // finish (validation: vkDestroyImageView in use). The reactive mask runs on its own, tiny context (a dispatch per frame each).
    void* reactiveContext_ = nullptr;
    std::uint64_t reactiveVersion_ = 0;  // the FSR 3 version id the reactive mask context is pinned to
    Context* ctx_ = nullptr;
    FsrCreateInfo info_{};
    struct Functions;
    Functions* fn_ = nullptr;
    std::string provider_;
    bool requirementsKnown_ = false;  // the DLL answered the resource requirements query (else: assume every mask is used)
    std::uint64_t requiredResources_ = 0, optionalResources_ = 0;
};

}  // namespace rvk::vk
