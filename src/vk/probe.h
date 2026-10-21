#pragma once

#include <cstdint>
#include <string>
#include <vector>

struct VkPhysicalDevice_T;  // = VkPhysicalDevice (global, as in vulkan_core.h)

namespace rvk::vk {

// What a physical device offers, measured against the renderer's floor (docs/PLAN.md "Vulkan floor").
struct DeviceReport {
    std::string name;
    std::uint32_t apiVersion = 0;
    std::uint32_t driverVersion = 0;
    std::uint32_t vendorId = 0;
    bool discrete = false;

    // required
    bool api13 = false;
    bool dynamicRendering = false;
    bool synchronization2 = false;
    bool extendedDynamicState = false;   // 1.3 core feature set (EDS1/EDS2 are core, checked anyway)
    bool pushDescriptor = false;         // VK_KHR_push_descriptor
    bool descriptorIndexing = false;     // sampled image array non-uniform + update-after-bind + partially bound + runtime array
    bool timelineSemaphore = false;
    bool textureCompressionBC = false;
    bool swapchain = false;

    // optional
    bool extendedDynamicState3 = false;
    bool vertexInputDynamicState = false;
    bool graphicsPipelineLibrary = false;
    bool deviceFault = false;
    bool memoryBudget = false;
    bool robustness2 = false;

    // ray tracing (optional tier, docs/research/raytracing.md): extension present AND its feature bit set
    struct RayTracing {
        bool accelerationStructure = false;   // VK_KHR_acceleration_structure + accelerationStructure
        bool rayQuery = false;                // VK_KHR_ray_query + rayQuery
        bool deferredHostOperations = false;  // VK_KHR_deferred_host_operations (required by acceleration_structure)
        bool positionFetch = false;           // VK_KHR_ray_tracing_position_fetch + rayTracingPositionFetch
        bool opacityMicromap = false;         // VK_EXT_opacity_micromap + micromap
        bool pipeline = false;                // VK_KHR_ray_tracing_pipeline + rayTracingPipeline (information only)
        bool maintenance1 = false;            // VK_KHR_ray_tracing_maintenance1 + rayTracingMaintenance1
        bool bufferDeviceAddress = false;     // Vulkan 1.2 bufferDeviceAddress (the renderer enables it anyway)
        std::vector<std::string> extensionsMissing;  // of the three the tier needs (AS, ray query, deferred host operations)
        std::vector<std::string> featuresMissing;    // extension present, feature bit false
        // VkPhysicalDeviceAccelerationStructurePropertiesKHR (valid when the extension exists)
        std::uint64_t maxGeometryCount = 0;
        std::uint64_t maxInstanceCount = 0;
        std::uint64_t maxPrimitiveCount = 0;
        std::uint32_t maxPerStageDescriptorAccelerationStructures = 0;
        std::uint32_t maxDescriptorSetAccelerationStructures = 0;
        std::uint32_t minScratchOffsetAlignment = 0;  // minAccelerationStructureScratchOffsetAlignment
        // VkPhysicalDeviceRayTracingPipelinePropertiesKHR (valid when that extension exists; information)
        std::uint32_t maxRayRecursionDepth = 0;
        std::uint32_t shaderGroupHandleSize = 0;
        // the tier the renderer uses: acceleration structures + ray query + deferred host operations
        bool tier() const { return accelerationStructure && rayQuery && deferredHostOperations && bufferDeviceAddress; }
    } rt;

    std::string driverName;  // VkPhysicalDeviceVulkan12Properties::driverName / driverInfo
    std::string driverInfo;

    std::vector<std::string> missing;    // names of missing required items (empty = usable)
    bool usable() const { return missing.empty(); }
};

struct ProbeResult {
    bool loaderFound = false;
    std::uint32_t instanceVersion = 0;
    std::vector<DeviceReport> devices;
    int best = -1;  // index into devices of the preferred usable device, -1 = none
};

// Inspects one physical device of an instance whose functions volk has loaded.
DeviceReport inspectDevice(::VkPhysicalDevice_T* physicalDevice);

// Loads the Vulkan loader (volk), creates a throw-away instance, inspects every physical device, logs the report.
// Must not run under the loader lock (LoadLibrary).
ProbeResult probe();

std::string versionString(std::uint32_t apiVersion);

}  // namespace rvk::vk
