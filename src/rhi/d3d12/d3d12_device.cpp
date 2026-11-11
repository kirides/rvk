// The DX12 backend of the slim RHI (rhi/rhi.h). Requirements (no fallback below them): Shader Model 6.6 (dynamic resources: the HLSL reaches textures,
// samplers and buffers through ResourceDescriptorHeap / SamplerDescriptorHeap) and Resource Binding Tier 3. The root signature is fixed (the RHI's
// binding model, rhi.h): 32 root constants (b0 space0), one root CBV (b8 space1), a table of 4 UAVs (u0..u3 space1), both heaps directly indexed.
// Vulkan structs are the interface's vocabulary and are translated here: formats, image layouts -> resource states, blend / cull / compare enums,
// vertex input -> input layout (semantic ATTR<location>), VkDrawIndexedIndirectCommand -> D3D12_DRAW_INDEXED_ARGUMENTS (same 20 bytes),
// VkAccelerationStructureInstanceKHR -> D3D12_RAYTRACING_INSTANCE_DESC (same 64 bytes).

// the vcpkg directx-headers port's headers (Agility SDK 1.619: ID3D12DeviceFactory, ...), which must win over the Windows SDK's older d3d12.h
#include <directx/d3d12.h>
#include <dxgi1_6.h>
#include "rhi/d3d12/d3d12_test_hooks.h"
#include <wrl/client.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <format>
#include <functional>
#include <map>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include <spdlog/spdlog.h>

// PIX events on the command lists (capture regions): the header-only encoder of the vcpkg winpixevent port, no WinPixEventRuntime.dll / lib needed for
// command list events (PIX reads the encoded blob); the debug layer accepts this format, unlike the raw BeginEvent(0, wide string) one
#define USE_PIX
#include <pix3.h>

#include "core/crash_dump.h"
#include "core/gpu_events.h"
#include "core/caps.h"
#include "rhi/d3d12/d3d12_ffx.h"
#include "rhi/d3d12/heap_alloc.h"
#include "rhi/game_device.h"
#include "rhi/rhi.h"
#include "shader/embedded.h"
#include "shader/pass_push.h"

#include <imgui.h>
#include "imgui_impl_dx12.h"

