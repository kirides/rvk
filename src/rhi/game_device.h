#pragma once

// The coarse backend seam under the D3D9 device model (docs/research/dx12-game-port.md "Architecture"): rhi::GameDevice is an rhi::Device that also owns
// the frame loop of the game's renderer. d3d9::Device (one implementation of the D3D9 model, backend neutral) talks to it, and to nothing below it:
// no vk/*.h include, no Vulkan or D3D12 object in d3d9::Device's interface. Per-draw logic (prepareDraw, constants, DrawBlock, interning, batching,
// replacers) stays inline in d3d9; virtual calls happen per chunk / scope / pipeline resolve / resource (< 600 per frame), never per draw.
//
// Implementations: the Vulkan one lives in rhi/vulkan/vulkan_device.cpp (rhi::vulkan::wrapGame: over the renderer's vk::Context / Frames / Bindless;
// WP2b / WP2c move the remaining members into it), the D3D12 one will be rhi/d3d12/game_device.cpp.
//
// What is in this header today (WP2a) and what comes with the next steps (TODO list, kept in docs/research/dx12-game-port.md "Status"):
//   built (WP2a)   GpuRef / GpuAddr / RingAlloc (the shader-visible and raw GPU addresses), ShaderModule + create / destroy, refShift
//   WP2b           textures and passes on rhi::CommandList; optional-module interfaces (Upscaler, FrameGenerator, PresentWrapper)
//   WP2c           packet application / record threads (GamePipeline, ChunkHandle), pipeline resolve, StretchRect, read-backs, GPU timestamps, regions,
//                  the frame ring (RingAlloc source), mesh arena, swapchain / Presenter, the frame loop (begin / submit / retire)

#include <array>
#include <cstdint>
#include <string_view>

#include "rhi/rhi.h"

namespace rvk::rhi {

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
};

}  // namespace rvk::rhi
