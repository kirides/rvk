// eCAPIInstance on Vulkan: one adapter (the Vulkan device the renderer will use), the display's 32-bit modes, SM3 caps.

#include <windows.h>

#include <algorithm>
#include <cfloat>
#include <cstring>
#include <mutex>
#include <optional>

#include <spdlog/spdlog.h>

#include "hal/vulkan/hal_vulkan.h"
#include "vk/probe.h"
#include "vk/vk.h"

namespace rvk::hal::vulkan {
namespace {

struct AdapterInfo {
    std::string name = "Vulkan device";
    std::uint32_t vendorId = 0, deviceId = 0, driverVersion = 0;
    std::uint64_t vramMB = 0;
    bool usable = false;  // a device meeting the renderer's floor was found
};

AdapterInfo queryAdapter(int preferred) {
    AdapterInfo info;
    if (volkInitialize() != VK_SUCCESS) return info;
    VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    app.apiVersion = VK_API_VERSION_1_3;
    VkInstanceCreateInfo ici{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    ici.pApplicationInfo = &app;
    VkInstance instance = VK_NULL_HANDLE;
    if (vkCreateInstance(&ici, nullptr, &instance) != VK_SUCCESS) return info;
    volkLoadInstanceOnly(instance);
    std::uint32_t n = 0;
    vkEnumeratePhysicalDevices(instance, &n, nullptr);
    std::vector<VkPhysicalDevice> pds(n);
    vkEnumeratePhysicalDevices(instance, &n, pds.data());
    int pick = -1;
    for (int i = 0; i < static_cast<int>(n); ++i) {
        const vk::DeviceReport r = vk::inspectDevice(pds[static_cast<std::size_t>(i)]);
        if (!r.usable()) continue;
        if (i == preferred) {
            pick = i;
            break;
        }
        if (pick < 0 || (r.discrete && !vk::inspectDevice(pds[static_cast<std::size_t>(pick)]).discrete)) pick = i;
    }
    if (pick >= 0) {
        info.usable = true;
        VkPhysicalDeviceProperties p{};
        vkGetPhysicalDeviceProperties(pds[static_cast<std::size_t>(pick)], &p);
        info.name = std::string(p.deviceName) + " (Vulkan)";
        info.vendorId = p.vendorID;
        info.deviceId = p.deviceID;
        info.driverVersion = p.driverVersion;
        VkPhysicalDeviceMemoryProperties mp{};
        vkGetPhysicalDeviceMemoryProperties(pds[static_cast<std::size_t>(pick)], &mp);
        for (std::uint32_t h = 0; h < mp.memoryHeapCount; ++h) {
            if ((mp.memoryHeaps[h].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) != 0) {
                info.vramMB = std::max<std::uint64_t>(info.vramMB, mp.memoryHeaps[h].size >> 20);
            }
        }
    }
    vkDestroyInstance(instance, nullptr);
    return info;
}

// The query creates a Vulkan instance (~0.1 s): done once per process, the first time by the early
// eCGfxAdmin::sEnumAdapterDesc detour (bootstrap), then reused by HalInstance::GetAdapterDesc.
const AdapterInfo& cachedAdapter(int preferred) {
    static std::mutex mutex;
    static std::optional<AdapterInfo> cache;
    static int cachedFor = 0;
    std::scoped_lock lock(mutex);
    if (!cache || cachedFor != preferred) {
        cache = queryAdapter(preferred);
        cachedFor = preferred;
    }
    return *cache;
}

// what an SM3 D3D9 driver reports (D3DCAPS9 without DeviceType/AdapterOrdinal)
D3DCaps9Tail sm3Caps() {
    D3DCaps9Tail c{};
    c.caps2 = 0x40000000 | 0x20000000 | 0x20000;  // CANAUTOGENMIPMAP | DYNAMICTEXTURES | FULLSCREENGAMMA
    c.caps3 = 0x20 | 0x80 | 0x100 | 0x200;
    c.presentationIntervals = 0x8000000F;
    c.devCaps = 0x001BBEF0;
    c.primitiveMiscCaps = 0x000EFCF2;  // incl. CULL*, COLORWRITEENABLE, BLENDOP, SEPARATEALPHABLEND, MRT bits
    c.rasterCaps = 0x0F732191;         // incl. SCISSORTEST, SLOPESCALEDEPTHBIAS, DEPTHBIAS, MIPMAPLODBIAS, ANISOTROPY
    c.zCmpCaps = 0xFF;
    c.srcBlendCaps = 0x3FFF;
    c.destBlendCaps = 0x3FFF;
    c.alphaCmpCaps = 0xFF;
    c.shadeCaps = 0x84208;
    c.textureCaps = 0x1EC05;  // no POW2 restrictions; cube, volume, mip, projected, alpha
    c.textureFilterCaps = 0x03030700;
    c.cubeTextureFilterCaps = 0x03030300;
    c.volumeTextureFilterCaps = 0x03030300;
    c.textureAddressCaps = 0x3F;
    c.volumeTextureAddressCaps = 0x3F;
    c.lineCaps = 0x1F;
    c.maxTextureWidth = 8192;
    c.maxTextureHeight = 8192;
    c.maxVolumeExtent = 2048;
    c.maxTextureRepeat = 8192;
    c.maxTextureAspectRatio = 8192;
    c.maxAnisotropy = 16;
    c.maxVertexW = 1e10f;
    c.guardBandLeft = -1e8f;
    c.guardBandTop = -1e8f;
    c.guardBandRight = 1e8f;
    c.guardBandBottom = 1e8f;
    c.stencilCaps = 0x1FF;
    c.fvfCaps = 0x00180008;
    c.textureOpCaps = 0x03FEFFFF;
    c.maxTextureBlendStages = 8;
    c.maxSimultaneousTextures = 8;
    c.vertexProcessingCaps = 0x17B;
    c.maxActiveLights = 8;
    c.maxUserClipPlanes = 6;
    c.maxVertexBlendMatrices = 4;
    c.maxVertexBlendMatrixIndex = 8;
    c.maxPointSize = 256.0f;
    c.maxPrimitiveCount = 0xFFFFFF;
    c.maxVertexIndex = 0xFFFFFF;
    c.maxStreams = 16;
    c.maxStreamStride = 508;
    c.vertexShaderVersion = 0xFFFE0300;
    c.maxVertexShaderConst = 256;
    c.pixelShaderVersion = 0xFFFF0300;
    c.pixelShader1xMaxValue = FLT_MAX;
    c.devCaps2 = 0x51;
    c.numberOfAdaptersInGroup = 1;
    c.declTypes = 0x3FF;
    c.numSimultaneousRTs = 4;
    c.stretchRectFilterCaps = 0x03030300;
    c.vs20Caps = {1, 24, 32, 4};
    c.ps20Caps = {0x1F, 24, 32, 4, 512};  // incl. PREDICATION (read by eCShaderMaterialAdmin::PostInitialize)
    c.vertexTextureFilterCaps = 0x03000300;
    c.maxVShaderInstructionsExecuted = 0xFFFF;
    c.maxPShaderInstructionsExecuted = 0xFFFF;
    c.maxVertexShader30InstructionSlots = 32768;
    c.maxPixelShader30InstructionSlots = 32768;
    return c;
}

}  // namespace

HalInstance::HalInstance(const Options& options) : options_(options) {}

bEResult HalInstance::Init(eCGfxShared::eEAPIType) {
    // the display's 32-bit modes: the engine picks the one matching the window exactly (as with D3D9's mode list)
    DEVMODEW dm{};
    dm.dmSize = sizeof(dm);
    for (DWORD i = 0; EnumDisplaySettingsW(nullptr, i, &dm); ++i) {
        if (dm.dmBitsPerPel != 32) continue;
        const Mode m{dm.dmPelsWidth, dm.dmPelsHeight, dm.dmDisplayFrequency};
        const bool dup = std::ranges::any_of(modes_, [&](const Mode& o) {
            return o.width == m.width && o.height == m.height && o.refresh == m.refresh;
        });
        if (!dup) modes_.push_back(m);
    }
    if (modes_.empty()) {
        modes_.push_back({1280, 720, 60});
    }
    spdlog::info("hal: Vulkan instance, {} display modes", modes_.size());
    return bEResult_Ok;
}

bEResult HalInstance::Finit() { return bEResult_Ok; }

bEResult HalInstance::GetAdapterCount(unsigned long* a0) {
    if (a0) *a0 = 1;
    return bEResult_Ok;
}

bEResult HalInstance::GetModeCount(unsigned long, unsigned long* a1) {
    if (a1) *a1 = static_cast<unsigned long>(modes_.size());
    return bEResult_Ok;
}

bool fillAdapterDesc(int preferredAdapter, eCGfxShared::eSGfxAdapterDesc* desc) {
    const AdapterInfo& info = cachedAdapter(preferredAdapter);
    std::memset(desc, 0, sizeof(*desc));
    std::snprintf(desc->driver, sizeof(desc->driver), "risen-enhanced-graphics");
    std::snprintf(desc->description, sizeof(desc->description), "%s", info.name.c_str());
    desc->driverVersion = info.driverVersion;
    desc->vendorId = info.vendorId;
    desc->deviceId = info.deviceId;
    desc->caps = sm3Caps();
    desc->dedicatedVideoMemoryMB = static_cast<std::int32_t>(std::min<std::uint64_t>(info.vramMB, 0x7FFFFFFF));
    spdlog::info("hal: adapter '{}' ({} MiB){}", info.name, info.vramMB, info.usable ? "" : ": NOT usable");
    return info.usable;
}

bEResult HalInstance::GetAdapterDesc(unsigned long, eCGfxShared::eSGfxAdapterDesc* a1) {
    if (a1 == nullptr) return bEResult_False;
    fillAdapterDesc(options_.adapter, a1);
    return bEResult_Ok;
}

bEResult HalInstance::GetModeDesc(unsigned long, unsigned long a1, eCGfxShared::eSGfxModeDesc* a2) {
    if (a2 == nullptr || a1 >= modes_.size()) return bEResult_False;
    a2->width = modes_[a1].width;
    a2->height = modes_[a1].height;
    a2->refreshRate = modes_[a1].refresh;
    a2->format = 22;  // D3DFMT_X8R8G8B8: the only format CreateGraphicsSubsystem accepts
    return bEResult_Ok;
}

bEResult HalInstance::CreateDevice(unsigned long, eCAPIDevice::eSAPIStartParameters const& a1, eCAPIDevice** a2) {
    if (a2 == nullptr) return bEResult_False;
    *a2 = nullptr;
    auto* device = new HalDevice();
    if (!device->create(options_, a1)) {
        spdlog::error("hal: Vulkan device creation failed");
        delete device;
        return bEResult_False;
    }
    *a2 = device;
    return bEResult_Ok;
}

}  // namespace rvk::hal::vulkan
