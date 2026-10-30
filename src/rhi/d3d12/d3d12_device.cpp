// The DX12 backend of the slim RHI (rhi/rhi.h). Requirements (no fallback below them): Shader Model 6.6 (dynamic resources: the HLSL reaches textures,
// samplers and buffers through ResourceDescriptorHeap / SamplerDescriptorHeap) and Resource Binding Tier 3. The root signature is fixed (the RHI's
// binding model, rhi.h): 32 root constants (b0 space0), one root CBV (b8 space1), a table of 4 UAVs (u0..u3 space1), both heaps directly indexed.
// Vulkan structs are the interface's vocabulary and are translated here: formats, image layouts -> resource states, blend / cull / compare enums,
// vertex input -> input layout (semantic ATTR<location>), VkDrawIndexedIndirectCommand -> D3D12_DRAW_INDEXED_ARGUMENTS (same 20 bytes),
// VkAccelerationStructureInstanceKHR -> D3D12_RAYTRACING_INSTANCE_DESC (same 64 bytes).

#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl/client.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstring>
#include <format>
#include <map>
#include <mutex>
#include <string>
#include <vector>

#include <spdlog/spdlog.h>

#include "rhi/rhi.h"

namespace rvk::rhi {
namespace {

using Microsoft::WRL::ComPtr;

constexpr std::uint32_t kHeapSize = 65536;      // shader-visible CBV / SRV / UAV heap (the Vulkan side's bindless arrays together are smaller)
constexpr std::uint32_t kUavRing = 256;         // the last entries of that heap: tables of kStorageImages UAVs written per setStorageViews
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

DXGI_FORMAT toDxgi(VkFormat f) {
    switch (f) {
    case VK_FORMAT_R8_UNORM: return DXGI_FORMAT_R8_UNORM;
    case VK_FORMAT_R8_UINT: return DXGI_FORMAT_R8_UINT;
    case VK_FORMAT_R8G8_UNORM: return DXGI_FORMAT_R8G8_UNORM;
    case VK_FORMAT_R16_SFLOAT: return DXGI_FORMAT_R16_FLOAT;
    case VK_FORMAT_R16_UNORM: return DXGI_FORMAT_R16_UNORM;
    case VK_FORMAT_R16_UINT: return DXGI_FORMAT_R16_UINT;
    case VK_FORMAT_R8G8B8A8_UNORM: return DXGI_FORMAT_R8G8B8A8_UNORM;
    case VK_FORMAT_R8G8B8A8_SRGB: return DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
    case VK_FORMAT_B8G8R8A8_UNORM: return DXGI_FORMAT_B8G8R8A8_UNORM;
    case VK_FORMAT_B8G8R8A8_SRGB: return DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;
    case VK_FORMAT_A2B10G10R10_UNORM_PACK32: return DXGI_FORMAT_R10G10B10A2_UNORM;
    case VK_FORMAT_R16G16_SFLOAT: return DXGI_FORMAT_R16G16_FLOAT;
    case VK_FORMAT_R32_SFLOAT: return DXGI_FORMAT_R32_FLOAT;
    case VK_FORMAT_R32_UINT: return DXGI_FORMAT_R32_UINT;
    case VK_FORMAT_D32_SFLOAT: return DXGI_FORMAT_D32_FLOAT;
    case VK_FORMAT_R16G16B16A16_SFLOAT: return DXGI_FORMAT_R16G16B16A16_FLOAT;
    case VK_FORMAT_R32G32_SFLOAT: return DXGI_FORMAT_R32G32_FLOAT;
    case VK_FORMAT_R32G32B32_SFLOAT: return DXGI_FORMAT_R32G32B32_FLOAT;
    case VK_FORMAT_R32G32B32A32_SFLOAT: return DXGI_FORMAT_R32G32B32A32_FLOAT;
    default: return DXGI_FORMAT_UNKNOWN;
    }
}

bool isDepth(VkFormat f) { return f == VK_FORMAT_D32_SFLOAT; }

// the resource format of a texture: depth formats that are also sampled are typeless
DXGI_FORMAT resourceFormat(VkFormat f, bool sampled) {
    if (f == VK_FORMAT_D32_SFLOAT && sampled) return DXGI_FORMAT_R32_TYPELESS;
    return toDxgi(f);
}
DXGI_FORMAT srvFormat(VkFormat f) { return f == VK_FORMAT_D32_SFLOAT ? DXGI_FORMAT_R32_FLOAT : toDxgi(f); }

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

class D3D12DeviceImpl;

// a CPU-writable range of descriptors of a heap with a free list
struct HeapAllocator {
    std::uint32_t next = 1;  // 0 is reserved (the default descriptor)
    std::uint32_t limit = 0;
    std::vector<std::uint32_t> freeList;
    std::mutex mutex;
    std::uint32_t alloc() {
        const std::lock_guard lock(mutex);
        if (!freeList.empty()) {
            const std::uint32_t i = freeList.back();
            freeList.pop_back();
            return i;
        }
        return next < limit ? next++ : 0;
    }
    void release(std::uint32_t i) {
        if (i == 0) return;
        const std::lock_guard lock(mutex);
        freeList.push_back(i);
    }
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
    D3D12_GPU_VIRTUAL_ADDRESS gpuAddress() const { return resource_->GetGPUVirtualAddress(); }
    D3D12_RESOURCE_STATES state = D3D12_RESOURCE_STATE_COMMON;

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
    D3D12_RESOURCE_STATES state = D3D12_RESOURCE_STATE_COMMON;
    // the state per mip level (all layers alike) while the levels differ (CommandList::transitionMips); `state` is the whole texture's otherwise
    std::array<D3D12_RESOURCE_STATES, 16> mipStates{};
    bool split = false;
    // a swapchain back buffer (owned by the swapchain; no SRV)
    void adopt(ComPtr<ID3D12Resource> r) { resource_ = std::move(r); }
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

// A BLAS or TLAS: the structure's storage (always in RAYTRACING_ACCELERATION_STRUCTURE state: such a resource cannot change state) and the build scratch
// (UNORDERED_ACCESS), sized from the prebuild info when the object is created and kept for rebuilds. A TLAS also owns a heap SRV (tlasSlot()).
class D3DAccel final : public AccelerationStructure {
public:
    D3DAccel(D3D12DeviceImpl& dev, bool tlas) : dev_(dev), tlas_(tlas) {}
    ~D3DAccel() override;
    bool createBlas(std::span<const TriangleGeometry> geometries);
    bool createTlas(std::uint32_t maxInstances);
    bool isTlas() const override { return tlas_; }
    std::uint64_t address() const override { return storage_->GetGPUVirtualAddress(); }
    std::uint32_t tlasSlot() const override { return slot_; }
    std::uint64_t scratchAddress() const { return scratch_->GetGPUVirtualAddress(); }
    const std::vector<TriangleGeometry>& geometries() const { return geometries_; }
    std::uint32_t maxInstances() const { return maxInstances_; }
    // D3D12_RAYTRACING_GEOMETRY_DESCs of the BLAS (from the buffers' current addresses)
    void geometryDescs(std::vector<D3D12_RAYTRACING_GEOMETRY_DESC>& out) const;

private:
    bool allocate(const D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS& inputs);

