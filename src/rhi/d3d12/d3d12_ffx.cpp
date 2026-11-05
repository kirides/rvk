#include "rhi/d3d12/d3d12_ffx.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <format>
#include <mutex>
#include <thread>
#include <tuple>
#include <vector>

#include <spdlog/spdlog.h>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <d3d12.h>
#include <dxgi1_5.h>
#include <wrl/client.h>

#include <dx12/ffx_api_dx12.h>
#include <dx12/ffx_api_framegeneration_dx12.h>
#include <ffx_api.h>
#include <ffx_api_loader.h>
#include <ffx_framegeneration.h>
#include <ffx_upscale.h>

#include "vk/framegen.h"  // the pure version helpers (parseFrameGenVersion, pickSwapchainProvider ...)

namespace rvk::rhi::d3d12ffx {

namespace {

using Microsoft::WRL::ComPtr;

std::string narrow(const wchar_t* w) {
    if (w == nullptr) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
    if (n <= 1) return {};
    std::string s(static_cast<std::size_t>(n - 1), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w, -1, s.data(), n, nullptr, nullptr);
    return s;
}

void fsrMessage(std::uint32_t type, const wchar_t* message) {
    if (type == FFX_API_MESSAGE_TYPE_ERROR) spdlog::error("fsr: {}", narrow(message));
    else spdlog::warn("fsr: {}", narrow(message));
}
void fgMessage(std::uint32_t type, const wchar_t* message) {
    if (type == FFX_API_MESSAGE_TYPE_ERROR) spdlog::error("framegen: {}", narrow(message));
    else spdlog::warn("framegen: {}", narrow(message));
}
void fgSwapchainMessage(std::uint32_t type, const wchar_t* message) {
    if (type == FFX_API_MESSAGE_TYPE_ERROR) spdlog::error("fg swapchain: {}", narrow(message));
    else spdlog::warn("fg swapchain: {}", narrow(message));
}

constexpr std::uint32_t kRead = FFX_API_RESOURCE_STATE_PIXEL_COMPUTE_READ;  // = ALL_SHADER_RESOURCE, the RHI's SHADER_READ_ONLY_OPTIMAL
constexpr std::uint32_t kWrite = FFX_API_RESOURCE_STATE_UNORDERED_ACCESS;   // = the RHI's GENERAL

FfxApiResource resource(Texture* t, std::uint32_t state) {
    if (t == nullptr) return {};
    return ffxApiGetResourceDX12(resourceOf(*t), state);
}

VkImageLayout layoutOf(std::uint32_t state) { return state == kWrite ? VK_IMAGE_LAYOUT_GENERAL : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL; }

std::uint32_t ffxFormat(VkFormat f) {
    return f == VK_FORMAT_UNDEFINED ? 0u : ffxApiGetSurfaceFormatDX12(static_cast<DXGI_FORMAT>(dxgiFormatOf(f)));
}

HMODULE loadModule(const std::filesystem::path& dll, ffxFunctions& api, const char* who) {
    HMODULE m = LoadLibraryExW(dll.c_str(), nullptr, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
    if (m == nullptr) {
        spdlog::warn("{}: cannot load {} (error {})", who, dll.string(), GetLastError());
        return nullptr;
    }
    ffxLoadFunctions(&api, m);
    if (api.CreateContext == nullptr || api.DestroyContext == nullptr || api.Configure == nullptr || api.Query == nullptr || api.Dispatch == nullptr) {
        spdlog::warn("{}: {} is not an FFX API DLL (entry points missing)", who, dll.string());
        FreeLibrary(m);
        return nullptr;
    }
    return m;
}

std::vector<vk::FrameGenVersion> queryVersions(const ffxFunctions& api, ID3D12Device* device, std::uint64_t createDescType) {
    std::vector<vk::FrameGenVersion> out;
    std::uint64_t count = 0;
    ffxQueryDescGetVersions q{};
    q.header.type = FFX_API_QUERY_DESC_TYPE_GET_VERSIONS;
    q.createDescType = createDescType;
    q.device = device;
    q.outputCount = &count;
    if (api.Query(nullptr, &q.header) != FFX_API_RETURN_OK || count == 0) return out;
    std::vector<std::uint64_t> ids(count);
    std::vector<const char*> names(count);
    q.outputCount = &count;
    q.versionIds = ids.data();
    q.versionNames = names.data();
    if (api.Query(nullptr, &q.header) != FFX_API_RETURN_OK) return out;
    for (std::uint64_t i = 0; i < count && i < ids.size(); ++i) {
        out.push_back(vk::parseFrameGenVersion(ids[i], names[i] != nullptr ? names[i] : ""));  // copied now: the API may overwrite the strings later
    }
    return out;
}

D3D12_RESOURCE_STATES d3dStateOf(std::uint32_t s) {
    switch (s) {
        case FFX_API_RESOURCE_STATE_UNORDERED_ACCESS: return D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
        case FFX_API_RESOURCE_STATE_COMPUTE_READ: return D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
        case FFX_API_RESOURCE_STATE_PIXEL_READ: return D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
        case FFX_API_RESOURCE_STATE_PIXEL_COMPUTE_READ: return D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
        case FFX_API_RESOURCE_STATE_COPY_SRC: return D3D12_RESOURCE_STATE_COPY_SOURCE;
        case FFX_API_RESOURCE_STATE_COPY_DEST: return D3D12_RESOURCE_STATE_COPY_DEST;
        case FFX_API_RESOURCE_STATE_GENERIC_READ: return D3D12_RESOURCE_STATE_GENERIC_READ;
        case FFX_API_RESOURCE_STATE_PRESENT: return D3D12_RESOURCE_STATE_PRESENT;
        case FFX_API_RESOURCE_STATE_RENDER_TARGET: return D3D12_RESOURCE_STATE_RENDER_TARGET;
        case FFX_API_RESOURCE_STATE_DEPTH_ATTACHMENT: return D3D12_RESOURCE_STATE_DEPTH_WRITE;
        default: return D3D12_RESOURCE_STATE_COMMON;
    }
}

thread_local bool gInCallback = false;

double qpcSeconds() {
    static const double freq = [] {
        LARGE_INTEGER f;
        QueryPerformanceFrequency(&f);
        return static_cast<double>(f.QuadPart);
    }();
    LARGE_INTEGER c;
    QueryPerformanceCounter(&c);
    return static_cast<double>(c.QuadPart) / freq;
}

// ---- the upscaler ------------------------------------------------------------------------------------------------------------------

class DxUpscaler final : public Upscaler {
public:
    explicit DxUpscaler(GameDevice& device) : dev_(device), n_(nativeOf(device)) {}
    ~DxUpscaler() override { destroy(); }

    bool load(const std::filesystem::path& dll) override {
        if (module_ != nullptr) return true;
        module_ = loadModule(dll, api_, "fsr");
        if (module_ == nullptr) return false;
        spdlog::info("fsr: loaded {} (D3D12)", dll.string());
        return true;
    }
    bool loaded() const override { return module_ != nullptr; }

    bool createContext(const UpscalerCreateInfo& info) override {
        if (module_ == nullptr) return false;
        destroyContext();
        const std::vector<UpscalerVersion> list = versionList();
        const UpscalerVersion* v = pick(list, info.ml);
        if (v == nullptr) {
            spdlog::error("fsr: the API offers no {} upscaler on this device ({} version(s))", info.ml ? "ML (FSR 4)" : "FSR 3", list.size());
            return false;
        }
        const UpscalerVersion* v3 = pick(list, false);
        std::uint32_t flags = 0;
        ffxContext c = createRaw(info, v->id, flags);
        if (c == nullptr) return false;
        context_ = c;
        info_ = info;
        reactiveVersion_ = v3 != nullptr ? v3->id : 0;

        ffxQueryGetProviderVersion q{};
        q.header.type = FFX_API_QUERY_DESC_TYPE_GET_PROVIDER_VERSION;
        provider_.clear();
        if (api_.Query(&c, &q.header) == FFX_API_RETURN_OK && q.versionName != nullptr) provider_ = q.versionName;
        spdlog::info("fsr: context {}x{} -> {}x{} flags 0x{:x}, provider '{}' (version id 0x{:x}), D3D12", info.maxRenderSize.width, info.maxRenderSize.height,
                     info.maxUpscaleSize.width, info.maxUpscaleSize.height, flags, provider_, v->id);

        ffxQueryDescUpscaleGetResourceRequirements rq{};
        rq.header.type = FFX_API_QUERY_DESC_TYPE_UPSCALE_GET_RESOURCE_REQUIREMENTS;
        requirementsKnown_ = api_.Query(&c, &rq.header) == FFX_API_RETURN_OK;
        required_ = requirementsKnown_ ? rq.required_resources : 0;
        optional_ = requirementsKnown_ ? rq.optional_resources : 0;
        if (requirementsKnown_) {
            spdlog::info("fsr: resources required 0x{:x}, optional 0x{:x} (reactive mask {}, transparency mask {})", required_, optional_,
                         consumesReactiveMask() ? "used" : "not used", (optional_ | required_) & (1u << 5) ? "used" : "not used");
        } else {
            spdlog::info("fsr: the provider does not answer the resource requirements query: every mask is built");
        }
        return true;
    }
    bool hasContext() const override { return context_ != nullptr; }
    void destroyContext() override {
        if (reactiveContext_ != nullptr) {
            ffxContext r = reactiveContext_;
            api_.DestroyContext(&r, nullptr);
        }
        reactiveContext_ = nullptr;
        if (context_ != nullptr) {
            ffxContext c = context_;
            api_.DestroyContext(&c, nullptr);
        }
        context_ = nullptr;
        provider_.clear();
        requirementsKnown_ = false;
        required_ = optional_ = 0;
    }
    void destroy() override {
        destroyContext();
        if (module_ != nullptr) {
            FreeLibrary(module_);
            module_ = nullptr;
            api_ = {};
        }
    }

    bool dispatch(const UpscalerDispatch& d) override {
        if (context_ == nullptr || d.color == nullptr || d.depth == nullptr || d.motion == nullptr || d.output == nullptr) return false;
        for (Texture* in : {d.color, d.depth, d.motion, d.exposure, d.reactive, d.transparency}) {
            if (in != nullptr) d.cmd->transition(*in, layoutOf(kRead));
        }
        d.cmd->transition(*d.output, layoutOf(kWrite));

        ffxDispatchDescUpscale u{};
        u.header.type = FFX_API_DISPATCH_DESC_TYPE_UPSCALE;
        u.commandList = listOf(*d.cmd);
        u.color = resource(d.color, kRead);
        u.depth = resource(d.depth, kRead);
        u.motionVectors = resource(d.motion, kRead);
        u.exposure = resource(d.exposure, kRead);
        u.reactive = resource(d.reactive, kRead);
        u.transparencyAndComposition = resource(d.transparency, kRead);
        u.output = resource(d.output, kWrite);
        u.jitterOffset = {d.jitterX, d.jitterY};
        u.motionVectorScale = {d.motionScaleX, d.motionScaleY};
        u.renderSize = {d.renderSize.width, d.renderSize.height};
        u.upscaleSize = {d.upscaleSize.width, d.upscaleSize.height};
        u.enableSharpening = d.sharpness > 0.0f;
        u.sharpness = d.sharpness;
        u.frameTimeDelta = d.frameTimeMs;
        u.preExposure = d.preExposure;
        u.reset = d.reset;
        u.cameraNear = d.cameraNear;
        u.cameraFar = d.cameraFar;
        u.cameraFovAngleVertical = d.cameraFovY;
        u.viewSpaceToMetersFactor = d.viewSpaceToMeters;
        u.flags = d.nonLinearSrgb ? FFX_UPSCALE_FLAG_NON_LINEAR_COLOR_SRGB : 0;

        ffxContext c = context_;
        const ffxReturnCode_t rc = api_.Dispatch(&c, &u.header);
        listAfterForeign(dev_, *d.cmd);
        if (rc != FFX_API_RETURN_OK) {
            spdlog::error("fsr: ffxDispatch failed ({})", rc);
            return false;
        }
        return true;
    }

    bool generateReactiveMask(CommandList& cmd, Texture& opaque, Texture& color, Texture& out, VkExtent2D renderSize, float scale, float cutoff,
                              float binaryValue) override {
        if (context_ == nullptr) return false;
        if (reactiveContext_ == nullptr) {
            UpscalerCreateInfo tiny = info_;
            tiny.maxRenderSize = tiny.maxUpscaleSize = {32, 32};  // the mask pass reads its size from the dispatch
            tiny.ml = false;
            if (reactiveVersion_ == 0) return false;  // no FSR 3 provider: the auto mask is an FSR 3 feature
            std::uint32_t flags = 0;
            reactiveContext_ = createRaw(tiny, reactiveVersion_, flags);
            if (reactiveContext_ == nullptr) return false;
            spdlog::info("fsr: reactive mask context (32x32: only its own pipeline is used)");
        }
        for (Texture* t : {&opaque, &color, &out}) cmd.transition(*t, layoutOf(kRead));
        ffxDispatchDescUpscaleGenerateReactiveMask r{};
        r.header.type = FFX_API_DISPATCH_DESC_TYPE_UPSCALE_GENERATEREACTIVEMASK;
        r.commandList = listOf(cmd);
        r.colorOpaqueOnly = resource(&opaque, kRead);
        r.colorPreUpscale = resource(&color, kRead);
        r.outReactive = resource(&out, kRead);
        r.renderSize = {renderSize.width, renderSize.height};
        r.scale = scale;
        r.cutoffThreshold = cutoff;
        r.binaryValue = binaryValue;
        // a cutoff of 0 = no threshold: the (scaled) difference itself is the mask
        r.flags = FFX_UPSCALE_AUTOREACTIVEFLAGS_APPLY_TONEMAP | (cutoff > 0.0f ? FFX_UPSCALE_AUTOREACTIVEFLAGS_APPLY_THRESHOLD : 0u) |
                  FFX_UPSCALE_AUTOREACTIVEFLAGS_USE_COMPONENTS_MAX;
        ffxContext c = reactiveContext_;
        const ffxReturnCode_t rc = api_.Dispatch(&c, &r.header);
        listAfterForeign(dev_, cmd);
        if (rc != FFX_API_RETURN_OK) {
            spdlog::error("fsr: reactive mask dispatch failed ({})", rc);
            return false;
        }
        return true;
    }

    const std::string& providerName() const override { return provider_; }
    bool consumesReactiveMask() const override { return !requirementsKnown_ || ((required_ | optional_) & kResourceReactiveMask) != 0; }

    int jitterPhaseCount(std::uint32_t renderWidth, std::uint32_t displayWidth) override {
        if (context_ == nullptr) return 0;
        std::int32_t count = 0;
        ffxQueryDescUpscaleGetJitterPhaseCount q{};
        q.header.type = FFX_API_QUERY_DESC_TYPE_UPSCALE_GETJITTERPHASECOUNT;
        q.renderWidth = renderWidth;
        q.displayWidth = displayWidth;
        q.pOutPhaseCount = &count;
        ffxContext c = context_;
        return api_.Query(&c, &q.header) == FFX_API_RETURN_OK ? count : 0;
    }
    bool jitterOffset(int index, int phaseCount, float& x, float& y) override {
        if (context_ == nullptr) return false;
        ffxQueryDescUpscaleGetJitterOffset q{};
        q.header.type = FFX_API_QUERY_DESC_TYPE_UPSCALE_GETJITTEROFFSET;
        q.index = index;
        q.phaseCount = phaseCount;
        q.pOutX = &x;
        q.pOutY = &y;
        ffxContext c = context_;
        return api_.Query(&c, &q.header) == FFX_API_RETURN_OK;
    }
    bool renderSizeForMode(VkExtent2D display, std::uint32_t mode, VkExtent2D& render) override {
        if (context_ == nullptr) return false;
        ffxQueryDescUpscaleGetRenderResolutionFromQualityMode q{};
        q.header.type = FFX_API_QUERY_DESC_TYPE_UPSCALE_GETRENDERRESOLUTIONFROMQUALITYMODE;
        q.displayWidth = display.width;
        q.displayHeight = display.height;
        q.qualityMode = mode;
        q.pOutRenderWidth = &render.width;
        q.pOutRenderHeight = &render.height;
        ffxContext c = context_;
        return api_.Query(&c, &q.header) == FFX_API_RETURN_OK;
    }

    std::vector<UpscalerVersion> versionList() override {
        std::vector<UpscalerVersion> out;
        if (module_ == nullptr) return out;
        for (const vk::FrameGenVersion& v : queryVersions(api_, n_.device, FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE)) {
            out.push_back({v.id, v.name, v.major, v.minor, v.patch, v.fromDriver});
        }
        return out;
    }
    std::string versions() override {
        std::string s;
        for (const UpscalerVersion& v : versionList()) s += std::format("{}0x{:x}: {}", s.empty() ? "" : ", ", v.id, v.name);
        return s;
    }

private:
    static constexpr std::uint64_t kResourceReactiveMask = 1u << 4;  // FFX_API_QUERY_RESOURCE_INPUT_REACTIVEMASK

    static const UpscalerVersion* pick(const std::vector<UpscalerVersion>& list, bool ml) {
        const UpscalerVersion* best = nullptr;
        for (const UpscalerVersion& v : list) {
            if (ml ? v.major < 4 : v.major != 3) continue;
            if (best == nullptr || std::tie(v.major, v.minor, v.patch) > std::tie(best->major, best->minor, best->patch)) best = &v;
        }
        return best;
    }

    // `versionId` 0 = no override (the newest version the API finds, driver provided ones included); else the id of a version returned by the version query
    ffxContext createRaw(const UpscalerCreateInfo& info, std::uint64_t versionId, std::uint32_t& flagsOut) {
        ffxCreateBackendDX12Desc backend{};
        backend.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_BACKEND_DX12;
        backend.device = n_.device;

        ffxCreateContextDescUpscaleVersion apiVersion{};  // SDK 2.1+: the version of the headers this was built against
        apiVersion.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE_VERSION;
        apiVersion.header.pNext = &backend.header;
        apiVersion.version = FFX_UPSCALER_VERSION;

        ffxOverrideVersion ovr{};
        ovr.header.type = FFX_API_DESC_TYPE_OVERRIDE_VERSION;
        ovr.header.pNext = &apiVersion.header;
        ovr.versionId = versionId;

        ffxCreateContextDescUpscale create{};
        create.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE;
        create.header.pNext = versionId != 0 ? &ovr.header : &apiVersion.header;
        create.flags = (info.hdr ? FFX_UPSCALE_ENABLE_HIGH_DYNAMIC_RANGE : 0) |
                       (info.motionJitterCancelled ? FFX_UPSCALE_ENABLE_MOTION_VECTORS_JITTER_CANCELLATION : 0) |
                       (info.invertedDepth ? FFX_UPSCALE_ENABLE_DEPTH_INVERTED : 0) | (info.infiniteDepth ? FFX_UPSCALE_ENABLE_DEPTH_INFINITE : 0) |
                       (info.autoExposure ? FFX_UPSCALE_ENABLE_AUTO_EXPOSURE : 0) | (info.debugChecking ? FFX_UPSCALE_ENABLE_DEBUG_CHECKING : 0) |
                       (info.nonLinearColor ? FFX_UPSCALE_ENABLE_NON_LINEAR_COLORSPACE : 0);
        create.maxRenderSize = {info.maxRenderSize.width, info.maxRenderSize.height};
        create.maxUpscaleSize = {info.maxUpscaleSize.width, info.maxUpscaleSize.height};
        create.fpMessage = fsrMessage;

        ffxContext c = nullptr;
        const ffxReturnCode_t rc = api_.CreateContext(&c, &create.header, nullptr);
        if (rc != FFX_API_RETURN_OK || c == nullptr) {
            spdlog::error("fsr: ffxCreateContext failed ({})", rc);
            return nullptr;
        }
        flagsOut = create.flags;
        return c;
    }

    GameDevice& dev_;
    Native n_;
    HMODULE module_ = nullptr;
    ffxFunctions api_{};
    ffxContext context_ = nullptr;
    ffxContext reactiveContext_ = nullptr;  // the automatic reactive mask runs on a tiny context of its own (one dispatch per frame each)
    std::uint64_t reactiveVersion_ = 0;
    UpscalerCreateInfo info_{};
    std::string provider_;
    bool requirementsKnown_ = false;
    std::uint64_t required_ = 0, optional_ = 0;
};

// ---- the frame generation swapchain --------------------------------------------------------------------------------------------------

class FgSwapchainImpl final : public FgSwapchain {
public:
    explicit FgSwapchainImpl(GameDevice& device) : dev_(device), n_(nativeOf(device)) {}
    ~FgSwapchainImpl() override {
        stopSampler();
        destroy();
        if (module_ != nullptr) FreeLibrary(module_);
    }

    bool load(const std::filesystem::path& dll) override {
        if (module_ != nullptr) return true;
        module_ = loadModule(dll, api_, "fg swapchain");
        if (module_ == nullptr) return false;
        // the runtime's messages of this effect (AMD's own validation of our calls)
        ffxConfigureDescGlobalDebug dbg{};
        dbg.header.type = FFX_API_CONFIGURE_DESC_TYPE_GLOBALDEBUG;
        dbg.effectId = FFX_API_EFFECT_ID_FRAMEGENERATIONSWAPCHAIN;
        dbg.fpMessage = fgSwapchainMessage;
        dbg.debugLevel = FFX_API_CONFIGURE_GLOBALDEBUG_LEVEL_WARNINGS;
        api_.Configure(nullptr, &dbg.header);
        return true;
    }
    bool loaded() const override { return module_ != nullptr; }

    bool create(void* hwnd, std::uint32_t width, std::uint32_t height, std::uint32_t bufferCount, bool tearing, std::string* reason) override {
        const auto fail = [&](std::string why) {
            spdlog::warn("fg swapchain: not created: {}", why);
            if (reason != nullptr) *reason = std::move(why);
            return false;
        };
        if (module_ == nullptr) return fail("the FidelityFX loader is not loaded");
        if (context_ != nullptr) return fail("already created");
        // the provider: pinned, never a driver-provided one (the Vulkan one crashes in its destroy: frame-generation.md 5.3)
        const std::vector<vk::FrameGenVersion> versions = queryVersions(api_, n_.device, FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATIONSWAPCHAIN_FOR_HWND_DX12);
        const vk::FrameGenVersion* pin = vk::pickSwapchainProvider(versions);
        if (pin == nullptr) return fail(std::format("the API offers no usable swapchain provider ({})", vk::describeVersions(versions)));

        DXGI_SWAP_CHAIN_DESC1 sd{};
        sd.Width = width;
        sd.Height = height;
        sd.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        sd.SampleDesc = {1, 0};
        sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        sd.BufferCount = bufferCount;
        sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
        sd.AlphaMode = DXGI_ALPHA_MODE_IGNORE;
        sd.Flags = tearing ? DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING : 0;

        IDXGISwapChain4* sc = nullptr;
        ffxCreateContextDescFrameGenerationSwapChainForHwndDX12 cs{};
        cs.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATIONSWAPCHAIN_FOR_HWND_DX12;
        cs.swapchain = &sc;
        cs.hwnd = static_cast<HWND>(hwnd);
        cs.desc = &sd;
        cs.fullscreenDesc = nullptr;
        cs.dxgiFactory = n_.factory;
        cs.gameQueue = n_.queue;
        ffxCreateContextDescFrameGenerationSwapChainVersionDX12 ver{};
        ver.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATIONSWAPCHAIN_VERSION_DX12;
        ver.version = FFX_FRAMEGENERATION_SWAPCHAIN_DX12_VERSION;
        ffxOverrideVersion ovr{};
        ovr.header.type = FFX_API_DESC_TYPE_OVERRIDE_VERSION;
        ovr.versionId = pin->id;
        cs.header.pNext = &ver.header;
        ver.header.pNext = &ovr.header;
        ovr.header.pNext = nullptr;

        ffxContext c = nullptr;
        const ffxReturnCode_t rc = api_.CreateContext(&c, &cs.header, nullptr);
        if (rc != FFX_API_RETURN_OK || c == nullptr || sc == nullptr) {
            return fail(std::format("ffxCreateContext(swapchain) returned {} (provider '{}')", rc, pin->name));
        }
        ffxQueryGetProviderVersion pv{};
        pv.header.type = FFX_API_QUERY_DESC_TYPE_GET_PROVIDER_VERSION;
        provider_ = pin->name;
        if (api_.Query(&c, &pv.header) == FFX_API_RETURN_OK && pv.versionName != nullptr) provider_ = pv.versionName;
        context_ = c;
        sc_.Attach(sc);
        // the UI composition's render target views (the present callback runs on AMD's threads: a ring of our own, not the device's)
        D3D12_DESCRIPTOR_HEAP_DESC hd{};
        hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
        hd.NumDescriptors = kRtvCount;
        if (FAILED(n_.device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&rtvHeap_)))) {
            destroy();
            return fail("the render target view heap of the UI composition cannot be created");
        }
        rtvStep_ = n_.device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
        extent_ = {width, height};
        spdlog::info("fg swapchain: created {}x{}, {} app-facing buffers, provider '{}' (offered: {})", width, height, bufferCount, provider_, vk::describeVersions(versions));
        return true;
    }

