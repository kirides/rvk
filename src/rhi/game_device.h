#pragma once

// The coarse backend seam under the D3D9 device model (docs/research/dx12-game-port.md "Architecture"): rhi::GameDevice is an rhi::Device that also owns
// the frame loop of the game's renderer. d3d9::Device (one implementation of the D3D9 model, backend neutral) talks to it, and to nothing below it:
// no vk/*.h include, no Vulkan or D3D12 object in d3d9::Device's interface. Per-draw logic (prepareDraw, constants, DrawBlock, interning, batching,
// replacers) stays inline in d3d9; virtual calls happen per chunk / scope / pipeline resolve / resource (< 600 per frame), never per draw.
//
// Implementations: the Vulkan one lives in rhi/vulkan/vulkan_device.cpp (rhi::createGameDevice(Backend::Vulkan, options): it owns the vk::Context / Frames /
// Bindless / MeshArena / pipeline caches and the presentation; rhi::vulkan::wrap / wrapGame = the same class over someone else's objects, for the tools),
// the D3D12 one will be rhi/d3d12/game_device.cpp.
//
// What stays outside the backend and is named Vulkan: the value vocabulary of this header and rhi.h (VkFormat, VkExtent2D, VkImageUsageFlags, VkFilter, the
// blend / compare / stencil / topology enums, VkViewport / VkRect2D / VkClearAttachment, VkPipelineStageFlags2 / VkAccessFlags2 of memoryBarrier, the 64-byte
// VkAccelerationStructureInstanceKHR record): plain data both backends map (D3D12 has its own table in rhi/d3d12). No Vulkan object, handle or call.

// What is in this header (WP2a: GpuRef / GpuAddr / RingAlloc, ShaderModule; WP2b: resources, command lists, rings, capture pool, transients, optional modules):
//   built (WP2a)   GpuRef / GpuAddr / RingAlloc (the shader-visible and raw GPU addresses), ShaderModule + create / destroy, refShift
//   built (WP2b)   textures / buffers of the D3D9 model on rhi::Texture / rhi::Buffer (layout tracked by the RHI texture), the frame's command lists,
//                  the upload ring + mesh arena fronts, the vertex-capture pool, transient images, capabilities, format queries, the optional modules
//                  (rhi/modules.h: Upscaler, FrameGenerator, PresentWrapper, ImGuiBackend), adapters
//   built (WP2c)   packet application / record threads (GamePipeline), pipeline resolve, StretchRect / read-backs (on CommandList), GPU timestamps, debug
//                  regions + crash markers, the frame ring (inline RingChunkState front end), the frame loop (beginFrame / submitFrame / retire),
//                  the presentation (GamePresenter: swapchain, present thread, frame generation swapchain, exclusive fullscreen), the null vertex
//                  stream, game samplers; the device owns every Vulkan object (createGameDevice): d3d9 / enh / overlay / hal / native hold none

#include <array>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "core/caps.h"
#include "rhi/modules.h"
#include "rhi/rhi.h"
#include "shader/abi.h"

namespace rvk::rhi {

// Frames in flight of the game device (slots of per-frame resources; both backends)
inline constexpr std::uint32_t kFramesInFlight = 2;

// ---- GPU references ---------------------------------------------------------------------------------------------------------------

// How a GpuRef counts bytes: the shift of a byte offset into the 64-bit value. Vulkan: 0 (a GpuRef is the buffer device address). D3D12: 32 (a GpuRef
// is `slot | offset << 32` = RvkBufRef{slot, offset}, the buffer's index in the bindless heap and a byte offset into it). Set once by the game device
// when it is created (GameDevice::refShift()); read by GpuRef::plus.
namespace detail {
inline std::uint32_t g_refShift = 0;
}
inline std::uint32_t refShift() { return detail::g_refShift; }
inline void setRefShift(std::uint32_t shift) { detail::g_refShift = shift; }

// What shaders read as an 8-byte reference (RVK_REF / the uint2 pairs of the ABI): one 64-bit value whose meaning belongs to the backend. The application
// copies it into blocks and push constants (`bits`, `pair()`) and moves it by byte offsets (`plus`); it never looks inside.
//
// Moving a reference (`plus`, used for the biased constant-window addresses `base - first * 16` too): the byte delta is added in the offset field, which
// for refShift 32 is the high word. A delta that does not fit (a negative one: the biased windows) wraps modulo 2^32 inside the high word and never touches
// the slot in the low word; a shader adds the (non-negative) index back with 32-bit arithmetic on the offset word and lands on the real offset.
// For refShift 0 the same expression is the plain 64-bit address arithmetic. 0 is the null reference (slot 0 / address 0 are never valid).
struct GpuRef {
    std::uint64_t bits = 0;

    constexpr GpuRef() = default;
    constexpr explicit GpuRef(std::uint64_t value) : bits(value) {}
    static constexpr GpuRef fromWords(std::uint32_t low, std::uint32_t high) { return GpuRef{static_cast<std::uint64_t>(low) | static_cast<std::uint64_t>(high) << 32}; }

    constexpr bool null() const { return bits == 0; }
    constexpr explicit operator bool() const { return bits != 0; }
    constexpr std::uint32_t low() const { return static_cast<std::uint32_t>(bits); }
    constexpr std::uint32_t high() const { return static_cast<std::uint32_t>(bits >> 32); }
    // {low, high}: the two 32-bit words of the ABI's uint2 references
    constexpr std::array<std::uint32_t, 2> pair() const { return {low(), high()}; }

    // moved by `delta` bytes (negative allowed), with an explicit refShift (unit tests, the shift of another backend)
    constexpr GpuRef plusAt(std::int64_t delta, std::uint32_t shift) const { return GpuRef{bits + (static_cast<std::uint64_t>(delta) << shift)}; }
    // moved by `delta` bytes with the active backend's refShift
    GpuRef plus(std::int64_t delta) const { return plusAt(delta, detail::g_refShift); }