    D3D12DeviceImpl& dev_;
    bool tlas_;
    std::vector<TriangleGeometry> geometries_;
    std::uint32_t maxInstances_ = 0;
    ComPtr<ID3D12Resource> storage_, scratch_;
    std::uint32_t slot_ = 0;
};

class D3DCommandList;

class D3D12DeviceImpl final : public Device {
public:
    ~D3D12DeviceImpl() override {
        if (fenceEvent_ != nullptr) {
            waitIdle();
            CloseHandle(fenceEvent_);
        }
    }

    bool create(const DeviceDesc& desc) {
        UINT factoryFlags = 0;
        if (desc.validation) {
            ComPtr<ID3D12Debug> debug;
            if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debug)))) {
                debug->EnableDebugLayer();
                factoryFlags |= DXGI_CREATE_FACTORY_DEBUG;
                ComPtr<ID3D12Debug1> debug1;
                if (SUCCEEDED(debug.As(&debug1))) debug1->SetEnableGPUBasedValidation(TRUE);
            }
        }
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
            if (SUCCEEDED(D3D12CreateDevice(a.Get(), D3D_FEATURE_LEVEL_12_0, __uuidof(ID3D12Device), nullptr))) {
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
        RVK_HR(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&device_)), "D3D12CreateDevice");
        if (desc.validation) {
            // the debug layer's messages go to the log and count as errors (a debugger's output window is not needed)
            ComPtr<ID3D12InfoQueue1> iq;
            if (SUCCEEDED(device_.As(&iq))) {
                DWORD cookie = 0;
                iq->RegisterMessageCallback(
                    [](D3D12_MESSAGE_CATEGORY, D3D12_MESSAGE_SEVERITY severity, D3D12_MESSAGE_ID id, LPCSTR text, void* self) {
                        auto* d = static_cast<D3D12DeviceImpl*>(self);
                        if (severity == D3D12_MESSAGE_SEVERITY_CORRUPTION || severity == D3D12_MESSAGE_SEVERITY_ERROR) {
                            ++d->errors_;
                            spdlog::error("d3d12: validation {}: {}", static_cast<int>(id), text);
                        } else if (severity == D3D12_MESSAGE_SEVERITY_WARNING) {
                            spdlog::warn("d3d12: validation {}: {}", static_cast<int>(id), text);
                        }
                    },
                    D3D12_MESSAGE_CALLBACK_FLAG_NONE, this, &cookie);
            }
        }
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
        D3D12_FEATURE_DATA_D3D12_OPTIONS5 o5{};
        if (desc.rayTracing && SUCCEEDED(device_->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS5, &o5, sizeof(o5))) &&
            o5.RaytracingTier >= D3D12_RAYTRACING_TIER_1_1 && SUCCEEDED(device_.As(&device5_))) {
            caps_.rayQuery = true;
        }
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
        srvInc_ = device_->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        samplerInc_ = device_->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER);
        rtvInc_ = device_->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
        dsvInc_ = device_->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_DSV);
        srvAlloc_.limit = kHeapSize - kUavRing;
        samplerAlloc_.limit = kSamplerHeapSize;
        // entry 0 of the texture heap: a null SRV that reads (0, 0, 0, 1) (the Vulkan side's 1x1 default texture)
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
        caps_.backend = Backend::D3D12;
        caps_.bindless = true;
        caps_.heapSize = kHeapSize;
        caps_.drawIndirectCount = true;  // ExecuteIndirect with a count buffer
        caps_.textureBlit = false;       // no scaling copy (a mip chain would need a compute / graphics pass of the backend's own)
        return true;
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
            e.Format = toDxgi(a.format);
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

    std::unique_ptr<AccelerationStructure> createBlas(std::span<const TriangleGeometry> geometries) override {
        if (!caps_.rayQuery) return nullptr;
        auto a = std::make_unique<D3DAccel>(*this, false);
        if (!a->createBlas(geometries)) return nullptr;
        return a;
    }
    std::unique_ptr<AccelerationStructure> createTlas(std::uint32_t maxInstances) override {
        if (!caps_.rayQuery) return nullptr;
        auto a = std::make_unique<D3DAccel>(*this, true);
        if (!a->createTlas(maxInstances)) return nullptr;
        return a;
    }

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
        const std::uint32_t t = uavTable_++ % (kUavRing / rhi::kStorageImages);
        return kHeapSize - kUavRing + t * rhi::kStorageImages;
    }
    ID3D12Device5* device5() { return device5_.Get(); }
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

    void waitIdle() {
        const std::uint64_t v = ++fenceValue_;
        queue_->Signal(fence_.Get(), v);
        if (fence_->GetCompletedValue() < v) {
            fence_->SetEventOnCompletion(v, fenceEvent_);
            WaitForSingleObject(fenceEvent_, INFINITE);
        }
    }