    void destroy() override {
        if (context_ == nullptr) {
            sc_.Reset();
            return;
        }
        stopSampler();
        waitForPresents();
        {
            const std::lock_guard lock(mutex_);
            ffxContext c = context_;
            api_.DestroyContext(&c, nullptr);  // the swapchain context first ...
            context_ = nullptr;
            sc_.Reset();                       // ... then our reference of the swapchain (the owner released its own before)
        }
        rtvHeap_.Reset();
        spdlog::info("fg swapchain: destroyed");
    }
    bool valid() const override { return context_ != nullptr && sc_ != nullptr; }
    IDXGISwapChain4* swapchain() const override { return sc_.Get(); }
    const void* presentCallback() const override { return reinterpret_cast<const void*>(&FgSwapchainImpl::presentThunk); }
    void* presentUser() override { return this; }

    // ---- PresentWrapper ----
    const std::string& providerName() const override { return provider_; }
    // the swapchain's own present counter when it keeps one (IDXGISwapChain::GetLastPresentCount of AMD's wrapper), else the callbacks' count (which run
    // when the frame is recorded, ahead of the real present)
    std::uint64_t realPresentCount() const override {
        UINT n = 0;
        if (sc_ != nullptr && SUCCEEDED(sc_->GetLastPresentCount(&n)) && n != 0) return n;
        return presents_.load(std::memory_order_relaxed);
    }
    void waitForPresents() override {
        if (context_ == nullptr) return;
        ffxDispatchDescFrameGenerationSwapChainWaitForPresentsDX12 w{};
        w.header.type = FFX_API_DISPATCH_DESC_TYPE_FRAMEGENERATIONSWAPCHAIN_WAIT_FOR_PRESENTS_DX12;
        ffxContext c = context_;
        api_.Dispatch(&c, &w.header);
    }
    std::mutex& contextMutex() override { return mutex_; }
    void setUi(PresentUi ui) override {
        const std::lock_guard lock(uiMutex_);
        ui_ = std::move(ui);
    }
    Counters takeCounters() override {
        const std::lock_guard lock(counterMutex_);
        const Counters c = counters_;
        counters_ = {};
        return c;
    }
    void startSampler() override {
        if (sampling_.exchange(true) || context_ == nullptr) return;
        sampler_ = std::thread([this] {
            SetThreadDescription(GetCurrentThread(), L"rvk fg present sampler");
            std::uint64_t last = realPresentCount();
            while (sampling_.load(std::memory_order_relaxed)) {
                const std::uint64_t n = realPresentCount();
                if (n != last) {
                    const double t = qpcSeconds();
                    const std::lock_guard lock(sampleMutex_);
                    samples_.push_back({t, n});
                    last = n;
                }
                YieldProcessor();
            }
        });
    }
    void stopSampler() override {
        sampling_ = false;
        if (sampler_.joinable()) sampler_.join();
    }
    std::vector<PresentSample> takeSamples() override {
        const std::lock_guard lock(sampleMutex_);
        std::vector<PresentSample> v;
        v.swap(samples_);
        return v;
    }

private:
    static constexpr std::uint32_t kRtvCount = 16;

