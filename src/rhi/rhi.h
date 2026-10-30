#pragma once

// The slim RHI (docs/research/hlsl-rhi.md 5): Vulkan 1.3 and DX12 behind one interface, the surface the renderer uses and nothing else:
// dynamic rendering, descriptor indexing (the bindless heap), constant buffers and push constants. Vulkan structs / enums are the vocabulary
// (VkFormat, VkImageUsageFlags, VkImageLayout, VkCompareOp, ...); the DX12 backend translates them (format table, layout -> resource state,
// root signature from the fixed binding model below). One HLSL source per shader: a Shader carries the SPIR-V and the DXIL of it.
//
// The binding model (identical on both backends, shaders/include/rvk_abi.hlsli):
//   push constants   <= 128 bytes                       Vulkan push constants / DX12 root constants (b0, space0)
//   bindless heap    sampled textures (rhi::Texture::heapSampled), samplers (heapSampler), storage buffers (Buffer::heapIndex), reached by index
//                    from push constants / blocks: Vulkan set 0 (typed arrays), DX12 ResourceDescriptorHeap / SamplerDescriptorHeap (SM 6.6)
//   constant buffer  one per command list state, b8 space1 (setConstantBuffer): Vulkan push descriptor set 1 binding 8, DX12 a root CBV
//   storage images   u0..u3 space1 (setStorageTextures): Vulkan push descriptor set 1 bindings 0..3, DX12 a descriptor table
//   references       an 8-byte value in blocks / push constants that shaders read as RVK_REF: Buffer::shaderRef (device address on Vulkan,
//                    {heap index, offset} on DX12)

#include <array>
#include <cstdint>
#include <functional>
#include <memory>
#include <span>
#include <string>
#include <string_view>

#include "vk/vk.h"

