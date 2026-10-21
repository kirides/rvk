// d3d9::Device: creation, frames, resources, state setters, clears, copies. Draw preparation is in draw.cpp.

#include "d3d9/device.h"

#include <windows.h>

#include <cstring>
#include <fstream>

#include <spdlog/spdlog.h>

#include "d3d9/convert.h"
#include "shader/sm_ir.h"
#include "shader/spirv.h"

namespace rvk::d3d9 {
namespace {

// bump when the translator's output changes: invalidates the SPIR-V disk cache
constexpr std::uint32_t kTranslatorVersion = 1;

std::uint64_t fnv1a(const void* data, std::size_t size, std::uint64_t h = 0xcbf29ce484222325ull) {
    const auto* p = static_cast<const unsigned char*>(data);
    for (std::size_t i = 0; i < size; ++i) {
        h = (h ^ p[i]) * 0x100000001b3ull;
    }
    return h;
}

}  // namespace

Device::Device() = default;
Device::~Device() { destroy(); }

std::uint32_t Device::slotOf(std::uint32_t d3dSampler) {
    if (d3dSampler >= kVertexSamplerD3D && d3dSampler < kVertexSamplerD3D + 4) {
        return 16 + (d3dSampler - kVertexSamplerD3D);
    }
    return d3dSampler < 16 ? d3dSampler : kSamplerCount;
}

bool Device::recordingThread() const { return GetCurrentThreadId() == renderThread_; }

bool Device::create(const DeviceOptions& options) {
    options_ = options;
    renderThread_ = GetCurrentThreadId();
    if (!ctx_.create({options.validation, options.adapter, "Risen"})) {
        return false;
    }
    // D24S8 depth is optional in Vulkan (AMD has none): fall back to D32S8
    VkFormatProperties fp{};
    vkGetPhysicalDeviceFormatProperties(ctx_.physicalDevice(), VK_FORMAT_D24_UNORM_S8_UINT, &fp);
    const VkFormatFeatureFlags need = VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT;
    d24s8_ = (fp.optimalTilingFeatures & need) == need ? VK_FORMAT_D24_UNORM_S8_UINT : VK_FORMAT_D32_SFLOAT_S8_UINT;

    if (!frames_.create(ctx_) || !bindless_.create(ctx_, frames_)) {
        return false;
    }
    const auto pipelineFile = options.cacheDir.empty() ? std::filesystem::path() : options.cacheDir / "pipelines.bin";
    if (!pipelines_.create(ctx_, bindless_.pipelineLayout(), pipelineFile)) {
        return false;
    }
    if (!options.offscreen && !swapchain_.create(ctx_, options.window, options.width, options.height, options.vsync)) {
        return false;
    }
    for (auto& s : acquireSemaphores_) {
        VkSemaphoreCreateInfo sci{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
        vkCreateSemaphore(ctx_.device(), &sci, nullptr, &s);
    }
    {
        VkBufferCreateInfo bci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
        bci.size = 16;
        bci.usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT;
        VmaAllocationCreateInfo aci{};
        aci.usage = VMA_MEMORY_USAGE_AUTO;
        aci.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;
        VmaAllocationInfo info{};
        if (vmaCreateBuffer(ctx_.allocator(), &bci, &aci, &nullVertexBuffer_, &nullVertexAlloc_, &info) != VK_SUCCESS) {
            return false;
        }
        const float v[4] = {0.0f, 0.0f, 0.0f, 1.0f};
        std::memcpy(info.pMappedData, v, sizeof(v));
    }

    for (std::uint32_t i = 0; i < RS_COUNT; ++i) {
        rs_[i] = defaultRenderState(i);
    }
    if (options.depthFormat == 0) {
        rs_[RS_ZENABLE] = ZB_FALSE;
    }
    for (auto& s : samp_) {
        for (std::uint32_t i = 0; i < SAMP_COUNT; ++i) {
            s[i] = defaultSamplerState(i);
        }
    }
    for (auto& st : streams_) {
        st.freq = 1;
    }
    created_ = true;
    beginFrame();
    if (!resize(options.width, options.height)) {
        return false;
    }
    bindless_.recordInit(frames_.cmd());
    spdlog::info("d3d9: device ready, back buffer {}x{}, depth {} ({})", options.width, options.height,
                 options.depthFormat != 0 ? vk::d3dFormatName(options.depthFormat) : "none",
                 d24s8_ == VK_FORMAT_D24_UNORM_S8_UINT ? "D24S8" : "D32S8 for D24S8");
    return true;
}

bool Device::resize(std::uint32_t width, std::uint32_t height) {
    endRendering();
    width = std::max(1u, width);
    height = std::max(1u, height);
    TextureDesc bb;
    bb.width = width;
    bb.height = height;
    bb.format = vk::d3dfmt::X8R8G8B8;
    bb.usage = USAGE_RENDERTARGET;
    bb.pool = POOL_DEFAULT;
    auto newBack = createTexture(bb);
    if (!newBack || !newBack->valid()) {
        return false;
    }
    std::unique_ptr<Texture> newDepth;
    if (options_.depthFormat != 0) {
        TextureDesc dd = bb;
        dd.format = options_.depthFormat;
        dd.usage = USAGE_DEPTHSTENCIL;
        newDepth = createTexture(dd);
    }
    if (backBuffer_) {
        release(std::move(backBuffer_));
    }
    if (depthBuffer_) {
        release(std::move(depthBuffer_));
    }
    backBuffer_ = std::move(newBack);
    depthBuffer_ = std::move(newDepth);
    backBufferSurface_ = backBuffer_->surface(0, 0);
    depthSurface_ = depthBuffer_ ? depthBuffer_->surface(0, 0) : nullptr;
    rt_ = {};
    rt_[0] = backBufferSurface_;
    ds_ = depthSurface_;
    viewport_ = {0, 0, width, height, 0.0f, 1.0f};
    scissor_[0] = 0;
    scissor_[1] = 0;
    scissor_[2] = static_cast<std::int32_t>(width);
    scissor_[3] = static_cast<std::int32_t>(height);
    options_.width = width;
    options_.height = height;
    if (swapchain_.valid() && (swapchain_.extent().width != width || swapchain_.extent().height != height)) {
        swapchain_.recreate(width, height);
    }
    return true;
}

void Device::destroy() {
    if (!created_) {
        return;
    }
    frames_.waitIdle();
    processReleases();
    backBuffer_.reset();
    depthBuffer_.reset();
    {
        const std::lock_guard lock(shaderMutex_);
        for (auto& [h, s] : shaders_) {
            if (s->module != VK_NULL_HANDLE) {
                vkDestroyShaderModule(ctx_.device(), s->module, nullptr);
                s->module = VK_NULL_HANDLE;
            }
        }
        shaders_.clear();
    }
    vs_.reset();
    ps_.reset();
    for (auto s : samplerObjects_) {
        vkDestroySampler(ctx_.device(), s, nullptr);
    }
    samplerObjects_.clear();
    samplers_.clear();
    if (nullVertexBuffer_ != VK_NULL_HANDLE) {
        vmaDestroyBuffer(ctx_.allocator(), nullVertexBuffer_, nullVertexAlloc_);
        nullVertexBuffer_ = VK_NULL_HANDLE;
    }
    for (auto& s : acquireSemaphores_) {
        if (s != VK_NULL_HANDLE) {
            vkDestroySemaphore(ctx_.device(), s, nullptr);
            s = VK_NULL_HANDLE;
        }
    }
    swapchain_.destroy();
    pipelines_.destroy();
    // frames_ runs the retired destructors (images, bindless slot releases): before the bindless heap goes away
    frames_.destroy();
    bindless_.destroy();
    ctx_.destroy();
    created_ = false;
}

// ---- frames ---------------------------------------------------------------------------------------------------------

void Device::beginFrame() {
    frames_.begin();
    processReleases();
    uploadUsed_ = false;
    rendering_ = false;
    boundPipeline_ = VK_NULL_HANDLE;
    vsConstDirty_ = psConstDirty_ = true;
    const VkDescriptorSet set = bindless_.set();
    vkCmdBindDescriptorSets(frames_.cmd(), VK_PIPELINE_BIND_POINT_GRAPHICS, bindless_.pipelineLayout(), 0, 1, &set, 0, nullptr);
    stats_.draws = 0;
    stats_.renderPasses = 0;
    stats_.drawsSkipped = 0;
}

VkCommandBuffer Device::uploadCommands() {
    VkCommandBuffer cmd = frames_.uploadCmd();
    if (!uploadUsed_) {
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
    }
    return cmd;
}

void Device::submitFrame(VkSemaphore waitAcquire, VkSemaphore signalRendered) {
    endRendering();
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
    }
    frames_.submit(waitAcquire, signalRendered);
}

void Device::present() {
    endRendering();
    ++stats_.frames;
    if (options_.offscreen || !swapchain_.valid()) {
        submitFrame(VK_NULL_HANDLE, VK_NULL_HANDLE);
        beginFrame();
        return;
    }
    const std::uint32_t slot = static_cast<std::uint32_t>(frames_.currentValue() % vk::Frames::kInFlight);
    VkSemaphore acquired = acquireSemaphores_[slot];
    std::uint32_t imageIndex = 0;
    if (!swapchain_.acquire(acquired, imageIndex)) {
        // minimised or out of date: drop the frame's presentation, keep its work
        submitFrame(VK_NULL_HANDLE, VK_NULL_HANDLE);
        RECT rc{};
        GetClientRect(options_.window, &rc);
        if (rc.right > 0 && rc.bottom > 0) {
            swapchain_.recreate(static_cast<std::uint32_t>(rc.right), static_cast<std::uint32_t>(rc.bottom));
        }
        beginFrame();
        return;
    }
    VkCommandBuffer cmd = frames_.cmd();
    vk::Image& bb = backBuffer_->image();
    bb.transition(cmd, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    VkImageMemoryBarrier2 b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
    b.srcStageMask = VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT;
    b.dstStageMask = VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT;
    b.dstAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
    b.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    b.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    b.image = swapchain_.image(imageIndex);
    b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    VkDependencyInfo di{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    di.imageMemoryBarrierCount = 1;
    di.pImageMemoryBarriers = &b;
    vkCmdPipelineBarrier2(cmd, &di);
    const VkExtent2D ext = swapchain_.extent();
    VkImageBlit2 region{VK_STRUCTURE_TYPE_IMAGE_BLIT_2};
    region.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    region.srcOffsets[1] = {static_cast<std::int32_t>(backBuffer_->desc().width), static_cast<std::int32_t>(backBuffer_->desc().height), 1};
    region.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    region.dstOffsets[1] = {static_cast<std::int32_t>(ext.width), static_cast<std::int32_t>(ext.height), 1};
    VkBlitImageInfo2 bi{VK_STRUCTURE_TYPE_BLIT_IMAGE_INFO_2};
    bi.srcImage = bb.image();
    bi.srcImageLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    bi.dstImage = swapchain_.image(imageIndex);
    bi.dstImageLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    bi.regionCount = 1;
    bi.pRegions = &region;
    bi.filter = VK_FILTER_LINEAR;
    vkCmdBlitImage2(cmd, &bi);
    b.srcStageMask = VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT;
    b.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
    b.dstStageMask = VK_PIPELINE_STAGE_2_NONE;
    b.dstAccessMask = VK_ACCESS_2_NONE;
    b.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    b.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    vkCmdPipelineBarrier2(cmd, &di);
    submitFrame(acquired, swapchain_.renderedSemaphore(imageIndex));
    const VkResult r = swapchain_.present(imageIndex);
    if (r == VK_ERROR_OUT_OF_DATE_KHR || r == VK_SUBOPTIMAL_KHR) {
        RECT rc{};
        GetClientRect(options_.window, &rc);
        if (rc.right > 0 && rc.bottom > 0) {
            swapchain_.recreate(static_cast<std::uint32_t>(rc.right), static_cast<std::uint32_t>(rc.bottom));
        }
    } else if (r != VK_SUCCESS) {
        spdlog::error("vulkan: present: {}", vk::resultString(r));
    }
    beginFrame();
}

// ---- resources ------------------------------------------------------------------------------------------------------

std::unique_ptr<Texture> Device::createTexture(const TextureDesc& desc) {
    auto t = std::make_unique<Texture>(*this, desc);
    if (!t->valid()) {
        return nullptr;
    }
    return t;
}

std::unique_ptr<Buffer> Device::createBuffer(std::uint32_t size, std::uint32_t usage, std::uint32_t pool, bool index, bool index32) {
    return std::make_unique<Buffer>(*this, size, usage, pool, index, index32);
}

std::unique_ptr<VertexDecl> Device::createVertexDecl(std::span<const VertexElement> elements) {
    return std::make_unique<VertexDecl>(elements);
}

std::shared_ptr<Shader> Device::createShader(std::span<const std::uint32_t> bytecode) {
    const std::size_t bytes = shader::byteLength(bytecode.data(), bytecode.size());
    if (bytes == 0) {
        spdlog::warn("d3d9: shader bytecode without END token");
        return nullptr;
    }
    const std::span<const std::uint32_t> tokens = bytecode.first(bytes / 4);
    const std::uint64_t hash = fnv1a(tokens.data(), tokens.size_bytes(), fnv1a(&kTranslatorVersion, sizeof(kTranslatorVersion)));
    {
        const std::lock_guard lock(shaderMutex_);
        if (auto it = shaders_.find(hash); it != shaders_.end()) {
            return it->second;
        }
    }
    const auto dec = shader::decode(tokens);
    if (!dec.ok()) {
        spdlog::warn("d3d9: shader {:016x}: {}", hash, dec.error);
        return nullptr;
    }
    const auto glsl = shader::toGlsl(dec.program);
    if (!glsl.ok()) {
        spdlog::warn("d3d9: shader {:016x}: {}", hash, glsl.error);
        return nullptr;
    }
    std::vector<std::uint32_t> spirv;
    const auto cacheFile = options_.cacheDir.empty() ? std::filesystem::path()
                                                     : options_.cacheDir / "spirv" / std::format("{:016x}.spv", hash);
    if (!cacheFile.empty()) {
        std::ifstream in(cacheFile, std::ios::binary | std::ios::ate);
        if (in) {
            const auto size = static_cast<std::size_t>(in.tellg());
            spirv.resize(size / 4);
            in.seekg(0);
            in.read(reinterpret_cast<char*>(spirv.data()), static_cast<std::streamsize>(spirv.size() * 4));
        }
    }
    if (spirv.empty()) {
        auto compiled = shader::compileGlsl(dec.program.version.stage, glsl.glsl);
        if (!compiled.ok()) {
            spdlog::error("d3d9: shader {:016x} does not compile:\n{}", hash, compiled.log);
            return nullptr;
        }
        spirv = std::move(compiled.words);
        if (!cacheFile.empty()) {
            std::error_code ec;
            std::filesystem::create_directories(cacheFile.parent_path(), ec);
            std::ofstream out(cacheFile, std::ios::binary);
            out.write(reinterpret_cast<const char*>(spirv.data()), static_cast<std::streamsize>(spirv.size() * 4));
        }
    }
    auto s = std::make_shared<Shader>();
    s->stage = dec.program.version.stage;
    s->module = vk::createShaderModule(ctx_, spirv);
    s->reflection = glsl.reflection;
    s->bytecode.assign(tokens.begin(), tokens.end());
    s->hash = hash;
    if (s->module == VK_NULL_HANDLE) {
        return nullptr;
    }
    const std::lock_guard lock(shaderMutex_);
    auto [it, inserted] = shaders_.emplace(hash, s);
    if (!inserted) {  // another thread was faster
        vkDestroyShaderModule(ctx_.device(), s->module, nullptr);
    }
    return it->second;
}

void Device::release(std::unique_ptr<Texture> texture) {
    if (texture) {
        const std::lock_guard lock(releaseMutex_);
        releasedTextures_.push_back(std::move(texture));
    }
}
void Device::release(std::unique_ptr<Buffer> buffer) {
    if (buffer) {
        const std::lock_guard lock(releaseMutex_);
        releasedBuffers_.push_back(std::move(buffer));
    }
}
void Device::release(std::unique_ptr<VertexDecl> decl) {
    if (decl) {
        const std::lock_guard lock(releaseMutex_);
        releasedDecls_.push_back(std::move(decl));
    }
}

void Device::processReleases() {
    std::vector<std::unique_ptr<Texture>> textures;
    std::vector<std::unique_ptr<Buffer>> buffers;
    std::vector<std::unique_ptr<VertexDecl>> decls;
    {
        const std::lock_guard lock(releaseMutex_);
        textures.swap(releasedTextures_);
        buffers.swap(releasedBuffers_);
        decls.swap(releasedDecls_);
    }
    for (auto& t : textures) {
        for (auto& bound : tex_) {
            if (bound == t.get()) {
                bound = nullptr;
            }
        }
        for (auto& r : rt_) {
            if (r != nullptr && r->texture() == t.get()) {
                r = nullptr;
            }
        }
        if (ds_ != nullptr && ds_->texture() == t.get()) {
            ds_ = nullptr;
        }
    }
    for (auto& b : buffers) {
        for (auto& st : streams_) {
            if (st.buffer == b.get()) {
                st.buffer = nullptr;
            }
        }
        if (indices_ == b.get()) {
            indices_ = nullptr;
        }
    }
    for (auto& d : decls) {
        if (decl_ == d.get()) {
            decl_ = nullptr;
        }
    }
    // destructors retire the GPU objects through Frames
}

// ---- state ----------------------------------------------------------------------------------------------------------

void Device::setRenderState(std::uint32_t rs, std::uint32_t value) {
    if (rs < RS_COUNT) {
        rs_[rs] = value;
    }
}

void Device::setSamplerState(std::uint32_t sampler, std::uint32_t ss, std::uint32_t value) {
    const std::uint32_t slot = slotOf(sampler);
    if (slot < kSamplerCount && ss < SAMP_COUNT) {
        samp_[slot][ss] = value;
    }
}

std::uint32_t Device::samplerState(std::uint32_t sampler, std::uint32_t ss) const {
    const std::uint32_t slot = slotOf(sampler);
    return slot < kSamplerCount && ss < SAMP_COUNT ? samp_[slot][ss] : 0;
}

void Device::setTexture(std::uint32_t sampler, Texture* texture) {
    const std::uint32_t slot = slotOf(sampler);
    if (slot < kSamplerCount) {
        tex_[slot] = texture;
    }
}

Texture* Device::texture(std::uint32_t sampler) const {
    const std::uint32_t slot = slotOf(sampler);
    return slot < kSamplerCount ? tex_[slot] : nullptr;
}

void Device::setStreamSource(std::uint32_t stream, Buffer* buffer, std::uint32_t offset, std::uint32_t stride) {
    if (stream < kStreams) {
        streams_[stream].buffer = buffer;
        streams_[stream].offset = offset;
        streams_[stream].stride = stride;
    }
}

void Device::setStreamSourceFreq(std::uint32_t stream, std::uint32_t setting) {
    if (stream < kStreams) {
        streams_[stream].freq = setting;
    }
}

void Device::setIndices(Buffer* buffer) { indices_ = buffer; }
void Device::setVertexDecl(VertexDecl* decl) { decl_ = decl; }

void Device::setVertexShader(std::shared_ptr<Shader> shader) {
    if (shader != vs_) {
        vs_ = std::move(shader);
        vsConstDirty_ = true;
    }
}

void Device::setPixelShader(std::shared_ptr<Shader> shader) {
    if (shader != ps_) {
        ps_ = std::move(shader);
        psConstDirty_ = true;
    }
}

void Device::setVertexShaderConstantF(std::uint32_t start, const float* data, std::uint32_t count) {
    if (start >= kVsFloatConsts || data == nullptr) return;
    count = std::min(count, kVsFloatConsts - start);
    std::memcpy(vsF_[start], data, count * 16);
    vsConstDirty_ = true;
}

void Device::setPixelShaderConstantF(std::uint32_t start, const float* data, std::uint32_t count) {
    if (start >= kPsFloatConsts || data == nullptr) return;
    count = std::min(count, kPsFloatConsts - start);
    std::memcpy(psF_[start], data, count * 16);
    psConstDirty_ = true;
}

void Device::setVertexShaderConstantI(std::uint32_t start, const std::int32_t* data, std::uint32_t count) {
    if (start >= 16 || data == nullptr) return;
    count = std::min(count, 16 - start);
    std::memcpy(vsI_[start], data, count * 16);
    vsConstDirty_ = true;
}

void Device::setPixelShaderConstantI(std::uint32_t start, const std::int32_t* data, std::uint32_t count) {
    if (start >= 16 || data == nullptr) return;
    count = std::min(count, 16 - start);
    std::memcpy(psI_[start], data, count * 16);
    psConstDirty_ = true;
}

void Device::setVertexShaderConstantB(std::uint32_t start, const std::int32_t* data, std::uint32_t count) {
    if (start >= 16 || data == nullptr) return;
    count = std::min(count, 16 - start);
    for (std::uint32_t i = 0; i < count; ++i) vsB_[start + i] = data[i] != 0 ? 1u : 0u;
    vsConstDirty_ = true;
}

void Device::setPixelShaderConstantB(std::uint32_t start, const std::int32_t* data, std::uint32_t count) {
    if (start >= 16 || data == nullptr) return;
    count = std::min(count, 16 - start);
    for (std::uint32_t i = 0; i < count; ++i) psB_[start + i] = data[i] != 0 ? 1u : 0u;
    psConstDirty_ = true;
}

void Device::getVertexShaderConstantF(std::uint32_t start, float* data, std::uint32_t count) const {
    if (start >= kVsFloatConsts || data == nullptr) return;
    std::memcpy(data, vsF_[start], std::min(count, kVsFloatConsts - start) * 16);
}

void Device::getPixelShaderConstantF(std::uint32_t start, float* data, std::uint32_t count) const {
    if (start >= kPsFloatConsts || data == nullptr) return;
    std::memcpy(data, psF_[start], std::min(count, kPsFloatConsts - start) * 16);
}

void Device::setRenderTarget(std::uint32_t index, Surface* surface) {
    if (index >= kRenderTargets || rt_[index] == surface) {
        return;
    }
    endRendering();
    rt_[index] = surface;
    if (index == 0 && surface != nullptr) {
        // D3D9 resets the viewport and the scissor rect to the new render target
        viewport_ = {0, 0, surface->width(), surface->height(), 0.0f, 1.0f};
        scissor_[0] = 0;
        scissor_[1] = 0;
        scissor_[2] = static_cast<std::int32_t>(surface->width());
        scissor_[3] = static_cast<std::int32_t>(surface->height());
    }
}

void Device::setDepthStencil(Surface* surface) {
    if (ds_ == surface) {
        return;
    }
    endRendering();
    ds_ = surface;
}

void Device::setViewport(const Viewport& vp) { viewport_ = vp; }

void Device::setScissorRect(std::int32_t left, std::int32_t top, std::int32_t right, std::int32_t bottom) {
    scissor_[0] = left;
    scissor_[1] = top;
    scissor_[2] = right;
    scissor_[3] = bottom;
}

// ---- commands -------------------------------------------------------------------------------------------------------

void Device::clear(std::uint32_t flags, std::uint32_t color, float z, std::uint32_t stencil) {
    if (!beginRendering()) {
        return;
    }
    // D3D9 clears the viewport rectangle (and the scissor rect when the scissor test is on)
    std::int32_t x0 = static_cast<std::int32_t>(viewport_.x), y0 = static_cast<std::int32_t>(viewport_.y);
    std::int32_t x1 = x0 + static_cast<std::int32_t>(viewport_.width), y1 = y0 + static_cast<std::int32_t>(viewport_.height);
    if (rs_[RS_SCISSORTESTENABLE] != 0) {
        x0 = std::max(x0, scissor_[0]);
        y0 = std::max(y0, scissor_[1]);
        x1 = std::min(x1, scissor_[2]);
        y1 = std::min(y1, scissor_[3]);
    }
    x0 = std::max(x0, 0);
    y0 = std::max(y0, 0);
    x1 = std::min(x1, static_cast<std::int32_t>(targetWidth_));
    y1 = std::min(y1, static_cast<std::int32_t>(targetHeight_));
    if (x1 <= x0 || y1 <= y0) {
        return;
    }
    const VkClearRect rect{{{x0, y0}, {static_cast<std::uint32_t>(x1 - x0), static_cast<std::uint32_t>(y1 - y0)}}, 0, 1};
    VkClearAttachment atts[kRenderTargets + 1]{};
    std::uint32_t n = 0;
    if ((flags & CLEAR_TARGET) != 0) {
        float c[4];
        colorToFloat4(color, c);
        for (std::uint32_t i = 0; i < colorCount_; ++i) {
            atts[n].aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            atts[n].colorAttachment = i;
            std::memcpy(atts[n].clearValue.color.float32, c, sizeof(c));
            ++n;
        }
    }
    VkImageAspectFlags ds = 0;
    if ((flags & CLEAR_ZBUFFER) != 0 && depthFormat_ != VK_FORMAT_UNDEFINED) ds |= VK_IMAGE_ASPECT_DEPTH_BIT;
    if ((flags & CLEAR_STENCIL) != 0 && stencilFormat_ != VK_FORMAT_UNDEFINED) ds |= VK_IMAGE_ASPECT_STENCIL_BIT;
    if (ds != 0) {
        atts[n].aspectMask = ds;
        atts[n].clearValue.depthStencil = {std::clamp(z, 0.0f, 1.0f), stencil & 0xFF};
        ++n;
    }
    if (n > 0) {
        vkCmdClearAttachments(frames_.cmd(), n, atts, 1, &rect);
    }
}

void Device::blit(vk::Image& src, std::uint32_t srcLevel, std::uint32_t srcLayer, const VkOffset3D srcBox[2], vk::Image& dst,
                  std::uint32_t dstLevel, std::uint32_t dstLayer, const VkOffset3D dstBox[2], VkFilter filter) {
    VkCommandBuffer cmd = frames_.cmd();
    src.transition(cmd, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    dst.transition(cmd, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    VkImageBlit2 region{VK_STRUCTURE_TYPE_IMAGE_BLIT_2};
    region.srcSubresource = {src.aspect(), srcLevel, srcLayer, 1};
    region.srcOffsets[0] = srcBox[0];
    region.srcOffsets[1] = srcBox[1];
    region.dstSubresource = {dst.aspect(), dstLevel, dstLayer, 1};
    region.dstOffsets[0] = dstBox[0];
    region.dstOffsets[1] = dstBox[1];
    VkBlitImageInfo2 bi{VK_STRUCTURE_TYPE_BLIT_IMAGE_INFO_2};
    bi.srcImage = src.image();
    bi.srcImageLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    bi.dstImage = dst.image();
    bi.dstImageLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    bi.regionCount = 1;
    bi.pRegions = &region;
    bi.filter = vk::isDepthFormat(src.format()) ? VK_FILTER_NEAREST : filter;
    vkCmdBlitImage2(cmd, &bi);
}

void Device::stretchRect(Surface* src, const std::int32_t* srcRect, Surface* dst, const std::int32_t* dstRect, bool linear) {
    if (src == nullptr || dst == nullptr || src->texture()->isNull() || dst->texture()->isNull()) {
        return;
    }
    endRendering();
    auto box = [](const std::int32_t* r, Surface* s, VkOffset3D out[2]) {
        if (r != nullptr) {
            out[0] = {r[0], r[1], 0};
            out[1] = {r[2], r[3], 1};
        } else {
            out[0] = {0, 0, 0};
            out[1] = {static_cast<std::int32_t>(s->width()), static_cast<std::int32_t>(s->height()), 1};
        }
    };
    VkOffset3D sb[2], db[2];
    box(srcRect, src, sb);
    box(dstRect, dst, db);
    blit(src->texture()->image(), src->level(), src->face(), sb, dst->texture()->image(), dst->level(), dst->face(), db,
         linear ? VK_FILTER_LINEAR : VK_FILTER_NEAREST);
    src->texture()->lastUseFrame_ = frameValue();
    dst->texture()->lastUseFrame_ = frameValue();
}

bool Device::readRenderTarget(Surface* src, Texture* dst, std::uint32_t dstLevel) {
    if (src == nullptr || dst == nullptr || src->texture()->isNull()) {
        return false;
    }
    std::vector<std::uint8_t> pixels;
    const std::uint32_t w = src->width(), h = src->height();
    const vk::FormatInfo& f = src->texture()->format();
    const VkDeviceSize size = vk::storageSize(f, w, h);
    endRendering();
    VkBuffer rb = VK_NULL_HANDLE;
    VmaAllocation ra = VK_NULL_HANDLE;
    VmaAllocationInfo info{};
    VkBufferCreateInfo bci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bci.size = size;
    bci.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    VmaAllocationCreateInfo aci{};
    aci.usage = VMA_MEMORY_USAGE_AUTO;
    aci.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;
    if (vmaCreateBuffer(ctx_.allocator(), &bci, &aci, &rb, &ra, &info) != VK_SUCCESS) {
        return false;
    }
    vk::Image& img = src->texture()->image();
    img.transition(frames_.cmd(), VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    VkBufferImageCopy region{};
    region.imageSubresource = {img.aspect() & ~VkImageAspectFlags(VK_IMAGE_ASPECT_STENCIL_BIT), src->level(), src->face(), 1};
    region.imageExtent = {w, h, 1};
    vkCmdCopyImageToBuffer(frames_.cmd(), img.image(), VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, rb, 1, &region);
    // the copy must finish before the CPU reads: end the frame's work here and start a new command buffer
    submitFrame(VK_NULL_HANDLE, VK_NULL_HANDLE);
    frames_.waitIdle();
    beginFrame();
    Texture::Locked lk;
    const bool ok = dst->lock(dstLevel, 0, nullptr, 0, lk);
    if (ok) {
        const std::uint32_t rowBytes = static_cast<std::uint32_t>(size / h);
        for (std::uint32_t y = 0; y < h; ++y) {
            std::memcpy(static_cast<std::uint8_t*>(lk.bits) + static_cast<std::size_t>(y) * lk.rowPitch,
                        static_cast<const std::uint8_t*>(info.pMappedData) + static_cast<std::size_t>(y) * rowBytes,
                        std::min(rowBytes, lk.rowPitch));
        }
        dst->unlock(dstLevel, 0);
    }
    vmaDestroyBuffer(ctx_.allocator(), rb, ra);
    return ok;
}

bool Device::readBackBuffer(std::vector<std::uint8_t>& bgra, std::uint32_t& width, std::uint32_t& height) {
    TextureDesc d;
    d.width = backBuffer_->desc().width;
    d.height = backBuffer_->desc().height;
    d.format = vk::d3dfmt::A8R8G8B8;
    d.pool = POOL_SYSTEMMEM;
    auto tmp = createTexture(d);
    if (!tmp || !readRenderTarget(backBufferSurface_, tmp.get(), 0)) {
        return false;
    }
    Texture::Locked lk;
    if (!tmp->lock(0, 0, nullptr, LOCK_READONLY, lk)) {
        return false;
    }
    width = d.width;
    height = d.height;
    bgra.assign(static_cast<const std::uint8_t*>(lk.bits), static_cast<const std::uint8_t*>(lk.bits) + lk.rowPitch * height);
    release(std::move(tmp));
    return true;
}

// ---- samplers -------------------------------------------------------------------------------------------------------

bool Device::SamplerKey::operator==(const SamplerKey& o) const { return std::memcmp(states, o.states, sizeof(states)) == 0; }

std::size_t Device::SamplerKeyHash::operator()(const SamplerKey& k) const {
    return static_cast<std::size_t>(fnv1a(k.states, sizeof(k.states)));
}

std::uint32_t Device::samplerIndex(std::uint32_t slot) {
    SamplerKey key{};
    std::memcpy(key.states, samp_[slot].data(), sizeof(key.states));
    key.states[SAMP_ELEMENTINDEX] = 0;
    key.states[SAMP_DMAPOFFSET] = 0;
    key.states[SAMP_SRGBTEXTURE] = 0;
    const std::lock_guard lock(samplerMutex_);
    if (auto it = samplers_.find(key); it != samplers_.end()) {
        return it->second;
    }
    const auto& s = key.states;
    VkSamplerCreateInfo ci{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    ci.magFilter = filter(s[SAMP_MAGFILTER]);
    ci.minFilter = filter(s[SAMP_MINFILTER]);
    ci.mipmapMode = mipmapMode(s[SAMP_MIPFILTER]);
    ci.addressModeU = addressMode(s[SAMP_ADDRESSU]);
    ci.addressModeV = addressMode(s[SAMP_ADDRESSV]);
    ci.addressModeW = addressMode(s[SAMP_ADDRESSW]);
    ci.mipLodBias = std::clamp(asFloat(s[SAMP_MIPMAPLODBIAS]), -15.0f, 15.0f);
    const bool aniso = (s[SAMP_MINFILTER] == TEXF_ANISOTROPIC || s[SAMP_MAGFILTER] == TEXF_ANISOTROPIC) &&
                       ctx_.features().samplerAnisotropy;
    ci.anisotropyEnable = aniso ? VK_TRUE : VK_FALSE;
    ci.maxAnisotropy = aniso ? std::clamp(static_cast<float>(s[SAMP_MAXANISOTROPY]), 1.0f, ctx_.features().maxAnisotropy) : 1.0f;
    // MAXMIPLEVEL is the most detailed level used; MIPFILTER NONE samples that level only
    ci.minLod = static_cast<float>(s[SAMP_MAXMIPLEVEL]);
    ci.maxLod = s[SAMP_MIPFILTER] == TEXF_NONE ? ci.minLod : VK_LOD_CLAMP_NONE;
    const std::uint32_t border = s[SAMP_BORDERCOLOR];
    const bool white = (border & 0x00FFFFFF) == 0x00FFFFFF;
    const bool opaque = (border >> 24) >= 0x80;
    ci.borderColor = white ? VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE
                           : (opaque ? VK_BORDER_COLOR_FLOAT_OPAQUE_BLACK : VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK);
    VkSampler sampler = VK_NULL_HANDLE;
    if (vkCreateSampler(ctx_.device(), &ci, nullptr, &sampler) != VK_SUCCESS) {
        return 0;
    }
    samplerObjects_.push_back(sampler);
    const std::uint32_t index = bindless_.addSampler(sampler);
    samplers_.emplace(key, index);
    return index;
}

}  // namespace rvk::d3d9