    constexpr bool operator==(const GpuRef&) const = default;
};

// A location in GPU memory in both forms the renderer needs: the shader reference (`ref`) and the raw GPU virtual address (`va`: what acceleration structure
// builds take as vertexAddress / indexAddress / instances, rhi::Buffer::gpuAddress). On Vulkan the two are the same number; on D3D12 `ref` is
// {heap slot, offset} and `va` the resource's GPU virtual address.
struct GpuAddr {
    GpuRef ref;
    std::uint64_t va = 0;

    constexpr bool null() const { return va == 0 && ref.null(); }
    GpuAddr plus(std::int64_t delta) const { return {ref.plus(delta), va + static_cast<std::uint64_t>(delta)}; }
    constexpr bool operator==(const GpuAddr&) const = default;
};

// A block of the frame's transient (upload ring) memory: written by the CPU through `ptr`, read by the GPU through `ref` (shaders) / `gpuVa` (builds) until
// the frame that allocated it has completed. `buffer` + `offset` locate it for copies (an opaque backend handle: Vulkan VkBuffer; D3D12 the resource).
struct RingAlloc {
    std::uint64_t buffer = 0;
    std::uint64_t offset = 0;
    void* ptr = nullptr;
    GpuRef ref;
    std::uint64_t gpuVa = 0;
    // Vertex captures (GameDevice::allocCapture): the write form of `ref`. D3D12 reads a buffer through an SRV and writes it through a UAV: two heap
    // slots, so a capture has two references (`ref` for readers, `refRw` for the shaders that store into it). Null: `ref` serves both (Vulkan, the ring).
    GpuRef refRw;

    GpuAddr addr() const { return {ref, gpuVa}; }
    GpuRef rw() const { return refRw.null() ? ref : refRw; }
};

// The frame ring's current chunk, shared between the backend (which switches chunks) and the inline front end GameDevice::allocRing (called several
// times per draw: constants, DrawBlock, dynamic data): the fast path is plain arithmetic on this state, the backend is only asked for a new chunk.
struct RingChunkState {
    std::uint64_t buffer = 0;        // the chunk's backend buffer handle (RingAlloc::buffer)
    std::uint8_t* ptr = nullptr;     // null: no chunk yet this frame
    GpuRef ref;                      // the chunk's base reference
    std::uint64_t va = 0;            // ... and GPU address
    std::uint64_t size = 0, used = 0;
    std::uint64_t bytes = 0;         // statistics: bytes handed out since creation

