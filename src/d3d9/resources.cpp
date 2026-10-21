// Textures, surfaces, buffers and vertex declarations of the D3D9 device model (d3d9/device.h).

#include <algorithm>
#include <bit>
#include <cstring>

#include <spdlog/spdlog.h>

#include "d3d9/device.h"

namespace rvk::d3d9 {

// ---- Texture ---------------------------------------------------------------------------------------------------------

Texture::Texture(Device& device, const TextureDesc& desc) : dev_(device), desc_(desc) {
    if (desc_.type != TextureType::Volume) {
        desc_.depth = 1;
    }
    const std::uint32_t maxDim = std::max({desc_.width, desc_.height, desc_.depth, 1u});
    const std::uint32_t fullChain = static_cast<std::uint32_t>(std::bit_width(maxDim));
    if (desc_.levels == 0 || desc_.levels > fullChain) {
        desc_.levels = fullChain;
    }
    if (desc_.format == vk::d3dfmt::NULLFMT) {
        isNull_ = true;  // depth-only passes render to a NULL colour target: no memory
        valid_ = true;
        return;
    }
    const auto fmt = vk::formatInfo(desc_.format, dev_.d24s8Format());
    if (!fmt) {
        spdlog::warn("d3d9: texture format {} (0x{:x}) is not supported", vk::d3dFormatName(desc_.format), desc_.format);
        return;
    }
    fmt_ = *fmt;
    kind_ = desc_.type == TextureType::Cube ? vk::TextureKind::Cube : desc_.type == TextureType::Volume ? vk::TextureKind::Volume
                                                                                                       : vk::TextureKind::Tex2D;
    vk::ImageDesc id;
    id.type = desc_.type == TextureType::Volume ? VK_IMAGE_TYPE_3D : VK_IMAGE_TYPE_2D;
    id.format = fmt_.format;
    id.extent = {desc_.width, desc_.height, desc_.depth};
    id.mips = desc_.levels;
    id.layers = faces();
    id.cube = desc_.type == TextureType::Cube;
    id.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    if (fmt_.depth) {
        id.usage |= VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
    } else if (isRenderTarget()) {
        id.usage |= VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    }
    id.swizzle = fmt_.swizzle;
    id.name = isRenderTarget() || fmt_.depth ? "d3d9 render target" : "d3d9 texture";
    if (!image_.create(dev_.ctx(), id)) {
        return;
    }
    slot_ = dev_.bindless().addTexture(kind_, image_.view());
    valid_ = true;
}

Texture::~Texture() {
    surfaces_.clear();
    if (slot_ != 0) {
        dev_.bindless().releaseTexture(kind_, slot_);
    }
    if (image_.valid()) {
        // the GPU may still use it in frames in flight
        auto* ctx = &dev_.ctx();
        auto img = std::make_shared<vk::Image>(std::move(image_));
        dev_.frames().retire([ctx, img] { img->destroy(*ctx); });
    }
}

Texture::Level& Texture::cpuLevel(std::uint32_t level, std::uint32_t face) {
    Level& l = cpu_[face * desc_.levels + level];
    if (l.data.empty()) {
        const vk::Pitch p = vk::d3dPitch(fmt_, levelWidth(level), levelHeight(level));
        l.rowPitch = p.rowBytes;
        l.slicePitch = p.rowBytes * p.rows;
        l.data.assign(static_cast<std::size_t>(l.slicePitch) * levelDepth(level), 0);
    }
    return l;
}

bool Texture::lock(std::uint32_t level, std::uint32_t face, const std::int32_t* rect, std::uint32_t flags, Locked& out) {
    if (!valid_ || isNull_ || level >= desc_.levels || face >= faces()) {
        return false;
    }
    if (isRenderTarget() || fmt_.depth) {
        spdlog::warn("d3d9: LockRect on a render target / depth texture is not supported");
        return false;
    }
    const std::lock_guard lock(mutex_);
    if (cpu_.empty()) {
        cpu_.resize(static_cast<std::size_t>(faces()) * desc_.levels);
    }
    Level& l = cpuLevel(level, face);
    std::size_t offset = 0;
    if (rect != nullptr) {
        const std::uint32_t left = static_cast<std::uint32_t>(std::max(0, rect[0]));
        const std::uint32_t top = static_cast<std::uint32_t>(std::max(0, rect[1]));
        if (fmt_.compressed) {
            offset = static_cast<std::size_t>(top / 4) * l.rowPitch + static_cast<std::size_t>(left / 4) * fmt_.blockBytes;
        } else {
            offset = static_cast<std::size_t>(top) * l.rowPitch + static_cast<std::size_t>(left) * fmt_.srcBlockBytes;
        }
    }
    out.bits = l.data.data() + offset;
    out.rowPitch = l.rowPitch;
    out.slicePitch = l.slicePitch;
    if ((flags & LOCK_READONLY) == 0) {
        l.dirty = true;  // uploaded on the next use after unlock
    }
    return true;
}

void Texture::unlock(std::uint32_t level, std::uint32_t face) {
    if (level < desc_.levels && face < faces() && !cpu_.empty() && cpu_[face * desc_.levels + level].dirty) {
        pending_ = true;
    }
}

void Texture::setLevelData(std::uint32_t level, std::uint32_t face, std::span<const std::uint8_t> data, std::uint32_t rowPitch) {
    if (!valid_ || isNull_ || level >= desc_.levels || face >= faces()) {
        return;
    }
    const std::lock_guard lock(mutex_);
    if (cpu_.empty()) {
        cpu_.resize(static_cast<std::size_t>(faces()) * desc_.levels);
    }
    Level& l = cpuLevel(level, face);
    const vk::Pitch p = vk::d3dPitch(fmt_, levelWidth(level), levelHeight(level));
    const std::uint32_t rows = p.rows * levelDepth(level);
    for (std::uint32_t r = 0; r < rows; ++r) {
        const std::size_t src = static_cast<std::size_t>(r) * rowPitch;
        if (src + p.rowBytes > data.size()) {
            break;
        }
        std::memcpy(l.data.data() + static_cast<std::size_t>(r) * l.rowPitch, data.data() + src, p.rowBytes);
    }
    l.dirty = true;
    pending_ = true;
}

Surface* Texture::surface(std::uint32_t level, std::uint32_t face) {
    if (level >= desc_.levels || face >= faces()) {
        return nullptr;
    }
    const std::lock_guard lock(mutex_);
    for (auto& s : surfaces_) {
        if (s->level() == level && s->face() == face) {
            return s.get();
        }
    }
    surfaces_.push_back(std::make_unique<Surface>(this, level, face, false));
    return surfaces_.back().get();
}

void Texture::upload(VkCommandBuffer cmd) {
    const std::lock_guard lock(mutex_);
    pending_ = false;
    bool any = false;
    lastUseFrame_ = dev_.frameValue();
    for (std::uint32_t face = 0; face < faces() && !cpu_.empty(); ++face) {
        for (std::uint32_t level = 0; level < desc_.levels; ++level) {
            Level& l = cpu_[face * desc_.levels + level];
            if (!l.dirty) {
                continue;
            }
            if (!any) {
                image_.transition(cmd, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
                any = true;
            }
            const std::uint32_t w = levelWidth(level), h = levelHeight(level), d = levelDepth(level);
            const std::uint64_t sliceBytes = vk::storageSize(fmt_, w, h);
            vk::RingAlloc staging = dev_.frames().allocate(sliceBytes * d, 16);
            if (staging.ptr == nullptr) {
                continue;
            }
            for (std::uint32_t z = 0; z < d; ++z) {
                vk::convertLevel(fmt_, l.data.data() + static_cast<std::size_t>(z) * l.slicePitch, l.rowPitch,
                                 static_cast<std::uint8_t*>(staging.ptr) + z * sliceBytes, w, h);
            }
            VkBufferImageCopy region{};
            region.bufferOffset = staging.offset;
            region.imageSubresource = {image_.aspect() & ~VkImageAspectFlags(VK_IMAGE_ASPECT_STENCIL_BIT), level,
                                       desc_.type == TextureType::Cube ? face : 0u, 1};
            region.imageExtent = {w, h, d};
            vkCmdCopyBufferToImage(cmd, staging.buffer, image_.image(), VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
            l.dirty = false;
            // file textures are never read back by the engine: drop the CPU copy (D3D9's managed pool would keep it)
            if (desc_.pool != POOL_SYSTEMMEM && (desc_.usage & USAGE_DYNAMIC) == 0) {
                std::vector<std::uint8_t>().swap(l.data);
            }
        }
    }
}

bool Texture::prepareForSampling(VkCommandBuffer cmd) {
    if (!valid_ || isNull_) {
        return false;
    }
    if (pending_) {
        upload(cmd);
    }
    image_.transition(cmd, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    return true;
}

// ---- Surface ---------------------------------------------------------------------------------------------------------

Surface::Surface(Texture* texture, std::uint32_t level, std::uint32_t face, bool ownsTexture)
    : texture_(texture), level_(level), face_(face), owns_(ownsTexture) {}

Surface::~Surface() {
    if (owns_) {
        delete texture_;
    }
}

// ---- Buffer ----------------------------------------------------------------------------------------------------------

Buffer::Buffer(Device& device, std::uint32_t size, std::uint32_t usage, std::uint32_t pool, bool index, bool index32)
    : dev_(device), size_(size), usage_(usage), pool_(pool), index_(index), index32_(index32), cpu_(size, 0) {}

Buffer::~Buffer() {
    if (static_ != VK_NULL_HANDLE) {
        auto* ctx = &dev_.ctx();
        VkBuffer b = static_;
        VmaAllocation a = staticAlloc_;
        dev_.frames().retire([ctx, b, a] { vmaDestroyBuffer(ctx->allocator(), b, a); });
    }
}

void* Buffer::lock(std::uint32_t offset, std::uint32_t size, std::uint32_t flags) {
    if (offset >= size_ && size_ != 0) {
        return nullptr;
    }
    const std::lock_guard lock(mutex_);
    if ((flags & LOCK_READONLY) == 0) {
        dirty_ = true;
        if ((flags & LOCK_NOOVERWRITE) == 0) {
            gpuFrame_ = 0;  // DISCARD (or a plain lock): draws recorded before keep the old GPU copy
        }
    }
    (void)size;
    return cpu_.data() + offset;
}

void Buffer::unlock() {}

Buffer::Binding Buffer::gpu() {
    const std::lock_guard lock(mutex_);
    const std::uint64_t frame = dev_.frameValue();
    if (dynamic() || pool_ == POOL_SYSTEMMEM) {
        // ring copy per frame; NOOVERWRITE appends go into the same copy (the CPU writes land before the GPU runs)
        if (gpuFrame_ != frame) {
            vk::RingAlloc a = dev_.frames().allocate(size_, 16);
            if (a.ptr == nullptr) {
                return {};
            }
            std::memcpy(a.ptr, cpu_.data(), size_);
            binding_ = {a.buffer, a.offset};
            boundPtr_ = static_cast<std::uint8_t*>(a.ptr);
            gpuFrame_ = frame;
            dirty_ = false;
        } else if (dirty_) {
            std::memcpy(boundPtr_, cpu_.data(), size_);
            dirty_ = false;
        }
        return binding_;
    }
    if (dirty_ || static_ == VK_NULL_HANDLE) {
        // static buffer: a fresh GPU buffer per update, so draws recorded earlier keep the old contents
        VkBufferCreateInfo bci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
        bci.size = std::max<std::uint32_t>(size_, 4);
        bci.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT | (index_ ? VK_BUFFER_USAGE_INDEX_BUFFER_BIT : VK_BUFFER_USAGE_VERTEX_BUFFER_BIT);
        VmaAllocationCreateInfo aci{};
        aci.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
        VkBuffer nb = VK_NULL_HANDLE;
        VmaAllocation na = VK_NULL_HANDLE;
        if (vmaCreateBuffer(dev_.ctx().allocator(), &bci, &aci, &nb, &na, nullptr) != VK_SUCCESS) {
            return {};
        }
        vk::RingAlloc staging = dev_.frames().allocate(size_, 16);
        if (staging.ptr == nullptr) {
            vmaDestroyBuffer(dev_.ctx().allocator(), nb, na);
            return {};
        }
        std::memcpy(staging.ptr, cpu_.data(), size_);
        VkCommandBuffer cmd = dev_.uploadCommands();
        const VkBufferCopy copy{staging.offset, 0, size_};
        vkCmdCopyBuffer(cmd, staging.buffer, nb, 1, &copy);
        if (static_ != VK_NULL_HANDLE) {
            auto* ctx = &dev_.ctx();
            VkBuffer ob = static_;
            VmaAllocation oa = staticAlloc_;
            dev_.frames().retire([ctx, ob, oa] { vmaDestroyBuffer(ctx->allocator(), ob, oa); });
        }
        static_ = nb;
        staticAlloc_ = na;
        binding_ = {nb, 0};
        dirty_ = false;
    }
    return binding_;
}

// ---- VertexDecl ------------------------------------------------------------------------------------------------------

VertexDecl::VertexDecl(std::span<const VertexElement> elements) {
    for (const auto& e : elements) {
        if (e.stream == kDeclEndStream) {
            break;
        }
        elements_.push_back(e);
    }
}

}  // namespace rvk::d3d9
