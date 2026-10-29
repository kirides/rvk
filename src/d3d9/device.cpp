// d3d9::Device: creation, frames, resources, state setters, clears, copies. Draw preparation is in draw.cpp.

#include "d3d9/device.h"
#include "shader/uber.h"

#include <windows.h>

#include <chrono>
#include <charconv>
#include <cmath>
#include <cstring>
#include <fstream>

#include <spdlog/spdlog.h>

#include "d3d9/convert.h"
#include "d3d9/memeq.h"
#include "d3d9/target_names.h"
#include "shader/sm_ir.h"
#include "shader/spirv.h"
#include "vk/debug_names.h"

namespace rvk::d3d9 {
namespace {

// bump when the translator's output changes: invalidates the SPIR-V disk cache
// 2: the ABI declarations come from shaders/include/rvk_abi.glsl (48-byte push block, FrameBlock)
constexpr std::uint32_t kTranslatorVersion = 4;
// bump when the tint epilogue (GlslOptions::tintEpilogue) changes: the tint variants' cache files
constexpr std::uint32_t kTintVersion = 2;
// bump when the billboard fade variant (Device::lodModule) changes
constexpr std::uint32_t kLodVariantVersion = 1;

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

// ---- frame semantics (d3d9/scene.h) -----------------------------------------------------------------------------------------

double Device::secondsSinceCreate() const {
    return std::chrono::duration<double>(std::chrono::steady_clock::now() - createdAt_).count();
}

bool Device::setMainCamera(const scene::CameraInput& in) {
    if (camera_.valid) return false;
    camera_.set(in, stats_.frames + 1, secondsSinceCreate());
    frameAddr_ = 0;  // the frame block gets the camera
    return true;
}

void Device::tagImage(Texture* texture, scene::ImageRole role, const char* label, std::uint32_t labelIndex) {
    if (texture == nullptr || role == scene::ImageRole::None) return;
    if (roles_[static_cast<std::size_t>(role)] != texture) frameAddr_ = 0;  // the frame block lists the role images
    roles_[static_cast<std::size_t>(role)] = texture;
    if (texture->role_ == role) {
        if (label != nullptr) texture->setDebugLabel(label, labelIndex);  // a finer name for the same role (no-op when unchanged)
        return;
    }
    const scene::ImageRole old = texture->role_;
    if (old != scene::ImageRole::None && roles_[static_cast<std::size_t>(old)] == texture) {
        roles_[static_cast<std::size_t>(old)] = nullptr;
    }
    texture->role_ = role;
    texture->setDebugLabel(label != nullptr ? label : scene::name(role), label != nullptr ? labelIndex : ~0u);
    // logged when found; a pooled target that keeps changing roles would flood the log: the first 64 changes only
    if (roleLogs_ < 64) {
        ++roleLogs_;
        const TextureDesc& d = texture->desc();
        spdlog::info("scene: {} = texture {} ({}x{} {}{}{}) at frame {}{}", scene::name(role), static_cast<void*>(texture), d.width,
                     d.height, vk::d3dFormatName(d.format), d.type == TextureType::Cube ? " cube" : "",
                     texture->isDepth() ? " depth" : "", stats_.frames + 1,
                     old != scene::ImageRole::None ? std::format(" (was the {})", scene::name(old)) : std::string());
    }
}

void Device::forgetImage(Texture* texture) {
    for (auto& r : roles_) {
        if (r == texture) {
            r = nullptr;
            frameAddr_ = 0;
        }
    }
}

// ---- enhancements (d3d9/inject.h, docs/research/injection.md) ----------------------------------------------------------------

VkDeviceAddress Device::frameBlockAddress() {
    if (frameAddr_ != 0) return frameAddr_;
    scene::FrameExtras x;
    x.frameIndex = stats_.frames + 1;
    x.now = secondsSinceCreate();
    x.mainViews = firings_.mainViews();
    if (const Texture* hdr = imageWithRole(scene::ImageRole::SceneHdr)) {
        x.width = static_cast<float>(hdr->desc().width);
        x.height = static_cast<float>(hdr->desc().height);
    } else {
        x.width = static_cast<float>(options_.width);
        x.height = static_cast<float>(options_.height);
    }
    const auto slot = [&](scene::ImageRole r) {
        const Texture* t = imageWithRole(r);
        return t != nullptr ? t->bindlessSlot() : 0u;
    };
    x.images = {slot(scene::ImageRole::SceneHdr), slot(scene::ImageRole::Refraction), slot(scene::ImageRole::DepthMap),
                slot(scene::ImageRole::SkyEnvCube)};
    x.params = &frameParams_;
    x.reverseZ = reverseZ_;
    if (rtSceneValid()) {
        x.rtTlas = rtScene_.tlas;
        x.rtRecords = rtScene_.records;
        x.rtInstances = rtScene_.instances;
        x.rtRecordCount = rtScene_.recordCount;
    }
    if (!giHeader_.empty()) {
        const vk::RingAlloc g = frames_.allocate(giHeader_.size(), 16);
        if (g.ptr != nullptr) {
            std::memcpy(g.ptr, giHeader_.data(), giHeader_.size());
            x.gi = g.address;
        }
    }
    x.ao = ao_;
    x.sgi = sgi_;
    x.jitter = jitter_;
    const vk::RingAlloc a = frames_.allocate(sizeof(shader::abi::FrameBlock), 16);
    scene::fillFrameBlock(*static_cast<shader::abi::FrameBlock*>(a.ptr), camera_, lastCamera_, x);
    frameAddr_ = a.address;
    ++perf_.frameBlocks;
    return frameAddr_;
}

void Device::setFrameParam(std::uint32_t index, const std::array<float, 4>& value) {
    if (index >= frameParams_.size() || frameParams_[index] == value) return;
    frameParams_[index] = value;
    frameAddr_ = 0;
}

std::uint32_t Device::addPass(inject::Point point, std::string name, inject::PassFn fn, const void* owner, int order) {
    if (injecting_) {
        spdlog::error("d3d9: addPass({}) inside an injected pass ignored", name);
        return 0;
    }
    const std::string label = name;
    const std::uint32_t id = passes_.add(point, std::move(name), std::move(fn), owner, order);
    spdlog::info("d3d9: pass '{}' at {} (id {}, {} passes)", label, inject::name(point), id, passes_.size());
    return id;
}

void Device::removePass(std::uint32_t id) {
    if (injecting_) {
        spdlog::error("d3d9: removePass({}) inside an injected pass ignored", id);
        return;
    }
    if (passes_.remove(id)) spdlog::info("d3d9: pass {} removed ({} passes)", id, passes_.size());
}

void Device::removePasses(const void* owner) {
    if (!injecting_) passes_.removeOwner(owner);
}

void Device::beginMainView() {
    firings_.beginMainView();
    if (firings_.mainViews() != 1) return;  // a frame's later main views keep its jitter
    std::array<float, 4> j{};
    if (jitterSource_) {
        const std::array<float, 2> now = jitterSource_(stats_.frames + 1);
        j = {now[0], now[1], jitter_[0], jitter_[1]};
    }
    if (j != jitter_) {
        jitter_ = j;
        frameAddr_ = 0;
    }
}

void Device::setJitterSource(JitterSource source) {
    jitterSource_ = std::move(source);
    if (!jitterSource_ && jitter_ != std::array<float, 4>{}) {
        jitter_ = {};
        frameAddr_ = 0;
    }
}

void Device::geometryJitter(float& ndcX, float& ndcY) const {
    ndcX = ndcY = 0.0f;
    if (jitter_[0] == 0.0f && jitter_[1] == 0.0f) return;
    const scene::Phase ph = phases_.current();
    if (ph.view != scene::View::Main) return;
    if (ph.pass != scene::Pass::DepthPrepass && ph.pass != scene::Pass::Solid && ph.pass != scene::Pass::Refracted &&
        ph.pass != scene::Pass::Alpha && ph.pass != scene::Pass::AlphaLate) {
        return;
    }
    const Texture* hdr = imageWithRole(scene::ImageRole::SceneHdr);
    if (hdr == nullptr || targetWidth_ != hdr->desc().width || targetHeight_ != hdr->desc().height) return;
    // the projection translation FSR documents (Cauldron: +2 jx / w, -2 jy / h in y-up NDC; posFix is in that space)
    ndcX = 2.0f * jitter_[0] / static_cast<float>(targetWidth_);
    ndcY = -2.0f * jitter_[1] / static_cast<float>(targetHeight_);
}

void Device::inject(inject::Point point) {
    if (!passes_.any(point) || injecting_ || !created_) return;  // the common case: nothing registered
    const bool inMain = phases_.view() == scene::View::Main;
    if (!firings_.take(point, inMain)) return;
    syncPhaseRegions();
    const auto t0 = std::chrono::steady_clock::now();
    endRendering();
    VkCommandBuffer cmd = frames_.cmd();
    gpuMark("before injection");
    Surface* rt0 = rt_[0];
    PassContext ctx{*this, cmd, point};
    ctx.mainView = inject::inMainView(point) ? firings_.mainViews() : 0;
    ctx.frameBlock = frameBlockAddress();
    ctx.sceneHdr = imageWithRole(scene::ImageRole::SceneHdr);
    ctx.depthMap = imageWithRole(scene::ImageRole::DepthMap);
    ctx.refraction = imageWithRole(scene::ImageRole::Refraction);
    ctx.target = rt0 != nullptr && !rt0->texture()->isNull() ? rt0->texture() : nullptr;
    injecting_ = true;
    const auto& list = passes_.passes(point);
    for (std::size_t i = 0; i < list.size(); ++i) {
        ctx.frameBlock = frameBlockAddress();  // an earlier pass may have changed it (e.g. the RT scene's TLAS)
        {
            RegionScope region(*this, list[i].name.c_str());
            const auto p0 = options_.perf ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
            list[i].fn(ctx);
            if (options_.perf) {
                perf_.injectPassMs[std::format("{} @{}", list[i].name, inject::name(point))] +=
                    std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - p0).count();
            }
        }
        if (gpuTimer_.enabled()) {
            const std::uint32_t slot = frames_.currentSlot();
            gpuTimer_.mark(cmd, slot, vk::GpuTimer::Stream::Main,
                           gpuTimer_.details(slot) ? std::format("pass {} at {}", list[i].name, inject::name(point)) : std::string());
        }
    }
    injecting_ = false;
    invalidateRecordingState();
    ++perf_.injections;
    perf_.injectMs += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
}

bool Device::readable(Texture* texture, VkCommandBuffer cmd) {
    if (texture == nullptr || texture->isNull()) return false;
    texture->prepareForSampling(cmd);
    texture->lastUseFrame_ = frameValue();
    return texture->image().layout() == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
}

bool Device::create(const DeviceOptions& options) {
    options_ = options;
    renderThread_ = GetCurrentThreadId();
    vk::ContextOptions co;
    co.validation = options.validation || options.syncValidation;
    co.adapter = options.adapter;
    co.syncValidation = options.syncValidation;
    co.allowRayTracing = options.allowRayTracing;
    co.frameGenQueues = options.frameGenQueues;
    if (!ctx_.create(co)) {
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
    {
        // the static vertex / index buffers' arena (vk/mesh_arena.h); with the ray tracing tier also BLAS build inputs that
        // shaders read through their addresses (docs/research/raytracing.md 2)
        VkBufferUsageFlags usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_VERTEX_BUFFER_BIT;
        if (ctx_.features().rayTracing.enabled) {
            usage |= VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                     VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR;
        }
        meshArena_.create(ctx_, usage);
    }
    transients_.init(ctx_, frames_, bindless_);
    const auto pipelineFile = options.cacheDir.empty() ? std::filesystem::path() : options.cacheDir / "pipelines.bin";
    if (!pipelines_.create(ctx_, bindless_.pipelineLayout(), pipelineFile, options.pipelineLibraries)) {
        return false;
    }
    loadPipelineKeys();
    if (!options.offscreen && !swapchain_.create(ctx_, options.window, options.width, options.height, options.vsync)) {
        return false;
    }
    if (swapchain_.valid()) {
        presenter_.start(swapchain_);
    }
    if (options.perf && gpuTimer_.create(ctx_, vk::Frames::kInFlight)) {
        spdlog::info("d3d9: [debug] perf: GPU timestamps on{}",
                     options.perfFrame != 0 ? std::format(", pass timings of frame {}", options.perfFrame) : std::string());
    }
    for (auto& s : acquireSemaphores_) {
        VkSemaphoreCreateInfo sci{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
        vkCreateSemaphore(ctx_.device(), &sci, nullptr, &s);
        vk::names::object(ctx_, s, VK_OBJECT_TYPE_SEMAPHORE, "rvk swapchain acquire");
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
        vk::names::object(ctx_, nullVertexBuffer_, VK_OBJECT_TYPE_BUFFER, "rvk null vertex buffer (unbound streams)");
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
    samplerSlotCache_.fill(UINT32_MAX);
    samplerMemo_ = {};
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
    backBuffer_->setDebugLabel("back buffer (D3D9 model)");
    if (depthBuffer_) depthBuffer_->setDebugLabel("back buffer depth-stencil");
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
        presenter_.waitIdle();
        swapchain_.recreate(width, height);
    }
    return true;
}

void Device::windowResized() {
    if (options_.offscreen) {
        return;
    }
    presenter_.waitIdle();
    recreateSwapchain();
}

void Device::setVsync(bool vsync) {
    if (vsync == options_.vsync) {
        return;
    }
    options_.vsync = vsync;
    pacerChecked_ = {};
    pacerRequested_ = -1.0;
    presenter_.waitIdle();
    swapchain_.setVsync(vsync);
    if (swapchain_.valid()) {
        swapchain_.recreate(swapchain_.extent().width, swapchain_.extent().height);
    }
}

void Device::resetPacer() {
    pacerChecked_ = {};
    pacerRequested_ = -1.0;
    pacerMonitor_ = nullptr;  // the monitor's refresh rate is queried again
}

void Device::displayModeChanged() { resetPacer(); }

void Device::setRefreshCap(double hz) {
    hz = std::max(0.0, hz);
    if (hz == refreshCap_) return;
    refreshCap_ = hz;
    pacerChecked_ = {};
    pacerRequested_ = -1.0;
}

bool Device::setFullScreenExclusive(void* monitor, bool hold) {
    if (options_.offscreen || !swapchain_.valid()) return false;
    if (!ctx_.features().fullScreenExclusive) monitor = nullptr;
    bool recreated = false;
    if (monitor != fseMonitor_) {
        recreated = true;
        fseMonitor_ = monitor;
        presenter_.waitIdle();
        swapchain_.releaseFullScreenExclusive();
        swapchain_.setFullScreenExclusive(monitor);
        recreateSwapchain();
        fseRetry_ = {};
        resetPacer();
    }
    if (hold != fseHold_) {
        fseHold_ = hold;
        fseRetry_ = {};
        if (!hold && swapchain_.fullScreenExclusiveHeld()) {
            presenter_.waitIdle();
            swapchain_.releaseFullScreenExclusive();
            spdlog::info("vulkan: exclusive fullscreen released");
            resetPacer();
        }
    }
    return recreated;
}

// render thread, the present thread idle: take the exclusive mode while wanted (retried once a second after a failure / loss)
void Device::serviceFullScreenExclusive() {
    if (swapchain_.takeExclusiveLost()) {
        spdlog::info("vulkan: exclusive fullscreen lost (acquire): re-acquired later");
        fseRetry_ = std::chrono::steady_clock::now() + std::chrono::seconds(1);
        resetPacer();
    }
    if (!fseHold_ || !swapchain_.fullScreenExclusiveConfigured() || swapchain_.fullScreenExclusiveHeld()) return;
    const auto now = std::chrono::steady_clock::now();
    if (now < fseRetry_) return;
    const VkResult r = swapchain_.acquireFullScreenExclusive();
    if (r == VK_SUCCESS) {
        spdlog::info("vulkan: exclusive fullscreen acquired (swapchain {}x{}, present mode {})", swapchain_.extent().width,
                     swapchain_.extent().height, static_cast<int>(swapchain_.presentMode()));
        resetPacer();
    } else {
        static int logged = 0;
        if (logged++ < 10) spdlog::warn("vulkan: vkAcquireFullScreenExclusiveModeEXT: {} (retried in 1 s)", vk::resultString(r));
        fseRetry_ = now + std::chrono::seconds(1);
    }
}

void Device::setMaxFps(double fps) {
    fps = std::max(0.0, fps);
    if (fps == options_.maxFps) {
        return;
    }
    options_.maxFps = fps;
    pacerChecked_ = {};  // updatePacer re-evaluates at the next present
    pacerRequested_ = -1.0;
}

void Device::setMenuCap(double fps) {
    fps = std::max(0.0, fps);
    if (fps == menuCap_) {
        return;
    }
    menuCap_ = fps;
    pacerChecked_ = {};  // updatePacer re-evaluates at the next present (the requested rate decides whether it changes)
}

void Device::setOverlay(PresentOverlay* overlay, bool intoBackBuffer) {
    overlay_ = overlay;
    overlayIntoBackBuffer_ = intoBackBuffer;
}

VkFormat Device::overlayFormat() const {
    if (overlayIntoBackBuffer_ || !swapchain_.valid()) {
        return backBuffer_ ? backBuffer_->image().format() : VK_FORMAT_B8G8R8A8_UNORM;
    }
    return swapchain_.format();
}

// after foreign commands (the overlay binds its own pipeline, descriptor set and dynamic state) in the frame's command buffer
void Device::invalidateRecordingState() {
    rec_.reset();
    lastPipelineValid_ = false;
    vsConstDirty_ = psConstDirty_ = true;
    const VkDescriptorSet set = bindless_.set();
    vkCmdBindDescriptorSets(frames_.cmd(), VK_PIPELINE_BIND_POINT_GRAPHICS, bindless_.pipelineLayout(), 0, 1, &set, 0, nullptr);
}

void Device::drawOverlayIntoBackBuffer() {
    if (overlay_ == nullptr || !overlayIntoBackBuffer_ || !backBuffer_ || !overlay_->active()) {
        return;
    }
    endRendering();
    VkCommandBuffer cmd = frames_.cmd();
    vk::Image& bb = backBuffer_->image();
    bb.transition(cmd, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    VkRenderingAttachmentInfo att{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
    att.imageView = bb.attachmentView(ctx_, 0, 0);
    att.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    att.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
    att.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    const VkExtent2D extent{backBuffer_->desc().width, backBuffer_->desc().height};
    VkRenderingInfo ri{VK_STRUCTURE_TYPE_RENDERING_INFO};
    ri.renderArea = {{0, 0}, extent};
    ri.layerCount = 1;
    ri.colorAttachmentCount = 1;
    ri.pColorAttachments = &att;
    vkCmdBeginRendering(cmd, &ri);
    overlay_->record(cmd, extent);
    vkCmdEndRendering(cmd);
    invalidateRecordingState();
}

void Device::destroy() {
    if (!created_) {
        return;
    }
    stopVariantWorker();
    frames_.waitIdle();
    stopRecordThreads();
    processReleases();
    backBuffer_.reset();
    depthBuffer_.reset();
    orphanLive();
    {
        const std::lock_guard lock(shaderMutex_);
        for (auto& [h, s] : shaders_) {
            if (s->module != VK_NULL_HANDLE) {
                vkDestroyShaderModule(ctx_.device(), s->module, nullptr);
                s->module = VK_NULL_HANDLE;
            }
            for (VkShaderModule* m : {&s->tintModule, &s->lodModule}) {
                if (*m != VK_NULL_HANDLE) {
                    vkDestroyShaderModule(ctx_.device(), *m, nullptr);
                    *m = VK_NULL_HANDLE;
                }
            }
            for (auto& m : s->variants) {
                if (const VkShaderModule v = m.exchange(VK_NULL_HANDLE); v != VK_NULL_HANDLE) vkDestroyShaderModule(ctx_.device(), v, nullptr);
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
    presenter_.stop();
    if (keySaver_.joinable()) {
        keySaver_.join();
    }
    savePipelineKeys();
    if (keySaver_.joinable()) {
        keySaver_.join();
    }
    swapchain_.destroy();
    gpuTimer_.destroy();
    pipelines_.destroy();
    passes_ = inject::Registry{};
    transients_.destroy();  // retired: frames_ destroys them
    // frames_ runs the retired destructors (images, bindless slot releases): before the bindless heap goes away
    frames_.destroy();
    meshArena_.destroy();  // after frames_: its retired frees go into the arena
    bindless_.destroy();
    ctx_.setLostReporter(nullptr);
    crashMarkers_.destroy();
    markersOn_ = false;
    ctx_.destroy();
    created_ = false;
}

// ---- frames ---------------------------------------------------------------------------------------------------------

void Device::beginFrame() {
    frames_.begin();
    viArena_.clear();  // every chunk of the last submission has been recorded and executed
    dynArena_.clear();
    lastVi_ = nullptr;
    lastDyn_ = nullptr;
    resetRecordPools(frames_.currentSlot());
    substituteFrom_ = nullptr;
    substituteSlot_ = 0;
    perf_.slotWaitMs += frames_.lastSlotWaitMs();
    if (gpuTimer_.enabled()) {
        const std::uint32_t slot = frames_.currentSlot();
        const vk::GpuTimer::Result r = gpuTimer_.collect(slot);
        if (r.valid) {
            live_.gpuMs = static_cast<float>(r.mainMs + r.uploadMs);
            ++perf_.gpuSubmits;
            perf_.gpuMs += r.mainMs;
            perf_.gpuUploadMs += r.uploadMs;
            if (!r.segments.empty()) {
                spdlog::info("perf: GPU passes of frame {} (one submission: {:.3f} ms main, {:.3f} ms uploads)", gpuDetailFrame_[slot],
                             r.mainMs, r.uploadMs);
                for (const auto& seg : r.segments) {
                    spdlog::info("perf:   {:8.3f} ms  {}", seg.ms, seg.label);
                }
            }
        }
        // a frame's submissions: the present's and any mid-frame flush (read-backs)
        // -PerfFrame N: that frame; the environment variable RVK_PERF_FRAMES = M extends it to the M frames from N on
        static const std::uint64_t perfSpan = [] {
            char e[16];  // the Win32 environment (the proxy sets it from DllMain; the CRT's copy is stale)
            const DWORD n = GetEnvironmentVariableA("RVK_PERF_FRAMES", e, sizeof e);
            return n > 0 && n < sizeof e ? std::max<std::uint64_t>(std::strtoull(e, nullptr, 10), 1) : 1;
        }();
        const bool details = options_.perfFrame != 0 && stats_.frames + 1 >= options_.perfFrame && stats_.frames + 1 < options_.perfFrame + perfSpan;
        gpuDetailFrame_[slot] = details ? stats_.frames + 1 : 0;
        gpuTimer_.start(slot, details);
        gpuTimer_.mark(frames_.cmd(), slot, vk::GpuTimer::Stream::Main, "begin");
    }
    processReleases();
    uploadUsed_ = false;
    rendering_ = false;
    rec_.reset();
    lastPipelineValid_ = false;
    vsConstDirty_ = psConstDirty_ = true;
    dirtyConstWindows();  // the window blocks live in the frame's ring
    frameAddr_ = 0;  // ring memory of the frame block belongs to a submission
    passIndex_ = 0;
    const VkDescriptorSet set = bindless_.set();
    vkCmdBindDescriptorSets(frames_.cmd(), VK_PIPELINE_BIND_POINT_GRAPHICS, bindless_.pipelineLayout(), 0, 1, &set, 0, nullptr);
    regionsReopen();
}

// ---- debug regions ----------------------------------------------------------------------------------------------------------

namespace {

void beginLabel(VkCommandBuffer cmd, const char* name) {
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
    vkCmdBeginDebugUtilsLabelEXT(cmd, &l);
}

// "base" or "base #index" into a fixed buffer (no allocation)
void formatLabel(char (&out)[48], const char* base, std::uint32_t index) {
    std::size_t n = 0;
    for (; base[n] != 0 && n < sizeof(out) - 12; ++n) out[n] = base[n];
    if (index != Device::kNoRegionIndex) {
        out[n++] = ' ';
        out[n++] = '#';
        const auto r = std::to_chars(out + n, out + sizeof(out) - 1, index);
        n = static_cast<std::size_t>(r.ptr - out);
    }
    out[n] = 0;
}

}  // namespace

void Device::regionBegin(const char* name, std::uint32_t index) {
    if (!regionsOn_ || regionCount_ >= kMaxRegions) return;
    OpenRegion& r = regions_[regionCount_++];
    r.base = name;
    r.index = index;
    formatLabel(r.text, name, index);
    r.marked = false;
    if (rendering_ && scopeSecondary_) {
        scopeLabel(true, r.text);  // in order with the scope's chunks
    } else {
        beginLabel(frames_.cmd(), r.text);
        if (markersOn_ && !rendering_) {
            crashMarkers_.begin(frames_.cmd(), frameValue(), r.text);
            r.marked = true;
        }
    }
}

void Device::regionEnd() {
    if (!regionsOn_ || regionCount_ == 0) return;
    --regionCount_;
    if (rendering_ && scopeSecondary_) {
        scopeLabel(false, nullptr);
    } else {
        vkCmdEndDebugUtilsLabelEXT(frames_.cmd());
        if (regions_[regionCount_].marked && markersOn_ && !rendering_) crashMarkers_.end(frames_.cmd(), frameValue(), regions_[regionCount_].text);
    }
}

void Device::setCrashMarkers(bool on) {
    if (on == markersOn_) return;
    if (!on) {
        markersOn_ = false;
        ctx_.setLostReporter(nullptr);
        return;
    }
    if (!created_ || !crashMarkers_.create(ctx_)) return;
    markersOn_ = true;
    ctx_.setLostReporter([this]() { return crashMarkers_.report(); });
    if (!regionsOn_) spdlog::warn("vulkan: crash markers: they follow the capture regions (debug.regions is off)");
}

void Device::setRegionsEnabled(bool on) {
    const bool can = ctx_.debugUtilsAvailable() && vkCmdBeginDebugUtilsLabelEXT != nullptr && vkCmdEndDebugUtilsLabelEXT != nullptr;
    on = on && can && created_;
    if (on == regionsOn_) return;
    phasesSeen_ = ~0ull;
    if (on) {
        regionsOn_ = true;
        regionsFrame_ = ~0ull;  // a fresh stack
        regionCount_ = 0;
        regionsReopen();
    } else {
        for (std::uint32_t i = 0; i < regionCount_; ++i) vkCmdEndDebugUtilsLabelEXT(frames_.cmd());
        regionCount_ = 0;
        regionsOn_ = false;
    }
}

// a submission is about to be made: its command buffer must not end with labels open
void Device::regionsClose() {
    if (!regionsOn_) return;
    for (std::uint32_t i = 0; i < regionCount_; ++i) vkCmdEndDebugUtilsLabelEXT(frames_.cmd());
}

// a new command buffer: the frame's region (a new frame: a new one) and whatever was open at the submit
void Device::regionsReopen() {
    if (!regionsOn_) return;
    phasesSeen_ = ~0ull;
    VkCommandBuffer cmd = frames_.cmd();
    if (regionCount_ == 0 || regionsFrame_ != stats_.frames) {
        regionCount_ = 0;
        regionsFrame_ = stats_.frames;
        phaseKey_ = 0;
        regionBegin("Frame", static_cast<std::uint32_t>(stats_.frames + 1));
        return;
    }
    for (std::uint32_t i = 0; i < regionCount_; ++i) beginLabel(cmd, regions_[i].text);
}

// The view / pass regions follow the phase tracker; called when a draw is recorded and before an injection. Cheap when the
// phase did not change (a key over the open scopes); the wanted path is built in a fixed array.
void Device::syncPhaseRegions() {
    if (!regionsOn_ || injecting_ || regionCount_ == 0) return;
    if (phasesSeen_ == phases_.generation()) return;  // V4: nothing moved since the last look (the key below would be the same)
    const scene::Phase cur = phases_.current();
    std::uint64_t key = 1469598103934665603ull;
    const auto mixKey = [&](std::uint64_t v) {
        key ^= v;
        key *= 1099511628211ull;
    };
    for (const scene::Phase& e : phases_.enclosing()) mixKey((static_cast<std::uint64_t>(e.view) << 8) | static_cast<std::uint64_t>(e.pass));
    mixKey((static_cast<std::uint64_t>(cur.view) << 8) | static_cast<std::uint64_t>(cur.pass));
    mixKey(phases_.viewsEntered(cur.view));
    phasesSeen_ = phases_.generation();
    if (key == phaseKey_) return;
    phaseKey_ = key;
    flushBatch();  // the batched draws belong to the regions they were issued in
    struct Want {
        const char* base;
        std::uint32_t index;
    };
    std::array<Want, scene::PhaseTracker::kMaxDepth + 2> want;
    std::size_t n = 0;
    const auto view = [&](scene::View v) {
        const bool numbered = v == scene::View::Shadow || v == scene::View::Main || v == scene::View::Preview;
        want[n++] = {scene::name(v), numbered ? phases_.viewsEntered(v) : kNoRegionIndex};
    };
    for (const scene::Phase& e : phases_.enclosing()) {
        if (e.view != scene::View::None && n < want.size() - 2) view(e.view);
    }
    if (cur.view != scene::View::None && n < want.size() - 1) {
        view(cur.view);
        if (cur.pass != scene::Pass::None) want[n++] = {scene::name(cur.pass), kNoRegionIndex};
    }
    // reconcile with the open stack below the frame's region (names are static strings: the pointers identify them)
    std::uint32_t k = 1;
    while (k < regionCount_ && k - 1 < n && regions_[k].base == want[k - 1].base && regions_[k].index == want[k - 1].index) ++k;
    while (regionCount_ > k) regionEnd();
    for (std::size_t i = k - 1; i < n; ++i) regionBegin(want[i].base, want[i].index);
}

void Device::gpuMark(const char* what) {
    if (gpuTimer_.enabled()) {
        gpuTimer_.mark(frames_.cmd(), frames_.currentSlot(), vk::GpuTimer::Stream::Main, what);
    }
}

PerfStats Device::takePerf() {
    perf_.pipelineBinds += rec_.pipelineBinds;
    rec_.pipelineBinds = 0;
    PerfStats p = std::move(perf_);
    perf_ = PerfStats{};
    p.pipelinesBuilt = pipelines_.built() - perfPipelinesBase_;
    p.pipelineMs = pipelines_.buildMs() - perfPipelineMsBase_;
    perfPipelinesBase_ = pipelines_.built();
    perfPipelineMsBase_ = pipelines_.buildMs();
    p.libraries = pipelines_.libraries() - perfLibrariesBase_;
    p.libraryMs = pipelines_.libraryMs() - perfLibraryMsBase_;
    p.optimized = pipelines_.optimized() - perfOptimizedBase_;
    p.optimizeMs = pipelines_.optimizeMs() - perfOptimizeMsBase_;
    perfLibrariesBase_ = pipelines_.libraries();
    perfLibraryMsBase_ = pipelines_.libraryMs();
    perfOptimizedBase_ = pipelines_.optimized();
    perfOptimizeMsBase_ = pipelines_.optimizeMs();
    p.prewarmed = pipelines_.prewarmed() - perfPrewarmedBase_;
    p.prewarmMs = pipelines_.prewarmMs() - perfPrewarmMsBase_;
    perfPrewarmedBase_ = pipelines_.prewarmed();
    perfPrewarmMsBase_ = pipelines_.prewarmMs();
    const std::uint64_t built = shadersBuilt_.load(), compiled = shadersCompiled_.load(), ns = shaderNs_.load();
    p.shadersBuilt = built - perfShadersBase_;
    p.shadersCompiled = compiled - perfShadersCompiledBase_;
    p.shaderMs = static_cast<double>(ns - perfShaderNsBase_) * 1e-6;
    perfShadersBase_ = built;
    perfShadersCompiledBase_ = compiled;
    perfShaderNsBase_ = ns;
    p.pacer = pacer_.takeStats();
    p.pacerRate = pacer_.rate();
    p.pacerVblank = pacer_.vblankLocked();
    return p;
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
        if (gpuTimer_.enabled()) {
            gpuTimer_.mark(cmd, frames_.currentSlot(), vk::GpuTimer::Stream::Upload, "upload begin");
        }
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
        if (gpuTimer_.enabled()) {
            gpuTimer_.mark(frames_.uploadCmd(), frames_.currentSlot(), vk::GpuTimer::Stream::Upload, "uploads");
        }
    }
    gpuMark(signalRendered != VK_NULL_HANDLE ? "present blit + barriers since the last pass"
                                             : "copies + barriers since the last pass (flush)");
    regionsClose();
    const auto t0 = std::chrono::steady_clock::now();
    frames_.submit(waitAcquire, signalRendered);
    perf_.submitMs += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    perf_.ringBytes += frames_.ringBytes() - ringBytesBase_;
    ringBytesBase_ = frames_.ringBytes();
}

// the pacer's rate follows the window's monitor. EnumDisplaySettings is slow (it stalled the frames while the world loaded
// when it ran once a second), so the rate is only queried again when the window moves to another monitor.
void Device::updatePacer() {
    const auto now = std::chrono::steady_clock::now();
    if (now - pacerChecked_ < std::chrono::seconds(1)) {
        return;
    }
    pacerChecked_ = now;
    double refresh = 0.0;
    if (options_.vsync) {
        void* monitor = MonitorFromWindow(options_.window, MONITOR_DEFAULTTONEAREST);
        if (monitor != pacerMonitor_) {
            pacerMonitor_ = monitor;
            const auto t0 = std::chrono::steady_clock::now();
            pacerMonitorRate_ = vk::monitorRefreshRate(options_.window);
            spdlog::info("vulkan: monitor refresh rate {} Hz (queried in {:.1f} ms)", pacerMonitorRate_,
                         std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count());
        }
        refresh = pacerMonitorRate_;
    }
    // vsync: the refresh rate (or the game's refresh rate below it in borderless fullscreen: setRefreshCap), or max_fps
    // below it; without vsync max_fps or nothing
    double rate = refresh;
    const bool refreshCapped = refresh > 0.0 && refreshCap_ > 0.0 && refreshCap_ < refresh - 0.5;
    if (refreshCapped) {
        rate = refreshCap_;
    }
    if (options_.maxFps > 0.0 && (rate <= 0.0 || options_.maxFps < rate)) {
        rate = options_.maxFps;
    }
    const bool menuCapped = menuCap_ > 0.0 && (rate <= 0.0 || menuCap_ < rate);
    if (menuCapped) {
        rate = menuCap_;
    }
    // an unlocked vsync pacer retries the lock (DWM's measured period strays under load)
    vk::VblankClock probe;
    const double vblanks = rate > 0.0 ? std::round(refresh / rate) : 0.0;
    const bool lockable = refresh > 0.0 && vblanks >= 1.0 && vblanks <= 8.0 && std::abs(refresh / vblanks - rate) < 0.01 * rate;
    const bool relock = rate == pacerRequested_ && lockable && !pacer_.vblankLocked() && vk::dwmVblankClock(probe) &&
                        std::abs(probe.rate() - refresh) < 0.005 * refresh;
    if (rate != pacerRequested_ || relock) {
        pacerRequested_ = rate;
        // with vsync the slots sit on DWM's vblank grid (the refresh rate or an integer fraction of it)
        pacer_.setRate(rate, refresh);
        vk::VblankClock clock;
        const bool dwm = vk::dwmVblankClock(clock);
        spdlog::info("vulkan: DWM composition clock {}", dwm ? std::format("{:.3f} Hz", clock.rate()) : std::string("unavailable"));
        spdlog::info("vulkan: frame pacing at {:.3f} fps{}{}", pacer_.rate(),
                     menuCapped                                       ? " (menu)"
                     : options_.maxFps > 0.0 && rate == options_.maxFps ? " (max_fps)"
                     : refreshCapped && rate == refreshCap_           ? " (vsync, the game's refresh rate)"
                     : options_.vsync                                 ? " (vsync)"
                                                                      : "",
                     pacer_.vblankLocked() ? ", locked to DWM's vblank" : "");
    }
}

void Device::present() {
    presentStart_ = std::chrono::steady_clock::now();
    if (stats_.frames > 0) {
        ++perf_.frames;
        const double cpu = std::chrono::duration<double, std::milli>(presentStart_ - presentEnd_).count();
        perf_.cpuFrameMs += cpu;
        live_.cpuMs = static_cast<float>(cpu);
    }
    if (stats_.frames > 1) {
        // the base frame interval (present to present), smoothed: the manual frame generation shows its frame half of it before the real one
        const double interval = std::chrono::duration<double, std::milli>(presentStart_ - lastPresentStart_).count();
        if (interval > 0.0 && interval < 250.0) baseIntervalMs_ = baseIntervalMs_ <= 0.0 ? interval : baseIntervalMs_ * 0.9 + interval * 0.1;
    }
    struct PresentEnd {
        Device& d;
        ~PresentEnd() {
            const auto now = std::chrono::steady_clock::now();
            if (d.stats_.frames > 1) {
                d.perf_.presentMs += std::chrono::duration<double, std::milli>(now - d.presentStart_).count();
                const float interval = std::chrono::duration<float, std::milli>(d.presentStart_ - d.lastPresentStart_).count();
                d.live_.intervalMs = interval;
                if (d.options_.perf) {  // takePerf (the only consumer) runs only with [debug] perf
                    d.perf_.frameIntervalsMs.push_back(interval);
                }
            }
            d.lastPresentStart_ = d.presentStart_;
            d.presentEnd_ = now;
        }
    } presentEnd{*this};
    endRendering();
    ++stats_.frames;
    phases_.endFrame();
    firings_.endFrame();
    shadowDepthMaps_ = 0;
    draws_.endFrame();
    capturedPrev_.swap(captured_);
    captured_.clear();
    // the G-buffer's attachments are armed once a frame's solid pass found every pipeline / variant they need
    if (gbuf_.normal != nullptr && !gbufArmed_) {
        const bool timeout = secondsSinceCreate() - gbufRequestedAt_ > 10.0;
        if ((gbufSolid_ > 0 && gbufMissing_ == 0) || (timeout && gbufSolid_ > 0)) {
            gbufArmed_ = true;
            spdlog::info("d3d9: g-buffer armed: {} solid draws, {} without their pipeline / variant yet{}", gbufSolid_, gbufMissing_,
                         gbufMissing_ != 0 ? " (10 s timeout: built at their draws)" : "");
        }
    }
    gbufSolid_ = gbufMissing_ = 0;
    lastRecord_ = nullptr;
    lastCamera_ = camera_;
    camera_.advance();
    // per-frame counters span every submission of the frame (read-backs flush mid-frame)
    stats_.lastFrameDraws = stats_.draws;
    perf_.draws += stats_.draws;
    perf_.renderPasses += stats_.renderPasses;
    stats_.draws = 0;
    stats_.renderPasses = 0;
    stats_.drawsSkipped = 0;
    if (options_.offscreen || !swapchain_.valid()) {
        submitFrame(VK_NULL_HANDLE, VK_NULL_HANDLE);
        beginFrame();
        return;
    }
    if (unpaced_) {
        // loading screen: never wait, neither for the pacer nor for a vsync swapchain (FIFO blocks acquire / present at the
        // refresh rate: 5.6 s instead of 2.1 s per quick load, docs/research/menu-fps.md 4). A frame is shown at most every
        // 1 / kLoadingFps (or the pacer's interval when that is longer); the others are submitted without presenting.
        constexpr double kLoadingFps = 30.0;
        const auto now = std::chrono::steady_clock::now();
        const double fps = pacer_.rate() > 0.0 ? std::min(pacer_.rate(), kLoadingFps) : kLoadingFps;
        if (now - loadingShown_ < std::chrono::duration<double>(1.0 / fps)) {
            ++stats_.loadingFramesDropped;
            submitFrame(VK_NULL_HANDLE, VK_NULL_HANDLE);
            beginFrame();
            return;
        }
        loadingShown_ = now;
    }
    updatePacer();
    const auto tw = std::chrono::steady_clock::now();
    if (!unpaced_) {
        pacer_.wait();  // after an unpaced stretch the slot is long past: the pacer restarts its cadence
    }
    stats_.pacerMs += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - tw).count();
    // Manual frame generation (enh/framegen, docs/research/frame-generation.md 2.1 (ii)): the interpolated frame is shown first, the
    // real frame half a base interval later, both through this swapchain (simple timed spacing; the pacing of the AMD swapchain is stage F3)
    vk::Image* generated = generated_;
    generated_ = nullptr;
    const bool twoFrames = generated != nullptr && generated->valid() && !unpaced_;
    std::chrono::steady_clock::time_point generatedAt{};
    for (int stage = 0; stage < (twoFrames ? 2 : 1); ++stage) {
    const bool showGenerated = twoFrames && stage == 0;
    if (twoFrames && stage == 1) {
        const double left = 0.5 * baseIntervalMs_ - std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - generatedAt).count();
        if (left > 0.05) pacer_.sleepFor(left);
    }
    const std::uint32_t slot = static_cast<std::uint32_t>(frames_.currentValue() % vk::Frames::kInFlight);
    VkSemaphore acquired = acquireSemaphores_[slot];
    std::uint32_t imageIndex = 0;
    const auto tq = std::chrono::steady_clock::now();
    // the swapchain is the present thread's until its present returned
    const VkResult previous = presenter_.waitIdle();
    stats_.presentMs += presenter_.takePresentMs();
    if (previous == VK_ERROR_OUT_OF_DATE_KHR || previous == VK_SUBOPTIMAL_KHR) {
        ++stats_.presentSuboptimal;
        ++stats_.swapchainRecreates;
        recreateSwapchain();
    } else if (previous == VK_ERROR_FULL_SCREEN_EXCLUSIVE_MODE_LOST_EXT) {
        // exclusive fullscreen taken away (another window, a mode change): presents go on non-exclusive, re-acquired later
        swapchain_.noteExclusiveLost();
        spdlog::info("vulkan: exclusive fullscreen lost (present): re-acquired later");
        fseRetry_ = std::chrono::steady_clock::now() + std::chrono::seconds(1);
        resetPacer();
    } else if (previous != VK_SUCCESS) {
        spdlog::error("vulkan: present: {}", vk::resultString(previous));
    }
    serviceFullScreenExclusive();
    const auto ta = std::chrono::steady_clock::now();
    stats_.presentWaitMs += std::chrono::duration<double, std::milli>(ta - tq).count();
    const bool acquiredOk = swapchain_.acquire(acquired, imageIndex);
    stats_.acquireMs += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - ta).count();
    if (!acquiredOk) {
        // minimised or out of date: drop the frame's presentation, keep its work (exclusive fullscreen lost: no recreation,
        // serviceFullScreenExclusive re-acquires it)
        ++stats_.acquireFailures;
        submitFrame(VK_NULL_HANDLE, VK_NULL_HANDLE);
        if (!swapchain_.exclusiveLost()) recreateSwapchain();
        beginFrame();
        return;
    }
    std::optional<RegionScope> presentRegion;
    presentRegion.emplace(*this, showGenerated ? "Present (generated)" : "Present");
    VkCommandBuffer cmd = frames_.cmd();
    // the frame shown: the engine's back buffer, or (first of the two) the generated frame
    vk::Image& bb = showGenerated ? *generated : backBuffer_->image();
    const VkExtent3D srcExtent = showGenerated ? generated->desc().extent
                                               : VkExtent3D{backBuffer_->desc().width, backBuffer_->desc().height, 1};
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
    region.srcOffsets[1] = {static_cast<std::int32_t>(srcExtent.width), static_cast<std::int32_t>(srcExtent.height), 1};
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
    b.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    // the overlay (src/overlay) on top of the copied frame: only in the swapchain image, never in the back buffer that
    // captures read
    if (overlay_ != nullptr && !overlayIntoBackBuffer_ && swapchain_.view(imageIndex) != VK_NULL_HANDLE && overlay_->active()) {
        b.dstStageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
        b.dstAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;
        b.newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        vkCmdPipelineBarrier2(cmd, &di);
        VkRenderingAttachmentInfo att{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
        att.imageView = swapchain_.view(imageIndex);
        att.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        att.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
        att.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        VkRenderingInfo ri{VK_STRUCTURE_TYPE_RENDERING_INFO};
        ri.renderArea = {{0, 0}, ext};
        ri.layerCount = 1;
        ri.colorAttachmentCount = 1;
        ri.pColorAttachments = &att;
        vkCmdBeginRendering(cmd, &ri);
        {
            RegionScope overlayRegion(*this, "Overlay");
            overlay_->record(cmd, ext);
        }
        vkCmdEndRendering(cmd);
        b.srcStageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
        b.srcAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;
        b.oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    }
    b.dstStageMask = VK_PIPELINE_STAGE_2_NONE;
    b.dstAccessMask = VK_ACCESS_2_NONE;
    b.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    vkCmdPipelineBarrier2(cmd, &di);
    presentRegion.reset();
    submitFrame(acquired, swapchain_.renderedSemaphore(imageIndex));
    presenter_.present(imageIndex);
    ++stats_.presentsQueued;
    if (showGenerated) {
        ++stats_.generatedPresented;
        generatedAt = std::chrono::steady_clock::now();
    }
    // persist new pipelines every 10 s (the game is usually killed, not shut down)
    if (presentStart_ - pipelinesSaved_ > std::chrono::seconds(10)) {
        pipelinesSaved_ = presentStart_;
        pipelines_.saveAsync();
        savePipelineKeys();
    }
    beginFrame();
    }  // for (stage)
}

// render thread, the present thread idle
void Device::recreateSwapchain() {
    RECT rc{};
    GetClientRect(options_.window, &rc);
    if (rc.right > 0 && rc.bottom > 0) {
        swapchain_.recreate(static_cast<std::uint32_t>(rc.right), static_cast<std::uint32_t>(rc.bottom));
    }
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

namespace {
// the debug name of a translated shader's module (vk/debug_names.h): "vs|ps <hash> [<variant>]"; empty with names off
std::string moduleLabel(const vk::Context& ctx, shader::Stage stage, std::uint64_t hash, std::string_view tag) {
    if (!ctx.namesEnabled()) return {};
    return std::format("{} {:016x}{}{}", stage == shader::Stage::Vertex ? "vs" : "ps", hash, tag.empty() ? "" : " ", tag);
}
}  // namespace

std::shared_ptr<Shader> Device::createShader(std::span<const std::uint32_t> bytecode) {
    const std::size_t bytes = shader::byteLength(bytecode.data(), bytecode.size());
    if (bytes == 0) {
        spdlog::warn("d3d9: shader bytecode without END token");
        return nullptr;
    }
    const std::span<const std::uint32_t> tokens = bytecode.first(bytes / 4);
    const std::uint32_t hashSeed = kTranslatorVersion | (shader::debugInfo() ? 0x80000000u : 0u);  // debug-info SPIR-V is cached apart
    const std::uint64_t hash = fnv1a(tokens.data(), tokens.size_bytes(), fnv1a(&hashSeed, sizeof(hashSeed)));
    {
        const std::lock_guard lock(shaderMutex_);
        if (auto it = shaders_.find(hash); it != shaders_.end()) {
            return it->second;
        }
    }
    struct Timed {
        std::atomic<std::uint64_t>& ns;
        std::chrono::steady_clock::time_point t0 = std::chrono::steady_clock::now();
        ~Timed() {
            ns += static_cast<std::uint64_t>(
                std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - t0).count());
        }
    } timed{shaderNs_};
    ++shadersBuilt_;
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
        ++shadersCompiled_;
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
    s->module = vk::createShaderModule(ctx_, spirv, moduleLabel(ctx_, s->stage, hash, {}));
    s->reflection = glsl.reflection;
    s->bytecode.assign(tokens.begin(), tokens.end());
    s->hash = hash;
    s->semantics = shaderSemantics(tokens, s->reflection);
    for (std::uint32_t slot = 0; slot < kSamplerCount; ++slot) {
        if (s->reflection.samplers[slot] != shader::TextureType::Unknown) s->samplerMask |= 1u << slot;
    }
    if (s->module == VK_NULL_HANDLE) {
        return nullptr;
    }
    {
        const std::lock_guard lock(shaderMutex_);
        auto [it, inserted] = shaders_.emplace(hash, s);
        if (!inserted) {  // another thread was faster
            vkDestroyShaderModule(ctx_.device(), s->module, nullptr);
            return it->second;
        }
        moduleHashes_.emplace(s->module, hash);
    }
    prewarmPipelines(hash);
    // with pipeline libraries the shader's compilation happens here (often a loader thread), not at its first draw
    pipelines_.prepareShader(s->module, s->stage == shader::Stage::Vertex ? VK_SHADER_STAGE_VERTEX_BIT : VK_SHADER_STAGE_FRAGMENT_BIT);
    return s;
}

// The tint debug view's variant of a pixel shader (GlslOptions::tintEpilogue): translated and compiled on its first tinted
// draw (render thread; a short hitch when the view is switched on), cached on the shader and in the SPIR-V disk cache. The
// normal module is never touched, so with the view off every draw renders exactly as before.
VkShaderModule Device::tintModule(Shader& ps) {
    if (ps.tintModule == VK_NULL_HANDLE && !ps.tintFailed) {
        ps.tintFailed = true;  // until it succeeds: one attempt per shader
        ps.tintModule = variantModule(ps, shader::GlslOptions{.tintEpilogue = true}, std::format("tint{}", kTintVersion));
        ps.tintFailed = ps.tintModule == VK_NULL_HANDLE;
    }
    return ps.tintModule;
}

// The SpeedTree billboards' vertex shader with the baked fade-in distances (BLENDWEIGHT1.xy: start, range; engine/lod.h)
// scaled by FrameBlock params[kFrameParamLod].x: used for billboard draws while lod.trees is not 1, built as tintModule
VkShaderModule Device::lodModule(Shader& vs) {
    if (vs.lodModule == VK_NULL_HANDLE && !vs.lodFailed) {
        vs.lodFailed = true;
        shader::GlslOptions o;
        o.inputOverrides.push_back(
            {shader::Usage::BlendWeight, 1, std::format("vec4($v.xy * pc.frame.params[{}].x, $v.zw)", shader::abi::kFrameParamLod)});
        vs.lodModule = variantModule(vs, o, std::format("lod{}_t{}", kLodVariantVersion, kTranslatorVersion));
        vs.lodFailed = vs.lodModule == VK_NULL_HANDLE;
        spdlog::info("d3d9: billboard fade variant of vertex shader {:016x}: {}", vs.hash, vs.lodFailed ? "FAILED" : "built");
    }
    return vs.lodModule;
}

void Device::setBillboardFadeScale(float scale) {
    billboardFadeScale_ = scale;
    setFrameParam(shader::abi::kFrameParamLod, {scale, 0.0f, 0.0f, 0.0f});
}

// a variant of a translated shader (render thread): translated with `options`, compiled (or read from the SPIR-V disk cache
// under `<hash>_<tag>.spv`), null when it fails
namespace {

// VariantSlot::Base: the plain GLSL material shader of a request the port covers
std::atomic<std::uint32_t> g_uberSkip{0};  // debug.ubershader_skip

bool uberCovers(const MaterialSource& src) {
    return shader::uberSupports(src.file, src.entry) &&
           (shader::uberClassMask(src.entry, src.macros) & g_uberSkip.load(std::memory_order_relaxed)) == 0;
}

bool uberBaseOptions(const Shader& s, shader::GlslOptions& o) {
    const MaterialSource* src = s.source.load(std::memory_order_acquire);
    if (s.stage != shader::Stage::Pixel || src == nullptr || !uberCovers(*src)) return false;
    o.uber = true;
    return true;
}

}  // namespace

void Device::setUbershaderSkip(std::uint32_t mask) { g_uberSkip.store(mask, std::memory_order_relaxed); }

void Device::setUbershader(bool on) {
    ubershader_.store(on, std::memory_order_relaxed);
    if (on) setVariantProvider(VariantSlot::Base, {std::format("base_t{}", kTranslatorVersion), &uberBaseOptions});
    spdlog::info("d3d9: GLSL material shaders {}", on ? "on" : "off");
}

// A material pixel shader built from the GLSL material shader with its request's macros (+ the enhancement hooks of
// options.uberDefines); null when it does not build (the draw keeps the translated shader)
VkShaderModule Device::uberModule(const Shader& s, const MaterialSource& src, const shader::GlslOptions& options, std::string_view tag) {
    const std::string glsl = shader::uberPixelGlsl(src.entry, src.macros, options.uberDefines);
    if (glsl.empty()) return VK_NULL_HANDLE;
    std::vector<std::uint32_t> spirv;
    const auto cacheFile =
        options_.cacheDir.empty()
            ? std::filesystem::path()
            : options_.cacheDir / "spirv" /
                  std::format("{:016x}_{}_u{:08x}.spv", s.hash, tag, static_cast<std::uint32_t>(shader::uberSourceHash() ^ (shader::uberSourceHash() >> 32)));
    if (!cacheFile.empty()) {
        std::ifstream in(cacheFile, std::ios::binary | std::ios::ate);
        if (in) {
            spirv.resize(static_cast<std::size_t>(in.tellg()) / 4);
            in.seekg(0);
            in.read(reinterpret_cast<char*>(spirv.data()), static_cast<std::streamsize>(spirv.size() * 4));
        }
    }
    if (spirv.empty()) {
        auto compiled = shader::compileGlsl(shader::Stage::Pixel, glsl);
        if (!compiled.ok()) {
            if (ubershadersFailed_.fetch_add(1) < 12) {
                spdlog::warn("d3d9: GLSL material shader of {:016x} ({} {} variant {}) does not compile: {}", s.hash, src.file, src.entry, tag,
                             compiled.log.substr(0, 600));
            }
            return VK_NULL_HANDLE;
        }
        spirv = std::move(compiled.words);
        if (!cacheFile.empty()) {
            std::error_code ec;
            std::filesystem::create_directories(cacheFile.parent_path(), ec);
            std::ofstream out(cacheFile, std::ios::binary);
            out.write(reinterpret_cast<const char*>(spirv.data()), static_cast<std::streamsize>(spirv.size() * 4));
        }
    }
    if (const std::uint64_t n = ++ubershadersBuilt_; n == 1 || n % 50 == 0) {
        spdlog::info("d3d9: GLSL material shaders: {} modules built, {} failed", n, ubershadersFailed_.load());
    }
    return vk::createShaderModule(ctx_, spirv,
                                  ctx_.namesEnabled() ? moduleLabel(ctx_, s.stage, s.hash, std::format("{} {} {}", src.file, src.entry, tag)) : std::string());
}

VkShaderModule Device::variantModule(const Shader& s, const shader::GlslOptions& options, std::string_view tag) {
    if (options.uber && ubershader_.load(std::memory_order_relaxed) && s.stage == shader::Stage::Pixel) {
        const MaterialSource* src = s.source.load(std::memory_order_acquire);
        if (src != nullptr && uberCovers(*src)) return uberModule(s, *src, options, tag);
    }
    const auto dec = shader::decode(s.bytecode);
    if (!dec.ok()) {
        return VK_NULL_HANDLE;
    }
    const auto glsl = shader::toGlsl(dec.program, options);
    if (!glsl.ok()) {
        spdlog::warn("d3d9: {} variant of shader {:016x}: {}", tag, s.hash, glsl.error);
        return VK_NULL_HANDLE;
    }
    std::vector<std::uint32_t> spirv;
    const auto cacheFile = options_.cacheDir.empty() ? std::filesystem::path()
                                                     : options_.cacheDir / "spirv" / std::format("{:016x}_{}.spv", s.hash, tag);
    if (!cacheFile.empty()) {
        std::ifstream in(cacheFile, std::ios::binary | std::ios::ate);
        if (in) {
            spirv.resize(static_cast<std::size_t>(in.tellg()) / 4);
            in.seekg(0);
            in.read(reinterpret_cast<char*>(spirv.data()), static_cast<std::streamsize>(spirv.size() * 4));
        }
    }
    if (spirv.empty()) {
        auto compiled = shader::compileGlsl(dec.program.version.stage, glsl.glsl);
        if (!compiled.ok()) {
            spdlog::error("d3d9: {} variant of shader {:016x} does not compile:\n{}", tag, s.hash, compiled.log);
            return VK_NULL_HANDLE;
        }
        spirv = std::move(compiled.words);
        if (!cacheFile.empty()) {
            std::error_code ec;
            std::filesystem::create_directories(cacheFile.parent_path(), ec);
            std::ofstream out(cacheFile, std::ios::binary);
            out.write(reinterpret_cast<const char*>(spirv.data()), static_cast<std::streamsize>(spirv.size() * 4));
        }
    }
    return vk::createShaderModule(ctx_, spirv, moduleLabel(ctx_, s.stage, s.hash, tag));
}

// ---- thin G-buffer (enh/gbuffer) ----

void Device::setGbuffer(const GbufferTargets& g) {
    const bool was = gbuf_.normal != nullptr;
    gbuf_ = g;
    if (g.normal == nullptr) {
        if (was) spdlog::info("d3d9: g-buffer off");
        gbufArmed_ = false;
        return;
    }
    if (!was) {
        gbufRequestedAt_ = secondsSinceCreate();
        gbufArmed_ = false;
        spdlog::info("d3d9: g-buffer requested ({}x{}): building the solid pass's pipelines with its attachments", g.width, g.height);
    }
}

// ---- background variants (VariantSlot; enh/gi, enh/rt_scene) ----

void Device::setVariantProvider(VariantSlot slot, VariantProvider provider) {
    const std::lock_guard lock(variantMutex_);
    variantProviders_[static_cast<std::size_t>(slot)] = std::move(provider);
}

std::shared_ptr<Shader> Device::shaderOwner(const Shader* s) {
    if (s == nullptr) return nullptr;
    const std::lock_guard lock(shaderMutex_);
    const auto it = shaders_.find(s->hash);
    return it != shaders_.end() && it->second.get() == s ? it->second : nullptr;
}

VkShaderModule Device::variant(const ShaderRef& s, VariantSlot slot) {
    const std::size_t i = static_cast<std::size_t>(slot);
    const std::uint8_t state = s->variantState[i].load(std::memory_order_acquire);
    if (state == 2) return s->variants[i].load(std::memory_order_acquire);
    if (state != 0) return VK_NULL_HANDLE;
    s->variantState[i].store(1, std::memory_order_release);
    {
        const std::lock_guard lock(variantMutex_);
        if (!variantProviders_[i].options) {
            s->variantState[i].store(3);
            return VK_NULL_HANDLE;
        }
        std::shared_ptr<Shader> owner = shaderOwner(s.get());
        if (!owner) {
            s->variantState[i].store(3);
            return VK_NULL_HANDLE;
        }
        variantJobs_.push_back({std::move(owner), slot});
        if (variantThreads_.empty()) {
            variantStop_ = false;
            for (int t = 0; t < 3; ++t) variantThreads_.emplace_back([this] { variantWorker(); });
        }
    }
    variantCv_.notify_one();
    return VK_NULL_HANDLE;
}

// translates + compiles the queued variants (SPIR-V disk cache as variantModule), in request order
void Device::variantWorker() {
    SetThreadDescription(GetCurrentThread(), L"rvk shader variants");
    for (;;) {
        VariantJob job;
        VariantProvider provider;
        {
            std::unique_lock lock(variantMutex_);
            variantCv_.wait(lock, [this] { return variantStop_ || !variantJobs_.empty(); });
            if (variantStop_) return;
            job = std::move(variantJobs_.front());
            variantJobs_.pop_front();
            provider = variantProviders_[static_cast<std::size_t>(job.slot)];
        }
        const std::size_t i = static_cast<std::size_t>(job.slot);
        shader::GlslOptions o;
        if (!provider.options || !provider.options(*job.shader, o)) {
            job.shader->variantState[i].store(3, std::memory_order_release);
            continue;
        }
        const VkShaderModule m = variantModule(*job.shader, o, std::format("{}_t{}", provider.tag, kTranslatorVersion));
        if (m == VK_NULL_HANDLE) {
            ++variantsFailed_;
            spdlog::warn("d3d9: {} variant of shader {:016x} failed (the draws keep the engine's shader)", provider.tag, job.shader->hash);
            job.shader->variantState[i].store(3, std::memory_order_release);
            continue;
        }
        ++variantsBuilt_;
        job.shader->variants[i].store(m, std::memory_order_release);
        job.shader->variantState[i].store(2, std::memory_order_release);
    }
}

void Device::stopVariantWorker() {
    {
        const std::lock_guard lock(variantMutex_);
        variantStop_ = true;
        for (VariantJob& j : variantJobs_) j.shader->variantState[static_cast<std::size_t>(j.slot)].store(0);
        variantJobs_.clear();
    }
    variantCv_.notify_all();
    for (std::thread& t : variantThreads_) {
        if (t.joinable()) t.join();
    }
    variantThreads_.clear();
}

void Device::trackLive(Texture* texture) {
    const std::lock_guard lock(liveMutex_);
    liveTextures_.insert(texture);
}
void Device::trackLive(Buffer* buffer) {
    const std::lock_guard lock(liveMutex_);
    liveBuffers_.insert(buffer);
}
bool Device::untrackLive(Texture* texture) {
    const std::lock_guard lock(liveMutex_);
    if (texture->orphaned_) return false;
    liveTextures_.erase(texture);
    return true;
}
bool Device::untrackLive(Buffer* buffer) {
    const std::lock_guard lock(liveMutex_);
    if (buffer->orphaned_) return false;
    liveBuffers_.erase(buffer);
    return true;
}

void Device::orphanLive() {
    const std::lock_guard lock(liveMutex_);
    if (!liveTextures_.empty() || !liveBuffers_.empty()) {
        spdlog::info("d3d9: device destroyed with {} textures and {} buffers still alive (released later by the engine, e.g. at "
                     "process exit): their GPU objects are freed now",
                     liveTextures_.size(), liveBuffers_.size());
    }
    for (Texture* t : liveTextures_) t->orphan();
    for (Buffer* b : liveBuffers_) b->orphan();
    liveTextures_.clear();
    liveBuffers_.clear();
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
        for (DrawObserver* o : observers_) o->textureReleased(t.get());
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
        if (!observers_.empty()) notifyBufferChanged(b->id());
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
    if (slot < kSamplerCount && ss < SAMP_COUNT && samp_[slot][ss] != value) {
        samp_[slot][ss] = value;
        samplerSlotCache_[slot] = UINT32_MAX;
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

void Device::setVertexShader(const std::shared_ptr<Shader>& shader) {
    if (const ShaderRef ref(shader); ref != vs_) {
        vs_ = ref;
        vsConstDirty_ = true;
    }
}

void Device::setPixelShader(const std::shared_ptr<Shader>& shader) {
    if (const ShaderRef ref(shader); ref != ps_) {
        ps_ = ref;
        psConstDirty_ = true;
    }
}

// The register setters mark the stage dirty (a new StageConsts block at its next draw) only when they change a register: on a
// clean stage the written range is compared first (equal = nothing to do). Once dirty they just copy. The native frame
// re-sets every live register of every composed draw, mostly with the values already there (vulkanize plan.md V1)
namespace {
template <class T>
bool writeRegisters(T* dst, const T* src, std::size_t bytes, bool& dirty, bool compare, std::uint64_t& unchanged) {
    if (!dirty && compare) {
        if (std::memcmp(dst, src, bytes) == 0) {
            ++unchanged;
            return false;
        }
        dirty = true;
    }
    std::memcpy(dst, src, bytes);
    return true;
}
}  // namespace

// the register windows (abi.h) a write of [start, start + count) touches
std::uint32_t Device::constWindowMask(bool vertex, std::uint32_t start, std::uint32_t count) {
    const std::uint32_t* f = vertex ? shader::abi::kVsWindowFirst : shader::abi::kPsWindowFirst;
    std::uint32_t mask = 0;
    for (std::uint32_t w = 0; w < shader::abi::kConstWindows; ++w) {
        if (start < f[w + 1] && start + count > f[w]) mask |= 1u << w;
    }
    return mask;
}

void Device::setVertexShaderConstantF(std::uint32_t start, const float* data, std::uint32_t count) {
    if (start >= kVsFloatConsts || data == nullptr) return;
    count = std::min(count, kVsFloatConsts - start);
    // split constants need the comparison also on a stage that is already dirty (the windows are tracked apart)
    if (constCompare_ && (splitConstants_ || !vsConstDirty_)) {
        // V4: only the span from the first to the last register that differs is copied (and dirties its windows)
        std::uint32_t first = 0, last = 0;
        if (!diffRegisters(vsF_ + start, reinterpret_cast<const float(*)[4]>(data), count, first, last)) {
            ++perf_.constSetsUnchanged;
            return;
        }
        std::memcpy(vsF_[start + first], data + first * 4, (last - first + 1) * 16);
        vsConstDirty_ = true;
        constWinDirty_[0] |= constWindowMask(true, start + first, last - first + 1);
        return;
    }
    std::memcpy(vsF_[start], data, count * 16);
    vsConstDirty_ = true;
    constWinDirty_[0] |= constWindowMask(true, start, count);
}

void Device::setPixelShaderConstantF(std::uint32_t start, const float* data, std::uint32_t count) {
    if (start >= kPsFloatConsts || data == nullptr) return;
    count = std::min(count, kPsFloatConsts - start);
    if (constCompare_ && (splitConstants_ || !psConstDirty_)) {
        std::uint32_t first = 0, last = 0;
        if (!diffRegisters(psF_ + start, reinterpret_cast<const float(*)[4]>(data), count, first, last)) {
            ++perf_.constSetsUnchanged;
            return;
        }
        std::memcpy(psF_[start + first], data + first * 4, (last - first + 1) * 16);
        psConstDirty_ = true;
        constWinDirty_[1] |= constWindowMask(false, start + first, last - first + 1);
        return;
    }
    std::memcpy(psF_[start], data, count * 16);
    psConstDirty_ = true;
    constWinDirty_[1] |= constWindowMask(false, start, count);
}

void Device::setVertexShaderConstantI(std::uint32_t start, const std::int32_t* data, std::uint32_t count) {
    if (start >= 16 || data == nullptr) return;
    count = std::min(count, 16 - start);
    writeRegisters(vsI_[start], data, count * 16, vsConstDirty_, constCompare_, perf_.constSetsUnchanged);
}

void Device::setPixelShaderConstantI(std::uint32_t start, const std::int32_t* data, std::uint32_t count) {
    if (start >= 16 || data == nullptr) return;
    count = std::min(count, 16 - start);
    writeRegisters(psI_[start], data, count * 16, psConstDirty_, constCompare_, perf_.constSetsUnchanged);
}

void Device::setVertexShaderConstantB(std::uint32_t start, const std::int32_t* data, std::uint32_t count) {
    if (start >= 16 || data == nullptr) return;
    count = std::min(count, 16 - start);
    for (std::uint32_t i = 0; i < count; ++i) {
        const std::uint32_t v = data[i] != 0 ? 1u : 0u;
        if (vsB_[start + i] != v || !constCompare_) {
            vsB_[start + i] = v;
            vsConstDirty_ = true;
        }
    }
}

void Device::setPixelShaderConstantB(std::uint32_t start, const std::int32_t* data, std::uint32_t count) {
    if (start >= 16 || data == nullptr) return;
    count = std::min(count, 16 - start);
    for (std::uint32_t i = 0; i < count; ++i) {
        const std::uint32_t v = data[i] != 0 ? 1u : 0u;
        if (psB_[start + i] != v || !constCompare_) {
            psB_[start + i] = v;
            psConstDirty_ = true;
        }
    }
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
    if (index == 0 && surface != nullptr && phases_.pass() == scene::Pass::ShadowMask && vk::names::enabled()) {
        debugnames::onShadowMaskTarget(*this, surface);  // debug names of the mask chain's targets (d3d9/target_names.h)
    }
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
    ++stats_.totalClears;
    if (rendering_ && (scopeGbuf_ != gbufferScope() || scopeSecondary_ != wantSecondaryScope())) endRendering();
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
        for (std::uint32_t i = 0; i < engineColorCount_; ++i) {
            atts[n].aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            atts[n].colorAttachment = i;
            std::memcpy(atts[n].clearValue.color.float32, c, sizeof(c));
            ++n;
        }
    }
    VkImageAspectFlags ds = 0;
    if ((flags & CLEAR_ZBUFFER) != 0 && depthFormat_ != VK_FORMAT_UNDEFINED) {
        ds |= VK_IMAGE_ASPECT_DEPTH_BIT;
        // the convention of the buffer follows the view that clears it (reversed: 1 -> 0 = infinity, else 1 - k z)
        ds_->texture()->reverseZ_ = reverseZ_ && phases_.current().view == scene::View::Main;
        if (ds_->texture()->reverseZ_) z = z >= 1.0f ? 0.0f : 1.0f - reverseDepthK() * std::max(z, 0.0f);
    }
    if ((flags & CLEAR_STENCIL) != 0 && stencilFormat_ != VK_FORMAT_UNDEFINED) ds |= VK_IMAGE_ASPECT_STENCIL_BIT;
    if (ds != 0) {
        atts[n].aspectMask = ds;
        atts[n].clearValue.depthStencil = {std::clamp(z, 0.0f, 1.0f), stencil & 0xFF};
        ++n;
    }
    if (n > 0) {
        flushBatch();  // the draws batched before the clear
        if (scopeSecondary_) {
            ClearOp op;
            std::copy(atts, atts + n, op.atts);
            op.count = n;
            op.rect = rect;
            emitClear(op);
        } else {
            vkCmdClearAttachments(frames_.cmd(), n, atts, 1, &rect);
        }
    }
}

void Device::blit(vk::Image& src, std::uint32_t srcLevel, std::uint32_t srcLayer, const VkOffset3D srcBox[2], vk::Image& dst,
                  std::uint32_t dstLevel, std::uint32_t dstLayer, const VkOffset3D dstBox[2], VkFilter filter) {
    VkCommandBuffer cmd = frames_.cmd();
    const bool same = &src == &dst;
    if (same) {
        src.transition(cmd, VK_IMAGE_LAYOUT_GENERAL, true);  // forced: orders this blit after the previous mip's
    } else {
        src.transition(cmd, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
        dst.transition(cmd, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    }
    VkImageBlit2 region{VK_STRUCTURE_TYPE_IMAGE_BLIT_2};
    region.srcSubresource = {src.aspect(), srcLevel, srcLayer, 1};
    region.srcOffsets[0] = srcBox[0];
    region.srcOffsets[1] = srcBox[1];
    region.dstSubresource = {dst.aspect(), dstLevel, dstLayer, 1};
    region.dstOffsets[0] = dstBox[0];
    region.dstOffsets[1] = dstBox[1];
    VkBlitImageInfo2 bi{VK_STRUCTURE_TYPE_BLIT_IMAGE_INFO_2};
    bi.srcImage = src.image();
    bi.srcImageLayout = same ? VK_IMAGE_LAYOUT_GENERAL : VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    bi.dstImage = dst.image();
    bi.dstImageLayout = same ? VK_IMAGE_LAYOUT_GENERAL : VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
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
    dst->texture()->gpuWritten_ = true;  // a later READONLY lock must not upload the (stale) CPU copy over it
}

bool Device::readRenderTarget(Surface* src, Texture* dst, std::uint32_t dstLevel) {
    if (src == nullptr || dst == nullptr || src->texture()->isNull()) {
        return false;
    }
    std::vector<std::uint8_t> pixels;
    const std::uint32_t w = src->width(), h = src->height();
    const vk::FormatInfo& f = src->texture()->format();
    const VkDeviceSize size = vk::storageSize(f, w, h);
    if (!readImageLevel(src->texture()->image(), src->level(), src->face(), {w, h, 1}, size, pixels)) {
        return false;
    }
    Texture::Locked lk;
    const bool ok = dst->lock(dstLevel, 0, nullptr, LOCK_DISCARD, lk);  // overwritten whole: no read-back of the old contents
    if (ok) {
        const std::uint32_t rowBytes = static_cast<std::uint32_t>(size / h);
        for (std::uint32_t y = 0; y < h; ++y) {
            std::memcpy(static_cast<std::uint8_t*>(lk.bits) + static_cast<std::size_t>(y) * lk.rowPitch,
                        pixels.data() + static_cast<std::size_t>(y) * rowBytes, std::min(rowBytes, lk.rowPitch));
        }
        dst->unlock(dstLevel, 0);
    }
    return ok;
}

namespace {

// One 4x4 block of BC1 / BC2 / BC3 (D3D9 DXT1 / DXT2-3 / DXT4-5) to 16 BGRA texels (A8R8G8B8 in memory: B, G, R, A).
void decodeDxtBlock(const std::uint8_t* b, std::uint32_t d3dFormat, std::uint8_t out[16][4]) {
    const std::uint8_t* c = d3dFormat == vk::d3dfmt::DXT1 ? b : b + 8;
    std::uint16_t c0, c1;
    std::memcpy(&c0, c, 2);
    std::memcpy(&c1, c + 2, 2);
    const auto expand = [](std::uint16_t v, std::uint8_t* rgb) {  // rgb = R, G, B
        rgb[0] = static_cast<std::uint8_t>(((v >> 11) & 31) * 255 / 31);
        rgb[1] = static_cast<std::uint8_t>(((v >> 5) & 63) * 255 / 63);
        rgb[2] = static_cast<std::uint8_t>((v & 31) * 255 / 31);
    };
    std::uint8_t pal[4][4];  // R, G, B, A
    expand(c0, pal[0]);
    expand(c1, pal[1]);
    pal[0][3] = pal[1][3] = pal[2][3] = 255;
    pal[3][3] = 255;
    const bool fourColours = d3dFormat != vk::d3dfmt::DXT1 || c0 > c1;
    for (int i = 0; i < 3; ++i) {
        if (fourColours) {
            pal[2][i] = static_cast<std::uint8_t>((2 * pal[0][i] + pal[1][i]) / 3);
            pal[3][i] = static_cast<std::uint8_t>((pal[0][i] + 2 * pal[1][i]) / 3);
        } else {
            pal[2][i] = static_cast<std::uint8_t>((pal[0][i] + pal[1][i]) / 2);
            pal[3][i] = 0;
        }
    }
    if (!fourColours) pal[3][3] = 0;  // DXT1 transparent black
    std::uint32_t idx;
    std::memcpy(&idx, c + 4, 4);
    std::uint8_t alpha[16];
    std::fill(alpha, alpha + 16, std::uint8_t{255});
    if (d3dFormat == vk::d3dfmt::DXT2 || d3dFormat == vk::d3dfmt::DXT3) {
        for (int i = 0; i < 16; ++i) {
            const std::uint8_t n = (b[i / 2] >> ((i & 1) * 4)) & 15;
            alpha[i] = static_cast<std::uint8_t>(n * 17);
        }
    } else if (d3dFormat == vk::d3dfmt::DXT4 || d3dFormat == vk::d3dfmt::DXT5) {
        std::uint8_t a[8] = {b[0], b[1]};
        for (int i = 1; i <= (a[0] > a[1] ? 6 : 4); ++i) {
            a[i + 1] = a[0] > a[1] ? static_cast<std::uint8_t>(((7 - i) * a[0] + i * a[1]) / 7)
                                   : static_cast<std::uint8_t>(((5 - i) * a[0] + i * a[1]) / 5);
        }
        if (a[0] <= a[1]) {
            a[6] = 0;
            a[7] = 255;
        }
        std::uint64_t bits = 0;
        for (int i = 0; i < 6; ++i) bits |= static_cast<std::uint64_t>(b[2 + i]) << (8 * i);
        for (int i = 0; i < 16; ++i) alpha[i] = a[(bits >> (3 * i)) & 7];
    }
    for (int i = 0; i < 16; ++i) {
        const std::uint8_t* p = pal[(idx >> (2 * i)) & 3];
        out[i][0] = p[2];
        out[i][1] = p[1];
        out[i][2] = p[0];
        out[i][3] = d3dFormat == vk::d3dfmt::DXT1 ? p[3] : alpha[i];
    }
}

bool isBlockDxt(std::uint32_t f) {
    return f == vk::d3dfmt::DXT1 || f == vk::d3dfmt::DXT2 || f == vk::d3dfmt::DXT3 || f == vk::d3dfmt::DXT4 || f == vk::d3dfmt::DXT5;
}

}  // namespace

bool Device::loadSurfaceFromSurface(Texture* dst, std::uint32_t dstLevel, const std::int32_t* dstRect, Texture* src,
                                    std::uint32_t srcLevel, const std::int32_t* srcRect, bool linear, std::string* why) {
    const auto fail = [&](const char* reason) {
        if (why != nullptr) *why = reason;
        return false;
    };
    if (dst == nullptr || src == nullptr || !dst->valid() || !src->valid() || dst->isNull() || src->isNull() ||
        dstLevel >= dst->levels() || srcLevel >= src->levels() || dst->desc().type != TextureType::Tex2D ||
        src->desc().type != TextureType::Tex2D) {
        return fail("invalid textures / levels");
    }
    if (!recordingThread()) {
        return fail("not on the render thread");
    }
    const auto identity = [](VkComponentSwizzle s, VkComponentSwizzle own) {
        return s == VK_COMPONENT_SWIZZLE_IDENTITY || s == own;
    };
    const VkComponentMapping& sw = src->format().swizzle;
    if (!identity(sw.r, VK_COMPONENT_SWIZZLE_R) || !identity(sw.g, VK_COMPONENT_SWIZZLE_G) ||
        !identity(sw.b, VK_COMPONENT_SWIZZLE_B) || !identity(sw.a, VK_COMPONENT_SWIZZLE_A) || src->isDepth()) {
        return fail("source format needs a swizzle / is depth");
    }
    VkFormatProperties fp{};
    vkGetPhysicalDeviceFormatProperties(ctx_.physicalDevice(), src->format().format, &fp);
    if ((fp.optimalTilingFeatures & VK_FORMAT_FEATURE_BLIT_SRC_BIT) == 0) {
        return fail("the source format cannot be blitted");
    }
    const std::uint32_t lw = dst->levelWidth(dstLevel), lh = dst->levelHeight(dstLevel);
    std::int32_t r[4] = {0, 0, static_cast<std::int32_t>(lw), static_cast<std::int32_t>(lh)};
    if (dstRect != nullptr) {
        r[0] = std::clamp<std::int32_t>(dstRect[0], 0, static_cast<std::int32_t>(lw));
        r[1] = std::clamp<std::int32_t>(dstRect[1], 0, static_cast<std::int32_t>(lh));
        r[2] = std::clamp<std::int32_t>(dstRect[2], 0, static_cast<std::int32_t>(lw));
        r[3] = std::clamp<std::int32_t>(dstRect[3], 0, static_cast<std::int32_t>(lh));
        if (r[0] >= r[2] || r[1] >= r[3]) {
            return fail("empty destination rect");
        }
    }
    endRendering();
    src->prepareForSampling(frames_.cmd());  // pending file data first
    Surface* srcSurface = src->surface(srcLevel, 0);
    if (dst->isRenderTarget()) {
        stretchRect(srcSurface, srcRect, dst->surface(dstLevel, 0), r, linear);
        return true;
    }
    if (dst->format().compressed || dst->format().convert != vk::Convert::None) {
        return fail("compressed / converted destination format");
    }
    const std::uint32_t w = static_cast<std::uint32_t>(r[2] - r[0]), h = static_cast<std::uint32_t>(r[3] - r[1]);
    if (src->format().compressed && isBlockDxt(src->desc().format) && dst->format().format == VK_FORMAT_B8G8R8A8_UNORM && !linear) {
        // A block-compressed source into 32-bit CPU texels (the sky absorption image: DXT1 -> A8R8G8B8): decoded on the CPU
        // (nearest). vkCmdBlitImage from a BC source gave wrong texels here (only the first block row / column region of an
        // 8x8 DXT1 level came out, the rest 0) although the source image and the blit parameters are right.
        const std::uint32_t sLvlW = src->levelWidth(srcLevel), sLvlH = src->levelHeight(srcLevel);
        const std::int32_t sr[4] = {srcRect != nullptr ? srcRect[0] : 0, srcRect != nullptr ? srcRect[1] : 0,
                                    srcRect != nullptr ? srcRect[2] : static_cast<std::int32_t>(sLvlW),
                                    srcRect != nullptr ? srcRect[3] : static_cast<std::int32_t>(sLvlH)};
        if (sr[0] >= 0 && sr[1] >= 0 && sr[0] < sr[2] && sr[1] < sr[3] && sr[2] <= static_cast<std::int32_t>(sLvlW) &&
            sr[3] <= static_cast<std::int32_t>(sLvlH)) {
            std::vector<std::uint8_t> raw;
            if (!readImageLevel(src->image(), srcLevel, 0, {sLvlW, sLvlH, 1}, vk::storageSize(src->format(), sLvlW, sLvlH), raw)) {
                return fail("read-back of the source failed");
            }
            const std::uint32_t bw = (sLvlW + 3) / 4, blockBytes = src->format().blockBytes;
            const std::uint32_t rw = static_cast<std::uint32_t>(sr[2] - sr[0]), rh = static_cast<std::uint32_t>(sr[3] - sr[1]);
            Texture::Locked lk;
            if (!dst->lock(dstLevel, 0, r, dstRect == nullptr ? LOCK_DISCARD : 0, lk)) {
                return fail("destination lock failed");
            }
            std::uint8_t block[16][4];
            std::uint32_t cachedBlock = ~0u;
            for (std::uint32_t y = 0; y < h; ++y) {
                const std::uint32_t sy = static_cast<std::uint32_t>(sr[1]) + (y * rh + rh / 2) / h;
                auto* row = reinterpret_cast<std::uint32_t*>(static_cast<std::uint8_t*>(lk.bits) + static_cast<std::size_t>(y) * lk.rowPitch);
                for (std::uint32_t x = 0; x < w; ++x) {
                    const std::uint32_t sx = static_cast<std::uint32_t>(sr[0]) + (x * rw + rw / 2) / w;
                    const std::uint32_t bi = (sy / 4) * bw + sx / 4;
                    if (bi != cachedBlock) {
                        decodeDxtBlock(raw.data() + static_cast<std::size_t>(bi) * blockBytes, src->desc().format, block);
                        cachedBlock = bi;
                    }
                    std::memcpy(&row[x], block[(sy & 3) * 4 + (sx & 3)], 4);
                }
            }
            dst->unlock(dstLevel, 0);
            return true;
        }
    }
    // a CPU-side destination: blit into a temporary render target of the destination's format, read it back into the rect
    TextureDesc td;
    td.width = w;
    td.height = h;
    td.levels = 1;
    td.format = dst->desc().format;
    td.usage = USAGE_RENDERTARGET;
    td.pool = POOL_DEFAULT;
    auto tmp = createTexture(td);
    if (!tmp) {
        return fail("temporary render target");
    }
    stretchRect(srcSurface, srcRect, tmp->surface(0, 0), nullptr, linear);
    std::vector<std::uint8_t> pixels;
    const VkDeviceSize size = vk::storageSize(tmp->format(), w, h);
    const bool read = readImageLevel(tmp->image(), 0, 0, {w, h, 1}, size, pixels);
    release(std::move(tmp));
    if (!read) {
        return fail("read-back failed");
    }
    Texture::Locked lk;
    if (!dst->lock(dstLevel, 0, r, dstRect == nullptr ? LOCK_DISCARD : 0, lk)) {
        return fail("destination lock failed");
    }
    const std::size_t rowBytes = static_cast<std::size_t>(size / h);
    for (std::uint32_t y = 0; y < h; ++y) {
        std::memcpy(static_cast<std::uint8_t*>(lk.bits) + static_cast<std::size_t>(y) * lk.rowPitch, pixels.data() + y * rowBytes,
                    rowBytes);
    }
    dst->unlock(dstLevel, 0);
    return true;
}

bool Device::readImageLevel(vk::Image& img, std::uint32_t level, std::uint32_t layer, VkExtent3D extent, VkDeviceSize size,
                            std::vector<std::uint8_t>& out) {
    if (!img.valid() || size == 0) {
        return false;
    }
    const auto t0 = std::chrono::steady_clock::now();
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
    vk::names::object(ctx_, rb, VK_OBJECT_TYPE_BUFFER, "rvk image read-back");
    img.transition(frames_.cmd(), VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    VkBufferImageCopy region{};
    region.imageSubresource = {img.aspect() & ~VkImageAspectFlags(VK_IMAGE_ASPECT_STENCIL_BIT), level, layer, 1};
    region.imageExtent = extent;
    vkCmdCopyImageToBuffer(frames_.cmd(), img.image(), VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, rb, 1, &region);
    // the copy must finish before the CPU reads: end the frame's work here and start a new command buffer
    submitFrame(VK_NULL_HANDLE, VK_NULL_HANDLE);
    frames_.waitFor(frames_.currentValue() - 1);
    beginFrame();
    vmaInvalidateAllocation(ctx_.allocator(), ra, 0, VK_WHOLE_SIZE);
    const auto* p = static_cast<const std::uint8_t*>(info.pMappedData);
    out.assign(p, p + size);
    vmaDestroyBuffer(ctx_.allocator(), rb, ra);
    ++perf_.readbacks;
    perf_.readbackMs += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    return true;
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

// ---- pipelines of earlier runs ---------------------------------------------------------------------------------------

namespace {

constexpr char kKeysMagic[8] = {'R', 'V', 'K', 'P', 'K', 'E', 'Y', '1'};

}  // namespace

void Device::loadPipelineKeys() {
    if (options_.cacheDir.empty()) {
        return;
    }
    std::ifstream in(options_.cacheDir / "pipelines.keys", std::ios::binary);
    char magic[8]{};
    std::uint32_t recordSize = 0;
    if (!in.read(magic, sizeof(magic)) || std::memcmp(magic, kKeysMagic, sizeof(magic)) != 0 ||
        !in.read(reinterpret_cast<char*>(&recordSize), sizeof(recordSize)) || recordSize != sizeof(PipelineKey)) {
        return;  // none yet, or written by another build (the description layout changed)
    }
    const std::lock_guard lock(keysMutex_);
    PipelineKey k;
    while (in.read(reinterpret_cast<char*>(&k), sizeof(k))) {
        if (keySet_.emplace(reinterpret_cast<const char*>(&k), sizeof(k)).second) {
            keys_.push_back(k);
            pendingKeys_.emplace(k.vs, keys_.size() - 1);
            if (k.ps != k.vs) {
                pendingKeys_.emplace(k.ps, keys_.size() - 1);
            }
        }
    }
    spdlog::info("d3d9: {} pipelines known from earlier runs (built in the background once their shaders exist)", keys_.size());
}

void Device::savePipelineKeys() {
    if (options_.cacheDir.empty()) {
        return;
    }
    std::vector<char> blob;
    {
        const std::lock_guard lock(keysMutex_);
        if (!keysDirty_) {
            return;
        }
        keysDirty_ = false;
        blob.resize(sizeof(kKeysMagic) + sizeof(std::uint32_t) + keys_.size() * sizeof(PipelineKey));
        std::memcpy(blob.data(), kKeysMagic, sizeof(kKeysMagic));
        const std::uint32_t recordSize = sizeof(PipelineKey);
        std::memcpy(blob.data() + sizeof(kKeysMagic), &recordSize, sizeof(recordSize));
        if (!keys_.empty()) {
            std::memcpy(blob.data() + sizeof(kKeysMagic) + sizeof(recordSize), keys_.data(), keys_.size() * sizeof(PipelineKey));
        }
    }
    if (keySaver_.joinable()) {
        keySaver_.join();
    }
    // the file I/O off the render thread; <file>.tmp + rename so a killed game leaves the previous file
    keySaver_ = std::thread([file = options_.cacheDir / "pipelines.keys", blob = std::move(blob)] {
        std::error_code ec;
        std::filesystem::create_directories(file.parent_path(), ec);
        auto tmp = file;
        tmp += ".tmp";
        {
            std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
            out.write(blob.data(), static_cast<std::streamsize>(blob.size()));
        }
        std::filesystem::rename(tmp, file, ec);
    });
}

// a pipeline was created for a draw: remember it by its shaders' hashes (render thread)
void Device::recordPipelineKey(const vk::PipelineDesc& pd) {
    PipelineKey k;
    {
        const std::lock_guard lock(shaderMutex_);
        const auto v = moduleHashes_.find(pd.vs);
        const auto p = moduleHashes_.find(pd.fs);
        if (v == moduleHashes_.end() || p == moduleHashes_.end()) {
            return;
        }
        k.vs = v->second;
        k.ps = p->second;
    }
    std::memcpy(&k.desc, &pd, sizeof(pd));
    k.desc.vs = VK_NULL_HANDLE;
    k.desc.fs = VK_NULL_HANDLE;
    const std::lock_guard lock(keysMutex_);
    if (keySet_.emplace(reinterpret_cast<const char*>(&k), sizeof(k)).second) {
        keys_.push_back(k);
        keysDirty_ = true;
    }
}

// a shader was created (any thread): queue the known pipelines whose shaders now both exist
void Device::prewarmPipelines(std::uint64_t shaderHash) {
    std::vector<PipelineKey> ready;
    {
        const std::lock_guard lock(keysMutex_);
        auto [b, e] = pendingKeys_.equal_range(shaderHash);
        for (auto it = b; it != e;) {
            const PipelineKey& k = keys_[it->second];
            const std::uint64_t other = k.vs == shaderHash ? k.ps : k.vs;
            bool otherReady = other == shaderHash;
            if (!otherReady) {
                const std::lock_guard sl(shaderMutex_);
                otherReady = shaders_.contains(other);
            }
            if (otherReady) {
                ready.push_back(k);
                // the partner's entry for the same key is skipped by the descriptor check below when it comes
                it = pendingKeys_.erase(it);
            } else {
                ++it;
            }
        }
    }
    for (PipelineKey& k : ready) {
        {
            const std::lock_guard sl(shaderMutex_);
            const auto v = shaders_.find(k.vs);
            const auto p = shaders_.find(k.ps);
            if (v == shaders_.end() || p == shaders_.end()) {
                continue;
            }
            k.desc.vs = v->second->module;
            k.desc.fs = p->second->module;
        }
        pipelines_.prewarm(k.desc);
    }
}

// ---- samplers -------------------------------------------------------------------------------------------------------

bool Device::SamplerKey::operator==(const SamplerKey& o) const { return std::memcmp(states, o.states, sizeof(states)) == 0; }

std::size_t Device::SamplerKeyHash::operator()(const SamplerKey& k) const {
    return static_cast<std::size_t>(fnv1a(k.states, sizeof(k.states)));
}

std::uint32_t Device::samplerIndex(std::uint32_t slot) {
    if (samplerSlotCache_[slot] != UINT32_MAX) {
        return samplerSlotCache_[slot];
    }
    auto& memo = samplerMemo_[slot];
    const std::uint32_t* row = samp_[slot].data();
    for (std::size_t i = 0; i < memo.size(); ++i) {
        if (memo[i].index != UINT32_MAX && std::memcmp(memo[i].states.data(), row, sizeof(memo[i].states)) == 0) {
            samplerSlotCache_[slot] = memo[i].index;
            if (i != 0) std::swap(memo[0], memo[i]);
            return memo[0].index;
        }
    }
    const std::uint32_t index = lookupSampler(slot);
    samplerSlotCache_[slot] = index;
    memo[1] = memo[0];
    std::memcpy(memo[0].states.data(), row, sizeof(memo[0].states));
    memo[0].index = index;
    return index;
}

// the slot's sampler with `bias` added to its LOD bias (not cached per slot: the bias depends on the draw)
std::uint32_t Device::samplerIndexBiased(std::uint32_t slot, float bias) {
    std::uint32_t states[SAMP_COUNT];
    std::memcpy(states, samp_[slot].data(), sizeof(states));
    states[SAMP_MIPMAPLODBIAS] = std::bit_cast<std::uint32_t>(asFloat(states[SAMP_MIPMAPLODBIAS]) + bias);
    return lookupSamplerStates(states);
}

std::uint32_t Device::lookupSampler(std::uint32_t slot) { return lookupSamplerStates(samp_[slot].data()); }

std::uint32_t Device::samplerFor(bool linear, bool clamp) {
    std::uint32_t states[SAMP_COUNT];
    for (std::uint32_t i = 0; i < SAMP_COUNT; ++i) states[i] = defaultSamplerState(i);
    const std::uint32_t f = linear ? TEXF_LINEAR : TEXF_POINT;
    states[SAMP_MAGFILTER] = states[SAMP_MINFILTER] = f;
    states[SAMP_MIPFILTER] = TEXF_LINEAR;
    states[SAMP_ADDRESSU] = states[SAMP_ADDRESSV] = states[SAMP_ADDRESSW] = clamp ? TADDRESS_CLAMP : TADDRESS_WRAP;
    return lookupSamplerStates(states);
}

void Device::addDrawReplacer(DrawReplacer* replacer) {
    if (replacer != nullptr && std::find(replacers_.begin(), replacers_.end(), replacer) == replacers_.end()) {
        replacers_.push_back(replacer);
    }
}

void Device::removeDrawReplacer(DrawReplacer* replacer) { std::erase(replacers_, replacer); }

void Device::addDrawObserver(DrawObserver* observer) {
    if (observer != nullptr && std::find(observers_.begin(), observers_.end(), observer) == observers_.end()) {
        observers_.push_back(observer);
    }
}

void Device::removeDrawObserver(DrawObserver* observer) { std::erase(observers_, observer); }

void Device::registerReplacement(std::shared_ptr<Shader> vs, std::shared_ptr<Shader> ps, bool procedural) {
    if (!vs || !ps) return;
    if (waterTemplateValid_) prebuildReplacement(*vs, *ps, procedural);
    registered_.push_back({std::move(vs), std::move(ps), procedural});
}

bool Device::prebuildReplacement(const Shader& vs, const Shader& ps, bool procedural) {
    vk::PipelineDesc pd{};
    if (!replacementDesc(vs, ps, procedural, pd)) return false;
    if (!pipelines_.contains(pd)) pipelines_.prewarm(pd);
    return true;
}

bool Device::replacementReady(const Shader& vs, const Shader& ps, bool procedural) const {
    vk::PipelineDesc pd{};
    return replacementDesc(vs, ps, procedural, pd) && pipelines_.contains(pd);
}

bool Device::replacementDesc(const Shader& vs, const Shader& ps, bool procedural, vk::PipelineDesc& out) const {
    if (!waterTemplateValid_ || vs.module == VK_NULL_HANDLE || ps.module == VK_NULL_HANDLE) return false;
    vk::PipelineDesc pd = waterTemplate_;
    pd.vs = vs.module;
    pd.fs = ps.module;
    if (procedural) {
        pd.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
        pd.bindingCount = 0;
        pd.attributeCount = 0;
        std::memset(pd.bindings, 0, sizeof(pd.bindings));
        std::memset(pd.attributes, 0, sizeof(pd.attributes));
    }
    for (std::uint32_t i = 0; i < pd.colorCount; ++i) {
        if (!ctx_.features().extendedDynamicState3) {
            pd.blend[i] = vk::BlendAttachment{};
            pd.blend[i].writeMask = 0xF;
        }
    }
    out = pd;
    return true;
}

std::uint32_t Device::lookupSamplerStates(const std::uint32_t* states) {
    SamplerKey key{};
    std::memcpy(key.states, states, sizeof(key.states));
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
    if (ctx_.namesEnabled()) {
        static constexpr const char* kFilter[] = {"none", "point", "linear", "aniso", "pyramid", "gauss"};
        static constexpr const char* kAddress[] = {"?", "wrap", "mirror", "clamp", "border", "mirror-once"};
        const auto f = [](const char* const* t, std::uint32_t v, std::uint32_t n) { return v < n ? t[v] : "?"; };
        vk::names::object(ctx_, sampler, VK_OBJECT_TYPE_SAMPLER,
                          std::format("sampler [slot {}] min {} mag {} mip {} | U {} V {} W {}{}", index, f(kFilter, s[SAMP_MINFILTER], 6),
                                      f(kFilter, s[SAMP_MAGFILTER], 6), f(kFilter, s[SAMP_MIPFILTER], 6), f(kAddress, s[SAMP_ADDRESSU], 6),
                                      f(kAddress, s[SAMP_ADDRESSV], 6), f(kAddress, s[SAMP_ADDRESSW], 6),
                                      aniso ? std::format(" | aniso {}x", ci.maxAnisotropy) : std::string()));
    }
    samplers_.emplace(key, index);
    return index;
}

}  // namespace rvk::d3d9