    // a block in the current chunk or an empty RingAlloc (ptr null); `first` = the aligned minimum offset
    RingAlloc tryAlloc(std::uint64_t blockSize, std::uint64_t alignment, std::uint64_t first) {
        if (ptr == nullptr) return {};
        const std::uint64_t aligned = (used + alignment - 1) & ~(alignment - 1);
        const std::uint64_t offset = aligned > first ? aligned : first;
        if (offset + blockSize > size) return {};
        used = offset + blockSize;
        return {buffer, offset, ptr + offset, ref.plus(static_cast<std::int64_t>(offset)), va + offset};
    }
};

// ---- shader modules ---------------------------------------------------------------------------------------------------------------

// One compiled shader stage of the backend (Vulkan VkShaderModule; D3D12: the DXIL blob kept for the PSO factory). Opaque: created and destroyed by the
// GameDevice, compared and hashed by value, None (0) = no module / compile failed / not built yet.
enum class ShaderModule : std::uint64_t { None = 0 };

// A resolved graphics pipeline of the game's draw path, by value (Vulkan: the VkPipeline; D3D12: the PSO / an entry in the backend's
// factory). The D3D9 model compares and carries it (interned draws, batch grouping) but never interprets it.
enum class GamePipeline : std::uint64_t { None = 0 };

// ---- pipeline description (what a draw's shaders and fixed input need; byte-compared and hashed by the D3D9 model) ------------------

struct BlendAttachment {
    std::uint32_t enable = 0;
    VkBlendFactor srcColor = VK_BLEND_FACTOR_ONE;
    VkBlendFactor dstColor = VK_BLEND_FACTOR_ZERO;
    VkBlendOp colorOp = VK_BLEND_OP_ADD;
    VkBlendFactor srcAlpha = VK_BLEND_FACTOR_ONE;
    VkBlendFactor dstAlpha = VK_BLEND_FACTOR_ZERO;
    VkBlendOp alphaOp = VK_BLEND_OP_ADD;
    VkColorComponentFlags writeMask = 0xF;
};

struct VertexBinding {
    std::uint32_t stride = 0;
    std::uint32_t perInstance = 0;  // VK_VERTEX_INPUT_RATE_INSTANCE
};

struct VertexAttribute {
    std::uint32_t location = 0;
    std::uint32_t binding = 0;
    VkFormat format = VK_FORMAT_UNDEFINED;
    std::uint32_t offset = 0;
};

inline constexpr std::uint32_t kMaxColorTargets = 4;
inline constexpr std::uint32_t kMaxVertexBindings = 8;
inline constexpr std::uint32_t kMaxVertexAttributes = 16;

// Plain data, compared and hashed bytewise: always value-initialise (PipelineDesc d{}) so padding is zero.
struct PipelineDesc {
    rhi::ShaderModule vs = rhi::ShaderModule::None;
    rhi::ShaderModule fs = rhi::ShaderModule::None;
    VkPrimitiveTopology topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;  // only its class matters (topology is dynamic)
    VkPolygonMode polygonMode = VK_POLYGON_MODE_FILL;                     // ignored with EDS3
    std::uint32_t colorCount = 0;
    VkFormat colorFormats[kMaxColorTargets]{};
    VkFormat depthFormat = VK_FORMAT_UNDEFINED;
    VkFormat stencilFormat = VK_FORMAT_UNDEFINED;
    BlendAttachment blend[kMaxColorTargets]{};  // ignored with EDS3
    std::uint32_t bindingCount = 0;
    VertexBinding bindings[kMaxVertexBindings]{};
    std::uint32_t attributeCount = 0;
    VertexAttribute attributes[kMaxVertexAttributes]{};
};

// ---- the draw packet model (d3d9 builds packets and chunks from its state; the backend records them: pipeline binds,
// vertex input / dynamic state, push constants, draws. Virtual calls happen per chunk / scope / inline draw / pipeline
// resolve, never below that) ----

struct VertexInputValues {
    std::uint32_t bufferCount = 0;
    std::uint64_t buffers[kMaxVertexBindings]{};  // rhi Buffer handles (the Vulkan backend casts to VkBuffer)
    std::uint64_t offsets[kMaxVertexBindings]{};
    bool dynamicLayout = false;  // the layout below is recorded per draw, not baked in the pipeline
    std::uint32_t bindingCount = 0;
    std::uint32_t attributeCount = 0;
    VertexBinding bindings[kMaxVertexBindings]{};
    VertexAttribute attributes[kMaxVertexAttributes]{};
    std::int32_t vertexShift = 0;  // vertexOffset / firstVertex of a per-vertex stream bound at offset 0 (mesh arena)
};

struct DynamicStateValues {
    VkViewport viewport{};
    VkRect2D scissor{};
    VkCullModeFlags cull = 0;
    VkPrimitiveTopology topology{};
    VkBool32 depthTest = 0;
    VkBool32 depthWrite = 0;
    VkCompareOp depthCompare{};
    VkBool32 stencilTest = 0;
    VkStencilOp stencilFront[3]{};
    VkCompareOp stencilFrontCompare{};
    VkStencilOp stencilBack[3]{};
    VkCompareOp stencilBackCompare{};
    std::uint32_t stencilMasks[3]{};  // compare, write, reference
    VkBool32 depthBiasEnable = 0;
    float depthBias[2]{};             // constant, slope
    float blendConstants[4]{};
    std::uint32_t colorCount = 0;     // the blend arrays' used entries (extended dynamic state 3)
    VkBool32 blendEnable[kMaxColorTargets]{};
    VkColorBlendEquationEXT blendEquation[kMaxColorTargets]{};
    VkColorComponentFlags writeMask[kMaxColorTargets]{};
};

// an attachment clear recorded inside a rendering scope (the D3D9 clear of the scope's targets)
struct ClearOp {
    VkClearAttachment atts[kMaxColorTargets + 1]{};
    std::uint32_t count = 0;
    VkClearRect rect{};
};

struct DrawPacket {
    // Clear: chunk-only (`first` = the chunk's ClearOp index). DrawIndexedIndirect (frame-ownership.md 7.11, F2b-1): `count` indexed draws from the
    // VkDrawIndexedIndirectCommand records at indirectBuffer + indirectOffset (stride kIndirectStride), index binding and push block as DrawIndexed
    // DrawIndexedIndirectCount (F2c-4c, frame-ownership.md 7.32): up to `count` records at indirectBuffer + indirectOffset, the number that run = the uint32 at
    // indirectBuffer + `first` (a GPU-compacted block; the same buffer holds arguments and count; needs DeviceCaps::drawIndirectCount)
    enum class Op : std::uint8_t { Draw, DrawIndexed, Clear, DrawIndexedIndirect, DrawIndexedIndirectCount };
    Op op = Op::Draw;
    bool batchable = false;
    bool index32 = false;
    GamePipeline pipeline = GamePipeline::None;
    const VertexInputValues* vi = nullptr;  // interned by the D3D9 model (equal pointer = equal contents)
    const DynamicStateValues* dyn = nullptr;
    shader::abi::PushConstants push{};
    std::uint64_t indexBuffer = 0;  // rhi Buffer handle
    std::uint64_t indexOffset = 0;  // the binding's offset (0 when folded into first)
    std::uint32_t count = 0;        // vertices / indices
    std::uint32_t instances = 1;
    std::uint32_t first = 0;        // first vertex / index
    std::int32_t vertexOffset = 0;  // indexed draws
    std::uint64_t indirectBuffer = 0;  // DrawIndexedIndirect: rhi Buffer handle of the argument records (a ring block)
    std::uint64_t indirectOffset = 0;
    std::uint32_t scene = 0;        // d3d9 model only: 1 + the index of the draw's DrawRecord while a scene draw waits in the batch (0 = not a scene draw)
    std::uint32_t sceneGen = 0;     // d3d9 model only: the generation of the record list `scene` indexes (a flush starts a new one: F6-2 guard against a stale index)
};
inline constexpr std::uint32_t kIndirectStride = 20;  // sizeof(VkDrawIndexedIndirectCommand) = D3D12_DRAW_INDEXED_ARGUMENTS

// A block of packets the D3D9 model hands to the backend at a scope's chunk boundary; the backend may record it on a worker
// thread into secondary command buffers (when the scope began secondary) and executes them at the scope's end.
struct DrawChunk {
    std::vector<DrawPacket> packets;
    std::vector<ClearOp> clears;  // Op::Clear packets: `first` indexes it
    std::uint32_t colorCount = 0;
    VkFormat colorFormats[kMaxColorTargets]{};
    VkFormat depthFormat = VK_FORMAT_UNDEFINED;
    VkFormat stencilFormat = VK_FORMAT_UNDEFINED;
};

// Packets per chunk: a flush or this many packets closes a chunk (the worker's unit of work)
inline constexpr std::uint32_t kChunkPackets = 160;

// the pipeline cache's cumulative counters (diagnostics: the perf lines)
struct PipelineStats {
    std::uint64_t built = 0, libraries = 0, optimized = 0, prewarmed = 0, cached = 0;
    double buildMs = 0.0, libraryMs = 0.0, optimizeMs = 0.0, prewarmMs = 0.0;
    bool librariesEnabled = false;
};

// ---- capabilities, meshes, transients -----------------------------------------------------------------------------------------------

// What the renderer's enhancements ask the device beyond DeviceCaps
struct GameCaps {
    bool rayTracing = false;                    // the ray tracing tier is on (acceleration structures + ray queries, rt.allow and the GPU)
    bool vertexStores = false;                  // vertex shaders may write storage buffers (the vertex captures of the RT scene / object motion)
    bool storageImageExtendedFormats = false;   // storage images of the formats the G-buffer / GTAO use without a format qualifier
    std::uint32_t maxComputeWorkGroupInvocations = 0;
    std::uint32_t maxComputeWorkGroupSizeX = 0;
    std::uint32_t maxComputeSharedMemory = 0;   // bytes
    // texture uploads: the row pitch / buffer offset alignment of a buffer -> texture copy (Vulkan: tightly packed rows, 16; D3D12: 256 / 512)
    std::uint32_t uploadRowAlign = 1;
    std::uint32_t uploadOffsetAlign = 16;
    // what the draw path sets that is not part of the pipeline description: on Vulkan these are dynamic state extensions, on D3D12
    // they come from the PSO and the render-target selection
    bool extendedDynamicState3 = false;  // multi-target blend, write masks, depth bias, front face, topology, stencil ops
    bool vertexInputDynamicState = false;  // vertex input not baked into the pipeline: the layout is recorded per draw
    bool samplerAnisotropy = false;
    float maxAnisotropy = 1.0f;  // the device's anisotropy ratio cap (1 = none)
    // VK_EXT_full_screen_exclusive: the swapchain may take the exclusive fullscreen mode (the HAL's device bring-up offers it
    // as the `vulkan.exclusive_fullscreen` setting; on D3D12 a DXGI flip model swapchain has its own presentation interval)
    bool fullScreenExclusive = false;
    std::uint32_t maxVertexInputAttributes = 16;  // vertex input locations (the scene variant's ref stream is location 24: needs >= 25)
};

// A block of the mesh arena (static vertex / index data in device memory, shared big buffers; any thread). `buffer` is the arena buffer it lives in (owned by the
// device), `native` that buffer's backend handle for the draw packets (Vulkan: the VkBuffer, like RingAlloc::buffer), `addr` the data's address.
struct MeshAlloc {
    Buffer* buffer = nullptr;
    std::uint64_t native = 0;
    std::uint64_t offset = 0;   // of the data in the buffer (a multiple of the requested multiple)
    GpuAddr addr;               // null without device addresses
    std::uint64_t backend[3]{}; // the backend's bookkeeping for freeMesh
    bool valid() const { return buffer != nullptr; }
};

// A transient image of the passes by key (GameDevice::transient): created on first use, recreated when the description changes, destroyed after the GPU is
// done by releaseTransient / device destruction. Contents are undefined at the start of each frame's use (a pass that keeps history keeps the same key and
// description). SAMPLED usage: heapSampled() is the bindless slot.
struct TransientDesc {
    VkFormat format = VK_FORMAT_R16G16B16A16_SFLOAT;
    VkExtent2D extent{1, 1};
    VkImageUsageFlags usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    bool operator==(const TransientDesc& o) const {
        return format == o.format && extent.width == o.extent.width && extent.height == o.extent.height && usage == o.usage;
    }
};

// ---- GPU timestamps ([debug] perf) ----
// Marks of the frame's two command streams: a "begin" mark at the start of a submission and marks after each pass split
// into labelled segments (a render pass, a copy, the present blit). A slot's results are read once that slot's previous
// submission has completed, so reading never stalls. Backends without timestamp queries keep the timer off.
enum class TimestampStream : std::uint8_t { Main, Upload };
struct TimestampSegment {
    std::string label;
    double ms = 0.0;
};
struct TimestampResult {
    bool valid = false;
    double mainMs = 0.0;    // first to last mark of the main command list
    double uploadMs = 0.0;  // the same for the upload command list
    std::vector<TimestampSegment> segments;  // the main list's segments, only with details
};


// ---- the game device's creation and presentation ----------------------------------------------------------------------------------

struct GameDeviceOptions {
    bool validation = false;
    bool syncValidation = false;  // + synchronization validation ([vulkan] sync_validation)
    int adapter = -1;
    bool allowRayTracing = true;  // rt.allow
    bool frameGenQueues = false;  // framegen.mode != off at start: the extra queues of the frame generation swapchain
    bool d3d12Messages = false;   // D3D12: the debug layer's messages into rvk.log without GPU based validation ([debug] d3d12_messages)
    bool d3d12Dred = true;        // D3D12: DRED breadcrumbs + page fault data for a device removal report ([debug] d3d12_dred; RVK_DRED=0 off)
    bool asyncPipelines = true;   // D3D12: PSOs are built on background threads (a draw whose PSO is pending is skipped); false = built at first use (tests)
};

// A sampler of the D3D9 model (SetSamplerState translated by d3d9, the backend creates it and puts it into the bindless sampler heap)
struct GameSamplerDesc {
    VkFilter minFilter = VK_FILTER_LINEAR, magFilter = VK_FILTER_LINEAR;
    VkSamplerMipmapMode mipmap = VK_SAMPLER_MIPMAP_MODE_LINEAR;
    VkSamplerAddressMode addressU = VK_SAMPLER_ADDRESS_MODE_REPEAT, addressV = VK_SAMPLER_ADDRESS_MODE_REPEAT, addressW = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    float lodBias = 0.0f;
    float maxAnisotropy = 1.0f;  // > 1: anisotropic (clamped to GameCaps::maxAnisotropy)
    float minLod = 0.0f;
    bool singleLevel = false;    // maxLod = minLod (no mip filter)
    VkBorderColor border = VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK;
};

// Drawn on top of the frame at present (the in-game overlay, src/overlay): after the back buffer was copied into the swapchain image, so captures of
// the back buffer (rvk_screenshot, the engine snapshot) never contain it; or, for debugging, into the back buffer itself. Render thread.
class PresentOverlay {
public:
    virtual ~PresentOverlay() = default;
    // anything to draw this frame? false leaves the present path untouched
    virtual bool active() = 0;
    // records draws inside a dynamic rendering scope of one colour attachment of `extent` (LOAD / STORE; viewport, scissor,
    // pipeline and descriptor sets are the overlay's own)
    virtual void record(CommandList& cmd, VkExtent2D extent) = 0;
    // The AMD frame generation swapchain draws the overlay in its present callback (another thread, on the generated and the real
    // frame): prepare builds the UI on the render thread and keeps its draw lists (true: there is something to draw), recordPrepared
    // draws the latest ones into the scope (any thread; the same contract as record)
    virtual bool prepare(VkExtent2D /*extent*/) { return false; }
    virtual void recordPrepared(CommandList& /*cmd*/, VkExtent2D /*extent*/) {}
    virtual bool hasPrepared() { return false; }
};

struct PresentInfo {
    bool valid = false;  // a swapchain exists
    VkExtent2D extent{};
    VkFormat format = VK_FORMAT_B8G8R8A8_UNORM;
    std::uint32_t minImageCount = 0, imageCount = 0;
    int presentMode = 0;  // the backend's code (logs only)
};

// what the present thread reports of its latest present
enum class PresentStatus { Ok, Suboptimal, OutOfDate, ExclusiveLost, Error };

// The game device's presentation: swapchain on the game's window, the present thread, AMD's frame generation swapchain, exclusive fullscreen. The renderer
// draws into its own back buffer and the presenter copies it into the acquired image (blitFrame), so swapchain recreation never touches engine-visible
// resources. All calls on the render thread, except where noted; the swapchain belongs to the present thread until waitIdle() returned.
//
// One frame:  waitIdle -> acquire -> [blitFrame, drawOverlay] -> finishFrame -> GameDevice::submitFrame -> queuePresent
class GamePresenter {
public:
    virtual ~GamePresenter() = default;
    virtual bool create(void* window, std::uint32_t width, std::uint32_t height, bool vsync) = 0;
    // the present thread stops first (waitIdle before)
    virtual void destroy() = 0;
    virtual PresentInfo info() const = 0;
    bool valid() const { return info().valid; }
    virtual void setVsync(bool vsync) = 0;  // applies at the next recreate
    virtual bool recreate(std::uint32_t width, std::uint32_t height) = 0;  // the present thread idle

