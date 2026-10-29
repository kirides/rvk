#pragma once

// Vulkan instance + device for the renderer (docs/PLAN.md "Vulkan floor": 1.3 core + VK_KHR_push_descriptor).

#include <atomic>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <vector>

#include "core/caps.h"
#include "vk/debug_names.h"
#include "vk/probe.h"
#include "vk/vk.h"

namespace rvk::vk {

struct ContextOptions {
    bool validation = false;  // VK_LAYER_KHRONOS_validation + debug messenger
    int adapter = -1;         // physical device index, -1 = best discrete GPU
    const char* appName = "Risen";
    bool syncValidation = false;  // the validation layer's synchronization validation (VkValidationFeaturesEXT)
    // enable the ray tracing tier (acceleration structures + ray query + optional position fetch / opacity micromaps) when
    // the device has it (player setting rt.allow, read at device creation; docs/research/raytracing.md)
    bool allowRayTracing = true;
    // VK_KHR_pipeline_executable_properties when the driver has it (shader statistics: rvk_vktest --shader-stats)
    bool pipelineStatistics = false;
    // request the extra queues AMD's frame generation swapchain needs (docs/research/frame-generation.md 2.1: four distinct
    // VkQueues): only when frame generation may be used (framegen.mode != off at startup); with false the device has the one
    // queue of the floor, exactly as before
    bool frameGenQueues = false;
};

// The extra queues of the frame generation swapchain (stage F3). `present` and `acquire` are distinct queues of the game queue's
// family (present: also transfer; acquire: any), `async` a compute queue (a compute-only family, else another queue of the game
// family; null = none, the swapchain's async compute is optional). The game queue is Context::queue(). Access to the extra
// queues is the swapchain's (the FFX DLL submits and presents on them from its own threads).
struct FrameGenQueues {
    bool requested = false;   // ContextOptions::frameGenQueues
    bool created = false;     // the queues exist
    VkQueue present = VK_NULL_HANDLE, acquire = VK_NULL_HANDLE, async = VK_NULL_HANDLE;
    std::uint32_t presentFamily = 0, acquireFamily = 0, asyncFamily = 0;
    std::string reason;       // why not created (when requested or not)
};

// What the frame generation setup found (enh / hal side), reported with the capabilities (caps: lines, the System tab)
struct FrameGenReport {
    bool probed = false;      // the effect DLL was queried
    bool dllPresent = false;  // amd_fidelityfx_framegeneration_vk.dll sits next to the loader
    bool fsr3 = false;        // the API offers an FSR 3 frame generation provider on this device
    bool ml = false;          // ... an FSR 4 (ML) provider
    std::string versions;     // "id: name, ..." of the frame generation providers
    std::string swapchain;    // the swapchain providers and the one that would be pinned
    std::string reason;       // why frame generation is unavailable (empty when fsr3)
};

// The optional ray tracing tier of the device (docs/research/raytracing.md 1).
struct RayTracingFeatures {
    bool available = false;  // the device offers the tier (acceleration structures + ray query + deferred host operations)
    bool enabled = false;    // enabled on this device: buffers get the BLAS-input usage, ray query shaders may be created
    bool positionFetch = false;    // VK_KHR_ray_tracing_position_fetch enabled
    bool opacityMicromap = false;  // VK_EXT_opacity_micromap enabled
    bool maintenance1 = false;     // VK_KHR_ray_tracing_maintenance1 enabled
    std::string reason;            // why not enabled (empty when enabled)
    std::uint32_t minScratchOffsetAlignment = 0;  // acceleration structure build scratch alignment
    std::uint64_t maxGeometryCount = 0, maxInstanceCount = 0, maxPrimitiveCount = 0;
};

// Optional device features that were found and enabled.
struct DeviceFeatures {
    bool extendedDynamicState3 = false;     // blend enable/equation/write mask + polygon mode as dynamic state
    bool vertexInputDynamicState = false;   // vertex input as dynamic state
    bool graphicsPipelineLibrary = false;   // VK_EXT_graphics_pipeline_library: shaders compiled once, pipelines linked fast
    bool robustness2NullDescriptor = false;
    bool memoryBudget = false;
    bool deviceFault = false;
    bool bufferMarker = false;    // VK_AMD_buffer_marker enabled (debug.crash_markers: vk/crash_markers.h)
    bool samplerAnisotropy = false;
    bool shaderFloat16Int16 = false;  // shaderFloat16 + shaderInt16 enabled (the FidelityFX upscaler's fp16 shaders)
    bool storageImageExtendedFormats = false;  // RG16F / R16F ... storage images (the G-buffer's motion)
    bool indirectDraws = false;   // multiDrawIndirect + drawIndirectFirstInstance: one vkCmdDrawIndexedIndirect for many draws, the item index as firstInstance (no user at the moment: the removed native opaque prototype used it, see git history)
    bool vertexStores = false;    // vertexPipelineStoresAndAtomics: vertex shaders write buffers (enh/gi: skinned positions)
    bool shaderDrawParameters = false;  // gl_DrawID / gl_BaseInstance in shaders (Vulkan 1.1 feature, optional in 1.3)
    bool drawIndirectCount = false;     // vkCmdDraw*IndirectCount (Vulkan 1.2 feature): a GPU-written draw count
    float maxAnisotropy = 1.0f;
    bool hostQueryReset = false;  // vkResetQueryPool from the host (GPU timestamps of [debug] perf)
    float timestampPeriod = 0.0f; // ns per timestamp tick; 0 = the queue cannot write timestamps
    bool pipelineStatistics = false;  // VK_KHR_pipeline_executable_properties enabled (ContextOptions::pipelineStatistics)
    bool fullScreenExclusive = false; // VK_EXT_full_screen_exclusive (+ instance VK_KHR_get_surface_capabilities2) enabled
    RayTracingFeatures rayTracing;
};

class Context {
public:
    Context() = default;
    Context(const Context&) = delete;
    Context& operator=(const Context&) = delete;
    ~Context();