namespace rvk::rhi {
namespace {

using Microsoft::WRL::ComPtr;

constexpr std::uint32_t kHeapSize = 65536;      // shader-visible CBV / SRV / UAV heap (the Vulkan side's bindless arrays together are smaller)
constexpr std::uint32_t kUavRing = 2048;        // the last entries of that heap: tables of kStorageImages UAVs written per setStorageViews / clear / blit view, per frame slot
constexpr std::uint32_t kUavRingPerSlot = kUavRing / rhi::kFramesInFlight;
constexpr std::uint32_t kViewRing = 512;        // single transient SRVs (blits), before the UAV ring: kViewRing / kFramesInFlight per frame slot
constexpr std::uint32_t kReservedSlots = 3;     // 0 = 2D default, 1 = cube default, 2 = 3D default (a texel (0, 0, 0, 1) of that dimension)
constexpr std::uint32_t kLowSlotBegin = 14336, kLowSlotEnd = 16384;  // reserved for TextureDesc::lowHeapSlot (the pack word addresses 14 bits); other resources hop over it
constexpr std::uint32_t kSamplerHeapSize = 2048;
constexpr std::uint32_t kRtvCount = 128, kDsvCount = 32;

#define RVK_HR(expr, what)                                                                                          \
    do {                                                                                                            \
        const HRESULT rvk_hr = (expr);                                                                              \
        if (FAILED(rvk_hr)) {                                                                                       \
            spdlog::error("d3d12: {} failed (0x{:08x})", what, static_cast<unsigned>(rvk_hr));                      \
            return false;                                                                                           \
        }                                                                                                           \
    } while (0)

// The typed DXGI format of a storage format (render targets, UAVs, copies, input layouts; depth formats: the DSV format)
DXGI_FORMAT toDxgi(VkFormat f) {
    switch (f) {
    case VK_FORMAT_R8_UNORM: return DXGI_FORMAT_R8_UNORM;
    case VK_FORMAT_R8_UINT: return DXGI_FORMAT_R8_UINT;
    case VK_FORMAT_R8G8_UNORM: return DXGI_FORMAT_R8G8_UNORM;
    case VK_FORMAT_R8G8_SNORM: return DXGI_FORMAT_R8G8_SNORM;
    case VK_FORMAT_R16_SFLOAT: return DXGI_FORMAT_R16_FLOAT;
    case VK_FORMAT_R16_UNORM: return DXGI_FORMAT_R16_UNORM;
    case VK_FORMAT_R16_UINT: return DXGI_FORMAT_R16_UINT;
    case VK_FORMAT_R8G8B8A8_UNORM: return DXGI_FORMAT_R8G8B8A8_UNORM;
    case VK_FORMAT_R8G8B8A8_SNORM: return DXGI_FORMAT_R8G8B8A8_SNORM;
    case VK_FORMAT_R8G8B8A8_SRGB: return DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
    case VK_FORMAT_B8G8R8A8_UNORM: return DXGI_FORMAT_B8G8R8A8_UNORM;
    case VK_FORMAT_B8G8R8A8_SRGB: return DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;
    case VK_FORMAT_R5G6B5_UNORM_PACK16: return DXGI_FORMAT_B5G6R5_UNORM;
    case VK_FORMAT_A1R5G5B5_UNORM_PACK16: return DXGI_FORMAT_B5G5R5A1_UNORM;
    case VK_FORMAT_B4G4R4A4_UNORM_PACK16: return DXGI_FORMAT_B4G4R4A4_UNORM;
    case VK_FORMAT_A2B10G10R10_UNORM_PACK32:
    case VK_FORMAT_A2R10G10B10_UNORM_PACK32: return DXGI_FORMAT_R10G10B10A2_UNORM;
    case VK_FORMAT_R16G16_SFLOAT: return DXGI_FORMAT_R16G16_FLOAT;
    case VK_FORMAT_R16G16_UNORM: return DXGI_FORMAT_R16G16_UNORM;
    case VK_FORMAT_R16G16_SNORM: return DXGI_FORMAT_R16G16_SNORM;
    case VK_FORMAT_R16G16B16A16_SFLOAT: return DXGI_FORMAT_R16G16B16A16_FLOAT;
    case VK_FORMAT_R16G16B16A16_UNORM: return DXGI_FORMAT_R16G16B16A16_UNORM;
    case VK_FORMAT_R16G16B16A16_SNORM: return DXGI_FORMAT_R16G16B16A16_SNORM;
    case VK_FORMAT_R32_SFLOAT: return DXGI_FORMAT_R32_FLOAT;
    case VK_FORMAT_R32_UINT: return DXGI_FORMAT_R32_UINT;
    case VK_FORMAT_R32G32_UINT: return DXGI_FORMAT_R32G32_UINT;  // instance-rate refs (the GPU-driven draws' per-draw index)
    case VK_FORMAT_R32G32_SFLOAT: return DXGI_FORMAT_R32G32_FLOAT;
    case VK_FORMAT_R32G32B32_SFLOAT: return DXGI_FORMAT_R32G32B32_FLOAT;
    case VK_FORMAT_R32G32B32A32_SFLOAT: return DXGI_FORMAT_R32G32B32A32_FLOAT;
    case VK_FORMAT_BC1_RGBA_UNORM_BLOCK: return DXGI_FORMAT_BC1_UNORM;
    case VK_FORMAT_BC2_UNORM_BLOCK: return DXGI_FORMAT_BC2_UNORM;
    case VK_FORMAT_BC3_UNORM_BLOCK: return DXGI_FORMAT_BC3_UNORM;
    case VK_FORMAT_BC4_UNORM_BLOCK: return DXGI_FORMAT_BC4_UNORM;
    case VK_FORMAT_BC5_UNORM_BLOCK: return DXGI_FORMAT_BC5_UNORM;
    case VK_FORMAT_D16_UNORM: return DXGI_FORMAT_D16_UNORM;
    case VK_FORMAT_D24_UNORM_S8_UINT: return DXGI_FORMAT_D24_UNORM_S8_UINT;
    case VK_FORMAT_D32_SFLOAT: return DXGI_FORMAT_D32_FLOAT;
    case VK_FORMAT_D32_SFLOAT_S8_UINT: return DXGI_FORMAT_D32_FLOAT_S8X24_UINT;
    default: return DXGI_FORMAT_UNKNOWN;
    }
}

// vertex attribute formats: the scaled integer ones (D3D9 UBYTE4, SHORT2 / 4, UDEC3) have no DXGI twin, they are read as integers
// (docs/research/dx12-game-port.md WP0: the vertex shader of such a draw converts)
DXGI_FORMAT toDxgiVertex(VkFormat f) {
    switch (f) {
    case VK_FORMAT_R8G8B8A8_USCALED: return DXGI_FORMAT_R8G8B8A8_UNORM;  // (the vertex shader multiplies by 255: translator, push flags bits 17..31)
    case VK_FORMAT_R16G16_SSCALED: return DXGI_FORMAT_R16G16_SINT;
    case VK_FORMAT_R16G16B16A16_SSCALED: return DXGI_FORMAT_R16G16B16A16_SINT;
    case VK_FORMAT_A2B10G10R10_USCALED_PACK32: return DXGI_FORMAT_R10G10B10A2_UINT;
    default: return toDxgi(f);
    }
}

bool isDepth(VkFormat f) {
    return f == VK_FORMAT_D16_UNORM || f == VK_FORMAT_D24_UNORM_S8_UINT || f == VK_FORMAT_D32_SFLOAT || f == VK_FORMAT_D32_SFLOAT_S8_UINT;
}

// the resource format of a texture: depth formats that are also sampled are typeless
DXGI_FORMAT resourceFormat(VkFormat f, bool sampled) {
    if (sampled) {
        switch (f) {
        case VK_FORMAT_D16_UNORM: return DXGI_FORMAT_R16_TYPELESS;
        case VK_FORMAT_D24_UNORM_S8_UINT: return DXGI_FORMAT_R24G8_TYPELESS;
        case VK_FORMAT_D32_SFLOAT: return DXGI_FORMAT_R32_TYPELESS;
        case VK_FORMAT_D32_SFLOAT_S8_UINT: return DXGI_FORMAT_R32G8X24_TYPELESS;
        default: break;
        }
    }
    return toDxgi(f);
}
DXGI_FORMAT srvFormat(VkFormat f) {
    switch (f) {
    case VK_FORMAT_D16_UNORM: return DXGI_FORMAT_R16_UNORM;
    case VK_FORMAT_D24_UNORM_S8_UINT: return DXGI_FORMAT_R24_UNORM_X8_TYPELESS;
    case VK_FORMAT_D32_SFLOAT: return DXGI_FORMAT_R32_FLOAT;
    case VK_FORMAT_D32_SFLOAT_S8_UINT: return DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS;
    default: return toDxgi(f);
    }
}

// The sampled view's component mapping: the texture's own swizzle (VkComponentMapping: L8 -> r r r 1, X8 -> .. 1) composed with what the DXGI
// format needs of its own. A2R10G10B10 (D3D9: B in the low bits) is read as R10G10B10A2 with R and B swapped (sampled textures only: render
// targets keep the logical order); B4G4R4A4 is D3D9's A4R4G4B4 as DXGI knows it, so Vulkan's compensating swizzle is dropped.
UINT srvMapping(const TextureDesc& d) {
    const VkComponentSwizzle id = VK_COMPONENT_SWIZZLE_IDENTITY;
    VkComponentSwizzle user[4] = {d.swizzle.r, d.swizzle.g, d.swizzle.b, d.swizzle.a};
    int inherent[4] = {0, 1, 2, 3};
    if (d.format == VK_FORMAT_B4G4R4A4_UNORM_PACK16) {
        const bool alphaOne = d.swizzle.a == VK_COMPONENT_SWIZZLE_ONE;
        user[0] = user[1] = user[2] = id;
        user[3] = alphaOne ? VK_COMPONENT_SWIZZLE_ONE : id;
    } else if (d.format == VK_FORMAT_A2R10G10B10_UNORM_PACK32 && (d.usage & VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT) == 0) {
        inherent[0] = 2;
        inherent[2] = 0;
    }
    UINT m[4];
    for (int i = 0; i < 4; ++i) {
        switch (user[i]) {
        case VK_COMPONENT_SWIZZLE_ZERO: m[i] = D3D12_SHADER_COMPONENT_MAPPING_FORCE_VALUE_0; break;
        case VK_COMPONENT_SWIZZLE_ONE: m[i] = D3D12_SHADER_COMPONENT_MAPPING_FORCE_VALUE_1; break;
        case VK_COMPONENT_SWIZZLE_R: m[i] = static_cast<UINT>(inherent[0]); break;
        case VK_COMPONENT_SWIZZLE_G: m[i] = static_cast<UINT>(inherent[1]); break;
        case VK_COMPONENT_SWIZZLE_B: m[i] = static_cast<UINT>(inherent[2]); break;
        case VK_COMPONENT_SWIZZLE_A: m[i] = static_cast<UINT>(inherent[3]); break;
        default: m[i] = static_cast<UINT>(inherent[i]); break;
        }
    }
    return D3D12_ENCODE_SHADER_4_COMPONENT_MAPPING(m[0], m[1], m[2], m[3]);
}

D3D12_RESOURCE_STATES toState(VkImageLayout l) {
    switch (l) {
    case VK_IMAGE_LAYOUT_GENERAL: return D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    case VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL: return D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE;
    case VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL: return D3D12_RESOURCE_STATE_RENDER_TARGET;
    case VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL:
    case VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL: return D3D12_RESOURCE_STATE_DEPTH_WRITE;
    case VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_OPTIMAL:
    case VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL: return D3D12_RESOURCE_STATE_DEPTH_READ | D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE;
    case VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL: return D3D12_RESOURCE_STATE_COPY_SOURCE;
    case VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL: return D3D12_RESOURCE_STATE_COPY_DEST;
    case VK_IMAGE_LAYOUT_PRESENT_SRC_KHR: return D3D12_RESOURCE_STATE_PRESENT;
    default: return D3D12_RESOURCE_STATE_COMMON;
    }
}

D3D12_BLEND toBlend(VkBlendFactor f) {
    switch (f) {
    case VK_BLEND_FACTOR_ZERO: return D3D12_BLEND_ZERO;
    case VK_BLEND_FACTOR_ONE: return D3D12_BLEND_ONE;
    case VK_BLEND_FACTOR_SRC_COLOR: return D3D12_BLEND_SRC_COLOR;
    case VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR: return D3D12_BLEND_INV_SRC_COLOR;
    case VK_BLEND_FACTOR_DST_COLOR: return D3D12_BLEND_DEST_COLOR;
    case VK_BLEND_FACTOR_ONE_MINUS_DST_COLOR: return D3D12_BLEND_INV_DEST_COLOR;
    case VK_BLEND_FACTOR_SRC_ALPHA: return D3D12_BLEND_SRC_ALPHA;
    case VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA: return D3D12_BLEND_INV_SRC_ALPHA;
    case VK_BLEND_FACTOR_DST_ALPHA: return D3D12_BLEND_DEST_ALPHA;
    case VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA: return D3D12_BLEND_INV_DEST_ALPHA;
    case VK_BLEND_FACTOR_CONSTANT_COLOR:
    case VK_BLEND_FACTOR_CONSTANT_ALPHA: return D3D12_BLEND_BLEND_FACTOR;
    case VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_COLOR:
    case VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_ALPHA: return D3D12_BLEND_INV_BLEND_FACTOR;
    case VK_BLEND_FACTOR_SRC_ALPHA_SATURATE: return D3D12_BLEND_SRC_ALPHA_SAT;
    default: return D3D12_BLEND_ONE;
    }
}

D3D12_BLEND_OP toBlendOp(VkBlendOp o) {
    switch (o) {
    case VK_BLEND_OP_SUBTRACT: return D3D12_BLEND_OP_SUBTRACT;
    case VK_BLEND_OP_REVERSE_SUBTRACT: return D3D12_BLEND_OP_REV_SUBTRACT;
    case VK_BLEND_OP_MIN: return D3D12_BLEND_OP_MIN;
    case VK_BLEND_OP_MAX: return D3D12_BLEND_OP_MAX;
    default: return D3D12_BLEND_OP_ADD;
    }
}

D3D12_COMPARISON_FUNC toCompare(VkCompareOp o) {
    switch (o) {
    case VK_COMPARE_OP_NEVER: return D3D12_COMPARISON_FUNC_NEVER;
    case VK_COMPARE_OP_LESS: return D3D12_COMPARISON_FUNC_LESS;
    case VK_COMPARE_OP_EQUAL: return D3D12_COMPARISON_FUNC_EQUAL;
    case VK_COMPARE_OP_LESS_OR_EQUAL: return D3D12_COMPARISON_FUNC_LESS_EQUAL;
    case VK_COMPARE_OP_GREATER: return D3D12_COMPARISON_FUNC_GREATER;
    case VK_COMPARE_OP_NOT_EQUAL: return D3D12_COMPARISON_FUNC_NOT_EQUAL;
    case VK_COMPARE_OP_GREATER_OR_EQUAL: return D3D12_COMPARISON_FUNC_GREATER_EQUAL;
    default: return D3D12_COMPARISON_FUNC_ALWAYS;
    }
}

D3D_PRIMITIVE_TOPOLOGY toTopology(VkPrimitiveTopology t) {
    switch (t) {
    case VK_PRIMITIVE_TOPOLOGY_POINT_LIST: return D3D_PRIMITIVE_TOPOLOGY_POINTLIST;
    case VK_PRIMITIVE_TOPOLOGY_LINE_LIST: return D3D_PRIMITIVE_TOPOLOGY_LINELIST;
    case VK_PRIMITIVE_TOPOLOGY_LINE_STRIP: return D3D_PRIMITIVE_TOPOLOGY_LINESTRIP;
    case VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP: return D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP;
    default: return D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
    }
}

D3D12_PRIMITIVE_TOPOLOGY_TYPE toTopologyType(VkPrimitiveTopology t) {
    switch (t) {
    case VK_PRIMITIVE_TOPOLOGY_POINT_LIST: return D3D12_PRIMITIVE_TOPOLOGY_TYPE_POINT;
    case VK_PRIMITIVE_TOPOLOGY_LINE_LIST:
    case VK_PRIMITIVE_TOPOLOGY_LINE_STRIP: return D3D12_PRIMITIVE_TOPOLOGY_TYPE_LINE;
    default: return D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    }
}

std::uint64_t alignUp(std::uint64_t v, std::uint64_t a) { return (v + a - 1) / a * a; }

std::wstring widen(const char* s) {
    if (s == nullptr) return {};
    std::wstring w;
    for (; *s != '\0'; ++s) w += static_cast<wchar_t>(static_cast<unsigned char>(*s));
    return w;
}

// ---- the Agility SDK (docs/research/hlsl-rhi.md "Agility SDK"; GD3D11's scheme) ------------------------------------------------------------------
// A mod / plugin DLL cannot export D3D12SDKVersion / D3D12SDKPath from the exe, so the SDK's D3D12Core.dll is activated at run time instead:
// D3D12GetInterface(CLSID_D3D12SDKConfiguration) -> ID3D12SDKConfiguration1::CreateDeviceFactory(version, dir) -> devices made through that factory run on the
// SDK's runtime. Without the DLL (or when the factory cannot be made) the system runtime is used.

struct AgilityRuntime {
    ComPtr<ID3D12DeviceFactory> factory;  // null = the system runtime
    std::uint32_t version = 0;            // the D3D12SDKVersion of the loaded core (0 = none)
    std::string dir;                      // the directory that was tried (with a trailing backslash)
};

std::string withTrailingSlash(std::string s) {
    if (!s.empty() && s.back() != '\\' && s.back() != '/') s += '\\';
    return s;
}

// "<directory of the module this code lives in>\d3d12\": the exe for the tools, d3d9.dll (bin\rvk) in the game
std::string defaultAgilityDir() {
    HMODULE self = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, reinterpret_cast<LPCWSTR>(&defaultAgilityDir), &self);
    char path[MAX_PATH]{};
    const DWORD n = GetModuleFileNameA(self, path, MAX_PATH);
    std::string s(path, n < MAX_PATH ? n : 0);
    const auto slash = s.find_last_of("\\/");
    s.erase(slash == std::string::npos ? 0 : slash + 1);
    // the game's d3d9.dll sits in bin, its files in bin\rvk
    if (GetFileAttributesA((s + "d3d12\\D3D12Core.dll").c_str()) == INVALID_FILE_ATTRIBUTES &&
        GetFileAttributesA((s + "rvk\\d3d12\\D3D12Core.dll").c_str()) != INVALID_FILE_ATTRIBUTES) {
        return s + "rvk\\d3d12\\";
    }
    return s + "d3d12\\";
}

bool envFlag(const char* name, bool fallback) {
    char v[16]{};
    const DWORD n = GetEnvironmentVariableA(name, v, sizeof(v));
    if (n == 0 || n >= sizeof(v)) return fallback;
    return v[0] != '0' && v[0] != 'f' && v[0] != 'F';
}

// D3D12SDKVersion, exported by D3D12Core.dll (loaded only to read it); 0 = no usable DLL there
std::uint32_t coreSdkVersion(const std::string& dir) {
    HMODULE core = LoadLibraryExA((dir + "D3D12Core.dll").c_str(), nullptr, LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
    if (core == nullptr) return 0;
    const auto* v = reinterpret_cast<const UINT*>(GetProcAddress(core, "D3D12SDKVersion"));
    const std::uint32_t version = v != nullptr ? *v : 0;
    FreeLibrary(core);
    return version;
}

// once per process (a second factory would leak the first one; ApplyToGlobalState fails once a device exists): the first device's desc decides
const AgilityRuntime& agility(const DeviceDesc& desc) {
    static const AgilityRuntime runtime = [&desc] {
        AgilityRuntime r;
        char env[MAX_PATH]{};
        const DWORD envLen = GetEnvironmentVariableA("RVK_D3D12_SDK_DIR", env, MAX_PATH);
        r.dir = desc.d3d12SdkDir != nullptr && desc.d3d12SdkDir[0] != '\0' ? std::string(desc.d3d12SdkDir)
                : envLen > 0 && envLen < MAX_PATH                           ? std::string(env, envLen)
                                                                            : defaultAgilityDir();
        r.dir = withTrailingSlash(r.dir);
        const std::uint32_t version = coreSdkVersion(r.dir);
        if (version == 0) {
            spdlog::info("d3d12: system runtime (no Agility SDK in {})", r.dir);
            return r;
        }
        PFN_D3D12_GET_INTERFACE getInterface = nullptr;
        if (HMODULE d3d12 = LoadLibraryA("d3d12.dll")) getInterface = reinterpret_cast<PFN_D3D12_GET_INTERFACE>(GetProcAddress(d3d12, "D3D12GetInterface"));
        ComPtr<ID3D12SDKConfiguration1> config;
        if (getInterface == nullptr || FAILED(getInterface(CLSID_D3D12SDKConfiguration, IID_PPV_ARGS(&config)))) {
            spdlog::warn("d3d12: system runtime (the Agility SDK {} in {} needs ID3D12SDKConfiguration1: Windows 10 1809+ with a current d3d12.dll)", version, r.dir);
            return r;
        }
        ComPtr<ID3D12DeviceFactory> factory;
        const HRESULT hr = config->CreateDeviceFactory(version, r.dir.c_str(), IID_PPV_ARGS(&factory));
        if (FAILED(hr)) {
            spdlog::warn("d3d12: system runtime (CreateDeviceFactory({}, {}) failed 0x{:08x})", version, r.dir, static_cast<unsigned>(hr));
            return r;
        }
        // CreateDeviceFactory only redirects the devices made through the factory; the process-global runtime (and with it the exported free functions,
        // D3D12SerializeRootSignature among them) stays on the system's d3d12.dll, whose serializer may reject the SM 6.6 root signature flags
        // (CBV_SRV_UAV_HEAP_DIRECTLY_INDEXED). ApplyToGlobalState points the global state at the loaded core. Must happen before any device exists.
        const HRESULT apply = factory->ApplyToGlobalState();
        if (FAILED(apply)) spdlog::warn("d3d12: ApplyToGlobalState failed (0x{:08x}): the root signature serializer stays on the system runtime", static_cast<unsigned>(apply));
        r.factory = std::move(factory);
        r.version = version;
        spdlog::info("d3d12: Agility SDK {} from {}", version, r.dir);
        return r;
    }();
    return runtime;
}

class D3D12DeviceImpl;

// 14.22: a BLAS's range of the device's acceleration structure arena (D3D12DeviceImpl::asAlloc)
struct AsSpan {
    std::int32_t chunk = -1;  // -1 = none (a dedicated resource)
    std::uint64_t offset = 0, size = 0;
};

class D3DBuffer final : public Buffer {
public:
    D3DBuffer(D3D12DeviceImpl& dev, const BufferDesc& desc) : dev_(dev), desc_(desc) {}
    ~D3DBuffer() override;
    bool create();
    const BufferDesc& desc() const override { return desc_; }
    void* map() override { return mapped_; }
    void invalidate() override {}  // (upload / readback heaps are coherent)
    void flush() override {}
    std::uint32_t heapIndex() const override { return slot_; }
    std::uint32_t heapIndexRw() const override { return slotRw_; }
    std::array<std::uint32_t, 2> shaderRef(std::uint64_t offset) const override { return {slot_, static_cast<std::uint32_t>(offset)}; }
    std::array<std::uint32_t, 2> shaderRefRw(std::uint64_t offset) const override { return {slotRw_, static_cast<std::uint32_t>(offset)}; }
    ID3D12Resource* resource() const { return resource_.Get(); }
    std::uint64_t gpuAddress() const override { return resource_->GetGPUVirtualAddress(); }
    D3D12_RESOURCE_STATES state = D3D12_RESOURCE_STATE_COMMON;
    // the mesh arena's blocks: device memory that lives in GENERIC_READ (draws read it from any thread without barriers); the copy helpers bracket
    // their writes with COPY_DEST; no UAV
    bool arena = false;
    // the capture pool's chunks: a GPU upload heap (device memory the CPU maps: headers are written through map()) with UAV access
    bool gpuUpload = false;

private:
    D3D12DeviceImpl& dev_;
    BufferDesc desc_;
    ComPtr<ID3D12Resource> resource_;
    void* mapped_ = nullptr;
    std::uint32_t slot_ = 0;    // the ByteAddressBuffer SRV
    std::uint32_t slotRw_ = 0;  // the RWByteAddressBuffer UAV (device memory)
};

class D3DTexture final : public Texture {
public:
    D3DTexture(D3D12DeviceImpl& dev, const TextureDesc& desc) : dev_(dev), desc_(desc) {}
    ~D3DTexture() override;
    bool create();
    const TextureDesc& desc() const override { return desc_; }
    std::uint32_t heapSampled() const override { return slot_; }
    std::uint32_t heapSampledLayer(std::uint32_t layer) override;
    ID3D12Resource* resource() const { return resource_.Get(); }
    VkImageLayout layout() const override { return split ? VK_IMAGE_LAYOUT_MAX_ENUM : lastLayout; }
    VkImageLayout lastLayout = VK_IMAGE_LAYOUT_UNDEFINED;  // the layout of the last transition (the RHI's vocabulary for `state`)
    D3D12_RESOURCE_STATES state = D3D12_RESOURCE_STATE_COMMON;
    // the state per mip level (all layers alike) while the levels differ (CommandList::transitionMips); `state` is the whole texture's otherwise
    std::array<D3D12_RESOURCE_STATES, 16> mipStates{};
    bool split = false;
    // a swapchain back buffer (owned by the swapchain; no SRV)
    void adopt(ComPtr<ID3D12Resource> r) { resource_ = std::move(r); }
    void release() { resource_.Reset(); }
    bool isArray() const { return !desc_.cube && desc_.type == VK_IMAGE_TYPE_2D && desc_.layers > 1; }
    // D3D12 subresource index of (mip, layer)
    UINT subresource(Subresource s) const { return s.mip + s.layer * desc_.mips; }

private:
    D3D12DeviceImpl& dev_;
    TextureDesc desc_;
    ComPtr<ID3D12Resource> resource_;
    std::uint32_t slot_ = 0;
    std::vector<std::uint32_t> layerSlots_;  // 2D arrays: the Texture2D SRV of each layer, created on demand
};

class D3DPipeline final : public Pipeline {
public:
    ComPtr<ID3D12PipelineState> pso;
    bool compute = false;
    D3D_PRIMITIVE_TOPOLOGY topology = D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
};

// A BLAS or TLAS: the structure's storage (always in RAYTRACING_ACCELERATION_STRUCTURE state: such a resource cannot change state). A TLAS also owns its
// build scratch (UNORDERED_ACCESS) and a heap SRV (tlasSlot()); BLAS builds take their scratch from the device's shared pool.
class D3DAccel final : public AccelerationStructure {
public:
    D3DAccel(D3D12DeviceImpl& dev, bool tlas) : dev_(dev), tlas_(tlas) {}
    ~D3DAccel() override;
    bool createBlas(const BlasDesc& desc);
    bool createTlas(const TlasDesc& desc);
    bool createStorage(std::uint64_t bytes, const char* name);
    bool isTlas() const override { return tlas_; }
    std::uint64_t address() const override { return (storage_ ? storage_->GetGPUVirtualAddress() : arenaBuffer_->GetGPUVirtualAddress()) + span_.offset; }
    std::uint32_t tlasSlot() const override { return slot_; }
    std::uint64_t storageBytes() const override { return storageBytes_; }
    std::uint64_t scratchAddress() const { return scratch_->GetGPUVirtualAddress(); }
    const std::vector<TriangleGeometry>& geometries() const { return geometries_; }
    std::uint32_t maxInstances() const { return maxInstances_; }
    D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAGS flags() const { return flags_; }
    bool allowUpdate() const { return allowUpdate_; }
    std::uint64_t scratchNeed(bool update) const { return update ? updateScratch_ : buildScratch_; }

private:
    bool allocate(const D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS& inputs, bool scratch);

    D3D12DeviceImpl& dev_;
    bool tlas_;
    std::vector<TriangleGeometry> geometries_;
    std::uint32_t maxInstances_ = 0;
    D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAGS flags_ = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_NONE;
    bool allowUpdate_ = false;
    std::uint64_t buildScratch_ = 0, updateScratch_ = 0, storageBytes_ = 0;
    ComPtr<ID3D12Resource> storage_, scratch_;
    std::uint32_t slot_ = 0;
    // 14.22: a BLAS in the device's arena (storage_ null): the chunk's buffer + the span
    ID3D12Resource* arenaBuffer_ = nullptr;
    AsSpan span_;
    bool storageFor(std::uint64_t bytes, const wchar_t* name);
};

// compacted sizes: UINT64s written by EmitRaytracingAccelerationStructurePostbuildInfo into a UAV buffer, copied to a readback buffer
class D3DSizeQuery final : public SizeQuery {
public:
    D3DSizeQuery(D3D12DeviceImpl& dev, std::uint32_t capacity) : dev_(dev), capacity_(capacity), written_(capacity, false) {}
    bool create();
    std::uint32_t capacity() const override { return capacity_; }
    bool read(std::uint32_t first, std::uint32_t count, std::uint64_t* sizes) override {
        bool all = true;
        const auto* p = static_cast<const std::uint64_t*>(readback_->map());
        for (std::uint32_t i = 0; i < count; ++i) {
            const std::uint32_t e = first + i;
            sizes[i] = e < capacity_ && written_[e] ? p[e] : 0;
            all = all && e < capacity_ && written_[e];
        }
        return all;
    }
    ID3D12Resource* gpu() const { return gpu_.Get(); }
    D3DBuffer& readbackBuffer() { return static_cast<D3DBuffer&>(*readback_); }
    bool inUav = false;  // moved to UNORDERED_ACCESS (where it stays between uses)
    void markWritten(std::uint32_t first, std::uint32_t count) {
        for (std::uint32_t i = 0; i < count && first + i < capacity_; ++i) written_[first + i] = true;
    }

private:
    D3D12DeviceImpl& dev_;
    std::uint32_t capacity_;
    std::vector<bool> written_;
    ComPtr<ID3D12Resource> gpu_;
    std::unique_ptr<Buffer> readback_;
};

class D3DCommandList;

class D3D12DeviceImpl final : public GameDevice {
    // Declared first = destroyed last: with the debug layer, the D3D12 objects still alive after every other member was released are listed by name
    // (a leak at the device's teardown; the layer's own report at process exit has no names). The message callback is unregistered here too: a
    // device kept alive by a leak would otherwise call into the destroyed object.
    struct Teardown {
        ComPtr<ID3D12DebugDevice> debug;
        ComPtr<ID3D12InfoQueue1> queue;
        DWORD cookie = 0;
        ~Teardown() {
            if (debug) {
                spdlog::info("d3d12: device teardown: objects still alive (the device's Refcount 2 = this report's debug + info queue references):");
                debug->ReportLiveDeviceObjects(D3D12_RLDO_DETAIL | D3D12_RLDO_IGNORE_INTERNAL);
            }
            if (queue && cookie != 0) queue->UnregisterMessageCallback(cookie);
        }
    } teardown_;

public:
    ~D3D12DeviceImpl() override;
#include "d3d12_game_decl.inc"
    bool gpuUploadHeapSupported() const { return gpuUploadHeap_; }
    friend struct D3D12TestAccess;


    bool create(const DeviceDesc& desc) {
        const AgilityRuntime& sdk = agility(desc);
        // the debug layer, GPU based validation and DRED are configured before the device exists: through the factory's configuration interfaces when
        // there is one (a factory-made device ignores the global D3D12GetDebugInterface state), else through the global ones
        auto config = [&sdk](REFCLSID clsid, REFIID iid, void** out) -> HRESULT {
            if (sdk.factory) return sdk.factory->GetConfigurationInterface(clsid, iid, out);
            return D3D12GetDebugInterface(iid, out);  // (the global state answers the debug and the DRED interface by IID)
        };
        UINT factoryFlags = 0;
        // debug.d3d12_messages / DeviceDesc::messages: the layer's messages without GPU based validation (which runs at a fraction of the speed)
        const bool messages = desc.messages || envFlag("RVK_D3D12_MESSAGES", false);
        const bool layer = desc.validation || messages;
        if (layer) {
            ComPtr<ID3D12Debug> debug;
            if (SUCCEEDED(config(CLSID_D3D12Debug, IID_PPV_ARGS(&debug)))) {
                debug->EnableDebugLayer();
                factoryFlags |= DXGI_CREATE_FACTORY_DEBUG;
                ComPtr<ID3D12Debug1> debug1;
                if (desc.validation && SUCCEEDED(debug.As(&debug1))) {
                    debug1->SetEnableGPUBasedValidation(GetEnvironmentVariableA("RVK_NO_GBV", nullptr, 0) == 0);  // (RVK_NO_GBV: the layer without the slow GPU-based validation)
                }
            } else {
                spdlog::warn("d3d12: the debug layer is not available (d3d12SDKLayers.dll missing{})", sdk.factory ? " from the Agility SDK directory" : "");
            }
        }
        // DRED: breadcrumbs + page fault data for a device removal. On by default, also without validation (both cost next to nothing: the breadcrumbs are
        // a few writes per command list op; docs/research/dx12-game-port.md): RVK_DRED=0 / DeviceDesc::dred = false turns it off.
        if (desc.validation || (desc.dred && envFlag("RVK_DRED", true))) {
            ComPtr<ID3D12DeviceRemovedExtendedDataSettings1> dred;
            if (SUCCEEDED(config(CLSID_D3D12DeviceRemovedExtendedData, IID_PPV_ARGS(&dred)))) {
                dred->SetAutoBreadcrumbsEnablement(D3D12_DRED_ENABLEMENT_FORCED_ON);
                dred->SetBreadcrumbContextEnablement(D3D12_DRED_ENABLEMENT_FORCED_ON);
                dred->SetPageFaultEnablement(D3D12_DRED_ENABLEMENT_FORCED_ON);
                dredOn_ = true;
            }
        }
        // (creates through the Agility factory when there is one, else the system runtime; falls back to the latter if the factory refuses)
        auto makeDevice = [&](IUnknown* adapter, REFIID iid, void** out, bool* viaFactory) -> HRESULT {
            if (sdk.factory) {
                const HRESULT hr = sdk.factory->CreateDevice(adapter, D3D_FEATURE_LEVEL_12_0, iid, out);
                if (SUCCEEDED(hr)) {
                    if (viaFactory != nullptr) *viaFactory = true;
                    return hr;
                }
            }
            if (viaFactory != nullptr) *viaFactory = false;
            return D3D12CreateDevice(adapter, D3D_FEATURE_LEVEL_12_0, iid, out);
        };
        RVK_HR(CreateDXGIFactory2(factoryFlags, IID_PPV_ARGS(&factory_)), "CreateDXGIFactory2");
        ComPtr<IDXGIFactory6> f6;
        factory_.As(&f6);
        ComPtr<IDXGIAdapter1> adapter;
        for (UINT i = 0;; ++i) {
            ComPtr<IDXGIAdapter1> a;
            const HRESULT hr = f6 ? f6->EnumAdapterByGpuPreference(i, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE, IID_PPV_ARGS(&a))
                                  : factory_->EnumAdapters1(i, &a);
            if (FAILED(hr)) break;
            DXGI_ADAPTER_DESC1 ad{};
            a->GetDesc1(&ad);
            if ((ad.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) != 0) continue;
            if (desc.adapter >= 0 && static_cast<UINT>(desc.adapter) != i) continue;
            if (SUCCEEDED(makeDevice(a.Get(), __uuidof(ID3D12Device), nullptr, nullptr))) {
                adapter = a;
                break;
            }
        }
        if (!adapter) {
            spdlog::error("d3d12: no adapter with feature level 12_0");
            return false;
        }
        DXGI_ADAPTER_DESC1 ad{};
        adapter->GetDesc1(&ad);
        {
            std::string name;
            for (const wchar_t* c = ad.Description; *c != L'\0'; ++c) name += static_cast<char>(*c < 128 ? *c : '?');
            caps_.adapterName = name;
        }
        adapter.As(&adapter3_);
        vramDedicated_ = ad.DedicatedVideoMemory;
        vramShared_ = ad.SharedSystemMemory;
        bool viaFactory = false;
        RVK_HR(makeDevice(adapter.Get(), IID_PPV_ARGS(&device_), &viaFactory), "D3D12CreateDevice");
        gpuevents::record(gpuevents::Kind::DeviceCreated, "d3d12");
        caps_.agilitySdkVersion = viaFactory ? sdk.version : 0;
        spdlog::info("d3d12: device from {}", viaFactory ? std::format("the Agility SDK {} factory", sdk.version) : std::string("the system runtime"));
        if (layer) {
            // the debug layer's messages go to the log and count as errors (a debugger's output window is not needed)
            ComPtr<ID3D12InfoQueue1> iq;
            if (SUCCEEDED(device_.As(&iq))) {
                // Agility SDK 620's layer warns on every ID3D12Device::CheckFeatureSupport (message 989: use ID3D12Device16::CheckFeatureSupport1, which the
                // 1.721 headers do not declare yet): denied, the capability queries are made once at device creation
                D3D12_MESSAGE_ID deny[] = {static_cast<D3D12_MESSAGE_ID>(989)};
                D3D12_INFO_QUEUE_FILTER filter{};
                filter.DenyList.NumIDs = 1;
                filter.DenyList.pIDList = deny;
                iq->PushStorageFilter(&filter);
                DWORD cookie = 0;
                iq->RegisterMessageCallback(
                    [](D3D12_MESSAGE_CATEGORY, D3D12_MESSAGE_SEVERITY severity, D3D12_MESSAGE_ID id, LPCSTR text, void* self) {
                        auto* d = static_cast<D3D12DeviceImpl*>(self);
                        if (severity == D3D12_MESSAGE_SEVERITY_CORRUPTION || severity == D3D12_MESSAGE_SEVERITY_ERROR) {
                            ++d->errors_;
                            spdlog::error("d3d12: validation {}: {}", static_cast<int>(id), text);
                        } else if (severity == D3D12_MESSAGE_SEVERITY_WARNING && id != 820 && id != 821 && id != 1328 && id != 245 && id != 679) {  // (clear value / initial state notes)
                            spdlog::warn("d3d12: validation {}: {}", static_cast<int>(id), text);
                        }
                    },
                    D3D12_MESSAGE_CALLBACK_FLAG_NONE, this, &cookie);
                teardown_.queue = iq;
                teardown_.cookie = cookie;
            }
            device_.As(&teardown_.debug);
        }
        queryRayTracingCaps(desc);
        D3D12_FEATURE_DATA_SHADER_MODEL sm{D3D_SHADER_MODEL_6_6};
        if (FAILED(device_->CheckFeatureSupport(D3D12_FEATURE_SHADER_MODEL, &sm, sizeof(sm))) || sm.HighestShaderModel < D3D_SHADER_MODEL_6_6) {
            spdlog::error("d3d12: '{}' does not support Shader Model 6.6 (bindless is required)", caps_.adapterName);
            return false;
        }
        D3D12_FEATURE_DATA_D3D12_OPTIONS o{};
        device_->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS, &o, sizeof(o));
        if (o.ResourceBindingTier < D3D12_RESOURCE_BINDING_TIER_3) {
            spdlog::error("d3d12: '{}' offers resource binding tier {} (3 is required)", caps_.adapterName, static_cast<int>(o.ResourceBindingTier));
            return false;
        }
        // inline ray queries (DXR tier 1.1) when asked for
        D3D12_FEATURE_DATA_D3D12_OPTIONS4 o4{};
        caps_.shaderFloat16 = SUCCEEDED(device_->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS4, &o4, sizeof(o4))) && o4.Native16BitShaderOpsSupported;
        D3D12_FEATURE_DATA_D3D12_OPTIONS5 o5{};
        const bool o5ok = SUCCEEDED(device_->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS5, &o5, sizeof(o5)));
        dxr11_ = o5ok && o5.RaytracingTier >= D3D12_RAYTRACING_TIER_1_1;
        if (desc.rayTracing && dxr11_ && SUCCEEDED(device_.As(&device5_))) {
            caps_.rayQuery = true;
        }
        // (14.22: the BLAS arena's first chunk now, not inside the first frame that builds the scene)
        if (caps_.rayQuery && !asArenaGrow()) spdlog::warn("d3d12: the acceleration structure arena could not be created (BLAS storage stays one buffer each)");
        D3D12_COMMAND_QUEUE_DESC qd{D3D12_COMMAND_LIST_TYPE_DIRECT, D3D12_COMMAND_QUEUE_PRIORITY_NORMAL, D3D12_COMMAND_QUEUE_FLAG_NONE, 0};
        RVK_HR(device_->CreateCommandQueue(&qd, IID_PPV_ARGS(&queue_)), "CreateCommandQueue");
        RVK_HR(device_->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator_)), "CreateCommandAllocator");
        RVK_HR(device_->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator_.Get(), nullptr, IID_PPV_ARGS(&list_)), "CreateCommandList");
        list_->Close();
        RVK_HR(device_->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence_)), "CreateFence");
        fenceEvent_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);

        const auto makeHeap = [&](D3D12_DESCRIPTOR_HEAP_TYPE type, UINT n, bool visible, ComPtr<ID3D12DescriptorHeap>& out) {
            D3D12_DESCRIPTOR_HEAP_DESC hd{type, n, visible ? D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE : D3D12_DESCRIPTOR_HEAP_FLAG_NONE, 0};
            return SUCCEEDED(device_->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&out)));
        };
        if (!makeHeap(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, kHeapSize, true, srvHeap_) ||
            !makeHeap(D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER, kSamplerHeapSize, true, samplerHeap_) ||
            !makeHeap(D3D12_DESCRIPTOR_HEAP_TYPE_RTV, kRtvCount, false, rtvHeap_) || !makeHeap(D3D12_DESCRIPTOR_HEAP_TYPE_DSV, kDsvCount, false, dsvHeap_) ||
            !makeHeap(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 1, false, uavClearHeap_)) {
            spdlog::error("d3d12: descriptor heaps");
            return false;
        }
        // (names: the debug layer's live-object report at the teardown names what leaked)
        queue_->SetName(L"rhi queue");
        allocator_->SetName(L"rhi legacy allocator");
        list_->SetName(L"rhi legacy list");
        fence_->SetName(L"rhi legacy fence");
        srvHeap_->SetName(L"rhi srv heap");
        samplerHeap_->SetName(L"rhi sampler heap");
        rtvHeap_->SetName(L"rhi rtv heap");
        dsvHeap_->SetName(L"rhi dsv heap");
        uavClearHeap_->SetName(L"rhi uav clear heap");
        srvInc_ = device_->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        samplerInc_ = device_->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER);
        rtvInc_ = device_->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
        dsvInc_ = device_->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_DSV);
        srvAlloc_.next = kReservedSlots;
        srvAlloc_.limit = kHeapSize - kUavRing - kViewRing;
        srvAlloc_.reserveLow(kLowSlotBegin, kLowSlotEnd);
        samplerAlloc_.limit = kSamplerHeapSize;
        // entry 0 of the texture heap: a null SRV until the default texture below exists
        {
            D3D12_SHADER_RESOURCE_VIEW_DESC sd{};
            sd.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
            sd.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
            sd.Shader4ComponentMapping = D3D12_ENCODE_SHADER_4_COMPONENT_MAPPING(0, 0, 0, D3D12_SHADER_COMPONENT_MAPPING_FORCE_VALUE_1);
            sd.Texture2D.MipLevels = 1;
            device_->CreateShaderResourceView(nullptr, &sd, srvCpu(0));
            D3D12_SAMPLER_DESC sm0{};
            sm0.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
            sm0.AddressU = sm0.AddressV = sm0.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
            sm0.MaxLOD = D3D12_FLOAT32_MAX;
            device_->CreateSampler(&sm0, samplerCpu(0));
        }
        if (!createRootSignature()) return false;
        // Entry 0 must READ (0, 0, 0, 1) like the Vulkan side's 1x1 default texture: a null descriptor does not (AMD returns (0, 0, 0, 0) whatever the component
        // mapping says: a shadow mask sampled from "no texture" shadowed everything). So entry 0 becomes a copy of the SRV of a real 1x1 opaque black texture.
        {
            defaultTexture_ = createTexture({VK_IMAGE_TYPE_2D, VK_FORMAT_R8G8B8A8_UNORM, {1, 1, 1}, 1, 1, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, "rhi default texture"});
            auto texel = createBuffer({512, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, Memory::Upload, "rhi default texel"});
            if (!defaultTexture_ || !texel) {
                spdlog::error("d3d12: default texture");
                return false;
            }
            const std::uint8_t black[4] = {0, 0, 0, 255};
            std::memcpy(texel->map(), black, 4);
            submitAndWait([&](CommandList& cl) {
                cl.transition(*defaultTexture_, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
                cl.copyBufferToTexture(*defaultTexture_, *texel, 0);
                cl.transition(*defaultTexture_, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
            });
            D3D12_SHADER_RESOURCE_VIEW_DESC sd{};  // (the shader-visible heap is write only: no CopyDescriptors from it, a view of its own)
            sd.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
            sd.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
            sd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            sd.Texture2D.MipLevels = 1;
            device_->CreateShaderResourceView(static_cast<D3DTexture&>(*defaultTexture_).resource(), &sd, srvCpu(0));
        }
        caps_.backend = Backend::D3D12;
        caps_.bindless = true;
        caps_.heapSize = kHeapSize;
        caps_.drawIndirectCount = true;  // ExecuteIndirect with a count buffer
        caps_.multiDrawIndirect = true;  // ExecuteIndirect with any command count; StartInstanceLocation of the records is honoured
        caps_.drawIndirectFirstInstance = true;
        caps_.textureBlit = false;       // no scaling copy (a mip chain would need a compute / graphics pass of the backend's own)
        surveyFeatures();
        return true;
    }

    // Information only: what the GPU / runtime offers beyond the floor (each CheckFeatureSupport; a struct the runtime does not know = unknown).
    void surveyFeatures() {
        auto& out = caps_.featureSurvey;
        const auto add = [&](std::string name, std::string value) {
            spdlog::info("caps: d3d12 {}: {}", name, value);
            out.emplace_back(std::move(name), std::move(value));
        };
        const auto yn = [](bool b) { return b ? "supported" : "unsupported"; };
        const auto unknown = "unknown (the runtime does not know this feature struct: older D3D12 runtime / Agility SDK)";
        const auto query = [&](D3D12_FEATURE f, auto& data) { return SUCCEEDED(device_->CheckFeatureSupport(f, &data, sizeof(data))); };
        add("Agility SDK", caps_.agilitySdkVersion != 0 ? std::format("{} (D3D12Core from the SDK dir)", caps_.agilitySdkVersion) : "none (system D3D12 runtime)");
        D3D12_FEATURE_DATA_SHADER_MODEL sm{D3D_SHADER_MODEL_6_9};
        if (!query(D3D12_FEATURE_SHADER_MODEL, sm)) {
            sm.HighestShaderModel = D3D_SHADER_MODEL_6_6;
            query(D3D12_FEATURE_SHADER_MODEL, sm);
        }
        add("highest shader model", std::format("{}.{}", (static_cast<int>(sm.HighestShaderModel) >> 4) & 0xF, static_cast<int>(sm.HighestShaderModel) & 0xF));
        D3D12_FEATURE_DATA_D3D12_OPTIONS1 o1{};
        if (query(D3D12_FEATURE_D3D12_OPTIONS1, o1)) {
            add("wave size", std::format("lanes {} - {}, wave ops {}", o1.WaveLaneCountMin, o1.WaveLaneCountMax, yn(o1.WaveOps)));
        } else {
            add("wave size", unknown);
        }
        D3D12_FEATURE_DATA_D3D12_OPTIONS5 o5{};
        if (query(D3D12_FEATURE_D3D12_OPTIONS5, o5)) {
            const int t = o5.RaytracingTier;
            add("DXR tier", t >= D3D12_RAYTRACING_TIER_1_2 ? "1.2 (opacity micromaps + shader execution reordering are part of it)"
                            : t >= D3D12_RAYTRACING_TIER_1_1 ? "1.1 (inline ray queries; no OMM / SER)"
                            : t >= D3D12_RAYTRACING_TIER_1_0 ? "1.0"
                                                             : "unsupported");
        } else {
            add("DXR tier", unknown);
        }
        D3D12_FEATURE_DATA_D3D12_OPTIONS6 o6{};
        add("variable rate shading", query(D3D12_FEATURE_D3D12_OPTIONS6, o6)
                                         ? std::format("tier {} (tile size {})", o6.VariableShadingRateTier == D3D12_VARIABLE_SHADING_RATE_TIER_2 ? "2" : o6.VariableShadingRateTier == D3D12_VARIABLE_SHADING_RATE_TIER_1 ? "1" : "none",
                                                       o6.ShadingRateImageTileSize)
                                         : unknown);
        D3D12_FEATURE_DATA_D3D12_OPTIONS7 o7{};
        if (query(D3D12_FEATURE_D3D12_OPTIONS7, o7)) {
            add("mesh shaders", o7.MeshShaderTier >= D3D12_MESH_SHADER_TIER_1 ? "tier 1" : "unsupported");
            add("sampler feedback", o7.SamplerFeedbackTier >= D3D12_SAMPLER_FEEDBACK_TIER_1_0 ? "tier 1.0" : o7.SamplerFeedbackTier != 0 ? "tier 0.9" : "unsupported");
        } else {
            add("mesh shaders", unknown);
            add("sampler feedback", unknown);
        }
        D3D12_FEATURE_DATA_D3D12_OPTIONS12 o12{};
        if (query(D3D12_FEATURE_D3D12_OPTIONS12, o12)) {
            add("enhanced barriers", yn(o12.EnhancedBarriersSupported));
            add("relaxed format casting", yn(o12.RelaxedFormatCastingSupported));
        } else {
            add("enhanced barriers", unknown);
            add("relaxed format casting", unknown);
        }
        D3D12_FEATURE_DATA_D3D12_OPTIONS14 o14{};
        add("advanced texture ops (OPTIONS14)", query(D3D12_FEATURE_D3D12_OPTIONS14, o14) ? yn(o14.AdvancedTextureOpsSupported) : unknown);
        D3D12_FEATURE_DATA_D3D12_OPTIONS16 o16{};
        if (query(D3D12_FEATURE_D3D12_OPTIONS16, o16)) {
            add("GPU upload heap", yn(o16.GPUUploadHeapSupported));
            add("resizable BAR", o16.GPUUploadHeapSupported ? "yes (implied by the GPU upload heap: CPU-visible VRAM)" : "no / not exposed (no GPU upload heap)");
        } else {
            add("GPU upload heap", unknown);
            add("resizable BAR", unknown);
        }
        D3D12_FEATURE_DATA_D3D12_OPTIONS21 o21{};
        if (query(D3D12_FEATURE_D3D12_OPTIONS21, o21)) {
            add("work graphs", o21.WorkGraphsTier >= D3D12_WORK_GRAPHS_TIER_1_0 ? "tier 1.0" : "unsupported");
            add("ExecuteIndirect", o21.ExecuteIndirectTier >= D3D12_EXECUTE_INDIRECT_TIER_1_1 ? "tier 1.1 (indirect draw / dispatch ids, incrementing constants)" : "tier 1.0");
        } else {
            add("work graphs", unknown);
            add("ExecuteIndirect", unknown);
        }
        D3D12_FEATURE_DATA_D3D12_OPTIONS22 o22{};
        add("shader execution reordering (OPTIONS22)",
            query(D3D12_FEATURE_D3D12_OPTIONS22, o22) ? std::format("{}", o22.ShaderExecutionReorderingActuallyReorders ? "hardware reorders" : "API only, the hardware does not reorder") : unknown);
        add("opacity micromap tier", "no separate tier query in the vendored headers (Agility SDK 1.619): follows the DXR tier");
    }

    const DeviceCaps& caps() const override { return caps_; }
    std::uint32_t validationErrors() const override { return errors_.load(); }

    std::unique_ptr<Buffer> createBuffer(const BufferDesc& desc) override {
        auto b = std::make_unique<D3DBuffer>(*this, desc);
        if (!b->create()) return nullptr;
        return b;
    }
    std::unique_ptr<Texture> createTexture(const TextureDesc& desc) override {
        auto t = std::make_unique<D3DTexture>(*this, desc);
        if (!t->create()) return nullptr;
        return t;
    }
    SamplerIndex createSampler(const SamplerDesc& desc) override {
        const std::uint32_t i = samplerAlloc_.alloc();
        if (i == 0) return 0;
        D3D12_SAMPLER_DESC sd{};
        sd.Filter = desc.filter == VK_FILTER_NEAREST ? D3D12_FILTER_MIN_MAG_MIP_POINT : D3D12_FILTER_MIN_MAG_MIP_LINEAR;
        const auto mode = [](VkSamplerAddressMode m) {
            switch (m) {
            case VK_SAMPLER_ADDRESS_MODE_REPEAT: return D3D12_TEXTURE_ADDRESS_MODE_WRAP;
            case VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT: return D3D12_TEXTURE_ADDRESS_MODE_MIRROR;
            default: return D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
            }
        };
        sd.AddressU = sd.AddressV = sd.AddressW = mode(desc.address);
        sd.MaxLOD = D3D12_FLOAT32_MAX;
        sd.MaxAnisotropy = 1;
        if (desc.maxAnisotropy > 1.0f && desc.filter == VK_FILTER_LINEAR) {
            sd.Filter = D3D12_FILTER_ANISOTROPIC;
            sd.MaxAnisotropy = static_cast<UINT>(std::min(desc.maxAnisotropy, 16.0f));
        }
        device_->CreateSampler(&sd, samplerCpu(i));
        return i;
    }

    std::unique_ptr<Pipeline> createComputePipeline(const ComputePipelineDesc& desc) override {
        if (desc.cs.dxil.empty()) {
            spdlog::error("d3d12: compute pipeline '{}' has no DXIL", desc.name != nullptr ? desc.name : "");
            return nullptr;
        }
        D3D12_COMPUTE_PIPELINE_STATE_DESC pd{};
        pd.pRootSignature = rootSignature_.Get();
        pd.CS = {desc.cs.dxil.data(), desc.cs.dxil.size()};
        auto p = std::make_unique<D3DPipeline>();
        p->compute = true;
        const HRESULT hr = device_->CreateComputePipelineState(&pd, IID_PPV_ARGS(&p->pso));
        if (FAILED(hr)) {
            spdlog::error("d3d12: CreateComputePipelineState '{}' failed (0x{:08x})", desc.name != nullptr ? desc.name : "", static_cast<unsigned>(hr));
            return nullptr;
        }
        if (desc.name != nullptr) p->pso->SetName(widen(desc.name).c_str());
        return p;
    }

    std::unique_ptr<Pipeline> createGraphicsPipeline(const GraphicsPipelineDesc& desc) override {
        if (desc.vs.dxil.empty() || desc.ps.dxil.empty()) {
            spdlog::error("d3d12: graphics pipeline '{}' has no DXIL", desc.name != nullptr ? desc.name : "");
            return nullptr;
        }
        D3D12_GRAPHICS_PIPELINE_STATE_DESC pd{};
        pd.pRootSignature = rootSignature_.Get();
        pd.VS = {desc.vs.dxil.data(), desc.vs.dxil.size()};
        pd.PS = {desc.ps.dxil.data(), desc.ps.dxil.size()};
        pd.SampleMask = 0xFFFFFFFFu;
        pd.SampleDesc = {1, 0};
        pd.PrimitiveTopologyType = toTopologyType(desc.topology);
        // vertex input: attribute location N is the semantic ATTR<N> (the translator's convention); per-instance bindings step once per instance
        std::vector<D3D12_INPUT_ELEMENT_DESC> elements;
        for (const VkVertexInputAttributeDescription& a : desc.vertexAttributes) {
            const bool perInstance = std::any_of(desc.vertexBindings.begin(), desc.vertexBindings.end(), [&](const VkVertexInputBindingDescription& b) {
                return b.binding == a.binding && b.inputRate == VK_VERTEX_INPUT_RATE_INSTANCE;
            });
            D3D12_INPUT_ELEMENT_DESC e{};
            e.SemanticName = "ATTR";
            e.SemanticIndex = a.location;
            e.Format = toDxgiVertex(a.format);
            e.InputSlot = a.binding;
            e.AlignedByteOffset = a.offset;
            e.InputSlotClass = perInstance ? D3D12_INPUT_CLASSIFICATION_PER_INSTANCE_DATA : D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA;
            e.InstanceDataStepRate = perInstance ? 1 : 0;
            elements.push_back(e);
        }
        pd.InputLayout = {elements.data(), static_cast<UINT>(elements.size())};
        for (std::uint32_t i = 0; i < desc.colorFormats.size(); ++i) {
            if (desc.colorFormats[i] == VK_FORMAT_UNDEFINED) continue;
            pd.RTVFormats[i] = toDxgi(desc.colorFormats[i]);
            pd.NumRenderTargets = i + 1;
            auto& rt = pd.BlendState.RenderTarget[i];
            rt.BlendEnable = desc.blend.blendEnable;
            rt.SrcBlend = toBlend(desc.blend.srcColorBlendFactor);
            rt.DestBlend = toBlend(desc.blend.dstColorBlendFactor);
            rt.BlendOp = toBlendOp(desc.blend.colorBlendOp);
            rt.SrcBlendAlpha = toBlend(desc.blend.srcAlphaBlendFactor);
            rt.DestBlendAlpha = toBlend(desc.blend.dstAlphaBlendFactor);
            rt.BlendOpAlpha = toBlendOp(desc.blend.alphaBlendOp);
            rt.RenderTargetWriteMask = desc.blend.colorWriteMask != 0 ? static_cast<UINT8>(desc.blend.colorWriteMask) : 0xF;
        }
        pd.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
        pd.RasterizerState.CullMode = (desc.cull & VK_CULL_MODE_FRONT_BIT) != 0 && (desc.cull & VK_CULL_MODE_BACK_BIT) == 0 ? D3D12_CULL_MODE_FRONT
                                      : (desc.cull & VK_CULL_MODE_BACK_BIT) != 0                                        ? D3D12_CULL_MODE_BACK
                                                                                                                       : D3D12_CULL_MODE_NONE;
        pd.RasterizerState.FrontCounterClockwise = desc.frontFace == VK_FRONT_FACE_COUNTER_CLOCKWISE;
        pd.RasterizerState.DepthClipEnable = TRUE;
        pd.DepthStencilState.DepthEnable = desc.depthTest;
        pd.DepthStencilState.DepthWriteMask = desc.depthWrite ? D3D12_DEPTH_WRITE_MASK_ALL : D3D12_DEPTH_WRITE_MASK_ZERO;
        pd.DepthStencilState.DepthFunc = toCompare(desc.depthOp);
        pd.DSVFormat = toDxgi(desc.depthFormat);
        auto p = std::make_unique<D3DPipeline>();
        p->topology = toTopology(desc.topology);
        const HRESULT hr = device_->CreateGraphicsPipelineState(&pd, IID_PPV_ARGS(&p->pso));
        if (FAILED(hr)) {
            spdlog::error("d3d12: CreateGraphicsPipelineState '{}' failed (0x{:08x})", desc.name != nullptr ? desc.name : "", static_cast<unsigned>(hr));
            return nullptr;
        }
        if (desc.name != nullptr) p->pso->SetName(widen(desc.name).c_str());
        return p;
    }

    std::unique_ptr<AccelerationStructure> createBlas(const BlasDesc& desc) override {
        if (!caps_.rayQuery) return nullptr;
        auto a = std::make_unique<D3DAccel>(*this, false);
        if (!a->createBlas(desc)) return nullptr;
        return a;
    }
    std::unique_ptr<AccelerationStructure> createTlas(const TlasDesc& desc) override {
        if (!caps_.rayQuery) return nullptr;
        auto a = std::make_unique<D3DAccel>(*this, true);
        if (!a->createTlas(desc)) return nullptr;
        return a;
    }
    std::unique_ptr<AccelerationStructure> createBlasStorage(std::uint64_t bytes, const char*) override {
        if (!caps_.rayQuery) return nullptr;
        auto a = std::make_unique<D3DAccel>(*this, false);
        if (!a->createStorage(bytes, "rhi blas (compacted)")) return nullptr;
        return a;
    }
    std::unique_ptr<SizeQuery> createSizeQuery(std::uint32_t capacity) override {
        if (!caps_.rayQuery) return nullptr;
        auto q = std::make_unique<D3DSizeQuery>(*this, capacity);
        if (!q->create()) return nullptr;
        return q;
    }
    std::uint64_t buildScratchBytes() const override { return scratchSize_; }
    // The shared scratch pool of BLAS builds: at least `bytes`. A replaced pool is kept until the device dies (lists recorded earlier may name it).
    bool ensureScratch(std::uint64_t bytes) {
        if (scratch_ && scratchSize_ >= bytes) return true;
        if (scratch_) scratchGraveyard_.push_back(std::move(scratch_));
        scratchSize_ = alignUp(std::max<std::uint64_t>(bytes, 4ull << 20), 256);
        scratch_ = createDefaultBuffer(scratchSize_, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COMMON);
        if (!scratch_) {
            scratchSize_ = 0;
            return false;
        }
        scratch_->SetName(L"rhi build scratch pool");
        return true;
    }
    std::uint64_t scratchAddress() const { return scratch_->GetGPUVirtualAddress(); }
    std::uint64_t scratchSize() const { return scratchSize_; }

    void submitAndWait(const std::function<void(CommandList&)>& record) override;
    std::unique_ptr<Swapchain> createSwapchain(const SwapchainDesc& desc) override;
    bool renderAndPresent(Swapchain& swapchain, const std::function<void(CommandList&, Texture&)>& record) override;
    IDXGIFactory2* factory() { return factory_.Get(); }
    ID3D12CommandQueue* queue() { return queue_.Get(); }

    // ---- internals shared with the resources / the command list ----
    ID3D12Device* device() { return device_.Get(); }
    ID3D12RootSignature* rootSignature() { return rootSignature_.Get(); }
    D3D12_CPU_DESCRIPTOR_HANDLE srvCpu(std::uint32_t i) const {
        D3D12_CPU_DESCRIPTOR_HANDLE h = srvHeap_->GetCPUDescriptorHandleForHeapStart();
        h.ptr += static_cast<SIZE_T>(i) * srvInc_;
        return h;
    }
    D3D12_GPU_DESCRIPTOR_HANDLE srvGpu(std::uint32_t i) const {
        D3D12_GPU_DESCRIPTOR_HANDLE h = srvHeap_->GetGPUDescriptorHandleForHeapStart();
        h.ptr += static_cast<UINT64>(i) * srvInc_;
        return h;
    }
    D3D12_CPU_DESCRIPTOR_HANDLE samplerCpu(std::uint32_t i) const {
        D3D12_CPU_DESCRIPTOR_HANDLE h = samplerHeap_->GetCPUDescriptorHandleForHeapStart();
        h.ptr += static_cast<SIZE_T>(i) * samplerInc_;
        return h;
    }
    D3D12_CPU_DESCRIPTOR_HANDLE rtvCpu(std::uint32_t i) const {
        D3D12_CPU_DESCRIPTOR_HANDLE h = rtvHeap_->GetCPUDescriptorHandleForHeapStart();
        h.ptr += static_cast<SIZE_T>(i) * rtvInc_;
        return h;
    }
    D3D12_CPU_DESCRIPTOR_HANDLE dsvCpu(std::uint32_t i) const {
        D3D12_CPU_DESCRIPTOR_HANDLE h = dsvHeap_->GetCPUDescriptorHandleForHeapStart();
        h.ptr += static_cast<SIZE_T>(i) * dsvInc_;
        return h;
    }
    HeapAllocator& srvAlloc() { return srvAlloc_; }
    std::uint32_t nextRtv() { return rtvNext_++ % kRtvCount; }
    std::uint32_t nextDsv() { return dsvNext_++ % kDsvCount; }
    // a table of kStorageImages UAV descriptors in the ring at the end of the heap: its first index
    std::uint32_t nextUavTable() {
        const std::uint32_t perSlot = kUavRingPerSlot / rhi::kStorageImages;
        const std::uint32_t t = uavTable_++ % perSlot;
        return kHeapSize - kUavRing + (gameMode_ ? frameSlot_ : 0) * kUavRingPerSlot + t * rhi::kStorageImages;
    }
    // one transient SRV slot of this frame slot's share of the view ring
    std::uint32_t nextViewSlot() {
        const std::uint32_t perSlot = kViewRing / rhi::kFramesInFlight;
        const std::uint32_t t = viewNext_++ % perSlot;
        return kHeapSize - kUavRing - kViewRing + (gameMode_ ? frameSlot_ : 0) * perSlot + t;
    }
    ID3D12Device5* device5() { return device5_.Get(); }
    // 14.22: the BLAS storage arena. Every CreateCommittedResource of a BLAS (its build storage and its compacted copy) waited on the CPU inside the AMD driver for the
    // paging fence (d3d12core NtGdiDdDDIWaitForSynchronizationObjectFromCpu below amdxc64's allocation): a spot change that builds and compacts ~100 - 300 BLAS stalled
    // the engine thread for 0.5 - 2.2 s (rvk.log `rt: scene: slow pass`, `diag: hang`). BLAS storage now lives in a few large buffers in the RAYTRACING_ACCELERATION_STRUCTURE
    // state (an acceleration structure may sit at any 256-byte aligned address of such a buffer; every AS barrier of this backend is a null UAV barrier), suballocated
    // first-fit with coalescing; the first chunk is created with the device (ray query on). A request larger than a chunk gets a dedicated buffer as before.
    static constexpr std::uint64_t kAsChunk = 64ull << 20;
    struct AsChunk {
        ComPtr<ID3D12Resource> buffer;
        std::vector<std::pair<std::uint64_t, std::uint64_t>> free;  // (offset, size), sorted by offset
        std::uint64_t used = 0;
    };
    bool asArenaGrow() {
        ComPtr<ID3D12Resource> b = createDefaultBuffer(kAsChunk, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE);
        if (!b) return false;
        b->SetName(L"rhi blas arena");
        AsChunk c;
        c.buffer = std::move(b);
        c.free.push_back({0, kAsChunk});
        asChunks_.push_back(std::move(c));
        ++asStats_.chunks;
        return true;
    }
    // `bytes` (a multiple of 256) from the arena: the chunk's resource + offset; false = not from the arena (larger than a chunk / no memory)
    bool asAlloc(std::uint64_t bytes, AsSpan& out, ID3D12Resource*& resource) {
        static const bool off = [] {
            char e[8];
            return GetEnvironmentVariableA("RVK_AS_ARENA", e, sizeof e) > 0 && e[0] == '0';
        }();
        if (off || bytes == 0 || bytes > kAsChunk) return false;  // (RVK_AS_ARENA=0: one buffer per BLAS, the old behaviour, A/B)
        const std::lock_guard lock(asMutex_);
        for (int pass = 0; pass < 2; ++pass) {
            for (std::size_t ci = 0; ci < asChunks_.size(); ++ci) {
                AsChunk& c = asChunks_[ci];
                for (std::size_t i = 0; i < c.free.size(); ++i) {
                    auto& [off, size] = c.free[i];
                    if (size < bytes) continue;
                    out = {static_cast<std::int32_t>(ci), off, bytes};
                    off += bytes;
                    size -= bytes;
                    if (size == 0) c.free.erase(c.free.begin() + static_cast<std::ptrdiff_t>(i));
                    c.used += bytes;
                    resource = c.buffer.Get();
                    ++asStats_.allocs;
                    return true;
                }
            }
            if (pass == 0 && !asArenaGrow()) return false;
        }
        return false;
    }
    void asFree(const AsSpan& span) {
        if (span.chunk < 0) return;
        const std::lock_guard lock(asMutex_);
        if (static_cast<std::size_t>(span.chunk) >= asChunks_.size()) return;
        AsChunk& c = asChunks_[static_cast<std::size_t>(span.chunk)];
        auto it = std::lower_bound(c.free.begin(), c.free.end(), std::pair<std::uint64_t, std::uint64_t>{span.offset, 0});
        it = c.free.insert(it, {span.offset, span.size});
        // (coalesce with the neighbours)
        if (it + 1 != c.free.end() && it->first + it->second == (it + 1)->first) {
            it->second += (it + 1)->second;
            c.free.erase(it + 1);
        }
        if (it != c.free.begin() && (it - 1)->first + (it - 1)->second == it->first) {
            (it - 1)->second += it->second;
            c.free.erase(it);
        }
        c.used -= span.size;
        ++asStats_.frees;
    }
    struct AsStats {
        std::uint64_t chunks = 0, allocs = 0, frees = 0, dedicated = 0;
    } asStats_;
    std::mutex asMutex_;
    std::vector<AsChunk> asChunks_;
    ComPtr<ID3D12Resource> scratch_;
    std::vector<ComPtr<ID3D12Resource>> scratchGraveyard_;
    std::uint64_t scratchSize_ = 0;
    // a committed buffer in a default heap (acceleration structure storage, scratch)
    ComPtr<ID3D12Resource> createDefaultBuffer(std::uint64_t size, D3D12_RESOURCE_FLAGS flags, D3D12_RESOURCE_STATES state) {
        D3D12_HEAP_PROPERTIES hp{};
        hp.Type = D3D12_HEAP_TYPE_DEFAULT;
        D3D12_RESOURCE_DESC rd{};
        rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        rd.Width = size;
        rd.Height = rd.DepthOrArraySize = rd.MipLevels = 1;
        rd.SampleDesc = {1, 0};
        rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        rd.Flags = flags;
        ComPtr<ID3D12Resource> r;
        if (FAILED(device_->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd, state, nullptr, IID_PPV_ARGS(&r)))) return nullptr;
        return r;
    }
    // the ExecuteIndirect signature of indexed draws `stride` bytes apart (one D3D12_DRAW_INDEXED_ARGUMENTS each; no root arguments)
    ID3D12CommandSignature* drawIndexedSignature(std::uint32_t stride) {
        auto it = drawSignatures_.find(stride);
        if (it != drawSignatures_.end()) return it->second.Get();
        D3D12_INDIRECT_ARGUMENT_DESC arg{};
        arg.Type = D3D12_INDIRECT_ARGUMENT_TYPE_DRAW_INDEXED;
        D3D12_COMMAND_SIGNATURE_DESC cd{stride, 1, &arg, 0};
        ComPtr<ID3D12CommandSignature> sig;
        if (FAILED(device_->CreateCommandSignature(&cd, nullptr, IID_PPV_ARGS(&sig)))) return nullptr;
        return (drawSignatures_[stride] = std::move(sig)).Get();
    }
    D3D12_CPU_DESCRIPTOR_HANDLE uavClearCpu() const { return uavClearHeap_->GetCPUDescriptorHandleForHeapStart(); }
    ID3D12DescriptorHeap* srvHeap() { return srvHeap_.Get(); }
    ID3D12DescriptorHeap* samplerHeap() { return samplerHeap_.Get(); }

    void waitIdle() override;
    void legacyWaitIdle() {
        if (removed_.load()) return;
        if (!queue_ || !fence_ || fenceEvent_ == nullptr) {  // (a resource outliving the device's core objects: a teardown order bug, see ~D3D12DeviceImpl)
            spdlog::error("d3d12: waitIdle after the queue / fence were released (a resource destroyed after the device's teardown)");
            return;
        }
        const std::uint64_t v = ++fenceValue_;
        queue_->Signal(fence_.Get(), v);
        if (fence_->GetCompletedValue() < v) {
            if (FAILED(fence_->SetEventOnCompletion(v, fenceEvent_))) {
                checkRemoved("waitIdle (SetEventOnCompletion)");
                return;
            }
            // a removed device signals its fences; a GPU that never answers is reported after 10 s instead of hanging the process
            if (WaitForSingleObject(fenceEvent_, 10000) == WAIT_TIMEOUT) {
                spdlog::error("d3d12: the GPU did not finish within 10 s (hang)");
                markRemoved("waitIdle: 10 s timeout", DXGI_ERROR_DEVICE_HUNG);
                return;
            }
        }
        checkRemoved("waitIdle");
    }

    // ---- device removal (docs/research/dx12-game-port.md "Agility SDK 620: debugging") ----
    bool deviceLost() const override { return removed_.load(); }
    bool simulateDeviceLoss() override {
        ComPtr<ID3D12Device5> d5;
        if (FAILED(device_.As(&d5))) return false;
        d5->RemoveDevice();
        return true;
    }
    // GetDeviceRemovedReason: on the first failure the reason, DRED's breadcrumbs / page fault and a minidump are written (once)
    bool checkRemoved(const char* where) {
        if (removed_.load()) return true;
        const HRESULT reason = device_ ? device_->GetDeviceRemovedReason() : S_OK;
        if (SUCCEEDED(reason)) return false;
        markRemoved(where, reason);
        return true;
    }
    void markRemoved(const char* where, HRESULT reason) {
        if (removed_.exchange(true)) return;
        std::string text = std::format("D3D12 device removed at {}: 0x{:08x} ({})\r\nadapter {}, Agility SDK {}\r\n", where, static_cast<unsigned>(reason),
                                       removedReasonName(reason), caps_.adapterName, caps_.agilitySdkVersion);
        if (device_ && dredOn_) text += dredReport();
        else text += "DRED is off (RVK_DRED=0): no breadcrumbs / page fault data\r\n";
        std::size_t from = 0;
        while (from < text.size()) {
            const std::size_t end = text.find("\r\n", from);
            spdlog::error("d3d12: {}", text.substr(from, end == std::string::npos ? std::string::npos : end - from));
            if (end == std::string::npos) break;
            from = end + 2;
        }
        // the other renderer processes on this GPU (core/gpu_events.h, rt-shadows.md 11.2): a removal without a page fault of its own shortly after another
        // process's page fault / hang is that process's GPU reset
        const bool faulted = text.find("DRED: page fault at") != std::string::npos;
        gpuevents::record(faulted ? gpuevents::Kind::LostWithFault : gpuevents::Kind::Lost, std::format("d3d12, at {}", where));
        for (const std::string& o : gpuevents::reportOthers()) {
            spdlog::error("d3d12: {}", o);
            text += o + "\r\n";
        }
        if (const auto dump = crashdump::write("d3d12_device_removed", text); !dump.empty()) spdlog::error("d3d12: minidump written to {}", dump.string());
    }

