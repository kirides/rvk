#pragma once

// Ray tracing building blocks on the optional tier (docs/research/raytracing.md): buffers with device addresses,
// bottom- / top-level acceleration structure builds, and a self-test that proves the path end to end on this GPU (BLAS ->
// TLAS -> GL_EXT_ray_query in a GLSL compute shader -> pixels checked). Only usable when
// Context::features().rayTracing.enabled. The TLAS reaches shaders by its device address (accelerationStructureEXT(uint64_t)
// in GLSL), so it fits the push-constant / buffer-address ABI without a descriptor.

#include <cstdint>
#include <span>
#include <string>

#include "vk/vk.h"

namespace rvk::vk {
class Context;
}

namespace rvk::vk::rt {

// a VMA buffer with its device address (and a mapping when host-visible)
struct AddressBuffer {
    VkBuffer buffer = VK_NULL_HANDLE;
    VmaAllocation allocation = VK_NULL_HANDLE;
    VkDeviceAddress address = 0;
    VkDeviceSize size = 0;
    void* mapped = nullptr;

    // SHADER_DEVICE_ADDRESS is always added to `usage`; `alignment` 0 = the buffer's own requirement
    bool create(Context& ctx, VkDeviceSize size, VkBufferUsageFlags usage, bool hostVisible, VkDeviceSize alignment,
                const char* name);
    void destroy(Context& ctx);
};

struct AccelerationStructure {
    VkAccelerationStructureKHR handle = VK_NULL_HANDLE;
    AddressBuffer storage;
    VkDeviceAddress address = 0;  // vkGetAccelerationStructureDeviceAddressKHR (TLAS: what shaders get)
    void destroy(Context& ctx);
};

// one geometry of a BLAS: indexed triangles read by address (buffers created with
// ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY + SHADER_DEVICE_ADDRESS usage: the static VB / IB of the renderer when the
// tier is on, docs/research/raytracing.md 2)
struct Triangles {
    VkDeviceAddress vertices = 0;  // first position
    VkFormat vertexFormat = VK_FORMAT_R32G32B32_SFLOAT;
    VkDeviceSize vertexStride = 12;
    std::uint32_t maxVertex = 0;   // highest vertex index referenced
    VkDeviceAddress indices = 0;
    VkIndexType indexType = VK_INDEX_TYPE_UINT32;
    std::uint32_t triangleCount = 0;
    bool opaque = true;            // false: ray queries see the triangles as candidates (alpha test in the shader)
};

// Records the build of a BLAS of `geometries` into `cmd`; creates `out` and `scratch` (keep both alive until the command
// buffer has executed). False (logged) on failure.
bool buildBottomLevel(Context& ctx, VkCommandBuffer cmd, std::span<const Triangles> geometries,
                      VkBuildAccelerationStructureFlagsKHR flags, AccelerationStructure& out, AddressBuffer& scratch);
// Records the build of a TLAS over `count` VkAccelerationStructureInstanceKHR at `instances` (16-byte aligned, buffer with
// BUILD_INPUT_READ_ONLY usage). The BLASes must be built before (buildBarrier between).
bool buildTopLevel(Context& ctx, VkCommandBuffer cmd, VkDeviceAddress instances, std::uint32_t count,
                   VkBuildAccelerationStructureFlagsKHR flags, AccelerationStructure& out, AddressBuffer& scratch);
// acceleration structure build writes -> `dstStage` reads (the next build: ACCELERATION_STRUCTURE_BUILD, ray queries:
// COMPUTE_SHADER / FRAGMENT_SHADER)
void buildBarrier(VkCommandBuffer cmd, VkPipelineStageFlags2 dstStage);

struct SelfTestResult {
    bool ran = false;   // false: the tier is not enabled (detail says why)
    bool pass = false;
    std::string detail;
    double milliseconds = 0.0;
};
// BLAS (one triangle) -> TLAS (two instances: one opaque, one non-opaque with a stand-in alpha test in the shader, different
// transforms and custom indices) -> 64x64 orthographic rays with GL_EXT_ray_query in shaders/test/rt_query.comp -> every
// pixel checked against the analytic result (instance, hit distance, primitive). Own command pool and fence, blocks;
// submits through Context::queueSubmit (safe beside the renderer's present thread).
SelfTestResult selfTest(Context& ctx);

}  // namespace rvk::vk::rt