private:
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
    std::uint32_t rtvNext_ = 0, dsvNext_ = 0, uavTable_ = 0;
    DeviceCaps caps_;
    std::atomic<std::uint32_t> errors_{0};
};

// ---- resources --------------------------------------------------------------------------------------------------------------

bool D3DBuffer::create() {
    D3D12_HEAP_PROPERTIES hp{};
    hp.Type = desc_.memory == Memory::Upload ? D3D12_HEAP_TYPE_UPLOAD : desc_.memory == Memory::Readback ? D3D12_HEAP_TYPE_READBACK : D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC rd{};
    rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    rd.Width = alignUp(std::max<std::uint64_t>(desc_.size, 4), 4);
    rd.Height = rd.DepthOrArraySize = rd.MipLevels = 1;
    rd.SampleDesc = {1, 0};
    rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    // shader-writable (RWByteAddressBuffer, atomics): device memory only (upload / readback heaps cannot have UAVs)
    const bool writable = (desc_.usage & VK_BUFFER_USAGE_STORAGE_BUFFER_BIT) != 0 && hp.Type == D3D12_HEAP_TYPE_DEFAULT;
    if (writable) rd.Flags |= D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    state = hp.Type == D3D12_HEAP_TYPE_UPLOAD ? D3D12_RESOURCE_STATE_GENERIC_READ : hp.Type == D3D12_HEAP_TYPE_READBACK ? D3D12_RESOURCE_STATE_COPY_DEST : D3D12_RESOURCE_STATE_COMMON;
    if (FAILED(dev_.device()->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd, state, nullptr, IID_PPV_ARGS(&resource_)))) return false;
    if (desc_.name != nullptr) resource_->SetName(widen(desc_.name).c_str());
    if (hp.Type != D3D12_HEAP_TYPE_DEFAULT) {
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
            slot_ = dev_.srvAlloc().alloc();
            D3D12_SHADER_RESOURCE_VIEW_DESC sd{};
            sd.Format = srvFormat(desc_.format);
            sd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
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
        sd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
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
    dev_.waitIdle();
    if (!isArray()) dev_.srvAlloc().release(slot_);
    for (std::uint32_t s : layerSlots_) dev_.srvAlloc().release(s);
}