private:
    static const char* removedReasonName(HRESULT hr) {
        switch (hr) {
        case DXGI_ERROR_DEVICE_HUNG: return "DXGI_ERROR_DEVICE_HUNG: the GPU stopped answering (TDR)";
        case DXGI_ERROR_DEVICE_REMOVED: return "DXGI_ERROR_DEVICE_REMOVED: the adapter was removed / reset";
        case DXGI_ERROR_DEVICE_RESET: return "DXGI_ERROR_DEVICE_RESET: a command made the GPU reset";
        case DXGI_ERROR_DRIVER_INTERNAL_ERROR: return "DXGI_ERROR_DRIVER_INTERNAL_ERROR";
        case DXGI_ERROR_INVALID_CALL: return "DXGI_ERROR_INVALID_CALL";
        default: return "unknown reason";
        }
    }
    static std::string narrow(const wchar_t* w) {
        std::string s;
        for (; w != nullptr && *w != L'\0'; ++w) s += static_cast<char>(*w < 128 ? *w : '?');
        return s;
    }
    static const char* breadcrumbOp(UINT op) {
        static constexpr const char* kNames[] = {
            "SetMarker", "BeginEvent", "EndEvent", "DrawInstanced", "DrawIndexedInstanced", "ExecuteIndirect", "Dispatch", "CopyBufferRegion",
            "CopyTextureRegion", "CopyResource", "CopyTiles", "ResolveSubresource", "ClearRenderTargetView", "ClearUnorderedAccessView",
            "ClearDepthStencilView", "ResourceBarrier", "ExecuteBundle", "Present", "ResolveQueryData", "BeginSubmission", "EndSubmission",
            "DecodeFrame", "ProcessFrames", "AtomicCopyBufferUint", "AtomicCopyBufferUint64", "ResolveSubresourceRegion", "WriteBufferImmediate",
            "DecodeFrame1", "SetProtectedResourceSession", "DecodeFrame2", "ProcessFrames1", "BuildRaytracingAccelerationStructure",
            "EmitRaytracingAccelerationStructurePostbuildInfo", "CopyRaytracingAccelerationStructure", "DispatchRays", "InitializeMetaCommand",
            "ExecuteMetaCommand", "EstimateMotion", "ResolveMotionVectorHeap", "SetPipelineState1", "InitializeExtensionCommand",
            "ExecuteExtensionCommand", "DispatchMesh"};
        return op < std::size(kNames) ? kNames[op] : "(newer op)";
    }
    // DRED (ID3D12DeviceRemovedExtendedData1): the command lists that had not finished (the op the GPU stopped at, with its neighbours) and the page fault
    // (the faulting address, the live and the recently freed allocations around it)
    std::string dredReport() {
        std::string text;
        ComPtr<ID3D12DeviceRemovedExtendedData1> dred;
        if (FAILED(device_.As(&dred))) return "DRED: ID3D12DeviceRemovedExtendedData1 is not available\r\n";
        D3D12_DRED_AUTO_BREADCRUMBS_OUTPUT1 ab{};
        if (SUCCEEDED(dred->GetAutoBreadcrumbsOutput1(&ab))) {
            int lists = 0;
            for (const D3D12_AUTO_BREADCRUMB_NODE1* n = ab.pHeadAutoBreadcrumbNode; n != nullptr && lists < 16; n = n->pNext) {
                const UINT last = n->pLastBreadcrumbValue != nullptr ? *n->pLastBreadcrumbValue : 0;
                if (last >= n->BreadcrumbCount) continue;  // finished
                ++lists;
                text += std::format("DRED: command list '{}' on queue '{}': {} of {} ops completed\r\n",
                                    n->pCommandListDebugNameW != nullptr ? narrow(n->pCommandListDebugNameW) : std::string("(unnamed)"),
                                    n->pCommandQueueDebugNameW != nullptr ? narrow(n->pCommandQueueDebugNameW) : std::string("(unnamed)"), last,
                                    n->BreadcrumbCount);
                const UINT first = last > 3 ? last - 3 : 0;
                for (UINT i = first; i < n->BreadcrumbCount && i <= last + 3 && n->pCommandHistory != nullptr; ++i) {
                    text += std::format("DRED:   op {}: {}{}\r\n", i, breadcrumbOp(static_cast<UINT>(n->pCommandHistory[i])), i == last ? "   <-- GPU stopped here" : "");
                }
                for (UINT i = 0; i < n->BreadcrumbContextsCount && n->pBreadcrumbContexts != nullptr; ++i) {
                    if (n->pBreadcrumbContexts[i].BreadcrumbIndex <= last + 3 && n->pBreadcrumbContexts[i].BreadcrumbIndex + 6 >= last)
                        text += std::format("DRED:   context at op {}: {}\r\n", n->pBreadcrumbContexts[i].BreadcrumbIndex, narrow(n->pBreadcrumbContexts[i].pContextString));
                }
            }
            if (lists == 0) text += "DRED: no command list with unfinished ops\r\n";
        } else {
            text += "DRED: no breadcrumb data\r\n";
        }
        D3D12_DRED_PAGE_FAULT_OUTPUT1 pf{};
        if (SUCCEEDED(dred->GetPageFaultAllocationOutput1(&pf)) && pf.PageFaultVA != 0) {
            text += std::format("DRED: page fault at GPU VA 0x{:x}\r\n", static_cast<std::uint64_t>(pf.PageFaultVA));
            const auto list = [&](const char* what, const D3D12_DRED_ALLOCATION_NODE1* n) {
                int count = 0;
                for (; n != nullptr && count < 8; n = n->pNext, ++count) {
                    text += std::format("DRED:   {} allocation '{}' (type {})\r\n", what, n->ObjectNameW != nullptr ? narrow(n->ObjectNameW) : std::string("(unnamed)"),
                                        static_cast<int>(n->AllocationType));
                }
            };
            list("live", pf.pHeadExistingAllocationNode);
            list("recently freed", pf.pHeadRecentFreedAllocationNode);
        } else {
            text += "DRED: no page fault recorded\r\n";
        }
        return text;
    }
    // what the device offers beyond the floor (docs/research/raytracing.md 9); DXR 1.2 = opacity micromaps + shader execution reordering, work graphs,
    // enhanced barriers and the ExecuteIndirect tier are reported for the record. The renderer uses none of them (the RX 7900 XTX has none of the DXR 1.2 ones).
    void queryRayTracingCaps(const DeviceDesc&) {
        D3D12_FEATURE_DATA_D3D12_OPTIONS5 o5{};
        if (SUCCEEDED(device_->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS5, &o5, sizeof(o5)))) {
            const auto tier = static_cast<int>(o5.RaytracingTier);
            caps_.dxrTier = tier >= D3D12_RAYTRACING_TIER_1_2 ? 12 : tier >= D3D12_RAYTRACING_TIER_1_1 ? 11 : tier >= D3D12_RAYTRACING_TIER_1_0 ? 10 : 0;
        }
        D3D12_FEATURE_DATA_D3D12_OPTIONS22 o22{};
        const bool o22Ok = SUCCEEDED(device_->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS22, &o22, sizeof(o22)));
        caps_.serReorders = o22Ok && o22.ShaderExecutionReorderingActuallyReorders != FALSE;
        D3D12_FEATURE_DATA_D3D12_OPTIONS21 o21{};
        const bool o21Ok = SUCCEEDED(device_->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS21, &o21, sizeof(o21)));
        D3D12_FEATURE_DATA_D3D12_OPTIONS12 o12{};
        const bool o12Ok = SUCCEEDED(device_->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS12, &o12, sizeof(o12)));
        spdlog::info("d3d12: caps: DXR {} (opacity micromaps and shader execution reordering are DXR 1.2: {}), SER reorders {}, work graphs tier {}, ExecuteIndirect "
                     "tier {}, enhanced barriers {}; clustered geometry is NVIDIA-only (NVAPI / VK_NV_cluster_acceleration_structure: not queried here)",
                     caps_.dxrTier == 12 ? "1.2" : caps_.dxrTier == 11 ? "1.1" : caps_.dxrTier == 10 ? "1.0" : "none", caps_.dxrTier >= 12 ? "offered" : "not offered",
                     o22Ok ? (caps_.serReorders ? "yes" : "no") : "not reported", o21Ok ? static_cast<int>(o21.WorkGraphsTier) / 10 : -1,
                     o21Ok ? static_cast<int>(o21.ExecuteIndirectTier) / 10 : -1, o12Ok ? (o12.EnhancedBarriersSupported ? "yes" : "no") : "not reported");
    }