    // the present thread: until the queued present returned; the latest present's outcome since the last call (Ok when none; `error` = the API's text)
    virtual PresentStatus waitIdle(std::string* error = nullptr) = 0;
    virtual void waitDone() = 0;  // ... without collecting the result
    virtual double takePresentMs() = 0;  // summed ms inside the API's present (present thread), taken and zeroed

    // AMD's frame generation swapchain: the next recreate creates it instead of ours (a normal swapchain when `on` is false); a build that cannot create
    // it falls back to a normal one (frameGenReason()). `loaderDll` = the FidelityFX loader.
    virtual void setFrameGen(bool on, const std::filesystem::path& loaderDll = {}) = 0;
    virtual bool frameGenWanted() const = 0;
    virtual bool frameGenActive() const = 0;
    virtual const std::string& frameGenReason() const = 0;
    // tears the frame generation swapchain down without building another (the generator's context must be gone)
    virtual void releaseFrameGen() = 0;
    // the frame generation swapchain as the RHI's PresentWrapper (null while it is not in use)
    virtual PresentWrapper* presentWrapper() = 0;
    // until the previous present returned and the GPU work it queued (interpolation) is done (true: done within 2 s)
    virtual bool waitFrameGenPresent() = 0;

    // Exclusive fullscreen (docs/research/window-mode.md 8): `monitor` != null = the next recreate builds the swapchain with the exclusive structures
    virtual void setFullScreenExclusive(void* monitor) = 0;
    virtual bool fullScreenExclusiveConfigured() const = 0;
    virtual bool fullScreenExclusiveHeld() const = 0;
    virtual bool acquireFullScreenExclusive(std::string* error) = 0;  // the present thread idle
    virtual void releaseFullScreenExclusive() = 0;
    virtual void noteExclusiveLost() = 0;  // a present returned "lost": the exclusive mode is no longer held
    virtual bool takeExclusiveLost() = 0;  // an acquire reported it (taken and cleared)