// ---- acceleration structures (DXR 1.1) ---------------------------------------------------------------------------------------

D3DAccel::~D3DAccel() {
    dev_.waitIdle();
    dev_.srvAlloc().release(slot_);
}

void D3DAccel::geometryDescs(std::vector<D3D12_RAYTRACING_GEOMETRY_DESC>& out) const {
    for (const TriangleGeometry& t : geometries_) {
        D3D12_RAYTRACING_GEOMETRY_DESC g{};
        g.Type = D3D12_RAYTRACING_GEOMETRY_TYPE_TRIANGLES;
        g.Flags = t.opaque ? D3D12_RAYTRACING_GEOMETRY_FLAG_OPAQUE : D3D12_RAYTRACING_GEOMETRY_FLAG_NONE;
        g.Triangles.VertexFormat = toDxgi(t.vertexFormat);
        g.Triangles.VertexCount = t.vertexCount;
        g.Triangles.VertexBuffer.StartAddress = static_cast<D3DBuffer*>(t.vertices)->gpuAddress() + t.vertexOffset;
        g.Triangles.VertexBuffer.StrideInBytes = t.vertexStride;
        if (t.indices != nullptr) {
            g.Triangles.IndexFormat = t.indexType == VK_INDEX_TYPE_UINT16 ? DXGI_FORMAT_R16_UINT : DXGI_FORMAT_R32_UINT;
            g.Triangles.IndexCount = t.indexCount;
            g.Triangles.IndexBuffer = static_cast<D3DBuffer*>(t.indices)->gpuAddress() + t.indexOffset;
        }
        out.push_back(g);
    }
}