    // false (and logged) when Vulkan or a device meeting the floor is not available
    bool create(const ContextOptions& options);
    void destroy();

    VkInstance instance() const { return instance_; }
    VkPhysicalDevice physicalDevice() const { return physical_; }
    VkDevice device() const { return device_; }
    VkQueue queue() const { return queue_; }
    // The queue is used by the render thread (submits) and the present thread (vkQueuePresentKHR): every queue operation,
    // and vkDeviceWaitIdle (it accesses all queues), goes through these, which hold the queue lock.
    VkResult queueSubmit(const VkSubmitInfo2& submit, VkFence fence = VK_NULL_HANDLE);
    VkResult queuePresent(const VkPresentInfoKHR& present);
    void waitIdle();
    // the queue lock itself, for queue access by foreign code (ImGui's texture uploads submit and wait on the queue)
    std::unique_lock<std::mutex> queueLock() { return std::unique_lock<std::mutex>(queueMutex_); }
    std::uint32_t queueFamily() const { return queueFamily_; }
    const FrameGenQueues& frameGenQueues() const { return fgQueues_; }
    // set by the frame generation setup before capabilities() is built
    void setFrameGenReport(FrameGenReport report) { fgReport_ = std::move(report); }
    const FrameGenReport& frameGenReport() const { return fgReport_; }
    VmaAllocator allocator() const { return allocator_; }
    const VkPhysicalDeviceProperties& properties() const { return props_; }
    const DeviceFeatures& features() const { return features_; }
    const std::string& deviceName() const { return deviceName_; }
    // VK_EXT_debug_utils is enabled on the instance (object names, command buffer labels)
    bool debugUtilsAvailable() const { return debugUtils_; }
    // the capability report of this device (core/caps.h): GPU, driver, optional features available / enabled + reasons.
    // `pipelineLibrariesInUse`: [vulkan] pipeline_libraries (the device enables the extension; the pipeline cache decides)
    caps::Report capabilities(bool pipelineLibrariesInUse) const;

    // validation messages of severity error seen so far (tests assert 0)
    std::uint32_t validationErrors() const { return validationErrors_.load(); }
    void noteValidationError() { ++validationErrors_; }
    // The first VK_ERROR_DEVICE_LOST of a queue operation: logs VK_EXT_device_fault's report (description, faulting addresses,
    // vendor infos; the vendor binary goes to bin\rvk\devicefault.bin) once; later calls do nothing. `where` names the operation.
    void reportDeviceLost(const char* where);
    // called by reportDeviceLost after the fault report: its text is logged too (the crash markers' unfinished regions)
    void setLostReporter(std::function<std::string()> fn) { lostReporter_ = std::move(fn); }

    // debug names (no-op without VK_EXT_debug_utils or with `debug.names` off: vk/debug_names.h)
    bool namesEnabled() const;
    void setName(VkObjectType type, std::uint64_t handle, const char* name) const;
    template <class T>
    void setName(T handle, VkObjectType type, const char* name) const {
        setName(type, reinterpret_cast<std::uint64_t>(handle), name);
    }

private:
    bool createInstance(const ContextOptions& options);
    bool pickPhysicalDevice(const ContextOptions& options);
    bool createDevice(bool withRayTracing);
    void planFrameGenQueues(const std::vector<VkQueueFamilyProperties>& families);
    bool wantPipelineStatistics_ = false;
    bool createAllocator();

    VkInstance instance_ = VK_NULL_HANDLE;
    VkDebugUtilsMessengerEXT messenger_ = VK_NULL_HANDLE;
    VkPhysicalDevice physical_ = VK_NULL_HANDLE;
    VkDevice device_ = VK_NULL_HANDLE;
    VkQueue queue_ = VK_NULL_HANDLE;
    std::uint32_t queueFamily_ = 0;
    std::uint32_t queueFamilyCount_ = 1;       // queues the game queue's family offers
    std::uint32_t computeFamily_ = ~0u;        // a compute-only family (no graphics), ~0 = none
    std::uint32_t computeFamilyCount_ = 0;
    bool wantFrameGenQueues_ = false;
    FrameGenQueues fgQueues_;
    FrameGenReport fgReport_;
    VmaAllocator allocator_ = VK_NULL_HANDLE;
    VkPhysicalDeviceProperties props_{};
    DeviceFeatures features_;
    std::string deviceName_;
    DeviceReport report_;  // the probe of the chosen device (availability, driver, ray tracing limits)
    bool debugUtils_ = false;
    bool surfaceCaps2_ = false;              // instance: VK_KHR_get_surface_capabilities2 enabled
    bool fullScreenExclusiveDevice_ = false; // the device offers VK_EXT_full_screen_exclusive
    std::mutex queueMutex_;
    std::atomic<std::uint32_t> validationErrors_{0};
    std::atomic<bool> lostReported_{false};
    std::function<std::string()> lostReporter_;
};

// VkResult -> "VK_ERROR_DEVICE_LOST" etc.
const char* resultString(VkResult r);

}  // namespace rvk::vk
