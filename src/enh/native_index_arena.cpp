#include "enh/native_index_arena.h"

#include <algorithm>
#include <cstring>
#include <string>

#include <spdlog/spdlog.h>

#include "enh/native_parts.h"

namespace rvk::enh {

NativeIndexArena::NativeIndexArena(d3d9::Device& device) : dev_(device) {}

NativeIndexArena::~NativeIndexArena() { destroyBuffer(); }

bool NativeIndexArena::create() {
    vk::Context& ctx = dev_.ctx();
    VkBufferCreateInfo bci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bci.size = static_cast<VkDeviceSize>(kCapacity) * 4;
    bci.usage = VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    VmaAllocationCreateInfo aci{};
    aci.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
    if (vmaCreateBuffer(ctx.allocator(), &bci, &aci, &buffer_, &allocation_, nullptr) != VK_SUCCESS) {
        buffer_ = VK_NULL_HANDLE;
        allocation_ = VK_NULL_HANDLE;
        spdlog::error("enh: native index arena: buffer creation failed");
        return false;
    }
    ctx.setName(buffer_, VK_OBJECT_TYPE_BUFFER, "native index arena");
    used_ = 0;
    stagedFirst_ = 0;
    staged_.clear();
    cache_.clear();
    return true;
}

void NativeIndexArena::destroyBuffer() {
    if (buffer_ == VK_NULL_HANDLE) return;
    vk::Context* ctx = &dev_.ctx();
    const VkBuffer b = buffer_;
    const VmaAllocation a = allocation_;
    buffer_ = VK_NULL_HANDLE;
    allocation_ = VK_NULL_HANDLE;
    dev_.frames().retire([ctx, b, a] { vmaDestroyBuffer(ctx->allocator(), b, a); });
}

void NativeIndexArena::beginFrame() {
    if (buffer_ == VK_NULL_HANDLE) {
        create();
        return;
    }
    if (resetPending_) {
        resetPending_ = false;
        ++stats_.resets;
        spdlog::info("enh: native index arena: full ({} indices), replaced", used_);
        destroyBuffer();
        create();
    }
}

std::uint32_t NativeIndexArena::get(const NativePart& p) {
    if (buffer_ == VK_NULL_HANDLE || p.indexBuffer == nullptr || p.indexCount == 0) return kNone;
    const Key key{p.ibId, p.ibGen, p.first, p.indexCount, p.index32 != 0 ? 1u : 0u};
    if (const auto it = cache_.find(key); it != cache_.end()) return it->second;
    if (used_ + p.indexCount > kCapacity) {
        resetPending_ = true;
        ++stats_.full;
        return kNone;
    }
    const std::uint32_t isz = p.index32 != 0 ? 4u : 2u;
    raw_.resize(static_cast<std::size_t>(p.indexCount) * isz);
    if (!p.indexBuffer->readCpu(p.first * isz, p.indexCount * isz, raw_.data())) {
        ++stats_.unreadable;
        return kNone;
    }
    // sanity of the engine's data (diagnostic): every index inside the vertex buffer, few degenerate triangles; a half-written buffer shows as zeros
    {
        std::uint32_t maxIndex = 0, degenerate = 0, zeros = 0;
        const std::uint32_t tris = p.indexCount / 3;
        for (std::uint32_t t = 0; t < tris; ++t) {
            std::uint32_t v[3];
            for (int k = 0; k < 3; ++k) {
                const std::size_t i = static_cast<std::size_t>(t) * 3 + k;
                v[k] = isz == 4 ? reinterpret_cast<const std::uint32_t*>(raw_.data())[i] : reinterpret_cast<const std::uint16_t*>(raw_.data())[i];
                maxIndex = std::max(maxIndex, v[k]);
            }
            if (v[0] == v[1] || v[1] == v[2] || v[0] == v[2]) ++degenerate;
            if (v[0] == 0 && v[1] == 0 && v[2] == 0) ++zeros;
        }
        const std::uint32_t vcount = p.stride != 0 && p.vertexBuffer != nullptr ? p.vertexBuffer->size() / p.stride : 0;
        if ((vcount != 0 && maxIndex >= vcount) || zeros * 4 > tris + 3 || degenerate * 2 > tris + 1) {
            ++stats_.suspect;
            static int logged = 0;
            if (logged++ < 30) {
                spdlog::info("enh: native index arena: suspect indices: part with {} indices (first {}), max index {} of {} vertices, {} of {} triangles degenerate ({} all zero), material '{}'", p.indexCount,
                             p.first, maxIndex, vcount, degenerate, tris, zeros, p.name != nullptr ? *p.name : std::string("?"));
            }
        }
    }
    const std::uint32_t at = used_;
    if (staged_.empty()) stagedFirst_ = at;
    const std::size_t base = staged_.size();
    staged_.resize(base + p.indexCount);
    if (isz == 4) {
        std::memcpy(staged_.data() + base, raw_.data(), raw_.size());
    } else {
        const auto* src = reinterpret_cast<const std::uint16_t*>(raw_.data());
        for (std::uint32_t i = 0; i < p.indexCount; ++i) staged_[base + i] = src[i];
    }
    used_ += p.indexCount;
    ++stats_.parts;
    stats_.indices += p.indexCount;
    cache_.emplace(key, at);
    return at;
}

void NativeIndexArena::flush(VkCommandBuffer cmd) {
    if (buffer_ == VK_NULL_HANDLE || staged_.empty()) return;
    const VkDeviceSize bytes = static_cast<VkDeviceSize>(staged_.size()) * 4;
    const vk::RingAlloc a = dev_.frames().allocate(bytes, 16);
    if (a.ptr == nullptr) {  // the indices would be lost: forget everything (the next frame stages its parts again)
        cache_.clear();
        staged_.clear();
        used_ = 0;
        return;
    }
    std::memcpy(a.ptr, staged_.data(), static_cast<std::size_t>(bytes));
    const VkBufferCopy region{a.offset, static_cast<VkDeviceSize>(stagedFirst_) * 4, bytes};
    vkCmdCopyBuffer(cmd, a.buffer, buffer_, 1, &region);
    VkMemoryBarrier2 mb{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
    mb.srcStageMask = VK_PIPELINE_STAGE_2_COPY_BIT;
    mb.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
    mb.dstStageMask = VK_PIPELINE_STAGE_2_INDEX_INPUT_BIT;
    mb.dstAccessMask = VK_ACCESS_2_INDEX_READ_BIT;
    VkDependencyInfo dep{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    dep.memoryBarrierCount = 1;
    dep.pMemoryBarriers = &mb;
    vkCmdPipelineBarrier2(cmd, &dep);
    staged_.clear();
}

}  // namespace rvk::enh