#include "d3d12_game_priv.inc"
    bool createRootSignature() {
        D3D12_ROOT_PARAMETER params[3]{};
        params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
        params[0].Constants = {0, 0, 32};  // b0 space0
        params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
        params[1].Descriptor = {8, 1};  // b8 space1
        params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        D3D12_DESCRIPTOR_RANGE uav{};
        uav.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
        uav.NumDescriptors = rhi::kStorageImages;
        uav.BaseShaderRegister = 0;  // u0..u7
        uav.RegisterSpace = 1;
        uav.OffsetInDescriptorsFromTableStart = 0;
        params[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        params[2].DescriptorTable = {1, &uav};
        params[2].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        D3D12_ROOT_SIGNATURE_DESC rd{};
        rd.NumParameters = 3;
        rd.pParameters = params;
        rd.Flags = D3D12_ROOT_SIGNATURE_FLAG_CBV_SRV_UAV_HEAP_DIRECTLY_INDEXED | D3D12_ROOT_SIGNATURE_FLAG_SAMPLER_HEAP_DIRECTLY_INDEXED |
                   D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;
        ComPtr<ID3DBlob> blob, error;
        const HRESULT hr = D3D12SerializeRootSignature(&rd, D3D_ROOT_SIGNATURE_VERSION_1_0, &blob, &error);
        if (FAILED(hr)) {
            spdlog::error("d3d12: root signature: {}", error ? static_cast<const char*>(error->GetBufferPointer()) : "?");
            return false;
        }
        RVK_HR(device_->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(&rootSignature_)), "CreateRootSignature");
        rootSignature_->SetName(L"rhi root signature");
        return true;
    }

    friend class D3DCommandList;

    ComPtr<IDXGIFactory2> factory_;
    ComPtr<ID3D12Device> device_;
    ComPtr<ID3D12Device5> device5_;  // DXR (null without DeviceDesc::rayTracing / tier 1.1)
    std::map<std::uint32_t, ComPtr<ID3D12CommandSignature>> drawSignatures_;
    ComPtr<ID3D12CommandQueue> queue_;
    ComPtr<ID3D12CommandAllocator> allocator_;
    ComPtr<ID3D12GraphicsCommandList> list_;
    ComPtr<ID3D12Fence> fence_;
    HANDLE fenceEvent_ = nullptr;
    std::uint64_t fenceValue_ = 0;
    ComPtr<ID3D12RootSignature> rootSignature_;
    ComPtr<ID3D12DescriptorHeap> srvHeap_, samplerHeap_, rtvHeap_, dsvHeap_;
    ComPtr<ID3D12DescriptorHeap> uavClearHeap_;  // one CPU-only UAV descriptor: the source of ClearUnorderedAccessView*
    UINT srvInc_ = 0, samplerInc_ = 0, rtvInc_ = 0, dsvInc_ = 0;
    HeapAllocator srvAlloc_, samplerAlloc_;
    std::uint32_t rtvNext_ = 0, dsvNext_ = 0, uavTable_ = 0, viewNext_ = 0;
    DeviceCaps caps_;
    std::atomic<std::uint32_t> errors_{0};
    std::atomic<bool> removed_{false};  // the device was removed (markRemoved ran)
    bool dredOn_ = false;               // DRED was enabled before the device was made
    std::unique_ptr<Texture> defaultTexture_;  // the texture behind heap entry 0 (declared last: released first)
};

