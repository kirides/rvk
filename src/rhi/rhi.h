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
//   storage images   u0..u7 space1 (setStorageViews): Vulkan push descriptor set 1 bindings 0..7, DX12 a descriptor table of 8
//   references       an 8-byte value in blocks / push constants that shaders read as RVK_REF: Buffer::shaderRef (device address on Vulkan,
//                    {heap index, offset} on DX12); the read-write twin is Buffer::shaderRefRw (RVK_BUF_LOAD / STORE / ADD of rvk_abi.hlsli)
//   acceleration     the TLAS of a ray query shader: AccelerationStructure::tlasSlot() in the push constants / blocks, RVK_TLAS(slot) in HLSL
//   structures       (Vulkan: the bindless acceleration structure array, DX12: a RAYTRACING_ACCELERATION_STRUCTURE SRV in the heap)

#include <algorithm>
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

// storage image slots of a pass (u0..u7 space1; CommandList::setStorageViews)
inline constexpr std::uint32_t kStorageImages = 8;
const char* backendName(Backend b);

// ---- resources --------------------------------------------------------------------------------------------------------------

enum class Memory {
    Device,    // GPU only
    Upload,    // CPU writes, GPU reads (persistently mapped, write combined)
    Readback,  // GPU writes, CPU reads (persistently mapped, cached)
};

struct BufferDesc {
    std::uint64_t size = 0;
    // STORAGE: heapIndex() / shaderRef() (read) and, in Device memory, heapIndexRw() / shaderRefRw() (read-write); UNIFORM: setConstantBuffer;
    // VERTEX / INDEX / INDIRECT: setVertexBuffers / setIndexBuffer / drawIndexedIndirectCount; TRANSFER_*: copies. (Vulkan checks the flags, DX12 does not:
    // set them honestly.)
    VkBufferUsageFlags usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
    Memory memory = Memory::Device;
    const char* name = nullptr;
};

class Buffer {
public:
    virtual ~Buffer() = default;
    virtual const BufferDesc& desc() const = 0;
    // the persistent mapping (Upload / Readback), null for Device memory
    virtual void* map() = 0;
    // Memory::Readback: makes what the GPU wrote visible to the CPU reads of map() (call it once the GPU is done with the copy); Memory::Upload: makes
    // the CPU's writes visible to the GPU (before the submission that reads them). No-ops where the memory is coherent (always on DX12).
    virtual void invalidate() = 0;
    virtual void flush() = 0;
    // index of the buffer as a ByteAddressBuffer in the bindless heap (STORAGE usage; 0 = none; always 0 on Vulkan: buffers are reached by address)
    virtual std::uint32_t heapIndex() const = 0;
    // index of the buffer as a RWByteAddressBuffer (a UAV) in the bindless heap (STORAGE usage and Memory::Device; 0 = none; always 0 on Vulkan)
    virtual std::uint32_t heapIndexRw() const = 0;
    // What shaders read as RVK_REF to this buffer at `offset` (read-only access: RVK_LOAD). Vulkan: the buffer's device address (low, high).
    // DX12: {heapIndex() (an SRV), offset}. The two words are opaque to the application: copy them into a push constant / block.
    virtual std::array<std::uint32_t, 2> shaderRef(std::uint64_t offset = 0) const = 0;
    // What shaders use for read-write access (rvk_abi.hlsli RVK_BUF_LOAD / RVK_BUF_STORE / RVK_BUF_ADD take it as a uint2): Vulkan: the same
    // device address as shaderRef, DX12: {heapIndexRw() (a UAV), offset}. The buffer must be in the write state (bufferBarrier(.., ShaderWrite)).
    virtual std::array<std::uint32_t, 2> shaderRefRw(std::uint64_t offset = 0) const = 0;
    // The raw GPU address (Vulkan: the buffer's device address, D3D12: the GPU virtual address): what acceleration structure builds take as
    // TriangleGeometry::vertexAddress / indexAddress and buildTlasAt. Not a shader reference (use shaderRef for those: on DX12 it is a heap index).
    virtual std::uint64_t gpuAddress() const = 0;
};

