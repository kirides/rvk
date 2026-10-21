#pragma once

#include <cstdint>
#include <string>
#include <vector>

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

    std::vector<std::string> missing;    // names of missing required items (empty = usable)
    bool usable() const { return missing.empty(); }
};

struct ProbeResult {
    bool loaderFound = false;
    std::uint32_t instanceVersion = 0;
    std::vector<DeviceReport> devices;
    int best = -1;  // index into devices of the preferred usable device, -1 = none
};

// Loads the Vulkan loader (volk), creates a throw-away instance, inspects every physical device, logs the report.
// Must not run under the loader lock (LoadLibrary).
ProbeResult probe();

std::string versionString(std::uint32_t apiVersion);

}  // namespace rvk::vk