// ---- resources --------------------------------------------------------------------------------------------------------------

bool D3DBuffer::create() {
    D3D12_HEAP_PROPERTIES hp{};
    hp.Type = desc_.memory == Memory::Upload ? D3D12_HEAP_TYPE_UPLOAD : desc_.memory == Memory::Readback ? D3D12_HEAP_TYPE_READBACK : D3D12_HEAP_TYPE_DEFAULT;
    // a read-back buffer the GPU writes with shaders (probes, diagnostics): a readback heap has no UAV access, so it is a GPU upload heap
    // (device memory the CPU maps; the CPU reads it after the frame's fence)
    if (desc_.memory == Memory::Readback && (desc_.usage & VK_BUFFER_USAGE_STORAGE_BUFFER_BIT) != 0 && !arena && dev_.gpuUploadHeapSupported()) gpuUpload = true;
    if (gpuUpload) hp.Type = D3D12_HEAP_TYPE_GPU_UPLOAD;
    D3D12_RESOURCE_DESC rd{};
    rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    rd.Width = alignUp(std::max<std::uint64_t>(desc_.size, 4), 4);
    rd.Height = rd.DepthOrArraySize = rd.MipLevels = 1;
    rd.SampleDesc = {1, 0};
    rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    // shader-writable (RWByteAddressBuffer, atomics): device memory only (upload / readback heaps cannot have UAVs)
    const bool writable = (desc_.usage & VK_BUFFER_USAGE_STORAGE_BUFFER_BIT) != 0 && (hp.Type == D3D12_HEAP_TYPE_DEFAULT || gpuUpload) && !arena;
    if (writable) rd.Flags |= D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    state = hp.Type == D3D12_HEAP_TYPE_UPLOAD ? D3D12_RESOURCE_STATE_GENERIC_READ : hp.Type == D3D12_HEAP_TYPE_GPU_UPLOAD && desc_.memory == Memory::Readback ? D3D12_RESOURCE_STATE_COMMON : hp.Type == D3D12_HEAP_TYPE_READBACK ? D3D12_RESOURCE_STATE_COPY_DEST : D3D12_RESOURCE_STATE_COMMON;
    const HRESULT hr0 = dev_.device()->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd, state, nullptr, IID_PPV_ARGS(&resource_));
    if (FAILED(hr0)) {
        dev_.checkRemoved(hr0, "CreateCommittedResource");
        spdlog::error("d3d12: CreateCommittedResource (buffer '{}', {} bytes, heap {}) failed (0x{:08x})", desc_.name != nullptr ? desc_.name : "?", rd.Width, static_cast<int>(hp.Type), static_cast<unsigned>(hr0));
        return false;
    }
    if (desc_.name != nullptr) resource_->SetName(widen(desc_.name).c_str());
    if (hp.Type != D3D12_HEAP_TYPE_DEFAULT) {  // (a GPU upload heap maps too)
        D3D12_RANGE none{0, 0};
        if (FAILED(resource_->Map(0, hp.Type == D3D12_HEAP_TYPE_READBACK ? nullptr : &none, &mapped_))) return false;
    }
    if ((desc_.usage & VK_BUFFER_USAGE_STORAGE_BUFFER_BIT) != 0) {
        slot_ = dev_.srvAlloc().alloc();
        D3D12_SHADER_RESOURCE_VIEW_DESC sd{};
        sd.Format = DXGI_FORMAT_R32_TYPELESS;
        sd.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
        sd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        sd.Buffer.NumElements = static_cast<UINT>(rd.Width / 4);
        sd.Buffer.Flags = D3D12_BUFFER_SRV_FLAG_RAW;
        dev_.device()->CreateShaderResourceView(resource_.Get(), &sd, dev_.srvCpu(slot_));
    }
    if (writable) {
        slotRw_ = dev_.srvAlloc().alloc();
        D3D12_UNORDERED_ACCESS_VIEW_DESC ud{};
        ud.Format = DXGI_FORMAT_R32_TYPELESS;
        ud.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
        ud.Buffer.NumElements = static_cast<UINT>(rd.Width / 4);
        ud.Buffer.Flags = D3D12_BUFFER_UAV_FLAG_RAW;
        dev_.device()->CreateUnorderedAccessView(resource_.Get(), nullptr, &ud, dev_.srvCpu(slotRw_));
    }
    return true;
}

D3DBuffer::~D3DBuffer() {
    if (dev_.gameMode()) {
        // the GPU may still read it in the frames in flight: the resource and its heap slots go when the frame being recorded has completed
        D3D12DeviceImpl* d = &dev_;
        dev_.retire([d, r = std::move(resource_), a = slot_, b = slotRw_]() mutable {
            d->srvAlloc().release(a);
            d->srvAlloc().release(b);
            r.Reset();
        });
        return;
    }
    dev_.waitIdle();
    dev_.srvAlloc().release(slot_);
    dev_.srvAlloc().release(slotRw_);
}

bool D3DTexture::create() {
    if (toDxgi(desc_.format) == DXGI_FORMAT_UNKNOWN) {
        spdlog::error("d3d12: texture format {} is not supported by the RHI", static_cast<int>(desc_.format));
        return false;
    }
    if (desc_.cube) {  // a cube map is a 2D resource of 6 slices (the faces)
        desc_.type = VK_IMAGE_TYPE_2D;
        desc_.layers = 6;
        desc_.extent.depth = 1;
    }
    const bool volume = desc_.type == VK_IMAGE_TYPE_3D;
    D3D12_HEAP_PROPERTIES hp{};
    hp.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC rd{};
    rd.Dimension = volume ? D3D12_RESOURCE_DIMENSION_TEXTURE3D : D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    rd.Width = desc_.extent.width;
    rd.Height = desc_.extent.height;
    rd.DepthOrArraySize = static_cast<UINT16>(volume ? desc_.extent.depth : desc_.layers);
    rd.MipLevels = static_cast<UINT16>(desc_.mips);
    rd.Format = resourceFormat(desc_.format, (desc_.usage & VK_IMAGE_USAGE_SAMPLED_BIT) != 0);
    rd.SampleDesc = {1, 0};
    rd.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    if ((desc_.usage & VK_IMAGE_USAGE_STORAGE_BIT) != 0) rd.Flags |= D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    // a colour texture with a mip chain gets generated by scaled copies (draws into its levels): it must be a render target
    if (dev_.gameMode() && desc_.mips > 1 && !desc_.cube && !volume && !isDepth(desc_.format) && formatBlock(desc_.format).width == 1 &&
        (desc_.usage & VK_IMAGE_USAGE_TRANSFER_SRC_BIT) != 0 && dev_.formatSupport(desc_.format, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT, false)) {
        desc_.usage |= VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    }
    if ((desc_.usage & VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT) != 0) rd.Flags |= D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    if ((desc_.usage & VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT) != 0 || isDepth(desc_.format)) rd.Flags |= D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;
    // render targets / depth: the optimized clear value (black / far), so that clears take the fast path
    D3D12_CLEAR_VALUE cv{};
    const bool target = (rd.Flags & (D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET | D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL)) != 0;
    if (target) {
        cv.Format = toDxgi(desc_.format);
        if (isDepth(desc_.format)) cv.DepthStencil = {1.0f, 0};
    }
    if (FAILED(dev_.device()->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_COMMON, target ? &cv : nullptr, IID_PPV_ARGS(&resource_)))) return false;
    if (desc_.name != nullptr) resource_->SetName(widen(desc_.name).c_str());
    if ((desc_.usage & VK_IMAGE_USAGE_SAMPLED_BIT) != 0) {
        if (isArray()) {  // (no whole-array view: heapSampled is layer 0, see rhi.h)
            slot_ = heapSampledLayer(0);
        } else {
            slot_ = desc_.lowHeapSlot ? dev_.srvAlloc().allocLow() : 0u;
            if (slot_ == 0) slot_ = dev_.srvAlloc().alloc();  // (the low range is full: any slot, the caller checks the index)
            D3D12_SHADER_RESOURCE_VIEW_DESC sd{};
            sd.Format = srvFormat(desc_.format);
            sd.Shader4ComponentMapping = srvMapping(desc_);
            if (desc_.cube) {
                sd.ViewDimension = D3D12_SRV_DIMENSION_TEXTURECUBE;
                sd.TextureCube.MipLevels = desc_.mips;
            } else if (volume) {
                sd.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE3D;
                sd.Texture3D.MipLevels = desc_.mips;
            } else {
                sd.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
                sd.Texture2D.MipLevels = desc_.mips;
            }
            dev_.device()->CreateShaderResourceView(resource_.Get(), &sd, dev_.srvCpu(slot_));
        }
    }
    return true;
}