    // ---- one frame ----
    // acquires the next image; false = drop the frame's presentation (minimised / out of date / exclusive mode lost: exclusiveLost(); frame generation
    // had too many acquired: acquireNotReady()); the frame's work is still submitted
    virtual bool acquire() = 0;
    virtual bool exclusiveLost() const = 0;
    virtual bool acquireNotReady() const = 0;
    // copies `src` (any size, scaled linearly) into the acquired image (records into the frame's main list); the source ends in TRANSFER_SRC
    virtual void blitFrame(Texture& src, VkExtent3D srcExtent) = 0;
    // the overlay on top (frame generation: builds its draw lists for the swapchain's present callback instead)
    virtual void drawOverlay(PresentOverlay& overlay) = 0;
    // the acquired image into the state the presentation reads
    virtual void finishFrame() = 0;
    // after GameDevice::submitFrame: hands the image to the present thread
    virtual void queuePresent() = 0;
};

// ---- adapters (before any device exists) ----

struct AdapterInfo {
    std::string name;
    std::uint32_t vendorId = 0, deviceId = 0, driverVersion = 0;
    std::uint64_t vramMB = 0;
    bool usable = false;    // meets the renderer's floor
    bool discrete = false;
};
// Every adapter of the API in the API's own order (Vulkan: physical devices). Creates and destroys a temporary instance (~0.1 s). D3D12: not enumerated yet
// (empty).
std::vector<AdapterInfo> enumerateAdapters(Backend backend);
// the adapters of the last Vulkan enumeration (no new instance: safe to call while the game runs)
std::vector<AdapterInfo> lastEnumeratedAdapters();
// `debug.names` (a restart setting): the devices name their objects for graphics debuggers and validation messages as they create them; set before any
// device exists (Vulkan: VK_EXT_debug_utils names, vk/debug_names.h)
void setDebugNames(bool enabled);
bool debugNamesEnabled();

// ---- the game device --------------------------------------------------------------------------------------------------------------

class GameDevice : public Device {
public:
    // The shift of GpuRef::plus on this backend (0 Vulkan, 32 D3D12); the device sets the process-wide value (setRefShift) when it is created.
    virtual std::uint32_t refShift() const = 0;

