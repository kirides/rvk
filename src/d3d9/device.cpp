// d3d9::Device: creation, frames, resources, state setters, clears, copies. Draw preparation is in draw.cpp.

#include "d3d9/device.h"
#include "core/perf_window.h"
#include "core/profile.h"
#include "core/stall_sampler.h"
#include "shader/uber.h"

#include <windows.h>

#include <bit>
#include <map>
#include <chrono>
#include <charconv>
#include <cmath>
#include <cstring>
#include <fstream>

#include <spdlog/spdlog.h>

#include "d3d9/convert.h"
#include "engine/exposure.h"
#include "d3d9/memeq.h"
#include "d3d9/target_names.h"
#include "shader/sm_ir.h"
#include "shader/spirv.h"

namespace rvk::d3d9 {
namespace {
// diagnostics (RVK_EXPOSURE_LOG): the luminance chain's R32F targets by width (64 .. 1) and the HDR scene colour
Texture* g_diagLum[8] = {};
Texture* g_diagScene = nullptr;

// bump when the translator's output changes: invalidates the SPIR-V disk cache
// 2: the ABI declarations come from shaders/include/rvk_abi.glsl (48-byte push block, FrameBlock)
constexpr std::uint32_t kTranslatorVersion = 12;  // 12: precise position on Vulkan too; 11: rvk_pow on Vulkan too; 10: rcp / rsq / exp / log clamped to +-FLT_MAX; 9: DXIL parity (rvk_draw.hlsli: uint2 frame, window members, address helpers); 8: HLSL (shader/hlsl.cpp) through DXC
// bump when the tint epilogue (TranslateOptions::tintEpilogue) changes: the tint variants' cache files
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
    frameAddr_ = {};  // the frame block gets the camera
    return true;
}

void Device::tagImage(Texture* texture, scene::ImageRole role, const char* label, std::uint32_t labelIndex) {
    if (texture == nullptr || role == scene::ImageRole::None) return;
    if (roles_[static_cast<std::size_t>(role)] != texture) frameAddr_ = {};  // the frame block lists the role images
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
                     d.height, rhi::d3dFormatName(d.format), d.type == TextureType::Cube ? " cube" : "",
                     texture->isDepth() ? " depth" : "", stats_.frames + 1,
                     old != scene::ImageRole::None ? std::format(" (was the {})", scene::name(old)) : std::string());
    }
}

void Device::forgetImage(Texture* texture) {
    for (auto& r : roles_) {
        if (r == texture) {
            r = nullptr;
            frameAddr_ = {};
        }
    }
}

// ---- enhancements (d3d9/inject.h, docs/research/injection.md) ----------------------------------------------------------------

rhi::GpuRef Device::frameBlockAddress() {
    if (!frameAddr_.null()) return frameAddr_;
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
        x.rtRecords = rtScene_.records.bits;
        x.rtInstances = rtScene_.instances;
        x.rtRecordCount = rtScene_.recordCount;
        x.rtTlasSlot = rtScene_.tlasSlot;
    }
    if (!giHeader_.empty()) {
        const rhi::RingAlloc g = game_->allocRing(giHeader_.size(), 16);
        if (g.ptr != nullptr) {
            std::memcpy(g.ptr, giHeader_.data(), giHeader_.size());
            x.gi = g.ref.bits;
        }
    }
    x.ao = ao_;
    x.sgi = sgi_;
    x.jitter = jitter_;
    const rhi::RingAlloc a = game_->allocRing(sizeof(shader::abi::FrameBlock), 16);
    scene::fillFrameBlock(*static_cast<shader::abi::FrameBlock*>(a.ptr), camera_, lastCamera_, x);
    frameAddr_ = a.ref;
    ++perf_.frameBlocks;
    return frameAddr_;
}

void Device::setFrameParam(std::uint32_t index, const std::array<float, 4>& value) {
    if (index >= frameParams_.size() || frameParams_[index] == value) return;
    frameParams_[index] = value;
    frameAddr_ = {};
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
        frameAddr_ = {};
    }
}

void Device::setJitterSource(JitterSource source) {
    ++variantEpoch_;
    jitterSource_ = std::move(source);
    if (!jitterSource_ && jitter_ != std::array<float, 4>{}) {
        jitter_ = {};
        frameAddr_ = {};
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
    RVK_ZONE_DYN(injectZone, inject::name(point));
    syncPhaseRegions();
    const auto t0 = std::chrono::steady_clock::now();
    endRendering();
    gpuMark("before injection");
    Surface* rt0 = rt_[0];
    PassContext ctx{*this, game_->passCommands(), point};
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
        game_->passCommands();                 // (the list again, with nothing bound: a pass sees no state of the one before)
        {
            RegionScope region(*this, list[i].name.c_str());
            RVK_ZONE_DYN(passZone, list[i].name.c_str());
            const auto p0 = options_.perf ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
            list[i].fn(ctx);
            if (options_.perf) {
                perf_.injectPassMs[std::format("{} @{}", list[i].name, inject::name(point))] +=
                    std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - p0).count();
            }
        }
        if (game_->timestampsEnabled()) {
            const std::uint32_t slot = game_->frameSlot();
            game_->timestampMark(frameList(), slot, rhi::TimestampStream::Main,
                           game_->timestampDetails(slot) ? std::format("pass {} at {}", list[i].name, inject::name(point)) : std::string());
        }
    }
    injecting_ = false;
    invalidateRecordingState();
    ++perf_.injections;
    perf_.injectMs += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
}

bool Device::runPassNow(const char* name, const std::function<void(PassContext&)>& fn) {
    if (injecting_ || !created_ || !fn) return false;
    RVK_ZONE_DYN(passNowZone, name);
    syncPhaseRegions();
    const auto t0 = std::chrono::steady_clock::now();
    endRendering();
    gpuMark("before ad-hoc pass");
    Surface* rt0 = rt_[0];
    PassContext ctx{*this, game_->passCommands(), inject::Point::BeforeGui};
    ctx.mainView = 0;
    ctx.frameBlock = frameBlockAddress();
    ctx.sceneHdr = imageWithRole(scene::ImageRole::SceneHdr);
    ctx.depthMap = imageWithRole(scene::ImageRole::DepthMap);
    ctx.refraction = imageWithRole(scene::ImageRole::Refraction);
    ctx.target = rt0 != nullptr && !rt0->texture()->isNull() ? rt0->texture() : nullptr;
    injecting_ = true;
    game_->passCommands();
    {
        RegionScope region(*this, name);
        fn(ctx);
    }
    if (game_->timestampsEnabled()) {
        const std::uint32_t slot = game_->frameSlot();
        game_->timestampMark(frameList(), slot, rhi::TimestampStream::Main, game_->timestampDetails(slot) ? std::format("pass {} (now)", name) : std::string());
    }
    injecting_ = false;
    invalidateRecordingState();
    if (options_.perf) {
        perf_.injectPassMs[std::format("{} @now", name)] += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    }
    return true;
}

bool Device::readable(Texture* texture, rhi::CommandList& cmd) {
    if (texture == nullptr || texture->isNull()) return false;
    texture->prepareForSampling(cmd);
    texture->lastUseFrame_ = frameValue();
    return texture->rhi().layout() == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
}

bool Device::namesEnabled() const { return game_ != nullptr && game_->namesEnabled(); }

rhi::CommandList& Device::uploadList() { return game_->uploadCommands(); }  // (the backend orders it after the earlier work on its first use in a frame)