    static ffxReturnCode_t presentThunk(ffxCallbackDescFrameGenerationPresent* params, void* user) {
        return static_cast<ffxReturnCode_t>(static_cast<FgSwapchainImpl*>(user)->onPresent(params));
    }

    // Copies the frame to show (the real or the generated one) into the real swapchain buffer and draws the UI on top: what the SDK's default UI
    // composition does too. Barriers take the states the API reports for the two buffers and put them back.
    long onPresent(ffxCallbackDescFrameGenerationPresent* p) {
        const auto t0 = std::chrono::steady_clock::now();
        auto* cmd = static_cast<ID3D12GraphicsCommandList*>(p->commandList);
        auto* src = static_cast<ID3D12Resource*>(p->currentBackBuffer.resource);
        auto* dst = static_cast<ID3D12Resource*>(p->outputSwapChainBuffer.resource);
        if (cmd == nullptr || src == nullptr || dst == nullptr) return FFX_API_RETURN_ERROR_PARAMETER;
        const D3D12_RESOURCE_STATES srcState = d3dStateOf(p->currentBackBuffer.state);
        const D3D12_RESOURCE_STATES dstState = d3dStateOf(p->outputSwapChainBuffer.state);
        const VkExtent2D extent{p->outputSwapChainBuffer.description.width, p->outputSwapChainBuffer.description.height};
        const VkExtent2D srcExtent{p->currentBackBuffer.description.width, p->currentBackBuffer.description.height};

        bool ui = false;
        {
            const std::lock_guard lock(uiMutex_);
            ui = ui_.wants && ui_.record && ui_.wants();
        }
        const auto barrier = [&](ID3D12Resource* r, D3D12_RESOURCE_STATES from, D3D12_RESOURCE_STATES to) {
            if (from == to) return;
            D3D12_RESOURCE_BARRIER b{};
            b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            b.Transition.pResource = r;
            b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            b.Transition.StateBefore = from;
            b.Transition.StateAfter = to;
            cmd->ResourceBarrier(1, &b);
        };
        barrier(src, srcState, D3D12_RESOURCE_STATE_COPY_SOURCE);
        barrier(dst, dstState, D3D12_RESOURCE_STATE_COPY_DEST);
        if (srcExtent.width == extent.width && srcExtent.height == extent.height) {
            cmd->CopyResource(dst, src);
        } else {
            spdlog::error("fg swapchain: the frame is {}x{}, the swapchain {}x{}", srcExtent.width, srcExtent.height, extent.width, extent.height);
        }
        if (ui) {
            barrier(dst, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_RENDER_TARGET);
            const std::uint32_t slot = rtvNext_.fetch_add(1, std::memory_order_relaxed) % kRtvCount;
            D3D12_CPU_DESCRIPTOR_HANDLE h = rtvHeap_->GetCPUDescriptorHandleForHeapStart();
            h.ptr += static_cast<SIZE_T>(slot) * rtvStep_;
            D3D12_RENDER_TARGET_VIEW_DESC rv{};
            rv.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
            rv.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
            n_.device->CreateRenderTargetView(dst, &rv, h);
            cmd->OMSetRenderTargets(1, &h, FALSE, nullptr);
            const D3D12_VIEWPORT vp{0.0f, 0.0f, static_cast<float>(extent.width), static_cast<float>(extent.height), 0.0f, 1.0f};
            const D3D12_RECT sr{0, 0, static_cast<LONG>(extent.width), static_cast<LONG>(extent.height)};
            cmd->RSSetViewports(1, &vp);
            cmd->RSSetScissorRects(1, &sr);
            {
                const std::lock_guard lock(uiMutex_);
                if (ui_.record) {
                    const std::unique_ptr<CommandList> list = wrapList(dev_, cmd);
                    gInCallback = true;
                    ui_.record(*list, extent);
                    gInCallback = false;
                }
            }
            barrier(dst, D3D12_RESOURCE_STATE_RENDER_TARGET, dstState);
        } else {
            barrier(dst, D3D12_RESOURCE_STATE_COPY_DEST, dstState);
        }
        barrier(src, D3D12_RESOURCE_STATE_COPY_SOURCE, srcState);

        presents_.fetch_add(1, std::memory_order_relaxed);
        const std::lock_guard lock(counterMutex_);
        ++counters_.callbacks;
        if (p->isGeneratedFrame) ++counters_.generatedFrames;
        if (ui) ++counters_.uiFrames;
        counters_.callbackMs += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        return FFX_API_RETURN_OK;
    }