struct TextureDesc {
    VkImageType type = VK_IMAGE_TYPE_2D;
    VkFormat format = VK_FORMAT_R8G8B8A8_UNORM;
    VkExtent3D extent{1, 1, 1};
    std::uint32_t mips = 1;
    std::uint32_t layers = 1;
    VkImageUsageFlags usage = VK_IMAGE_USAGE_SAMPLED_BIT;  // SAMPLED: heapSampled; STORAGE: setStorageViews; COLOR/DEPTH_STENCIL_ATTACHMENT: rendering
    const char* name = nullptr;
    // A cube map (layers is forced to 6, extent.depth 1; the layers are the faces +X -X +Y -Y +Z -Z). 3D textures: type VK_IMAGE_TYPE_3D (layers 1).
    bool cube = false;
    // How shaders see the channels of the sampled view (L8 -> r r r 1 ...; all IDENTITY = as stored). Storage views and attachments ignore it.
    VkComponentMapping swizzle{};
};

// One mip level of one array layer (cube face); copies address textures with it. 3D textures have layer 0 only.
struct Subresource {
    std::uint32_t mip = 0;
    std::uint32_t layer = 0;
};

// A texture -> texture copy region (CommandList::copyTexture): `layerCount` layers from src.layer to dst.layer, the box `extent` (0 in every
// field = the whole source mip level) from srcOffset to dstOffset. Same format on both sides.
struct TextureCopy {
    Subresource src{};
    Subresource dst{};
    std::uint32_t layerCount = 1;
    VkOffset3D srcOffset{};
    VkOffset3D dstOffset{};
    VkExtent3D extent{0, 0, 0};
};

// A texture -> texture blit region (CommandList::blitTexture): one layer of one mip level to another, scaled from srcExtent to dstExtent
// (0 in every field = the whole mip level of that side).
struct TextureBlit {
    Subresource src{};
    Subresource dst{};
    VkOffset3D srcOffset{};  // the box of each side starts here; an extent of 0 in every field = the whole mip level (offset 0)
    VkOffset3D dstOffset{};
    VkExtent3D srcExtent{0, 0, 0};
    VkExtent3D dstExtent{0, 0, 0};
};

// A buffer -> texture box copy (CommandList::copyBufferToTextureBox): the box `extent` at `offset` of one subresource, read from the buffer at
// `bufferOffset` with rows `rowPitch` bytes apart (0 = tightly packed; DX12: a multiple of 256, offset a multiple of 512) and slices after each
// other. For block compressed formats rows / extents count blocks of 4x4 texels (offset and extent a multiple of 4 unless the box ends at the level edge).
struct BufferTextureCopy {
    std::uint64_t bufferOffset = 0;
    std::uint32_t rowPitch = 0;
    Subresource sub{};
    VkOffset3D offset{};
    VkExtent3D extent{1, 1, 1};
};

// the extent of mip level `mip` (each dimension halves, at least 1)
VkExtent3D mipExtent(const TextureDesc& desc, std::uint32_t mip);
// the bytes a buffer <-> texture copy of one mip level occupies in the buffer: rows are bufferRowPitch(format, mip width) apart, 3D slices follow each
// other (height rows each). On DX12 the buffer offset of a copy must be a multiple of 512.
std::uint64_t copyBytes(const TextureDesc& desc, std::uint32_t mip);

class Texture {
public:
    virtual ~Texture() = default;
    virtual const TextureDesc& desc() const = 0;
    // Index in the bindless heap of the sampled view (SAMPLED usage; 0 = none): all mips of a 2D texture (Texture2D), a cube (TextureCube) or a
    // volume (Texture3D) as the HLSL types of RVK_TEX2D / RVK_TEXCUBE / RVK_TEX3D. A 2D array texture (layers > 1, not cube) has no whole-array
    // view (the bindless set has no array kind): this is layer 0, the other layers come from heapSampledLayer.
    virtual std::uint32_t heapSampled() const = 0;
    // A Texture2D view (all mips) of one layer of a 2D array texture, created on first use (0 = none: not an array / SAMPLED missing)
    virtual std::uint32_t heapSampledLayer(std::uint32_t layer) = 0;
    // The layout every subresource of the texture is in after the commands recorded so far (the RHI tracks it: transitions through CommandList and
    // the backend's own barriers keep it; a texture created by the device starts UNDEFINED), VK_IMAGE_LAYOUT_MAX_ENUM while the mip levels differ
    // (CommandList::transitionMips). Lets callers skip a transition they would only repeat (CommandList::transition always records one).
    virtual VkImageLayout layout() const = 0;
    // a debug name (graphics debugger, validation messages); the creation name stays the one in desc() (null / empty = ignored)
    virtual void setDebugName(const std::string& /*name*/) {}
};