bool Device::create(const DeviceOptions& options) {
    options_ = options;
    renderThread_ = GetCurrentThreadId();
    rhi::GameDeviceOptions go;
    go.validation = options.validation;
    go.syncValidation = options.syncValidation;
    go.adapter = options.adapter;
    go.allowRayTracing = options.allowRayTracing;
    go.d3d12Messages = options.d3d12Messages;
    go.d3d12Dred = options.d3d12Dred;
    go.frameGenQueues = options.frameGenQueues;
    go.asyncPipelines = !options.offscreen;
    shader::setVaryingFillers(options.backend == rhi::Backend::D3D12);
    wp0_.on = GetEnvironmentVariableA("RVK_WP0", nullptr, 0) != 0;
    game_ = rhi::createGameDevice(options.backend, go);  // the backend seam (rhi/game_device.h): owns every API object
    if (!game_) {
        spdlog::error("d3d9: the {} game device could not be created", rhi::backendName(options.backend));
        return false;
    }
    // D24S8 depth is optional in Vulkan (AMD has none): fall back to D32S8
    d24s8_ = game_->formatSupport(VK_FORMAT_D24_UNORM_S8_UINT, VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT)
                 ? VK_FORMAT_D24_UNORM_S8_UINT
                 : VK_FORMAT_D32_SFLOAT_S8_UINT;
    const auto pipelineFile = options.cacheDir.empty() ? std::filesystem::path() : options.cacheDir / "pipelines.bin";
    if (!game_->createPipelineCaches(pipelineFile, options.pipelineLibraries)) {
        return false;
    }
    loadPipelineKeys();
    if (!options.offscreen && !pres().create(options.window, options.width, options.height, options.vsync)) {
        return false;
    }
    if (options.perf) {
        // [debug] perf: a frame that does not come within 8 s logs every thread's stack (a deadlock between the render thread, the
        // present thread and AMD's frame generation threads)
        diag::startHangWatchdog(8.0, [](void* d) { return static_cast<Device*>(d)->stats_.frames; }, this);
    }
    if (options.perf && game_->enableTimestamps()) {
        spdlog::info("d3d9: [debug] perf: GPU timestamps on{}",
                     options.perfFrame != 0 ? std::format(", pass timings of frame {}", options.perfFrame) : std::string());
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
    spdlog::info("d3d9: device ready, back buffer {}x{}, depth {} ({})", options.width, options.height,
                 options.depthFormat != 0 ? rhi::d3dFormatName(options.depthFormat) : "none",
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
    bb.format = rhi::d3dfmt::X8R8G8B8;
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
    if (pres().valid() && (pres().info().extent.width != width || pres().info().extent.height != height)) {
        pres().waitIdle();
        rebuildSwapchain(width, height);
    }
    return true;
}

void Device::windowResized() {
    if (options_.offscreen) {
        return;
    }
    pres().waitIdle();
    if (pres().frameGenActive()) {
        // AMD's swapchain follows the window by its size only: a re-creation at the same size would only cost the generator's context
        RECT rc{};
        GetClientRect(options_.window, &rc);
        if (static_cast<std::uint32_t>(rc.right) == pres().info().extent.width && static_cast<std::uint32_t>(rc.bottom) == pres().info().extent.height) {
            return;
        }
    }
    recreateSwapchain();
}

void Device::setVsync(bool vsync) {
    if (vsync == options_.vsync) {
        return;
    }
    options_.vsync = vsync;
    pacerChecked_ = {};
    pacerRequested_ = -1.0;
    pres().waitIdle();
    pres().setVsync(vsync);
    if (pres().valid()) {
        rebuildSwapchain(pres().info().extent.width, pres().info().extent.height);
    }
}

// every re-creation of the swapchain: the generator's context (frame generation swapchain) goes first, it belongs to the old one
void Device::rebuildSwapchain(std::uint32_t width, std::uint32_t height) {
    if (pres().frameGenActive() && fgClient_ != nullptr) fgClient_->swapchainWillChange();
    const bool fgBefore = pres().frameGenActive();
    pres().recreate(width, height);
    if (fgBefore) {
        // a new AMD swapchain counts its presents from zero: the stats baselines restart (else the differences underflow)
        fgTick_ = {};
        fgReportAt_ = {};
        fgReportReal_ = 0;
    }
}

bool Device::setFrameGenSwapchain(bool want, const std::filesystem::path& loaderDll) {
    if (options_.offscreen || !pres().valid()) return false;
    if (want == pres().frameGenActive()) return true;
    pres().waitIdle();
    if (fgClient_ != nullptr && pres().frameGenActive()) fgClient_->swapchainWillChange();
    if (!want) {
        if (auto* fg = presentWrapper()) fg->setUi({});
        fgUiInstalled_ = false;
    }
    pres().setFrameGen(want, loaderDll);
    recreateSwapchain();
    if (want && pres().frameGenActive() && !fgUiInstalled_) {
        // the overlay in the swapchain's present callback (on AMD's threads): the UI built on the render thread at present
        rhi::PresentUi hook;
        hook.wants = [this] { return overlay_ != nullptr && !overlayIntoBackBuffer_ && overlay_->hasPrepared(); };
        hook.record = [this](rhi::CommandList& cmd, VkExtent2D extent) {
            if (overlay_ != nullptr) overlay_->recordPrepared(cmd, extent);
        };
        presentWrapper()->setUi(std::move(hook));
        fgUiInstalled_ = true;
        // the real presents' times (a thread spinning on the API's present counter: [debug] perf only; RVK_FG_NO_SAMPLER=1 leaves it
        // out, e.g. for CPU measurements)
        if (options_.perf && GetEnvironmentVariableA("RVK_FG_NO_SAMPLER", nullptr, 0) == 0) presentWrapper()->startSampler();
    }
    pacerChecked_ = {};
    pacerRequested_ = -1.0;
    fgTick_ = {};
    fgReportAt_ = {};  // a new swapchain counts its presents from zero
    fgReportReal_ = 0;
    return pres().frameGenActive() == want;
}

void Device::shutdownFrameGenSwapchain() {
    pres().setFrameGen(false);
    if (!pres().frameGenActive()) return;
    pres().waitIdle();
    if (fgClient_ != nullptr) fgClient_->swapchainWillChange();
    if (auto* fg = presentWrapper()) fg->setUi({});
    fgUiInstalled_ = false;
    pres().releaseFrameGen();
}

// AMD's swapchain as the RHI's PresentWrapper (null while it is not in use)
rhi::PresentWrapper* Device::presentWrapper() { return game_ ? pres().presentWrapper() : nullptr; }

void Device::setFrameGenRunning(bool on) {
    if (on == fgRunning_) return;
    fgRunning_ = on;
    pacerChecked_ = {};  // the base rate is half the output rate while frames are generated
}

// once a second: the live base / output rates for the overlay (frame generation only; zero otherwise)
void Device::fgStatsTick(std::chrono::steady_clock::time_point now) {
    rhi::PresentWrapper* fg = presentWrapper();
    if (fgTick_ == std::chrono::steady_clock::time_point{}) {
        fgTick_ = now;
        fgTickBase_ = stats_.presentsQueued;
        fgTickGen_ = stats_.generatedPresented;
        fgTickReal_ = fg != nullptr ? fg->realPresentCount() : 0;
        return;
    }
    const double dt = std::chrono::duration<double>(now - fgTick_).count();
    if (dt < 1.0) return;
    const std::uint64_t base = stats_.presentsQueued - fgTickBase_;
    const std::uint64_t gen = stats_.generatedPresented - fgTickGen_;
    if (!fgRunning_) {
        live_.baseFps = live_.outputFps = 0.0f;
    } else if (fg != nullptr) {
        const std::uint64_t real = fg->realPresentCount();
        live_.baseFps = static_cast<float>(static_cast<double>(base) / dt);
        live_.outputFps = static_cast<float>(static_cast<double>(real - fgTickReal_) / dt);
        fgTickReal_ = real;
    } else {
        live_.baseFps = static_cast<float>(static_cast<double>(base - gen) / dt);
        live_.outputFps = static_cast<float>(static_cast<double>(base) / dt);
    }
    if (fg != nullptr) fgTickReal_ = fg->realPresentCount();
    fgTick_ = now;
    fgTickBase_ = stats_.presentsQueued;
    fgTickGen_ = stats_.generatedPresented;
}

Device::FrameGenReport Device::takeFrameGenReport() {
    FrameGenReport r;
    rhi::PresentWrapper* fg = presentWrapper();
    const auto now = std::chrono::steady_clock::now();
    if (fgReportAt_ == std::chrono::steady_clock::time_point{}) fgReportAt_ = now;
    r.seconds = std::chrono::duration<double>(now - fgReportAt_).count();
    r.swapchain = fg != nullptr;
    r.basePresents = stats_.presentsQueued - fgReportBase_;
    fgReportBase_ = stats_.presentsQueued;
    fgReportAt_ = now;
    if (fg != nullptr) {
        const std::uint64_t real = fg->realPresentCount();
        r.realPresents = real - fgReportReal_;
        fgReportReal_ = real;
        const rhi::PresentWrapper::Counters c = fg->takeCounters();
        r.callbacks = c.callbacks;
        r.generated = c.generatedFrames;
        r.uiFrames = c.uiFrames;
        r.callbackMs = c.callbackMs;
        const std::vector<rhi::PresentSample> samples = fg->takeSamples();
        for (std::size_t i = 0; i < samples.size(); ++i) {
            r.times.push_back(samples[i].seconds);
            if (i > 0) r.gapsMs.push_back((samples[i].seconds - samples[i - 1].seconds) * 1000.0 / static_cast<double>(std::max<std::uint64_t>(1, samples[i].count - samples[i - 1].count)));
        }
    }
    return r;
}

void Device::flushFrameAndWait() {
    RVK_ZONE("Device::flushFrameAndWait");
    endRendering();
    submitFrame(false);
    game_->waitFrame(game_->frameValue() - 1);
    beginFrame();
}

bool Device::waitFrameGenPresent() { return pres().waitFrameGenPresent(); }

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
    if (options_.offscreen || !pres().valid()) return false;
    if (monitor != nullptr && !game_->gameCaps().fullScreenExclusive) {
        static std::atomic<bool> logged{false};
        if (!logged.exchange(true)) {
            spdlog::info("{}: exclusive fullscreen is not available{}: the game's fullscreen stays a borderless window",
                         game_->caps().backend == rhi::Backend::D3D12 ? "d3d12" : "vulkan", game_->caps().backend == rhi::Backend::D3D12 ? " on the D3D12 backend" : " (the device lacks VK_EXT_full_screen_exclusive)");
        }
    }
    if (!game_->gameCaps().fullScreenExclusive || pres().frameGenWanted()) monitor = nullptr;  // not with AMD's swapchain
    bool recreated = false;
    if (monitor != fseMonitor_) {
        recreated = true;
        fseMonitor_ = monitor;
        pres().waitIdle();
        pres().releaseFullScreenExclusive();
        pres().setFullScreenExclusive(monitor);
        recreateSwapchain();
        fseRetry_ = {};
        resetPacer();
    }
    if (hold != fseHold_) {
        fseHold_ = hold;
        fseRetry_ = {};
        if (!hold && pres().fullScreenExclusiveHeld()) {
            pres().waitIdle();
            pres().releaseFullScreenExclusive();
            spdlog::info("vulkan: exclusive fullscreen released");
            resetPacer();
        }
    }
    return recreated;
}

// render thread, the present thread idle: take the exclusive mode while wanted (retried once a second after a failure / loss)
void Device::serviceFullScreenExclusive() {
    if (pres().takeExclusiveLost()) {
        spdlog::info("vulkan: exclusive fullscreen lost (acquire): re-acquired later");
        fseRetry_ = std::chrono::steady_clock::now() + std::chrono::seconds(1);
        resetPacer();
    }
    if (!fseHold_ || !pres().fullScreenExclusiveConfigured() || pres().fullScreenExclusiveHeld()) return;
    const auto now = std::chrono::steady_clock::now();
    if (now < fseRetry_) return;
    std::string error;
    if (pres().acquireFullScreenExclusive(&error)) {
        const rhi::PresentInfo info = pres().info();
        spdlog::info("vulkan: exclusive fullscreen acquired (swapchain {}x{}, present mode {})", info.extent.width, info.extent.height, info.presentMode);
        resetPacer();
    } else {
        static int logged = 0;
        if (logged++ < 10) spdlog::warn("vulkan: {} (retried in 1 s)", error);
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
    if (overlayIntoBackBuffer_ || !pres().valid()) {
        return backBuffer_ && backBuffer_->valid() && !backBuffer_->isNull() ? backBuffer_->rhi().desc().format : VK_FORMAT_B8G8R8A8_UNORM;
    }
    return pres().info().format;
}

// after foreign commands (the overlay binds its own pipeline, descriptor set and dynamic state) in the frame's command buffer
void Device::invalidateRecordingState() {
    lastPipelineValid_ = false;
    vsConstDirty_ = psConstDirty_ = true;
    game_->invalidateRecordingState();  // the backend forgets the frame list's recorded pipeline / vertex input / dynamic state
}

void Device::drawOverlayIntoBackBuffer() {
    if (overlay_ == nullptr || !overlayIntoBackBuffer_ || !backBuffer_ || !overlay_->active()) {
        return;
    }
    endRendering();
    rhi::CommandList& cl = game_->frameCommands();
    rhi::Texture& bb = backBuffer_->rhi();
    cl.transition(bb, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    const VkExtent2D extent{backBuffer_->desc().width, backBuffer_->desc().height};
    rhi::RenderingInfo ri;
    ri.colorCount = 1;
    ri.color[0].texture = &bb;
    ri.color[0].load = VK_ATTACHMENT_LOAD_OP_LOAD;
    ri.color[0].store = VK_ATTACHMENT_STORE_OP_STORE;
    ri.width = extent.width;
    ri.height = extent.height;
    cl.beginRendering(ri);
    overlay_->record(cl, extent);
    cl.endRendering();
    invalidateRecordingState();
}

void Device::destroy() {
    if (!created_) {
        return;
    }
    stopVariantWorker();
    stopShaderWorkers();
    game_->waitIdle();
    // the pipeline cache and its threads are the backend's (the backend's GameRecord), destroyed with the device
    processReleases();
    backBuffer_.reset();
    depthBuffer_.reset();
    orphanLive();
    {
        const std::lock_guard lock(shaderMutex_);
        std::unordered_set<rhi::ShaderModule> destroyed;  // (S-4: variant modules are shared by text: each destroyed once)
        for (auto& [h, s] : shaders_) {
            if (s->module != rhi::ShaderModule::None) {
                game_->destroyShaderModule(s->module, false);
                s->module = rhi::ShaderModule::None;
            }
            for (rhi::ShaderModule* m : {&s->tintModule, &s->lodModule}) {
                if (*m != rhi::ShaderModule::None) {
                    if (destroyed.insert(*m).second) game_->destroyShaderModule(*m, false);
                    *m = rhi::ShaderModule::None;
                }
            }
            for (auto& m : s->variants) {
                if (const rhi::ShaderModule v = m.exchange(rhi::ShaderModule::None); v != rhi::ShaderModule::None && destroyed.insert(v).second) game_->destroyShaderModule(v, false);
            }
        }
        shaders_.clear();
        sharedModules_.clear();
    }
    vs_.reset();
    ps_.reset();
    samplers_.clear();
    if (keySaver_.joinable()) {
        keySaver_.join();
    }
    savePipelineKeys();
    if (keySaver_.joinable()) {
        keySaver_.join();
    }
    passes_ = inject::Registry{};
    markersOn_ = false;
    game_.reset();  // the backend: the present thread and the swapchain, the frame ring, the heaps, every API object
    created_ = false;
}

// ---- frames ---------------------------------------------------------------------------------------------------------

void Device::beginFrame() {
    RVK_ZONE("Device::beginFrame");
    game_->beginFrame();
    viArena_.clear();  // every chunk of the last submission has been recorded and executed
    dynArena_.clear();
    lastVi_ = nullptr;
    lastDyn_ = nullptr;
    sceneBeginFrame();
    sceneReport();
    substituteFrom_ = nullptr;
    substituteSlot_ = 0;
    perf_.slotWaitMs += game_->lastSlotWaitMs();
    frameSlotWaitMs_ = static_cast<float>(game_->lastSlotWaitMs());
    if (game_->timestampsEnabled()) {
        const std::uint32_t slot = game_->frameSlot();
        const rhi::TimestampResult r = game_->timestampCollect(slot);
        if (r.valid) {
            live_.gpuMs = static_cast<float>(r.mainMs + r.uploadMs);
            ++perf_.gpuSubmits;
            perf_.gpuMs += r.mainMs;
            perf_.gpuUploadMs += r.uploadMs;
            std::vector<perfwin::Segment> windowSegs;  // (a perf window's frame: aggregated by core/perf_window, not logged per frame)
            for (const auto& seg : r.segments) windowSegs.push_back({seg.label, seg.ms});
            if (!r.segments.empty() && !perfwin::gpuFrame(gpuDetailFrame_[slot], r.mainMs, r.uploadMs, windowSegs)) {
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
        const bool inWindow = perfwin::beginFrame(stats_.frames + 1);  // `perf window <frames> <tag>` (core/perf_window.h)
        const bool details = inWindow || (options_.perfFrame != 0 && stats_.frames + 1 >= options_.perfFrame && stats_.frames + 1 < options_.perfFrame + perfSpan);
        gpuDetailFrame_[slot] = details ? stats_.frames + 1 : 0;
        game_->timestampStart(slot, details);
        game_->timestampMark(frameList(), slot, rhi::TimestampStream::Main, "begin");
    }
    processReleases();
    rendering_ = false;
    lastPipelineValid_ = false;
    vsConstDirty_ = psConstDirty_ = true;
    dirtyConstWindows();  // the window blocks live in the frame's ring
    frameAddr_ = {};  // ring memory of the frame block belongs to a submission
    passIndex_ = 0;
    game_->invalidateRecordingState();  // the backend forgets the frame list's recorded state and rebinds the frame's bindless set
    regionsReopen();
}

// ---- debug regions ----------------------------------------------------------------------------------------------------------

namespace {

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
        game_->labelBegin(r.text);
        if (markersOn_ && !rendering_) {
            game_->crashMarker(true, r.text);
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
        game_->labelEnd();
        if (regions_[regionCount_].marked && markersOn_ && !rendering_) game_->crashMarker(false, regions_[regionCount_].text);
    }
}

void Device::setCrashMarkers(bool on) {
    if (on == markersOn_) return;
    if (!on) {
        markersOn_ = false;
        game_->setCrashMarkers(false);
        return;
    }
    if (!created_ || !game_->setCrashMarkers(true)) return;
    markersOn_ = true;
    if (!regionsOn_) spdlog::warn("vulkan: crash markers: they follow the capture regions (debug.regions is off)");
}

void Device::setRegionsEnabled(bool on) {
    on = on && created_ && game_->regionsAvailable();
    if (on == regionsOn_) return;
    phasesSeen_ = ~0ull;
    if (on) {
        regionsOn_ = true;
        regionsFrame_ = ~0ull;  // a fresh stack
        regionCount_ = 0;
        regionsReopen();
    } else {
        for (std::uint32_t i = 0; i < regionCount_; ++i) game_->labelEnd();
        regionCount_ = 0;
        regionsOn_ = false;
    }
}

// a submission is about to be made: its command buffer must not end with labels open
void Device::regionsClose() {
    if (!regionsOn_) return;
    for (std::uint32_t i = 0; i < regionCount_; ++i) game_->labelEnd();
}

// a new command buffer: the frame's region (a new frame: a new one) and whatever was open at the submit
void Device::regionsReopen() {
    if (!regionsOn_) return;
    phasesSeen_ = ~0ull;
    if (regionCount_ == 0 || regionsFrame_ != stats_.frames) {
        regionCount_ = 0;
        regionsFrame_ = stats_.frames;
        phaseKey_ = 0;
        regionBegin("Frame", static_cast<std::uint32_t>(stats_.frames + 1));
        return;
    }
    for (std::uint32_t i = 0; i < regionCount_; ++i) game_->labelBegin(regions_[i].text);
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
    if (game_->timestampsEnabled()) {
        game_->timestampMark(frameList(), game_->frameSlot(), rhi::TimestampStream::Main, what);
    }
}

PerfStats Device::takePerf() {
    perf_.pipelineBinds += game_->takePipelineBinds();
    PerfStats p = std::move(perf_);
    perf_ = PerfStats{};
    const rhi::PipelineStats s = game_->pipelineStats();
    p.pipelinesBuilt = s.built - perfPipelinesBase_;
    p.pipelineMs = s.buildMs - perfPipelineMsBase_;
    perfPipelinesBase_ = s.built;
    perfPipelineMsBase_ = s.buildMs;
    p.libraries = s.libraries - perfLibrariesBase_;
    p.libraryMs = s.libraryMs - perfLibraryMsBase_;
    p.optimized = s.optimized - perfOptimizedBase_;
    p.optimizeMs = s.optimizeMs - perfOptimizeMsBase_;
    perfLibrariesBase_ = s.libraries;
    perfLibraryMsBase_ = s.libraryMs;
    perfOptimizedBase_ = s.optimized;
    perfOptimizeMsBase_ = s.optimizeMs;
    p.prewarmed = s.prewarmed - perfPrewarmedBase_;
    p.prewarmMs = s.prewarmMs - perfPrewarmMsBase_;
    perfPrewarmedBase_ = s.prewarmed;
    perfPrewarmMsBase_ = s.prewarmMs;
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

void Device::submitFrame(bool presenting) {
    RVK_ZONE("Device::submitFrame");
    endRendering();
    regionsClose();  // a submission's command buffer must not end with labels open
    const auto t0 = std::chrono::steady_clock::now();
    game_->submitFrame(presenting ? "present blit + barriers since the last pass" : "copies + barriers since the last pass (flush)");
    perf_.submitMs += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    perf_.ringBytes += game_->ringBytes() - ringBytesBase_;
    ringBytesBase_ = game_->ringBytes();
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
            pacerMonitorRate_ = pacing::monitorRefreshRate(options_.window);
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
    // Frame generation: the rates above are the OUTPUT rate (the monitor's refresh, vulkan.max_fps); every rendered frame is shown
    // with a generated one, so the engine's base cadence is half of it (docs/research/frame-generation.md 4.1). The menu cap is
    // not halved: nothing is generated in menus.
    const bool halved = fgRunning_ && !menuCapped && rate > 0.0;
    if (halved) {
        rate *= 0.5;
    }
    // an unlocked vsync pacer retries the lock (DWM's measured period strays under load)
    pacing::VblankClock probe;
    const double vblanks = rate > 0.0 ? std::round(refresh / rate) : 0.0;
    const bool lockable = refresh > 0.0 && vblanks >= 1.0 && vblanks <= 8.0 && std::abs(refresh / vblanks - rate) < 0.01 * rate;
    const bool relock = rate == pacerRequested_ && lockable && !pacer_.vblankLocked() && pacing::dwmVblankClock(probe) &&
                        std::abs(probe.rate() - refresh) < 0.005 * refresh;
    if (rate != pacerRequested_ || relock) {
        pacerRequested_ = rate;
        // with vsync the slots sit on DWM's vblank grid (the refresh rate or an integer fraction of it)
        pacer_.setRate(rate, refresh);
        pacing::VblankClock clock;
        const bool dwm = pacing::dwmVblankClock(clock);
        spdlog::info("vulkan: DWM composition clock {}", dwm ? std::format("{:.3f} Hz", clock.rate()) : std::string("unavailable"));
        const double outRate = halved ? rate * 2.0 : rate;
        spdlog::info("vulkan: frame pacing at {:.3f} fps{}{}{}", pacer_.rate(),
                     menuCapped                                       ? " (menu)"
                     : options_.maxFps > 0.0 && outRate == options_.maxFps ? " (max_fps)"
                     : refreshCapped && outRate == refreshCap_           ? " (vsync, the game's refresh rate)"
                     : options_.vsync                                 ? " (vsync)"
                                                                      : "",
                     halved ? std::format(", frame generation: base = output {:.3f} fps / 2", outRate) : std::string(),
                     pacer_.vblankLocked() ? ", locked to DWM's vblank" : "");
    }
}

// diagnostics (RVK_NAN_BISECT=<first frame>): after every draw into the HDR scene colour of that frame (and the next ones) the target is read back and
// a draw that adds NaN components is logged with its shaders and textures
void diagAfterDraw(Device& d, const char* what) {
    static const long first = [] { char e[32]; return GetEnvironmentVariableA("RVK_NAN_BISECT", e, sizeof e) != 0 ? std::atol(e) : -1L; }();
    if (first < 0 || static_cast<long>(d.stats().frames) < first || static_cast<long>(d.stats().frames) > first + 1) return;
    Surface* rt = d.renderTarget(0);
    if (rt == nullptr || rt->texture() == nullptr || rt->texture()->desc().format != 113 || rt->texture()->desc().width < 800) return;
    Texture* t = rt->texture();
    static std::map<Texture*, std::uint32_t> lasts;
    static std::uint64_t lastFrame = ~0ull;
    if (lastFrame != d.stats().frames) { lastFrame = d.stats().frames; lasts.clear(); }
    std::uint32_t& last = lasts[t];
    const std::uint32_t w = t->desc().width, h = t->desc().height;
    std::vector<std::uint8_t> v;
    if (!d.readImageLevel(t->rhi(), 0, 0, {w, h, 1}, static_cast<std::uint64_t>(w) * h * 8, v)) return;
    std::uint32_t nan = 0;
    for (std::size_t k = 0; k + 8 <= v.size(); k += 8) {
        for (int c = 0; c < 4; ++c) {
            const std::uint16_t hf = *reinterpret_cast<const std::uint16_t*>(v.data() + k + c * 2);
            if (((hf >> 10) & 31) == 31 && (hf & 1023) != 0) ++nan;
        }
    }
    if (nan != last) {
        const Shader* vs = d.vertexShader();
        const Shader* ps = d.pixelShader();
        spdlog::info("diag: nan: frame {} target {} draw '{}' phase {} vs {:016x} ps {:016x}: NaN components {} -> {}", d.stats().frames, static_cast<const void*>(t), what,
                     static_cast<int>(d.phases().pass()), vs ? vs->hash : 0, ps ? ps->hash : 0, last, nan);
        last = nan;
    }
}

void Device::present() {
    RVK_ZONE("Device::present");
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
            RVK_FRAME();  // the frame mark after the present's own work (Tracy guide 3.3: right after the swap)
            const auto now = std::chrono::steady_clock::now();
            if (d.stats_.frames > 1) {
                d.perf_.presentMs += std::chrono::duration<double, std::milli>(now - d.presentStart_).count();
                const float interval = std::chrono::duration<float, std::milli>(d.presentStart_ - d.lastPresentStart_).count();
                d.live_.intervalMs = interval;
                if (d.options_.perf) {  // takePerf (the only consumer) runs only with [debug] perf
                    d.perf_.frameIntervalsMs.push_back(interval);
                    d.perf_.frameSplits.push_back({d.live_.cpuMs, std::chrono::duration<float, std::milli>(now - d.presentStart_).count(),
                                                   d.frameSlotWaitMs_});
                }
            }
            d.lastPresentStart_ = d.presentStart_;
            d.presentEnd_ = now;
        }
    } presentEnd{*this};
    if (static bool exposureLog = GetEnvironmentVariableA("RVK_EXPOSURE_LOG", nullptr, 0) != 0; exposureLog && stats_.frames % 60 == 59) {
        // diagnostics: the engine's adapted luminance (the 1x1 R32F output of the AvgLuminance filter) as the GPU holds it
        if (const void* api = engine::exposure::adaptedLuminanceApi()) {
            if (Texture* t = engineTexture(api)) {
                std::vector<std::uint8_t> v;
                if (readImageLevel(t->rhi(), 0, 0, {1, 1, 1}, 4, v) && v.size() >= 4) {
                    float f;
                    std::memcpy(&f, v.data(), 4);
                    spdlog::info("diag: exposure: frame {} adapted luminance {:.5f}", stats_.frames, f);
                }
            }
        }
        for (int i = 6; i >= 0; --i) {
            Texture* t = g_diagLum[i];
            if (t == nullptr) continue;
            const std::uint32_t w = 1u << i;
            std::vector<std::uint8_t> v;
            if (!readImageLevel(t->rhi(), 0, 0, {w, w, 1}, static_cast<std::uint64_t>(w) * w * 4, v)) continue;
            double sum = 0, mn = 1e30, mx = -1e30;
            std::uint32_t bad = 0;
            for (std::size_t k = 0; k + 4 <= v.size(); k += 4) {
                float f;
                std::memcpy(&f, v.data() + k, 4);
                if (!std::isfinite(f)) { ++bad; continue; }
                sum += f; mn = std::min<double>(mn, f); mx = std::max<double>(mx, f);
            }
            spdlog::info("diag: exposure: lum chain {}x{}: mean {:.5f} min {:.5f} max {:.5f} non-finite {}", w, w, sum / std::max<std::size_t>(1, v.size() / 4 - bad), mn, mx, bad);
        }
        if (Texture* t = g_diagScene; t != nullptr && t->desc().width * t->desc().height <= 4000000) {
            std::vector<std::uint8_t> v;
            const std::uint32_t w = t->desc().width, h = t->desc().height;
            if (readImageLevel(t->rhi(), 0, 0, {w, h, 1}, static_cast<std::uint64_t>(w) * h * 8, v)) {
                double sum = 0;
                std::uint32_t nan = 0, inf = 0;
                float mx = 0;
                for (std::size_t k = 0; k + 8 <= v.size(); k += 8) {
                    for (int c = 0; c < 3; ++c) {
                        const std::uint16_t hf = *reinterpret_cast<const std::uint16_t*>(v.data() + k + c * 2);
                        const std::uint32_t e = (hf >> 10) & 31, m = hf & 1023;
                        if (e == 31) { (m != 0 ? nan : inf)++; continue; }
                        const float f = (e == 0 ? std::ldexp(static_cast<float>(m), -24) : std::ldexp(static_cast<float>(m + 1024), static_cast<int>(e) - 25)) * ((hf & 0x8000) ? -1.0f : 1.0f);
                        sum += f; mx = std::max(mx, f);
                    }
                }
                spdlog::info("diag: exposure: scene {}x{}: mean rgb {:.5f} max {:.2f} nan {} inf {}", w, h, sum / (3.0 * w * h), mx, nan, inf);
            }
        }
    }
    endRendering();
    ++stats_.frames;
    if (wp0_.on && stats_.frames % 600 == 0) wp0Log();
    phases_.endFrame();
    firings_.endFrame();
    shadowDepthMaps_ = 0;
    draws_.endFrame();
    capturedPrev_.swap(captured_);
    captured_.clear();
    // the G-buffer's attachments are armed once a frame's solid pass found every pipeline / variant they need
    if (gbuf_.active && !gbufArmed_) {
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
    if (options_.offscreen || !pres().valid()) {
        submitFrame(false);
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
            submitFrame(false);
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
    rhi::Texture* generated = generated_;
    generated_ = nullptr;
    const bool twoFrames = generated != nullptr && !unpaced_;
    std::chrono::steady_clock::time_point generatedAt{};
    for (int stage = 0; stage < (twoFrames ? 2 : 1); ++stage) {
    const bool showGenerated = twoFrames && stage == 0;
    if (twoFrames && stage == 1) {
        const double left = 0.5 * baseIntervalMs_ - std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - generatedAt).count();
        if (left > 0.05) pacer_.sleepFor(left);
    }
    rhi::GamePresenter& pr = pres();
    const auto tq = std::chrono::steady_clock::now();
    // the swapchain is the present thread's until its present returned
    std::string presentError;
    const rhi::PresentStatus previous = pr.waitIdle(&presentError);
    stats_.presentMs += pr.takePresentMs();
    if (previous == rhi::PresentStatus::Suboptimal && pr.frameGenActive()) {
        ++stats_.presentSuboptimal;  // AMD's swapchain: a hint, not worth re-creating the whole frame generation setup
    } else if (previous == rhi::PresentStatus::OutOfDate || previous == rhi::PresentStatus::Suboptimal) {
        ++stats_.presentSuboptimal;
        ++stats_.swapchainRecreates;
        recreateSwapchain();
    } else if (previous == rhi::PresentStatus::ExclusiveLost) {
        // exclusive fullscreen taken away (another window, a mode change): presents go on non-exclusive, re-acquired later
        pr.noteExclusiveLost();
        spdlog::info("vulkan: exclusive fullscreen lost (present): re-acquired later");
        fseRetry_ = std::chrono::steady_clock::now() + std::chrono::seconds(1);
        resetPacer();
    } else if (previous != rhi::PresentStatus::Ok) {
        spdlog::error("vulkan: present: {}", presentError);
    }
    serviceFullScreenExclusive();
    const auto ta = std::chrono::steady_clock::now();
    stats_.presentWaitMs += std::chrono::duration<double, std::milli>(ta - tq).count();
    const bool acquiredOk = pr.acquire();
    stats_.acquireMs += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - ta).count();
    if (!acquiredOk) {
        // minimised or out of date: drop the frame's presentation, keep its work (exclusive fullscreen lost: no recreation,
        // serviceFullScreenExclusive re-acquires it)
        ++stats_.acquireFailures;
        submitFrame(false);
        if (!pr.exclusiveLost() && !pr.acquireNotReady()) recreateSwapchain();
        beginFrame();
        return;
    }
    const bool fgSc = pr.frameGenActive();  // AMD's swapchain: the replacement image, its callback draws the overlay
    std::optional<RegionScope> presentRegion;
    presentRegion.emplace(*this, showGenerated ? "Present (generated)" : "Present");
    // the frame shown: the engine's back buffer, or (first of the two) the generated frame
    rhi::Texture& shown = showGenerated ? *generated : backBuffer_->rhi();
    const VkExtent3D srcExtent = showGenerated ? generated->desc().extent
                                               : VkExtent3D{backBuffer_->desc().width, backBuffer_->desc().height, 1};
    pr.blitFrame(shown, srcExtent);
    // the overlay (src/overlay) on top of the copied frame: only in the swapchain image, never in the back buffer that captures read
    if (overlay_ != nullptr && !overlayIntoBackBuffer_ && overlay_->active()) {
        if (fgSc) {
            pr.drawOverlay(*overlay_);  // built here (the render thread), drawn by the swapchain's present callback on every frame it shows
        } else {
            RegionScope overlayRegion(*this, "Overlay");
            pr.drawOverlay(*overlay_);
        }
    }
    pr.finishFrame();
    presentRegion.reset();
    submitFrame(true);
    pr.queuePresent();
    ++stats_.presentsQueued;
    fgStatsTick(std::chrono::steady_clock::now());
    if (showGenerated) {
        ++stats_.generatedPresented;
        generatedAt = std::chrono::steady_clock::now();
    }
    // persist new pipelines every 10 s (the game is usually killed, not shut down)
    if (presentStart_ - pipelinesSaved_ > std::chrono::seconds(10)) {
        pipelinesSaved_ = presentStart_;
        game_->savePipelineCache();
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
        rebuildSwapchain(static_cast<std::uint32_t>(rc.right), static_cast<std::uint32_t>(rc.bottom));
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
std::string moduleLabel(bool names, shader::Stage stage, std::uint64_t hash, std::string_view tag) {
    if (!names) return {};
    return std::format("{} {:016x}{}{}", stage == shader::Stage::Vertex ? "vs" : "ps", hash, tag.empty() ? "" : " ", tag);
}
}  // namespace

std::shared_ptr<Shader> Device::createShader(std::span<const std::uint32_t> bytecode) {
    RVK_ZONE("Device::createShader");
    const std::size_t bytes = shader::byteLength(bytecode.data(), bytecode.size());
    if (bytes == 0) {
        spdlog::warn("d3d9: shader bytecode without END token");
        return nullptr;
    }
    const std::span<const std::uint32_t> tokens = bytecode.first(bytes / 4);
    const std::uint32_t hashSeed = kTranslatorVersion | (shader::debugInfo() ? 0x80000000u : 0u);  // debug-info SPIR-V is cached apart
    const std::uint64_t includesHash = shader::hlslIncludesHash();  // an edited ABI header recompiles the cached modules
    const std::uint64_t hash = fnv1a(tokens.data(), tokens.size_bytes(), fnv1a(&includesHash, sizeof(includesHash), fnv1a(&hashSeed, sizeof(hashSeed))));
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
    auto hlsl = shader::toHlsl(dec.program);
    if (!hlsl.ok()) {
        spdlog::warn("d3d9: shader {:016x}: {}", hash, hlsl.error);
        return nullptr;
    }
    // The shader object is complete at once (reflection, semantics, sampler mask: what the engine's effect code asks for right after the
    // call); its SPIR-V module comes from the disk cache or from DXC on the shader workers (a cold cache costs ~60 ms per shader, ~2800
    // shaders at the engine's start-up: sequentially that was 170 s). Whoever needs the module first (a draw, Shader::wait()) waits for it.
    auto s = std::make_shared<Shader>();
    s->stage = dec.program.version.stage;
    s->reflection = hlsl.reflection;
    s->bytecode.assign(tokens.begin(), tokens.end());
    s->hash = hash;
    s->semantics = shaderSemantics(tokens, s->reflection);
    for (std::uint32_t slot = 0; slot < kSamplerCount; ++slot) {
        if (s->reflection.samplers[slot] != shader::TextureType::Unknown) s->samplerMask |= 1u << slot;
    }
    {
        const std::lock_guard lock(shaderMutex_);
        auto [it, inserted] = shaders_.emplace(hash, s);
        if (!inserted) {  // another thread was faster
            return it->second;
        }
    }
    auto done = std::make_shared<std::promise<void>>();
    s->ready = done->get_future().share();
    s->compiling.store(true, std::memory_order_release);
    const auto cacheFile = options_.cacheDir.empty() ? std::filesystem::path()
                                                     : options_.cacheDir / "spirv" / std::format("{:016x}.spv", hash);
    enqueueShaderJob(s.get(), [this, s, done, text = std::move(hlsl.hlsl), cacheFile, hash] {
        rhi::ShaderModule module = rhi::ShaderModule::None;
        const bool dx12 = options_.backend == rhi::Backend::D3D12;
        if (dx12) {
            ++shadersCompiled_;
            module = dxilModule(s->stage, text, "translated.hlsl", cacheFile, moduleLabel(game_->namesEnabled(), s->stage, hash, {}), hash);
        }
        std::vector<std::uint32_t> spirv;
        if (!dx12)
        if (!cacheFile.empty()) {
            std::ifstream in(cacheFile, std::ios::binary | std::ios::ate);
            if (in) {
                const auto size = static_cast<std::size_t>(in.tellg());
                spirv.resize(size / 4);
                in.seekg(0);
                in.read(reinterpret_cast<char*>(spirv.data()), static_cast<std::streamsize>(spirv.size() * 4));
            }
        }
        if (!dx12 && spirv.empty()) {
            ++shadersCompiled_;
            auto compiled = shader::compileHlsl(s->stage, text);
            if (!compiled.ok()) {
                spdlog::error("d3d9: shader {:016x} does not compile:\n{}", hash, compiled.log);
                s->compiling.store(false, std::memory_order_release);
                done->set_value();  // the module stays null: draws with this shader are skipped
                return;
            }
            spirv = std::move(compiled.words);
            if (!cacheFile.empty()) {
                std::error_code ec;
                std::filesystem::create_directories(cacheFile.parent_path(), ec);
                const auto tmp = std::filesystem::path(cacheFile).concat(".tmp");
                {
                    std::ofstream out(tmp, std::ios::binary);
                    out.write(reinterpret_cast<const char*>(spirv.data()), static_cast<std::streamsize>(spirv.size() * 4));
                }
                std::filesystem::rename(tmp, cacheFile, ec);  // (a half-written file is never a hit)
            }
        }
        if (!dx12) module = game_->createShaderModule({.spirv = spirv}, moduleLabel(game_->namesEnabled(), s->stage, hash, {}).c_str());
        if (module != rhi::ShaderModule::None) {
            {
                const std::lock_guard lock(shaderMutex_);
                moduleHashes_.emplace(module, hash);
            }
            s->module.store(module, std::memory_order_release);
        }
        s->compiling.store(false, std::memory_order_release);
        done->set_value();
        if (module != rhi::ShaderModule::None) {
            prewarmPipelines(hash);
            // with pipeline libraries the shader's compilation happens here (a worker thread), not at its first draw
            game_->preparePipelineShader(module, s->stage == shader::Stage::Vertex ? VK_SHADER_STAGE_VERTEX_BIT : VK_SHADER_STAGE_FRAGMENT_BIT);
        }
    });
    return s;
}

// The DX12 backend's shader module of generated HLSL: DXIL (SM 6.6) through DXC, cached next to the SPIR-V cache (`.dxo`), never SPIR-V
rhi::ShaderModule Device::dxilModule(shader::Stage stage, const std::string& text, std::string_view name, const std::filesystem::path& spvCacheFile,
                                     const std::string& label, std::uint64_t hash, bool half16) {
    std::vector<std::uint8_t> dxil;
    std::filesystem::path cacheFile;
    if (char dump[MAX_PATH]; GetEnvironmentVariableA("RVK_DUMP_HLSL", dump, MAX_PATH) != 0) {  // diagnostics: the generated text of every module
        std::error_code ec;
        std::filesystem::create_directories(dump, ec);
        std::ofstream(std::filesystem::path(dump) / std::format("{:016x}_{}.hlsl", hash, stage == shader::Stage::Vertex ? "vs" : "ps")) << text;
    }
    if (!spvCacheFile.empty()) {
        cacheFile = spvCacheFile.parent_path().parent_path() / "dxil" / spvCacheFile.filename();
        // keyed by the text too: the cached DXIL follows the translator (the SPIR-V cache key's hash does not see every change of the generated HLSL)
        const std::uint64_t textHash = fnv1a(text.data(), text.size());
        cacheFile.replace_extension();
        cacheFile += std::format("_{:08x}.dxo", static_cast<std::uint32_t>(textHash ^ (textHash >> 32)));
        std::ifstream in(cacheFile, std::ios::binary | std::ios::ate);
        if (in) {
            dxil.resize(static_cast<std::size_t>(in.tellg()));
            in.seekg(0);
            in.read(reinterpret_cast<char*>(dxil.data()), static_cast<std::streamsize>(dxil.size()));
            // a cache file that a crash / power loss left zero-filled or cut short (NTFS commits the rename before the data: found 2026-11-08, 14 of 5,067 files all zero)
            // reaches CreateGraphicsPipelineState as a blob the debug layer rejects ("Encoded Vertex Shader size doesn't match"; E_FAIL, the draws are skipped = unlit
            // meshes): the DXBC container names its own size, which must be the file's
            std::uint32_t declared = 0;
            if (dxil.size() >= 32) std::memcpy(&declared, dxil.data() + 24, 4);
            if (dxil.size() < 32 || std::memcmp(dxil.data(), "DXBC", 4) != 0 || declared != dxil.size()) {
                spdlog::warn("d3d9: DXIL cache file {} is damaged ({} bytes, container size {}): recompiled", cacheFile.filename().string(), dxil.size(), declared);
                dxil.clear();
                in.close();
                std::error_code ec;
                std::filesystem::remove(cacheFile, ec);
            }
        }
    }
    if (dxil.empty()) {
        auto compiled = shader::compileHlslDxil(stage, text, name, half16);
        if (!compiled.ok()) {
            spdlog::error("d3d9: shader {:016x} does not compile to DXIL: {}", hash, compiled.log.substr(0, 1500));
            return rhi::ShaderModule::None;
        }
        dxil = std::move(compiled.bytes);
        if (!cacheFile.empty()) {
            std::error_code ec;
            std::filesystem::create_directories(cacheFile.parent_path(), ec);
            const auto tmp = std::filesystem::path(cacheFile).concat(".tmp");
            {
                std::ofstream out(tmp, std::ios::binary);
                out.write(reinterpret_cast<const char*>(dxil.data()), static_cast<std::streamsize>(dxil.size()));
            }
            std::filesystem::rename(tmp, cacheFile, ec);
        }
    }
    return game_->createShaderModule({.dxil = dxil}, label.empty() ? nullptr : label.c_str());
}

void Device::enqueueShaderJob(Shader* shader, std::function<void()> job) {
    {
        const std::lock_guard lock(shaderJobMutex_);
        shaderJobs_.push_back({shader, std::move(job)});
        if (shaderThreads_.empty()) {
            shaderStop_ = false;
            // all but two hardware threads (the engine thread and the render thread keep running), at least two
            const unsigned hw = std::thread::hardware_concurrency();
            const unsigned n = std::clamp(hw > 3 ? hw - 2 : 2u, 2u, 12u);
            for (unsigned i = 0; i < n; ++i) shaderThreads_.emplace_back([this] { shaderWorker(); });
        }
    }
    shaderJobCv_.notify_one();
}

void Device::shaderWorker() {
    SetThreadDescription(GetCurrentThread(), L"rvk shader compile");
    for (;;) {
        ShaderJob job;
        {
            std::unique_lock lock(shaderJobMutex_);
            shaderJobCv_.wait(lock, [this] { return shaderStop_ || !shaderJobs_.empty(); });
            if (shaderStop_) return;
            job = std::move(shaderJobs_.front());
            shaderJobs_.pop_front();
        }
        job.run();
    }
}

std::size_t Device::queuedShaderJobs() const {
    const std::lock_guard lock(shaderJobMutex_);
    return shaderJobs_.size();
}

void Device::prioritizeShader(Shader& s) {
    if (s.promoted.exchange(true, std::memory_order_acq_rel)) return;
    const std::lock_guard lock(shaderJobMutex_);
    for (auto it = shaderJobs_.begin(); it != shaderJobs_.end(); ++it) {
        if (it->shader == &s) {
            if (it != shaderJobs_.begin()) {
                ShaderJob j = std::move(*it);
                shaderJobs_.erase(it);
                shaderJobs_.push_front(std::move(j));
            }
            return;
        }
    }
}

// drops the queued shader compiles and stops the workers (a compile in progress finishes)
void Device::stopShaderWorkers() {
    {
        const std::lock_guard lock(shaderJobMutex_);
        shaderJobs_.clear();  // (their promises break: a waiting draw wakes up and sees no module)
        shaderStop_ = true;
    }
    shaderJobCv_.notify_all();
    for (std::thread& t : shaderThreads_) {
        if (t.joinable()) t.join();
    }
    shaderThreads_.clear();
}

// The tint debug view's variant of a pixel shader (TranslateOptions::tintEpilogue): translated and compiled on its first tinted
// draw (render thread; a short hitch when the view is switched on), cached on the shader and in the SPIR-V disk cache. The
// normal module is never touched, so with the view off every draw renders exactly as before.
rhi::ShaderModule Device::tintModule(Shader& ps) {
    if (ps.tintModule == rhi::ShaderModule::None && !ps.tintFailed) {
        ps.tintFailed = true;  // until it succeeds: one attempt per shader
        ps.tintModule = variantModule(ps, shader::TranslateOptions{.tintEpilogue = true}, std::format("tint{}", kTintVersion));
        ps.tintFailed = ps.tintModule == rhi::ShaderModule::None;
    }
    return ps.tintModule;
}

// The SpeedTree billboards' vertex shader with the baked fade-in distances (BLENDWEIGHT1.xy: start, range; engine/lod.h)
// scaled by FrameBlock params[kFrameParamLod].x: used for billboard draws while lod.trees is not 1, built as tintModule
rhi::ShaderModule Device::lodModule(Shader& vs) {
    if (vs.lodModule == rhi::ShaderModule::None && !vs.lodFailed) {
        vs.lodFailed = true;
        shader::TranslateOptions o;
        o.inputOverrides.push_back(
            {shader::Usage::BlendWeight, 1, std::format("float4($v.xy * rvkPtr<RvkFrameData, 16>(pc.frame).Get().params[{}].x, $v.zw)", shader::abi::kFrameParamLod)});
        vs.lodModule = variantModule(vs, o, std::format("lod{}_t{}", kLodVariantVersion, kTranslatorVersion));
        vs.lodFailed = vs.lodModule == rhi::ShaderModule::None;
        spdlog::info("d3d9: billboard fade variant of vertex shader {:016x}: {}", vs.hash, vs.lodFailed ? "FAILED" : "built");
    }
    return vs.lodModule;
}

void Device::setBillboardFadeScale(float scale) {
    if (scale != billboardFadeScale_) ++variantEpoch_;
    billboardFadeScale_ = scale;
    setFrameParam(shader::abi::kFrameParamLod, {scale, 0.0f, 0.0f, 0.0f});
}

// a variant of a translated shader (render thread): translated with `options`, compiled (or read from the SPIR-V disk cache
// under `<hash>_<tag>.spv`), null when it fails
namespace {

// VariantSlot::Base: the plain GLSL material shader of a request the port covers
std::atomic<std::uint32_t> g_uberSkip{0};  // debug.ubershader_skip

bool uberCovers(const MaterialSource& src) {
    return shader::uberSupportsFile(src.file, src.entry) &&
           (shader::uberClassMask(src.entry, src.macros) & g_uberSkip.load(std::memory_order_relaxed)) == 0;
}

bool uberBaseOptions(const Shader& s, shader::TranslateOptions& o) {
    const MaterialSource* src = s.source.load(std::memory_order_acquire);
    if (s.stage != shader::Stage::Pixel || src == nullptr || !uberCovers(*src)) return false;
    o.uber = true;
    return true;
}

// S-1b: the scene vertex shader of a request the port covers (debug.ubershader_skip bit 256 keeps the vertex shaders translated: the A/B)
bool uberVsCovers(const MaterialSource& src) {
    return shader::uberVsSupports(src.file, src.entry) && (g_uberSkip.load(std::memory_order_relaxed) & 256u) == 0;
}

// 14.15 a: RVK_UBER_BONE_TR=1 = the bone-motion vertex forms stay the translated program even with the ubershader on (the A/B of the port's bone form)
bool uberBoneTranslated() {
    static const bool v = [] {
        char e[8];
        return GetEnvironmentVariableA("RVK_UBER_BONE_TR", e, sizeof e) > 0 && e[0] == '1';
    }();
    return v;
}

bool uberVertexOptions(const Shader& s, shader::TranslateOptions& o) {
    const MaterialSource* src = s.source.load(std::memory_order_acquire);
    if (s.stage != shader::Stage::Vertex || src == nullptr || !uberVsCovers(*src)) return false;
    o.uber = true;
    return true;
}

}  // namespace

rhi::ShaderModule Device::uberAskedNow(const Shader& ps) const {
    if (!ubershader_.load(std::memory_order_relaxed) || ps.source.load(std::memory_order_acquire) == nullptr) return rhi::ShaderModule::None;
    constexpr std::size_t i = static_cast<std::size_t>(VariantSlot::Base);
    return ps.variantState[i].load(std::memory_order_acquire) == 2 ? ps.variants[i].load(std::memory_order_acquire) : rhi::ShaderModule::None;
}

Device::UberWhy Device::uberWhyOf(const Shader& ps, VariantSlot slot) const {
    if (!ubershader_.load(std::memory_order_relaxed)) return UberWhy::None;
    const MaterialSource* src = ps.source.load(std::memory_order_acquire);
    if (src == nullptr) return UberWhy::NoRequest;
    if ((ps.uberSlots.load(std::memory_order_acquire) >> static_cast<std::size_t>(slot) & 1u) != 0) return UberWhy::Uber;
    if (!shader::uberSupportsFile(src->file, src->entry)) return src->file == "ge_default_leafs.fx" ? UberWhy::Leaf : UberWhy::None;  // (S-1b: the leaf shaders are covered; Leaf = an unknown leaf entry)
    if (!uberCovers(*src)) return UberWhy::Skipped;
    return UberWhy::Failed;
}

// S-1b: the form a scene vertex shader of this slot took (0 translated / not covered, 1 ubershader, 2 failed)
std::uint8_t Device::uberVsFormOf(const Shader& vs, VariantSlot slot) const {
    if (!ubershader_.load(std::memory_order_relaxed)) return 0;
    if ((vs.uberSlots.load(std::memory_order_acquire) >> static_cast<std::size_t>(slot) & 1u) != 0) return 1;
    const MaterialSource* src = vs.source.load(std::memory_order_acquire);
    return src != nullptr && uberVsCovers(*src) ? 2 : 0;
}

void Device::setUbershaderSkip(std::uint32_t mask) { g_uberSkip.store(mask, std::memory_order_relaxed); }

void Device::setUbershader(bool on) {
    const bool changed = ubershader_.load(std::memory_order_relaxed) != on;
    ++variantEpoch_;
    ubershader_.store(on, std::memory_order_relaxed);
    if (on) setVariantProvider(VariantSlot::Base, {std::format("base_t{}", kTranslatorVersion), &uberBaseOptions});
    // (S-1a: a toggle at run time builds every variant again in the other form: the modules built so far are of the old one)
    if (changed) resetUberVariants();
    spdlog::info("d3d9: HLSL material shaders {}", on ? "on" : "off");
}

// S-1a: forget the variants of the material pixel shaders (render thread, between frames: the workers are drained first). The old modules stay alive (pipelines hold them);
// the next request builds the slot's module in the current form (the SPIR-V / DXIL disk caches key the two forms apart)
void Device::resetUberVariants() {
    {
        std::unique_lock lock(variantMutex_);
        for (VariantJob& j : variantJobs_) j.shader->variantState[static_cast<std::size_t>(j.slot)].store(0);
        variantJobs_.clear();
        variantIdleCv_.wait(lock, [this] { return variantBusy_ == 0; });
    }
    std::vector<std::shared_ptr<Shader>> all;
    {
        const std::lock_guard lock(shaderMutex_);
        all.reserve(shaders_.size());
        for (const auto& [h, sh] : shaders_) {
            if (sh->source.load(std::memory_order_acquire) != nullptr) all.push_back(sh);  // (S-1b: the vertex shaders' scene forms too)
        }
    }
    for (const auto& sh : all) {
        for (std::size_t i = 0; i < kVariantSlots; ++i) {
            sh->variants[i].store(rhi::ShaderModule::None, std::memory_order_release);
            sh->variantState[i].store(0, std::memory_order_release);
        }
        sh->uberSlots.store(0, std::memory_order_release);
    }
    spdlog::info("d3d9: HLSL material shaders: {} shaders' variants reset", all.size());
}

bool Device::uberHalf() const {
    static const bool allowed = [] {
        char v[8];
        return GetEnvironmentVariableA("RVK_UBER_HALF", v, sizeof v) == 0 || v[0] != '0';
    }();
    return allowed && game_->caps().shaderFloat16;
}

// A material pixel shader built from the GLSL material shader with its request's macros (+ the enhancement hooks of
// options.uberDefines); null when it does not build (the draw keeps the translated shader)
rhi::ShaderModule Device::uberModule(const Shader& s, const MaterialSource& src, const shader::TranslateOptions& options, std::string_view tag) {
    const bool vertex = s.stage == shader::Stage::Vertex;
    std::string glsl;
    if (vertex) {
        // S-1b: the inputs the bytecode declares (their registers = the locations of the pipeline's vertex input state)
        const auto dec = shader::decode(s.bytecode);
        if (!dec.ok()) return rhi::ShaderModule::None;
        std::vector<shader::VertexInput> inputs;
        for (const auto& d : dec.program.inputs) {
            if (d.reg.type == shader::RegType::Input) inputs.push_back({d.usage, d.usageIndex, d.reg.index});
        }
        // the clip position from the translated shader of the same options (bit-identical to the translated forms the same geometry's other pass may take:
        // the depth-equal test of the colour pass after the z pre-pass, shader-permutations.md 12)
        shader::TranslateOptions to = options;
        to.uber = false;
        to.uberDefines.clear();
        // 14.15 a: the port's bone-history form (RVK_UBER_BONE_PREV): the translated part keeps the bone form's defines (RVK_BONE_PREV: window 2 can be redirected) and its
        // rvk_prevWorld output, but instead of the second translated run it only hands the port the world point of its clip position (the port adds its own displacement)
        const bool bonePrev = options.uberDefines.find("RVK_UBER_BONE_PREV") != std::string::npos;
        if (bonePrev) {
            to.rerunnable = false;
            to.epilogue =
                "    {\n"
                "        // (14.15 a: the clip position's world point, as the translated bone form reconstructs its previous one)\n"
                "        const float4 rvk_twW = mul(rvk_pos, rvkLoadFrame(pc.frame).invViewProj);\n"
                "        rvk_out.rvk_prevWorld = abs(rvk_twW.w) > 1e-20 ? float4(rvk_twW.xyz / rvk_twW.w, 1.0) : float4(0.0, 0.0, 0.0, 0.0);\n"
                "    }";
        }
        const auto translated = shader::toHlsl(dec.program, to);
        if (!translated.ok()) return rhi::ShaderModule::None;
        static const bool ownPosition = [] {  // RVK_UBER_VS_OWNPOS=1: the port's own position (the A/B / negative control of the bridge)
            char v[8];
            return GetEnvironmentVariableA("RVK_UBER_VS_OWNPOS", v, sizeof v) > 0 && v[0] == '1';
        }();
        // (S-4: the pulled form needs the translated part: its entry builds the input struct; with the port's own position it stays translated)
        if (options.pullVertices && ownPosition) return rhi::ShaderModule::None;
        if (bonePrev && ownPosition) return rhi::ShaderModule::None;  // (the bone form needs the translated part's world point)
        glsl = shader::uberVertexHlsl(src.file, src.entry, src.macros, inputs, options.sceneRecord, options.sceneView, options.uberDefines,
                                      ownPosition ? std::string_view() : std::string_view(translated.hlsl), options.pullVertices);
    } else {
        glsl = shader::uberPixelHlsl(src.file, src.entry, src.macros, options.uberDefines, options.sceneRecord, options.sceneView);
    }
    if (glsl.empty()) return rhi::ShaderModule::None;
    // S-2: the lit core in real half precision (ge_default.frag.hlsl `rvkh`) when the device has 16-bit arithmetic; float otherwise (RVK_UBER_HALF=0 forces it)
    const bool half16 = !vertex && uberHalf();
    // the cache key: the embedded sources + the assembled text (the request's defines, the translated part of a vertex shader: a translator change moves it too)
    std::uint64_t textHash = shader::uberHlslHash();
    for (const char c : glsl) textHash = (textHash ^ static_cast<std::uint8_t>(c)) * 1099511628211ull;
    std::vector<std::uint32_t> spirv;
    const auto cacheFile =
        options_.cacheDir.empty()
            ? std::filesystem::path()
            : options_.cacheDir / "spirv" /
                  std::format("{:016x}_{}_u{:08x}{}.spv", s.hash, tag, static_cast<std::uint32_t>(textHash ^ (textHash >> 32)), half16 ? "h" : "");
    // S-4: one module per distinct text (two requests of different bytecode that assemble the same text share it, and with it their pipelines and owned buckets)
    const ModuleTextKey textKey = moduleTextKey(s.stage, glsl, half16);
    if (const rhi::ShaderModule shared = sharedModule(textKey); shared != rhi::ShaderModule::None) return shared;
    if (options_.backend == rhi::Backend::D3D12) {
        return addSharedModule(textKey, dxilModule(s.stage, glsl, vertex ? "uber_vs.hlsl" : "uber.hlsl", cacheFile, game_->namesEnabled() ? moduleLabel(true, s.stage, s.hash, std::format("{} {} {}", src.file, src.entry, tag)) : std::string(), s.hash, half16));
    }
    if (!cacheFile.empty()) {
        std::ifstream in(cacheFile, std::ios::binary | std::ios::ate);
        if (in) {
            spirv.resize(static_cast<std::size_t>(in.tellg()) / 4);
            in.seekg(0);
            in.read(reinterpret_cast<char*>(spirv.data()), static_cast<std::streamsize>(spirv.size() * 4));
        }
    }
    if (spirv.empty()) {
        auto compiled = shader::compileHlsl(s.stage, glsl, vertex ? "uber_vs.hlsl" : "uber.hlsl", half16);
        if (!compiled.ok()) {
            if (ubershadersFailed_.fetch_add(1) < 12) {
                spdlog::warn("d3d9: GLSL material shader of {:016x} ({} {} variant {}) does not compile: {}", s.hash, src.file, src.entry, tag,
                             compiled.log.substr(0, 600));
            }
            return rhi::ShaderModule::None;
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
    return addSharedModule(textKey, game_->createShaderModule(
        {.spirv = spirv}, game_->namesEnabled() ? moduleLabel(game_->namesEnabled(), s.stage, s.hash, std::format("{} {} {}", src.file, src.entry, tag)).c_str() : nullptr));
}

rhi::ShaderModule Device::variantModule(const Shader& s, const shader::TranslateOptions& options, std::string_view tag, bool* viaUber) {
    if (viaUber != nullptr) *viaUber = false;
    if (options.uber && ubershader_.load(std::memory_order_relaxed) && (s.stage == shader::Stage::Pixel || s.stage == shader::Stage::Vertex)) {
        const MaterialSource* src = s.source.load(std::memory_order_acquire);
        if (src != nullptr && (s.stage == shader::Stage::Pixel ? uberCovers(*src) : uberVsCovers(*src))) {
            const rhi::ShaderModule m = uberModule(s, *src, options, tag);
            if (m != rhi::ShaderModule::None) {
                if (viaUber != nullptr) *viaUber = true;
                return m;
            }
            // S-1a: a request the ubershader cannot build stays on the translated shader of the same form (counted)
            ++uberFallbacks_;
        }
    }
    const auto dec = shader::decode(s.bytecode);
    if (!dec.ok()) {
        return rhi::ShaderModule::None;
    }
    const auto glsl = shader::toHlsl(dec.program, options);
    if (!glsl.ok()) {
        spdlog::warn("d3d9: {} variant of shader {:016x}: {}", tag, s.hash, glsl.error);
        return rhi::ShaderModule::None;
    }
    std::vector<std::uint32_t> spirv;
    const auto cacheFile = options_.cacheDir.empty() ? std::filesystem::path()
                                                     : options_.cacheDir / "spirv" / std::format("{:016x}_{}_h{:08x}.spv", s.hash, tag, static_cast<std::uint32_t>(shader::hlslIncludesHash() ^ (shader::hlslIncludesHash() >> 32)));
    const ModuleTextKey textKey = moduleTextKey(dec.program.version.stage, glsl.hlsl, false);  // (S-4: one module per distinct text, as uberModule)
    if (const rhi::ShaderModule shared = sharedModule(textKey); shared != rhi::ShaderModule::None) return shared;
    if (options_.backend == rhi::Backend::D3D12) {
        return addSharedModule(textKey, dxilModule(dec.program.version.stage, glsl.hlsl, "translated.hlsl", cacheFile, moduleLabel(game_->namesEnabled(), s.stage, s.hash, tag), s.hash));
    }
    if (!cacheFile.empty()) {
        std::ifstream in(cacheFile, std::ios::binary | std::ios::ate);
        if (in) {
            spirv.resize(static_cast<std::size_t>(in.tellg()) / 4);
            in.seekg(0);
            in.read(reinterpret_cast<char*>(spirv.data()), static_cast<std::streamsize>(spirv.size() * 4));
        }
    }
    if (spirv.empty()) {
        auto compiled = shader::compileHlsl(dec.program.version.stage, glsl.hlsl);
        if (!compiled.ok()) {
            spdlog::error("d3d9: {} variant of shader {:016x} does not compile:\n{}", tag, s.hash, compiled.log);
            return rhi::ShaderModule::None;
        }
        spirv = std::move(compiled.words);
        if (!cacheFile.empty()) {
            std::error_code ec;
            std::filesystem::create_directories(cacheFile.parent_path(), ec);
            std::ofstream out(cacheFile, std::ios::binary);
            out.write(reinterpret_cast<const char*>(spirv.data()), static_cast<std::streamsize>(spirv.size() * 4));
        }
    }
    return addSharedModule(textKey, game_->createShaderModule({.spirv = spirv}, moduleLabel(game_->namesEnabled(), s.stage, s.hash, tag).c_str()));
}

// S-4 (frame-ownership.md 14.12): variant modules by their text. The text is the whole input of the compile (defines, includes by name: the embedded headers are one
// set per build; the half flag apart), so equal text = the same module: two shaders of different bytecode whose variants assemble the same text share one module, and with
// it their pipelines and owned buckets (e.g. the z pass vertex shaders that differ only in inputs the pulled form does not read through a vertex input). Keyed by two
// independent 64-bit hashes + the length + stage + half
Device::ModuleTextKey Device::moduleTextKey(shader::Stage stage, std::string_view text, bool half16) {
    ModuleTextKey k;
    k.a = fnv1a(text.data(), text.size());
    std::uint64_t b = 0x9ae16a3b2f90404full;
    for (const char c : text) b = (b ^ static_cast<std::uint8_t>(c)) * 0x100000001b3ull + 0x632be59bd9b4e019ull;
    k.b = b;
    k.meta = text.size() << 8 | (stage == shader::Stage::Vertex ? 1u : 0u) | (half16 ? 2u : 0u);
    return k;
}

rhi::ShaderModule Device::sharedModule(const ModuleTextKey& key) {
    if (!shareModules_.load(std::memory_order_relaxed)) return rhi::ShaderModule::None;
    const std::lock_guard lock(sharedModuleMutex_);
    const auto it = sharedModules_.find(key);
    if (it == sharedModules_.end()) return rhi::ShaderModule::None;
    ++sharedModuleHits_;
    return it->second;
}

rhi::ShaderModule Device::addSharedModule(const ModuleTextKey& key, rhi::ShaderModule m) {
    if (m == rhi::ShaderModule::None || !shareModules_.load(std::memory_order_relaxed)) return m;
    const std::lock_guard lock(sharedModuleMutex_);
    const auto [it, fresh] = sharedModules_.try_emplace(key, m);
    if (!fresh) {
        // (another worker built the same text meanwhile: keep the first, this one is dropped)
        game_->destroyShaderModule(m, true);
        ++sharedModuleHits_;
        return it->second;
    }
    ++sharedModuleMisses_;
    return m;
}

// ---- thin G-buffer (enh/gbuffer) ----

void Device::setGbuffer(const GbufferTargets& g) {
    // (F-VAR: the epoch moves when the G-buffer's state does, not at every call: the enhancement passes set it every frame, and a per-frame epoch made every owned record stale)
    if (g.active != gbuf_.active || g.normal != gbuf_.normal || g.motion != gbuf_.motion || g.width != gbuf_.width || g.height != gbuf_.height) ++variantEpoch_;
    const bool was = gbuf_.active;
    const std::uint32_t wasAttachments = gbuf_.attachments();
    const bool hadNormal = gbuf_.normal != nullptr;
    gbuf_ = g;
    if (!g.active) {
        if (was) spdlog::info("d3d9: g-buffer off");
        gbufArmed_ = false;
        return;
    }
    if (!was || wasAttachments != g.attachments() || hadNormal != (g.normal != nullptr)) {
        gbufRequestedAt_ = secondsSinceCreate();
        // without attachments the scope's pipelines are the plain ones (only the pixel shader variant differs, taken per draw once built)
        gbufArmed_ = g.attachments() == 0;
        spdlog::info("d3d9: g-buffer requested ({}x{}, targets: {}): {}", g.width, g.height,
                     g.normal != nullptr ? "normal + motion" : g.motion != nullptr ? "motion" : "none (AO / screen probes only)",
                     gbufArmed_ ? "armed" : "building the solid pass's pipelines with its attachments");
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

rhi::ShaderModule Device::variant(const ShaderRef& s, VariantSlot slot) {
    const std::size_t i = static_cast<std::size_t>(slot);
    const std::uint8_t state = s->variantState[i].load(std::memory_order_acquire);
    if (state == 2) return s->variants[i].load(std::memory_order_acquire);
    if (state != 0) return rhi::ShaderModule::None;
    s->variantState[i].store(1, std::memory_order_release);
    {
        const std::lock_guard lock(variantMutex_);
        const bool scene = isSceneSlot(slot);
        const VariantSlot baseSlot = scene ? sceneBaseSlot(slot) : slot;
        if (baseSlot != VariantSlot::Count && !variantProviders_[static_cast<std::size_t>(baseSlot)].options) {
            s->variantState[i].store(3);
            return rhi::ShaderModule::None;
        }
        std::shared_ptr<Shader> owner = shaderOwner(s.get());
        if (!owner) {
            s->variantState[i].store(3);
            return rhi::ShaderModule::None;
        }
        variantJobs_.push_back({std::move(owner), slot});
        if (variantThreads_.empty()) {
            variantStop_ = false;
            for (int t = 0; t < 3; ++t) variantThreads_.emplace_back([this] { variantWorker(); });
        }
    }
    variantCv_.notify_one();
    return rhi::ShaderModule::None;
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
            ++variantBusy_;
            provider = variantProviders_[static_cast<std::size_t>(job.slot)];
            if (isSceneSlot(job.slot)) {
                // a scene form (7.11): the base slot's provider + sceneRecord (SceneVs / ScenePs: no base options)
                const VariantSlot b = sceneBaseSlot(job.slot);
                VariantProvider base;
                if (b != VariantSlot::Count) base = variantProviders_[static_cast<std::size_t>(b)];
                // (scene ABI v2: the same forms with the view table, slot = v1 slot + 7: frame-ownership.md 7.19)
                const bool view = isSceneView(job.slot);
                // (S-4: SceneVsPull = the v2 vertex shader's pulled form, not a "+7" slot; 14.13: the bone-motion forms' v2 are "+3", SceneVsBonePull the pulled one)
                const bool pull = job.slot == VariantSlot::SceneVsPull || job.slot == VariantSlot::SceneVsBonePull;
                const VariantSlot v1 = sceneView1Of(job.slot);
                const bool lodVs = v1 == VariantSlot::SceneVsLod;
                const bool pixel = v1 != VariantSlot::SceneVs && v1 != VariantSlot::SceneVsBone && !lodVs;
                provider.tag = (b != VariantSlot::Count ? base.tag + "_scene" : pixel ? "scene_ps" : lodVs ? std::format("scene_lod{}", kLodVariantVersion) : "scene_vs") + (view ? "2" : "") +
                               (pull ? std::format("p{:08x}", shader::hlslPullHash()) : "");
                const bool uberOn = ubershader_.load(std::memory_order_relaxed);
                const bool boneVs = v1 == VariantSlot::SceneVsBone;
                provider.options = [base, hasBase = b != VariantSlot::Count, pixel, lodVs, view, uberOn, pull, boneVs](const Shader& sh, shader::TranslateOptions& o) {
                    if ((sh.stage == shader::Stage::Pixel) != pixel) return false;
                    if (hasBase && (!base.options || !base.options(sh, o))) return false;
                    // S-1a: the plain scene pixel shader of a request the ubershader covers is built from it (the enhancement variants say so through their own hooks)
                    if (pixel && !hasBase && uberOn) uberBaseOptions(sh, o);
                    // S-1b: the plain scene vertex shader of a request the vertex port covers (the fade variant stays translated)
                    if (!pixel && !hasBase && uberOn && uberVertexOptions(sh, o) && lodVs) o.uberDefines = std::format("#define RVK_LOD_FADE {}\n", shader::abi::kFrameParamLod);
                    // 14.15 a: the bone-motion vertex forms of a request the port covers are the port's bone form (RVK_UBER_BONE_PREV; RVK_UBER_BONE_TR=1 keeps them translated)
                    if (!pixel && hasBase && boneVs && uberOn && !uberBoneTranslated() && uberVertexOptions(sh, o)) {
                        o.uberDefines += "#define RVK_UBER_BONE_PREV 1\n";
                        if (!sh.reflection.relativeConstants) o.uberDefines += "#define RVK_UBER_BONE_OBJ 1\n";  // (14.15 b: a rigid draw's object record history)
                    }
                    if (lodVs) {  // (the translated fallback of an ubershader build that failed needs it too)
                        o.inputOverrides.push_back({shader::Usage::BlendWeight, 1,
                                                    std::format("float4($v.xy * rvkPtr<RvkFrameData, 16>(pc.frame).Get().params[{}].x, $v.zw)", shader::abi::kFrameParamLod)});
                    }
                    o.sceneRecord = true;
                    o.sceneView = view;
                    o.pullVertices = pull;
                    return true;
                };
            }
        }
        const std::size_t i = static_cast<std::size_t>(job.slot);
        const auto done = [&] {
            {
                const std::lock_guard lock(variantMutex_);
                --variantBusy_;
            }
            variantIdleCv_.notify_all();
        };
        shader::TranslateOptions o;
        if (!provider.options || !provider.options(*job.shader, o)) {
            job.shader->variantState[i].store(3, std::memory_order_release);
            done();
            continue;
        }
        bool viaUber = false;
        const rhi::ShaderModule m = variantModule(*job.shader, o, std::format("{}_t{}", provider.tag, kTranslatorVersion), &viaUber);
        if (m == rhi::ShaderModule::None) {
            ++variantsFailed_;
            spdlog::warn("d3d9: {} variant of shader {:016x} failed (the draws keep the engine's shader)", provider.tag, job.shader->hash);
            job.shader->variantState[i].store(3, std::memory_order_release);
            done();
            continue;
        }
        ++variantsBuilt_;
        if (viaUber) job.shader->uberSlots.fetch_or(1u << i, std::memory_order_release);
        job.shader->variants[i].store(m, std::memory_order_release);
        job.shader->variantState[i].store(2, std::memory_order_release);
        done();
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
        copyRegisters(vsF_[start + first], data + first * 4, last - first + 1);
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
        copyRegisters(psF_[start + first], data + first * 4, last - first + 1);
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
    if (index == 0 && surface != nullptr && surface->texture() != nullptr && surface->level() == 0) {
        static const bool diag = GetEnvironmentVariableA("RVK_EXPOSURE_LOG", nullptr, 0) != 0;
        if (diag) {
            Texture* t = surface->texture();
            const auto& d = t->desc();
            if (d.format == 114 && d.width == d.height && d.width <= 64 && (d.width & (d.width - 1)) == 0) {
                g_diagLum[std::countr_zero(d.width)] = t;
            } else if (d.format == 113 && d.width >= 800) {
                g_diagScene = t;
            }
        }
    }
    if (index == 0 && surface != nullptr && phases_.pass() == scene::Pass::ShadowMask && rhi::debugNamesEnabled()) {
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

// the convention of the buffer follows the view that clears it (reversed: 1 -> 0 = infinity, else 1 - k z)
float Device::clearDepthValue(float z) {
    ds_->texture()->reverseZ_ = reverseZ_ && phases_.current().view == scene::View::Main;
    if (ds_->texture()->reverseZ_) z = z >= 1.0f ? 0.0f : 1.0f - reverseDepthK() * std::max(z, 0.0f);
    return z;
}

void Device::clear(std::uint32_t flags, std::uint32_t color, float z, std::uint32_t stencil) {
    ++stats_.totalClears;
    if (rendering_ && (scopeGbuf_ != gbufferScope() || scopeSecondary_ != wantSecondaryScope())) endRendering();
    const ScopeClear request = [&] {
        ScopeClear r{flags, {}, z, stencil};
        colorToFloat4(color, r.color);
        return r;
    }();
    if (!beginRendering(&request)) {
        return;
    }
    flags &= ~scopeClearFlags_;  // (the scope's load ops did those: the whole area, as the first command of the scope)
    if ((flags & (CLEAR_TARGET | CLEAR_ZBUFFER | CLEAR_STENCIL)) == 0) return;
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
        z = clearDepthValue(z);
    }
    if ((flags & CLEAR_STENCIL) != 0 && stencilFormat_ != VK_FORMAT_UNDEFINED) ds |= VK_IMAGE_ASPECT_STENCIL_BIT;
    if (ds != 0) {
        atts[n].aspectMask = ds;
        atts[n].clearValue.depthStencil = {std::clamp(z, 0.0f, 1.0f), stencil & 0xFF};
        ++n;
    }
    if (n > 0) {
        flushBatch();  // the draws batched before the clear
        ClearOp op;
        std::copy(atts, atts + n, op.atts);
        op.count = n;
        op.rect = rect;
        if (scopeSecondary_) {
            emitClear(op);
        } else {
            game_->clearAttachments(op);
        }
    }
}

void Device::blit(rhi::Texture& src, std::uint32_t srcLevel, std::uint32_t srcLayer, const VkOffset3D srcBox[2], rhi::Texture& dst,
                  std::uint32_t dstLevel, std::uint32_t dstLayer, const VkOffset3D dstBox[2], VkFilter filter) {
    rhi::CommandList& cl = game_->frameCommands();
    const bool same = &src == &dst;
    if (same) {
        cl.transition(src, VK_IMAGE_LAYOUT_GENERAL);  // (always recorded: orders this blit after the previous mip's)
    } else {
        if (src.layout() != VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL) cl.transition(src, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
        if (dst.layout() != VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL) cl.transition(dst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    }
    const auto extent = [](const VkOffset3D box[2]) {
        return VkExtent3D{static_cast<std::uint32_t>(std::max(1, box[1].x - box[0].x)), static_cast<std::uint32_t>(std::max(1, box[1].y - box[0].y)),
                          static_cast<std::uint32_t>(std::max(1, box[1].z - box[0].z))};
    };
    rhi::TextureBlit region;
    region.src = {srcLevel, srcLayer};
    region.dst = {dstLevel, dstLayer};
    region.srcOffset = srcBox[0];
    region.dstOffset = dstBox[0];
    region.srcExtent = extent(srcBox);
    region.dstExtent = extent(dstBox);
    cl.blitTexture(dst, src, std::span<const rhi::TextureBlit>(&region, 1), rhi::isDepthFormat(src.desc().format) ? VK_FILTER_NEAREST : filter);
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
    blit(src->texture()->rhi(), src->level(), src->face(), sb, dst->texture()->rhi(), dst->level(), dst->face(), db,
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
    const rhi::FormatInfo& f = src->texture()->format();
    const std::uint64_t size = rhi::storageSize(f, w, h);
    if (!readImageLevel(src->texture()->rhi(), src->level(), src->face(), {w, h, 1}, size, pixels)) {
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
    const std::uint8_t* c = d3dFormat == rhi::d3dfmt::DXT1 ? b : b + 8;
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
    const bool fourColours = d3dFormat != rhi::d3dfmt::DXT1 || c0 > c1;
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
    if (d3dFormat == rhi::d3dfmt::DXT2 || d3dFormat == rhi::d3dfmt::DXT3) {
        for (int i = 0; i < 16; ++i) {
            const std::uint8_t n = (b[i / 2] >> ((i & 1) * 4)) & 15;
            alpha[i] = static_cast<std::uint8_t>(n * 17);
        }
    } else if (d3dFormat == rhi::d3dfmt::DXT4 || d3dFormat == rhi::d3dfmt::DXT5) {
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
        out[i][3] = d3dFormat == rhi::d3dfmt::DXT1 ? p[3] : alpha[i];
    }
}

bool isBlockDxt(std::uint32_t f) {
    return f == rhi::d3dfmt::DXT1 || f == rhi::d3dfmt::DXT2 || f == rhi::d3dfmt::DXT3 || f == rhi::d3dfmt::DXT4 || f == rhi::d3dfmt::DXT5;
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
    if (!game_->formatSupport(src->format().format, VK_IMAGE_USAGE_TRANSFER_SRC_BIT)) {
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
    src->prepareForSampling(game_->frameCommands());  // pending file data first
    Surface* srcSurface = src->surface(srcLevel, 0);
    if (dst->isRenderTarget()) {
        stretchRect(srcSurface, srcRect, dst->surface(dstLevel, 0), r, linear);
        return true;
    }
    if (dst->format().compressed || dst->format().convert != rhi::Convert::None) {
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
            if (!readImageLevel(src->rhi(), srcLevel, 0, {sLvlW, sLvlH, 1}, rhi::storageSize(src->format(), sLvlW, sLvlH), raw)) {
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
    const std::uint64_t size = rhi::storageSize(tmp->format(), w, h);
    const bool read = readImageLevel(tmp->rhi(), 0, 0, {w, h, 1}, size, pixels);
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

bool Device::readImageLevel(rhi::Texture& tex, std::uint32_t level, std::uint32_t layer, VkExtent3D extent, std::uint64_t size,
                            std::vector<std::uint8_t>& out) {
    if (size == 0) {
        return false;
    }
    const auto t0 = std::chrono::steady_clock::now();
    endRendering();
    // whole levels only: the buffer holds the level's rows `bufferRowPitch` apart (rhi::copyBytes), the result is repacked tightly below
    const rhi::TextureDesc& td = tex.desc();
    const VkExtent3D full = rhi::mipExtent(td, level);
    const rhi::FormatBlock fb = rhi::formatBlock(td.format);
    if (fb.bytes == 0 || extent.width != full.width || extent.height != full.height || extent.depth != full.depth) {
        spdlog::warn("d3d9: image read-back of {}x{}x{} (level {} is {}x{}x{}, format {}): not a whole level of a known format", extent.width, extent.height,
                     extent.depth, level, full.width, full.height, full.depth, static_cast<int>(td.format));
        return false;
    }
    rhi::BufferDesc bd;
    bd.size = rhi::copyBytes(td, level);
    bd.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    bd.memory = rhi::Memory::Readback;
    bd.name = "rvk image read-back";
    const std::unique_ptr<rhi::Buffer> rb = game_->createBuffer(bd);
    if (!rb) {
        return false;
    }
    rhi::CommandList& cl = game_->frameCommands();
    if (tex.layout() != VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL) cl.transition(tex, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    cl.copyTextureToBuffer(*rb, 0, tex, {level, layer});
    // the copy must finish before the CPU reads: end the frame's work here and start a new command buffer
    flushFrameAndWait();
    rb->invalidate();
    const auto* p = static_cast<const std::uint8_t*>(rb->map());
    const std::uint32_t rowBytes = ((full.width + fb.width - 1) / fb.width) * fb.bytes;
    const std::uint32_t blockRows = (full.height + fb.height - 1) / fb.height;
    const std::uint32_t pitch = rhi::bufferRowPitch(td.format, full.width);
    out.resize(static_cast<std::size_t>(rowBytes) * blockRows * full.depth);
    for (std::uint32_t z = 0; z < full.depth; ++z) {
        for (std::uint32_t r = 0; r < blockRows; ++r) {
            std::memcpy(out.data() + (static_cast<std::size_t>(z) * blockRows + r) * rowBytes,
                        p + (static_cast<std::size_t>(z) * blockRows + r) * pitch, rowBytes);
        }
    }
    if (out.size() > size) out.resize(static_cast<std::size_t>(size));
    ++perf_.readbacks;
    perf_.readbackMs += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    return true;
}

bool Device::readBackBuffer(std::vector<std::uint8_t>& bgra, std::uint32_t& width, std::uint32_t& height) {
    TextureDesc d;
    d.width = backBuffer_->desc().width;
    d.height = backBuffer_->desc().height;
    d.format = rhi::d3dfmt::A8R8G8B8;
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
    std::ifstream in(options_.cacheDir / (options_.backend == rhi::Backend::Vulkan ? "pipelines.keys" : "pipelines_d3d12.keys"), std::ios::binary);
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
    keySaver_ = std::thread([file = options_.cacheDir / (options_.backend == rhi::Backend::Vulkan ? "pipelines.keys" : "pipelines_d3d12.keys"), blob = std::move(blob)] {
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
void Device::recordPipelineKey(const PipelineDesc& pd) {
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
    k.desc.vs = rhi::ShaderModule::None;
    k.desc.fs = rhi::ShaderModule::None;
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
            if (k.desc.vs == rhi::ShaderModule::None || k.desc.fs == rhi::ShaderModule::None) continue;  // not compiled yet: its own completion prewarms
        }
        game_->prewarmPipeline(k.desc);
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
    PipelineDesc pd{};
    if (!replacementDesc(vs, ps, procedural, pd)) return false;
    if (!game_->pipelineContains(pd)) game_->prewarmPipeline(pd);
    return true;
}

bool Device::replacementReady(const Shader& vs, const Shader& ps, bool procedural) const {
    PipelineDesc pd{};
    return replacementDesc(vs, ps, procedural, pd) && game_->pipelineContains(pd);
}

bool Device::replacementDesc(const Shader& vs, const Shader& ps, bool procedural, PipelineDesc& out) const {
    if (!waterTemplateValid_ || vs.module == rhi::ShaderModule::None || ps.module == rhi::ShaderModule::None) return false;
    PipelineDesc pd = waterTemplate_;
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
        if (!game_->gameCaps().extendedDynamicState3) {
            pd.blend[i] = BlendAttachment{};
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
    rhi::GameSamplerDesc sd;
    sd.magFilter = filter(s[SAMP_MAGFILTER]);
    sd.minFilter = filter(s[SAMP_MINFILTER]);
    sd.mipmap = mipmapMode(s[SAMP_MIPFILTER]);
    sd.addressU = addressMode(s[SAMP_ADDRESSU]);
    sd.addressV = addressMode(s[SAMP_ADDRESSV]);
    sd.addressW = addressMode(s[SAMP_ADDRESSW]);
    sd.lodBias = std::clamp(asFloat(s[SAMP_MIPMAPLODBIAS]), -15.0f, 15.0f);
    const rhi::GameCaps& caps = game_->gameCaps();
    const bool aniso = (s[SAMP_MINFILTER] == TEXF_ANISOTROPIC || s[SAMP_MAGFILTER] == TEXF_ANISOTROPIC) && caps.samplerAnisotropy;
    sd.maxAnisotropy = aniso ? std::clamp(static_cast<float>(s[SAMP_MAXANISOTROPY]), 1.0f, caps.maxAnisotropy) : 1.0f;
    // MAXMIPLEVEL is the most detailed level used; MIPFILTER NONE samples that level only
    sd.minLod = static_cast<float>(s[SAMP_MAXMIPLEVEL]);
    sd.singleLevel = s[SAMP_MIPFILTER] == TEXF_NONE;
    const std::uint32_t border = s[SAMP_BORDERCOLOR];
    const bool white = (border & 0x00FFFFFF) == 0x00FFFFFF;
    const bool opaque = (border >> 24) >= 0x80;
    sd.border = white ? VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE : (opaque ? VK_BORDER_COLOR_FLOAT_OPAQUE_BLACK : VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK);
    std::string name;
    if (game_->namesEnabled()) {
        static constexpr const char* kFilter[] = {"none", "point", "linear", "aniso", "pyramid", "gauss"};
        static constexpr const char* kAddress[] = {"?", "wrap", "mirror", "clamp", "border", "mirror-once"};
        const auto f = [](const char* const* t, std::uint32_t v, std::uint32_t n) { return v < n ? t[v] : "?"; };
        name = std::format("min {} mag {} mip {} | U {} V {} W {}{}", f(kFilter, s[SAMP_MINFILTER], 6), f(kFilter, s[SAMP_MAGFILTER], 6),
                           f(kFilter, s[SAMP_MIPFILTER], 6), f(kAddress, s[SAMP_ADDRESSU], 6), f(kAddress, s[SAMP_ADDRESSV], 6),
                           f(kAddress, s[SAMP_ADDRESSW], 6), aniso ? std::format(" | aniso {}x", sd.maxAnisotropy) : std::string());
    }
    const std::uint32_t index = game_->createGameSampler(sd, name);
    if (index == 0) {
        return 0;
    }
    samplers_.emplace(key, index);
    return index;
}

void Device::logTracedMaterial(draws::Kind kind, bool jittered) {
    static const std::string filter = [] {
        char e[128];  // the Win32 environment (the proxy sets it from DllMain; the CRT's copy is stale)
        const DWORD n = GetEnvironmentVariableA("RVK_TRACE_MATERIAL", e, sizeof e);
        return n > 0 && n < sizeof e ? std::string(e, n) : std::string();
    }();
    if (filter.empty()) return;
    // a matching material, or any draw of an entity a matching material was drawn for (its other passes: z pre-pass, shadows)
    const bool named = tag_.materialName != nullptr && std::strstr(tag_.materialName, filter.c_str()) != nullptr;
    if (named && tag_.entity != nullptr && tracedEntities_.size() < 64) tracedEntities_.insert(tag_.entity);
    if (!named && (tag_.entity == nullptr || !tracedEntities_.contains(tag_.entity))) return;
    const scene::Phase ph = phases_.current();
    const std::uint64_t key = (reinterpret_cast<std::uintptr_t>(tag_.material) * 31u) ^ (static_cast<std::uint64_t>(ph.view) << 56) ^
                              (static_cast<std::uint64_t>(ph.pass) << 48) ^ (ps_ ? ps_->hash : 0u) ^ ((vs_ ? vs_->hash : 0u) << 1);
    if (tracedMaterialDraws_.size() >= 256 || !tracedMaterialDraws_.insert(key).second) return;
    std::string tex;
    for (std::uint32_t slot = 0; slot < 16; ++slot) {
        if (!ps_ || ps_->reflection.samplers[slot] == shader::TextureType::Unknown) continue;
        const Texture* t = tex_[slot];
        tex += t != nullptr ? std::format(" s{}={}x{} {}", slot, t->desc().width, t->desc().height, rhi::d3dFormatName(t->desc().format))
                            : std::format(" s{}=-", slot);
    }
    spdlog::info("d3d9: material trace '{}' (entity {}) {}/{} kind {}: vs {:016x} ps {:016x} (texkill {}), blend {} {}/{} op {}, "
                 "z {} write {} func {}, alpha test {} func {} ref {}, colour write 0x{:x}, cull {}, stencil {}, jittered {}, "
                 "ps c5 ({:.3g} {:.3g} {:.3g} {:.3g}), samplers [{} ]",
                 tag_.materialName != nullptr ? tag_.materialName : "(entity match)", tag_.entity, scene::name(ph.view), scene::name(ph.pass), draws::name(kind), vs_ ? vs_->hash : 0u,
                 ps_ ? ps_->hash : 0u, ps_ && ps_->reflection.discards, rs_[RS_ALPHABLENDENABLE], rs_[RS_SRCBLEND], rs_[RS_DESTBLEND],
                 rs_[RS_BLENDOP], rs_[RS_ZENABLE], rs_[RS_ZWRITEENABLE], rs_[RS_ZFUNC], rs_[RS_ALPHATESTENABLE], rs_[RS_ALPHAFUNC],
                 rs_[RS_ALPHAREF], rs_[RS_COLORWRITEENABLE], rs_[RS_CULLMODE], rs_[RS_STENCILENABLE], jittered, psF_[5][0], psF_[5][1],
                 psF_[5][2], psF_[5][3], tex);
}

}  // namespace rvk::d3d9
