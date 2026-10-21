#include "vk/raytracing.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstring>
#include <format>
#include <vector>

#include <spdlog/spdlog.h>

#include "shader/embedded.h"
#include "vk/context.h"
#include "vk/passes.h"

namespace rvk::vk::rt {

bool AddressBuffer::create(Context& ctx, VkDeviceSize bytes, VkBufferUsageFlags usage, bool hostVisible, VkDeviceSize alignment,
                           const char* name) {
    VkBufferCreateInfo bci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bci.size = bytes;
    bci.usage = usage | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;
    VmaAllocationCreateInfo aci{};
    aci.usage = hostVisible ? VMA_MEMORY_USAGE_AUTO : VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
    if (hostVisible) aci.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;
    VmaAllocationInfo info{};
    const VkResult r = alignment != 0
                           ? vmaCreateBufferWithAlignment(ctx.allocator(), &bci, &aci, alignment, &buffer, &allocation, &info)
                           : vmaCreateBuffer(ctx.allocator(), &bci, &aci, &buffer, &allocation, &info);
    if (r != VK_SUCCESS) {
        spdlog::error("vulkan: rt buffer '{}' ({} bytes): {}", name != nullptr ? name : "?", bytes, resultString(r));
        buffer = VK_NULL_HANDLE;
        allocation = VK_NULL_HANDLE;
        return false;
    }
    size = bytes;
    mapped = info.pMappedData;
    VkBufferDeviceAddressInfo dai{VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO};
    dai.buffer = buffer;
    address = vkGetBufferDeviceAddress(ctx.device(), &dai);
    if (name != nullptr) ctx.setName(buffer, VK_OBJECT_TYPE_BUFFER, name);
    return true;
}

void AddressBuffer::destroy(Context& ctx) {
    if (buffer != VK_NULL_HANDLE) vmaDestroyBuffer(ctx.allocator(), buffer, allocation);
    *this = {};
}

void AccelerationStructure::destroy(Context& ctx) {
    if (handle != VK_NULL_HANDLE) vkDestroyAccelerationStructureKHR(ctx.device(), handle, nullptr);
    handle = VK_NULL_HANDLE;
    address = 0;
    storage.destroy(ctx);
}

namespace {

// sizes -> storage + AS object + scratch, then the build command
bool build(Context& ctx, VkCommandBuffer cmd, VkAccelerationStructureTypeKHR type,
           VkAccelerationStructureBuildGeometryInfoKHR& info, std::span<const VkAccelerationStructureBuildRangeInfoKHR> ranges,
           std::span<const std::uint32_t> maxPrimitives, AccelerationStructure& out, AddressBuffer& scratch, const char* what) {
    VkAccelerationStructureBuildSizesInfoKHR sizes{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR};
    vkGetAccelerationStructureBuildSizesKHR(ctx.device(), VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR, &info,
                                            maxPrimitives.data(), &sizes);
    if (!out.storage.create(ctx, sizes.accelerationStructureSize, VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR, false,
                            256, what)) {
        return false;
    }
    VkAccelerationStructureCreateInfoKHR ci{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_CREATE_INFO_KHR};
    ci.buffer = out.storage.buffer;
    ci.size = sizes.accelerationStructureSize;
    ci.type = type;
    if (const VkResult r = vkCreateAccelerationStructureKHR(ctx.device(), &ci, nullptr, &out.handle); r != VK_SUCCESS) {
        spdlog::error("vulkan: vkCreateAccelerationStructureKHR ({}): {}", what, resultString(r));
        out.destroy(ctx);
        return false;
    }
    ctx.setName(out.handle, VK_OBJECT_TYPE_ACCELERATION_STRUCTURE_KHR, what);
    const VkDeviceSize align = std::max<VkDeviceSize>(ctx.features().rayTracing.minScratchOffsetAlignment, 16);
    if (!scratch.create(ctx, std::max<VkDeviceSize>(sizes.buildScratchSize, 4), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, false, align,
                        "rt build scratch")) {
        out.destroy(ctx);
        return false;
    }
    VkAccelerationStructureDeviceAddressInfoKHR ai{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_DEVICE_ADDRESS_INFO_KHR};
    ai.accelerationStructure = out.handle;
    out.address = vkGetAccelerationStructureDeviceAddressKHR(ctx.device(), &ai);
    info.dstAccelerationStructure = out.handle;
    info.scratchData.deviceAddress = scratch.address;
    const VkAccelerationStructureBuildRangeInfoKHR* pRanges = ranges.data();
    vkCmdBuildAccelerationStructuresKHR(cmd, 1, &info, &pRanges);
    return true;
}

}  // namespace

bool buildBottomLevel(Context& ctx, VkCommandBuffer cmd, std::span<const Triangles> geometries,
                      VkBuildAccelerationStructureFlagsKHR flags, AccelerationStructure& out, AddressBuffer& scratch) {
    std::vector<VkAccelerationStructureGeometryKHR> geoms;
    std::vector<VkAccelerationStructureBuildRangeInfoKHR> ranges;
    std::vector<std::uint32_t> counts;
    for (const Triangles& t : geometries) {
        VkAccelerationStructureGeometryKHR g{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR};
        g.geometryType = VK_GEOMETRY_TYPE_TRIANGLES_KHR;
        g.flags = t.opaque ? VK_GEOMETRY_OPAQUE_BIT_KHR : 0;
        auto& tri = g.geometry.triangles;
        tri.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_TRIANGLES_DATA_KHR;
        tri.vertexFormat = t.vertexFormat;
        tri.vertexData.deviceAddress = t.vertices;
        tri.vertexStride = t.vertexStride;
        tri.maxVertex = t.maxVertex;
        tri.indexType = t.indexType;
        tri.indexData.deviceAddress = t.indices;
        geoms.push_back(g);
        ranges.push_back({t.triangleCount, 0, 0, 0});
        counts.push_back(t.triangleCount);
    }
    VkAccelerationStructureBuildGeometryInfoKHR info{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR};
    info.type = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR;
    info.flags = flags;
    info.mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR;
    info.geometryCount = static_cast<std::uint32_t>(geoms.size());
    info.pGeometries = geoms.data();
    return build(ctx, cmd, VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR, info, ranges, counts, out, scratch, "rt blas");
}

bool buildTopLevel(Context& ctx, VkCommandBuffer cmd, VkDeviceAddress instances, std::uint32_t count,
                   VkBuildAccelerationStructureFlagsKHR flags, AccelerationStructure& out, AddressBuffer& scratch) {
    VkAccelerationStructureGeometryKHR g{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR};
    g.geometryType = VK_GEOMETRY_TYPE_INSTANCES_KHR;
    g.geometry.instances.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_INSTANCES_DATA_KHR;
    g.geometry.instances.arrayOfPointers = VK_FALSE;
    g.geometry.instances.data.deviceAddress = instances;
    VkAccelerationStructureBuildGeometryInfoKHR info{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR};
    info.type = VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR;
    info.flags = flags;
    info.mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR;
    info.geometryCount = 1;
    info.pGeometries = &g;
    const VkAccelerationStructureBuildRangeInfoKHR range{count, 0, 0, 0};
    return build(ctx, cmd, VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR, info, {&range, 1}, {&count, 1}, out, scratch, "rt tlas");
}

void buildBarrier(VkCommandBuffer cmd, VkPipelineStageFlags2 dstStage) {
    VkMemoryBarrier2 b{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
    b.srcStageMask = VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_BUILD_BIT_KHR;
    b.srcAccessMask = VK_ACCESS_2_ACCELERATION_STRUCTURE_WRITE_BIT_KHR;
    b.dstStageMask = dstStage;
    b.dstAccessMask = VK_ACCESS_2_ACCELERATION_STRUCTURE_READ_BIT_KHR;
    if (dstStage == VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_BUILD_BIT_KHR) {
        b.dstAccessMask |= VK_ACCESS_2_ACCELERATION_STRUCTURE_WRITE_BIT_KHR;
    }
    VkDependencyInfo di{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    di.memoryBarrierCount = 1;
    di.pMemoryBarriers = &b;
    vkCmdPipelineBarrier2(cmd, &di);
}

// ---- self-test --------------------------------------------------------------------------------------------------------

namespace {

constexpr std::uint32_t kSize = 64;
constexpr std::uint32_t kIndexA = 7, kIndexB = 9;  // instance custom indices

// the expected pixel (rt_query.comp's encoding); `ambiguous`: on a triangle edge (either result is valid)
std::uint32_t expected(std::uint32_t px, std::uint32_t py, bool& ambiguous) {
    const float x = (static_cast<float>(px) + 0.5f) / static_cast<float>(kSize) * 2.0f - 1.0f;
    const float y = (static_cast<float>(py) + 0.5f) / static_cast<float>(kSize) * 2.0f - 1.0f;
    const auto onEdge = [](float a, float b) { return std::abs(a - b) < 1e-3f; };
    ambiguous = onEdge(x + y, 0.0f) || onEdge(std::abs(x), 0.8f) || onEdge(std::abs(y), 0.8f) || onEdge(x, 0.0f);
    // instance A: identity, z = 0 (t = 10), the triangle (-0.8,-0.8) (0.8,-0.8) (-0.8,0.8), opaque
    if (x >= -0.8f && y >= -0.8f && x + y <= 0.0f) return (1u << 31) | (40u << 8) | (kIndexA + 1);
    // instance B: rotated 180 degrees about z, z = 1 (t = 11), non-opaque: the shader accepts barycentric u < 0.5, i.e.
    // object x = -x < 0
    if (x <= 0.8f && y <= 0.8f && x + y >= 0.0f && x > 0.0f) return (1u << 31) | (44u << 8) | (kIndexB + 1);
    return 0;
}

}  // namespace

SelfTestResult selfTest(Context& ctx) {
    SelfTestResult res;
    if (!ctx.features().rayTracing.enabled) {
        res.detail = "ray tracing is off: " + ctx.features().rayTracing.reason;
        return res;
    }
    res.ran = true;
    const auto t0 = std::chrono::steady_clock::now();
    const auto spirv = shader::embedded::spirv("test/rt_query.comp");
    if (spirv.empty()) {
        res.detail = "test/rt_query.comp is not embedded";
        return res;
    }

    AddressBuffer vertices, indices, instances, output, blasScratch, tlasScratch;
    AccelerationStructure blas, tlas;
    VkPipelineLayout layout = VK_NULL_HANDLE;
    VkPipeline pipeline = VK_NULL_HANDLE;
    VkCommandPool pool = VK_NULL_HANDLE;
    VkFence fence = VK_NULL_HANDLE;
    const auto cleanup = [&] {
        if (fence != VK_NULL_HANDLE) vkDestroyFence(ctx.device(), fence, nullptr);
        if (pool != VK_NULL_HANDLE) vkDestroyCommandPool(ctx.device(), pool, nullptr);
        if (pipeline != VK_NULL_HANDLE) vkDestroyPipeline(ctx.device(), pipeline, nullptr);
        if (layout != VK_NULL_HANDLE) vkDestroyPipelineLayout(ctx.device(), layout, nullptr);
        tlas.destroy(ctx);
        blas.destroy(ctx);
        for (AddressBuffer* b : {&vertices, &indices, &instances, &output, &blasScratch, &tlasScratch}) b->destroy(ctx);
    };
    const auto fail = [&](std::string why) {
        cleanup();
        res.detail = std::move(why);
        return res;
    };

    constexpr VkBufferUsageFlags kInput = VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR;
    const float verts[9] = {-0.8f, -0.8f, 0.0f, 0.8f, -0.8f, 0.0f, -0.8f, 0.8f, 0.0f};
    const std::uint32_t idx[3] = {0, 1, 2};
    if (!vertices.create(ctx, sizeof(verts), kInput | VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, true, 0, "rt test vertices") ||
        !indices.create(ctx, sizeof(idx), kInput | VK_BUFFER_USAGE_INDEX_BUFFER_BIT, true, 0, "rt test indices") ||
        !instances.create(ctx, 2 * sizeof(VkAccelerationStructureInstanceKHR), kInput, true, 16, "rt test instances") ||
        !output.create(ctx, kSize * kSize * 4, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, true, 0, "rt test output")) {
        return fail("buffer creation failed");
    }
    std::memcpy(vertices.mapped, verts, sizeof(verts));
    std::memcpy(indices.mapped, idx, sizeof(idx));
    std::memset(output.mapped, 0xCD, kSize * kSize * 4);

    // pipeline: push constants only (TLAS address, output address, size)
    struct Push {
        std::uint64_t tlas;
        std::uint64_t out;
        std::uint32_t width, height;
    };
    const VkPushConstantRange range{VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(Push)};
    VkPipelineLayoutCreateInfo lci{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    lci.pushConstantRangeCount = 1;
    lci.pPushConstantRanges = &range;
    if (vkCreatePipelineLayout(ctx.device(), &lci, nullptr, &layout) != VK_SUCCESS) return fail("pipeline layout");
    pipeline = createComputePipeline(ctx, layout, spirv, "rt self-test");
    if (pipeline == VK_NULL_HANDLE) return fail("the ray query compute pipeline failed to build");

    VkCommandPoolCreateInfo pci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    pci.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
    pci.queueFamilyIndex = ctx.queueFamily();
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    VkCommandBufferAllocateInfo cai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    VkFenceCreateInfo fci{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    if (vkCreateCommandPool(ctx.device(), &pci, nullptr, &pool) != VK_SUCCESS) return fail("command pool");
    cai.commandPool = pool;
    cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cai.commandBufferCount = 1;
    if (vkAllocateCommandBuffers(ctx.device(), &cai, &cmd) != VK_SUCCESS || vkCreateFence(ctx.device(), &fci, nullptr, &fence) != VK_SUCCESS) {
        return fail("command buffer / fence");
    }
    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(cmd, &bi);

    const Triangles tri{vertices.address, VK_FORMAT_R32G32B32_SFLOAT, 12, 2, indices.address, VK_INDEX_TYPE_UINT32, 1, true};
    if (!buildBottomLevel(ctx, cmd, {&tri, 1}, VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR, blas, blasScratch)) {
        vkEndCommandBuffer(cmd);
        return fail("BLAS build");
    }
    // two instances of the BLAS: A as it is (opaque), B rotated 180 degrees about z, 1 further away, forced non-opaque
    VkAccelerationStructureInstanceKHR inst[2]{};
    inst[0].transform = {{{1, 0, 0, 0}, {0, 1, 0, 0}, {0, 0, 1, 0}}};
    inst[0].instanceCustomIndex = kIndexA;
    inst[0].mask = 0xFF;
    inst[0].flags = VK_GEOMETRY_INSTANCE_TRIANGLE_FACING_CULL_DISABLE_BIT_KHR;
    inst[0].accelerationStructureReference = blas.address;
    inst[1] = inst[0];
    inst[1].transform = {{{-1, 0, 0, 0}, {0, -1, 0, 0}, {0, 0, 1, 1}}};
    inst[1].instanceCustomIndex = kIndexB;
    inst[1].flags = VK_GEOMETRY_INSTANCE_TRIANGLE_FACING_CULL_DISABLE_BIT_KHR | VK_GEOMETRY_INSTANCE_FORCE_NO_OPAQUE_BIT_KHR;
    std::memcpy(instances.mapped, inst, sizeof(inst));
    for (AddressBuffer* b : {&vertices, &indices, &instances, &output}) vmaFlushAllocation(ctx.allocator(), b->allocation, 0, VK_WHOLE_SIZE);

    buildBarrier(cmd, VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_BUILD_BIT_KHR);
    if (!buildTopLevel(ctx, cmd, instances.address, 2, VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR, tlas, tlasScratch)) {
        vkEndCommandBuffer(cmd);
        return fail("TLAS build");
    }
    buildBarrier(cmd, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT);
    const Push push{tlas.address, output.address, kSize, kSize};
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline);
    vkCmdPushConstants(cmd, layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(push), &push);
    vkCmdDispatch(cmd, kSize / 8, kSize / 8, 1);
    VkMemoryBarrier2 toHost{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
    toHost.srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
    toHost.srcAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
    toHost.dstStageMask = VK_PIPELINE_STAGE_2_HOST_BIT;
    toHost.dstAccessMask = VK_ACCESS_2_HOST_READ_BIT;
    VkDependencyInfo di{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    di.memoryBarrierCount = 1;
    di.pMemoryBarriers = &toHost;
    vkCmdPipelineBarrier2(cmd, &di);
    vkEndCommandBuffer(cmd);

    VkCommandBufferSubmitInfo cbi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO};
    cbi.commandBuffer = cmd;
    VkSubmitInfo2 si{VK_STRUCTURE_TYPE_SUBMIT_INFO_2};
    si.commandBufferInfoCount = 1;
    si.pCommandBufferInfos = &cbi;
    if (const VkResult r = ctx.queueSubmit(si, fence); r != VK_SUCCESS) return fail(std::format("submit: {}", resultString(r)));
    if (const VkResult r = vkWaitForFences(ctx.device(), 1, &fence, VK_TRUE, 10'000'000'000ull); r != VK_SUCCESS) {
        return fail(std::format("wait: {}", resultString(r)));
    }
    vmaInvalidateAllocation(ctx.allocator(), output.allocation, 0, VK_WHOLE_SIZE);

    const auto* px = static_cast<const std::uint32_t*>(output.mapped);
    int bad = 0, hitsA = 0, hitsB = 0, misses = 0, skipped = 0;
    std::string first;
    for (std::uint32_t y = 0; y < kSize; ++y) {
        for (std::uint32_t x = 0; x < kSize; ++x) {
            bool ambiguous = false;
            const std::uint32_t want = expected(x, y, ambiguous);
            const std::uint32_t got = px[y * kSize + x];
            if (ambiguous) {
                ++skipped;
                continue;
            }
            if (got != want) {
                if (bad++ == 0) first = std::format("pixel ({}, {}) = 0x{:08x}, expected 0x{:08x}", x, y, got, want);
                continue;
            }
            (got == 0 ? misses : (got & 0xFF) == kIndexA + 1 ? hitsA : hitsB)++;
        }
    }
    res.milliseconds = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    res.pass = bad == 0 && hitsA > 0 && hitsB > 0 && misses > 0;
    res.detail = std::format("{} pixels: {} opaque hits, {} alpha-tested hits, {} misses, {} on edges skipped, {} wrong{}{}; BLAS {} B, "
                             "TLAS {} B",
                             kSize * kSize, hitsA, hitsB, misses, skipped, bad, first.empty() ? "" : " (first: ", first.empty() ? "" : first + ")",
                             blas.storage.size, tlas.storage.size);
    cleanup();
    return res;
}

}  // namespace rvk::vk::rt
