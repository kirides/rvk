// The Vulkan backend of the slim RHI (rhi/rhi.h): a thin layer over the renderer's own vk:: classes (Context, Frames, Bindless, PassLayout,
// Image, pass pipelines). Nothing here is new Vulkan code: it is the pass machinery of vk/passes.h behind the interface.

#include <array>
#include <vector>

#include <spdlog/spdlog.h>

#include "rhi/rhi.h"
#include "shader/abi.h"
#include "vk/bindless.h"
#include "vk/context.h"
#include "vk/frames.h"
#include "vk/image.h"
#include "vk/passes.h"
#include "vk/swapchain.h"

namespace rvk::rhi {
namespace {

constexpr std::uint32_t kConstantBufferBinding = 8;  // b8 space1: set 1 binding 8
constexpr std::uint32_t kStorageImages = 4;          // u0..u3 space1: set 1 bindings 0..3

class VkDeviceImpl;

class VkBufferImpl final : public Buffer {
public:
    VkBufferImpl(VkDeviceImpl& dev, const BufferDesc& desc) : dev_(dev), desc_(desc) {}
    ~VkBufferImpl() override;
    bool create();
    const BufferDesc& desc() const override { return desc_; }
    void* map() override { return mapped_; }
    std::uint32_t heapIndex() const override { return 0; }  // Vulkan reaches buffers by address (RVK_REF)
    std::array<std::uint32_t, 2> shaderRef(std::uint64_t offset) const override {
        const std::uint64_t a = address_ + offset;
        return {static_cast<std::uint32_t>(a), static_cast<std::uint32_t>(a >> 32)};
    }
    VkBuffer buffer() const { return buffer_; }

private:
    VkDeviceImpl& dev_;
    BufferDesc desc_;
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
    vk::Image& image() { return image_; }
    // a swapchain image (owned by the swapchain)
    void adopt(VkImage image, VkImageView view) {
        vk::ImageDesc d;
        d.format = desc_.format;
        d.extent = desc_.extent;
        d.usage = desc_.usage;
        image_.adopt(image, view, d);
    }

private:
    VkDeviceImpl& dev_;
    TextureDesc desc_;
    vk::Image image_;
    std::uint32_t slot_ = 0;
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
    VkPipelineImpl(vk::Context& ctx, VkPipeline p, VkPipelineBindPoint bp) : ctx_(ctx), pipeline_(p), bindPoint_(bp) {}
    ~VkPipelineImpl() override {
        if (pipeline_ != VK_NULL_HANDLE) vkDestroyPipeline(ctx_.device(), pipeline_, nullptr);
    }
    VkPipeline pipeline() const { return pipeline_; }
    VkPipelineBindPoint bindPoint() const { return bindPoint_; }

private:
    vk::Context& ctx_;
    VkPipeline pipeline_;
    VkPipelineBindPoint bindPoint_;
};

class VkCommandListImpl final : public CommandList {
public:
    VkCommandListImpl(VkDeviceImpl& dev, VkCommandBuffer cmd) : dev_(dev), cmd_(cmd) {}

