// Adapter enumeration before a device exists (rhi/game_device.h enumerateAdapters): a throw-away Vulkan instance, every physical device inspected against
// the renderer's floor.

#include <algorithm>
#include <mutex>
#include <string>
#include <vector>

#include <spdlog/spdlog.h>

#include "rhi/game_device.h"
#include "vk/debug_names.h"
#include "vk/probe.h"
#include "vk/vk.h"

namespace rvk::rhi {

namespace {
std::mutex g_lastMutex;
std::vector<AdapterInfo> g_last;  // the most recent Vulkan enumeration (the overlay's adapter list: enumerating again at run time is not safe)
}  // namespace

std::vector<AdapterInfo> lastEnumeratedAdapters() {
    const std::lock_guard lock(g_lastMutex);
    return g_last;
}

std::vector<AdapterInfo> enumerateAdapters(Backend backend) {
    std::vector<AdapterInfo> out;
    if (backend == Backend::D3D12) return enumerateD3D12Adapters();
    if (volkInitialize() != VK_SUCCESS) return out;
    VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    app.apiVersion = VK_API_VERSION_1_3;
    VkInstanceCreateInfo ici{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    ici.pApplicationInfo = &app;
    VkInstance instance = VK_NULL_HANDLE;
    if (vkCreateInstance(&ici, nullptr, &instance) != VK_SUCCESS) return out;
    volkLoadInstanceOnly(instance);
    std::uint32_t n = 0;
    vkEnumeratePhysicalDevices(instance, &n, nullptr);
    std::vector<VkPhysicalDevice> pds(n);
    vkEnumeratePhysicalDevices(instance, &n, pds.data());
    for (VkPhysicalDevice pd : pds) {
        const vk::DeviceReport r = vk::inspectDevice(pd);
        AdapterInfo a;
        VkPhysicalDeviceProperties p{};
        vkGetPhysicalDeviceProperties(pd, &p);
        a.name = p.deviceName;
        a.vendorId = p.vendorID;
        a.deviceId = p.deviceID;
        a.driverVersion = p.driverVersion;
        a.usable = r.usable();
        a.discrete = r.discrete;
        VkPhysicalDeviceMemoryProperties mp{};
        vkGetPhysicalDeviceMemoryProperties(pd, &mp);
        for (std::uint32_t h = 0; h < mp.memoryHeapCount; ++h) {
            if ((mp.memoryHeaps[h].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) != 0) {
                a.vramMB = std::max<std::uint64_t>(a.vramMB, mp.memoryHeaps[h].size >> 20);
            }
        }
        out.push_back(std::move(a));
    }
    vkDestroyInstance(instance, nullptr);
    {
        const std::lock_guard lock(g_lastMutex);
        g_last = out;
    }
    return out;
}

void probeBackend(Backend backend) {
    if (backend == Backend::Vulkan) vk::probe();
    else {
        std::string reason;
        const bool ok = d3d12Usable(-1, &reason);
        spdlog::info("probe: D3D12: {}", ok ? std::string("usable (feature level 12_0, Shader Model 6.6, binding tier 3)") : reason);
    }
}

void setDebugNames(bool enabled) { vk::names::setEnabled(enabled); }
bool debugNamesEnabled() { return vk::names::enabled(); }

}  // namespace rvk::rhi