    GameDevice& dev_;
    Native n_;
    HMODULE module_ = nullptr;
    ffxFunctions api_{};
    ffxContext context_ = nullptr;
    ComPtr<IDXGISwapChain4> sc_;
    std::string provider_;
    VkExtent2D extent_{};
    std::mutex mutex_;

    ComPtr<ID3D12DescriptorHeap> rtvHeap_;
    UINT rtvStep_ = 0;
    std::atomic<std::uint32_t> rtvNext_{0};

    std::mutex uiMutex_;
    PresentUi ui_;
    std::mutex counterMutex_;
    Counters counters_;
    std::atomic<std::uint64_t> presents_{0};

    std::atomic<bool> sampling_{false};
    std::thread sampler_;
    std::mutex sampleMutex_;
    std::vector<PresentSample> samples_;
};

// ---- the frame generator -------------------------------------------------------------------------------------------------------------

class DxFrameGenerator final : public FrameGenerator {
public:
    explicit DxFrameGenerator(GameDevice& device) : dev_(device), n_(nativeOf(device)) {}
    ~DxFrameGenerator() override { destroy(); }

    bool load(const std::filesystem::path& dll) override {
        if (module_ != nullptr) return true;
        module_ = loadModule(dll, api_, "framegen");
        if (module_ == nullptr) return false;
        // the runtime's messages of this effect (the upscaler's go through its create description)
        ffxConfigureDescGlobalDebug dbg{};
        dbg.header.type = FFX_API_CONFIGURE_DESC_TYPE_GLOBALDEBUG;
        dbg.effectId = FFX_API_EFFECT_ID_FRAMEGENERATION;
        dbg.fpMessage = fgMessage;
        dbg.debugLevel = FFX_API_CONFIGURE_GLOBALDEBUG_LEVEL_WARNINGS;
        api_.Configure(nullptr, &dbg.header);
        spdlog::info("framegen: loaded {} (D3D12)", dll.string());
        return true;
    }
    bool loaded() const override { return module_ != nullptr; }

