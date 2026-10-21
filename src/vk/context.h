#pragma once

// Vulkan instance + device for the renderer (docs/PLAN.md "Vulkan floor": 1.3 core + VK_KHR_push_descriptor).

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>

#include "vk/vk.h"

namespace rvk::vk {

struct ContextOptions {
    bool validation = false;  // VK_LAYER_KHRONOS_validation + debug messenger
    int adapter = -1;         // physical device index, -1 = best discrete GPU
    const char* appName = "Risen";
};

// Optional device features that were found and enabled.
struct DeviceFeatures {
    bool extendedDynamicState3 = false;     // blend enable/equation/write mask + polygon mode as dynamic state
    bool vertexInputDynamicState = false;   // vertex input as dynamic state
    bool graphicsPipelineLibrary = false;   // VK_EXT_graphics_pipeline_library: shaders compiled once, pipelines linked fast
    bool robustness2NullDescriptor = false;
    bool memoryBudget = false;
    bool deviceFault = false;
    bool samplerAnisotropy = false;
    float maxAnisotropy = 1.0f;
    bool hostQueryReset = false;  // vkResetQueryPool from the host (GPU timestamps of [debug] perf)
    float timestampPeriod = 0.0f; // ns per timestamp tick; 0 = the queue cannot write timestamps
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
    std::uint32_t queueFamily() const { return queueFamily_; }
    VmaAllocator allocator() const { return allocator_; }
    const VkPhysicalDeviceProperties& properties() const { return props_; }
    const DeviceFeatures& features() const { return features_; }
    const std::string& deviceName() const { return deviceName_; }

    // validation messages of severity error seen so far (tests assert 0)
    std::uint32_t validationErrors() const { return validationErrors_.load(); }
    void noteValidationError() { ++validationErrors_; }

    // debug names (no-op without VK_EXT_debug_utils)
    void setName(VkObjectType type, std::uint64_t handle, const char* name) const;
    template <class T>
    void setName(T handle, VkObjectType type, const char* name) const {
        setName(type, reinterpret_cast<std::uint64_t>(handle), name);
    }

private:
    bool createInstance(const ContextOptions& options);
    bool pickPhysicalDevice(const ContextOptions& options);
    bool createDevice();
    bool createAllocator();

    VkInstance instance_ = VK_NULL_HANDLE;
    VkDebugUtilsMessengerEXT messenger_ = VK_NULL_HANDLE;
    VkPhysicalDevice physical_ = VK_NULL_HANDLE;
    VkDevice device_ = VK_NULL_HANDLE;
    VkQueue queue_ = VK_NULL_HANDLE;
    std::uint32_t queueFamily_ = 0;
    VmaAllocator allocator_ = VK_NULL_HANDLE;
    VkPhysicalDeviceProperties props_{};
    DeviceFeatures features_;
    std::string deviceName_;
    bool debugUtils_ = false;
    std::mutex queueMutex_;
    std::atomic<std::uint32_t> validationErrors_{0};
};

// VkResult -> "VK_ERROR_DEVICE_LOST" etc.
const char* resultString(VkResult r);

}  // namespace rvk::vk