namespace rvk::rhi {

enum class Backend { Vulkan, D3D12 };
const char* backendName(Backend b);

// ---- resources --------------------------------------------------------------------------------------------------------------

enum class Memory {
    Device,    // GPU only
    Upload,    // CPU writes, GPU reads (persistently mapped, write combined)
    Readback,  // GPU writes, CPU reads (persistently mapped, cached)
};

struct BufferDesc {
    std::uint64_t size = 0;
    VkBufferUsageFlags usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;  // STORAGE: heapIndex() / shaderRef(); UNIFORM: setConstantBuffer; TRANSFER_*: copies
    Memory memory = Memory::Device;
    const char* name = nullptr;
};

class Buffer {
public:
    virtual ~Buffer() = default;
    virtual const BufferDesc& desc() const = 0;
    // the persistent mapping (Upload / Readback), null for Device memory
    virtual void* map() = 0;
    // index of the buffer as a ByteAddressBuffer in the bindless heap (STORAGE usage; 0 = none)
    virtual std::uint32_t heapIndex() const = 0;
    // what shaders read as RVK_REF to this buffer at `offset`
    virtual std::array<std::uint32_t, 2> shaderRef(std::uint64_t offset = 0) const = 0;
};

struct TextureDesc {
    VkImageType type = VK_IMAGE_TYPE_2D;
    VkFormat format = VK_FORMAT_R8G8B8A8_UNORM;
    VkExtent3D extent{1, 1, 1};
    std::uint32_t mips = 1;
    std::uint32_t layers = 1;
    VkImageUsageFlags usage = VK_IMAGE_USAGE_SAMPLED_BIT;  // SAMPLED: heapSampled; STORAGE: setStorageTextures; COLOR/DEPTH_STENCIL_ATTACHMENT: rendering
    const char* name = nullptr;
};

class Texture {
public:
    virtual ~Texture() = default;
    virtual const TextureDesc& desc() const = 0;
    // index in the bindless heap of the sampled view (SAMPLED usage; 0 = none)
    virtual std::uint32_t heapSampled() const = 0;
};

struct SamplerDesc {
    VkFilter filter = VK_FILTER_LINEAR;
    VkSamplerAddressMode address = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
};
// a sampler in the bindless sampler heap: its index
using SamplerIndex = std::uint32_t;

// ---- pipelines --------------------------------------------------------------------------------------------------------------

// both compilations of one HLSL entry point (shader/dxc.h: Target::Spirv / Target::Dxil); a backend uses the one it needs
struct Shader {
    std::span<const std::uint32_t> spirv;
    std::span<const std::uint8_t> dxil;
};

class Pipeline {
public:
    virtual ~Pipeline() = default;
};

struct ComputePipelineDesc {
    Shader cs;
    const char* name = nullptr;
};

struct GraphicsPipelineDesc {
    Shader vs, ps;
    std::array<VkFormat, 4> colorFormats{VK_FORMAT_UNDEFINED, VK_FORMAT_UNDEFINED, VK_FORMAT_UNDEFINED, VK_FORMAT_UNDEFINED};
    VkFormat depthFormat = VK_FORMAT_UNDEFINED;
    VkPipelineColorBlendAttachmentState blend{};  // for every colour attachment (blendEnable VK_FALSE = none, colorWriteMask 0 = all)
    VkCullModeFlags cull = VK_CULL_MODE_NONE;
    VkFrontFace frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    VkPrimitiveTopology topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    bool depthTest = false;
    bool depthWrite = false;
    VkCompareOp depthOp = VK_COMPARE_OP_LESS_OR_EQUAL;
    const char* name = nullptr;
};

// ---- swapchain --------------------------------------------------------------------------------------------------------------

struct SwapchainDesc {
    void* window = nullptr;  // HWND
    std::uint32_t width = 0, height = 0;
    bool vsync = true;
};

class Swapchain {
public:
    virtual ~Swapchain() = default;
    virtual std::uint32_t width() const = 0;
    virtual std::uint32_t height() const = 0;
    virtual VkFormat format() const = 0;
    virtual bool resize(std::uint32_t width, std::uint32_t height) = 0;
};

// ---- command lists ----------------------------------------------------------------------------------------------------------

struct RenderingInfo {
    struct Attachment {
        Texture* texture = nullptr;
        VkAttachmentLoadOp load = VK_ATTACHMENT_LOAD_OP_LOAD;
        VkAttachmentStoreOp store = VK_ATTACHMENT_STORE_OP_STORE;
        VkClearColorValue clear{};
    };
    std::array<Attachment, 4> color{};
    std::uint32_t colorCount = 0;
    Attachment depth{};
    VkClearDepthStencilValue depthClear{1.0f, 0};
    std::uint32_t width = 0, height = 0;
};

class CommandList {
public:
    virtual ~CommandList() = default;

    // whole-resource transition to `layout` (the old layout is tracked per texture; UNDEFINED discards the contents)
    virtual void transition(Texture& texture, VkImageLayout layout) = 0;
    // makes the shader / transfer writes of a buffer visible to later reads (a whole-buffer barrier)
    virtual void bufferBarrier(Buffer& buffer) = 0;

    // bindings (valid for the next draws / dispatches; both backends bind the bindless heap themselves)
    virtual void setPushConstants(const void* data, std::uint32_t size) = 0;
    virtual void setConstantBuffer(Buffer& buffer, std::uint64_t offset, std::uint64_t size) = 0;
    // storage images u0.. (textures in GENERAL layout); a null entry is left unbound
    virtual void setStorageTextures(std::span<Texture* const> textures) = 0;

    virtual void bindPipeline(Pipeline& pipeline) = 0;
    virtual void dispatch(std::uint32_t x, std::uint32_t y, std::uint32_t z) = 0;

    // dynamic rendering: attachments must be in COLOR_ATTACHMENT_OPTIMAL / DEPTH_STENCIL_ATTACHMENT_OPTIMAL layout
    virtual void beginRendering(const RenderingInfo& info) = 0;
    virtual void endRendering() = 0;
    virtual void setViewport(float x, float y, float w, float h) = 0;  // y down, as D3D; the backends flip as needed
    virtual void setScissor(std::int32_t x, std::int32_t y, std::uint32_t w, std::uint32_t h) = 0;
    virtual void draw(std::uint32_t vertexCount, std::uint32_t instanceCount, std::uint32_t firstVertex, std::uint32_t firstInstance) = 0;