    bool createContext(const FrameGeneratorCreateInfo& info) override {
        if (module_ == nullptr) return false;
        destroyContext();
        const std::vector<vk::FrameGenVersion> list = queryVersions(api_, n_.device, FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATION);
        const vk::FrameGenVersion* v = vk::pickFrameGenProvider(list, info.ml);
        if (v == nullptr) {
            spdlog::error("framegen: the API offers no {} frame generation on this device ({} version(s): {})", info.ml ? "ML (FSR 4)" : "FSR 3", list.size(),
                          vk::describeVersions(list));
            return false;
        }
        ffxCreateBackendDX12Desc backend{};
        backend.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_BACKEND_DX12;
        backend.device = n_.device;

        ffxCreateContextDescFrameGenerationVersion apiVersion{};  // SDK 2.1+: the version of the headers this was built against
        apiVersion.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATION_VERSION;
        apiVersion.version = FFX_FRAMEGENERATION_VERSION;

        ffxCreateContextDescFrameGenerationHudless hudless{};
        hudless.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATION_HUDLESS;
        hudless.hudlessBackBufferFormat = ffxFormat(info.hudlessFormat);

        ffxOverrideVersion ovr{};
        ovr.header.type = FFX_API_DESC_TYPE_OVERRIDE_VERSION;
        ovr.versionId = v->id;  // always pinned: the choice is ours, not the API's "newest"

        ffxCreateContextDescFrameGeneration create{};
        create.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATION;
        create.flags = (info.asyncCompute ? FFX_FRAMEGENERATION_ENABLE_ASYNC_WORKLOAD_SUPPORT : 0) | (info.depthInverted ? FFX_FRAMEGENERATION_ENABLE_DEPTH_INVERTED : 0) |
                       (info.depthInfinite ? FFX_FRAMEGENERATION_ENABLE_DEPTH_INFINITE : 0) | (info.hdr ? FFX_FRAMEGENERATION_ENABLE_HIGH_DYNAMIC_RANGE : 0) |
                       (info.debugChecking ? FFX_FRAMEGENERATION_ENABLE_DEBUG_CHECKING : 0);
        create.displaySize = {info.displaySize.width, info.displaySize.height};
        create.maxRenderSize = {info.maxRenderSize.width, info.maxRenderSize.height};
        create.backBufferFormat = ffxFormat(info.backBufferFormat);
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
        const ffxReturnCode_t rc = api_.CreateContext(&c, &create.header, nullptr);
        if (rc != FFX_API_RETURN_OK || c == nullptr) {
            spdlog::error("framegen: ffxCreateContext failed ({}) for provider '{}'", rc, v->name);
            return false;
        }
        context_ = c;
        info_ = info;
        ffxQueryGetProviderVersion q{};
        q.header.type = FFX_API_QUERY_DESC_TYPE_GET_PROVIDER_VERSION;
        provider_.clear();
        if (api_.Query(&c, &q.header) == FFX_API_RETURN_OK && q.versionName != nullptr) provider_ = q.versionName;
        spdlog::info("framegen: context {}x{} (render up to {}x{}) flags 0x{:x}, provider '{}' (version id 0x{:x}), {} MiB, D3D12", info.displaySize.width,
                     info.displaySize.height, info.maxRenderSize.width, info.maxRenderSize.height, create.flags, provider_, v->id, memoryBytes() >> 20);
        return true;
    }
    bool hasContext() const override { return context_ != nullptr; }
    void destroyContext() override {
        detachFromSwapchain();
        if (context_ != nullptr) {
            ffxContext c = context_;
            api_.DestroyContext(&c, nullptr);
        }
        context_ = nullptr;
        provider_.clear();
    }
    void destroy() override {
        destroyContext();
        if (module_ != nullptr) {
            FreeLibrary(module_);
            module_ = nullptr;
            api_ = {};
        }
    }
    bool detachFromSwapchain() override {
        if (context_ == nullptr || !info_.swapchain || cfgSwapchain_ == nullptr) return true;
        ffxConfigureDescFrameGeneration c{};
        c.header.type = FFX_API_CONFIGURE_DESC_TYPE_FRAMEGENERATION;
        c.swapChain = cfgSwapchain_;
        c.presentCallback = reinterpret_cast<FfxApiPresentCallbackFunc>(const_cast<void*>(cfgPresent_));
        c.presentCallbackUserContext = cfgPresentUser_;
        c.frameGenerationCallback = nullptr;
        c.frameGenerationCallbackUserContext = nullptr;
        c.frameGenerationEnabled = false;
        c.HUDLessColor = {};
        ffxContext ctx = context_;
        const ffxReturnCode_t rc = api_.Configure(&ctx, &c.header);
        cfgSwapchain_ = nullptr;
        if (rc != FFX_API_RETURN_OK) spdlog::warn("framegen: detaching from the swapchain failed ({})", rc);
        return rc == FFX_API_RETURN_OK;
    }

