// The Vulkan backend of the slim RHI (rhi/rhi.h): a thin layer over the renderer's own vk:: classes (Context, Frames, Bindless, PassLayout,
// Image, pass pipelines). Nothing here is new Vulkan code: it is the pass machinery of vk/passes.h behind the interface, plus the pieces the
// interface adds on top (texture subresource views, vertex / index / indirect draws, acceleration structures).
//
// Two ways to get a Device: createVulkanDevice (owns its own vk::Context / Frames / Bindless: tools, tests) and rhi::vulkan::wrap (rhi/vulkan/wrap.h:
// the same implementation over the renderer's own objects, with non-owning textures / command lists over its images / frame command buffer: the
// pass migration, docs/research/hlsl-rhi.md 8). In the wrapped mode nothing is destroyed with a GPU wait: objects are retired through
// Frames::retire (VkDeviceImpl::defer).

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <format>
#include <map>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include <spdlog/spdlog.h>

#include "rhi/game_device.h"
#include "rhi/rhi.h"
#include "rhi/vulkan/game_present.h"
#include "rhi/vulkan/wrap.h"
#include "shader/abi.h"
#include "vk/bindless.h"
#include "vk/context.h"
#include "vk/crash_markers.h"
#include "vk/debug_names.h"
#include "vk/frames.h"
#include "vk/framegen.h"
#include "vk/gputimer.h"
#include "vk/game_record.h"
#include "vk/image.h"
#include "vk/mesh_arena.h"
#include "vk/passes.h"
#include "vk/pipeline.h"
#include "vk/raytracing.h"
#include "vk/swapchain.h"

namespace rvk::rhi {
static_assert(kFramesInFlight == vk::Frames::kInFlight);
namespace {

constexpr std::uint32_t kConstantBufferBinding = 8;  // b8 space1: set 1 binding 8
// (storage images: u0..u7 space1 = set 1 bindings 0..7, rhi::kStorageImages)

class VkDeviceImpl;

class VkBufferImpl final : public Buffer {
public:
    VkBufferImpl(VkDeviceImpl& dev, const BufferDesc& desc) : dev_(dev), desc_(desc) {}
    ~VkBufferImpl() override;
    bool create();
    const BufferDesc& desc() const override { return desc_; }
    void* map() override { return mapped_; }
    void invalidate() override;
    void flush() override;
    std::uint32_t heapIndex() const override { return 0; }    // Vulkan reaches buffers by address (RVK_REF)
    std::uint32_t heapIndexRw() const override { return 0; }
    std::array<std::uint32_t, 2> shaderRef(std::uint64_t offset) const override {
        const std::uint64_t a = address_ + offset;
        return {static_cast<std::uint32_t>(a), static_cast<std::uint32_t>(a >> 32)};
    }
    // (the same address for read and write access)
    std::array<std::uint32_t, 2> shaderRefRw(std::uint64_t offset) const override { return shaderRef(offset); }
    std::uint64_t gpuAddress() const override { return address_; }
    VkBuffer buffer() const { return buffer_; }
    std::uint64_t address() const { return address_; }
    // a buffer owned elsewhere (rhi::vulkan::wrapBuffer): not destroyed here
    void wrapExternal(VkBuffer buffer, std::uint64_t address, void* mapped) {
        buffer_ = buffer;
        address_ = address;
        mapped_ = mapped;
        external_ = true;
    }

private:
    VkDeviceImpl& dev_;
    BufferDesc desc_;
    bool external_ = false;
    VkBuffer buffer_ = VK_NULL_HANDLE;
    VmaAllocation alloc_ = VK_NULL_HANDLE;
    void* mapped_ = nullptr;
    std::uint64_t address_ = 0;
};

class VkTextureImpl final : public Texture {
public:
    VkTextureImpl(VkDeviceImpl& dev, const TextureDesc& desc) : dev_(dev), desc_(desc) {}
    ~VkTextureImpl() override;
    bool create();
    const TextureDesc& desc() const override { return desc_; }
    std::uint32_t heapSampled() const override { return slot_; }
    std::uint32_t heapSampledLayer(std::uint32_t layer) override;
    VkImageLayout layout() const override { return image_->layout(); }
    void setDebugName(const std::string& name) override;
    vk::Image& image() { return *image_; }
    // the texture is a view of an image owned elsewhere (rhi::vulkan::wrapImage): its tracked layout (vk::Image) stays the one source of truth, `sampledSlot`
    // (bindless 2D slot, 0 = none) is not released here
    void wrap(vk::Image& image, std::uint32_t sampledSlot) {
        image_ = &image;
        external_ = true;
        slot_ = sampledSlot;
    }
    // a swapchain image (owned by the swapchain)
    void adopt(VkImage image, VkImageView view) {
        vk::ImageDesc d;
        d.format = desc_.format;
        d.extent = desc_.extent;
        d.usage = desc_.usage;
        own_.adopt(image, view, d);
    }
    // 2D view of one mip level / layer as a render target
    VkImageView attachmentView(std::uint32_t mip, std::uint32_t layer);
    // the storage image view of one mip level / layer (rhi.h StorageView: 3D, 2D array of one layer, or 2D)
    VkImageView storageView(std::uint32_t mip, std::uint32_t layer);

private:
    bool isArray() const { return !desc_.cube && desc_.type == VK_IMAGE_TYPE_2D && desc_.layers > 1; }
    vk::TextureKind kind() const {
        return desc_.cube ? vk::TextureKind::Cube : desc_.type == VK_IMAGE_TYPE_3D ? vk::TextureKind::Volume : vk::TextureKind::Tex2D;
    }
    bool plain2D() const { return desc_.type == VK_IMAGE_TYPE_2D && !desc_.cube && desc_.layers == 1 && desc_.mips == 1; }
    // a view of a subresource range, cached (destroyed with the texture)
    VkImageView view(VkImageViewType type, VkImageAspectFlags aspect, std::uint32_t baseMip, std::uint32_t mipCount, std::uint32_t baseLayer,
                     std::uint32_t layerCount);

    VkDeviceImpl& dev_;
    TextureDesc desc_;
    vk::Image own_;
    vk::Image* image_ = &own_;
    bool external_ = false;
    std::uint32_t slot_ = 0;
    std::vector<std::uint32_t> layerSlots_;  // 2D arrays: the Texture2D slot of each layer, created on demand
    std::map<std::array<std::uint32_t, 6>, VkImageView> views_;
};

class VkSwapchainImpl final : public Swapchain {
public:
    explicit VkSwapchainImpl(VkDeviceImpl& dev) : dev_(dev) {}
    ~VkSwapchainImpl() override;
    bool create(const SwapchainDesc& desc);
    std::uint32_t width() const override { return sc_.extent().width; }
    std::uint32_t height() const override { return sc_.extent().height; }
    VkFormat format() const override { return sc_.format(); }
    bool resize(std::uint32_t w, std::uint32_t h) override;
    vk::Swapchain& swapchain() { return sc_; }
    VkTextureImpl& image(std::uint32_t i) { return *images_[i]; }
    VkSemaphore acquired() const { return acquired_; }

private:
    void wrapImages();
    VkDeviceImpl& dev_;
    vk::Swapchain sc_;
    std::vector<std::unique_ptr<VkTextureImpl>> images_;
    VkSemaphore acquired_ = VK_NULL_HANDLE;
};

class VkPipelineImpl final : public Pipeline {
public:
    VkPipelineImpl(VkDeviceImpl& dev, VkPipeline p, VkPipelineBindPoint bp) : dev_(dev), pipeline_(p), bindPoint_(bp) {}
    ~VkPipelineImpl() override;
    VkPipeline pipeline() const { return pipeline_; }
    VkPipelineBindPoint bindPoint() const { return bindPoint_; }

private:
    VkDeviceImpl& dev_;
    VkPipeline pipeline_;
    VkPipelineBindPoint bindPoint_;
};

// A BLAS or TLAS: the acceleration structure object and its storage (a buffer with a device address), sized from
// vkGetAccelerationStructureBuildSizesKHR when it is created. A TLAS also keeps its build scratch for rebuilds; BLAS builds take their scratch from the
// device's shared pool (VkDeviceImpl::ensureScratch), so thousands of BLASes do not each hold one.
class VkAccelImpl final : public AccelerationStructure {
public:
    VkAccelImpl(VkDeviceImpl& dev, bool tlas) : dev_(dev), tlas_(tlas) {}
    ~VkAccelImpl() override;
    bool createBlas(const BlasDesc& desc);
    bool createTlas(const TlasDesc& desc);
    bool createStorage(std::uint64_t bytes, const char* name);
    bool isTlas() const override { return tlas_; }
    std::uint64_t address() const override { return address_; }
    std::uint32_t tlasSlot() const override { return slot_; }
    std::uint64_t storageBytes() const override { return storage_.size; }
    VkAccelerationStructureKHR handle() const { return handle_; }
    const std::vector<TriangleGeometry>& geometries() const { return geometries_; }
    bool allowUpdate() const { return allowUpdate_; }
    VkBuildAccelerationStructureFlagsKHR flags() const { return flags_; }
    VkDeviceSize scratchNeed(bool update) const { return update ? updateScratch_ : buildScratch_; }
    void recordBuildTlas(VkCommandBuffer cmd, VkDeviceAddress instances, std::uint32_t count);

private:
    bool allocate(const VkAccelerationStructureBuildGeometryInfoKHR& info, const std::uint32_t* maxPrimitives, const char* what, bool scratch);
    bool createHandle(VkAccelerationStructureTypeKHR type, VkDeviceSize size, const char* what);

    VkDeviceImpl& dev_;
    bool tlas_;
    std::vector<TriangleGeometry> geometries_;  // BLAS: the buffers are referenced, not owned
    std::uint32_t maxInstances_ = 0;
    VkBuildAccelerationStructureFlagsKHR flags_ = 0;
    bool allowUpdate_ = false;
    VkDeviceSize buildScratch_ = 0, updateScratch_ = 0;
    vk::rt::AddressBuffer storage_, scratch_;  // (scratch_: TLAS only)
    VkAccelerationStructureKHR handle_ = VK_NULL_HANDLE;
    VkDeviceAddress address_ = 0;
    std::uint32_t slot_ = 0;
    bool hasSlot_ = false;  // a TLAS holds a slot of the shared bindless array (0 is a valid one)
};

// compacted sizes of built BLASes: a query pool
class VkSizeQueryImpl final : public SizeQuery {
public:
    VkSizeQueryImpl(VkDeviceImpl& dev, std::uint32_t capacity) : dev_(dev), capacity_(capacity) {}
    ~VkSizeQueryImpl() override;
    bool create();
    std::uint32_t capacity() const override { return capacity_; }
    bool read(std::uint32_t first, std::uint32_t count, std::uint64_t* sizes) override;
    VkQueryPool pool() const { return pool_; }

private:
    VkDeviceImpl& dev_;
    std::uint32_t capacity_;
    VkQueryPool pool_ = VK_NULL_HANDLE;
};

class VkCommandListImpl final : public CommandList {
public:
    VkCommandListImpl(VkDeviceImpl& dev, VkCommandBuffer cmd) : dev_(dev), cmd_(cmd) {}
    VkCommandBuffer cmd() const { return cmd_; }
    // records into another command buffer from now on, with no pipeline / bindings bound (the device's persistent frame lists)
    void rebind(VkCommandBuffer cmd) {
        cmd_ = cmd;
        pipeline_ = nullptr;
        cb_ = {};
        views_.fill(VK_NULL_HANDLE);
        dirty_ = true;
        pipelineBound_ = false;
    }