    // copies: buffer <-> buffer, buffer -> texture (mip 0, layer 0, the whole extent, rows `bufferRowPitch(format, width)` bytes apart),
    // texture -> buffer likewise. The texture must be in TRANSFER_DST_OPTIMAL (upload) / TRANSFER_SRC_OPTIMAL (readback) layout.
    virtual void copyBuffer(Buffer& dst, std::uint64_t dstOffset, Buffer& src, std::uint64_t srcOffset, std::uint64_t size) = 0;
    virtual void copyBufferToTexture(Texture& dst, Buffer& src, std::uint64_t srcOffset) = 0;
    virtual void copyTextureToBuffer(Buffer& dst, std::uint64_t dstOffset, Texture& src) = 0;

    virtual void beginRegion(std::string_view name) = 0;
    virtual void endRegion() = 0;
};

// ---- device -----------------------------------------------------------------------------------------------------------------

struct DeviceDesc {
    Backend backend = Backend::Vulkan;
    bool validation = false;  // Vulkan validation layer / D3D12 debug layer (+ GPU based validation)
    int adapter = -1;         // -1 = the high performance adapter
    const char* name = "rvk";
};

struct DeviceCaps {
    std::string adapterName;
    Backend backend = Backend::Vulkan;
    bool bindless = false;          // descriptor indexing / SM 6.6 dynamic resources: required, creation fails without
    std::uint32_t maxPushBytes = 128;
    std::uint32_t heapSize = 0;     // entries of the texture / buffer heap
};

class Device {
public:
    virtual ~Device() = default;
    virtual const DeviceCaps& caps() const = 0;
    // validation messages of severity error seen so far (Vulkan validation layer / D3D12 debug layer + GPU based validation; 0 without validation)
    virtual std::uint32_t validationErrors() const = 0;

    virtual std::unique_ptr<Buffer> createBuffer(const BufferDesc& desc) = 0;
    virtual std::unique_ptr<Texture> createTexture(const TextureDesc& desc) = 0;
    virtual SamplerIndex createSampler(const SamplerDesc& desc) = 0;
    virtual std::unique_ptr<Pipeline> createComputePipeline(const ComputePipelineDesc& desc) = 0;
    virtual std::unique_ptr<Pipeline> createGraphicsPipeline(const GraphicsPipelineDesc& desc) = 0;

    // records into the returned list, executes it on the graphics queue and waits for the GPU (tools, tests, one-off work: the frame loop of the
    // renderer keeps its own submission machinery on top of the backend)
    virtual void submitAndWait(const std::function<void(CommandList&)>& record) = 0;

    virtual std::unique_ptr<Swapchain> createSwapchain(const SwapchainDesc& desc) = 0;
    // acquires the swapchain's next back buffer (usage: colour attachment, transfer src / dst; its layout starts UNDEFINED), records into a list, moves the back buffer to
    // PRESENT_SRC, submits, presents and waits. False when the swapchain is out of date / the window is minimised: resize it and try again.
    virtual bool renderAndPresent(Swapchain& swapchain, const std::function<void(CommandList&, Texture& backBuffer)>& record) = 0;
};

// the backends (rhi/vulkan in rvk_core, rhi/d3d12 in the rvk_rhi_d3d12 library); null (and logged) when the API or the device does not meet the floor
std::unique_ptr<Device> createVulkanDevice(const DeviceDesc& desc);
std::unique_ptr<Device> createD3D12Device(const DeviceDesc& desc);

// ---- formats (both backends) ------------------------------------------------------------------------------------------------

// bytes per pixel of the uncompressed colour formats the RHI knows (0 = unknown)
std::uint32_t formatBytes(VkFormat format);
// the row pitch of a buffer <-> texture copy: width * formatBytes rounded up to 256 (D3D12's alignment, used on Vulkan too)
std::uint32_t bufferRowPitch(VkFormat format, std::uint32_t width);

}  // namespace rvk::rhi