    bool configure(const FrameGeneratorConfigure& cfg) override {
        if (context_ == nullptr) return false;
        ffxConfigureDescFrameGeneration c{};
        c.header.type = FFX_API_CONFIGURE_DESC_TYPE_FRAMEGENERATION;
        c.frameGenerationEnabled = cfg.enabled;
        c.allowAsyncWorkloads = info_.asyncCompute;
        if (info_.swapchain) {
            // the callback model: the swapchain interpolates inside its present (our callback runs the dispatch) and composes the UI
            auto* sc = static_cast<FgSwapchain*>(cfg.present);
            c.swapChain = sc != nullptr ? sc->swapchain() : nullptr;
            c.presentCallback = sc != nullptr ? reinterpret_cast<FfxApiPresentCallbackFunc>(const_cast<void*>(sc->presentCallback())) : nullptr;
            c.presentCallbackUserContext = sc != nullptr ? sc->presentUser() : nullptr;
            c.frameGenerationCallback = reinterpret_cast<FfxApiFrameGenerationDispatchFunc>(&DxFrameGenerator::frameGenCallback);
            c.frameGenerationCallbackUserContext = this;
            c.flags = cfg.debugFlags;
            resetFrame_.store(cfg.reset ? cfg.frameId : 0);
            cfgSwapchain_ = c.swapChain;
            cfgPresent_ = reinterpret_cast<const void*>(c.presentCallback);
            cfgPresentUser_ = c.presentCallbackUserContext;
        } else {
            c.swapChain = nullptr;  // the manual model: no swapchain, the provider only interpolates
            c.flags = FFX_FRAMEGENERATION_FLAG_NO_SWAPCHAIN_CONTEXT_NOTIFY | cfg.debugFlags;
        }
        const VkExtent2D rect = cfg.rect.width != 0 ? cfg.rect : info_.displaySize;
        c.generationRect = {0, 0, static_cast<std::int32_t>(rect.width), static_cast<std::int32_t>(rect.height)};
        c.frameID = cfg.frameId;
        c.HUDLessColor = resource(cfg.hudless, kRead);
        ffxContext ctx = context_;
        const ffxReturnCode_t rc = api_.Configure(&ctx, &c.header);
        if (rc != FFX_API_RETURN_OK) {
            spdlog::error("framegen: ffxConfigure failed ({})", rc);
            return false;
        }
        return true;
    }

