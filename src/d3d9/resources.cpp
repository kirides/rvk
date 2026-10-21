// Textures, surfaces, buffers and vertex declarations of the D3D9 device model (d3d9/device.h).

#include <algorithm>
#include <atomic>
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
    for (Level& l : cpu_) {
        freeLevel(l);
    }
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

namespace {
std::atomic<std::uint64_t> g_textureCpuBytes{0};
}

std::uint64_t Texture::cpuBytesTotal() { return g_textureCpuBytes.load(std::memory_order_relaxed); }

Texture::Level& Texture::cpuLevel(std::uint32_t level, std::uint32_t face) {
    Level& l = cpu_[face * desc_.levels + level];
    if (l.data.empty()) {
        const vk::Pitch p = vk::d3dPitch(fmt_, levelWidth(level), levelHeight(level));
        l.rowPitch = p.rowBytes;
        l.slicePitch = p.rowBytes * p.rows;
        l.data.assign(static_cast<std::size_t>(l.slicePitch) * levelDepth(level), 0);
        g_textureCpuBytes.fetch_add(l.data.size(), std::memory_order_relaxed);
    }
    return l;
}

void Texture::freeLevel(Level& l) {
    g_textureCpuBytes.fetch_sub(l.data.size(), std::memory_order_relaxed);
    std::vector<std::uint8_t>().swap(l.data);
}

void Texture::markDirty(Level& l, std::uint32_t level, const std::int32_t* rect) {
    const std::uint32_t w = levelWidth(level), h = levelHeight(level), d = levelDepth(level);
    std::uint32_t x0 = 0, y0 = 0, x1 = w, y1 = h;
    if (rect != nullptr) {
        const auto clampTo = [](std::int32_t v, std::uint32_t hi) {
            return static_cast<std::uint32_t>(std::clamp<std::int64_t>(v, 0, static_cast<std::int64_t>(hi)));
        };
        const std::uint32_t rl = clampTo(rect[0], w), rt = clampTo(rect[1], h), rr = clampTo(rect[2], w), rb = clampTo(rect[3], h);
        if (rl < rr && rt < rb) {  // an empty / inverted rect counts as the whole level
            x0 = rl;
            y0 = rt;
            x1 = rr;
            y1 = rb;
            if (fmt_.compressed) {  // whole 4x4 blocks (the copy extent may end at the level edge)
                x0 &= ~3u;
                y0 &= ~3u;
                x1 = std::min((x1 + 3) & ~3u, w);
                y1 = std::min((y1 + 3) & ~3u, h);
            }
        }
    }
    if (!l.dirty) {
        l.box[0] = x0;
        l.box[1] = y0;
        l.box[3] = x1;
        l.box[4] = y1;
    } else {
        l.box[0] = std::min(l.box[0], x0);
        l.box[1] = std::min(l.box[1], y0);
        l.box[3] = std::max(l.box[3], x1);
        l.box[4] = std::max(l.box[4], y1);
    }
    l.box[2] = 0;  // LockBox has no box: always all slices
    l.box[5] = d;
    l.dirty = true;  // uploaded on the next use after unlock
}

bool Texture::restoreLevel(std::uint32_t level, std::uint32_t face) {
    const std::uint32_t w = levelWidth(level), h = levelHeight(level), d = levelDepth(level);
    const char* why = nullptr;
    if (!dev_.recordingThread()) {
        why = "not on the render thread";
    } else if (fmt_.convert != vk::Convert::None) {
        why = "converted format";
    }
    std::vector<std::uint8_t> bytes;
    if (why == nullptr) {
        const VkDeviceSize size = vk::storageSize(fmt_, w, h) * d;
        if (!dev_.readImageLevel(image_, level, desc_.type == TextureType::Cube ? face : 0u, {w, h, d}, size, bytes)) {
            why = "read-back failed";
        }
    }
    const std::lock_guard lock(mutex_);
    if (why != nullptr) {
        if (!restoreWarned_) {
            restoreWarned_ = true;
            spdlog::warn("d3d9: lock of a {}x{} {} texture (pool {}) whose CPU copy was dropped: {}; its texels read as 0 (only the "
                         "locked region is uploaded)",
                         desc_.width, desc_.height, vk::d3dFormatName(desc_.format), desc_.pool, why);
        }
        return false;
    }
    Level& l = cpuLevel(level, face);
    std::memcpy(l.data.data(), bytes.data(), std::min(l.data.size(), bytes.size()));  // Convert::None: storage = D3D9 layout
    l.dropped = false;
    spdlog::debug("d3d9: restored level {} face {} of a {}x{} {} texture from the GPU", level, face, desc_.width, desc_.height,
                  vk::d3dFormatName(desc_.format));
    return true;
}