struct SamplerDesc {
    VkFilter filter = VK_FILTER_LINEAR;
    VkSamplerAddressMode address = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    float maxAnisotropy = 1.0f;  // > 1: anisotropic filtering up to this ratio (clamped to what the device offers; with VK_FILTER_LINEAR only)
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
    VkPrimitiveTopology topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;  // lists and strips of triangles / lines / points
    bool depthTest = false;
    bool depthWrite = false;
    VkCompareOp depthOp = VK_COMPARE_OP_LESS_OR_EQUAL;
    const char* name = nullptr;
    // Vertex input (empty = none: the vertex shader pulls or generates its data). Locations follow the HLSL convention of the translator
    // (`[[vk::location(N)]] float4 vN : ATTRN`): attribute location N is semantic ATTR<N> on DX12. The binding strides are supplied per draw by
    // setVertexBuffers (dynamic state on Vulkan: the stride in vertexBindings is ignored); inputRate selects per vertex / per instance data.
    // Only read while the pipeline is created.
    std::span<const VkVertexInputBindingDescription> vertexBindings{};
    std::span<const VkVertexInputAttributeDescription> vertexAttributes{};
};

// ---- acceleration structures (optional: DeviceCaps::rayQuery) -----------------------------------------------------------------

class Buffer;

// One triangle geometry of a BLAS. The backends add the build-input usage to their buffers themselves; on DX12 device-memory buffers are moved to a
// shader-read state by the builds. Positions are float3 (R32G32B32_SFLOAT).
struct TriangleGeometry {
    Buffer* vertices = nullptr;
    std::uint64_t vertexOffset = 0;
    std::uint32_t vertexStride = 12;
    VkFormat vertexFormat = VK_FORMAT_R32G32B32_SFLOAT;
    std::uint32_t vertexCount = 0;
    Buffer* indices = nullptr;  // null (and no indexAddress) = a triangle list of vertexCount vertices
    std::uint64_t indexOffset = 0;
    VkIndexType indexType = VK_INDEX_TYPE_UINT32;
    std::uint32_t indexCount = 0;
    bool opaque = true;  // false: ray queries see the triangles as non-opaque candidates (an alpha test in the shader)
    // Raw GPU addresses instead of buffer + offset (the renderer's own memory: mesh arena, the frame ring, captures written by shaders). When non-zero
    // they replace `vertices` + `vertexOffset` / `indices` + `indexOffset`. On DX12 the memory must be readable by the build (an upload heap or a buffer
    // in a shader-read state): the builds cannot transition what they only know by address.
    std::uint64_t vertexAddress = 0;
    std::uint64_t indexAddress = 0;
    bool indexed() const { return indices != nullptr || indexAddress != 0; }
    // triangles of this geometry
    std::uint32_t triangleCount() const { return (indexed() ? indexCount : vertexCount) / 3; }
};

// What a BLAS is made for (Device::createBlas). `geometries` only fix the sizes (formats, vertex / index counts): a build may bring other buffers
// or addresses of the same shape (BlasBuild::geometries).
struct BlasDesc {
    std::span<const TriangleGeometry> geometries;
    bool allowUpdate = false;      // BlasBuild::update (refit in place: same counts, new vertex positions)
    bool allowCompaction = false;  // CommandList::queryCompactedSizes / compactBlas
    bool preferFastBuild = false;  // else fast trace
    const char* name = nullptr;
};

// TLAS: room for `maxInstances` records; preferFastBuild = rebuilt every frame (else fast trace)
struct TlasDesc {
    std::uint32_t maxInstances = 0;
    bool preferFastBuild = false;
    const char* name = nullptr;
};

// A bottom- or top-level acceleration structure. Memory for the structure belongs to the object (a TLAS also keeps its build scratch for rebuilds; BLAS
// builds take their scratch from the device's shared pool, see CommandList::buildBlases).
class AccelerationStructure {
public:
    virtual ~AccelerationStructure() = default;
    virtual bool isTlas() const = 0;
    // The GPU address instance records name: VkAccelerationStructureInstanceKHR::accelerationStructureReference (which is byte-compatible with
    // D3D12_RAYTRACING_INSTANCE_DESC, 64 bytes: transform 3x4, custom index / mask, flags, reference) takes the BLAS's address().
    virtual std::uint64_t address() const = 0;
    // TLAS: the slot shaders pass to RVK_TLAS (Vulkan: the bindless acceleration structure array of the renderer's set, shared with its RT scene: slot 0 is valid there; DX12: the CBV/SRV/UAV heap); 0 for a BLAS
    virtual std::uint32_t tlasSlot() const = 0;
    // bytes of the structure's storage
    virtual std::uint64_t storageBytes() const = 0;
};