    bool prepare(const FrameGeneratorPrepare& p) override {
        if (context_ == nullptr || p.depth == nullptr || p.motion == nullptr) return false;
        p.cmd->transition(*p.depth, layoutOf(kRead));
        p.cmd->transition(*p.motion, layoutOf(kRead));
        ffxDispatchDescFrameGenerationPrepareV2 d{};
        d.header.type = FFX_API_DISPATCH_DESC_TYPE_FRAMEGENERATION_PREPARE_V2;
        d.frameID = p.frameId;
        d.flags = info_.swapchain ? 0 : FFX_FRAMEGENERATION_FLAG_NO_SWAPCHAIN_CONTEXT_NOTIFY;
        d.commandList = listOf(*p.cmd);
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
        const ffxReturnCode_t rc = api_.Dispatch(&c, &d.header);
        listAfterForeign(dev_, *p.cmd);
        if (rc != FFX_API_RETURN_OK) {
            spdlog::error("framegen: ffxDispatch(prepare) failed ({})", rc);
            return false;
        }
        return true;
    }

    bool dispatch(const FrameGeneratorDispatch& d) override {
        if (context_ == nullptr || d.color == nullptr || d.output == nullptr) return false;
        d.cmd->transition(*d.color, layoutOf(kRead));
        d.cmd->transition(*d.output, layoutOf(kWrite));
        ffxDispatchDescFrameGeneration f{};
        f.header.type = FFX_API_DISPATCH_DESC_TYPE_FRAMEGENERATION;
        f.commandList = listOf(*d.cmd);
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
        const ffxReturnCode_t rc = api_.Dispatch(&c, &f.header);
        listAfterForeign(dev_, *d.cmd);
        if (rc != FFX_API_RETURN_OK) {
            spdlog::error("framegen: ffxDispatch(frame generation) failed ({})", rc);
            return false;
        }
        return true;
    }

