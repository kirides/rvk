#include "vk/context.h"

#include <algorithm>
#include <cstring>
#include <vector>

#include <spdlog/spdlog.h>

#include "vk/probe.h"

namespace rvk::vk {
namespace {

bool hasExtension(const std::vector<VkExtensionProperties>& exts, const char* name) {
    return std::ranges::any_of(exts, [&](const VkExtensionProperties& e) { return std::strcmp(e.extensionName, name) == 0; });
}

VKAPI_ATTR VkBool32 VKAPI_CALL debugCallback(VkDebugUtilsMessageSeverityFlagBitsEXT severity, VkDebugUtilsMessageTypeFlagsEXT,
                                             const VkDebugUtilsMessengerCallbackDataEXT* data, void* user) {
    auto* ctx = static_cast<Context*>(user);
    static std::atomic<int> logged{0};
    const bool error = (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT) != 0;
    if (error) {
        ctx->noteValidationError();
    } else if (data->pMessage != nullptr && std::strstr(data->pMessage, "and this write is unused") != nullptr) {
        // a pixel shader writing oC0 in a depth-only pass (D3D9 NULL colour target, shadow maps): defined, the write is dropped
        return VK_FALSE;
    }
    // throttle: a broken state can produce a message per draw
    if (logged.fetch_add(1) < 500) {
        if (error) {
            spdlog::error("vulkan: {}", data->pMessage);
        } else {
            spdlog::warn("vulkan: {}", data->pMessage);
        }
    }
    return VK_FALSE;
}

}  // namespace

const char* resultString(VkResult r) {
    switch (r) {
    case VK_SUCCESS: return "VK_SUCCESS";
    case VK_NOT_READY: return "VK_NOT_READY";
    case VK_TIMEOUT: return "VK_TIMEOUT";
    case VK_SUBOPTIMAL_KHR: return "VK_SUBOPTIMAL_KHR";
    case VK_ERROR_OUT_OF_HOST_MEMORY: return "VK_ERROR_OUT_OF_HOST_MEMORY";
    case VK_ERROR_OUT_OF_DEVICE_MEMORY: return "VK_ERROR_OUT_OF_DEVICE_MEMORY";
    case VK_ERROR_INITIALIZATION_FAILED: return "VK_ERROR_INITIALIZATION_FAILED";
    case VK_ERROR_DEVICE_LOST: return "VK_ERROR_DEVICE_LOST";
    case VK_ERROR_EXTENSION_NOT_PRESENT: return "VK_ERROR_EXTENSION_NOT_PRESENT";
    case VK_ERROR_FEATURE_NOT_PRESENT: return "VK_ERROR_FEATURE_NOT_PRESENT";
    case VK_ERROR_SURFACE_LOST_KHR: return "VK_ERROR_SURFACE_LOST_KHR";
    case VK_ERROR_OUT_OF_DATE_KHR: return "VK_ERROR_OUT_OF_DATE_KHR";
    case VK_ERROR_FORMAT_NOT_SUPPORTED: return "VK_ERROR_FORMAT_NOT_SUPPORTED";
    default: return "VK_ERROR_?";
    }
}

Context::~Context() { destroy(); }

bool Context::create(const ContextOptions& options) {
    if (volkInitialize() != VK_SUCCESS) {
        spdlog::error("vulkan: no Vulkan loader");
        return false;
    }
    if (volkGetInstanceVersion() < VK_API_VERSION_1_3) {
        spdlog::error("vulkan: loader version {} < 1.3", versionString(volkGetInstanceVersion()));
        return false;
    }
    if (!createInstance(options) || !pickPhysicalDevice(options) || !createDevice() || !createAllocator()) {
        destroy();
        return false;
    }
    spdlog::info("vulkan: device '{}' ready (api {}, eds3={}, vertexInputDynamic={}, anisotropy={})", deviceName_,
                 versionString(props_.apiVersion), features_.extendedDynamicState3, features_.vertexInputDynamicState,
                 features_.maxAnisotropy);
    return true;
}

bool Context::createInstance(const ContextOptions& options) {
    std::uint32_t n = 0;
    vkEnumerateInstanceExtensionProperties(nullptr, &n, nullptr);
    std::vector<VkExtensionProperties> exts(n);
    vkEnumerateInstanceExtensionProperties(nullptr, &n, exts.data());

    std::vector<const char*> enabled = {VK_KHR_SURFACE_EXTENSION_NAME, VK_KHR_WIN32_SURFACE_EXTENSION_NAME};
    debugUtils_ = hasExtension(exts, VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
    if (debugUtils_) {
        enabled.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
    }
    std::vector<const char*> layers;
    {
        std::uint32_t ln = 0;
        vkEnumerateInstanceLayerProperties(&ln, nullptr);
        std::vector<VkLayerProperties> props(ln);
        vkEnumerateInstanceLayerProperties(&ln, props.data());
        std::string names;
        for (const auto& l : props) names += std::string(names.empty() ? "" : ", ") + l.layerName;
        spdlog::info("vulkan: available layers: {}", names.empty() ? "none" : names);
    }
    if (options.validation) {
        std::uint32_t ln = 0;
        vkEnumerateInstanceLayerProperties(&ln, nullptr);
        std::vector<VkLayerProperties> props(ln);
        vkEnumerateInstanceLayerProperties(&ln, props.data());
        const bool have = std::ranges::any_of(props, [](const VkLayerProperties& l) {
            return std::strcmp(l.layerName, "VK_LAYER_KHRONOS_validation") == 0;
        });
        if (have) {
            layers.push_back("VK_LAYER_KHRONOS_validation");
        } else {
            spdlog::warn("vulkan: validation requested but VK_LAYER_KHRONOS_validation is not installed");
        }
    }

    VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    app.pApplicationName = options.appName;
    app.pEngineName = "risen-enhanced-graphics";
    app.engineVersion = VK_MAKE_VERSION(0, 1, 0);
    app.apiVersion = VK_API_VERSION_1_3;
    VkInstanceCreateInfo ici{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    ici.pApplicationInfo = &app;
    ici.enabledExtensionCount = static_cast<std::uint32_t>(enabled.size());
    ici.ppEnabledExtensionNames = enabled.data();
    ici.enabledLayerCount = static_cast<std::uint32_t>(layers.size());
    ici.ppEnabledLayerNames = layers.data();
    if (const VkResult r = vkCreateInstance(&ici, nullptr, &instance_); r != VK_SUCCESS) {
        spdlog::error("vulkan: vkCreateInstance: {}", resultString(r));
        return false;
    }
    volkLoadInstanceOnly(instance_);

    if (debugUtils_ && !layers.empty()) {
        VkDebugUtilsMessengerCreateInfoEXT mci{VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT};
        mci.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
        mci.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT |
                          VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT;
        mci.pfnUserCallback = debugCallback;
        mci.pUserData = this;
        vkCreateDebugUtilsMessengerEXT(instance_, &mci, nullptr, &messenger_);
        spdlog::info("vulkan: validation layer enabled");
    }
    return true;
}

bool Context::pickPhysicalDevice(const ContextOptions& options) {
    std::uint32_t n = 0;
    vkEnumeratePhysicalDevices(instance_, &n, nullptr);
    std::vector<VkPhysicalDevice> pds(n);
    vkEnumeratePhysicalDevices(instance_, &n, pds.data());
    const ProbeResult probeResult = [&] {
        ProbeResult r;
        r.loaderFound = true;
        for (auto pd : pds) {
            r.devices.push_back(inspectDevice(pd));
        }
        return r;
    }();
    int pick = -1;
    if (options.adapter >= 0 && options.adapter < static_cast<int>(n)) {
        if (probeResult.devices[static_cast<std::size_t>(options.adapter)].usable()) {
            pick = options.adapter;
        } else {
            spdlog::warn("vulkan: configured adapter {} does not meet the floor", options.adapter);
        }
    }
    for (int i = 0; pick < 0 && i < static_cast<int>(n); ++i) {
        if (probeResult.devices[static_cast<std::size_t>(i)].usable() && probeResult.devices[static_cast<std::size_t>(i)].discrete) {
            pick = i;
        }
    }
    for (int i = 0; pick < 0 && i < static_cast<int>(n); ++i) {
        if (probeResult.devices[static_cast<std::size_t>(i)].usable()) {
            pick = i;
        }
    }
    if (pick < 0) {
        spdlog::error("vulkan: no physical device meets the floor (Vulkan 1.3, dynamic rendering, push descriptors, ...)");
        return false;
    }
    physical_ = pds[static_cast<std::size_t>(pick)];
    const DeviceReport& rep = probeResult.devices[static_cast<std::size_t>(pick)];
    vkGetPhysicalDeviceProperties(physical_, &props_);
    deviceName_ = props_.deviceName;
    features_.extendedDynamicState3 = rep.extendedDynamicState3;
    features_.vertexInputDynamicState = rep.vertexInputDynamicState;
    features_.memoryBudget = rep.memoryBudget;
    features_.deviceFault = rep.deviceFault;

    std::uint32_t qn = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(physical_, &qn, nullptr);
    std::vector<VkQueueFamilyProperties> qf(qn);
    vkGetPhysicalDeviceQueueFamilyProperties(physical_, &qn, qf.data());
    for (std::uint32_t i = 0; i < qn; ++i) {
        if ((qf[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) != 0 && vkGetPhysicalDeviceWin32PresentationSupportKHR(physical_, i)) {
            queueFamily_ = i;
            return true;
        }
    }
    spdlog::error("vulkan: no graphics queue family that can present");
    return false;
}

bool Context::createDevice() {
    std::uint32_t n = 0;
    vkEnumerateDeviceExtensionProperties(physical_, nullptr, &n, nullptr);
    std::vector<VkExtensionProperties> exts(n);
    vkEnumerateDeviceExtensionProperties(physical_, nullptr, &n, exts.data());

    std::vector<const char*> enabled = {VK_KHR_SWAPCHAIN_EXTENSION_NAME};
    if (props_.apiVersion < VK_API_VERSION_1_4 || hasExtension(exts, VK_KHR_PUSH_DESCRIPTOR_EXTENSION_NAME)) {
        enabled.push_back(VK_KHR_PUSH_DESCRIPTOR_EXTENSION_NAME);
    }

    // supported core features
    VkPhysicalDeviceFeatures2 sup2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
    vkGetPhysicalDeviceFeatures2(physical_, &sup2);

    VkPhysicalDeviceFeatures2 f2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
    f2.features.textureCompressionBC = VK_TRUE;
    f2.features.samplerAnisotropy = sup2.features.samplerAnisotropy;
    f2.features.fillModeNonSolid = sup2.features.fillModeNonSolid;
    f2.features.independentBlend = sup2.features.independentBlend;
    f2.features.shaderClipDistance = sup2.features.shaderClipDistance;
    f2.features.depthClamp = sup2.features.depthClamp;
    f2.features.depthBiasClamp = sup2.features.depthBiasClamp;
    f2.features.imageCubeArray = sup2.features.imageCubeArray;
    features_.samplerAnisotropy = sup2.features.samplerAnisotropy == VK_TRUE;
    features_.maxAnisotropy = features_.samplerAnisotropy ? props_.limits.maxSamplerAnisotropy : 1.0f;

    VkPhysicalDeviceVulkan12Features f12{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
    f12.descriptorIndexing = VK_TRUE;
    f12.runtimeDescriptorArray = VK_TRUE;
    f12.shaderSampledImageArrayNonUniformIndexing = VK_TRUE;
    f12.descriptorBindingSampledImageUpdateAfterBind = VK_TRUE;
    f12.descriptorBindingPartiallyBound = VK_TRUE;
    f12.descriptorBindingVariableDescriptorCount = VK_TRUE;
    f12.descriptorBindingUpdateUnusedWhilePending = VK_TRUE;
    f12.timelineSemaphore = VK_TRUE;
    f12.bufferDeviceAddress = VK_TRUE;
    f12.scalarBlockLayout = VK_TRUE;
    VkPhysicalDeviceVulkan13Features f13{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
    f13.dynamicRendering = VK_TRUE;
    f13.synchronization2 = VK_TRUE;
    f13.maintenance4 = VK_TRUE;
    // glslang emits GLSL `discard` as OpDemoteToHelperInvocation for SPIR-V 1.6 (D3D9 texkill keeps derivatives working too)
    f13.shaderDemoteToHelperInvocation = VK_TRUE;
    f13.shaderTerminateInvocation = VK_TRUE;
    f2.pNext = &f12;
    f12.pNext = &f13;
    void** tail = &f13.pNext;

    VkPhysicalDeviceExtendedDynamicState3FeaturesEXT eds3{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTENDED_DYNAMIC_STATE_3_FEATURES_EXT};
    if (features_.extendedDynamicState3) {
        VkPhysicalDeviceExtendedDynamicState3FeaturesEXT q{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTENDED_DYNAMIC_STATE_3_FEATURES_EXT};
        VkPhysicalDeviceFeatures2 qf{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, &q};
        vkGetPhysicalDeviceFeatures2(physical_, &qf);
        // only the subset the D3D9 state model needs
        if (q.extendedDynamicState3ColorBlendEnable && q.extendedDynamicState3ColorBlendEquation &&
            q.extendedDynamicState3ColorWriteMask) {
            eds3.extendedDynamicState3ColorBlendEnable = VK_TRUE;
            eds3.extendedDynamicState3ColorBlendEquation = VK_TRUE;
            eds3.extendedDynamicState3ColorWriteMask = VK_TRUE;
            eds3.extendedDynamicState3PolygonMode = q.extendedDynamicState3PolygonMode;
            enabled.push_back(VK_EXT_EXTENDED_DYNAMIC_STATE_3_EXTENSION_NAME);
            *tail = &eds3;
            tail = &eds3.pNext;
        } else {
            features_.extendedDynamicState3 = false;
        }
    }
    VkPhysicalDeviceVertexInputDynamicStateFeaturesEXT vids{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VERTEX_INPUT_DYNAMIC_STATE_FEATURES_EXT};
    if (features_.vertexInputDynamicState) {
        vids.vertexInputDynamicState = VK_TRUE;
        enabled.push_back(VK_EXT_VERTEX_INPUT_DYNAMIC_STATE_EXTENSION_NAME);
        *tail = &vids;
        tail = &vids.pNext;
    }
    VkPhysicalDeviceRobustness2FeaturesEXT rob2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ROBUSTNESS_2_FEATURES_EXT};
    if (hasExtension(exts, VK_EXT_ROBUSTNESS_2_EXTENSION_NAME)) {
        VkPhysicalDeviceRobustness2FeaturesEXT q{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ROBUSTNESS_2_FEATURES_EXT};
        VkPhysicalDeviceFeatures2 qf{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, &q};
        vkGetPhysicalDeviceFeatures2(physical_, &qf);
        if (q.nullDescriptor) {
            rob2.nullDescriptor = VK_TRUE;
            features_.robustness2NullDescriptor = true;
            enabled.push_back(VK_EXT_ROBUSTNESS_2_EXTENSION_NAME);
            *tail = &rob2;
            tail = &rob2.pNext;
        }
    }
    if (features_.memoryBudget) {
        enabled.push_back(VK_EXT_MEMORY_BUDGET_EXTENSION_NAME);
    }
    VkPhysicalDeviceFaultFeaturesEXT fault{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FAULT_FEATURES_EXT};
    if (features_.deviceFault) {
        fault.deviceFault = VK_TRUE;
        enabled.push_back(VK_EXT_DEVICE_FAULT_EXTENSION_NAME);
        *tail = &fault;
        tail = &fault.pNext;
    }

    const float priority = 1.0f;
    VkDeviceQueueCreateInfo qci{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    qci.queueFamilyIndex = queueFamily_;
    qci.queueCount = 1;
    qci.pQueuePriorities = &priority;
    VkDeviceCreateInfo dci{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, &f2};
    dci.queueCreateInfoCount = 1;
    dci.pQueueCreateInfos = &qci;
    dci.enabledExtensionCount = static_cast<std::uint32_t>(enabled.size());
    dci.ppEnabledExtensionNames = enabled.data();
    if (const VkResult r = vkCreateDevice(physical_, &dci, nullptr, &device_); r != VK_SUCCESS) {
        spdlog::error("vulkan: vkCreateDevice: {}", resultString(r));
        return false;
    }
    volkLoadDevice(device_);
    vkGetDeviceQueue(device_, queueFamily_, 0, &queue_);
    return true;
}

bool Context::createAllocator() {
    VmaAllocatorCreateInfo aci{};
    aci.flags = VMA_ALLOCATOR_CREATE_BUFFER_DEVICE_ADDRESS_BIT;
    if (features_.memoryBudget) {
        aci.flags |= VMA_ALLOCATOR_CREATE_EXT_MEMORY_BUDGET_BIT;
    }
    aci.physicalDevice = physical_;
    aci.device = device_;
    aci.instance = instance_;
    aci.vulkanApiVersion = VK_API_VERSION_1_3;
    VmaVulkanFunctions funcs{};
    if (vmaImportVulkanFunctionsFromVolk(&aci, &funcs) != VK_SUCCESS) {
        spdlog::error("vulkan: vmaImportVulkanFunctionsFromVolk failed");
        return false;
    }
    aci.pVulkanFunctions = &funcs;
    if (const VkResult r = vmaCreateAllocator(&aci, &allocator_); r != VK_SUCCESS) {
        spdlog::error("vulkan: vmaCreateAllocator: {}", resultString(r));
        return false;
    }
    return true;
}

void Context::destroy() {
    if (device_ != VK_NULL_HANDLE) {
        vkDeviceWaitIdle(device_);
    }
    if (allocator_ != VK_NULL_HANDLE) {
        vmaDestroyAllocator(allocator_);
        allocator_ = VK_NULL_HANDLE;
    }
    if (device_ != VK_NULL_HANDLE) {
        vkDestroyDevice(device_, nullptr);
        device_ = VK_NULL_HANDLE;
    }
    if (messenger_ != VK_NULL_HANDLE) {
        vkDestroyDebugUtilsMessengerEXT(instance_, messenger_, nullptr);
        messenger_ = VK_NULL_HANDLE;
    }
    if (instance_ != VK_NULL_HANDLE) {
        vkDestroyInstance(instance_, nullptr);
        instance_ = VK_NULL_HANDLE;
    }
}

void Context::setName(VkObjectType type, std::uint64_t handle, const char* name) const {
    if (!debugUtils_ || device_ == VK_NULL_HANDLE || vkSetDebugUtilsObjectNameEXT == nullptr) {
        return;
    }
    VkDebugUtilsObjectNameInfoEXT info{VK_STRUCTURE_TYPE_DEBUG_UTILS_OBJECT_NAME_INFO_EXT};
    info.objectType = type;
    info.objectHandle = handle;
    info.pObjectName = name;
    vkSetDebugUtilsObjectNameEXT(device_, &info);
}

}  // namespace rvk::vk
