#include "vk/probe.h"

#include <cstring>
#include <format>

#include <spdlog/spdlog.h>
#include <volk.h>

namespace rvk::vk {
namespace {

bool hasExtension(const std::vector<VkExtensionProperties>& exts, const char* name) {
    for (const auto& e : exts) {
        if (std::strcmp(e.extensionName, name) == 0) {
            return true;
        }
    }
    return false;
}

DeviceReport inspect(VkPhysicalDevice pd) {
    DeviceReport r;
    VkPhysicalDeviceProperties props{};
    vkGetPhysicalDeviceProperties(pd, &props);
    r.name = props.deviceName;
    r.apiVersion = props.apiVersion;
    r.driverVersion = props.driverVersion;
    r.vendorId = props.vendorID;
    r.discrete = props.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU;
    r.api13 = props.apiVersion >= VK_API_VERSION_1_3;

    std::uint32_t n = 0;
    vkEnumerateDeviceExtensionProperties(pd, nullptr, &n, nullptr);
    std::vector<VkExtensionProperties> exts(n);
    vkEnumerateDeviceExtensionProperties(pd, nullptr, &n, exts.data());

    r.swapchain = hasExtension(exts, VK_KHR_SWAPCHAIN_EXTENSION_NAME);
    r.pushDescriptor = hasExtension(exts, VK_KHR_PUSH_DESCRIPTOR_EXTENSION_NAME) || props.apiVersion >= VK_API_VERSION_1_4;
    r.extendedDynamicState3 = hasExtension(exts, VK_EXT_EXTENDED_DYNAMIC_STATE_3_EXTENSION_NAME);
    r.vertexInputDynamicState = hasExtension(exts, VK_EXT_VERTEX_INPUT_DYNAMIC_STATE_EXTENSION_NAME);
    r.graphicsPipelineLibrary = hasExtension(exts, VK_EXT_GRAPHICS_PIPELINE_LIBRARY_EXTENSION_NAME);
    r.deviceFault = hasExtension(exts, VK_EXT_DEVICE_FAULT_EXTENSION_NAME);
    r.memoryBudget = hasExtension(exts, VK_EXT_MEMORY_BUDGET_EXTENSION_NAME);
    r.robustness2 = hasExtension(exts, VK_EXT_ROBUSTNESS_2_EXTENSION_NAME);

    if (r.api13) {
        VkPhysicalDeviceVulkan13Features f13{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
        VkPhysicalDeviceVulkan12Features f12{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES, &f13};
        VkPhysicalDeviceFeatures2 f2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, &f12};
        vkGetPhysicalDeviceFeatures2(pd, &f2);
        r.dynamicRendering = f13.dynamicRendering == VK_TRUE;
        r.synchronization2 = f13.synchronization2 == VK_TRUE;
        r.extendedDynamicState = true;  // EDS1 and EDS2 (without the optional sub-features) are core in 1.3
        r.descriptorIndexing = f12.descriptorIndexing && f12.shaderSampledImageArrayNonUniformIndexing &&
                               f12.descriptorBindingSampledImageUpdateAfterBind && f12.descriptorBindingPartiallyBound &&
                               f12.runtimeDescriptorArray && f12.descriptorBindingVariableDescriptorCount;
        r.timelineSemaphore = f12.timelineSemaphore == VK_TRUE;
        r.textureCompressionBC = f2.features.textureCompressionBC == VK_TRUE;
    }

    auto require = [&](bool ok, const char* what) {
        if (!ok) {
            r.missing.emplace_back(what);
        }
    };
    require(r.api13, "Vulkan 1.3");
    require(r.dynamicRendering, "dynamicRendering");
    require(r.synchronization2, "synchronization2");
    require(r.extendedDynamicState, "extendedDynamicState");
    require(r.pushDescriptor, VK_KHR_PUSH_DESCRIPTOR_EXTENSION_NAME);
    require(r.descriptorIndexing, "descriptorIndexing (update-after-bind, partially bound, variable count)");
    require(r.timelineSemaphore, "timelineSemaphore");
    require(r.textureCompressionBC, "textureCompressionBC");
    require(r.swapchain, VK_KHR_SWAPCHAIN_EXTENSION_NAME);
    return r;
}

}  // namespace

std::string versionString(std::uint32_t v) {
    return std::format("{}.{}.{}", VK_API_VERSION_MAJOR(v), VK_API_VERSION_MINOR(v), VK_API_VERSION_PATCH(v));
}

ProbeResult probe() {
    ProbeResult result;
    if (volkInitialize() != VK_SUCCESS) {
        spdlog::warn("vulkan probe: no Vulkan loader (vulkan-1.dll)");
        return result;
    }
    result.loaderFound = true;
    result.instanceVersion = volkGetInstanceVersion();

    VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    app.pApplicationName = "Risen";
    app.pEngineName = "risen-enhanced-graphics";
    app.apiVersion = VK_API_VERSION_1_3;
    VkInstanceCreateInfo ici{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    ici.pApplicationInfo = &app;
    VkInstance instance = VK_NULL_HANDLE;
    if (const VkResult res = vkCreateInstance(&ici, nullptr, &instance); res != VK_SUCCESS) {
        spdlog::warn("vulkan probe: vkCreateInstance failed ({})", static_cast<int>(res));
        return result;
    }
    volkLoadInstanceOnly(instance);

    std::uint32_t n = 0;
    vkEnumeratePhysicalDevices(instance, &n, nullptr);
    std::vector<VkPhysicalDevice> pds(n);
    vkEnumeratePhysicalDevices(instance, &n, pds.data());

    spdlog::info("vulkan probe: loader {}, {} physical device(s)", versionString(result.instanceVersion), n);
    for (std::uint32_t i = 0; i < n; ++i) {
        DeviceReport r = inspect(pds[i]);
        spdlog::info("  [{}] {} (api {}, vendor 0x{:04x}, {}): {}", i, r.name, versionString(r.apiVersion), r.vendorId,
                     r.discrete ? "discrete" : "integrated/other", r.usable() ? "usable" : "NOT usable");
        for (const auto& m : r.missing) {
            spdlog::info("      missing: {}", m);
        }
        spdlog::info("      optional: eds3={} vertexInputDynamic={} gpl={} deviceFault={} memoryBudget={} robustness2={}",
                     r.extendedDynamicState3, r.vertexInputDynamicState, r.graphicsPipelineLibrary, r.deviceFault,
                     r.memoryBudget, r.robustness2);
        const bool better = result.best < 0 || (r.discrete && !result.devices[static_cast<std::size_t>(result.best)].discrete);
        if (r.usable() && better) {
            result.best = static_cast<int>(i);
        }
        result.devices.push_back(std::move(r));
    }
    if (result.best >= 0) {
        spdlog::info("vulkan probe: preferred device [{}] {}", result.best,
                     result.devices[static_cast<std::size_t>(result.best)].name);
    } else {
        spdlog::warn("vulkan probe: no device meets the floor (Vulkan 1.3 + dynamic rendering + push descriptors ...)");
    }

    vkDestroyInstance(instance, nullptr);
    return result;
}

}  // namespace rvk::vk
