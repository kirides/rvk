#pragma once

// The coarse backend seam under the D3D9 device model (docs/research/dx12-game-port.md "Architecture"): rhi::GameDevice is an rhi::Device that also owns
// the frame loop of the game's renderer. d3d9::Device (one implementation of the D3D9 model, backend neutral) talks to it, and to nothing below it:
// no vk/*.h include, no Vulkan or D3D12 object in d3d9::Device's interface. Per-draw logic (prepareDraw, constants, DrawBlock, interning, batching,
// replacers) stays inline in d3d9; virtual calls happen per chunk / scope / pipeline resolve / resource (< 600 per frame), never per draw.
//
// Implementations: the Vulkan one lives in rhi/vulkan/vulkan_device.cpp (rhi::vulkan::wrapGame: over the renderer's vk::Context / Frames / Bindless;
// WP2b / WP2c move the remaining members into it), the D3D12 one will be rhi/d3d12/game_device.cpp.
//
// What is in this header (WP2a: GpuRef / GpuAddr / RingAlloc, ShaderModule; WP2b: resources, command lists, rings, capture pool, transients, optional modules):
//   built (WP2a)   GpuRef / GpuAddr / RingAlloc (the shader-visible and raw GPU addresses), ShaderModule + create / destroy, refShift
//   built (WP2b)   textures / buffers of the D3D9 model on rhi::Texture / rhi::Buffer (layout tracked by the RHI texture), the frame's command lists,
//                  the upload ring + mesh arena fronts, the vertex-capture pool, transient images, capabilities, format queries, the optional modules
//                  (rhi/modules.h: Upscaler, FrameGenerator, PresentWrapper, ImGuiBackend), adapters
//   WP2c           packet application / record threads (GamePipeline, ChunkHandle), pipeline resolve, StretchRect, read-backs, GPU timestamps, regions,
//                  the frame ring (RingAlloc source), swapchain / Presenter, the frame loop (begin / submit / retire)

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

    GpuAddr addr() const { return {ref, gpuVa}; }
};

// ---- shader modules ---------------------------------------------------------------------------------------------------------------

// One compiled shader stage of the backend (Vulkan VkShaderModule; D3D12: the DXIL blob kept for the PSO factory). Opaque: created and destroyed by the
// GameDevice, compared and hashed by value, None (0) = no module / compile failed / not built yet.
enum class ShaderModule : std::uint64_t { None = 0 };

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

// ---- adapters (before any device exists) --------------------------------------------------------------------------------------------

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
    virtual const GameCaps& gameCaps() const = 0;
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
    virtual RingAlloc allocRing(std::uint64_t size, std::uint64_t alignment = 256, std::uint64_t minOffset = 0) = 0;
    // The ring block as an rhi::Buffer (copy source / destination, build input): the chunk it lies in, owned by the device (valid until the device is destroyed)
    virtual Buffer& ringBuffer(const RingAlloc& alloc) = 0;
    // Memory that vertex shaders write (the vertex captures: rt skin outputs, object motion, positions of the RT scene's characters) and later passes read.
    // Vulkan: the ring; D3D12: a device-local UAV pool reset per frame (the CPU pointer may then be null: only `ref` / `gpuVa` are valid).
    virtual RingAlloc allocCapture(std::uint64_t size, std::uint64_t alignment = 16) = 0;
    // the frame being recorded: the value it signals (counts up), its slot (0 .. in flight - 1), the last completed value; waiting for a value blocks
    virtual std::uint64_t frameValue() const = 0;
    virtual std::uint32_t frameSlot() const = 0;
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

    // ---- optional modules (rhi/modules.h) ----
    // null when the backend has none (the caller keeps the feature off); the object is the caller's, retire it through retire() while the GPU may use it
    virtual std::unique_ptr<Upscaler> createUpscaler() = 0;
    virtual std::unique_ptr<FrameGenerator> createFrameGenerator() = 0;
    virtual std::unique_ptr<ImGuiBackend> createImGuiBackend() = 0;
};

}  // namespace rvk::rhi