// prebuild info -> storage + scratch. The storage is created in RAYTRACING_ACCELERATION_STRUCTURE state (it stays there: such a resource cannot be
// transitioned) with UAV access for the builds; both sizes are rounded up to the 256 byte alignment DXR wants.
bool D3DAccel::allocate(const D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS& inputs) {
    D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO info{};
    dev_.device5()->GetRaytracingAccelerationStructurePrebuildInfo(&inputs, &info);
    if (info.ResultDataMaxSizeInBytes == 0) return false;
    storage_ = dev_.createDefaultBuffer(alignUp(info.ResultDataMaxSizeInBytes, 256), D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
                                        D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE);
    // (the scratch is created in COMMON, which is what a buffer starts in whatever is asked for; the build promotes it to UNORDERED_ACCESS implicitly
    // and it decays back when the command list has executed)
    scratch_ = dev_.createDefaultBuffer(alignUp(std::max<std::uint64_t>(info.ScratchDataSizeInBytes, 256), 256), D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
                                        D3D12_RESOURCE_STATE_COMMON);
    if (!storage_ || !scratch_) return false;
    storage_->SetName(tlas_ ? L"rhi tlas" : L"rhi blas");
    scratch_->SetName(L"rhi build scratch");
    return true;
}

bool D3DAccel::createBlas(std::span<const TriangleGeometry> geometries) {
    geometries_.assign(geometries.begin(), geometries.end());
    for (const TriangleGeometry& t : geometries_) {
        if (t.vertices == nullptr) return false;
    }
    std::vector<D3D12_RAYTRACING_GEOMETRY_DESC> descs;
    geometryDescs(descs);
    D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS in{};
    in.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL;
    in.Flags = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_TRACE;
    in.NumDescs = static_cast<UINT>(descs.size());
    in.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;
    in.pGeometryDescs = descs.data();
    return allocate(in);
}