bool Texture::lock(std::uint32_t level, std::uint32_t face, const std::int32_t* rect, std::uint32_t flags, Locked& out) {
    if (!valid_ || isNull_ || level >= desc_.levels || face >= faces()) {
        return false;
    }
    if (isRenderTarget() || fmt_.depth) {
        spdlog::warn("d3d9: LockRect on a render target / depth texture is not supported");
        return false;
    }
    std::unique_lock lock(mutex_);
    if (cpu_.empty()) {
        cpu_.resize(static_cast<std::size_t>(faces()) * desc_.levels);
    }
    lockedOnce_ = true;
    if (cpu_[face * desc_.levels + level].dropped && (flags & LOCK_DISCARD) == 0) {
        lock.unlock();
        restoreLevel(level, face);
        lock.lock();
    }
    Level& l = cpuLevel(level, face);
    l.dropped = false;
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
        markDirty(l, level, rect);
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
    l.dropped = false;
    markDirty(l, level, nullptr);
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
            // only the dirty region: the rest of the image keeps what earlier uploads put there (the first upload is whole:
            // the image starts undefined, the CPU copy zeroed)
            if (!l.uploaded) {
                l.box[0] = l.box[1] = l.box[2] = 0;
                l.box[3] = levelWidth(level);
                l.box[4] = levelHeight(level);
                l.box[5] = levelDepth(level);
                l.uploaded = true;
            }
            const std::uint32_t x0 = l.box[0], y0 = l.box[1], z0 = l.box[2];
            const std::uint32_t w = l.box[3] - x0, h = l.box[4] - y0, d = l.box[5] - z0;
            const std::uint64_t sliceBytes = vk::storageSize(fmt_, w, h);
            vk::RingAlloc staging = dev_.frames().allocate(sliceBytes * d, 16);
            if (staging.ptr == nullptr) {
                continue;
            }
            const std::size_t srcOffset =
                fmt_.compressed ? static_cast<std::size_t>(y0 / 4) * l.rowPitch + static_cast<std::size_t>(x0 / 4) * fmt_.blockBytes
                                : static_cast<std::size_t>(y0) * l.rowPitch + static_cast<std::size_t>(x0) * fmt_.srcBlockBytes;
            for (std::uint32_t z = 0; z < d; ++z) {
                vk::convertLevel(fmt_, l.data.data() + static_cast<std::size_t>(z0 + z) * l.slicePitch + srcOffset, l.rowPitch,
                                 static_cast<std::uint8_t*>(staging.ptr) + z * sliceBytes, w, h);
            }
            VkBufferImageCopy region{};
            region.bufferOffset = staging.offset;
            region.imageSubresource = {image_.aspect() & ~VkImageAspectFlags(VK_IMAGE_ASPECT_STENCIL_BIT), level,
                                       desc_.type == TextureType::Cube ? face : 0u, 1};
            region.imageOffset = {static_cast<std::int32_t>(x0), static_cast<std::int32_t>(y0), static_cast<std::int32_t>(z0)};
            region.imageExtent = {w, h, d};
            vkCmdCopyBufferToImage(cmd, staging.buffer, image_.image(), VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
            l.dirty = false;
            // File textures (never locked) are not read back by the engine: drop the CPU copy. A texture that was locked keeps
            // it like D3D9's managed pool: partial locks (the engine's glyph atlases) must see the earlier contents.
            if (!lockedOnce_ && desc_.pool != POOL_SYSTEMMEM && (desc_.usage & USAGE_DYNAMIC) == 0) {
                freeLevel(l);
                l.dropped = true;
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
        // the written range (size 0 = up to the end)
        const std::uint32_t lo = std::min(offset, size_);
        const std::uint32_t hi = size == 0 ? size_ : static_cast<std::uint32_t>(std::min<std::uint64_t>(std::uint64_t{offset} + size, size_));
        if ((flags & LOCK_DISCARD) != 0) {
            // the old contents are undefined from here on: a fresh GPU copy needs only what is written after the DISCARD
            gpuFrame_ = 0;  // draws recorded before keep the old GPU copy
            discarded_ = true;
            dirtyLo_ = lo;
            dirtyHi_ = hi;
        } else {
            if ((flags & LOCK_NOOVERWRITE) == 0) {
                gpuFrame_ = 0;  // a plain lock: new GPU copy (of the whole buffer), draws recorded before keep the old one
                discarded_ = false;
            }
            dirtyLo_ = dirty_ ? std::min(dirtyLo_, lo) : lo;
            dirtyHi_ = dirty_ ? std::max(dirtyHi_, hi) : hi;
        }
        dirty_ = true;
    }
    return cpu_.data() + offset;
}

void Buffer::unlock() {}

Buffer::Binding Buffer::gpu() {
    const std::lock_guard lock(mutex_);
    const std::uint64_t frame = dev_.frameValue();
    if (dynamic() || pool_ == POOL_SYSTEMMEM) {
        // ring copy per frame; NOOVERWRITE appends go into the same copy (the CPU writes land before the GPU runs). Only the
        // bytes written since the last copy are copied (the sprite batcher locks one quad of a 448 KiB buffer per Lock).
        if (gpuFrame_ != frame) {
            vk::RingAlloc a = dev_.frames().allocate(size_, 16);
            if (a.ptr == nullptr) {
                return {};
            }
            // after a DISCARD only the range written since is defined; otherwise the copy needs everything (a new frame)
            const std::uint32_t lo = discarded_ ? dirtyLo_ : 0;
            const std::uint32_t hi = discarded_ ? dirtyHi_ : size_;
            if (hi > lo) {
                std::memcpy(static_cast<std::uint8_t*>(a.ptr) + lo, cpu_.data() + lo, hi - lo);
            }
            binding_ = {a.buffer, a.offset};
            boundPtr_ = static_cast<std::uint8_t*>(a.ptr);
            gpuFrame_ = frame;
        } else if (dirty_ && dirtyHi_ > dirtyLo_) {
            std::memcpy(boundPtr_ + dirtyLo_, cpu_.data() + dirtyLo_, dirtyHi_ - dirtyLo_);
        }
        dirty_ = false;
        discarded_ = false;
        dirtyLo_ = dirtyHi_ = 0;
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