// One BLAS of a batch (CommandList::buildBlases)
struct BlasBuild {
    AccelerationStructure* blas = nullptr;
    std::span<const TriangleGeometry> geometries{};  // empty = the ones it was created with
    bool update = false;                             // refit the structure in place (BlasDesc::allowUpdate; the previous build had the same counts)
};

// Compacted sizes of built BLASes (CommandList::queryCompactedSizes), read back by the CPU
class SizeQuery {
public:
    virtual ~SizeQuery() = default;
    virtual std::uint32_t capacity() const = 0;
    // The sizes of entries [first, first + count) once the GPU executed the command list that wrote them; false if (some of) them are not available
    // yet (always true on DX12 after submitAndWait). Entries that are not available read as 0.
    virtual bool read(std::uint32_t first, std::uint32_t count, std::uint64_t* sizes) = 0;
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
        // the mip level / array layer (cube face) rendered to: a 2D view of that subresource (3D textures cannot be attachments); `width` / `height`
        // of the RenderingInfo must be that level's size
        std::uint32_t mip = 0;
        std::uint32_t layer = 0;
    };
    std::array<Attachment, 4> color{};
    std::uint32_t colorCount = 0;
    Attachment depth{};
    VkClearDepthStencilValue depthClear{1.0f, 0};
    std::uint32_t width = 0, height = 0;
};

// How the shaders that follow use a buffer (CommandList::bufferBarrier)
enum class BufferAccess {
    ShaderRead,   // read through shaderRef / heapIndex (a ByteAddressBuffer), vertex / index / constant data
    ShaderWrite,  // read and write through shaderRefRw / heapIndexRw (a RWByteAddressBuffer, atomics)
};

// A storage image binding: one mip level of one layer. The view is a 3D view for volumes (RWTexture3D), an array view of a single layer when the
// texture has layers (RWTexture2DArray, the layer is z = 0), else a plain 2D view (RWTexture2D).
struct StorageView {
    Texture* texture = nullptr;
    std::uint32_t mip = 0;
    std::uint32_t layer = 0;
};

// One entry of CommandList::transitions: mip levels [baseMip, baseMip + mipCount) (all layers; the default = every level) to `layout`
struct TextureTransition {
    Texture* texture = nullptr;
    VkImageLayout layout = VK_IMAGE_LAYOUT_GENERAL;
    std::uint32_t baseMip = 0;
    std::uint32_t mipCount = 0xFFFFFFFFu;
};

class CommandList {
public:
    virtual ~CommandList() = default;

    // whole-resource transition to `layout` (the old layout is tracked per texture; UNDEFINED discards the contents). Transitioning to the layout
    // a texture is in already is a barrier that makes earlier writes visible (GENERAL -> GENERAL between two dispatches).
    virtual void transition(Texture& texture, VkImageLayout layout) = 0;
    // Mip levels [baseMip, baseMip + mipCount) of all layers to `layout`; the other levels keep theirs (tracked per level from then on; a whole-texture
    // transition moves every level again). The same-layout rule of transition applies. UNDEFINED is not allowed here (use transition to discard a
    // whole texture).
    virtual void transitionMips(Texture& texture, VkImageLayout layout, std::uint32_t baseMip, std::uint32_t mipCount) = 0;
    // Several transitions as one barrier (Vulkan: one vkCmdPipelineBarrier2 for all of them: a pass that moves many images between two dispatches pays for
    // one wait, not one per image). The entries apply in order; UNDEFINED is not allowed (see transitionMips).
    virtual void transitions(std::span<const TextureTransition> list) = 0;
    // Makes the shader / transfer writes of a buffer visible to later accesses and moves it to the state `next` needs: call it between a write
    // (copy, dispatch) and the next use by shaders, as a vertex / index / indirect source (Vulkan; DX12 transitions in the binding calls) or before
    // copies out of it. A whole-buffer barrier on Vulkan; on DX12 a transition (device memory) or, for ShaderWrite -> ShaderWrite, a UAV barrier.
    virtual void bufferBarrier(Buffer& buffer, BufferAccess next = BufferAccess::ShaderRead) = 0;
    // A global barrier between dispatches: the storage writes (images and buffers) and clears / copies of everything recorded so far are visible to
    // the dispatches, draws and copies that follow (Vulkan: a compute / fragment / transfer memory barrier; DX12: a UAV barrier on all resources). It does
    // not change layouts: a texture that is sampled next still needs its transition.
    virtual void computeBarrier() = 0;
    // A global memory barrier by Vulkan stages / accesses (one VkMemoryBarrier2; DX12: a UAV barrier on all resources, states are not involved): for memory
    // the RHI does not track, shader writes into raw-address buffers read by acceleration structure builds, copies, ...
    virtual void memoryBarrier(VkPipelineStageFlags2 srcStages, VkAccessFlags2 srcAccess, VkPipelineStageFlags2 dstStages, VkAccessFlags2 dstAccess) = 0;