    // A shader stage of the game's pipelines from one compilation of an HLSL entry point (rhi::Shader: the SPIR-V and / or the DXIL; a backend uses its own).
    // Any thread (the shader compile workers call it). `name` = debug name (null = none). ShaderModule::None on failure.
    virtual ShaderModule createShaderModule(const Shader& code, const char* name = nullptr) = 0;
    // `deferred`: the GPU may still use it (a pipeline built from it is bound in a frame in flight): destroyed once the frame being recorded has completed;
    // else immediately (device idle: shutdown, start-up). Render thread.
    virtual void destroyShaderModule(ShaderModule module, bool deferred) = 0;

    // ---- capabilities ----
    // (non-virtual: read per draw)
    const GameCaps& gameCaps() const { return gameCaps_; }
    // The bindless slot an unbound / unusable sampler of the shader's texture type reads (shader::TextureType: 2 = 2D, 3 = cube, 4 = volume): a texel
    // (0, 0, 0, 1) of that kind. Vulkan keeps one default per array at index 0; D3D12's one heap needs a descriptor of each dimension (slots 0, 1, 2).
    std::uint32_t defaultTextureSlot(std::uint32_t textureType) const { return textureType == 3 ? defaultSlots_[1] : textureType == 4 ? defaultSlots_[2] : defaultSlots_[0]; }
    // Whether `format` supports every use in `usage` (optimal tiling): SAMPLED, COLOR_ATTACHMENT, DEPTH_STENCIL_ATTACHMENT, STORAGE, TRANSFER_SRC (= a blit source);
    // `blend`: a colour attachment that blends too.
    virtual bool formatSupport(VkFormat format, VkImageUsageFlags usage, bool blend = false) const = 0;

    // ---- the frame's command lists (render thread) ----
    // The main list of the frame being recorded (outside of any rendering scope its commands are the renderer's own). The same object for the whole frame.
    virtual CommandList& frameCommands() = 0;
    // The upload list: copies that must land before the frame's draws; begun lazily (the first call of a frame begins it). The caller orders it after the
    // earlier work (d3d9::Device::uploadList).
    virtual CommandList& uploadCommands() = 0;
    // frameCommands() with its bound pipeline / pending bindings reset: what an injected pass records into (a pass sees no state of the one before)
    virtual CommandList& passCommands() = 0;
    // Backend escape hatch, explicitly Vulkan-only: the VkCommandBuffer a list records into, for code that calls vendor DLLs which record into it
    // (AMD's FFX Vulkan backends, ImGui's Vulkan backend). null on every other backend.
    virtual void* nativeCommandBuffer(CommandList& list) = 0;

    // ---- the frame ring and transient memory ----
    // transient memory for the frame being recorded (render thread): persistently mapped, GPU readable until the frame has completed; `alignment` a power
    // of two; `minOffset`: the block starts at least that far into its chunk (a dynamic buffer's block of bytes [lo, hi) is bound at offset - lo)
    // (non-virtual: the inline front end over RingChunkState; the backend is asked for a new chunk only when the current one is full)
    RingAlloc allocRing(std::uint64_t size, std::uint64_t alignment = 256, std::uint64_t minOffset = 0) {
        const std::uint64_t first = (minOffset + alignment - 1) & ~(alignment - 1);
        if (const RingAlloc a = ring_.tryAlloc(size, alignment, first); a.ptr != nullptr) {
            ring_.bytes += size;
            return a;
        }
        return allocRingSlow(size, alignment, minOffset);  // (counts the bytes itself)
    }
    // bytes handed out by the ring since creation (statistics)
    std::uint64_t ringBytes() const { return ring_.bytes; }
    // The ring block as an rhi::Buffer (copy source / destination, build input): the chunk it lies in, owned by the device (valid until the device is destroyed)
    virtual Buffer& ringBuffer(const RingAlloc& alloc) = 0;
    // The handle of any rhi Buffer in a DrawPacket's indirectBuffer (what RingAlloc::buffer is for ring blocks): the buffer needs INDIRECT usage and, on DX12, to be in
    // the INDIRECT_ARGUMENT state (bufferBarrier IndirectRead) when the packet is recorded
    virtual std::uint64_t packetHandle(Buffer& buffer) const = 0;
    // Memory that vertex shaders write (the vertex captures: rt skin outputs, object motion, positions of the RT scene's characters) and later passes read.
    // Vulkan: the ring; D3D12: a device-local UAV pool reset per frame (the CPU pointer may then be null: only `ref` / `gpuVa` are valid).
    virtual RingAlloc allocCapture(std::uint64_t size, std::uint64_t alignment = 16) = 0;
    // The capture memory's use by the commands that follow in `list`: write = vertex shaders / compute store into it (`RingAlloc::rw()`), else it is read
    // (acceleration structure builds, ray queries, shaders loading it: `ref`). A no-op on Vulkan; D3D12 moves the pool between UNORDERED_ACCESS and a read
    // state (builds, dispatches and rendering scopes do it themselves: only a compute pass that writes captures needs to say so).
    virtual void captureAccess(CommandList& /*list*/, bool /*write*/) {}
    // the frame being recorded: the value it signals (counts up), its slot (0 .. in flight - 1), the last completed value; waiting for a value blocks
    std::uint64_t frameValue() const { return frameValue_; }
    std::uint32_t frameSlot() const { return frameSlot_; }
    virtual std::uint64_t completedFrame() const = 0;
    virtual void waitFrame(std::uint64_t value) = 0;
    // runs `fn` when the frame being recorded (or, between frames, the last submitted one) has completed (any thread): destruction of what the GPU may still use
    virtual void retire(std::function<void()> fn) = 0;