// A Texture2D view of one layer of an array resource. The shader declares Texture2D, but a plain TEXTURE2D view always sees array slice 0 (D3D12 has
// no slice offset in it), so every layer is a 2D-array view of one slice. Whether the runtime accepts that for Texture2D accesses is checked by the
// debug layer / GPU based validation in rvk_rhitest.
std::uint32_t D3DTexture::heapSampledLayer(std::uint32_t layer) {
    if ((desc_.usage & VK_IMAGE_USAGE_SAMPLED_BIT) == 0 || layer >= desc_.layers) return 0;
    if (!isArray()) return layer == 0 ? slot_ : 0;
    if (layerSlots_.empty()) layerSlots_.assign(desc_.layers, 0);
    if (layerSlots_[layer] == 0) {
        const std::uint32_t slot = dev_.srvAlloc().alloc();
        D3D12_SHADER_RESOURCE_VIEW_DESC sd{};
        sd.Format = srvFormat(desc_.format);
        sd.Shader4ComponentMapping = srvMapping(desc_);
        sd.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DARRAY;
        sd.Texture2DArray.MipLevels = desc_.mips;
        sd.Texture2DArray.FirstArraySlice = layer;
        sd.Texture2DArray.ArraySize = 1;
        dev_.device()->CreateShaderResourceView(resource_.Get(), &sd, dev_.srvCpu(slot));
        layerSlots_[layer] = slot;
    }
    return layerSlots_[layer];
}

D3DTexture::~D3DTexture() {
    if (!resource_) return;
    if (dev_.gameMode()) {
        D3D12DeviceImpl* d = &dev_;
        std::vector<std::uint32_t> slots = layerSlots_;
        if (!isArray()) slots.push_back(slot_);
        dev_.retire([d, r = std::move(resource_), slots = std::move(slots)]() mutable {
            for (std::uint32_t sl : slots) d->srvAlloc().release(sl);
            r.Reset();
        });
        return;
    }
    dev_.waitIdle();
    if (!isArray()) dev_.srvAlloc().release(slot_);
    for (std::uint32_t s : layerSlots_) dev_.srvAlloc().release(s);
}

// ---- acceleration structures (DXR 1.1) ---------------------------------------------------------------------------------------

D3DAccel::~D3DAccel() {
    if (dev_.gameMode()) {
        D3D12DeviceImpl* d = &dev_;
        dev_.retire([d, a = std::move(storage_), b = std::move(scratch_), slot = slot_, span = span_]() mutable {
            d->srvAlloc().release(slot);
            d->asFree(span);  // (14.22: the arena span is reused only once the frames that may still read it are done)
            a.Reset();
            b.Reset();
        });
        return;
    }
    dev_.waitIdle();
    dev_.srvAlloc().release(slot_);
    dev_.asFree(span_);
}

namespace {

void geometryDescs(std::span<const TriangleGeometry> list, std::vector<D3D12_RAYTRACING_GEOMETRY_DESC>& out) {
    for (const TriangleGeometry& t : list) {
        D3D12_RAYTRACING_GEOMETRY_DESC g{};
        g.Type = D3D12_RAYTRACING_GEOMETRY_TYPE_TRIANGLES;
        g.Flags = t.opaque ? D3D12_RAYTRACING_GEOMETRY_FLAG_OPAQUE : D3D12_RAYTRACING_GEOMETRY_FLAG_NONE;
        g.Triangles.VertexFormat = toDxgi(t.vertexFormat);
        g.Triangles.VertexCount = t.vertexCount;
        g.Triangles.VertexBuffer.StartAddress =
            t.vertexAddress != 0 ? t.vertexAddress : (t.vertices != nullptr ? static_cast<D3DBuffer*>(t.vertices)->gpuAddress() + t.vertexOffset : 0);
        g.Triangles.VertexBuffer.StrideInBytes = t.vertexStride;
        if (t.indexed()) {
            g.Triangles.IndexFormat = t.indexType == VK_INDEX_TYPE_UINT16 ? DXGI_FORMAT_R16_UINT : DXGI_FORMAT_R32_UINT;
            g.Triangles.IndexCount = t.indexCount;
            g.Triangles.IndexBuffer = t.indexAddress != 0 ? t.indexAddress : static_cast<D3DBuffer*>(t.indices)->gpuAddress() + t.indexOffset;
        }
        out.push_back(g);
    }
}

}  // namespace

// prebuild info -> storage (+ scratch for a TLAS). The storage is created in RAYTRACING_ACCELERATION_STRUCTURE state (it stays there: such a resource
// cannot be transitioned) with UAV access for the builds; sizes are rounded up to the 256 byte alignment DXR wants.
bool D3DAccel::allocate(const D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS& inputs, bool scratch) {
    D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO info{};
    dev_.device5()->GetRaytracingAccelerationStructurePrebuildInfo(&inputs, &info);
    if (info.ResultDataMaxSizeInBytes == 0) return false;
    buildScratch_ = alignUp(std::max<std::uint64_t>(info.ScratchDataSizeInBytes, 256), 256);
    updateScratch_ = alignUp(std::max<std::uint64_t>(info.UpdateScratchDataSizeInBytes, 256), 256);
    storageBytes_ = alignUp(info.ResultDataMaxSizeInBytes, 256);
    if (tlas_) {
        storage_ = dev_.createDefaultBuffer(storageBytes_, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE);
        if (!storage_) return false;
        storage_->SetName(L"rhi tlas");
    } else if (!storageFor(storageBytes_, L"rhi blas")) {
        return false;
    }
    if (scratch) {
        // (the scratch is created in COMMON, which is what a buffer starts in whatever is asked for; the build promotes it to UNORDERED_ACCESS implicitly
        // and it decays back when the command list has executed)
        scratch_ = dev_.createDefaultBuffer(buildScratch_, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COMMON);
        if (!scratch_) return false;
        scratch_->SetName(L"rhi build scratch");
    }
    return true;
}

bool D3DAccel::createBlas(const BlasDesc& desc) {
    geometries_.assign(desc.geometries.begin(), desc.geometries.end());
    allowUpdate_ = desc.allowUpdate;
    // (AMD's finding: ALLOW_UPDATE + ALLOW_COMPACTION on one BLAS corrupts the build: a refittable BLAS is not compacted)
    const bool compaction = desc.allowCompaction && !desc.allowUpdate;
    flags_ = (desc.preferFastBuild ? D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_BUILD
                                   : D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_TRACE) |
             (desc.allowUpdate ? D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_ALLOW_UPDATE : D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_NONE) |
             (compaction ? D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_ALLOW_COMPACTION : D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_NONE);
    std::vector<D3D12_RAYTRACING_GEOMETRY_DESC> descs;
    geometryDescs(geometries_, descs);
    D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS in{};
    in.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL;
    in.Flags = flags_;
    in.NumDescs = static_cast<UINT>(descs.size());
    in.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;
    in.pGeometryDescs = descs.data();
    return allocate(in, false);
}

// 14.22: a BLAS's storage from the arena, a dedicated buffer when it does not fit a chunk
bool D3DAccel::storageFor(std::uint64_t bytes, const wchar_t* name) {
    if (dev_.asAlloc(bytes, span_, arenaBuffer_)) return true;
    span_ = {};
    arenaBuffer_ = nullptr;
    ++dev_.asStats_.dedicated;
    storage_ = dev_.createDefaultBuffer(bytes, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE);
    if (!storage_) return false;
    storage_->SetName(name);
    return true;
}

bool D3DAccel::createStorage(std::uint64_t bytes, const char* name) {
    storageBytes_ = alignUp(bytes, 256);
    return storageFor(storageBytes_, widen(name).c_str());
}

bool D3DAccel::createTlas(const TlasDesc& desc) {
    maxInstances_ = desc.maxInstances;
    flags_ = desc.preferFastBuild ? D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_BUILD
                                  : D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_TRACE;
    D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS in{};
    in.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL;
    in.Flags = flags_;
    in.NumDescs = desc.maxInstances;
    in.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;
    if (!allocate(in, true)) return false;
    // the heap entry shaders reach the TLAS through (RVK_TLAS)
    slot_ = dev_.srvAlloc().alloc();
    if (slot_ == 0) return false;
    D3D12_SHADER_RESOURCE_VIEW_DESC sd{};
    sd.ViewDimension = D3D12_SRV_DIMENSION_RAYTRACING_ACCELERATION_STRUCTURE;
    sd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    sd.RaytracingAccelerationStructure.Location = storage_->GetGPUVirtualAddress();
    dev_.device()->CreateShaderResourceView(nullptr, &sd, dev_.srvCpu(slot_));
    return true;
}

bool D3DSizeQuery::create() {
    gpu_ = dev_.createDefaultBuffer(alignUp(static_cast<std::uint64_t>(capacity_) * 8, 256), D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
                                    D3D12_RESOURCE_STATE_COMMON);  // (buffers always start in COMMON)
    if (!gpu_) return false;
    gpu_->SetName(L"rhi compacted sizes");
    BufferDesc bd;
    bd.size = alignUp(static_cast<std::uint64_t>(capacity_) * 8, 256);
    bd.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    bd.memory = Memory::Readback;
    bd.name = "rhi compacted sizes readback";
    readback_ = dev_.createBuffer(bd);
    return readback_ != nullptr;
}

// ---- command list -----------------------------------------------------------------------------------------------------------

class D3DCommandList final : public CommandList {
public:
    D3DCommandList(D3D12DeviceImpl& dev, ID3D12GraphicsCommandList* list) : dev_(dev), list_(list) {}
    // the game device's lists: a new command list segment (the bindings of the old one are gone)
    void rebind(ID3D12GraphicsCommandList* list) {
        list_ = list;
        list4_.Reset();
        resetBindings();
    }
    void resetBindings() {
        pipeline_ = nullptr;
        dirtyConstants_ = dirtyCb_ = dirtyTable_ = false;
        constantCount_ = 0;
        cbAddress_ = 0;
        tableFirst_ = 0;
    }

    void transition(Texture& texture, VkImageLayout layout) override {
        D3DTexture& t = static_cast<D3DTexture&>(texture);
        if (layout == VK_IMAGE_LAYOUT_UNDEFINED) {
            t.state = D3D12_RESOURCE_STATE_COMMON;  // the contents are discarded: no barrier, the next one starts from COMMON
            t.split = false;
            t.lastLayout = VK_IMAGE_LAYOUT_UNDEFINED;
            return;
        }
        transitionRange(t, stateFor(t, layout), 0, t.desc().mips);
        t.lastLayout = layout;
    }
    void transitionMips(Texture& texture, VkImageLayout layout, std::uint32_t baseMip, std::uint32_t mipCount) override {
        if (layout == VK_IMAGE_LAYOUT_UNDEFINED) return;
        D3DTexture& t = static_cast<D3DTexture&>(texture);
        transitionRange(t, stateFor(t, layout), baseMip, mipCount);
        t.lastLayout = layout;
    }

    // ShaderRead -> ALL_SHADER_RESOURCE (the state of an SRV), ShaderWrite -> UNORDERED_ACCESS; an UAV -> UAV step is a UAV barrier (the writes of the
    // previous dispatch become visible); a transition out of UNORDERED_ACCESS flushes the writes by itself
    void bufferBarrier(Buffer& buffer, BufferAccess next) override {
        D3DBuffer& b = static_cast<D3DBuffer&>(buffer);
        if (b.desc().memory != Memory::Device) return;  // upload / readback heaps keep their one state
        const D3D12_RESOURCE_STATES to = next == BufferAccess::ShaderWrite ? D3D12_RESOURCE_STATE_UNORDERED_ACCESS
                                         : next == BufferAccess::IndirectRead ? D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT
                                                                              : D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE;
        D3D12_RESOURCE_BARRIER rb{};
        if (b.state == to) {
            if (to != D3D12_RESOURCE_STATE_UNORDERED_ACCESS) return;
            rb.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
            rb.UAV.pResource = b.resource();
        } else {
            rb.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            rb.Transition = {b.resource(), D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, b.state, to};
            b.state = to;
        }
        list_->ResourceBarrier(1, &rb);
    }

    void transitions(std::span<const TextureTransition> list) override {
        for (const TextureTransition& t : list) {
            if (t.layout != VK_IMAGE_LAYOUT_UNDEFINED) {
                transitionRange(*static_cast<D3DTexture*>(t.texture), stateFor(*static_cast<D3DTexture*>(t.texture), t.layout), t.baseMip, t.mipCount);
                static_cast<D3DTexture*>(t.texture)->lastLayout = t.layout;
            }
        }
    }
    void computeBarrier() override { uavBarrier(); }

    void setPushConstants(const void* data, std::uint32_t size) override {
        std::memset(constants_.data(), 0, sizeof(constants_));
        std::memcpy(constants_.data(), data, std::min<std::size_t>(size, sizeof(constants_)));
        constantCount_ = (size + 3) / 4;
        dirtyConstants_ = true;
    }
    void setConstantBuffer(Buffer& buffer, std::uint64_t offset, std::uint64_t) override {
        cbAddress_ = static_cast<D3DBuffer&>(buffer).gpuAddress() + offset;
        dirtyCb_ = true;
    }
    // One UAV per slot in a fresh table of the ring; the view dimension follows the texture (rhi.h StorageView): 3D, 2D array of one slice, or 2D.
    void setStorageViews(std::span<const StorageView> views) override {
        tableFirst_ = dev_.nextUavTable();
        for (std::uint32_t i = 0; i < rhi::kStorageImages; ++i) {
            if (i >= views.size()) break;  // (the slots a pass does not name are not written: nothing reads them)
            D3D12_UNORDERED_ACCESS_VIEW_DESC ud{};
            ud.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
            ID3D12Resource* res = nullptr;
            if (i < views.size() && views[i].texture != nullptr) {
                D3DTexture* t = static_cast<D3DTexture*>(views[i].texture);
                const TextureDesc& d = t->desc();
                res = t->resource();
                ud.Format = toDxgi(d.format);
                if (d.type == VK_IMAGE_TYPE_3D) {
                    ud.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE3D;
                    ud.Texture3D.MipSlice = views[i].mip;
                    ud.Texture3D.FirstWSlice = 0;
                    ud.Texture3D.WSize = std::max(d.extent.depth >> views[i].mip, 1u);
                } else if (d.layers > 1) {
                    ud.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2DARRAY;
                    ud.Texture2DArray.MipSlice = views[i].mip;
                    ud.Texture2DArray.FirstArraySlice = views[i].layer;
                    ud.Texture2DArray.ArraySize = 1;
                } else {
                    ud.Texture2D.MipSlice = views[i].mip;
                }
            } else {
                ud.Format = DXGI_FORMAT_R8G8B8A8_UNORM;  // a null descriptor
            }
            dev_.device()->CreateUnorderedAccessView(res, nullptr, &ud, dev_.srvCpu(tableFirst_ + i));
        }
        dirtyTable_ = true;
    }

    void bindPipeline(Pipeline& pipeline) override {
        pipeline_ = static_cast<D3DPipeline*>(&pipeline);
        if (pipeline_->compute) {
            list_->SetComputeRootSignature(dev_.rootSignature());
        } else {
            list_->SetGraphicsRootSignature(dev_.rootSignature());
            list_->IASetPrimitiveTopology(pipeline_->topology);
        }
        list_->SetPipelineState(pipeline_->pso.Get());
        dirtyConstants_ = dirtyCb_ = dirtyTable_ = true;
    }

    void dispatch(std::uint32_t x, std::uint32_t y, std::uint32_t z) override {
        dev_.captureForRead(list_, false);  // (a dispatch that reads captures: the pool leaves the state the vertex shaders wrote it in)
        flush();
        list_->Dispatch(x, y, z);
    }