    // bindings (valid for the next draws / dispatches; both backends bind the bindless heap themselves)
    virtual void setPushConstants(const void* data, std::uint32_t size) = 0;
    virtual void setConstantBuffer(Buffer& buffer, std::uint64_t offset, std::uint64_t size) = 0;
    // storage images u0..u7 (textures in GENERAL layout); a null texture is left unbound
    virtual void setStorageViews(std::span<const StorageView> views) = 0;
    // mip 0, layer 0 of each texture
    void setStorageTextures(std::span<Texture* const> textures) {
        std::array<StorageView, kStorageImages> v{};
        for (std::size_t i = 0; i < textures.size() && i < v.size(); ++i) v[i].texture = textures[i];
        setStorageViews(std::span<const StorageView>(v.data(), std::min<std::size_t>(textures.size(), v.size())));
    }

    virtual void bindPipeline(Pipeline& pipeline) = 0;
    virtual void dispatch(std::uint32_t x, std::uint32_t y, std::uint32_t z) = 0;

    // dynamic rendering: attachments must be in COLOR_ATTACHMENT_OPTIMAL / DEPTH_STENCIL_ATTACHMENT_OPTIMAL layout
    virtual void beginRendering(const RenderingInfo& info) = 0;
    virtual void endRendering() = 0;
    virtual void setViewport(float x, float y, float w, float h) = 0;  // y down, as D3D; the backends flip as needed
    virtual void setScissor(std::int32_t x, std::int32_t y, std::uint32_t w, std::uint32_t h) = 0;
    virtual void draw(std::uint32_t vertexCount, std::uint32_t instanceCount, std::uint32_t firstVertex, std::uint32_t firstInstance) = 0;

    // vertex input: `buffers[i]` becomes binding first + i (VK_BUFFER_USAGE_VERTEX_BUFFER_BIT), `offsets` in bytes, `strides` the distance of two
    // elements. The bindings are those of GraphicsPipelineDesc::vertexBindings.
    virtual void setVertexBuffers(std::uint32_t first, std::span<Buffer* const> buffers, std::span<const std::uint64_t> offsets,
                                  std::span<const std::uint32_t> strides) = 0;
    // VK_BUFFER_USAGE_INDEX_BUFFER_BIT; `offset` in bytes (a multiple of the index size)
    virtual void setIndexBuffer(Buffer& buffer, std::uint64_t offset, VkIndexType type) = 0;
    virtual void drawIndexed(std::uint32_t indexCount, std::uint32_t instanceCount, std::uint32_t firstIndex, std::int32_t vertexOffset,
                             std::uint32_t firstInstance) = 0;
    // Up to `maxDraws` indexed draws from `args` (VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT; records of VkDrawIndexedIndirectCommand = D3D12_DRAW_INDEXED_ARGUMENTS:
    // indexCount, instanceCount, firstIndex, vertexOffset, firstInstance; `stride` bytes apart, >= 20, a multiple of 4); min(maxDraws, the uint32 at
    // countBuffer + countOffset) of them run. Needs DeviceCaps::drawIndirectCount.
    virtual void drawIndexedIndirectCount(Buffer& args, std::uint64_t argsOffset, Buffer& countBuffer, std::uint64_t countOffset, std::uint32_t maxDraws,
                                          std::uint32_t stride) = 0;

