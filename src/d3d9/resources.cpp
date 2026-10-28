// Textures, surfaces, buffers and vertex declarations of the D3D9 device model (d3d9/device.h).

#include <algorithm>
#include <atomic>
#include <bit>
#include <cstring>

#include <spdlog/spdlog.h>

#include "d3d9/device.h"
#include "vk/debug_names.h"

namespace rvk::d3d9 {

// ---- Texture ---------------------------------------------------------------------------------------------------------

Texture::Texture(Device& device, const TextureDesc& desc) : dev_(device), desc_(desc) {
    dev_.trackLive(this);
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
    if (!image_.create(dev_.ctx(), id)) {
        return;
    }
    slot_ = dev_.bindless().addTexture(kind_, image_.view());
    applyDebugName();  // after the slot: the name carries it
    valid_ = true;
}

// ---- debug names (vk/debug_names.h) ----

std::string Texture::debugName() const {
    vk::names::TextureInfo t;
    t.label = label_;
    t.labelIndex = labelIndex_;
    t.id = id_;
    t.width = desc_.width;
    t.height = desc_.height;
    t.depth = desc_.depth;
    t.levels = desc_.levels;
    t.format = vk::d3dFormatName(desc_.format);
    t.cube = desc_.type == TextureType::Cube;
    t.volume = desc_.type == TextureType::Volume;
    t.renderTarget = isRenderTarget();
    t.depthFormat = fmt_.depth;
    t.dynamic = (desc_.usage & USAGE_DYNAMIC) != 0;
    t.slot = slot_;
    return vk::names::textureName(t);
}

void Texture::applyDebugName() {
    vk::Context& ctx = dev_.ctx();
    if (!image_.valid() || !ctx.namesEnabled()) return;
    vk::names::image(ctx, image_, debugName());
}

void Texture::setDebugLabel(const char* label, std::uint32_t index) {
    if (label == label_ && index == labelIndex_) return;  // static strings: the pointer says it
    label_ = label;
    labelIndex_ = index;
    if (!image_.valid() || !dev_.ctx().namesEnabled()) return;
    const std::string name = debugName();
    vk::names::image(dev_.ctx(), image_, name);
    // a pooled target that changes roles every frame would flood the log: the first 160 only
    static std::atomic<int> logged{0};
    if (label != nullptr && logged.fetch_add(1) < 160) {
        spdlog::info("names: {} = image '{}' bindless slot {}", label, name, slot_);
    }
}

Texture::~Texture() {
    if (!dev_.untrackLive(this)) {
        // orphaned by Device::destroy: only the CPU copies are left
        surfaces_.clear();
        for (Level& l : cpu_) freeLevel(l);
        return;
    }
    if (role_ != scene::ImageRole::None) {
        dev_.forgetImage(this);
    }
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

void Texture::orphan() {
    // Device::destroy (frames idle): what the destructor would hand to the device, now; the frames run the retired work
    if (slot_ != 0) {
        dev_.bindless().releaseTexture(kind_, slot_);
        slot_ = 0;
    }
    if (image_.valid()) {
        auto* ctx = &dev_.ctx();
        auto img = std::make_shared<vk::Image>(std::move(image_));
        dev_.frames().retire([ctx, img] { img->destroy(*ctx); });
    }
    role_ = scene::ImageRole::None;
    orphaned_ = true;
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
    switch (textureLockDirty(flags, l.uploaded || gpuWritten_)) {
        case LockDirty::Locked:
            markDirty(l, level, rect);
            break;
        case LockDirty::WholeLevel: {
            // a fresh level is dirty as a whole in D3D9: what is written under this READONLY lock is uploaded (the savegame
            // thumbnails, eCGfxImageData::OnRead; docs/research/savegame-thumbnails.md)
            markDirty(l, level, nullptr);
            static std::atomic<int> logged{0};
            if (desc_.pool != POOL_SYSTEMMEM && logged.fetch_add(1) < 4) {  // system memory: read-back targets, not sampled
                spdlog::info("d3d9: read-only lock of level {} of a fresh {}x{} {} texture (pool {}): the level is uploaded as a whole",
                             level, desc_.width, desc_.height, vk::d3dFormatName(desc_.format), desc_.pool);
            }
            break;
        }
        case LockDirty::None:
            break;
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
    bool copied = false;
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
            dev_.countUpload(true, sliceBytes * d);
            l.dirty = false;
            copied = true;
            // File textures (never locked) are not read back by the engine: drop the CPU copy. A texture that was locked keeps
            // it like D3D9's managed pool: partial locks (the engine's glyph atlases) must see the earlier contents.
            if (!lockedOnce_ && desc_.pool != POOL_SYSTEMMEM && (desc_.usage & USAGE_DYNAMIC) == 0) {
                freeLevel(l);
                l.dropped = true;
            }
        }
    }
    if (copied) {
        generation_.fetch_add(1, std::memory_order_relaxed);  // identity.h: new CPU data on the GPU
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
    : dev_(device), size_(size), usage_(usage), pool_(pool), index_(index), index32_(index32), cpu_(size, 0) {
    dev_.trackLive(this);
}

Buffer::~Buffer() {
    if (!dev_.untrackLive(this)) return;  // orphaned by Device::destroy
    if (static_.valid()) {
        auto* arena = &dev_.meshArena();
        dev_.frames().retire([arena, a = static_] { arena->free(a); });
    }
}

void Buffer::orphan() {
    cleanStatic_.store(false, std::memory_order_release);
    if (static_.valid()) {
        auto* arena = &dev_.meshArena();
        dev_.frames().retire([arena, a = static_] { arena->free(a); });
        static_ = {};
    }
    orphaned_ = true;
}

void* Buffer::lock(std::uint32_t offset, std::uint32_t size, std::uint32_t flags) {
    if (offset >= size_ && size_ != 0) {
        return nullptr;
    }
    const std::lock_guard lock(mutex_);
    copies_.lock(offset, size, flags, size_);
    cleanStatic_.store(!copies_.dirty() && cleanStatic_.load(std::memory_order_relaxed), std::memory_order_release);
    return cpu_.data() + offset;
}

void Buffer::unlock() {}

bool Buffer::readCpu(std::uint32_t offset, std::uint32_t size, void* out) {
    const std::lock_guard lock(mutex_);
    if (offset > size_ || size > size_ - offset) return false;
    std::memcpy(out, cpu_.data() + offset, size);
    return true;
}

Buffer::Binding Buffer::gpu(std::uint32_t strideHint) {
    if (cleanStatic_.load(std::memory_order_acquire)) return binding_;
    const std::lock_guard lock(mutex_);
    const std::uint64_t frame = dev_.frameValue();
    const bool ring = dynamic() || pool_ == POOL_SYSTEMMEM;
    const BufferCopyState::Plan plan = copies_.plan(ring, frame, size_, static_.valid());
    using Op = BufferCopyState::Plan::Op;
    if (ring) {
        // ring copy per frame (identity.h: BufferCopyState::plan), in a block of only the bytes it needs (identity.h: RingBlock;
        // a DISCARD of a few KiB of our 4 MiB native vertex buffer reserves those KiB, not 4 MiB)
        const RingBlock blk = ringBlock(plan, ringLo_, ringHi_, size_);
        if (blk.fresh) {
            const VkDeviceSize bytes = std::max<VkDeviceSize>(blk.hi - blk.lo, 16);
            // at least lo into its chunk: the binding (block offset - lo) addresses buffer byte 0 and stays >= 0
            vk::RingAlloc a = dev_.frames().allocate(bytes, 16, blk.lo);
            if (a.ptr == nullptr) {
                return {};
            }
            ringPtr_ = static_cast<std::uint8_t*>(a.ptr);
            ringLo_ = blk.lo;
            ringHi_ = blk.hi;
            binding_ = {a.buffer, a.offset - blk.lo};
        }
        if (blk.copyHi > blk.copyLo) {
            std::memcpy(ringPtr_ + (blk.copyLo - ringLo_), cpu_.data() + blk.copyLo, blk.copyHi - blk.copyLo);
            dev_.countUpload(false, blk.copyHi - blk.copyLo);
        }
        copies_.committed(plan, true, frame);
        return binding_;
    }
    if (plan.op == Op::NewCopy) {
        // static buffer: a fresh copy per update in the mesh arena (vk/mesh_arena.h), so draws recorded earlier keep the old
        // contents; it starts at a multiple of the first use's stride / index size (draws fold the offset into the draw).
        // With the ray tracing tier the arena is also a BLAS build input that shaders read through its address
        // (docs/research/raytracing.md 2)
        const vk::MeshArena::Alloc na = dev_.meshArena().allocate(size_, strideHint != 0 ? strideHint : (index32_ ? 4u : 2u));
        if (!na.valid()) {
            return {};
        }
        vk::RingAlloc staging = dev_.frames().allocate(size_, 16);
        if (staging.ptr == nullptr) {
            dev_.meshArena().free(na);
            return {};
        }
        std::memcpy(staging.ptr, cpu_.data(), size_);
        dev_.countUpload(false, size_);
        VkCommandBuffer cmd = dev_.uploadCommands();
        const VkBufferCopy copy{staging.offset, na.offset, size_};
        vkCmdCopyBuffer(cmd, staging.buffer, na.buffer, 1, &copy);
        const bool renamed = static_.valid();
        if (renamed) {
            auto* arena = &dev_.meshArena();
            dev_.frames().retire([arena, a = static_] { arena->free(a); });
        }
        static_ = na;
        binding_ = {na.buffer, na.offset};
        copies_.committed(plan, false, frame);
        // observers (enh/rt_scene) forget what they built from the old copy (its address dies with it)
        if (renamed) dev_.notifyBufferChanged(id_);
    }
    if (static_.valid() && !copies_.dirty()) cleanStatic_.store(true, std::memory_order_release);
    return binding_;
}

VkDeviceAddress Buffer::deviceAddress() {
    const std::lock_guard lock(mutex_);
    if (!static_.valid() || dynamic() || pool_ == POOL_SYSTEMMEM || !dev_.ctx().features().rayTracing.enabled) return 0;
    return static_.address;
}

// ---- VertexDecl ------------------------------------------------------------------------------------------------------

VertexDecl::VertexDecl(std::span<const VertexElement> elements) {
    static std::atomic<std::uint64_t> nextId{1};
    id_ = nextId.fetch_add(1, std::memory_order_relaxed);
    for (const auto& e : elements) {
        if (e.stream == kDeclEndStream) {
            break;
        }
        elements_.push_back(e);
        blendIndices_ |= e.usage == static_cast<std::uint8_t>(shader::Usage::BlendIndices);
        texcoord4_ |= e.usage == static_cast<std::uint8_t>(shader::Usage::TexCoord) && e.usageIndex == 4;
    }
}

}  // namespace rvk::d3d9