    void transition(Texture& texture, VkImageLayout layout) override;
    void transitionMips(Texture& texture, VkImageLayout layout, std::uint32_t baseMip, std::uint32_t mipCount) override;
    void transitions(std::span<const TextureTransition> list) override;
    void bufferBarrier(Buffer&, BufferAccess) override;
    void computeBarrier() override;
    void memoryBarrier(VkPipelineStageFlags2 srcStages, VkAccessFlags2 srcAccess, VkPipelineStageFlags2 dstStages, VkAccessFlags2 dstAccess) override {
        VkMemoryBarrier2 b{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
        b.srcStageMask = srcStages;
        b.srcAccessMask = srcAccess;
        b.dstStageMask = dstStages;
        b.dstAccessMask = dstAccess;
        VkDependencyInfo di{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
        di.memoryBarrierCount = 1;
        di.pMemoryBarriers = &b;
        vkCmdPipelineBarrier2(cmd_, &di);
    }
    void setPushConstants(const void* data, std::uint32_t size) override;
    void setConstantBuffer(Buffer& buffer, std::uint64_t offset, std::uint64_t size) override {
        cb_ = {static_cast<VkBufferImpl&>(buffer).buffer(), offset, size};
        dirty_ = true;
    }
    void setStorageViews(std::span<const StorageView> views) override {
        views_.fill(VK_NULL_HANDLE);
        for (std::size_t i = 0; i < views.size() && i < views_.size(); ++i) {
            if (views[i].texture != nullptr) views_[i] = static_cast<VkTextureImpl*>(views[i].texture)->storageView(views[i].mip, views[i].layer);
        }
        dirty_ = true;
    }
    void bindPipeline(Pipeline& pipeline) override;
    void dispatch(std::uint32_t x, std::uint32_t y, std::uint32_t z) override {
        flush();
        vkCmdDispatch(cmd_, x, y, z);
    }
    void beginRendering(const RenderingInfo& info) override;
    void endRendering() override { vkCmdEndRendering(cmd_); }
    void setViewport(float x, float y, float w, float h) override {
        const VkViewport vp{x, y + h, w, -h, 0.0f, 1.0f};  // y down like D3D: a negative height
        vkCmdSetViewport(cmd_, 0, 1, &vp);
    }
    void setScissor(std::int32_t x, std::int32_t y, std::uint32_t w, std::uint32_t h) override {
        const VkRect2D r{{x, y}, {w, h}};
        vkCmdSetScissor(cmd_, 0, 1, &r);
    }
    void draw(std::uint32_t vertexCount, std::uint32_t instanceCount, std::uint32_t firstVertex, std::uint32_t firstInstance) override {
        flush();
        vkCmdDraw(cmd_, vertexCount, instanceCount, firstVertex, firstInstance);
    }
    void setVertexBuffers(std::uint32_t first, std::span<Buffer* const> buffers, std::span<const std::uint64_t> offsets,
                          std::span<const std::uint32_t> strides) override;
    void setIndexBuffer(Buffer& buffer, std::uint64_t offset, VkIndexType type) override {
        vkCmdBindIndexBuffer(cmd_, static_cast<VkBufferImpl&>(buffer).buffer(), offset, type);
    }
    void drawIndexed(std::uint32_t indexCount, std::uint32_t instanceCount, std::uint32_t firstIndex, std::int32_t vertexOffset,
                     std::uint32_t firstInstance) override {
        flush();
        vkCmdDrawIndexed(cmd_, indexCount, instanceCount, firstIndex, vertexOffset, firstInstance);
    }
    void drawIndexedIndirect(Buffer& args, std::uint64_t argsOffset, std::uint32_t drawCount, std::uint32_t stride) override;
    void drawIndexedIndirectCount(Buffer& args, std::uint64_t argsOffset, Buffer& countBuffer, std::uint64_t countOffset, std::uint32_t maxDraws,
                                  std::uint32_t stride) override {
        flush();
        vkCmdDrawIndexedIndirectCount(cmd_, static_cast<VkBufferImpl&>(args).buffer(), argsOffset, static_cast<VkBufferImpl&>(countBuffer).buffer(),
                                      countOffset, maxDraws, stride);
    }
    void copyBuffer(Buffer& dst, std::uint64_t dstOffset, Buffer& src, std::uint64_t srcOffset, std::uint64_t size) override {
        const VkBufferCopy c{srcOffset, dstOffset, size};
        vkCmdCopyBuffer(cmd_, static_cast<VkBufferImpl&>(src).buffer(), static_cast<VkBufferImpl&>(dst).buffer(), 1, &c);
    }
    void fillBuffer(Buffer& dst, std::uint64_t offset, std::uint64_t size, std::uint32_t value) override {
        vkCmdFillBuffer(cmd_, static_cast<VkBufferImpl&>(dst).buffer(), offset, size, value);
    }
    void copyBufferToTexture(Texture& dst, Buffer& src, std::uint64_t srcOffset, Subresource sub) override;
    void copyTextureToBuffer(Buffer& dst, std::uint64_t dstOffset, Texture& src, Subresource sub) override;
    void copyBufferToTextureBox(Texture& dst, Buffer& src, const BufferTextureCopy& copy) override;
    void clearTexture(Texture& texture, const VkClearColorValue& value, std::uint32_t baseMip, std::uint32_t mipCount) override;
    void copyTexture(Texture& dst, Texture& src, std::span<const TextureCopy> regions) override;
    bool blitTexture(Texture& dst, Texture& src, std::span<const TextureBlit> regions, VkFilter filter) override;
    void buildBlas(AccelerationStructure& blas) override {
        const BlasBuild b{&blas, {}, false};
        buildBlases(std::span<const BlasBuild>(&b, 1));
    }
    void buildTlas(AccelerationStructure& tlas, Buffer& instances, std::uint64_t instanceOffset, std::uint32_t count) override {
        static_cast<VkAccelImpl&>(tlas).recordBuildTlas(cmd_, static_cast<VkBufferImpl&>(instances).address() + instanceOffset, count);
    }
    void buildTlasAt(AccelerationStructure& tlas, std::uint64_t instancesAddress, std::uint32_t count) override {
        static_cast<VkAccelImpl&>(tlas).recordBuildTlas(cmd_, instancesAddress, count);
    }
    void buildBlases(std::span<const BlasBuild> builds) override;
    void queryCompactedSizes(SizeQuery& query, std::uint32_t first, std::span<AccelerationStructure* const> blases) override;
    void compactBlas(AccelerationStructure& dst, AccelerationStructure& src) override;
    void beginRegion(std::string_view) override {}
    void endRegion() override {}

private:
    void flush();

    VkDeviceImpl& dev_;
    VkCommandBuffer cmd_;
    const VkPipelineImpl* pipeline_ = nullptr;
    struct Cb {
        VkBuffer buffer = VK_NULL_HANDLE;
        std::uint64_t offset = 0, size = 0;
    } cb_;
    std::array<VkImageView, rhi::kStorageImages> views_{};
    bool dirty_ = true;
    bool pipelineBound_ = false;
};

class VkDeviceImpl final : public GameDevice {
public:
    VkDeviceImpl() : ctx_(ownCtx_), frames_(ownFrames_), bindless_(ownBindless_) { record_.init(ctx_, frames_, bindless_); }
    // over the renderer's objects (rhi::vulkan::wrap)
    VkDeviceImpl(vk::Context& ctx, vk::Frames& frames, vk::Bindless& bindless, vk::MeshArena* arena)
        : ctx_(ctx), frames_(frames), bindless_(bindless), external_(true), arena_(arena) { record_.init(ctx_, frames_, bindless_); }
    // the game device: owns its Context / Frames / Bindless / MeshArena / presentation, retires through the frames like a wrapped one (createGameDevice)
    struct GameTag {};
    explicit VkDeviceImpl(GameTag) : ctx_(ownCtx_), frames_(ownFrames_), bindless_(ownBindless_), external_(true), ownsGame_(true), arena_(&ownArena_) {
        record_.init(ctx_, frames_, bindless_);
    }

    ~VkDeviceImpl() override {
        record_.shutdown();  // joins the record threads and destroys the pipeline cache while the renderer's objects still exist
        gpuTimer_.destroy();
        if (ownsGame_) {
            destroyGame();
            return;
        }
        if (external_) {
            // the renderer's objects outlive us: what we made (samplers, pass layout) goes through the frame retirement
            vk::Context* ctx = &ctx_;
            vk::PassLayout layout = layout_;
            std::vector<VkSampler> samplers = std::move(samplers_);
            vk::rt::AddressBuffer scratch = scratch_;
            frames_.retire([ctx, layout, samplers, scratch]() mutable {
                for (VkSampler s : samplers) vkDestroySampler(ctx->device(), s, nullptr);
                layout.destroy(*ctx);
                scratch.destroy(*ctx);
            });
            return;
        }
        if (ctx_.device() != VK_NULL_HANDLE) ctx_.waitIdle();
        scratch_.destroy(ctx_);
        for (vk::rt::AddressBuffer& g : graveyard_) g.destroy(ctx_);
        for (VkSampler s : samplers_) vkDestroySampler(ctx_.device(), s, nullptr);
        layout_.destroy(ctx_);
        frames_.destroy();  // (runs the retired slot releases of the textures: the bindless set must still exist)
        bindless_.destroy();
        ctx_.destroy();
    }

    bool create(const DeviceDesc& desc) {
        vk::ContextOptions o;
        o.validation = desc.validation || desc.syncValidation;
        o.syncValidation = desc.syncValidation;
        o.adapter = desc.adapter;
        o.appName = desc.name;
        o.allowRayTracing = desc.rayTracing;
        if (!ctx_.create(o)) return false;
        if (!frames_.create(ctx_) || !bindless_.create(ctx_, frames_)) return false;
        if (!createLayout()) return false;
        setRefShift(refShift());
        caps_.adapterName = ctx_.deviceName();
        caps_.backend = Backend::Vulkan;
        caps_.bindless = true;
        caps_.heapSize = vk::Bindless::kMax2D;
        caps_.drawIndirectCount = ctx_.features().drawIndirectCount;
        caps_.multiDrawIndirect = ctx_.features().multiDrawIndirect;
        caps_.drawIndirectFirstInstance = ctx_.features().drawIndirectFirstInstance;
        caps_.textureBlit = true;
        // ray queries need the acceleration structure array in the bindless set (descriptor binding update after bind)
        caps_.rayQuery = ctx_.features().rayTracing.enabled && ctx_.features().rayTracing.tlasDescriptors;
        fillGameCaps();
        return true;
    }

    // over existing objects: the pass layout of the fixed binding model is the only thing created. No acceleration structures (the renderer's own
    // TLAS slots are not shared with allocTlasSlot yet), no swapchain, no submission.
    bool createWrapped() {
        if (!createLayout()) return false;
        setRefShift(refShift());
        caps_.adapterName = ctx_.deviceName();
        caps_.backend = Backend::Vulkan;
        caps_.bindless = true;
        caps_.heapSize = vk::Bindless::kMax2D;
        caps_.drawIndirectCount = ctx_.features().drawIndirectCount;
        caps_.multiDrawIndirect = ctx_.features().multiDrawIndirect;
        caps_.drawIndirectFirstInstance = ctx_.features().drawIndirectFirstInstance;
        caps_.textureBlit = true;
        // acceleration structures of the wrapped device: TLAS slots from the renderer's bindless set (shared with the RT scene)
        caps_.rayQuery = ctx_.features().rayTracing.enabled && ctx_.features().rayTracing.tlasDescriptors;
        fillGameCaps();
        return true;
    }

    // ---- the game device (createGameDevice) ----
    bool createGame(const GameDeviceOptions& options) {
        vk::ContextOptions co;
        co.validation = options.validation || options.syncValidation;
        co.adapter = options.adapter;
        co.syncValidation = options.syncValidation;
        co.allowRayTracing = options.allowRayTracing;
        co.frameGenQueues = options.frameGenQueues;
        if (!ctx_.create(co)) return false;
        if (!frames_.create(ctx_) || !bindless_.create(ctx_, frames_)) return false;
        frames_.attachRing(&ring_);
        if (!createLayout()) return false;
        setRefShift(refShift());
        caps_.adapterName = ctx_.deviceName();
        caps_.backend = Backend::Vulkan;
        caps_.bindless = true;
        caps_.heapSize = vk::Bindless::kMax2D;
        caps_.drawIndirectCount = ctx_.features().drawIndirectCount;
        caps_.multiDrawIndirect = ctx_.features().multiDrawIndirect;
        caps_.drawIndirectFirstInstance = ctx_.features().drawIndirectFirstInstance;
        caps_.textureBlit = true;
        caps_.rayQuery = ctx_.features().rayTracing.enabled && ctx_.features().rayTracing.tlasDescriptors;
        fillGameCaps();
        {
            // the static vertex / index buffers' arena (vk/mesh_arena.h); with the ray tracing tier also BLAS build inputs that shaders read through their
            // addresses (docs/research/raytracing.md 2)
            VkBufferUsageFlags usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_VERTEX_BUFFER_BIT;  // (TRANSFER_SRC: the cluster copies' read-back check)
            if (ctx_.features().rayTracing.enabled) {
                usage |= VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                         VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR;
            }
            ownArena_.create(ctx_, usage);
        }
        presenter_ = std::make_unique<vulkan::VkGamePresenter>(*this, ctx_, frames_);
        {
            VkBufferCreateInfo bci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
            bci.size = 16;
            bci.usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT;
            VmaAllocationCreateInfo aci{};
            aci.priority = 0.5f;  // VK_EXT_memory_priority: the default (a zero-initialised priority is the lowest)
            aci.usage = VMA_MEMORY_USAGE_AUTO;
            aci.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;
            VmaAllocationInfo info{};
            if (vmaCreateBuffer(ctx_.allocator(), &bci, &aci, &nullVertexVk_, &nullVertexAlloc_, &info) != VK_SUCCESS) return false;
            nullVertexBuffer_ = reinterpret_cast<std::uint64_t>(nullVertexVk_);
            vk::names::object(ctx_, nullVertexVk_, VK_OBJECT_TYPE_BUFFER, "rvk null vertex buffer (unbound streams)");
            const float v[4] = {0.0f, 0.0f, 0.0f, 1.0f};
            std::memcpy(info.pMappedData, v, sizeof(v));
        }
        frameValue_ = frames_.currentValue();
        frameSlot_ = frames_.currentSlot();
        return true;
    }

    // the device is idle (the owner waited): everything it owns goes, in the order the objects depend on each other
    void destroyGame() {
        if (ctx_.device() == VK_NULL_HANDLE) return;
        ctx_.waitIdle();
        presenter_.reset();  // the present thread, the swapchain, AMD's swapchain, the acquire semaphores
        mainList_.reset();
        uploadList_.reset();
        transients_.clear();  // (the textures' destructors retire their images through the frames)
        ringBuffers_.clear();
        meshBuffers_.clear();
        scratch_.destroy(ctx_);
        for (vk::rt::AddressBuffer& g : graveyard_) g.destroy(ctx_);
        graveyard_.clear();
        for (VkSampler s : samplers_) vkDestroySampler(ctx_.device(), s, nullptr);
        samplers_.clear();
        layout_.destroy(ctx_);
        if (nullVertexVk_ != VK_NULL_HANDLE) {
            vmaDestroyBuffer(ctx_.allocator(), nullVertexVk_, nullVertexAlloc_);
            nullVertexVk_ = VK_NULL_HANDLE;
            nullVertexBuffer_ = 0;
        }
        // frames_ runs the retired destructors (images, bindless slot releases): before the bindless heap goes away
        frames_.destroy();
        ownArena_.destroy();  // after frames_: its retired frees go into the arena
        bindless_.destroy();
        ctx_.setLostReporter(nullptr);
        crashMarkers_.destroy();
        ctx_.destroy();
    }

    void fillGameCaps() {
        const vk::DeviceFeatures& f = ctx_.features();
        const VkPhysicalDeviceLimits& lim = ctx_.properties().limits;
        gameCaps_.rayTracing = f.rayTracing.enabled;
        gameCaps_.vertexStores = f.vertexStores;
        gameCaps_.storageImageExtendedFormats = f.storageImageExtendedFormats;
        gameCaps_.maxComputeWorkGroupInvocations = lim.maxComputeWorkGroupInvocations;
        gameCaps_.maxComputeWorkGroupSizeX = lim.maxComputeWorkGroupSize[0];
        gameCaps_.maxComputeSharedMemory = lim.maxComputeSharedMemorySize;
        gameCaps_.uploadRowAlign = 1;      // tightly packed rows
        gameCaps_.uploadOffsetAlign = 16;  // the ring's own alignment
        gameCaps_.extendedDynamicState3 = f.extendedDynamicState3;
        gameCaps_.vertexInputDynamicState = f.vertexInputDynamicState;
        gameCaps_.samplerAnisotropy = f.samplerAnisotropy;
        gameCaps_.maxAnisotropy = f.samplerAnisotropy ? f.maxAnisotropy : 1.0f;
        gameCaps_.fullScreenExclusive = f.fullScreenExclusive;
        gameCaps_.maxVertexInputAttributes = lim.maxVertexInputAttributes;
    }

    bool createLayout() {
        VkDescriptorSetLayoutBinding b[rhi::kStorageImages + 1]{};
        const VkShaderStageFlags stages = VK_SHADER_STAGE_COMPUTE_BIT | VK_SHADER_STAGE_FRAGMENT_BIT | VK_SHADER_STAGE_VERTEX_BIT;
        for (std::uint32_t i = 0; i < rhi::kStorageImages; ++i) b[i] = {i, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, stages, nullptr};
        b[rhi::kStorageImages] = {kConstantBufferBinding, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, stages, nullptr};
        return layout_.create(ctx_, bindless_, b);
    }

    bool external() const { return external_; }
    // Runs `fn` when the GPU no longer uses what it destroys: after the frame being recorded has completed (Frames::retire) on a wrapped device,
    // after a full wait on an owning one (tools, tests: no frames in flight).
    void defer(std::function<void()> fn) {
        if (external_) {
            frames_.retire(std::move(fn));
        } else {
            ctx_.waitIdle();
            fn();
        }
    }

    const DeviceCaps& caps() const override { return caps_; }
    std::uint32_t validationErrors() const override { return ctx_.validationErrors(); }

    // ---- GameDevice (rhi/game_device.h) ----
    std::uint32_t refShift() const override { return 0; }  // a GpuRef is the buffer device address
    ShaderModule createShaderModule(const Shader& code, const char* name) override {
        if (code.spirv.empty()) return ShaderModule::None;
        return vk::rhiModule(vk::createShaderModule(ctx_, code.spirv, name != nullptr ? std::string_view(name) : std::string_view()));
    }
    void destroyShaderModule(ShaderModule module, bool deferred) override {
        const VkShaderModule m = vk::vkModule(module);
        if (m == VK_NULL_HANDLE) return;
        if (deferred) {
            vk::Context* ctx = &ctx_;
            defer([ctx, m] { vkDestroyShaderModule(ctx->device(), m, nullptr); });
        } else {
            vkDestroyShaderModule(ctx_.device(), m, nullptr);
        }
    }

    // ---- GameDevice: capabilities, command lists, rings, meshes, transients (rhi/game_device.h) ----
    bool formatSupport(VkFormat format, VkImageUsageFlags usage, bool blend) const override {
        VkFormatProperties fp{};
        vkGetPhysicalDeviceFormatProperties(ctx_.physicalDevice(), format, &fp);
        VkFormatFeatureFlags need = 0;
        if ((usage & VK_IMAGE_USAGE_SAMPLED_BIT) != 0) need |= VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT;
        if ((usage & VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT) != 0) need |= VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT;
        if (blend) need |= VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BLEND_BIT;
        if ((usage & VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT) != 0) need |= VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT;
        if ((usage & VK_IMAGE_USAGE_STORAGE_BIT) != 0) need |= VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT;
        if ((usage & VK_IMAGE_USAGE_TRANSFER_SRC_BIT) != 0) need |= VK_FORMAT_FEATURE_BLIT_SRC_BIT;
        return (fp.optimalTilingFeatures & need) == need;
    }

    CommandList& frameCommands() override { return listFor(mainList_, frames_.cmd(), false); }
    CommandList& passCommands() override { return listFor(mainList_, frames_.cmd(), true); }
    CommandList& uploadCommands() override {
        VkCommandBuffer cmd = frames_.uploadCmd();
        if (!uploadUsed_ && ownsGame_) {
            // the upload buffer runs between the previous frame's draws and this frame's: order it after all earlier work
            VkMemoryBarrier2 b{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
            b.srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
            b.srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT;
            b.dstStageMask = VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT;
            b.dstAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT | VK_ACCESS_2_TRANSFER_READ_BIT;
            VkDependencyInfo di{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
            di.memoryBarrierCount = 1;
            di.pMemoryBarriers = &b;
            vkCmdPipelineBarrier2(cmd, &di);
            uploadUsed_ = true;
            if (gpuTimer_.enabled()) gpuTimer_.mark(cmd, frames_.currentSlot(), vk::GpuTimer::Stream::Upload, "upload begin");
        }
        return listFor(uploadList_, cmd, false);
    }
    void* nativeCommandBuffer(CommandList& list) override;

    RingAlloc allocRingSlow(std::uint64_t size, std::uint64_t alignment, std::uint64_t minOffset) override {
        return frames_.allocate(size, alignment, minOffset);  // (switches ring_'s chunk)
    }
    RingAlloc allocCapture(std::uint64_t size, std::uint64_t alignment) override { return frames_.allocate(size, alignment, 0); }
    Buffer& ringBuffer(const RingAlloc& alloc) override;
    std::uint64_t packetHandle(Buffer& buffer) const override;
    std::uint64_t completedFrame() const override { return frames_.completedValue(); }
    void waitFrame(std::uint64_t value) override { frames_.waitFor(value); }
    void retire(std::function<void()> fn) override { frames_.retire(std::move(fn)); }

    MeshAlloc allocMesh(std::uint64_t size, std::uint32_t multiple) override;
    void freeMesh(const MeshAlloc& alloc, bool deferred) override;

    Texture* transient(std::string_view key, const TransientDesc& desc) override;
    void releaseTransient(std::string_view key) override {
        auto it = transients_.find(std::string(key));
        if (it != transients_.end()) transients_.erase(it);  // (the texture's destructor defers the destruction)
    }
    std::uint64_t transientBytes() const override {
        std::uint64_t n = 0;
        for (const auto& [k, e] : transients_) n += std::uint64_t{e.desc.extent.width} * e.desc.extent.height * std::max(1u, formatBytes(e.desc.format));
        return n;
    }

    // ---- GPU timestamps ([debug] perf): the renderer's query pool over the frame's two command streams ----
    bool enableTimestamps() override { return gpuTimer_.create(ctx_, rhi::kFramesInFlight); }
    bool timestampsEnabled() const override { return gpuTimer_.enabled(); }
    void timestampStart(std::uint32_t slot, bool details) override { gpuTimer_.start(slot, details); }
    bool timestampDetails(std::uint32_t slot) const override { return gpuTimer_.details(slot); }
    void timestampMark(CommandList& list, std::uint32_t slot, TimestampStream stream, const char* label) override {
        gpuTimer_.mark(vulkan::commandBufferOf(list), slot,
                       stream == TimestampStream::Main ? vk::GpuTimer::Stream::Main : vk::GpuTimer::Stream::Upload, label);
    }
    void timestampMark(CommandList& list, std::uint32_t slot, TimestampStream stream, std::string&& label) override {
        gpuTimer_.mark(vulkan::commandBufferOf(list), slot,
                       stream == TimestampStream::Main ? vk::GpuTimer::Stream::Main : vk::GpuTimer::Stream::Upload, std::move(label));
    }
    TimestampResult timestampCollect(std::uint32_t slot) override {
        const vk::GpuTimer::Result r = gpuTimer_.collect(slot);
        TimestampResult out;
        out.valid = r.valid;
        out.mainMs = r.mainMs;
        out.uploadMs = r.uploadMs;
        for (const vk::GpuTimer::Segment& s : r.segments) out.segments.push_back({s.label, s.ms});
        return out;
    }

    // ---- the draw path (pipeline cache, scopes, record threads): the recording itself lives in vk::GameRecord ----
    vk::GameRecordHolder record_;  // (vk::GameRecord; declared here so the ctor can init it after ctx_/frames_/bindless_)
    bool createPipelineCaches(const std::filesystem::path& cacheFile, bool libraries) override {
        return record_.create(ctx_, bindless_.pipelineLayout(), cacheFile, libraries);
    }
    GamePipeline resolvePipeline(const PipelineDesc& desc, bool* created) override { return record_.resolvePipeline(desc, created); }
    GamePipeline findPipeline(const PipelineDesc& desc) override { return record_.findPipeline(desc); }
    bool pipelineContains(const PipelineDesc& desc) override { return record_.containsPipeline(desc); }
    void prewarmPipeline(const PipelineDesc& desc) override { record_.prewarmPipeline(desc); }
    void prewarmOncePipeline(const PipelineDesc& desc) override { record_.prewarmOncePipeline(desc); }
    void preparePipelineShader(ShaderModule module, VkShaderStageFlagBits stage) override { record_.prepareShader(module, stage); }
    void savePipelineCache() override { record_.saveCache(); }
    PipelineStats pipelineStats() override { return record_.stats(); }
    void beginScope(const RenderingInfo& info, bool secondary) override { record_.beginScope(info, secondary); }
    void endScope() override { record_.endScope(); }
    void scopeChunk(rhi::DrawChunk&& chunk) override { record_.scopeChunk(std::move(chunk)); }
    void applyDrawPacket(const DrawPacket& packet) override { record_.applyDrawPacket(packet); }
    void scopeLabel(bool begin, const char* text) override { record_.scopeLabel(begin, text); }
    void invalidateRecordingState() override { record_.invalidateRecordingState(); }
    std::uint64_t takePipelineBinds() override { return record_.takePipelineBinds(); }
    void resetRecordPools() override { record_.resetRecordPools(frames_.currentSlot()); }

    void probeFrameGeneration(const std::filesystem::path& loaderDll) override { ctx_.setFrameGenReport(vk::probeFrameGeneration(ctx_, loaderDll)); }
    caps::Report capabilityReport(bool pipelineLibraries) override { return ctx_.capabilities(pipelineLibraries); }
    void runRaytracingSelfTest() override {
        const vk::rt::SelfTestResult t = vk::rt::selfTest(ctx_);
        if (!t.ran) {
            spdlog::info("rt selftest: skipped: {}", t.detail);
        } else {
            spdlog::log(t.pass ? spdlog::level::info : spdlog::level::err, "rt selftest: {} ({:.1f} ms): {}", t.pass ? "PASS" : "FAIL", t.milliseconds,
                        t.detail);
        }
    }
    bool videoMemory(std::uint64_t& used, std::uint64_t& budget) override {
        const VkPhysicalDeviceMemoryProperties* mp = nullptr;
        vmaGetMemoryProperties(ctx_.allocator(), &mp);
        VmaBudget budgets[VK_MAX_MEMORY_HEAPS]{};
        vmaGetHeapBudgets(ctx_.allocator(), budgets);
        used = budget = 0;
        for (std::uint32_t h = 0; h < mp->memoryHeapCount; ++h) {
            if ((mp->memoryHeaps[h].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) == 0) continue;
            used += budgets[h].usage;
            budget += budgets[h].budget;
        }
        return budget > 0;
    }
    void logMemoryStats() override {
        const VkPhysicalDeviceMemoryProperties* mp = nullptr;
        vmaGetMemoryProperties(ctx_.allocator(), &mp);
        VmaBudget budgets[VK_MAX_MEMORY_HEAPS]{};
        vmaGetHeapBudgets(ctx_.allocator(), budgets);
        std::uint64_t blocks = 0, allocs = 0, count = 0, local = 0;
        for (std::uint32_t h = 0; h < mp->memoryHeapCount; ++h) {
            blocks += budgets[h].statistics.blockBytes;
            allocs += budgets[h].statistics.allocationBytes;
            count += budgets[h].statistics.allocationCount;
            if ((mp->memoryHeaps[h].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) != 0) local += budgets[h].statistics.blockBytes;
        }
        spdlog::info("perf: GPU memory {:.1f} MiB in blocks ({:.1f} MiB device-local), {:.1f} MiB in {} allocations; ray tracing tier {}",
                     static_cast<double>(blocks) / 1048576.0, static_cast<double>(local) / 1048576.0, static_cast<double>(allocs) / 1048576.0, count,
                     ctx_.features().rayTracing.enabled ? "on" : "off");
        if (arena_ != nullptr) {
            const vk::MeshArena::Stats a = arena_->stats();
            spdlog::info("perf: mesh arena {} blocks of {} MiB + {} dedicated buffers, {:.1f} MiB used", a.blocks, vk::MeshArena::kBlockSize >> 20,
                         a.dedicated, static_cast<double>(a.used) / 1048576.0);
        }
    }

    std::unique_ptr<Upscaler> createUpscaler() override { return vulkan::createUpscaler(*this); }
    std::unique_ptr<FrameGenerator> createFrameGenerator() override { return vulkan::createFrameGenerator(*this); }
    std::unique_ptr<ImGuiBackend> createImGuiBackend() override { return vulkan::createImGuiBackend(*this); }

    vk::MeshArena* arena() { return arena_; }

    // ---- the frame loop ----
    void beginFrame() override {
        frames_.begin();
        if (!bindlessInit_) {
            bindless_.recordInit(frames_.cmd());
            bindlessInit_ = true;
        }
        frameValue_ = frames_.currentValue();
        frameSlot_ = frames_.currentSlot();
        uploadUsed_ = false;
        record_.resetRecordPools(frames_.currentSlot());  // a new frame slot: its secondary command buffer pools are reset
    }
    double lastSlotWaitMs() const override { return frames_.lastSlotWaitMs(); }
    void submitFrame(const char* finalMark) override {
        if (uploadUsed_) {
            VkMemoryBarrier2 b{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
            b.srcStageMask = VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT;
            b.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
            b.dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
            b.dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT;
            VkDependencyInfo di{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
            di.memoryBarrierCount = 1;
            di.pMemoryBarriers = &b;
            vkCmdPipelineBarrier2(frames_.uploadCmd(), &di);
            if (gpuTimer_.enabled()) gpuTimer_.mark(frames_.uploadCmd(), frames_.currentSlot(), vk::GpuTimer::Stream::Upload, "uploads");
        }
        if (gpuTimer_.enabled()) gpuTimer_.mark(frames_.cmd(), frames_.currentSlot(), vk::GpuTimer::Stream::Main, finalMark);
        VkSemaphore wait = VK_NULL_HANDLE, signal = VK_NULL_HANDLE;
        if (presenter_) presenter_->takeSubmitSync(wait, signal);
        frames_.submit(wait, signal);
        frameValue_ = frames_.currentValue();
        frameSlot_ = frames_.currentSlot();
    }
    void waitIdle() override { frames_.waitIdle(); }
    bool namesEnabled() const override { return ctx_.namesEnabled(); }

    SamplerIndex createGameSampler(const GameSamplerDesc& desc, const std::string& name) override {
        VkSamplerCreateInfo ci{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
        ci.magFilter = desc.magFilter;
        ci.minFilter = desc.minFilter;
        ci.mipmapMode = desc.mipmap;
        ci.addressModeU = desc.addressU;
        ci.addressModeV = desc.addressV;
        ci.addressModeW = desc.addressW;
        ci.mipLodBias = desc.lodBias;
        const bool aniso = desc.maxAnisotropy > 1.0f && ctx_.features().samplerAnisotropy;
        ci.anisotropyEnable = aniso ? VK_TRUE : VK_FALSE;
        ci.maxAnisotropy = aniso ? std::clamp(desc.maxAnisotropy, 1.0f, ctx_.features().maxAnisotropy) : 1.0f;
        ci.minLod = desc.minLod;
        ci.maxLod = desc.singleLevel ? ci.minLod : VK_LOD_CLAMP_NONE;
        ci.borderColor = desc.border;
        VkSampler sampler = VK_NULL_HANDLE;
        if (vkCreateSampler(ctx_.device(), &ci, nullptr, &sampler) != VK_SUCCESS) return 0;
        samplers_.push_back(sampler);
        const std::uint32_t index = bindless_.addSampler(sampler);
        if (ctx_.namesEnabled() && !name.empty()) {
            vk::names::object(ctx_, sampler, VK_OBJECT_TYPE_SAMPLER, std::format("sampler [slot {}] {}", index, name));
        }
        return index;
    }
    void clearAttachments(const ClearOp& op) override { vkCmdClearAttachments(frames_.cmd(), op.count, op.atts, 1, &op.rect); }

    // ---- debug regions and crash markers ----
    bool regionsAvailable() const override {
        return ctx_.debugUtilsAvailable() && vkCmdBeginDebugUtilsLabelEXT != nullptr && vkCmdEndDebugUtilsLabelEXT != nullptr;
    }
    void labelBegin(const char* name) override {
        // a colour per name (stable): the label's hue from a hash, so neighbouring regions differ
        std::uint32_t h = 2166136261u;
        for (const char* c = name; *c != 0; ++c) h = (h ^ static_cast<std::uint8_t>(*c)) * 16777619u;
        const float hue = static_cast<float>(h % 360u) / 60.0f;
        const float x = 1.0f - std::fabs(std::fmod(hue, 2.0f) - 1.0f);
        const float rgb[6][3] = {{1, x, 0}, {x, 1, 0}, {0, 1, x}, {0, x, 1}, {x, 0, 1}, {1, 0, x}};
        const float* c = rgb[static_cast<int>(hue) % 6];
        VkDebugUtilsLabelEXT l{VK_STRUCTURE_TYPE_DEBUG_UTILS_LABEL_EXT};
        l.pLabelName = name;
        l.color[0] = 0.35f + 0.5f * c[0];
        l.color[1] = 0.35f + 0.5f * c[1];
        l.color[2] = 0.35f + 0.5f * c[2];
        l.color[3] = 1.0f;
        vkCmdBeginDebugUtilsLabelEXT(frames_.cmd(), &l);
    }
    void labelEnd() override { vkCmdEndDebugUtilsLabelEXT(frames_.cmd()); }
    bool setCrashMarkers(bool on) override {
        if (!on) {
            ctx_.setLostReporter(nullptr);
            return true;
        }
        if (!crashMarkers_.create(ctx_)) return false;
        ctx_.setLostReporter([this]() { return crashMarkers_.report(); });
        return true;
    }
    void crashMarker(bool begin, const char* text) override {
        if (begin) {
            crashMarkers_.begin(frames_.cmd(), frameValue_, text);
        } else {
            crashMarkers_.end(frames_.cmd(), frameValue_, text);
        }
    }
    GamePresenter& presenter() override { return *presenter_; }

    std::unique_ptr<Buffer> createBuffer(const BufferDesc& desc) override {
        auto b = std::make_unique<VkBufferImpl>(*this, desc);
        if (!b->create()) return nullptr;
        return b;
    }
    std::unique_ptr<Texture> createTexture(const TextureDesc& desc) override {
        auto t = std::make_unique<VkTextureImpl>(*this, desc);
        if (!t->create()) return nullptr;
        return t;
    }
    SamplerIndex createSampler(const SamplerDesc& desc) override {
        VkSamplerCreateInfo ci{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
        ci.magFilter = ci.minFilter = desc.filter;
        ci.mipmapMode = desc.filter == VK_FILTER_NEAREST ? VK_SAMPLER_MIPMAP_MODE_NEAREST : VK_SAMPLER_MIPMAP_MODE_LINEAR;
        ci.addressModeU = ci.addressModeV = ci.addressModeW = desc.address;
        ci.maxLod = VK_LOD_CLAMP_NONE;
        if (desc.maxAnisotropy > 1.0f && desc.filter == VK_FILTER_LINEAR && ctx_.features().samplerAnisotropy) {
            ci.anisotropyEnable = VK_TRUE;
            ci.maxAnisotropy = std::min(desc.maxAnisotropy, ctx_.features().maxAnisotropy);
        }
        VkSampler s = VK_NULL_HANDLE;
        if (vkCreateSampler(ctx_.device(), &ci, nullptr, &s) != VK_SUCCESS) return 0;
        samplers_.push_back(s);
        return bindless_.addSampler(s);
    }
    std::unique_ptr<Pipeline> createComputePipeline(const ComputePipelineDesc& desc) override {
        VkPipeline p = vk::createComputePipeline(ctx_, layout_.layout(), desc.cs.spirv, desc.name != nullptr ? desc.name : "rhi compute");
        if (p == VK_NULL_HANDLE) return nullptr;
        return std::make_unique<VkPipelineImpl>(*this, p, VK_PIPELINE_BIND_POINT_COMPUTE);
    }
    std::unique_ptr<Pipeline> createGraphicsPipeline(const GraphicsPipelineDesc& desc) override {
        vk::PassGraphicsDesc g;
        g.vs = desc.vs.spirv;
        g.fs = desc.ps.spirv;
        g.color = desc.colorFormats[0];
        for (std::size_t i = 0; i < g.moreColors.size(); ++i) g.moreColors[i] = desc.colorFormats[i + 1];
        g.blend.enable = desc.blend.blendEnable;
        g.blend.srcColor = desc.blend.srcColorBlendFactor;
        g.blend.dstColor = desc.blend.dstColorBlendFactor;
        g.blend.colorOp = desc.blend.colorBlendOp;
        g.blend.srcAlpha = desc.blend.srcAlphaBlendFactor;
        g.blend.dstAlpha = desc.blend.dstAlphaBlendFactor;
        g.blend.alphaOp = desc.blend.alphaBlendOp;
        g.blend.writeMask = desc.blend.colorWriteMask != 0 ? desc.blend.colorWriteMask : 0xF;
        g.depth = desc.depthFormat;
        g.depthTest = desc.depthTest;
        g.depthWrite = desc.depthWrite;
        g.depthOp = desc.depthOp;
        g.cull = desc.cull;
        g.frontFace = desc.frontFace;
        g.topology = desc.topology;
        g.vertexBindings = desc.vertexBindings;
        g.vertexAttributes = desc.vertexAttributes;
        g.dynamicVertexStride = !desc.vertexBindings.empty();  // CommandList::setVertexBuffers supplies the strides
        g.name = desc.name != nullptr ? desc.name : "rhi graphics";
        VkPipeline p = vk::createPassGraphicsPipeline(ctx_, layout_.layout(), g);
        if (p == VK_NULL_HANDLE) return nullptr;
        return std::make_unique<VkPipelineImpl>(*this, p, VK_PIPELINE_BIND_POINT_GRAPHICS);
    }

    std::unique_ptr<AccelerationStructure> createBlas(const BlasDesc& desc) override {
        if (!caps_.rayQuery) return nullptr;
        auto a = std::make_unique<VkAccelImpl>(*this, false);
        if (!a->createBlas(desc)) return nullptr;
        return a;
    }
    std::unique_ptr<AccelerationStructure> createTlas(const TlasDesc& desc) override {
        if (!caps_.rayQuery) return nullptr;
        auto a = std::make_unique<VkAccelImpl>(*this, true);
        if (!a->createTlas(desc)) return nullptr;
        return a;
    }
    std::unique_ptr<AccelerationStructure> createBlasStorage(std::uint64_t bytes, const char* name) override {
        if (!caps_.rayQuery) return nullptr;
        auto a = std::make_unique<VkAccelImpl>(*this, false);
        if (!a->createStorage(bytes, name != nullptr ? name : "rhi blas (compacted)")) return nullptr;
        return a;
    }
    std::unique_ptr<SizeQuery> createSizeQuery(std::uint32_t capacity) override {
        if (!caps_.rayQuery) return nullptr;
        auto q = std::make_unique<VkSizeQueryImpl>(*this, capacity);
        if (!q->create()) return nullptr;
        return q;
    }
    std::uint64_t buildScratchBytes() const override { return scratch_.size; }

    // The shared scratch pool of BLAS builds: at least `bytes` (grown by replacing it; the old one goes through the device's deferred destruction, the
    // builds already recorded keep using it). Offsets handed out are multiples of scratchAlign().
    bool ensureScratch(VkDeviceSize bytes) {
        if (scratch_.buffer != VK_NULL_HANDLE && scratch_.size >= bytes) return true;
        if (scratch_.buffer != VK_NULL_HANDLE) {
            retireBuffer(scratch_);
            scratch_ = {};
        }
        const VkDeviceSize minimum = external_ ? (32ull << 20) : (1ull << 20);
        return scratch_.create(ctx_, std::max(bytes, minimum), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, false, scratchAlign(), "rhi build scratch");
    }
    VkDeviceSize scratchAlign() const { return std::max<VkDeviceSize>(ctx_.features().rayTracing.minScratchOffsetAlignment, 16); }
    const vk::rt::AddressBuffer& scratch() const { return scratch_; }
    // a buffer the GPU may still use: destroyed with the frame being recorded (wrapped) / with the device (owning: nothing waits mid-recording)
    void retireBuffer(vk::rt::AddressBuffer buf) {
        if (external_) {
            vk::Context* c = &ctx_;
            frames_.retire([c, buf]() mutable { buf.destroy(*c); });
        } else {
            graveyard_.push_back(buf);
        }
    }

    void submitAndWait(const std::function<void(CommandList&)>& record) override {
        if (external_) {
            spdlog::error("rhi: submitAndWait is not available on a wrapped device (the renderer submits its frames)");
            return;
        }
        frames_.begin();
        VkCommandBuffer cmd = frames_.cmd();
        if (!bindlessInit_) {
            bindless_.recordInit(cmd);
            bindlessInit_ = true;
        }
        VkCommandListImpl list(*this, cmd);
        record(list);
        frames_.submit(VK_NULL_HANDLE, VK_NULL_HANDLE);
        frames_.waitIdle();
    }

    std::unique_ptr<Swapchain> createSwapchain(const SwapchainDesc& desc) override {
        if (external_) return nullptr;  // the renderer owns its swapchain
        auto s = std::make_unique<VkSwapchainImpl>(*this);
        if (!s->create(desc)) return nullptr;
        return s;
    }
    bool renderAndPresent(Swapchain& swapchain, const std::function<void(CommandList&, Texture&)>& record) override {
        if (external_) return false;
        auto& sw = static_cast<VkSwapchainImpl&>(swapchain);
        std::uint32_t index = 0;
        if (!sw.swapchain().acquire(sw.acquired(), index)) return false;
        VkTextureImpl& back = sw.image(index);
        back.image().discard();  // the contents are undefined after the acquire
        frames_.begin();
        VkCommandBuffer cmd = frames_.cmd();
        if (!bindlessInit_) {
            bindless_.recordInit(cmd);
            bindlessInit_ = true;
        }
        VkCommandListImpl list(*this, cmd);
        record(list, back);
        back.image().transition(cmd, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR);
        frames_.submit(sw.acquired(), sw.swapchain().renderedSemaphore(index));
        sw.swapchain().present(index);
        frames_.waitIdle();
        return true;
    }

    vk::Context& ctx() { return ctx_; }
    vk::Bindless& bindless() { return bindless_; }
    vk::PassLayout& layout() { return layout_; }
    // the bindless acceleration structure array's slots are the renderer's (vk::Bindless::allocTlasSlot: shared with the RT scene's TLASes);
    // kMaxTlas = none left
    std::uint32_t allocTlasSlot() { return bindless_.allocTlasSlot(); }
    void freeTlasSlot(std::uint32_t slot) { bindless_.freeTlasSlot(slot); }

private:
    vk::Context ownCtx_;  // (unused when wrapped)
    vk::Frames ownFrames_;
    vk::Bindless ownBindless_;
    vk::MeshArena ownArena_;  // (the game device only)
    vk::Context& ctx_;
    vk::Frames& frames_;
    vk::Bindless& bindless_;
    bool external_ = false;  // retires through the frames instead of waiting (wrapped and game devices)
    bool ownsGame_ = false;  // the game device owns the renderer's objects (createGameDevice)
    bool uploadUsed_ = false;
    VkBuffer nullVertexVk_ = VK_NULL_HANDLE;
    VmaAllocation nullVertexAlloc_ = VK_NULL_HANDLE;
    vk::CrashMarkers crashMarkers_;
    std::unique_ptr<vulkan::VkGamePresenter> presenter_;
    vk::PassLayout layout_;
    DeviceCaps caps_;
    bool bindlessInit_ = false;
    vk::MeshArena* arena_ = nullptr;
    vk::GpuTimer gpuTimer_;  // [debug] perf timestamps (enableTimestamps creates it)
    std::unique_ptr<VkCommandListImpl> mainList_, uploadList_;  // the frame's persistent lists (frameCommands / uploadCommands)
    CommandList& listFor(std::unique_ptr<VkCommandListImpl>& slot, VkCommandBuffer cmd, bool reset) {
        if (!slot) {
            slot = std::make_unique<VkCommandListImpl>(*this, cmd);
        } else if (slot->cmd() != cmd || reset) {
            slot->rebind(cmd);
        }
        return *slot;
    }
    // wrappers of buffers the renderer owns: the ring's chunks (never freed before the frames) and the mesh arena's buffers (blocks live as long as the arena;
    // a dedicated buffer's wrapper is re-initialised when its handle comes back)
    std::mutex wrapMutex_;
    std::unordered_map<std::uint64_t, std::unique_ptr<VkBufferImpl>> ringBuffers_, meshBuffers_;
    struct TransientEntry {
        std::unique_ptr<VkTextureImpl> tex;
        TransientDesc desc;
    };
    std::unordered_map<std::string, TransientEntry> transients_;
    std::vector<VkSampler> samplers_;
    vk::rt::AddressBuffer scratch_;               // the BLAS build scratch pool
    std::vector<vk::rt::AddressBuffer> graveyard_;  // replaced pools of an owning device
};

// ---- swapchain --------------------------------------------------------------------------------------------------------------

bool VkSwapchainImpl::create(const SwapchainDesc& desc) {
    if (!sc_.create(dev_.ctx(), static_cast<HWND__*>(desc.window), desc.width, desc.height, desc.vsync)) return false;
    VkSemaphoreCreateInfo si{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    if (vkCreateSemaphore(dev_.ctx().device(), &si, nullptr, &acquired_) != VK_SUCCESS) return false;
    wrapImages();
    return true;
}

void VkSwapchainImpl::wrapImages() {
    images_.clear();
    TextureDesc td;
    td.format = sc_.format();
    td.extent = {sc_.extent().width, sc_.extent().height, 1};
    td.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    td.name = "rhi swapchain image";
    for (std::uint32_t i = 0; i < sc_.imageCount(); ++i) {
        auto t = std::make_unique<VkTextureImpl>(dev_, td);
        t->adopt(sc_.image(i), sc_.view(i));
        images_.push_back(std::move(t));
    }
}

bool VkSwapchainImpl::resize(std::uint32_t w, std::uint32_t h) {
    dev_.ctx().waitIdle();
    images_.clear();
    if (!sc_.recreate(w, h)) return false;
    wrapImages();
    return true;
}

VkSwapchainImpl::~VkSwapchainImpl() {
    dev_.ctx().waitIdle();
    images_.clear();
    if (acquired_ != VK_NULL_HANDLE) vkDestroySemaphore(dev_.ctx().device(), acquired_, nullptr);
    sc_.destroy();
}

// ---- buffers / textures -----------------------------------------------------------------------------------------------------

bool VkBufferImpl::create() {
    VkBufferCreateInfo bci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bci.size = desc_.size;
    bci.usage = desc_.usage | VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;
    // vertex, index and instance data of acceleration structure builds (DeviceDesc::rayTracing)
    if (dev_.ctx().features().rayTracing.enabled) bci.usage |= VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR;
    VmaAllocationCreateInfo aci{};
    aci.priority = 0.5f;  // VK_EXT_memory_priority: the default (a zero-initialised priority is the lowest)
    aci.usage = VMA_MEMORY_USAGE_AUTO;
    if (desc_.memory == Memory::Upload) {
        aci.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;
    } else if (desc_.memory == Memory::Readback) {
        aci.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;
    }
    VmaAllocationInfo info{};
    // acceleration structure inputs (instance records: 16 bytes) and scratch want aligned addresses; with ray tracing every buffer starts on 256 bytes
    const VkResult r = dev_.ctx().features().rayTracing.enabled
                           ? vmaCreateBufferWithAlignment(dev_.ctx().allocator(), &bci, &aci, 256, &buffer_, &alloc_, &info)
                           : vmaCreateBuffer(dev_.ctx().allocator(), &bci, &aci, &buffer_, &alloc_, &info);
    if (r != VK_SUCCESS) return false;
    mapped_ = info.pMappedData;
    VkBufferDeviceAddressInfo ai{VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO};
    ai.buffer = buffer_;
    address_ = vkGetBufferDeviceAddress(dev_.ctx().device(), &ai);
    return true;
}

void VkBufferImpl::invalidate() {
    if (mapped_ != nullptr) vmaInvalidateAllocation(dev_.ctx().allocator(), alloc_, 0, VK_WHOLE_SIZE);
}

void VkBufferImpl::flush() {
    if (mapped_ != nullptr) vmaFlushAllocation(dev_.ctx().allocator(), alloc_, 0, VK_WHOLE_SIZE);
}

VkBufferImpl::~VkBufferImpl() {
    if (buffer_ == VK_NULL_HANDLE || external_) return;
    VmaAllocator allocator = dev_.ctx().allocator();
    const VkBuffer buffer = buffer_;
    const VmaAllocation alloc = alloc_;
    dev_.defer([allocator, buffer, alloc] { vmaDestroyBuffer(allocator, buffer, alloc); });
}

VkPipelineImpl::~VkPipelineImpl() {
    if (pipeline_ == VK_NULL_HANDLE) return;
    VkDevice device = dev_.ctx().device();
    const VkPipeline p = pipeline_;
    dev_.defer([device, p] { vkDestroyPipeline(device, p, nullptr); });
}

bool VkTextureImpl::create() {
    if (desc_.cube) {  // a cube map is a 2D image of 6 layers (the faces)
        desc_.type = VK_IMAGE_TYPE_2D;
        desc_.layers = 6;
        desc_.extent.depth = 1;
    }
    vk::ImageDesc d;
    d.type = desc_.type;
    d.format = desc_.format;
    d.extent = desc_.extent;
    d.mips = desc_.mips;
    d.layers = desc_.layers;
    d.cube = desc_.cube;
    d.usage = desc_.usage | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    d.swizzle = desc_.swizzle;
    d.name = desc_.name;
    if (!image_->create(dev_.ctx(), d)) return false;
    if ((desc_.usage & VK_IMAGE_USAGE_SAMPLED_BIT) != 0) {
        // (an array has no whole-array kind in the bindless set: heapSampled is the view of layer 0)
        slot_ = isArray() ? heapSampledLayer(0) : dev_.bindless().addTexture(kind(), image_->view());
    }
    return true;
}

VkImageView VkTextureImpl::view(VkImageViewType type, VkImageAspectFlags aspect, std::uint32_t baseMip, std::uint32_t mipCount, std::uint32_t baseLayer,
                                std::uint32_t layerCount) {
    const std::array<std::uint32_t, 6> key{static_cast<std::uint32_t>(type), aspect, baseMip, mipCount, baseLayer, layerCount};
    auto it = views_.find(key);
    if (it != views_.end()) return it->second;
    VkImageViewCreateInfo vci{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    vci.image = image_->image();
    vci.viewType = type;
    vci.format = desc_.format;
    vci.subresourceRange = {aspect, baseMip, mipCount, baseLayer, layerCount};
    VkImageView v = VK_NULL_HANDLE;
    if (vkCreateImageView(dev_.ctx().device(), &vci, nullptr, &v) != VK_SUCCESS) {
        spdlog::error("rhi: vkCreateImageView failed for '{}'", desc_.name != nullptr ? desc_.name : "?");
        return VK_NULL_HANDLE;
    }
    views_[key] = v;
    return v;
}

std::uint32_t VkTextureImpl::heapSampledLayer(std::uint32_t layer) {
    if ((desc_.usage & VK_IMAGE_USAGE_SAMPLED_BIT) == 0 || layer >= desc_.layers) return 0;
    if (!isArray()) return layer == 0 ? slot_ : 0;
    if (layerSlots_.empty()) layerSlots_.assign(desc_.layers, 0);
    if (layerSlots_[layer] == 0) {
        VkImageView v = view(VK_IMAGE_VIEW_TYPE_2D, vk::isDepthFormat(desc_.format) ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT, 0, desc_.mips,
                             layer, 1);
        if (v != VK_NULL_HANDLE) layerSlots_[layer] = dev_.bindless().addTexture(vk::TextureKind::Tex2D, v);
    }
    return layerSlots_[layer];
}

void VkTextureImpl::setDebugName(const std::string& name) {
    if (!image_->valid() || !dev_.ctx().namesEnabled()) return;
    vk::names::image(dev_.ctx(), *image_, name);
}

VkImageView VkTextureImpl::attachmentView(std::uint32_t mip, std::uint32_t layer) {
    // a wrapped image: the image's own attachment views (identity swizzle, created once)
    if (external_) return image_->attachmentView(dev_.ctx(), mip, layer);
    // a swizzled sampled view (X8R8G8B8's alpha = 1, L8) cannot be an attachment: the image's own identity views
    const VkComponentMapping& sw = image_->desc().swizzle;
    if (sw.r != VK_COMPONENT_SWIZZLE_IDENTITY || sw.g != VK_COMPONENT_SWIZZLE_IDENTITY || sw.b != VK_COMPONENT_SWIZZLE_IDENTITY ||
        sw.a != VK_COMPONENT_SWIZZLE_IDENTITY) {
        return image_->attachmentView(dev_.ctx(), mip, layer);
    }
    if (plain2D() || (mip == 0 && layer == 0 && desc_.mips == 1 && desc_.layers == 1)) return image_->view();
    return view(VK_IMAGE_VIEW_TYPE_2D, image_->aspect(), mip, 1, layer, 1);
}

VkImageView VkTextureImpl::storageView(std::uint32_t mip, std::uint32_t layer) {
    if (desc_.type == VK_IMAGE_TYPE_3D) return view(VK_IMAGE_VIEW_TYPE_3D, VK_IMAGE_ASPECT_COLOR_BIT, mip, 1, 0, 1);
    if (desc_.layers > 1) return view(VK_IMAGE_VIEW_TYPE_2D_ARRAY, VK_IMAGE_ASPECT_COLOR_BIT, mip, 1, layer, 1);
    const VkComponentMapping& sw = image_->desc().swizzle;
    const bool identity = sw.r == VK_COMPONENT_SWIZZLE_IDENTITY && sw.g == VK_COMPONENT_SWIZZLE_IDENTITY && sw.b == VK_COMPONENT_SWIZZLE_IDENTITY &&
                          sw.a == VK_COMPONENT_SWIZZLE_IDENTITY;
    if (plain2D() && (identity || !external_)) return image_->view();
    return view(VK_IMAGE_VIEW_TYPE_2D, VK_IMAGE_ASPECT_COLOR_BIT, mip, 1, 0, 1);
}

VkTextureImpl::~VkTextureImpl() {
    if (slot_ != 0 && !isArray() && !external_) dev_.bindless().releaseTexture(kind(), slot_);
    for (std::uint32_t s : layerSlots_) {
        if (s != 0) dev_.bindless().releaseTexture(vk::TextureKind::Tex2D, s);
    }
    if (!image_->valid() || (external_ && views_.empty())) return;
    // the GPU may still use the image and the views: destroyed when it is done (a wait on an owning device, the frame retirement on a wrapped one)
    vk::Context* ctx = &dev_.ctx();
    std::vector<VkImageView> views;
    for (auto& [key, v] : views_) views.push_back(v);
    vk::Image img = *image_;  // (raw handles; destroy() frees them)
    const bool owned = !external_;
    dev_.defer([ctx, views = std::move(views), img, owned]() mutable {
        for (VkImageView v : views) vkDestroyImageView(ctx->device(), v, nullptr);
        if (owned) img.destroy(*ctx);
    });
}

// ---- acceleration structures ------------------------------------------------------------------------------------------------

VkAccelImpl::~VkAccelImpl() {
    // the GPU may still use it: destroyed when the frame being recorded has completed (a wait on an owning device)
    vk::Context* ctx = &dev_.ctx();
    vk::Bindless* bindless = &dev_.bindless();
    const VkAccelerationStructureKHR handle = handle_;
    vk::rt::AddressBuffer storage = storage_, scratch = scratch_;
    const std::uint32_t slot = slot_;
    const bool hasSlot = hasSlot_;
    dev_.defer([ctx, bindless, handle, storage, scratch, slot, hasSlot]() mutable {
        if (handle != VK_NULL_HANDLE) vkDestroyAccelerationStructureKHR(ctx->device(), handle, nullptr);
        storage.destroy(*ctx);
        scratch.destroy(*ctx);
        if (hasSlot) bindless->freeTlasSlot(slot);
    });
}

namespace {

// the geometry / ranges of a BLAS build info
void blasGeometries(std::span<const TriangleGeometry> list, std::vector<VkAccelerationStructureGeometryKHR>& geoms,
                    std::vector<VkAccelerationStructureBuildRangeInfoKHR>& ranges, std::vector<std::uint32_t>* counts) {
    for (const TriangleGeometry& t : list) {
        VkAccelerationStructureGeometryKHR g{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR};
        g.geometryType = VK_GEOMETRY_TYPE_TRIANGLES_KHR;
        g.flags = t.opaque ? VK_GEOMETRY_OPAQUE_BIT_KHR : 0;
        auto& tri = g.geometry.triangles;
        tri.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_TRIANGLES_DATA_KHR;
        tri.vertexFormat = t.vertexFormat;
        tri.vertexData.deviceAddress =
            t.vertexAddress != 0 ? t.vertexAddress : (t.vertices != nullptr ? static_cast<VkBufferImpl*>(t.vertices)->address() + t.vertexOffset : 0);
        tri.vertexStride = t.vertexStride;
        tri.maxVertex = t.vertexCount > 0 ? t.vertexCount - 1 : 0;
        if (t.indexed()) {
            tri.indexType = t.indexType;
            tri.indexData.deviceAddress =
                t.indexAddress != 0 ? t.indexAddress : static_cast<VkBufferImpl*>(t.indices)->address() + t.indexOffset;
        } else {
            tri.indexType = VK_INDEX_TYPE_NONE_KHR;
        }
        geoms.push_back(g);
        const std::uint32_t triangles = t.triangleCount();
        ranges.push_back({triangles, 0, 0, 0});
        if (counts != nullptr) counts->push_back(triangles);
    }
}

// build writes -> later builds (a TLAS over a freshly built BLAS) and ray queries in any shader stage
void barrierAfterBuild(VkCommandBuffer cmd) {
    VkMemoryBarrier2 b{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
    b.srcStageMask = VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_BUILD_BIT_KHR;
    b.srcAccessMask = VK_ACCESS_2_ACCELERATION_STRUCTURE_WRITE_BIT_KHR;
    b.dstStageMask = VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_BUILD_BIT_KHR | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT |
                     VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT;
    b.dstAccessMask = VK_ACCESS_2_ACCELERATION_STRUCTURE_READ_BIT_KHR | VK_ACCESS_2_ACCELERATION_STRUCTURE_WRITE_BIT_KHR;
    VkDependencyInfo di{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    di.memoryBarrierCount = 1;
    di.pMemoryBarriers = &b;
    vkCmdPipelineBarrier2(cmd, &di);
}

// Before a build: earlier builds / compaction copies (the scratch is reused, a TLAS reads BLASes) and the ray queries that still read what is
// rewritten (the previous use of a TLAS) are done.
void barrierBeforeBuild(VkCommandBuffer cmd, bool copyStage) {
    const VkPipelineStageFlags2 asStages =
        VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_BUILD_BIT_KHR | (copyStage ? VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_COPY_BIT_KHR : 0);
    VkMemoryBarrier2 b{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
    b.srcStageMask = asStages | VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
    b.srcAccessMask = VK_ACCESS_2_ACCELERATION_STRUCTURE_WRITE_BIT_KHR | VK_ACCESS_2_ACCELERATION_STRUCTURE_READ_BIT_KHR;
    b.dstStageMask = asStages;
    b.dstAccessMask = VK_ACCESS_2_ACCELERATION_STRUCTURE_READ_BIT_KHR | VK_ACCESS_2_ACCELERATION_STRUCTURE_WRITE_BIT_KHR;
    VkDependencyInfo di{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    di.memoryBarrierCount = 1;
    di.pMemoryBarriers = &b;
    vkCmdPipelineBarrier2(cmd, &di);
}

}  // namespace

bool VkAccelImpl::createHandle(VkAccelerationStructureTypeKHR type, VkDeviceSize size, const char* what) {
    vk::Context& ctx = dev_.ctx();
    if (!storage_.create(ctx, size, VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR, false, 256, what)) return false;
    VkAccelerationStructureCreateInfoKHR ci{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_CREATE_INFO_KHR};
    ci.buffer = storage_.buffer;
    ci.size = size;
    ci.type = type;
    if (vkCreateAccelerationStructureKHR(ctx.device(), &ci, nullptr, &handle_) != VK_SUCCESS) {
        spdlog::error("rhi: vkCreateAccelerationStructureKHR ({}) failed", what);
        return false;
    }
    ctx.setName(handle_, VK_OBJECT_TYPE_ACCELERATION_STRUCTURE_KHR, what);
    VkAccelerationStructureDeviceAddressInfoKHR ai{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_DEVICE_ADDRESS_INFO_KHR};
    ai.accelerationStructure = handle_;
    address_ = vkGetAccelerationStructureDeviceAddressKHR(ctx.device(), &ai);
    return true;
}

// sizes -> storage + the acceleration structure object (+ scratch for a TLAS; the build happens in record*)
bool VkAccelImpl::allocate(const VkAccelerationStructureBuildGeometryInfoKHR& info, const std::uint32_t* maxPrimitives, const char* what, bool scratch) {
    vk::Context& ctx = dev_.ctx();
    VkAccelerationStructureBuildSizesInfoKHR sizes{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR};
    vkGetAccelerationStructureBuildSizesKHR(ctx.device(), VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR, &info, maxPrimitives, &sizes);
    buildScratch_ = std::max<VkDeviceSize>(sizes.buildScratchSize, 4);
    updateScratch_ = std::max<VkDeviceSize>(sizes.updateScratchSize, 4);
    if (!createHandle(info.type, sizes.accelerationStructureSize, what)) return false;
    if (scratch && !scratch_.create(ctx, buildScratch_, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, false, dev_.scratchAlign(), "rhi build scratch")) return false;
    return true;
}

bool VkAccelImpl::createBlas(const BlasDesc& desc) {
    geometries_.assign(desc.geometries.begin(), desc.geometries.end());
    allowUpdate_ = desc.allowUpdate;
    flags_ = (desc.preferFastBuild ? VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_BUILD_BIT_KHR : VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR) |
             (desc.allowUpdate ? VK_BUILD_ACCELERATION_STRUCTURE_ALLOW_UPDATE_BIT_KHR : 0) |
             (desc.allowCompaction ? VK_BUILD_ACCELERATION_STRUCTURE_ALLOW_COMPACTION_BIT_KHR : 0);
    std::vector<VkAccelerationStructureGeometryKHR> geoms;
    std::vector<VkAccelerationStructureBuildRangeInfoKHR> ranges;
    std::vector<std::uint32_t> counts;
    blasGeometries(geometries_, geoms, ranges, &counts);
    VkAccelerationStructureBuildGeometryInfoKHR info{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR};
    info.type = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR;
    info.flags = flags_;
    info.mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR;
    info.geometryCount = static_cast<std::uint32_t>(geoms.size());
    info.pGeometries = geoms.data();
    return allocate(info, counts.data(), desc.name != nullptr ? desc.name : "rhi blas", false);
}

bool VkAccelImpl::createStorage(std::uint64_t bytes, const char* name) { return createHandle(VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR, bytes, name); }

bool VkAccelImpl::createTlas(const TlasDesc& desc) {
    maxInstances_ = desc.maxInstances;
    flags_ = desc.preferFastBuild ? VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_BUILD_BIT_KHR : VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR;
    slot_ = dev_.allocTlasSlot();
    if (slot_ >= shader::abi::kMaxTlas) {
        spdlog::error("rhi: no free TLAS slot ({} in the bindless array)", shader::abi::kMaxTlas);
        slot_ = 0;
        return false;
    }
    hasSlot_ = true;
    VkAccelerationStructureGeometryKHR g{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR};
    g.geometryType = VK_GEOMETRY_TYPE_INSTANCES_KHR;
    g.geometry.instances.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_INSTANCES_DATA_KHR;
    VkAccelerationStructureBuildGeometryInfoKHR info{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR};
    info.type = VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR;
    info.flags = flags_;
    info.mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR;
    info.geometryCount = 1;
    info.pGeometries = &g;
    if (!allocate(info, &maxInstances_, desc.name != nullptr ? desc.name : "rhi tlas", true)) return false;
    return dev_.bindless().setTlas(slot_, handle_);
}

void VkAccelImpl::recordBuildTlas(VkCommandBuffer cmd, VkDeviceAddress instances, std::uint32_t count) {
    barrierBeforeBuild(cmd, dev_.ctx().features().rayTracing.maintenance1);
    VkAccelerationStructureGeometryKHR g{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR};
    g.geometryType = VK_GEOMETRY_TYPE_INSTANCES_KHR;
    g.geometry.instances.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_INSTANCES_DATA_KHR;
    g.geometry.instances.arrayOfPointers = VK_FALSE;
    g.geometry.instances.data.deviceAddress = instances;
    VkAccelerationStructureBuildGeometryInfoKHR info{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR};
    info.type = VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR;
    info.flags = flags_;
    info.mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR;
    info.geometryCount = 1;
    info.pGeometries = &g;
    info.dstAccelerationStructure = handle_;
    info.scratchData.deviceAddress = scratch_.address;
    const VkAccelerationStructureBuildRangeInfoKHR range{std::min(count, maxInstances_), 0, 0, 0};
    const VkAccelerationStructureBuildRangeInfoKHR* pRange = &range;
    vkCmdBuildAccelerationStructuresKHR(cmd, 1, &info, &pRange);
    barrierAfterBuild(cmd);
}

VkSizeQueryImpl::~VkSizeQueryImpl() {
    if (pool_ == VK_NULL_HANDLE) return;
    vk::Context* ctx = &dev_.ctx();
    const VkQueryPool pool = pool_;
    dev_.defer([ctx, pool] { vkDestroyQueryPool(ctx->device(), pool, nullptr); });
}

bool VkSizeQueryImpl::create() {
    VkQueryPoolCreateInfo qi{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
    qi.queryType = VK_QUERY_TYPE_ACCELERATION_STRUCTURE_COMPACTED_SIZE_KHR;
    qi.queryCount = capacity_;
    return vkCreateQueryPool(dev_.ctx().device(), &qi, nullptr, &pool_) == VK_SUCCESS;
}

bool VkSizeQueryImpl::read(std::uint32_t first, std::uint32_t count, std::uint64_t* sizes) {
    if (count == 0) return true;
    std::vector<std::uint64_t> r(static_cast<std::size_t>(count) * 2, 0);
    vkGetQueryPoolResults(dev_.ctx().device(), pool_, first, count, r.size() * sizeof(std::uint64_t), r.data(), 2 * sizeof(std::uint64_t),
                          VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WITH_AVAILABILITY_BIT);
    bool all = true;
    for (std::uint32_t i = 0; i < count; ++i) {
        sizes[i] = r[i * 2 + 1] != 0 ? r[i * 2] : 0;
        all = all && r[i * 2 + 1] != 0;
    }
    return all;
}

// ---- command list -----------------------------------------------------------------------------------------------------------

void VkCommandListImpl::transition(Texture& texture, VkImageLayout layout) {
    vk::Image& img = static_cast<VkTextureImpl&>(texture).image();
    if (layout == VK_IMAGE_LAYOUT_UNDEFINED) {
        img.discard();
        return;
    }
    img.transition(cmd_, layout, true);  // the same layout again = a barrier (writes become visible)
}

void VkCommandListImpl::transitionMips(Texture& texture, VkImageLayout layout, std::uint32_t baseMip, std::uint32_t mipCount) {
    if (layout == VK_IMAGE_LAYOUT_UNDEFINED) return;
    static_cast<VkTextureImpl&>(texture).image().transitionMips(cmd_, layout, baseMip, mipCount, true);
}

void VkCommandListImpl::transitions(std::span<const TextureTransition> list) {
    std::vector<VkImageMemoryBarrier2> barriers;
    for (const TextureTransition& t : list) {
        if (t.layout == VK_IMAGE_LAYOUT_UNDEFINED) continue;
        static_cast<VkTextureImpl*>(t.texture)->image().appendBarriers(barriers, t.layout, t.baseMip, t.mipCount, true);
    }
    if (barriers.empty()) return;
    VkDependencyInfo di{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    di.imageMemoryBarrierCount = static_cast<std::uint32_t>(barriers.size());
    di.pImageMemoryBarriers = barriers.data();
    vkCmdPipelineBarrier2(cmd_, &di);
}

void VkCommandListImpl::computeBarrier() {
    VkMemoryBarrier2 b{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
    b.srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT;
    b.srcAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT | VK_ACCESS_2_TRANSFER_WRITE_BIT;
    b.dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT |
                     VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT;
    b.dstAccessMask = VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT | VK_ACCESS_2_SHADER_SAMPLED_READ_BIT |
                      VK_ACCESS_2_TRANSFER_READ_BIT | VK_ACCESS_2_TRANSFER_WRITE_BIT;
    VkDependencyInfo di{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    di.memoryBarrierCount = 1;
    di.pMemoryBarriers = &b;
    vkCmdPipelineBarrier2(cmd_, &di);
}

// (one barrier for every access kind: it also covers the buffer as a vertex / index / indirect source, a copy source and acceleration structure build input)
void VkCommandListImpl::drawIndexedIndirect(Buffer& args, std::uint64_t argsOffset, std::uint32_t drawCount, std::uint32_t stride) {
    flush();
    const VkBuffer b = static_cast<VkBufferImpl&>(args).buffer();
    if (drawCount > 1 && !dev_.caps().multiDrawIndirect) {  // drawCount > 1 needs the feature: one call per record
        for (std::uint32_t i = 0; i < drawCount; ++i) vkCmdDrawIndexedIndirect(cmd_, b, argsOffset + std::uint64_t(i) * stride, 1, stride);
        return;
    }
    vkCmdDrawIndexedIndirect(cmd_, b, argsOffset, drawCount, stride);
}

void VkCommandListImpl::bufferBarrier(Buffer&, BufferAccess) {
    VkMemoryBarrier2 b{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
    b.srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    b.srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT;
    b.dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    b.dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT;
    VkDependencyInfo di{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    di.memoryBarrierCount = 1;
    di.pMemoryBarriers = &b;
    vkCmdPipelineBarrier2(cmd_, &di);
}

void VkCommandListImpl::setPushConstants(const void* data, std::uint32_t size) { dev_.layout().push(cmd_, data, size); }

void VkCommandListImpl::bindPipeline(Pipeline& pipeline) {
    pipeline_ = static_cast<const VkPipelineImpl*>(&pipeline);
    vkCmdBindPipeline(cmd_, pipeline_->bindPoint(), pipeline_->pipeline());
    dev_.layout().bindSet0(cmd_, pipeline_->bindPoint(), dev_.bindless());
    dirty_ = true;
    pipelineBound_ = true;
}

// the push descriptors of set 1 (constant buffer, storage images) for the bound pipeline's bind point
void VkCommandListImpl::flush() {
    if (!dirty_ || pipeline_ == nullptr) return;
    std::array<VkWriteDescriptorSet, rhi::kStorageImages + 1> w{};
    std::array<VkDescriptorImageInfo, rhi::kStorageImages> ii{};
    VkDescriptorBufferInfo bi{cb_.buffer, cb_.offset, cb_.size};
    std::uint32_t n = 0;
    for (std::uint32_t i = 0; i < rhi::kStorageImages; ++i) {
        if (views_[i] == VK_NULL_HANDLE) continue;
        ii[i] = {VK_NULL_HANDLE, views_[i], VK_IMAGE_LAYOUT_GENERAL};
        w[n] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        w[n].dstBinding = i;
        w[n].descriptorCount = 1;
        w[n].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        w[n].pImageInfo = &ii[i];
        ++n;
    }
    if (cb_.buffer != VK_NULL_HANDLE) {
        w[n] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        w[n].dstBinding = kConstantBufferBinding;
        w[n].descriptorCount = 1;
        w[n].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        w[n].pBufferInfo = &bi;
        ++n;
    }
    if (n > 0) vkCmdPushDescriptorSetKHR(cmd_, pipeline_->bindPoint(), dev_.layout().layout(), 1, n, w.data());
    dirty_ = false;
}

void VkCommandListImpl::beginRendering(const RenderingInfo& info) {
    std::array<VkRenderingAttachmentInfo, 4> colors{};
    for (std::uint32_t i = 0; i < info.colorCount; ++i) {
        const auto& a = info.color[i];
        colors[i] = {VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
        colors[i].imageView = static_cast<VkTextureImpl*>(a.texture)->attachmentView(a.mip, a.layer);
        colors[i].imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        colors[i].loadOp = a.load;
        colors[i].storeOp = a.store;
        colors[i].clearValue.color = a.clear;
    }
    VkRenderingAttachmentInfo depth{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
    VkRenderingInfo ri{VK_STRUCTURE_TYPE_RENDERING_INFO};
    ri.renderArea = {{0, 0}, {info.width, info.height}};
    ri.layerCount = 1;
    ri.colorAttachmentCount = info.colorCount;
    ri.pColorAttachments = colors.data();
    if (info.depth.texture != nullptr) {
        depth.imageView = static_cast<VkTextureImpl*>(info.depth.texture)->attachmentView(info.depth.mip, info.depth.layer);
        depth.imageLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
        depth.loadOp = info.depth.load;
        depth.storeOp = info.depth.store;
        depth.clearValue.depthStencil = info.depthClear;
        ri.pDepthAttachment = &depth;
    }
    vkCmdBeginRendering(cmd_, &ri);
    setViewport(0.0f, 0.0f, static_cast<float>(info.width), static_cast<float>(info.height));
    setScissor(0, 0, info.width, info.height);
}

// (the pipelines have the vertex input binding stride as dynamic state, so the strides of the call apply)
void VkCommandListImpl::setVertexBuffers(std::uint32_t first, std::span<Buffer* const> buffers, std::span<const std::uint64_t> offsets,
                                         std::span<const std::uint32_t> strides) {
    std::array<VkBuffer, 8> bufs{};
    std::array<VkDeviceSize, 8> offs{};
    std::array<VkDeviceSize, 8> strs{};
    const std::uint32_t n = static_cast<std::uint32_t>(std::min<std::size_t>(buffers.size(), bufs.size()));
    for (std::uint32_t i = 0; i < n; ++i) {
        bufs[i] = static_cast<VkBufferImpl*>(buffers[i])->buffer();
        offs[i] = i < offsets.size() ? offsets[i] : 0;
        strs[i] = i < strides.size() ? strides[i] : 0;
    }
    vkCmdBindVertexBuffers2(cmd_, first, n, bufs.data(), offs.data(), nullptr, strs.data());
}

void VkCommandListImpl::copyBufferToTexture(Texture& dst, Buffer& src, std::uint64_t srcOffset, Subresource sub) {
    VkTextureImpl& t = static_cast<VkTextureImpl&>(dst);
    VkBufferImageCopy c{};
    c.bufferOffset = srcOffset;
    const VkExtent3D e = mipExtent(t.desc(), sub.mip);
    const FormatBlock fb = formatBlock(t.desc().format);
    c.bufferRowLength = bufferRowPitch(t.desc().format, e.width) / fb.bytes * fb.width;
    c.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, sub.mip, sub.layer, 1};
    c.imageExtent = e;
    vkCmdCopyBufferToImage(cmd_, static_cast<VkBufferImpl&>(src).buffer(), t.image().image(), VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &c);
}

void VkCommandListImpl::copyTextureToBuffer(Buffer& dst, std::uint64_t dstOffset, Texture& src, Subresource sub) {
    VkTextureImpl& t = static_cast<VkTextureImpl&>(src);
    VkBufferImageCopy c{};
    c.bufferOffset = dstOffset;
    const VkExtent3D e = mipExtent(t.desc(), sub.mip);
    const FormatBlock fb = formatBlock(t.desc().format);
    c.bufferRowLength = bufferRowPitch(t.desc().format, e.width) / fb.bytes * fb.width;
    c.imageSubresource = {t.image().aspect() & ~VkImageAspectFlags(VK_IMAGE_ASPECT_STENCIL_BIT), sub.mip, sub.layer, 1};
    c.imageExtent = e;
    vkCmdCopyImageToBuffer(cmd_, t.image().image(), VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, static_cast<VkBufferImpl&>(dst).buffer(), 1, &c);
}

void VkCommandListImpl::copyBufferToTextureBox(Texture& dst, Buffer& src, const BufferTextureCopy& copy) {
    VkTextureImpl& t = static_cast<VkTextureImpl&>(dst);
    VkBufferImageCopy c{};
    c.bufferOffset = copy.bufferOffset;
    const FormatBlock fb = formatBlock(t.desc().format);
    c.bufferRowLength = copy.rowPitch != 0 ? copy.rowPitch / fb.bytes * fb.width : 0;  // 0 = tightly packed
    c.imageSubresource = {t.image().aspect() & ~VkImageAspectFlags(VK_IMAGE_ASPECT_STENCIL_BIT), copy.sub.mip, copy.sub.layer, 1};
    c.imageOffset = copy.offset;
    c.imageExtent = copy.extent;
    vkCmdCopyBufferToImage(cmd_, static_cast<VkBufferImpl&>(src).buffer(), t.image().image(), VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &c);
}

void VkCommandListImpl::clearTexture(Texture& texture, const VkClearColorValue& value, std::uint32_t baseMip, std::uint32_t mipCount) {
    vk::Image& img = static_cast<VkTextureImpl&>(texture).image();
    const VkImageSubresourceRange r{VK_IMAGE_ASPECT_COLOR_BIT, baseMip, mipCount, 0, VK_REMAINING_ARRAY_LAYERS};
    vkCmdClearColorImage(cmd_, img.image(), VK_IMAGE_LAYOUT_GENERAL, &value, 1, &r);
}

void VkCommandListImpl::copyTexture(Texture& dst, Texture& src, std::span<const TextureCopy> regions) {
    VkTextureImpl& d = static_cast<VkTextureImpl&>(dst);
    VkTextureImpl& s = static_cast<VkTextureImpl&>(src);
    std::vector<VkImageCopy2> rs(regions.size());
    for (std::size_t i = 0; i < regions.size(); ++i) {
        const TextureCopy& c = regions[i];
        const bool whole = c.extent.width == 0 && c.extent.height == 0 && c.extent.depth == 0;
        rs[i] = {VK_STRUCTURE_TYPE_IMAGE_COPY_2};
        rs[i].srcSubresource = {s.image().aspect(), c.src.mip, c.src.layer, c.layerCount};
        rs[i].dstSubresource = {d.image().aspect(), c.dst.mip, c.dst.layer, c.layerCount};
        rs[i].srcOffset = c.srcOffset;
        rs[i].dstOffset = c.dstOffset;
        rs[i].extent = whole ? mipExtent(s.desc(), c.src.mip) : c.extent;
    }
    VkCopyImageInfo2 ci{VK_STRUCTURE_TYPE_COPY_IMAGE_INFO_2};
    ci.srcImage = s.image().image();
    ci.srcImageLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    ci.dstImage = d.image().image();
    ci.dstImageLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    ci.regionCount = static_cast<std::uint32_t>(rs.size());
    ci.pRegions = rs.data();
    vkCmdCopyImage2(cmd_, &ci);
}

bool VkCommandListImpl::blitTexture(Texture& dst, Texture& src, std::span<const TextureBlit> regions, VkFilter filter) {
    VkTextureImpl& d = static_cast<VkTextureImpl&>(dst);
    VkTextureImpl& s = static_cast<VkTextureImpl&>(src);
    std::vector<VkImageBlit2> rs(regions.size());
    for (std::size_t i = 0; i < regions.size(); ++i) {
        const TextureBlit& b = regions[i];
        const auto whole = [](const VkExtent3D& e) { return e.width == 0 && e.height == 0 && e.depth == 0; };
        const VkExtent3D se = whole(b.srcExtent) ? mipExtent(s.desc(), b.src.mip) : b.srcExtent;
        const VkExtent3D de = whole(b.dstExtent) ? mipExtent(d.desc(), b.dst.mip) : b.dstExtent;
        rs[i] = {VK_STRUCTURE_TYPE_IMAGE_BLIT_2};
        rs[i].srcSubresource = {s.image().aspect(), b.src.mip, b.src.layer, 1};
        rs[i].dstSubresource = {d.image().aspect(), b.dst.mip, b.dst.layer, 1};
        rs[i].srcOffsets[0] = b.srcOffset;
        rs[i].dstOffsets[0] = b.dstOffset;
        rs[i].srcOffsets[1] = {b.srcOffset.x + static_cast<std::int32_t>(se.width), b.srcOffset.y + static_cast<std::int32_t>(se.height),
                               b.srcOffset.z + static_cast<std::int32_t>(se.depth)};
        rs[i].dstOffsets[1] = {b.dstOffset.x + static_cast<std::int32_t>(de.width), b.dstOffset.y + static_cast<std::int32_t>(de.height),
                               b.dstOffset.z + static_cast<std::int32_t>(de.depth)};
    }
    VkBlitImageInfo2 bi{VK_STRUCTURE_TYPE_BLIT_IMAGE_INFO_2};
    // a texture blitted onto itself that lies in GENERAL as a whole (mip chains: the one layout both sides can have) keeps it; else the transfer layouts
    const bool general = &s == &d && s.image().layout() == VK_IMAGE_LAYOUT_GENERAL;
    bi.srcImage = s.image().image();
    bi.srcImageLayout = general ? VK_IMAGE_LAYOUT_GENERAL : VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    bi.dstImage = d.image().image();
    bi.dstImageLayout = general ? VK_IMAGE_LAYOUT_GENERAL : VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    bi.regionCount = static_cast<std::uint32_t>(rs.size());
    bi.pRegions = rs.data();
    bi.filter = filter;
    vkCmdBlitImage2(cmd_, &bi);
    return true;
}

void VkCommandListImpl::buildBlases(std::span<const BlasBuild> builds) {
    if (builds.empty()) return;
    vk::Context& ctx = dev_.ctx();
    const VkDeviceSize align = dev_.scratchAlign();
    const auto aligned = [&](VkDeviceSize v) { return (v + align - 1) & ~(align - 1); };
    // the pool holds the largest single build; the batch is cut into runs when it holds not all of them at once
    VkDeviceSize largest = 0;
    std::size_t geomTotal = 0;
    for (const BlasBuild& b : builds) {
        VkAccelImpl& a = static_cast<VkAccelImpl&>(*b.blas);
        largest = std::max(largest, aligned(a.scratchNeed(b.update && a.allowUpdate())));
        geomTotal += b.geometries.empty() ? a.geometries().size() : b.geometries.size();
    }
    if (!dev_.ensureScratch(largest)) {
        spdlog::error("rhi: the BLAS build scratch ({} bytes) could not be allocated", largest);
        return;
    }
    barrierBeforeBuild(cmd_, ctx.features().rayTracing.maintenance1);
    std::vector<VkAccelerationStructureGeometryKHR> geoms;
    std::vector<VkAccelerationStructureBuildRangeInfoKHR> ranges;
    geoms.reserve(geomTotal);
    ranges.reserve(geomTotal);
    std::vector<VkAccelerationStructureBuildGeometryInfoKHR> infos;
    std::vector<const VkAccelerationStructureBuildRangeInfoKHR*> pr;
    infos.reserve(builds.size());
    pr.reserve(builds.size());
    VkDeviceSize used = 0;
    const auto flushRun = [&] {
        if (infos.empty()) return;
        vkCmdBuildAccelerationStructuresKHR(cmd_, static_cast<std::uint32_t>(infos.size()), infos.data(), pr.data());
        infos.clear();
        pr.clear();
    };
    for (const BlasBuild& b : builds) {
        VkAccelImpl& a = static_cast<VkAccelImpl&>(*b.blas);
        const bool update = b.update && a.allowUpdate();
        const VkDeviceSize need = aligned(a.scratchNeed(update));
        if (used + need > dev_.scratch().size) {
            // the pool is full: record what we have, wait for it (scratch reuse), continue at its start
            flushRun();
            barrierBeforeBuild(cmd_, ctx.features().rayTracing.maintenance1);
            used = 0;
        }
        const std::size_t g0 = geoms.size();
        blasGeometries(b.geometries.empty() ? std::span<const TriangleGeometry>(a.geometries()) : b.geometries, geoms, ranges, nullptr);
        VkAccelerationStructureBuildGeometryInfoKHR info{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR};
        info.type = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR;
        info.flags = a.flags();
        info.mode = update ? VK_BUILD_ACCELERATION_STRUCTURE_MODE_UPDATE_KHR : VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR;
        info.srcAccelerationStructure = update ? a.handle() : VK_NULL_HANDLE;
        info.dstAccelerationStructure = a.handle();
        info.geometryCount = static_cast<std::uint32_t>(geoms.size() - g0);
        info.pGeometries = geoms.data() + g0;
        info.scratchData.deviceAddress = dev_.scratch().address + used;
        used += need;
        infos.push_back(info);
        pr.push_back(ranges.data() + g0);
    }
    flushRun();
    barrierAfterBuild(cmd_);
}

void VkCommandListImpl::queryCompactedSizes(SizeQuery& query, std::uint32_t first, std::span<AccelerationStructure* const> blases) {
    if (blases.empty()) return;
    VkSizeQueryImpl& q = static_cast<VkSizeQueryImpl&>(query);
    std::vector<VkAccelerationStructureKHR> handles;
    handles.reserve(blases.size());
    for (AccelerationStructure* b : blases) handles.push_back(static_cast<VkAccelImpl*>(b)->handle());
    // the builds -> the size queries
    VkMemoryBarrier2 mb{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
    mb.srcStageMask = VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_BUILD_BIT_KHR;
    mb.srcAccessMask = VK_ACCESS_2_ACCELERATION_STRUCTURE_WRITE_BIT_KHR;
    mb.dstStageMask = VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_BUILD_BIT_KHR |
                      (dev_.ctx().features().rayTracing.maintenance1 ? VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_COPY_BIT_KHR : 0);
    mb.dstAccessMask = VK_ACCESS_2_ACCELERATION_STRUCTURE_READ_BIT_KHR | VK_ACCESS_2_ACCELERATION_STRUCTURE_WRITE_BIT_KHR;
    VkDependencyInfo di{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    di.memoryBarrierCount = 1;
    di.pMemoryBarriers = &mb;
    vkCmdPipelineBarrier2(cmd_, &di);
    vkCmdResetQueryPool(cmd_, q.pool(), first, static_cast<std::uint32_t>(handles.size()));
    vkCmdWriteAccelerationStructuresPropertiesKHR(cmd_, static_cast<std::uint32_t>(handles.size()), handles.data(),
                                                  VK_QUERY_TYPE_ACCELERATION_STRUCTURE_COMPACTED_SIZE_KHR, q.pool(), first);
}

void VkCommandListImpl::compactBlas(AccelerationStructure& dst, AccelerationStructure& src) {
    barrierBeforeBuild(cmd_, dev_.ctx().features().rayTracing.maintenance1);  // (the builds that made `src`, and queries that still read the structures)
    VkCopyAccelerationStructureInfoKHR ci{VK_STRUCTURE_TYPE_COPY_ACCELERATION_STRUCTURE_INFO_KHR};
    ci.src = static_cast<VkAccelImpl&>(src).handle();
    ci.dst = static_cast<VkAccelImpl&>(dst).handle();
    ci.mode = VK_COPY_ACCELERATION_STRUCTURE_MODE_COMPACT_KHR;
    vkCmdCopyAccelerationStructureKHR(cmd_, &ci);
}

void* VkDeviceImpl::nativeCommandBuffer(CommandList& list) { return static_cast<VkCommandListImpl&>(list).cmd(); }

std::uint64_t VkDeviceImpl::packetHandle(Buffer& buffer) const { return reinterpret_cast<std::uint64_t>(static_cast<VkBufferImpl&>(buffer).buffer()); }

Buffer& VkDeviceImpl::ringBuffer(const RingAlloc& alloc) {
    const std::lock_guard lock(wrapMutex_);
    auto& slot = ringBuffers_[alloc.buffer];
    if (!slot) {
        BufferDesc d;
        d.name = "rvk upload ring chunk";
        slot = std::make_unique<VkBufferImpl>(*this, d);
        slot->wrapExternal(reinterpret_cast<VkBuffer>(alloc.buffer), alloc.gpuVa - alloc.offset,
                           alloc.ptr != nullptr ? static_cast<std::uint8_t*>(alloc.ptr) - alloc.offset : nullptr);
    }
    return *slot;
}

MeshAlloc VkDeviceImpl::allocMesh(std::uint64_t size, std::uint32_t multiple) {
    if (arena_ == nullptr) return {};
    const vk::MeshArena::Alloc a = arena_->allocate(size, multiple);
    if (!a.valid()) return {};
    MeshAlloc m;
    m.native = reinterpret_cast<std::uint64_t>(a.buffer);
    m.offset = a.offset;
    m.addr = a.addr;
    m.backend[0] = a.block;
    m.backend[1] = reinterpret_cast<std::uint64_t>(a.virt);
    m.backend[2] = reinterpret_cast<std::uint64_t>(a.dedicated);
    {
        const std::lock_guard lock(wrapMutex_);
        auto& slot = meshBuffers_[m.native];
        if (!slot) {
            BufferDesc d;
            d.name = "rvk mesh arena";
            slot = std::make_unique<VkBufferImpl>(*this, d);
        }
        slot->wrapExternal(a.buffer, a.addr.va != 0 ? a.addr.va - a.offset : 0, nullptr);
        m.buffer = slot.get();
    }
    return m;
}

void VkDeviceImpl::freeMesh(const MeshAlloc& alloc, bool deferred) {
    if (arena_ == nullptr || !alloc.valid()) return;
    vk::MeshArena::Alloc a;
    a.buffer = reinterpret_cast<VkBuffer>(alloc.native);
    a.offset = alloc.offset;
    a.addr = alloc.addr;
    a.block = static_cast<std::uint32_t>(alloc.backend[0]);
    a.virt = reinterpret_cast<VmaVirtualAllocation>(alloc.backend[1]);
    a.dedicated = reinterpret_cast<VmaAllocation>(alloc.backend[2]);
    vk::MeshArena* arena = arena_;
    if (deferred) {
        frames_.retire([arena, a] { arena->free(a); });
    } else {
        arena->free(a);
    }
}

Texture* VkDeviceImpl::transient(std::string_view key, const TransientDesc& desc) {
    const std::string k(key);
    auto it = transients_.find(k);
    if (it != transients_.end()) {
        if (it->second.desc == desc && it->second.tex) return it->second.tex.get();
        transients_.erase(it);  // the texture's destructor defers the GPU resources
    }
    // the entry first: its name (a node of the map, stable) is the image's debug name
    auto [pos, inserted] = transients_.emplace(k, TransientEntry{});
    TransientEntry& e = pos->second;
    e.desc = desc;
    TextureDesc td;
    td.format = desc.format;
    td.extent = {std::max(1u, desc.extent.width), std::max(1u, desc.extent.height), 1};
    td.usage = desc.usage;
    td.name = pos->first.c_str();
    auto t = std::make_unique<VkTextureImpl>(*this, td);
    if (!t->create()) {
        spdlog::error("vulkan: transient image {} ({}x{}) failed", k, desc.extent.width, desc.extent.height);
        transients_.erase(pos);
        return nullptr;
    }
    // the slot is what shaders sample it through: it joins the name
    if ((desc.usage & VK_IMAGE_USAGE_SAMPLED_BIT) != 0) t->setDebugName(std::format("{} [slot {}]", k, t->heapSampled()));
    spdlog::info("vulkan: transient image {} {}x{} format {} (bindless slot {})", k, desc.extent.width, desc.extent.height,
                 static_cast<int>(desc.format), t->heapSampled());
    e.tex = std::move(t);
    return e.tex.get();
}

}  // namespace

std::unique_ptr<Device> createVulkanDevice(const DeviceDesc& desc) {
    auto d = std::make_unique<VkDeviceImpl>();
    if (!d->create(desc)) return nullptr;
    return d;
}

// ---- over the renderer's objects (rhi/vulkan/wrap.h) -------------------------------------------------------------------------

namespace vulkan {

std::unique_ptr<GameDevice> wrapGame(vk::Context& ctx, vk::Frames& frames, vk::Bindless& bindless, vk::MeshArena* arena) {
    auto d = std::make_unique<VkDeviceImpl>(ctx, frames, bindless, arena);
    if (!d->createWrapped()) return nullptr;
    return d;
}

std::unique_ptr<Device> wrap(vk::Context& ctx, vk::Frames& frames, vk::Bindless& bindless) { return wrapGame(ctx, frames, bindless, nullptr); }

vk::Image& imageOf(Texture& texture) { return static_cast<VkTextureImpl&>(texture).image(); }
VkCommandBuffer commandBufferOf(CommandList& list) { return static_cast<VkCommandListImpl&>(list).cmd(); }
vk::Context& contextOf(Device& device) { return static_cast<VkDeviceImpl&>(device).ctx(); }

std::unique_ptr<Texture> wrapImage(Device& device, vk::Image& image, std::uint32_t sampledSlot, const char* name) {
    VkDeviceImpl& dev = static_cast<VkDeviceImpl&>(device);
    const vk::ImageDesc& id = image.desc();
    TextureDesc td;
    td.type = id.type;
    td.format = id.format;
    td.extent = id.extent;
    td.mips = id.mips;
    td.layers = id.layers;
    td.cube = id.cube;
    td.usage = id.usage;
    td.name = name != nullptr ? name : id.name;
    auto t = std::make_unique<VkTextureImpl>(dev, td);
    t->wrap(image, sampledSlot);
    return t;
}

std::unique_ptr<Buffer> wrapBuffer(Device& device, VkBuffer buffer, std::uint64_t address, std::uint64_t size, void* mapped, const char* name) {
    BufferDesc d;
    d.size = size;
    d.name = name;
    auto b = std::make_unique<VkBufferImpl>(static_cast<VkDeviceImpl&>(device), d);
    b->wrapExternal(buffer, address, mapped);
    return b;
}

std::unique_ptr<CommandList> wrapCommandBuffer(Device& device, VkCommandBuffer cmd) {
    return std::make_unique<VkCommandListImpl>(static_cast<VkDeviceImpl&>(device), cmd);
}

}  // namespace vulkan

std::unique_ptr<GameDevice> createGameDevice(Backend backend, const GameDeviceOptions& options) {
    if (backend == Backend::D3D12) return createD3D12GameDevice(options);
    auto d = std::make_unique<VkDeviceImpl>(VkDeviceImpl::GameTag{});
    if (!d->createGame(options)) return nullptr;
    return d;
}

}  // namespace rvk::rhi