    // copies: buffer <-> buffer, buffer -> texture and texture -> buffer of one subresource (default mip 0, layer 0): the whole mip level, rows
    // `bufferRowPitch(format, mip width)` bytes apart (see copyBytes). The texture must be in TRANSFER_DST_OPTIMAL (upload) /
    // TRANSFER_SRC_OPTIMAL (readback) layout. DX12 needs the buffer offset of a texture copy to be a multiple of 512.
    virtual void copyBuffer(Buffer& dst, std::uint64_t dstOffset, Buffer& src, std::uint64_t srcOffset, std::uint64_t size) = 0;
    // Fills `size` bytes (a multiple of 4) at `offset` with the 32-bit `value`. Device memory buffers with STORAGE usage (DX12: through the UAV). Later
    // reads or writes of the range want a bufferBarrier (Vulkan: a transfer write).
    virtual void fillBuffer(Buffer& dst, std::uint64_t offset, std::uint64_t size, std::uint32_t value) = 0;
    virtual void copyBufferToTexture(Texture& dst, Buffer& src, std::uint64_t srcOffset, Subresource sub = {}) = 0;
    virtual void copyTextureToBuffer(Buffer& dst, std::uint64_t dstOffset, Texture& src, Subresource sub = {}) = 0;
    // a box of one subresource from a buffer (texture upload of a dirty region; the texture in TRANSFER_DST_OPTIMAL)
    virtual void copyBufferToTextureBox(Texture& dst, Buffer& src, const BufferTextureCopy& copy) = 0;
    // Clears mip levels [baseMip, baseMip + mipCount) of all layers (colour textures with STORAGE usage, which must be in GENERAL layout; integer
    // formats take clear.uint32, the others clear.float32). Later shader access wants a transition to GENERAL (same layout = barrier).
    virtual void clearTexture(Texture& texture, const VkClearColorValue& value, std::uint32_t baseMip = 0, std::uint32_t mipCount = 1) = 0;
    // texture -> texture copies (same format; `src` in TRANSFER_SRC_OPTIMAL, `dst` in TRANSFER_DST_OPTIMAL, or both in one texture's different mip levels)
    virtual void copyTexture(Texture& dst, Texture& src, std::span<const TextureCopy> regions) = 0;
    // Scaled copies with a filter (mip chain generation): layouts as copyTexture, a texture may blit between its own mip levels (Vulkan: a texture blitted onto itself that is in GENERAL as a whole stays there). False (nothing
    // recorded, logged) without DeviceCaps::textureBlit.
    virtual bool blitTexture(Texture& dst, Texture& src, std::span<const TextureBlit> regions, VkFilter filter) = 0;

    // (Re)builds a BLAS from the geometries it was created with (the buffers must hold the data now), or a TLAS from `count` instance records
    // (VkAccelerationStructureInstanceKHR, 64 bytes each, at `instanceOffset`, a multiple of 16) in `instances`. Both end with a barrier: later builds
    // and the ray queries of later dispatches see the result. Build the BLASes a TLAS refers to before the TLAS (the same command list is fine).
    virtual void buildBlas(AccelerationStructure& blas) = 0;
    virtual void buildTlas(AccelerationStructure& tlas, Buffer& instances, std::uint64_t instanceOffset, std::uint32_t count) = 0;
    // The same TLAS build from raw memory at `instancesAddress` (16-byte aligned; see TriangleGeometry::vertexAddress for the DX12 condition).
    virtual void buildTlasAt(AccelerationStructure& tlas, std::uint64_t instancesAddress, std::uint32_t count) = 0;
    // A batch of BLAS builds / refits in one call. Their build scratch is sub-allocated from the device's shared pool (grown on demand); a batch that
    // outgrows the pool is cut into runs with a barrier between them. A barrier precedes the batch (earlier builds, whose scratch it reuses) and
    // follows it (later builds and ray queries). Vertex / index data written by earlier commands of this list needs its own memoryBarrier first.
    virtual void buildBlases(std::span<const BlasBuild> builds) = 0;
    // Writes the compacted size of each (built, allowCompaction) BLAS into entries first.. of `query` (barrier from the builds included), resetting them.
    virtual void queryCompactedSizes(SizeQuery& query, std::uint32_t first, std::span<AccelerationStructure* const> blases) = 0;
    // Copies `src` (built, sized by queryCompactedSizes: dst was created by Device::createBlasStorage of that many bytes) compacted into `dst`.
    virtual void compactBlas(AccelerationStructure& dst, AccelerationStructure& src) = 0;

