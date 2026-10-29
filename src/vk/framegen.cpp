#include "vk/framegen.h"

#include <algorithm>
#include <chrono>
#include <format>
#include <tuple>

#include <spdlog/spdlog.h>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <ffx_api.h>
#include <ffx_api_loader.h>
#include <ffx_framegeneration.h>
#include <vk/ffx_api_framegeneration_vk.h>
#include <vk/ffx_api_vk.h>

#include "vk/context.h"

namespace rvk::vk {

struct FrameGen::Functions {
    ffxFunctions api{};
};

FrameGenVersion parseFrameGenVersion(std::uint64_t id, std::string name) {
    FrameGenVersion v;
    v.id = id;
    v.name = std::move(name);
    v.fromDriver = (v.id & 0x80000000ull) != 0 || (v.name.size() >= 2 && v.name.ends_with(" *"));
    const char* p = v.name.c_str();
    while (*p != 0 && (*p < '0' || *p > '9')) ++p;
    int* parts[3] = {&v.major, &v.minor, &v.patch};
    for (int* part : parts) {
        if (*p < '0' || *p > '9') break;
        while (*p >= '0' && *p <= '9') *part = *part * 10 + (*p++ - '0');
        if (*p == '.') ++p;
    }
    return v;
}

const FrameGenVersion* pickFrameGenProvider(const std::vector<FrameGenVersion>& list, bool ml) {
    const FrameGenVersion* best = nullptr;
    for (const FrameGenVersion& v : list) {
        if (ml ? v.major < 4 : v.major != 3) continue;
        if (best == nullptr || std::tie(v.major, v.minor, v.patch) > std::tie(best->major, best->minor, best->patch)) best = &v;
    }
    return best;
}

const FrameGenVersion* pickSwapchainProvider(const std::vector<FrameGenVersion>& list) {
    const FrameGenVersion* best = nullptr;
    for (const FrameGenVersion& v : list) {
        if (v.fromDriver || v.major == 0) continue;
        if (best == nullptr || std::tie(v.major, v.minor, v.patch) > std::tie(best->major, best->minor, best->patch)) best = &v;
    }
    return best;
}

std::string describeVersions(const std::vector<FrameGenVersion>& list) {
    std::string s;
    for (const FrameGenVersion& v : list) s += std::format("{}0x{:x}: {}", s.empty() ? "" : ", ", v.id, v.name);
    return s;
}

namespace {

std::string narrow(const wchar_t* w) {
    if (w == nullptr) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
    if (n <= 1) return {};
    std::string s(static_cast<std::size_t>(n - 1), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w, -1, s.data(), n, nullptr, nullptr);
    return s;
}

void ffxMessage(std::uint32_t type, const wchar_t* message) {
    if (type == FFX_API_MESSAGE_TYPE_ERROR) {
        spdlog::error("framegen: {}", narrow(message));
    } else {
        spdlog::warn("framegen: {}", narrow(message));
    }
}

VkDeviceContext deviceContextOf(Context& ctx) {
    VkDeviceContext dc{};
    dc.vkDevice = ctx.device();
    dc.vkPhysicalDevice = ctx.physicalDevice();
    dc.vkDeviceProcAddr = vkGetDeviceProcAddr;
    return dc;
}

FfxApiResourceDescription describe(const Image& img) {
    const ImageDesc& d = img.desc();
    FfxApiResourceDescription r{};
    r.type = FFX_API_RESOURCE_TYPE_TEXTURE2D;
    r.width = d.extent.width;
    r.height = d.extent.height;
    r.depth = d.layers;
    r.mipCount = d.mips;
    r.format = ffxApiGetSurfaceFormatVK(d.format);
    r.flags = FFX_API_RESOURCE_FLAGS_NONE;
    r.usage = FFX_API_RESOURCE_USAGE_READ_ONLY;
    if ((d.usage & VK_IMAGE_USAGE_STORAGE_BIT) != 0) r.usage |= FFX_API_RESOURCE_USAGE_UAV;
    return r;
}

FfxApiResource resource(const Image* img, std::uint32_t state) {
    if (img == nullptr) return {};
    return ffxApiGetResourceVK(reinterpret_cast<void*>(img->image()), describe(*img), state);
}

constexpr std::uint32_t kRead = FFX_API_RESOURCE_STATE_COMPUTE_READ;
constexpr std::uint32_t kWrite = FFX_API_RESOURCE_STATE_UNORDERED_ACCESS;

VkImageLayout layoutOf(std::uint32_t state) {
    return state == kWrite ? VK_IMAGE_LAYOUT_GENERAL : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
}

std::vector<FrameGenVersion> queryVersions(const ffxFunctions& api, Context& ctx, std::uint64_t createDescType) {
    std::vector<FrameGenVersion> out;
    VkDeviceContext dc = deviceContextOf(ctx);
    std::uint64_t count = 0;
    ffxQueryDescGetVersions q{};
    q.header.type = FFX_API_QUERY_DESC_TYPE_GET_VERSIONS;
    q.createDescType = createDescType;
    q.device = &dc;
    q.outputCount = &count;
    if (api.Query(nullptr, &q.header) != FFX_API_RETURN_OK || count == 0) return out;
    std::vector<std::uint64_t> ids(count);
    std::vector<const char*> names(count);
    q.outputCount = &count;
    q.versionIds = ids.data();
    q.versionNames = names.data();
    if (api.Query(nullptr, &q.header) != FFX_API_RETURN_OK) return out;
    for (std::uint64_t i = 0; i < count && i < ids.size(); ++i) {
        // copied now: the API may overwrite the strings by a later query
        out.push_back(parseFrameGenVersion(ids[i], names[i] != nullptr ? names[i] : ""));
    }
    return out;
}

}  // namespace

FrameGenReport probeFrameGeneration(Context& ctx, const std::filesystem::path& loaderDll) {
    FrameGenReport r;
    std::error_code ec;
    const std::filesystem::path effect = loaderDll.parent_path() / "amd_fidelityfx_framegeneration_vk.dll";
    r.dllPresent = std::filesystem::exists(effect, ec);
    if (!r.dllPresent) {
        r.reason = "amd_fidelityfx_framegeneration_vk.dll is not installed";
        return r;
    }
    if (!std::filesystem::exists(loaderDll, ec)) {
        r.reason = "amd_fidelityfx_loader_vk.dll is missing";
        return r;
    }
    const auto t0 = std::chrono::steady_clock::now();
    FrameGen fg;
    if (!fg.load(loaderDll)) {
        r.reason = "the FidelityFX loader does not load";
        return r;
    }
    const std::vector<FrameGenVersion> list = fg.versionList(ctx);
    const std::vector<FrameGenVersion> sc = fg.swapchainVersionList(ctx);
    r.probed = true;
    r.versions = describeVersions(list);
    const FrameGenVersion* pinned = pickSwapchainProvider(sc);
    r.swapchain = describeVersions(sc) + (pinned != nullptr ? std::format(" (pinned: {})", pinned->name) : std::string(" (none usable)"));
    r.fsr3 = pickFrameGenProvider(list, false) != nullptr;
    r.ml = pickFrameGenProvider(list, true) != nullptr;
    if (!r.fsr3) r.reason = list.empty() ? "the API offers no frame generation provider on this device" : "the API offers no FSR 3 frame generation provider";
    spdlog::info("framegen: probe: providers [{}], swapchain providers [{}] ({:.0f} ms)", r.versions, r.swapchain,
                 std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count());
    return r;
}

bool FrameGen::load(const std::filesystem::path& dll) {
    if (module_ != nullptr) return true;
    HMODULE m = LoadLibraryExW(dll.c_str(), nullptr, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
    if (m == nullptr) {
        spdlog::warn("framegen: cannot load {} (error {})", dll.string(), GetLastError());
        return false;
    }
    auto* f = new Functions;
    ffxLoadFunctions(&f->api, m);
    if (f->api.CreateContext == nullptr || f->api.DestroyContext == nullptr || f->api.Configure == nullptr ||
        f->api.Query == nullptr || f->api.Dispatch == nullptr) {
        spdlog::warn("framegen: {} is not an FFX API DLL (entry points missing)", dll.string());
        delete f;
        FreeLibrary(m);
        return false;
    }
    // the runtime's messages of this effect (the upscaler's go through its create description)
    ffxConfigureDescGlobalDebug dbg{};
    dbg.header.type = FFX_API_CONFIGURE_DESC_TYPE_GLOBALDEBUG;
    dbg.effectId = FFX_API_EFFECT_ID_FRAMEGENERATION;
    dbg.fpMessage = ffxMessage;
    dbg.debugLevel = FFX_API_CONFIGURE_GLOBALDEBUG_LEVEL_WARNINGS;
    f->api.Configure(nullptr, &dbg.header);
    module_ = m;
    fn_ = f;
    spdlog::info("framegen: loaded {}", dll.string());
    return true;
}

std::vector<FrameGenVersion> FrameGen::versionList(Context& ctx) {
    if (module_ == nullptr) return {};
    return queryVersions(fn_->api, ctx, FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATION);
}

std::vector<FrameGenVersion> FrameGen::swapchainVersionList(Context& ctx) {
    if (module_ == nullptr) return {};
    return queryVersions(fn_->api, ctx, FFX_API_CREATE_CONTEXT_DESC_TYPE_FGSWAPCHAIN_VK);
}

bool FrameGen::createContext(Context& ctx, const FrameGenCreateInfo& info) {
    if (module_ == nullptr) return false;
    destroyContext();
    const std::vector<FrameGenVersion> list = versionList(ctx);
    const FrameGenVersion* v = pickFrameGenProvider(list, info.ml);
    if (v == nullptr) {
        spdlog::error("framegen: the API offers no {} frame generation on this device ({} version(s): {})", info.ml ? "ML (FSR 4)" : "FSR 3",
                      list.size(), describeVersions(list));
        return false;
    }

    ffxCreateBackendVKDesc backend{};
    backend.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_BACKEND_VK;
    backend.deviceContext = deviceContextOf(ctx);

    // SDK 2.1+: the version of the headers this was built against (the API refuses the newer calls without it)
    ffxCreateContextDescFrameGenerationVersion apiVersion{};
    apiVersion.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATION_VERSION;
    apiVersion.version = FFX_FRAMEGENERATION_VERSION;

    ffxCreateContextDescFrameGenerationHudless hudless{};
    hudless.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATION_HUDLESS;
    hudless.hudlessBackBufferFormat = info.hudlessFormat != VK_FORMAT_UNDEFINED ? ffxApiGetSurfaceFormatVK(info.hudlessFormat) : 0;

    ffxOverrideVersion ovr{};
    ovr.header.type = FFX_API_DESC_TYPE_OVERRIDE_VERSION;
    ovr.versionId = v->id;  // always pinned: the choice is ours, not the API's "newest"

    ffxCreateContextDescFrameGeneration create{};
    create.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATION;
    create.flags = (info.asyncCompute ? FFX_FRAMEGENERATION_ENABLE_ASYNC_WORKLOAD_SUPPORT : 0) |
                   (info.depthInverted ? FFX_FRAMEGENERATION_ENABLE_DEPTH_INVERTED : 0) |
                   (info.depthInfinite ? FFX_FRAMEGENERATION_ENABLE_DEPTH_INFINITE : 0) |
                   (info.hdr ? FFX_FRAMEGENERATION_ENABLE_HIGH_DYNAMIC_RANGE : 0) |
                   (info.debugChecking ? FFX_FRAMEGENERATION_ENABLE_DEBUG_CHECKING : 0);
    create.displaySize = {info.displaySize.width, info.displaySize.height};
    create.maxRenderSize = {info.maxRenderSize.width, info.maxRenderSize.height};
    create.backBufferFormat = ffxApiGetSurfaceFormatVK(info.backBufferFormat);
    // chain: create -> backend -> version [-> hudless format] -> override
    ffxApiHeader* tail = &create.header;
    const auto link = [&tail](ffxApiHeader* h) {
        tail->pNext = h;
        h->pNext = nullptr;
        tail = h;
    };
    link(&backend.header);
    link(&apiVersion.header);
    if (info.hudlessFormat != VK_FORMAT_UNDEFINED) link(&hudless.header);
    link(&ovr.header);

    ffxContext c = nullptr;
    const ffxReturnCode_t rc = fn_->api.CreateContext(&c, &create.header, nullptr);
    if (rc != FFX_API_RETURN_OK || c == nullptr) {
        spdlog::error("framegen: ffxCreateContext failed ({}) for provider '{}'", rc, v->name);
        return false;
    }
    context_ = c;
    info_ = info;
    ffxQueryGetProviderVersion q{};
    q.header.type = FFX_API_QUERY_DESC_TYPE_GET_PROVIDER_VERSION;
    provider_.clear();
    if (fn_->api.Query(&c, &q.header) == FFX_API_RETURN_OK && q.versionName != nullptr) provider_ = q.versionName;
    spdlog::info("framegen: context {}x{} (render up to {}x{}) flags 0x{:x}, provider '{}' (version id 0x{:x}), {} MiB", info.displaySize.width,
                 info.displaySize.height, info.maxRenderSize.width, info.maxRenderSize.height, create.flags, provider_, v->id,
                 memoryBytes() >> 20);
    return true;
}

bool FrameGen::detachFromSwapchain() {
    if (context_ == nullptr || !info_.swapchain || cfgSwapchain_ == VK_NULL_HANDLE) return true;
    ffxConfigureDescFrameGeneration c{};
    c.header.type = FFX_API_CONFIGURE_DESC_TYPE_FRAMEGENERATION;
    c.swapChain = reinterpret_cast<void*>(cfgSwapchain_);
    c.presentCallback = reinterpret_cast<FfxApiPresentCallbackFunc>(const_cast<void*>(cfgPresent_));
    c.presentCallbackUserContext = cfgPresentUser_;
    c.frameGenerationCallback = nullptr;
    c.frameGenerationCallbackUserContext = nullptr;
    c.frameGenerationEnabled = false;
    c.HUDLessColor = {};
    ffxContext ctx = context_;
    const ffxReturnCode_t rc = fn_->api.Configure(&ctx, &c.header);
    cfgSwapchain_ = VK_NULL_HANDLE;
    if (rc != FFX_API_RETURN_OK) spdlog::warn("framegen: detaching from the swapchain failed ({})", rc);
    return rc == FFX_API_RETURN_OK;
}

void FrameGen::destroyContext() {
    detachFromSwapchain();
    if (context_ != nullptr && fn_ != nullptr) {
        ffxContext c = context_;
        fn_->api.DestroyContext(&c, nullptr);
    }
    context_ = nullptr;
    provider_.clear();
}

void FrameGen::destroy() {
    destroyContext();
    delete fn_;
    fn_ = nullptr;
    if (module_ != nullptr) {
        FreeLibrary(static_cast<HMODULE>(module_));
        module_ = nullptr;
    }
}

std::uint64_t FrameGen::memoryBytes() {
    if (context_ == nullptr || fn_ == nullptr) return 0;
    FfxApiEffectMemoryUsage mu{};
    ffxQueryDescFrameGenerationGetGPUMemoryUsage q{};
    q.header.type = FFX_API_QUERY_DESC_TYPE_FRAMEGENERATION_GPU_MEMORY_USAGE;
    q.gpuMemoryUsageFrameGeneration = &mu;
    ffxContext c = context_;
    return fn_->api.Query(&c, &q.header) == FFX_API_RETURN_OK ? mu.totalUsageInBytes : 0;
}

bool FrameGen::configure(const FrameGenConfigure& cfg) {
    if (context_ == nullptr) return false;
    ffxConfigureDescFrameGeneration c{};
    c.header.type = FFX_API_CONFIGURE_DESC_TYPE_FRAMEGENERATION;
    c.frameGenerationEnabled = cfg.enabled;
    c.allowAsyncWorkloads = info_.asyncCompute;
    if (info_.swapchain) {
        // the callback model: the swapchain interpolates inside its present (our callback runs the dispatch) and composes the UI
        c.swapChain = reinterpret_cast<void*>(cfg.swapchain);
        c.presentCallback = reinterpret_cast<FfxApiPresentCallbackFunc>(const_cast<void*>(cfg.presentCallback));
        c.presentCallbackUserContext = cfg.presentUser;
        c.frameGenerationCallback = reinterpret_cast<FfxApiFrameGenerationDispatchFunc>(&FrameGen::frameGenCallback);
        c.frameGenerationCallbackUserContext = this;
        c.flags = cfg.debugFlags;
        resetFrame_.store(cfg.reset ? cfg.frameId : 0);
        cfgSwapchain_ = cfg.swapchain;
        cfgPresent_ = cfg.presentCallback;
        cfgPresentUser_ = cfg.presentUser;
    } else {
        c.swapChain = nullptr;  // the manual model: no swapchain, the provider only interpolates
        c.flags = FFX_FRAMEGENERATION_FLAG_NO_SWAPCHAIN_CONTEXT_NOTIFY | cfg.debugFlags;
    }
    const VkExtent2D rect = cfg.rect.width != 0 ? cfg.rect : info_.displaySize;
    c.generationRect = {0, 0, static_cast<std::int32_t>(rect.width), static_cast<std::int32_t>(rect.height)};
    c.frameID = cfg.frameId;
    c.HUDLessColor = resource(cfg.hudless, kRead);
    ffxContext ctx = context_;
    const ffxReturnCode_t rc = fn_->api.Configure(&ctx, &c.header);
    if (rc != FFX_API_RETURN_OK) {
        spdlog::error("framegen: ffxConfigure failed ({})", rc);
        return false;
    }
    return true;
}

bool FrameGen::prepare(const FrameGenPrepare& p) {
    if (context_ == nullptr || p.depth == nullptr || p.motion == nullptr) return false;
    p.depth->transition(p.cmd, layoutOf(kRead));
    p.motion->transition(p.cmd, layoutOf(kRead));
    ffxDispatchDescFrameGenerationPrepareV2 d{};
    d.header.type = FFX_API_DISPATCH_DESC_TYPE_FRAMEGENERATION_PREPARE_V2;
    d.frameID = p.frameId;
    d.flags = info_.swapchain ? 0 : FFX_FRAMEGENERATION_FLAG_NO_SWAPCHAIN_CONTEXT_NOTIFY;
    d.commandList = p.cmd;
    d.renderSize = {p.renderSize.width, p.renderSize.height};
    d.jitterOffset = {p.jitterX, p.jitterY};
    d.motionVectorScale = {p.motionScaleX, p.motionScaleY};
    d.frameTimeDelta = p.frameTimeMs;
    d.reset = p.reset;
    d.cameraNear = p.cameraNear;
    d.cameraFar = p.cameraFar;
    d.cameraFovAngleVertical = p.cameraFovY;
    d.viewSpaceToMetersFactor = p.viewSpaceToMeters;
    d.depth = resource(p.depth, kRead);
    d.motionVectors = resource(p.motion, kRead);
    std::copy_n(p.position, 3, d.cameraPosition);
    std::copy_n(p.up, 3, d.cameraUp);
    std::copy_n(p.right, 3, d.cameraRight);
    std::copy_n(p.forward, 3, d.cameraForward);
    ffxContext c = context_;
    const ffxReturnCode_t rc = fn_->api.Dispatch(&c, &d.header);
    p.depth->setTrackedLayout(layoutOf(kRead));
    p.motion->setTrackedLayout(layoutOf(kRead));
    if (rc != FFX_API_RETURN_OK) {
        spdlog::error("framegen: ffxDispatch(prepare) failed ({})", rc);
        return false;
    }
    return true;
}

// the swapchain's interpolation, inside its present (FgSwapchain::present holds the context mutex): the camera-cut reset of the
// frame is OR-ed into the swapchain's own (it sets it for the first frame after the generator was enabled)
long FrameGen::frameGenCallback(void* params, void* user) { return static_cast<FrameGen*>(user)->onFrameGeneration(params); }

long FrameGen::onFrameGeneration(void* paramsPtr) {
    auto* p = static_cast<ffxDispatchDescFrameGeneration*>(paramsPtr);
    if (context_ == nullptr || fn_ == nullptr) return FFX_API_RETURN_ERROR_PARAMETER;
    const std::uint64_t resetFrame = resetFrame_.load();
    if (resetFrame != 0 && resetFrame == p->frameID) p->reset = true;
    ffxContext c = context_;
    return static_cast<long>(fn_->api.Dispatch(&c, &p->header));
}

bool FrameGen::dispatch(const FrameGenDispatch& d) {
    if (context_ == nullptr || d.color == nullptr || d.output == nullptr) return false;
    d.color->transition(d.cmd, layoutOf(kRead));
    d.output->transition(d.cmd, layoutOf(kWrite));
    ffxDispatchDescFrameGeneration f{};
    f.header.type = FFX_API_DISPATCH_DESC_TYPE_FRAMEGENERATION;
    f.commandList = d.cmd;
    f.presentColor = resource(d.color, kRead);
    f.outputs[0] = resource(d.output, kWrite);
    f.numGeneratedFrames = 1;
    f.reset = d.reset;
    // display-referred sRGB colour (the HUDless / present images are the back buffer); luminance only matters for HDR
    f.backbufferTransferFunction = FFX_API_BACKBUFFER_TRANSFER_FUNCTION_SRGB;
    f.minMaxLuminance[0] = 0.0f;
    f.minMaxLuminance[1] = 80.0f;
    const VkExtent2D rect = d.rect.width != 0 ? d.rect : info_.displaySize;
    f.generationRect = {0, 0, static_cast<std::int32_t>(rect.width), static_cast<std::int32_t>(rect.height)};
    f.frameID = d.frameId;
    ffxContext c = context_;
    const ffxReturnCode_t rc = fn_->api.Dispatch(&c, &f.header);
    d.color->setTrackedLayout(layoutOf(kRead));
    d.output->setTrackedLayout(layoutOf(kWrite));
    if (rc != FFX_API_RETURN_OK) {
        spdlog::error("framegen: ffxDispatch(frame generation) failed ({})", rc);
        return false;
    }
    return true;
}

}  // namespace rvk::vk