    const std::string& providerName() const override { return provider_; }
    bool swapchainModel() const override { return info_.swapchain; }
    std::uint64_t memoryBytes() override {
        if (context_ == nullptr) return 0;
        FfxApiEffectMemoryUsage mu{};
        ffxQueryDescFrameGenerationGetGPUMemoryUsage q{};
        q.header.type = FFX_API_QUERY_DESC_TYPE_FRAMEGENERATION_GPU_MEMORY_USAGE;
        q.gpuMemoryUsageFrameGeneration = &mu;
        ffxContext c = context_;
        return api_.Query(&c, &q.header) == FFX_API_RETURN_OK ? mu.totalUsageInBytes : 0;
    }

private:
    // the swapchain's interpolation, inside its present (the present holds the context mutex): the camera-cut reset of the frame is OR-ed into the
    // swapchain's own (it sets it for the first frame after the generator was enabled)
    static long frameGenCallback(void* params, void* user) { return static_cast<DxFrameGenerator*>(user)->onFrameGeneration(params); }
    long onFrameGeneration(void* paramsPtr) {
        auto* p = static_cast<ffxDispatchDescFrameGeneration*>(paramsPtr);
        if (context_ == nullptr) return FFX_API_RETURN_ERROR_PARAMETER;
        const std::uint64_t resetFrame = resetFrame_.load();
        if (resetFrame != 0 && resetFrame == p->frameID) p->reset = true;
        ffxContext c = context_;
        return static_cast<long>(api_.Dispatch(&c, &p->header));
    }

    GameDevice& dev_;
    Native n_;
    HMODULE module_ = nullptr;
    ffxFunctions api_{};
    ffxContext context_ = nullptr;
    FrameGeneratorCreateInfo info_{};
    std::string provider_;
    std::atomic<std::uint64_t> resetFrame_{0};  // the frame whose interpolation resets (callback model), 0 = none
    void* cfgSwapchain_ = nullptr;              // callback model: what the last configure attached (destroyContext detaches it)
    const void* cfgPresent_ = nullptr;
    void* cfgPresentUser_ = nullptr;
};

}  // namespace

bool inPresentCallback() { return gInCallback; }
std::unique_ptr<Upscaler> createUpscaler(GameDevice& device) { return std::make_unique<DxUpscaler>(device); }
std::unique_ptr<FrameGenerator> createFrameGenerator(GameDevice& device) { return std::make_unique<DxFrameGenerator>(device); }
std::unique_ptr<FgSwapchain> FgSwapchain::make(GameDevice& device) { return std::make_unique<FgSwapchainImpl>(device); }

FrameGenProbe probeFrameGeneration(GameDevice& device, const std::filesystem::path& loaderDll) {
    FrameGenProbe r;
    std::error_code ec;
    const std::filesystem::path effect = loaderDll.parent_path() / "amd_fidelityfx_framegeneration_dx12.dll";
    r.dllPresent = std::filesystem::exists(effect, ec);
    if (!r.dllPresent) {
        r.reason = "amd_fidelityfx_framegeneration_dx12.dll is not installed";
        return r;
    }
    if (!std::filesystem::exists(loaderDll, ec)) {
        r.reason = "amd_fidelityfx_loader_dx12.dll is missing";
        return r;
    }
    const auto t0 = std::chrono::steady_clock::now();
    ffxFunctions api{};
    HMODULE m = loadModule(loaderDll, api, "framegen");
    if (m == nullptr) {
        r.reason = "the FidelityFX loader does not load";
        return r;
    }
    const Native n = nativeOf(device);
    const std::vector<vk::FrameGenVersion> list = queryVersions(api, n.device, FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATION);
    const std::vector<vk::FrameGenVersion> sc = queryVersions(api, n.device, FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATIONSWAPCHAIN_FOR_HWND_DX12);
    FreeLibrary(m);
    r.probed = true;
    r.versions = vk::describeVersions(list);
    const vk::FrameGenVersion* pinned = vk::pickSwapchainProvider(sc);
    r.swapchain = vk::describeVersions(sc) + (pinned != nullptr ? std::format(" (pinned: {})", pinned->name) : std::string(" (none usable)"));
    r.fsr3 = vk::pickFrameGenProvider(list, false) != nullptr;
    r.ml = vk::pickFrameGenProvider(list, true) != nullptr;
    if (!r.fsr3) r.reason = list.empty() ? "the API offers no frame generation provider on this device" : "the API offers no FSR 3 frame generation provider";
    spdlog::info("framegen: probe (D3D12): providers [{}], swapchain providers [{}] ({:.0f} ms)", r.versions, r.swapchain,
                 std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count());
    return r;
}

}  // namespace rvk::rhi::d3d12ffx
