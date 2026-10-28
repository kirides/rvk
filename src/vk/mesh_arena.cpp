#include "vk/mesh_arena.h"

#include <atomic>
#include <format>
#include <numeric>
#include <string>

#include <spdlog/spdlog.h>

#include "vk/context.h"

namespace rvk::vk {

std::uint32_t lcm(std::uint32_t a, std::uint32_t b) {
    a = a == 0 ? 1 : a;
    b = b == 0 ? 1 : b;
    return a / std::gcd(a, b) * b;
}

bool MeshArena::create(Context& ctx, VkBufferUsageFlags usage) {
    ctx_ = &ctx;
    usage_ = usage;
    return true;  // blocks are created on demand
}

void MeshArena::destroy() {
    const std::lock_guard lock(mutex_);
    for (Block& b : blocks_) {
        if (b.virt != VK_NULL_HANDLE) {
            vmaClearVirtualBlock(b.virt);
            vmaDestroyVirtualBlock(b.virt);
        }
        if (b.buffer != VK_NULL_HANDLE) vmaDestroyBuffer(ctx_->allocator(), b.buffer, b.allocation);
    }
    blocks_.clear();
    stats_ = {};
}

VkDeviceAddress MeshArena::addressOf(VkBuffer b) const {
    if ((usage_ & VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT) == 0) return 0;
    VkBufferDeviceAddressInfo ai{VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO};
    ai.buffer = b;
    return vkGetBufferDeviceAddress(ctx_->device(), &ai);
}

bool MeshArena::newBlock() {
    Block b;
    VkBufferCreateInfo bci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bci.size = kBlockSize;
    bci.usage = usage_;
    VmaAllocationCreateInfo aci{};
    aci.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
    if (vmaCreateBuffer(ctx_->allocator(), &bci, &aci, &b.buffer, &b.allocation, nullptr) != VK_SUCCESS) return false;
    VmaVirtualBlockCreateInfo vci{};
    vci.size = kBlockSize;
    if (vmaCreateVirtualBlock(&vci, &b.virt) != VK_SUCCESS) {
        vmaDestroyBuffer(ctx_->allocator(), b.buffer, b.allocation);
        return false;
    }
    b.address = addressOf(b.buffer);
    names::object(*ctx_, b.buffer, VK_OBJECT_TYPE_BUFFER, "mesh arena block " + std::to_string(blocks_.size() + 1));
    blocks_.push_back(b);
    ++stats_.blocks;
    spdlog::info("vulkan: mesh arena: block {} ({} MiB)", blocks_.size(), kBlockSize >> 20);
    return true;
}

MeshArena::Alloc MeshArena::allocate(VkDeviceSize size, std::uint32_t multiple) {
    Alloc a;
    size = std::max<VkDeviceSize>(size, 4);
    const std::uint32_t m = lcm(multiple, 4);
    const std::lock_guard lock(mutex_);
    if (size + m <= kMaxSuballocation) {
        VmaVirtualAllocationCreateInfo vai{};
        vai.size = size + m - 4;  // room to move the start to a multiple of m (the virtual allocation is 4-aligned)
        vai.alignment = 4;
        for (std::uint32_t pass = 0; pass < 2; ++pass) {
            for (std::uint32_t i = 0; i < blocks_.size(); ++i) {
                VkDeviceSize off = 0;
                if (vmaVirtualAllocate(blocks_[i].virt, &vai, &a.virt, &off) != VK_SUCCESS) continue;
                a.block = i;
                a.buffer = blocks_[i].buffer;
                a.offset = (off + m - 1) / m * m;
                a.address = blocks_[i].address != 0 ? blocks_[i].address + a.offset : 0;
                stats_.used += vai.size;
                return a;
            }
            if (pass == 0 && !newBlock()) break;
        }
        a = {};
    }
    // a dedicated buffer (large meshes, or no block could be created)
    VkBufferCreateInfo bci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bci.size = size;
    bci.usage = usage_;
    VmaAllocationCreateInfo aci{};
    aci.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
    if (vmaCreateBuffer(ctx_->allocator(), &bci, &aci, &a.buffer, &a.dedicated, nullptr) != VK_SUCCESS) return {};
    a.address = addressOf(a.buffer);
    if (ctx_->namesEnabled()) {
        static std::atomic<std::uint32_t> dedicated{0};  // numbered by creation, never reused
        names::object(*ctx_, a.buffer, VK_OBJECT_TYPE_BUFFER, std::format("mesh arena dedicated buffer {} ({} KiB)", dedicated++, size >> 10));
    }
    ++stats_.dedicated;
    stats_.used += size;
    return a;
}

void MeshArena::free(const Alloc& a) {
    if (!a.valid()) return;
    const std::lock_guard lock(mutex_);
    if (a.block == UINT32_MAX) {
        VmaAllocationInfo info{};
        vmaGetAllocationInfo(ctx_->allocator(), a.dedicated, &info);
        stats_.used -= std::min(stats_.used, info.size);
        --stats_.dedicated;
        vmaDestroyBuffer(ctx_->allocator(), a.buffer, a.dedicated);
        return;
    }
    if (a.block < blocks_.size()) {
        VmaVirtualAllocationInfo info{};
        vmaGetVirtualAllocationInfo(blocks_[a.block].virt, a.virt, &info);
        stats_.used -= std::min(stats_.used, info.size);
        vmaVirtualFree(blocks_[a.block].virt, a.virt);
    }
}

MeshArena::Stats MeshArena::stats() const {
    const std::lock_guard lock(mutex_);
    return stats_;
}

}  // namespace rvk::vk