bool D3DAccel::createTlas(std::uint32_t maxInstances) {
    maxInstances_ = maxInstances;
    D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS in{};
    in.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL;
    in.Flags = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_TRACE;
    in.NumDescs = maxInstances;
    in.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;
    if (!allocate(in)) return false;
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

// ---- command list -----------------------------------------------------------------------------------------------------------

class D3DCommandList final : public CommandList {
public:
    D3DCommandList(D3D12DeviceImpl& dev, ID3D12GraphicsCommandList* list) : dev_(dev), list_(list) {}

    void transition(Texture& texture, VkImageLayout layout) override {
        D3DTexture& t = static_cast<D3DTexture&>(texture);
        if (layout == VK_IMAGE_LAYOUT_UNDEFINED) {
            t.state = D3D12_RESOURCE_STATE_COMMON;  // the contents are discarded: no barrier, the next one starts from COMMON
            t.split = false;
            return;
        }
        transitionRange(t, toState(layout), 0, t.desc().mips);
    }
    void transitionMips(Texture& texture, VkImageLayout layout, std::uint32_t baseMip, std::uint32_t mipCount) override {
        if (layout == VK_IMAGE_LAYOUT_UNDEFINED) return;
        D3DTexture& t = static_cast<D3DTexture&>(texture);
        transitionRange(t, toState(layout), baseMip, mipCount);
    }

    // ShaderRead -> ALL_SHADER_RESOURCE (the state of an SRV), ShaderWrite -> UNORDERED_ACCESS; an UAV -> UAV step is a UAV barrier (the writes of the
    // previous dispatch become visible); a transition out of UNORDERED_ACCESS flushes the writes by itself
    void bufferBarrier(Buffer& buffer, BufferAccess next) override {
        D3DBuffer& b = static_cast<D3DBuffer&>(buffer);
        if (b.desc().memory != Memory::Device) return;  // upload / readback heaps keep their one state
        const D3D12_RESOURCE_STATES to = next == BufferAccess::ShaderWrite ? D3D12_RESOURCE_STATE_UNORDERED_ACCESS : D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE;
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
            if (t.layout != VK_IMAGE_LAYOUT_UNDEFINED) transitionRange(*static_cast<D3DTexture*>(t.texture), toState(t.layout), t.baseMip, t.mipCount);
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
    bool blitTexture(Texture&, Texture&, std::span<const TextureBlit>, VkFilter) override {
        spdlog::error("d3d12: blitTexture is not implemented (DeviceCaps::textureBlit)");
        return false;
    }

    // Acceleration structure builds (ID3D12GraphicsCommandList4). The inputs (vertex / index / instance buffers) must be in a shader read state: device
    // memory buffers are moved to ALL_SHADER_RESOURCE here (upload heaps are GENERIC_READ). The structures themselves live in
    // RAYTRACING_ACCELERATION_STRUCTURE state for good; each build ends with a UAV barrier on all UAV resources (null resource) so that the next build
    // (a TLAS over this BLAS) and the ray queries of later dispatches read the finished structure; the scratch buffer (promoted from COMMON to
    // UNORDERED_ACCESS by the build) is covered by the same barrier when the next build in this list reuses it.
    void buildBlas(AccelerationStructure& as) override {
        D3DAccel& a = static_cast<D3DAccel&>(as);
        for (const TriangleGeometry& t : a.geometries()) {
            bufferState(*static_cast<D3DBuffer*>(t.vertices), D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE);
            if (t.indices != nullptr) bufferState(*static_cast<D3DBuffer*>(t.indices), D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE);
        }
        std::vector<D3D12_RAYTRACING_GEOMETRY_DESC> descs;
        a.geometryDescs(descs);
        D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC bd{};
        bd.DestAccelerationStructureData = a.address();
        bd.ScratchAccelerationStructureData = a.scratchAddress();
        bd.Inputs.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL;
        bd.Inputs.Flags = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_TRACE;
        bd.Inputs.NumDescs = static_cast<UINT>(descs.size());
        bd.Inputs.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;
        bd.Inputs.pGeometryDescs = descs.data();
        list4()->BuildRaytracingAccelerationStructure(&bd, 0, nullptr);
        uavBarrier();
    }
    void buildTlas(AccelerationStructure& as, Buffer& instances, std::uint64_t instanceOffset, std::uint32_t count) override {
        D3DAccel& a = static_cast<D3DAccel&>(as);
        D3DBuffer& ib = static_cast<D3DBuffer&>(instances);
        bufferState(ib, D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE);
        D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC bd{};
        bd.DestAccelerationStructureData = a.address();
        bd.ScratchAccelerationStructureData = a.scratchAddress();
        bd.Inputs.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL;
        bd.Inputs.Flags = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_TRACE;
        bd.Inputs.NumDescs = std::min(count, a.maxInstances());
        bd.Inputs.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;
        bd.Inputs.InstanceDescs = ib.gpuAddress() + instanceOffset;
        list4()->BuildRaytracingAccelerationStructure(&bd, 0, nullptr);
        uavBarrier();
    }

    // (capture regions: PIX events need WinPixEventRuntime; the raw BeginEvent API is flagged by the debug layer)
    void beginRegion(std::string_view) override {}
    void endRegion() override {}

private:
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
        if (b.desc().memory != Memory::Device || b.state == to) return;
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

}  // namespace

std::unique_ptr<Device> createD3D12Device(const DeviceDesc& desc) {
    auto d = std::make_unique<D3D12DeviceImpl>();
    if (!d->create(desc)) return nullptr;
    return d;
}

}  // namespace rvk::rhi