    void transition(Texture& texture, VkImageLayout layout) override;
    void bufferBarrier(Buffer&) override;
    void setPushConstants(const void* data, std::uint32_t size) override;
    void setConstantBuffer(Buffer& buffer, std::uint64_t offset, std::uint64_t size) override {
        cb_ = {static_cast<VkBufferImpl&>(buffer).buffer(), offset, size};
        dirty_ = true;
    }
    void setStorageTextures(std::span<Texture* const> textures) override {
        views_.fill(VK_NULL_HANDLE);
        for (std::size_t i = 0; i < textures.size() && i < views_.size(); ++i) {
            if (textures[i] != nullptr) views_[i] = static_cast<VkTextureImpl*>(textures[i])->image().view();
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
    void copyBuffer(Buffer& dst, std::uint64_t dstOffset, Buffer& src, std::uint64_t srcOffset, std::uint64_t size) override {
        const VkBufferCopy c{srcOffset, dstOffset, size};
        vkCmdCopyBuffer(cmd_, static_cast<VkBufferImpl&>(src).buffer(), static_cast<VkBufferImpl&>(dst).buffer(), 1, &c);
    }
    void copyBufferToTexture(Texture& dst, Buffer& src, std::uint64_t srcOffset) override;
    void copyTextureToBuffer(Buffer& dst, std::uint64_t dstOffset, Texture& src) override;
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
    std::array<VkImageView, kStorageImages> views_{};
    bool dirty_ = true;
    bool pipelineBound_ = false;
};

class VkDeviceImpl final : public Device {
public:
    ~VkDeviceImpl() override {
        if (ctx_.device() != VK_NULL_HANDLE) ctx_.waitIdle();
        for (VkSampler s : samplers_) vkDestroySampler(ctx_.device(), s, nullptr);
        layout_.destroy(ctx_);
        frames_.destroy();  // (runs the retired slot releases of the textures: the bindless set must still exist)
        bindless_.destroy();
        ctx_.destroy();
    }

    bool create(const DeviceDesc& desc) {
        vk::ContextOptions o;
        o.validation = desc.validation;
        o.adapter = desc.adapter;
        o.appName = desc.name;
        o.allowRayTracing = false;
        if (!ctx_.create(o)) return false;
        if (!frames_.create(ctx_) || !bindless_.create(ctx_, frames_)) return false;
        VkDescriptorSetLayoutBinding b[kStorageImages + 1]{};
        const VkShaderStageFlags stages = VK_SHADER_STAGE_COMPUTE_BIT | VK_SHADER_STAGE_FRAGMENT_BIT | VK_SHADER_STAGE_VERTEX_BIT;
        for (std::uint32_t i = 0; i < kStorageImages; ++i) b[i] = {i, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, stages, nullptr};
        b[kStorageImages] = {kConstantBufferBinding, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, stages, nullptr};
        if (!layout_.create(ctx_, bindless_, b)) return false;
        caps_.adapterName = ctx_.deviceName();
        caps_.backend = Backend::Vulkan;
        caps_.bindless = true;
        caps_.heapSize = vk::Bindless::kMax2D;
        return true;
    }

    const DeviceCaps& caps() const override { return caps_; }
    std::uint32_t validationErrors() const override { return ctx_.validationErrors(); }

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
        VkSampler s = VK_NULL_HANDLE;
        if (vkCreateSampler(ctx_.device(), &ci, nullptr, &s) != VK_SUCCESS) return 0;
        samplers_.push_back(s);
        return bindless_.addSampler(s);
    }
    std::unique_ptr<Pipeline> createComputePipeline(const ComputePipelineDesc& desc) override {
        VkPipeline p = vk::createComputePipeline(ctx_, layout_.layout(), desc.cs.spirv, desc.name != nullptr ? desc.name : "rhi compute");
        if (p == VK_NULL_HANDLE) return nullptr;
        return std::make_unique<VkPipelineImpl>(ctx_, p, VK_PIPELINE_BIND_POINT_COMPUTE);
    }
    std::unique_ptr<Pipeline> createGraphicsPipeline(const GraphicsPipelineDesc& desc) override {
        vk::PassGraphicsDesc g;
        g.vs = desc.vs.spirv;
        g.fs = desc.ps.spirv;
        g.color = desc.colorFormats[0];
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
        g.name = desc.name != nullptr ? desc.name : "rhi graphics";
        VkPipeline p = vk::createPassGraphicsPipeline(ctx_, layout_.layout(), g);
        if (p == VK_NULL_HANDLE) return nullptr;
        return std::make_unique<VkPipelineImpl>(ctx_, p, VK_PIPELINE_BIND_POINT_GRAPHICS);
    }

    void submitAndWait(const std::function<void(CommandList&)>& record) override {
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
        auto s = std::make_unique<VkSwapchainImpl>(*this);
        if (!s->create(desc)) return nullptr;
        return s;
    }
    bool renderAndPresent(Swapchain& swapchain, const std::function<void(CommandList&, Texture&)>& record) override {
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

private:
    vk::Context ctx_;
    vk::Frames frames_;
    vk::Bindless bindless_;
    vk::PassLayout layout_;
    DeviceCaps caps_;
    bool bindlessInit_ = false;
    std::vector<VkSampler> samplers_;
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
    VmaAllocationCreateInfo aci{};
    aci.usage = VMA_MEMORY_USAGE_AUTO;
    if (desc_.memory == Memory::Upload) {
        aci.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;
    } else if (desc_.memory == Memory::Readback) {
        aci.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;
    }
    VmaAllocationInfo info{};
    if (vmaCreateBuffer(dev_.ctx().allocator(), &bci, &aci, &buffer_, &alloc_, &info) != VK_SUCCESS) return false;
    mapped_ = info.pMappedData;
    VkBufferDeviceAddressInfo ai{VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO};
    ai.buffer = buffer_;
    address_ = vkGetBufferDeviceAddress(dev_.ctx().device(), &ai);
    return true;
}

VkBufferImpl::~VkBufferImpl() {
    if (buffer_ != VK_NULL_HANDLE) vmaDestroyBuffer(dev_.ctx().allocator(), buffer_, alloc_);
}

bool VkTextureImpl::create() {
    vk::ImageDesc d;
    d.type = desc_.type;
    d.format = desc_.format;
    d.extent = desc_.extent;
    d.mips = desc_.mips;
    d.layers = desc_.layers;
    d.usage = desc_.usage | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    d.name = desc_.name;
    if (!image_.create(dev_.ctx(), d)) return false;
    if ((desc_.usage & VK_IMAGE_USAGE_SAMPLED_BIT) != 0) slot_ = dev_.bindless().addTexture(vk::TextureKind::Tex2D, image_.view());
    return true;
}

VkTextureImpl::~VkTextureImpl() {
    if (slot_ != 0) dev_.bindless().releaseTexture(vk::TextureKind::Tex2D, slot_);
    if (image_.valid()) {
        dev_.ctx().waitIdle();  // (tools / tests: no frames in flight reference it)
        image_.destroy(dev_.ctx());
    }
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

void VkCommandListImpl::bufferBarrier(Buffer&) {
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
    std::array<VkWriteDescriptorSet, kStorageImages + 1> w{};
    std::array<VkDescriptorImageInfo, kStorageImages> ii{};
    VkDescriptorBufferInfo bi{cb_.buffer, cb_.offset, cb_.size};
    std::uint32_t n = 0;
    for (std::uint32_t i = 0; i < kStorageImages; ++i) {
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
        colors[i].imageView = static_cast<VkTextureImpl*>(a.texture)->image().view();
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
        depth.imageView = static_cast<VkTextureImpl*>(info.depth.texture)->image().view();
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

void VkCommandListImpl::copyBufferToTexture(Texture& dst, Buffer& src, std::uint64_t srcOffset) {
    VkTextureImpl& t = static_cast<VkTextureImpl&>(dst);
    VkBufferImageCopy c{};
    c.bufferOffset = srcOffset;
    c.bufferRowLength = bufferRowPitch(t.desc().format, t.desc().extent.width) / formatBytes(t.desc().format);
    c.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    c.imageExtent = t.desc().extent;
    vkCmdCopyBufferToImage(cmd_, static_cast<VkBufferImpl&>(src).buffer(), t.image().image(), VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &c);
}

void VkCommandListImpl::copyTextureToBuffer(Buffer& dst, std::uint64_t dstOffset, Texture& src) {
    VkTextureImpl& t = static_cast<VkTextureImpl&>(src);
    VkBufferImageCopy c{};
    c.bufferOffset = dstOffset;
    c.bufferRowLength = bufferRowPitch(t.desc().format, t.desc().extent.width) / formatBytes(t.desc().format);
    c.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    c.imageExtent = t.desc().extent;
    vkCmdCopyImageToBuffer(cmd_, t.image().image(), VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, static_cast<VkBufferImpl&>(dst).buffer(), 1, &c);
}

}  // namespace

std::unique_ptr<Device> createVulkanDevice(const DeviceDesc& desc) {
    auto d = std::make_unique<VkDeviceImpl>();
    if (!d->create(desc)) return nullptr;
    return d;
}

}  // namespace rvk::rhi