    virtual void beginRegion(std::string_view name) = 0;
    virtual void endRegion() = 0;
};

// ---- device -----------------------------------------------------------------------------------------------------------------

struct DeviceDesc {
    Backend backend = Backend::Vulkan;
    bool validation = false;  // Vulkan validation layer / D3D12 debug layer (+ GPU based validation)
    int adapter = -1;         // -1 = the high performance adapter
    const char* name = "rvk";
    bool rayTracing = false;  // enable acceleration structures + ray queries when the device has them (DeviceCaps::rayQuery)
    // DX12 only: the directory holding the Agility SDK's D3D12Core.dll (+ d3d12SDKLayers.dll), with or without a trailing backslash. null = the environment
    // variable RVK_D3D12_SDK_DIR, else "d3d12\" next to the module this code is linked into (d3d9.dll in bin\rvk, or the tool's exe). The SDK is activated
    // once per process: the first device decides; a missing / unusable SDK falls back to the system runtime (DeviceCaps::agilitySdkVersion == 0).
    const char* d3d12SdkDir = nullptr;
};

struct DeviceCaps {
    std::string adapterName;
    Backend backend = Backend::Vulkan;
    bool bindless = false;          // descriptor indexing / SM 6.6 dynamic resources: required, creation fails without
    std::uint32_t maxPushBytes = 128;
    std::uint32_t heapSize = 0;     // entries of the texture / buffer heap
    bool drawIndirectCount = false;  // CommandList::drawIndexedIndirectCount (Vulkan 1.2 drawIndirectCount, always on DX12)
    bool textureBlit = false;        // CommandList::blitTexture (Vulkan only: the DX12 backend has no scaling copy)
    bool rayQuery = false;           // createBlas / createTlas + inline ray queries (Vulkan ray query with a bindless TLAS array, DXR tier 1.1); needs DeviceDesc::rayTracing
    std::uint32_t agilitySdkVersion = 0;  // DX12: the D3D12SDKVersion of the Agility SDK the device was created from (0 = the system runtime; always 0 on Vulkan)
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
    // null without DeviceCaps::rayQuery. The structure is allocated here (sized for the geometry / `maxInstances`), built by CommandList::buildBlas / buildTlas.
    virtual std::unique_ptr<AccelerationStructure> createBlas(const BlasDesc& desc) = 0;
    virtual std::unique_ptr<AccelerationStructure> createTlas(const TlasDesc& desc) = 0;
    std::unique_ptr<AccelerationStructure> createBlas(std::span<const TriangleGeometry> geometries) {
        BlasDesc d;
        d.geometries = geometries;
        return createBlas(d);
    }
    std::unique_ptr<AccelerationStructure> createTlas(std::uint32_t maxInstances) {
        TlasDesc d;
        d.maxInstances = maxInstances;
        return createTlas(d);
    }
    // An empty BLAS of `bytes` (the compaction target of CommandList::compactBlas); address() is valid at once, ray queries see it once compacted into.
    virtual std::unique_ptr<AccelerationStructure> createBlasStorage(std::uint64_t bytes, const char* name = nullptr) = 0;
    // room for `capacity` compacted sizes (CommandList::queryCompactedSizes)
    virtual std::unique_ptr<SizeQuery> createSizeQuery(std::uint32_t capacity) = 0;
    // bytes of the shared BLAS build scratch pool right now (0 before the first build)
    virtual std::uint64_t buildScratchBytes() const = 0;

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

// bytes per pixel of the uncompressed formats the RHI knows (0 = unknown or block compressed)
std::uint32_t formatBytes(VkFormat format);
// the texel block of a format: 1x1 for the uncompressed ones, 4x4 for BC1 - BC5; bytes = one block (0 = unknown format)
struct FormatBlock {
    std::uint32_t width = 1, height = 1, bytes = 0;
};
FormatBlock formatBlock(VkFormat format);
// the row pitch of a buffer <-> texture copy: a row of `width` texels (blocks) rounded up to 256 (D3D12's alignment, used on Vulkan too)
std::uint32_t bufferRowPitch(VkFormat format, std::uint32_t width);

}  // namespace rvk::rhi