    // (render target / depth views of the subresource are made per call into rings, like the UAV tables)
    void beginRendering(const RenderingInfo& info) override {
        std::array<D3D12_CPU_DESCRIPTOR_HANDLE, 4> rtv{};
        for (std::uint32_t i = 0; i < info.colorCount; ++i) {
            const auto& a = info.color[i];
            D3DTexture* t = static_cast<D3DTexture*>(a.texture);
            rtv[i] = dev_.rtvCpu(dev_.nextRtv());
            D3D12_RENDER_TARGET_VIEW_DESC rv{};
            rv.Format = toDxgi(t->desc().format);
            if (t->desc().layers > 1) {
                rv.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2DARRAY;
                rv.Texture2DArray.MipSlice = a.mip;
                rv.Texture2DArray.FirstArraySlice = a.layer;
                rv.Texture2DArray.ArraySize = 1;
            } else {
                rv.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
                rv.Texture2D.MipSlice = a.mip;
            }
            dev_.device()->CreateRenderTargetView(t->resource(), &rv, rtv[i]);
            if (a.load == VK_ATTACHMENT_LOAD_OP_CLEAR) {
                const float c[4] = {a.clear.float32[0], a.clear.float32[1], a.clear.float32[2], a.clear.float32[3]};
                list_->ClearRenderTargetView(rtv[i], c, 0, nullptr);
            }
        }
        D3D12_CPU_DESCRIPTOR_HANDLE dsv{};
        if (info.depth.texture != nullptr) {
            D3DTexture* t = static_cast<D3DTexture*>(info.depth.texture);
            dsv = dev_.dsvCpu(dev_.nextDsv());
            D3D12_DEPTH_STENCIL_VIEW_DESC dd{};
            dd.Format = toDxgi(t->desc().format);
            if (t->desc().layers > 1) {
                dd.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2DARRAY;
                dd.Texture2DArray.MipSlice = info.depth.mip;
                dd.Texture2DArray.FirstArraySlice = info.depth.layer;
                dd.Texture2DArray.ArraySize = 1;
            } else {
                dd.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
                dd.Texture2D.MipSlice = info.depth.mip;
            }
            dev_.device()->CreateDepthStencilView(t->resource(), &dd, dsv);
            if (info.depth.load == VK_ATTACHMENT_LOAD_OP_CLEAR) {
                list_->ClearDepthStencilView(dsv, D3D12_CLEAR_FLAG_DEPTH, info.depthClear.depth, static_cast<UINT8>(info.depthClear.stencil), 0, nullptr);
            }
        }
        list_->OMSetRenderTargets(info.colorCount, rtv.data(), FALSE, info.depth.texture != nullptr ? &dsv : nullptr);
        setViewport(0.0f, 0.0f, static_cast<float>(info.width), static_cast<float>(info.height));
        setScissor(0, 0, info.width, info.height);
    }
    void endRendering() override {}
    void setViewport(float x, float y, float w, float h) override {
        const D3D12_VIEWPORT vp{x, y, w, h, 0.0f, 1.0f};
        list_->RSSetViewports(1, &vp);
    }
    void setScissor(std::int32_t x, std::int32_t y, std::uint32_t w, std::uint32_t h) override {
        const D3D12_RECT r{x, y, x + static_cast<LONG>(w), y + static_cast<LONG>(h)};
        list_->RSSetScissorRects(1, &r);
    }
    void draw(std::uint32_t vertexCount, std::uint32_t instanceCount, std::uint32_t firstVertex, std::uint32_t firstInstance) override {
        flush();
        list_->DrawInstanced(vertexCount, instanceCount, firstVertex, firstInstance);
    }

    // vertex / index / indirect sources move to their read state here (device memory; upload heaps are GENERIC_READ already)
    void setVertexBuffers(std::uint32_t first, std::span<Buffer* const> buffers, std::span<const std::uint64_t> offsets,
                          std::span<const std::uint32_t> strides) override {
        std::array<D3D12_VERTEX_BUFFER_VIEW, 8> views{};
        const std::uint32_t n = static_cast<std::uint32_t>(std::min<std::size_t>(buffers.size(), views.size()));
        for (std::uint32_t i = 0; i < n; ++i) {
            D3DBuffer& b = *static_cast<D3DBuffer*>(buffers[i]);
            bufferState(b, D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER);
            const std::uint64_t off = i < offsets.size() ? offsets[i] : 0;
            views[i].BufferLocation = b.gpuAddress() + off;
            views[i].SizeInBytes = static_cast<UINT>(b.desc().size - off);
            views[i].StrideInBytes = i < strides.size() ? strides[i] : 0;
        }
        list_->IASetVertexBuffers(first, n, views.data());
    }
    void setIndexBuffer(Buffer& buffer, std::uint64_t offset, VkIndexType type) override {
        D3DBuffer& b = static_cast<D3DBuffer&>(buffer);
        bufferState(b, D3D12_RESOURCE_STATE_INDEX_BUFFER);
        D3D12_INDEX_BUFFER_VIEW v{};
        v.BufferLocation = b.gpuAddress() + offset;
        v.SizeInBytes = static_cast<UINT>(b.desc().size - offset);
        v.Format = type == VK_INDEX_TYPE_UINT16 ? DXGI_FORMAT_R16_UINT : DXGI_FORMAT_R32_UINT;
        list_->IASetIndexBuffer(&v);
    }
    void drawIndexed(std::uint32_t indexCount, std::uint32_t instanceCount, std::uint32_t firstIndex, std::int32_t vertexOffset,
                     std::uint32_t firstInstance) override {
        flush();
        list_->DrawIndexedInstanced(indexCount, instanceCount, firstIndex, vertexOffset, firstInstance);
    }
    void drawIndexedIndirect(Buffer& args, std::uint64_t argsOffset, std::uint32_t drawCount, std::uint32_t stride) override {
        D3DBuffer& a = static_cast<D3DBuffer&>(args);
        bufferState(a, D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT);
        flush();
        list_->ExecuteIndirect(dev_.drawIndexedSignature(stride), drawCount, a.resource(), argsOffset, nullptr, 0);
    }
    // ExecuteIndirect with a DRAW_INDEXED signature: D3D12_DRAW_INDEXED_ARGUMENTS is byte-identical to VkDrawIndexedIndirectCommand
    void drawIndexedIndirectCount(Buffer& args, std::uint64_t argsOffset, Buffer& countBuffer, std::uint64_t countOffset, std::uint32_t maxDraws,
                                  std::uint32_t stride) override {
        D3DBuffer& a = static_cast<D3DBuffer&>(args);
        D3DBuffer& c = static_cast<D3DBuffer&>(countBuffer);
        bufferState(a, D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT);
        bufferState(c, D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT);
        flush();
        list_->ExecuteIndirect(dev_.drawIndexedSignature(stride), maxDraws, a.resource(), argsOffset, c.resource(), countOffset);
    }

    void copyBuffer(Buffer& dst, std::uint64_t dstOffset, Buffer& src, std::uint64_t srcOffset, std::uint64_t size) override {
        D3DBuffer& d = static_cast<D3DBuffer&>(dst);
        D3DBuffer& s = static_cast<D3DBuffer&>(src);
        bufferState(d, D3D12_RESOURCE_STATE_COPY_DEST);
        bufferState(s, D3D12_RESOURCE_STATE_COPY_SOURCE);
        list_->CopyBufferRegion(d.resource(), dstOffset, s.resource(), srcOffset, size);
        if (d.arena) bufferState(d, D3D12_RESOURCE_STATE_GENERIC_READ);  // the arena's blocks live in GENERIC_READ
        // data copied into device memory is read next (vertex / index data, acceleration structure builds from its address: they need a shader-read state)
        else if (d.desc().memory == Memory::Device) bufferState(d, D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE);
    }
    void copyBufferToTexture(Texture& dst, Buffer& src, std::uint64_t srcOffset, Subresource sub) override {
        D3DTexture& t = static_cast<D3DTexture&>(dst);
        D3DBuffer& b = static_cast<D3DBuffer&>(src);
        bufferState(b, D3D12_RESOURCE_STATE_COPY_SOURCE);
        const VkExtent3D e = mipExtent(t.desc(), sub.mip);
        D3D12_TEXTURE_COPY_LOCATION dl{};
        dl.pResource = t.resource();
        dl.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        dl.SubresourceIndex = t.subresource(sub);
        D3D12_TEXTURE_COPY_LOCATION sl{};
        sl.pResource = b.resource();
        sl.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        sl.PlacedFootprint.Offset = srcOffset;
        sl.PlacedFootprint.Footprint = {toDxgi(t.desc().format), e.width, e.height, e.depth, bufferRowPitch(t.desc().format, e.width)};
        list_->CopyTextureRegion(&dl, 0, 0, 0, &sl, nullptr);
    }
    void copyBufferToTextureBox(Texture& dst, Buffer& src, const BufferTextureCopy& c) override {
        D3DTexture& t = static_cast<D3DTexture&>(dst);
        D3DBuffer& b = static_cast<D3DBuffer&>(src);
        bufferState(b, D3D12_RESOURCE_STATE_COPY_SOURCE);
        D3D12_TEXTURE_COPY_LOCATION dl{};
        dl.pResource = t.resource();
        dl.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        dl.SubresourceIndex = t.subresource(c.sub);
        D3D12_TEXTURE_COPY_LOCATION sl{};
        sl.pResource = b.resource();
        sl.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        sl.PlacedFootprint.Offset = c.bufferOffset;
        const std::uint32_t pitch = c.rowPitch != 0 ? c.rowPitch : bufferRowPitch(t.desc().format, c.extent.width);
        sl.PlacedFootprint.Footprint = {toDxgi(t.desc().format), c.extent.width, c.extent.height, c.extent.depth, pitch};
        list_->CopyTextureRegion(&dl, static_cast<UINT>(c.offset.x), static_cast<UINT>(c.offset.y), static_cast<UINT>(c.offset.z), &sl, nullptr);
    }
    void copyTextureToBuffer(Buffer& dst, std::uint64_t dstOffset, Texture& src, Subresource sub) override {
        D3DTexture& t = static_cast<D3DTexture&>(src);
        D3DBuffer& b = static_cast<D3DBuffer&>(dst);
        bufferState(b, D3D12_RESOURCE_STATE_COPY_DEST);
        const VkExtent3D e = mipExtent(t.desc(), sub.mip);
        D3D12_TEXTURE_COPY_LOCATION dl{};
        dl.pResource = b.resource();
        dl.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        dl.PlacedFootprint.Offset = dstOffset;
        dl.PlacedFootprint.Footprint = {toDxgi(t.desc().format), e.width, e.height, e.depth, bufferRowPitch(t.desc().format, e.width)};
        D3D12_TEXTURE_COPY_LOCATION sl{};
        sl.pResource = t.resource();
        sl.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        sl.SubresourceIndex = t.subresource(sub);
        list_->CopyTextureRegion(&dl, 0, 0, 0, &sl, nullptr);
    }

    // Clears use a UAV of the level in the ring (the GPU handle) and the same view in the CPU-only heap (the handle ClearUnorderedAccessView* reads).
    void clearTexture(Texture& texture, const VkClearColorValue& value, std::uint32_t baseMip, std::uint32_t mipCount) override {
        D3DTexture& t = static_cast<D3DTexture&>(texture);
        const TextureDesc& d = t.desc();
        const bool uintFormat = d.format == VK_FORMAT_R8_UINT || d.format == VK_FORMAT_R16_UINT || d.format == VK_FORMAT_R32_UINT;
        mipCount = std::min(mipCount, d.mips - std::min(baseMip, d.mips));
        for (std::uint32_t m = baseMip; m < baseMip + mipCount; ++m) {
            D3D12_UNORDERED_ACCESS_VIEW_DESC ud{};
            ud.Format = toDxgi(d.format);
            if (d.type == VK_IMAGE_TYPE_3D) {
                ud.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE3D;
                ud.Texture3D.MipSlice = m;
                ud.Texture3D.WSize = std::max(d.extent.depth >> m, 1u);
            } else if (d.layers > 1) {
                ud.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2DARRAY;
                ud.Texture2DArray.MipSlice = m;
                ud.Texture2DArray.ArraySize = d.layers;
            } else {
                ud.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
                ud.Texture2D.MipSlice = m;
            }
            const std::uint32_t slot = dev_.nextUavTable();
            dev_.device()->CreateUnorderedAccessView(t.resource(), nullptr, &ud, dev_.srvCpu(slot));
            dev_.device()->CreateUnorderedAccessView(t.resource(), nullptr, &ud, dev_.uavClearCpu());
            if (uintFormat) list_->ClearUnorderedAccessViewUint(dev_.srvGpu(slot), dev_.uavClearCpu(), t.resource(), value.uint32, 0, nullptr);
            else list_->ClearUnorderedAccessViewFloat(dev_.srvGpu(slot), dev_.uavClearCpu(), t.resource(), value.float32, 0, nullptr);
        }
    }
    void copyTexture(Texture& dst, Texture& src, std::span<const TextureCopy> regions) override {
        D3DTexture& d = static_cast<D3DTexture&>(dst);
        D3DTexture& s = static_cast<D3DTexture&>(src);
        for (const TextureCopy& c : regions) {
            const bool whole = c.extent.width == 0 && c.extent.height == 0 && c.extent.depth == 0;
            const VkExtent3D e = whole ? mipExtent(s.desc(), c.src.mip) : c.extent;
            const D3D12_BOX box{static_cast<UINT>(c.srcOffset.x), static_cast<UINT>(c.srcOffset.y), static_cast<UINT>(c.srcOffset.z),
                                static_cast<UINT>(c.srcOffset.x) + e.width, static_cast<UINT>(c.srcOffset.y) + e.height,
                                static_cast<UINT>(c.srcOffset.z) + e.depth};
            const bool boxed = !whole || c.srcOffset.x != 0 || c.srcOffset.y != 0 || c.srcOffset.z != 0;
            for (std::uint32_t l = 0; l < c.layerCount; ++l) {
                D3D12_TEXTURE_COPY_LOCATION dl{};
                dl.pResource = d.resource();
                dl.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
                dl.SubresourceIndex = d.subresource({c.dst.mip, c.dst.layer + l});
                D3D12_TEXTURE_COPY_LOCATION sl{};
                sl.pResource = s.resource();
                sl.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
                sl.SubresourceIndex = s.subresource({c.src.mip, c.src.layer + l});
                list_->CopyTextureRegion(&dl, static_cast<UINT>(c.dstOffset.x), static_cast<UINT>(c.dstOffset.y), static_cast<UINT>(c.dstOffset.z), &sl,
                                         boxed ? &box : nullptr);
            }
        }
    }
    // A scaled copy is a draw into the destination (D3D12 has no scaling copy): colour 2D destinations that can be render targets; a same-size, same-format
    // copy of anything else is a CopyTextureRegion. The subresource states are restored by blitDraw / not touched by the copy path.
    bool blitTexture(Texture& dstT, Texture& srcT, std::span<const TextureBlit> regions, VkFilter filter) override {
        D3DTexture& d = static_cast<D3DTexture&>(dstT);
        D3DTexture& s = static_cast<D3DTexture&>(srcT);
        bool ok = true;
        for (const TextureBlit& r : regions) {
            const auto whole = [](const VkExtent3D& e) { return e.width == 0 && e.height == 0 && e.depth == 0; };
            const VkExtent3D sm = mipExtent(s.desc(), r.src.mip), dm = mipExtent(d.desc(), r.dst.mip);
            const VkExtent3D se = whole(r.srcExtent) ? sm : r.srcExtent, de = whole(r.dstExtent) ? dm : r.dstExtent;
            const bool drawable = (d.desc().usage & VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT) != 0 && d.desc().type == VK_IMAGE_TYPE_2D && !isDepth(d.desc().format) &&
                                  s.desc().type == VK_IMAGE_TYPE_2D;
            if (drawable) {
                const float uv[4] = {static_cast<float>(r.srcOffset.x) / static_cast<float>(sm.width), static_cast<float>(r.srcOffset.y) / static_cast<float>(sm.height),
                                     static_cast<float>(se.width) / static_cast<float>(sm.width), static_cast<float>(se.height) / static_cast<float>(sm.height)};
                const D3D12_RECT dr{r.dstOffset.x, r.dstOffset.y, r.dstOffset.x + static_cast<LONG>(de.width), r.dstOffset.y + static_cast<LONG>(de.height)};
                ok = dev_.blitDraw(list_, d, r.dst, dr, s, r.src, uv, filter == VK_FILTER_LINEAR) && ok;
                resetBindings();
                dev_.invalidateMainRec();
            } else if (se.width == de.width && se.height == de.height && s.desc().format == d.desc().format) {
                TextureCopy c;
                c.src = r.src;
                c.dst = r.dst;
                c.srcOffset = r.srcOffset;
                c.dstOffset = r.dstOffset;
                c.extent = se;
                if (isDepth(s.desc().format) && (se.width != sm.width || se.height != sm.height)) c.extent = {0, 0, 0};
                copyTexture(dstT, srcT, std::span<const TextureCopy>(&c, 1));
            } else {
                spdlog::error("d3d12: blitTexture {}x{} -> {}x{} of format {} is not supported", se.width, se.height, de.width, de.height, static_cast<int>(s.desc().format));
                ok = false;
            }
        }
        return ok;
    }