    // ---- the mesh arena (the D3D9 model's static vertex / index buffers) ----
    // `size` bytes at an offset that is a multiple of `multiple` (a vertex stride; always 4-aligned too); any thread. An invalid MeshAlloc when out of memory.
    virtual MeshAlloc allocMesh(std::uint64_t size, std::uint32_t multiple) = 0;
    // `deferred`: the GPU may still read it: freed once the frame being recorded has completed; else at once. Any thread.
    virtual void freeMesh(const MeshAlloc& alloc, bool deferred) = 0;

    // ---- transient images ----
    // the image of `key` with `desc` (null on failure); logs the first creation. Render thread.
    virtual Texture* transient(std::string_view key, const TransientDesc& desc) = 0;
    virtual void releaseTransient(std::string_view key) = 0;
    // bytes of all transient images (diagnostics)
    virtual std::uint64_t transientBytes() const = 0;

    // ---- GPU timestamps ([debug] perf) ----
    // create the timer, once at device creation (only called when the renderer's [debug] perf is on); false = the backend
    // cannot time (no query pool / no timing queue): the renderer keeps its marks out
    virtual bool enableTimestamps() = 0;
    virtual bool timestampsEnabled() const = 0;
    // draws skipped so far because their pipeline was still being built (D3D12's background PSOs; 0 where pipelines are built at first use): a compare of two
    // images drawn in this window is not valid when it moved (frame-ownership.md 14.24)
    virtual std::uint64_t pipelineSkips() const { return 0; }
    // starts recording a slot; `details` keeps the labels of the main stream's marks for TimestampResult::segments
    virtual void timestampStart(std::uint32_t slot, bool details) = 0;
    virtual bool timestampDetails(std::uint32_t slot) const = 0;
    // writes a timestamp after all earlier commands of `list`; `label` names the work since the previous mark of that stream
    virtual void timestampMark(CommandList& list, std::uint32_t slot, TimestampStream stream, const char* label) = 0;
    virtual void timestampMark(CommandList& list, std::uint32_t slot, TimestampStream stream, std::string&& label) = 0;
    // reads the slot's previous submission (it must have completed) and resets the slot for reuse
    virtual TimestampResult timestampCollect(std::uint32_t slot) = 0;

    // ---- the draw path (pipeline resolve, scopes, chunks) ----
    // The pipeline of `desc`, created on demand (`created`: this call made a new one) or null while a background build
    // is pending. The backend owns the cache, its prewarm threads, its pipeline libraries and the saved blob.
    virtual bool createPipelineCaches(const std::filesystem::path& cacheFile, bool libraries) = 0;
    virtual GamePipeline resolvePipeline(const PipelineDesc& desc, bool* created = nullptr) = 0;
    // the pipeline of `desc` when the cache has one, else None (never builds)
    virtual GamePipeline findPipeline(const PipelineDesc& desc) = 0;
    // whether a pipeline of `desc` exists (built or prewarmed)
    virtual bool pipelineContains(const PipelineDesc& desc) = 0;
    // builds `desc` on a background thread unless it exists (pipelines known from earlier runs)
    virtual void prewarmPipeline(const PipelineDesc& desc) = 0;
    // the same for descriptions that repeat every frame until the pipeline exists: queued once
    virtual void prewarmOncePipeline(const PipelineDesc& desc) = 0;
    // with pipeline libraries: compiles the shader's pre-rasterization / fragment library now (any thread)
    virtual void preparePipelineShader(ShaderModule module, VkShaderStageFlagBits stage) = 0;
    // writes the backend's pipeline cache blob to its file, on a background thread when built since the last save
    virtual void savePipelineCache() = 0;
    // device teardown: the background pipeline builders (Vulkan prewarm workers + the library optimiser, D3D12 PSO threads) are stopped and joined, their queues
    // dropped, a build in flight finished; later prewarm requests are ignored. Called before the shader modules are destroyed (a worker still inside
    // vkCreateGraphicsPipelines with a destroyed module crashed amdvlk64 / hung the exit). Idempotent.
    virtual void stopPipelineBuilds() = 0;
    virtual PipelineStats pipelineStats() = 0;

    // A rendering scope: the backend records its begin (attachments transitioned and viewed at mip/layer; secondary
    // contents mode when `secondary`); endScope executes queued chunks in order, rebinds the frame's bindless state and
    // invalidates the frame list's recorded state. Between them d3d9 keeps its own viewport / dynamic state bookkeeping.
    virtual void beginScope(const RenderingInfo& info, bool secondary) = 0;
    virtual void endScope() = 0;
    // hand a chunk of packets to the backend (recorded on a worker thread when the scope began secondary)
    virtual void scopeChunk(DrawChunk&& chunk) = 0;
    // record one packet into the frame's command list now (the inline path: not batched, not threaded)
    virtual void applyDrawPacket(const DrawPacket& packet) = 0;
    // a debug label in order with the scope's chunks (labels of the region stack inside a threaded scope)
    virtual void scopeLabel(bool begin, const char* text) = 0;
    // the frame list's recorded pipeline / vertex input / dynamic state: undefined for the next draw (after a scope
    // end, a pass boundary, an external command)
    virtual void invalidateRecordingState() = 0;
    // pipelines bound into the frame's command list since the last call (the perf counter)
    virtual std::uint64_t takePipelineBinds() = 0;
    // a new frame slot: the backend's secondary command buffer pools of that slot are reset (its command buffers
    // were executed and the frame they belonged to has completed)
    virtual void resetRecordPools() = 0;

