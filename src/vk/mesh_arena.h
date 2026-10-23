#pragma once

// Suballocated device-local buffers for the engine's static vertex / index data (docs/research/performance.md 11): the
// meshes share a few large VkBuffers, so consecutive draws keep their vertex / index buffer bindings and the offset goes into
// the draw (vertexOffset / firstIndex) - the base of batched indirect draws. VMA virtual blocks over kBlockSize buffers; a
// request above kMaxSuballocation gets a dedicated buffer. Any thread (a mutex per call: allocations happen per mesh
// upload, not per draw).

#include <cstdint>
#include <mutex>
#include <vector>

#include "vk/vk.h"

namespace rvk::vk {

class Context;

class MeshArena {
public:
    static constexpr VkDeviceSize kBlockSize = 64ull << 20;
    static constexpr VkDeviceSize kMaxSuballocation = 8ull << 20;

    struct Alloc {
        VkBuffer buffer = VK_NULL_HANDLE;
        VkDeviceSize offset = 0;        // of the data in buffer (a multiple of the requested multiple)
        VkDeviceAddress address = 0;    // of the data (0 without device addresses)
        std::uint32_t block = UINT32_MAX;            // UINT32_MAX = dedicated
        VmaVirtualAllocation virt = VK_NULL_HANDLE;  // in the block
        VmaAllocation dedicated = VK_NULL_HANDLE;
        bool valid() const { return buffer != VK_NULL_HANDLE; }
    };

    bool create(Context& ctx, VkBufferUsageFlags usage);
    void destroy();

    // `size` bytes whose offset is a multiple of `multiple` (any value >= 1, e.g. a vertex stride; always 4-aligned too)
    Alloc allocate(VkDeviceSize size, std::uint32_t multiple);
    void free(const Alloc& a);

    struct Stats {
        std::uint32_t blocks = 0, dedicated = 0;
        VkDeviceSize used = 0;  // bytes handed out (incl. alignment padding)
    };
    Stats stats() const;

private:
    struct Block {
        VkBuffer buffer = VK_NULL_HANDLE;
        VmaAllocation allocation = VK_NULL_HANDLE;
        VmaVirtualBlock virt = VK_NULL_HANDLE;
        VkDeviceAddress address = 0;
    };
    bool newBlock();
    VkDeviceAddress addressOf(VkBuffer b) const;

    Context* ctx_ = nullptr;
    VkBufferUsageFlags usage_ = 0;
    mutable std::mutex mutex_;
    std::vector<Block> blocks_;
    Stats stats_;
};

// the smallest multiple of both a and b (a, b >= 1)
std::uint32_t lcm(std::uint32_t a, std::uint32_t b);

}  // namespace rvk::vk