    // Acceleration structure builds (ID3D12GraphicsCommandList4). The inputs (vertex / index / instance buffers) must be in a shader read state: device
    // memory buffers are moved to ALL_SHADER_RESOURCE here (upload heaps are GENERIC_READ). The structures themselves live in
    // RAYTRACING_ACCELERATION_STRUCTURE state for good; each build ends with a UAV barrier on all UAV resources (null resource) so that the next build
    // (a TLAS over this BLAS) and the ray queries of later dispatches read the finished structure; the scratch buffer (promoted from COMMON to
    // UNORDERED_ACCESS by the build) is covered by the same barrier when the next build in this list reuses it.
    void buildBlas(AccelerationStructure& as) override {
        const BlasBuild b{&as, {}, false};
        buildBlases(std::span<const BlasBuild>(&b, 1));
    }
    void buildBlases(std::span<const BlasBuild> builds) override {
        if (builds.empty()) return;
        dev_.captureForRead(list_, true);  // (captured vertices as build input: the pool in a shader-read state)
        std::uint64_t largest = 0;
        for (const BlasBuild& b : builds) {
            D3DAccel& a = static_cast<D3DAccel&>(*b.blas);
            largest = std::max(largest, a.scratchNeed(b.update && a.allowUpdate()));
        }
        if (!dev_.ensureScratch(largest)) {
            spdlog::error("d3d12: the BLAS build scratch ({} bytes) could not be allocated", largest);
            return;
        }
        std::uint64_t used = 0;
        for (const BlasBuild& b : builds) {
            D3DAccel& a = static_cast<D3DAccel&>(*b.blas);
            const bool update = b.update && a.allowUpdate();
            const std::uint64_t need = a.scratchNeed(update);
            if (used + need > dev_.scratchSize()) {
                uavBarrier();  // the pool is full: the builds so far finish before it is reused
                used = 0;
            }
            const std::span<const TriangleGeometry> geometries = b.geometries.empty() ? std::span<const TriangleGeometry>(a.geometries()) : b.geometries;
            for (const TriangleGeometry& t : geometries) {
                if (t.vertexAddress == 0 && t.vertices != nullptr) bufferState(*static_cast<D3DBuffer*>(t.vertices), D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE);
                if (t.indexAddress == 0 && t.indices != nullptr) bufferState(*static_cast<D3DBuffer*>(t.indices), D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE);
            }
            std::vector<D3D12_RAYTRACING_GEOMETRY_DESC> descs;
            geometryDescs(geometries, descs);
            D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC bd{};
            bd.DestAccelerationStructureData = a.address();
            bd.ScratchAccelerationStructureData = dev_.scratchAddress() + used;
            bd.Inputs.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL;
            bd.Inputs.Flags = a.flags() | (update ? D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PERFORM_UPDATE : D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_NONE);
            bd.Inputs.NumDescs = static_cast<UINT>(descs.size());
            bd.Inputs.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;
            bd.Inputs.pGeometryDescs = descs.data();
            if (update) bd.SourceAccelerationStructureData = a.address();
            used += need;
            list4()->BuildRaytracingAccelerationStructure(&bd, 0, nullptr);
        }
        uavBarrier();
    }
    void buildTlas(AccelerationStructure& as, Buffer& instances, std::uint64_t instanceOffset, std::uint32_t count) override {
        D3DBuffer& ib = static_cast<D3DBuffer&>(instances);
        bufferState(ib, D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE);
        buildTlasAt(as, ib.gpuAddress() + instanceOffset, count);
    }
    void buildTlasAt(AccelerationStructure& as, std::uint64_t instancesAddress, std::uint32_t count) override {
        D3DAccel& a = static_cast<D3DAccel&>(as);
        dev_.captureForRead(list_, true);
        D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC bd{};
        bd.DestAccelerationStructureData = a.address();
        bd.ScratchAccelerationStructureData = a.scratchAddress();
        bd.Inputs.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL;
        bd.Inputs.Flags = a.flags();
        bd.Inputs.NumDescs = std::min(count, a.maxInstances());
        bd.Inputs.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;
        bd.Inputs.InstanceDescs = instancesAddress;
        uavBarrier();
        list4()->BuildRaytracingAccelerationStructure(&bd, 0, nullptr);
        uavBarrier();
    }
    void queryCompactedSizes(SizeQuery& query, std::uint32_t first, std::span<AccelerationStructure* const> blases) override {
        if (blases.empty()) return;
        D3DSizeQuery& q = static_cast<D3DSizeQuery&>(query);
        std::vector<D3D12_GPU_VIRTUAL_ADDRESS> sources;
        for (AccelerationStructure* b : blases) sources.push_back(b->address());
        const std::uint32_t n = static_cast<std::uint32_t>(sources.size());
        uavBarrier();
        if (!q.inUav) {
            D3D12_RESOURCE_BARRIER tb{};
            tb.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            tb.Transition = {q.gpu(), D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_UNORDERED_ACCESS};
            list_->ResourceBarrier(1, &tb);
            q.inUav = true;
        }
        D3D12_RAYTRACING_ACCELERATION_STRUCTURE_POSTBUILD_INFO_DESC pd{};
        pd.DestBuffer = q.gpu()->GetGPUVirtualAddress() + static_cast<std::uint64_t>(first) * 8;
        pd.InfoType = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_POSTBUILD_INFO_COMPACTED_SIZE;
        list4()->EmitRaytracingAccelerationStructurePostbuildInfo(&pd, n, sources.data());
        // the sizes -> the readback buffer
        D3D12_RESOURCE_BARRIER rb{};
        rb.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        rb.Transition = {q.gpu(), D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE};
        list_->ResourceBarrier(1, &rb);
        list_->CopyBufferRegion(q.readbackBuffer().resource(), static_cast<std::uint64_t>(first) * 8, q.gpu(), static_cast<std::uint64_t>(first) * 8,
                                static_cast<std::uint64_t>(n) * 8);
        rb.Transition = {q.gpu(), D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS};
        list_->ResourceBarrier(1, &rb);
        q.markWritten(first, n);
    }
    void compactBlas(AccelerationStructure& dst, AccelerationStructure& src) override {
        uavBarrier();
        list4()->CopyRaytracingAccelerationStructure(dst.address(), src.address(), D3D12_RAYTRACING_ACCELERATION_STRUCTURE_COPY_MODE_COMPACT);
        uavBarrier();
    }
    void memoryBarrier(VkPipelineStageFlags2, VkAccessFlags2, VkPipelineStageFlags2, VkAccessFlags2) override { uavBarrier(); }
    void fillBuffer(Buffer& dst, std::uint64_t offset, std::uint64_t size, std::uint32_t value) override {
        D3DBuffer& b = static_cast<D3DBuffer&>(dst);
        bufferState(b, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        D3D12_UNORDERED_ACCESS_VIEW_DESC ud{};
        ud.Format = DXGI_FORMAT_R32_UINT;
        ud.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
        ud.Buffer.FirstElement = offset / 4;
        ud.Buffer.NumElements = static_cast<UINT>(size / 4);
        const std::uint32_t slot = dev_.nextUavTable();
        dev_.device()->CreateUnorderedAccessView(b.resource(), nullptr, &ud, dev_.srvCpu(slot));
        dev_.device()->CreateUnorderedAccessView(b.resource(), nullptr, &ud, dev_.uavClearCpu());
        const UINT v[4] = {value, value, value, value};
        list_->ClearUnorderedAccessViewUint(dev_.srvGpu(slot), dev_.uavClearCpu(), b.resource(), v, 0, nullptr);
        uavBarrier();
    }

    // capture regions (PIX / the DRED breadcrumbs' BeginEvent ops): PIX3 encoded events written by the header-only encoder
    void beginRegion(std::string_view name) override {
        // (PIXBeginEvent(list, ...) also records a CPU timeline event, which needs WinPixEventRuntime.dll: the GPU marker alone is encoded here)
        const std::string n(name);
        UINT64 blob[PIXEventsGraphicsRecordSpaceQwords];
        UINT64* end = PixEventsLegacy::EncodeBeginEventForContext(blob, static_cast<UINT64>(PIX_COLOR_DEFAULT), "%s", n.c_str());
        list_->BeginEvent(D3D12_EVENT_METADATA, blob, static_cast<UINT>(reinterpret_cast<BYTE*>(end) - reinterpret_cast<BYTE*>(blob)));
    }
    void endRegion() override { list_->EndEvent(); }
    ID3D12GraphicsCommandList* raw() const { return list_; }

private:
    // GENERAL is the UAV state for textures with storage usage; one without (a mip chain blitted onto itself) stays in COMMON
    static D3D12_RESOURCE_STATES stateFor(const D3DTexture& t, VkImageLayout layout) {
        if (layout == VK_IMAGE_LAYOUT_GENERAL && (t.desc().usage & VK_IMAGE_USAGE_STORAGE_BIT) == 0) return D3D12_RESOURCE_STATE_COMMON;
        return toState(layout);
    }
    // mip levels [base, base + count) (all layers) to state `to`; a whole texture keeps one state, levels that differ are tracked in mipStates. The
    // same state again is a UAV barrier for UNORDERED_ACCESS (the writes become visible), nothing otherwise.
    void transitionRange(D3DTexture& t, D3D12_RESOURCE_STATES to, std::uint32_t base, std::uint32_t count) {
        const std::uint32_t mips = std::min(t.desc().mips, 16u);
        if (base >= mips || count == 0) return;
        count = std::min(count, mips - base);
        D3D12_RESOURCE_BARRIER b{};
        if (!t.split && base == 0 && count >= t.desc().mips) {
            if (t.state == to) {
                if (to != D3D12_RESOURCE_STATE_UNORDERED_ACCESS) return;
                b.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;  // GENERAL again: the writes become visible
                b.UAV.pResource = t.resource();
            } else {
                b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
                b.Transition = {t.resource(), D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, t.state, to};
                t.state = to;
            }
            list_->ResourceBarrier(1, &b);
            return;
        }
        if (!t.split) {
            t.mipStates.fill(t.state);
            t.split = true;
        }
        std::vector<D3D12_RESOURCE_BARRIER> bs;
        bool uav = false;
        for (std::uint32_t m = base; m < base + count; ++m) {
            if (t.mipStates[m] == to) {
                uav = uav || to == D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
                continue;
            }
            for (std::uint32_t l = 0; l < t.desc().layers; ++l) {
                D3D12_RESOURCE_BARRIER tb{};
                tb.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
                tb.Transition = {t.resource(), m + l * t.desc().mips, t.mipStates[m], to};
                bs.push_back(tb);
            }
            t.mipStates[m] = to;
        }
        if (uav) {
            D3D12_RESOURCE_BARRIER ub{};
            ub.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
            ub.UAV.pResource = t.resource();
            bs.push_back(ub);
        }
        if (!bs.empty()) list_->ResourceBarrier(static_cast<UINT>(bs.size()), bs.data());
        bool same = true;
        for (std::uint32_t m = 1; m < mips; ++m) same = same && t.mipStates[m] == t.mipStates[0];
        if (same) {
            t.state = t.mipStates[0];
            t.split = false;
        }
    }
    // device-heap buffers move between their states (copy, shader, vertex / index / indirect source); upload / readback heaps keep theirs
    void bufferState(D3DBuffer& b, D3D12_RESOURCE_STATES to) {
        if ((b.desc().memory != Memory::Device && !b.arena) || b.state == to) return;
        D3D12_RESOURCE_BARRIER rb{};
        rb.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        rb.Transition = {b.resource(), D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, b.state, to};
        b.state = to;
        list_->ResourceBarrier(1, &rb);
    }
    void uavBarrier() {
        D3D12_RESOURCE_BARRIER rb{};
        rb.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
        rb.UAV.pResource = nullptr;
        list_->ResourceBarrier(1, &rb);
    }
    ID3D12GraphicsCommandList4* list4() {
        if (!list4_) list_->QueryInterface(IID_PPV_ARGS(&list4_));
        return list4_.Get();
    }

    // the root parameters for the bound pipeline's type
    void flush() {
        if (pipeline_ == nullptr) return;
        if (dirtyConstants_ && constantCount_ > 0) {
            if (pipeline_->compute) list_->SetComputeRoot32BitConstants(0, constantCount_, constants_.data(), 0);
            else list_->SetGraphicsRoot32BitConstants(0, constantCount_, constants_.data(), 0);
        }
        if (dirtyCb_ && cbAddress_ != 0) {
            if (pipeline_->compute) list_->SetComputeRootConstantBufferView(1, cbAddress_);
            else list_->SetGraphicsRootConstantBufferView(1, cbAddress_);
        }
        if (dirtyTable_ && tableFirst_ != 0) {
            if (pipeline_->compute) list_->SetComputeRootDescriptorTable(2, dev_.srvGpu(tableFirst_));
            else list_->SetGraphicsRootDescriptorTable(2, dev_.srvGpu(tableFirst_));
        }
        dirtyConstants_ = dirtyCb_ = dirtyTable_ = false;
    }

    D3D12DeviceImpl& dev_;
    ID3D12GraphicsCommandList* list_;
    ComPtr<ID3D12GraphicsCommandList4> list4_;
    D3DPipeline* pipeline_ = nullptr;
    std::array<std::uint32_t, 32> constants_{};
    std::uint32_t constantCount_ = 0;
    D3D12_GPU_VIRTUAL_ADDRESS cbAddress_ = 0;
    std::uint32_t tableFirst_ = 0;
    bool dirtyConstants_ = false, dirtyCb_ = false, dirtyTable_ = false;
};

// ---- swapchain (DXGI flip model) --------------------------------------------------------------------------------------------

class D3DSwapchain final : public Swapchain {
public:
    explicit D3DSwapchain(D3D12DeviceImpl& dev) : dev_(dev) {}
    ~D3DSwapchain() override {
        dev_.waitIdle();
        images_.clear();
    }
    bool create(const SwapchainDesc& desc) {
        vsync_ = desc.vsync;
        DXGI_SWAP_CHAIN_DESC1 sd{};
        sd.Width = desc.width;
        sd.Height = desc.height;
        sd.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        sd.SampleDesc = {1, 0};
        sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        sd.BufferCount = kBuffers;
        sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
        sd.AlphaMode = DXGI_ALPHA_MODE_IGNORE;
        ComPtr<IDXGISwapChain1> sc1;
        RVK_HR(dev_.factory()->CreateSwapChainForHwnd(dev_.queue(), static_cast<HWND>(desc.window), &sd, nullptr, nullptr, &sc1), "CreateSwapChainForHwnd");
        RVK_HR(sc1.As(&sc_), "IDXGISwapChain3");
        dev_.factory()->MakeWindowAssociation(static_cast<HWND>(desc.window), DXGI_MWA_NO_ALT_ENTER);
        width_ = desc.width;
        height_ = desc.height;
        return wrap();
    }
    std::uint32_t width() const override { return width_; }
    std::uint32_t height() const override { return height_; }
    VkFormat format() const override { return VK_FORMAT_B8G8R8A8_UNORM; }
    bool resize(std::uint32_t w, std::uint32_t h) override {
        dev_.waitIdle();
        images_.clear();
        if (FAILED(sc_->ResizeBuffers(kBuffers, w, h, DXGI_FORMAT_B8G8R8A8_UNORM, 0))) return false;
        width_ = w;
        height_ = h;
        return wrap();
    }
    IDXGISwapChain3* swapchain() { return sc_.Get(); }
    D3DTexture& current() { return *images_[sc_->GetCurrentBackBufferIndex()]; }
    bool vsync() const { return vsync_; }

private:
    static constexpr UINT kBuffers = 3;
    bool wrap() {
        TextureDesc td;
        td.format = VK_FORMAT_B8G8R8A8_UNORM;
        td.extent = {width_, height_, 1};
        td.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        td.name = "rhi swapchain image";
        for (UINT i = 0; i < kBuffers; ++i) {
            ComPtr<ID3D12Resource> r;
            RVK_HR(sc_->GetBuffer(i, IID_PPV_ARGS(&r)), "GetBuffer");
            auto t = std::make_unique<D3DTexture>(dev_, td);
            t->adopt(std::move(r));
            t->state = D3D12_RESOURCE_STATE_PRESENT;
            images_.push_back(std::move(t));
        }
        return true;
    }
    D3D12DeviceImpl& dev_;
    ComPtr<IDXGISwapChain3> sc_;
    std::vector<std::unique_ptr<D3DTexture>> images_;
    std::uint32_t width_ = 0, height_ = 0;
    bool vsync_ = true;
};

#include "d3d12_game_present.inc"
#include "d3d12_game_impl.inc"

std::unique_ptr<Swapchain> D3D12DeviceImpl::createSwapchain(const SwapchainDesc& desc) {
    auto s = std::make_unique<D3DSwapchain>(*this);
    if (!s->create(desc)) return nullptr;
    return s;
}

bool D3D12DeviceImpl::renderAndPresent(Swapchain& swapchain, const std::function<void(CommandList&, Texture&)>& record) {
    auto& sw = static_cast<D3DSwapchain&>(swapchain);
    D3DTexture& back = sw.current();
    allocator_->Reset();
    list_->Reset(allocator_.Get(), nullptr);
    ID3D12DescriptorHeap* heaps[2] = {srvHeap_.Get(), samplerHeap_.Get()};
    list_->SetDescriptorHeaps(2, heaps);
    D3DCommandList cl(*this, list_.Get());
    cl.transition(back, VK_IMAGE_LAYOUT_UNDEFINED);  // PRESENT / COMMON are the same state
    record(cl, back);
    cl.transition(back, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR);
    list_->Close();
    ID3D12CommandList* lists[1] = {list_.Get()};
    queue_->ExecuteCommandLists(1, lists);
    const HRESULT hr = sw.swapchain()->Present(sw.vsync() ? 1 : 0, 0);
    if (hr == DXGI_ERROR_DEVICE_REMOVED || hr == DXGI_ERROR_DEVICE_HUNG || hr == DXGI_ERROR_DEVICE_RESET) checkRemoved("Present");
    waitIdle();
    return SUCCEEDED(hr);
}

void D3D12DeviceImpl::submitAndWait(const std::function<void(CommandList&)>& record) {
    allocator_->Reset();
    list_->Reset(allocator_.Get(), nullptr);
    ID3D12DescriptorHeap* heaps[2] = {srvHeap_.Get(), samplerHeap_.Get()};
    list_->SetDescriptorHeaps(2, heaps);
    D3DCommandList cl(*this, list_.Get());
    record(cl);
    list_->Close();
    ID3D12CommandList* lists[1] = {list_.Get()};
    queue_->ExecuteCommandLists(1, lists);
    waitIdle();
}

// the pure parts for rvk_tests (d3d12_test_hooks.h)
struct D3D12TestAccess {
    using Key = D3D12DeviceImpl::RasterKey;
    static Key key(const DynamicStateValues& d) { return D3D12DeviceImpl::rasterKey(d); }
};

}  // namespace

namespace d3d12test {
std::uint32_t dxgiFormat(VkFormat f) { return static_cast<std::uint32_t>(toDxgi(f)); }
std::uint32_t dxgiVertexFormat(VkFormat f) { return static_cast<std::uint32_t>(toDxgiVertex(f)); }
std::uint32_t dxgiResourceFormat(VkFormat f, bool sampled) { return static_cast<std::uint32_t>(resourceFormat(f, sampled)); }
std::uint32_t dxgiSrvFormat(VkFormat f) { return static_cast<std::uint32_t>(srvFormat(f)); }
bool rasterKeysEqual(const DynamicStateValues& a, const DynamicStateValues& b) { return D3D12TestAccess::key(a) == D3D12TestAccess::key(b); }
std::int32_t rasterKeyBias(const DynamicStateValues& d) { return D3D12TestAccess::key(d).bias; }
std::uint32_t rasterKeySize() { return static_cast<std::uint32_t>(sizeof(D3D12TestAccess::Key)); }
std::uint32_t rasterKeyFieldBytes() {
    return static_cast<std::uint32_t>(5 * sizeof(std::uint32_t) + 6 * sizeof(std::uint32_t) + 4 * sizeof(std::uint32_t) + sizeof(std::int32_t) + sizeof(float));
}
}  // namespace d3d12test

std::unique_ptr<GameDevice> createD3D12GameDevice(const GameDeviceOptions& options) {
    auto d = std::make_unique<D3D12DeviceImpl>();
    if (!d->createGame(options)) return nullptr;
    return d;
}

std::vector<AdapterInfo> enumerateD3D12Adapters() {
    std::vector<AdapterInfo> out;
    ComPtr<IDXGIFactory2> factory;
    if (FAILED(CreateDXGIFactory2(0, IID_PPV_ARGS(&factory)))) return out;
    ComPtr<IDXGIFactory6> f6;
    factory.As(&f6);
    for (UINT i = 0;; ++i) {
        ComPtr<IDXGIAdapter1> a;
        const HRESULT hr = f6 ? f6->EnumAdapterByGpuPreference(i, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE, IID_PPV_ARGS(&a)) : factory->EnumAdapters1(i, &a);
        if (FAILED(hr)) break;
        DXGI_ADAPTER_DESC1 ad{};
        a->GetDesc1(&ad);
        if ((ad.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) != 0) continue;
        AdapterInfo info;
        for (const wchar_t* c = ad.Description; *c != 0; ++c) info.name += static_cast<char>(*c < 128 ? *c : '?');
        info.vendorId = ad.VendorId;
        info.deviceId = ad.DeviceId;
        info.vramMB = ad.DedicatedVideoMemory >> 20;
        info.discrete = ad.DedicatedVideoMemory > (512ull << 20);
        info.usable = SUCCEEDED(D3D12CreateDevice(a.Get(), D3D_FEATURE_LEVEL_12_0, __uuidof(ID3D12Device), nullptr));
        out.push_back(std::move(info));
    }
    return out;
}

bool d3d12Usable(int adapter, std::string* reason) {
    D3D12DeviceImpl probe;
    DeviceDesc dd;
    dd.backend = Backend::D3D12;
    dd.adapter = adapter;
    dd.name = "probe";
    const bool ok = probe.create(dd);
    if (!ok && reason != nullptr) *reason = "no adapter with feature level 12_0, Shader Model 6.6 and resource binding tier 3 (see the d3d12: lines above)";
    return ok;
}

std::unique_ptr<Device> createD3D12Device(const DeviceDesc& desc) {
    auto d = std::make_unique<D3D12DeviceImpl>();
    if (!d->create(desc)) return nullptr;
    return d;
}

#include "d3d12_game_ffx.inc"

}  // namespace rvk::rhi