    // ---- start-up diagnostics (the HAL's device bring-up) ----
    // which frame generation providers the vendor's API offers on this device (loads the loader DLL, lists, unloads: a few tens of ms); the result goes
    // into capabilityReport. Call it before.
    virtual void probeFrameGeneration(const std::filesystem::path& loaderDll) = 0;
    // the device's capability report (core/caps.h: what the GPU / driver / enabled features offer; logged, shown by the overlay, resolves settings)
    virtual caps::Report capabilityReport(bool pipelineLibraries) = 0;
    // builds a BLAS + TLAS and traces them with a ray query shader, logs "rt selftest: ..." (debug.rt_selftest)
    virtual void runRaytracingSelfTest() = 0;
    // logs the "perf: GPU memory ..." and "perf: mesh arena ..." lines (the allocator's heap statistics)
    virtual void logMemoryStats() = 0;
    // device-local video memory in use / the budget the OS gives this process (the overlay's VRAM bar); false = unknown
    virtual bool videoMemory(std::uint64_t& usedBytes, std::uint64_t& budgetBytes) { (void)usedBytes; (void)budgetBytes; return false; }

    // ---- optional modules (rhi/modules.h) ----
    // null when the backend has none (the caller keeps the feature off); the object is the caller's, retire it through retire() while the GPU may use it
    virtual std::unique_ptr<Upscaler> createUpscaler() = 0;
    virtual std::unique_ptr<FrameGenerator> createFrameGenerator() = 0;
    virtual std::unique_ptr<ImGuiBackend> createImGuiBackend() = 0;

    // ---- the frame loop (render thread) ----
    // starts the next frame: waits for its slot (the GPU may be a full slot behind: lastSlotWaitMs), runs the retirements that came due, begins the
    // command lists (a new frame slot resets the recording state: invalidateRecordingState + resetRecordPools are part of it)
    virtual void beginFrame() = 0;
    virtual double lastSlotWaitMs() const = 0;
    // ends and submits the frame's upload and main lists (the presenter's acquire / rendered semaphores ride along when an image is acquired).
    // `finalMark` labels the last GPU timestamp segment ("present blit ..." / "copies ... (flush)")
    virtual void submitFrame(const char* finalMark) = 0;
    // waits for the GPU to finish everything submitted
    virtual void waitIdle() = 0;
    virtual bool namesEnabled() const = 0;

    // ---- device-level objects of the D3D9 model ----
    // a sampler in the bindless sampler heap (its index; 0 on failure); `name` for graphics debuggers (empty = none)
    virtual SamplerIndex createGameSampler(const GameSamplerDesc& desc, const std::string& name) = 0;
    // A 16-byte vertex buffer holding (0, 0, 0, 1): what vertex inputs the declaration does not provide read. The handle goes into
    // VertexInputValues::buffers (0 on failure).
    std::uint64_t nullVertexBuffer() const { return nullVertexBuffer_; }  // (non-virtual: read per draw)
    // an attachment clear inside the open rendering scope of the main list (the inline path of D3D9 Clear)
    virtual void clearAttachments(const ClearOp& op) = 0;

    // ---- debug regions and crash markers (docs/research/capture-regions.md) ----
    // labels in the frame's main command buffer (a colour per name); available when the backend has a labelling extension
    virtual bool regionsAvailable() const = 0;
    virtual void labelBegin(const char* text) = 0;
    virtual void labelEnd() = 0;
    // debug.crash_markers: events written when the GPU reaches a region's start / end, reported on device loss; false = not available
    virtual bool setCrashMarkers(bool on) = 0;
    virtual void crashMarker(bool begin, const char* text) = 0;

    // ---- presentation ----
    virtual GamePresenter& presenter() = 0;

protected:
    // the ring front end's state and the frame counters (the backend keeps them current: beginFrame / chunk switches)
    RingChunkState ring_;
    GameCaps gameCaps_;
    std::uint32_t defaultSlots_[3] = {0, 0, 0};  // defaultTextureSlot: 2D, cube, volume
    std::uint64_t nullVertexBuffer_ = 0;  // the handle of nullVertexBuffer()
    std::uint64_t frameValue_ = 1;  // the value the frame being recorded signals
    std::uint32_t frameSlot_ = 0;
    // a block that did not fit the current chunk: the backend switches chunks (ring_ is updated) and allocates
    virtual RingAlloc allocRingSlow(std::uint64_t size, std::uint64_t alignment, std::uint64_t minOffset) = 0;
};

// The D3D12 backend (rhi/d3d12, delay-loaded d3d12.dll / dxgi.dll: only called when the session runs on D3D12): null with a logged reason when the GPU lacks
// Shader Model 6.6 / resource binding tier 3 (the caller falls back to Vulkan)
std::unique_ptr<GameDevice> createD3D12GameDevice(const GameDeviceOptions& options);
std::vector<AdapterInfo> enumerateD3D12Adapters();
// whether the D3D12 backend can run here (adapter 12_0, SM 6.6, binding tier 3); `reason` = why not
bool d3d12Usable(int adapter, std::string* reason);

// The game device of `backend` (it owns the API instance, device, frame ring, heaps, pipeline caches and the presentation). Vulkan only for now: null
// (logged) for D3D12.
std::unique_ptr<GameDevice> createGameDevice(Backend backend, const GameDeviceOptions& options);

// What the start-up probe logs about the backend's adapters (docs/PLAN.md "Vulkan floor"); a throw-away instance, not under the loader lock.
void probeBackend(Backend backend);

}  // namespace rvk::rhi
