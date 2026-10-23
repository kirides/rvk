#include "vk/fsr.h"

#include <format>

#include <spdlog/spdlog.h>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <ffx_api/ffx_api.h>
#include <ffx_api/ffx_api_loader.h>
#include <ffx_api/ffx_upscale.h>
#include <ffx_api/vk/ffx_api_vk.h>

#include "vk/context.h"

namespace rvk::vk {

struct Fsr::Functions {
    ffxFunctions api{};
};

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
        spdlog::error("fsr: {}", narrow(message));
    } else {
        spdlog::warn("fsr: {}", narrow(message));
    }
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
    if (isDepthFormat(d.format)) r.usage |= FFX_API_RESOURCE_USAGE_DEPTHTARGET;
    if (hasStencil(d.format)) r.usage |= FFX_API_RESOURCE_USAGE_STENCILTARGET;
    if ((d.usage & VK_IMAGE_USAGE_STORAGE_BIT) != 0) r.usage |= FFX_API_RESOURCE_USAGE_UAV;
    return r;
}

FfxApiResource resource(const Image* img, std::uint32_t state) {
    if (img == nullptr) return {};
    return ffxApiGetResourceVK(reinterpret_cast<void*>(img->image()), describe(*img), state);
}

VkImageLayout layoutOf(std::uint32_t state) {
    return state == FFX_API_RESOURCE_STATE_UNORDERED_ACCESS ? VK_IMAGE_LAYOUT_GENERAL : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
}

}  // namespace

bool Fsr::load(const std::filesystem::path& dll) {
    if (module_ != nullptr) return true;
    HMODULE m = LoadLibraryExW(dll.c_str(), nullptr, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
    if (m == nullptr) {
        spdlog::warn("fsr: cannot load {} (error {})", dll.string(), GetLastError());
        return false;
    }
    auto* f = new Functions;
    ffxLoadFunctions(&f->api, m);
    if (f->api.CreateContext == nullptr || f->api.DestroyContext == nullptr || f->api.Configure == nullptr ||
        f->api.Query == nullptr || f->api.Dispatch == nullptr) {
        spdlog::warn("fsr: {} is not an FFX API DLL (entry points missing)", dll.string());
        delete f;
        FreeLibrary(m);
        return false;
    }
    module_ = m;
    fn_ = f;
    spdlog::info("fsr: loaded {}", dll.string());
    return true;
}

bool Fsr::createContext(Context& ctx, const FsrCreateInfo& info) {
    if (module_ == nullptr) return false;
    destroyContext();

    ffxCreateBackendVKDesc backend{};
    backend.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_BACKEND_VK;
    backend.vkDevice = ctx.device();
    backend.vkPhysicalDevice = ctx.physicalDevice();
    backend.vkDeviceProcAddr = vkGetDeviceProcAddr;

    ffxCreateContextDescUpscale create{};
    create.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE;
    create.header.pNext = &backend.header;
    create.flags = (info.hdr ? FFX_UPSCALE_ENABLE_HIGH_DYNAMIC_RANGE : 0) |
                   (info.motionJitterCancelled ? FFX_UPSCALE_ENABLE_MOTION_VECTORS_JITTER_CANCELLATION : 0) |
                   (info.invertedDepth ? FFX_UPSCALE_ENABLE_DEPTH_INVERTED : 0) |
                   (info.infiniteDepth ? FFX_UPSCALE_ENABLE_DEPTH_INFINITE : 0) |
                   (info.autoExposure ? FFX_UPSCALE_ENABLE_AUTO_EXPOSURE : 0) |
                   (info.debugChecking ? FFX_UPSCALE_ENABLE_DEBUG_CHECKING : 0) |
                   (info.nonLinearColor ? FFX_UPSCALE_ENABLE_NON_LINEAR_COLORSPACE : 0);
    create.maxRenderSize = {info.maxRenderSize.width, info.maxRenderSize.height};
    create.maxUpscaleSize = {info.maxUpscaleSize.width, info.maxUpscaleSize.height};
    create.fpMessage = ffxMessage;

    ffxContext c = nullptr;
    const ffxReturnCode_t rc = fn_->api.CreateContext(&c, &create.header, nullptr);
    if (rc != FFX_API_RETURN_OK || c == nullptr) {
        spdlog::error("fsr: ffxCreateContext failed ({})", rc);
        return false;
    }
    context_ = c;

    ffxQueryGetProviderVersion q{};
    q.header.type = FFX_API_QUERY_DESC_TYPE_GET_PROVIDER_VERSION;
    provider_.clear();
    if (fn_->api.Query(&c, &q.header) == FFX_API_RETURN_OK && q.versionName != nullptr) provider_ = q.versionName;
    spdlog::info("fsr: context {}x{} -> {}x{} flags 0x{:x}, provider '{}'", info.maxRenderSize.width, info.maxRenderSize.height,
                 info.maxUpscaleSize.width, info.maxUpscaleSize.height, create.flags, provider_);
    return true;
}

void Fsr::destroyContext() {
    if (context_ != nullptr && fn_ != nullptr) {
        ffxContext c = context_;
        fn_->api.DestroyContext(&c, nullptr);
    }
    context_ = nullptr;
    provider_.clear();
}

void Fsr::destroy() {
    destroyContext();
    delete fn_;
    fn_ = nullptr;
    if (module_ != nullptr) {
        FreeLibrary(static_cast<HMODULE>(module_));
        module_ = nullptr;
    }
}

bool Fsr::dispatch(const FsrDispatch& d) {
    if (context_ == nullptr || d.color == nullptr || d.depth == nullptr || d.motion == nullptr || d.output == nullptr) return false;
    constexpr std::uint32_t kRead = FFX_API_RESOURCE_STATE_COMPUTE_READ;
    constexpr std::uint32_t kWrite = FFX_API_RESOURCE_STATE_UNORDERED_ACCESS;
    for (Image* in : {d.color, d.depth, d.motion, d.reactive, d.transparency}) {
        if (in != nullptr) in->transition(d.cmd, layoutOf(kRead));
    }
    d.output->transition(d.cmd, layoutOf(kWrite));

    ffxDispatchDescUpscale u{};
    u.header.type = FFX_API_DISPATCH_DESC_TYPE_UPSCALE;
    u.commandList = d.cmd;
    u.color = resource(d.color, kRead);
    u.depth = resource(d.depth, kRead);
    u.motionVectors = resource(d.motion, kRead);
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
    const ffxReturnCode_t rc = fn_->api.Dispatch(&c, &u.header);
    // the DLL's backend leaves every resource in the state it was given
    for (Image* in : {d.color, d.depth, d.motion, d.reactive, d.transparency}) {
        if (in != nullptr) in->setTrackedLayout(layoutOf(kRead));
    }
    d.output->setTrackedLayout(layoutOf(kWrite));
    if (rc != FFX_API_RETURN_OK) {
        spdlog::error("fsr: ffxDispatch failed ({})", rc);
        return false;
    }
    return true;
}

int Fsr::jitterPhaseCount(std::uint32_t renderWidth, std::uint32_t displayWidth) {
    if (context_ == nullptr) return 0;
    std::int32_t count = 0;
    ffxQueryDescUpscaleGetJitterPhaseCount q{};
    q.header.type = FFX_API_QUERY_DESC_TYPE_UPSCALE_GETJITTERPHASECOUNT;
    q.renderWidth = renderWidth;
    q.displayWidth = displayWidth;
    q.pOutPhaseCount = &count;
    ffxContext c = context_;
    return fn_->api.Query(&c, &q.header) == FFX_API_RETURN_OK ? count : 0;
}

bool Fsr::jitterOffset(int index, int phaseCount, float& x, float& y) {
    if (context_ == nullptr) return false;
    ffxQueryDescUpscaleGetJitterOffset q{};
    q.header.type = FFX_API_QUERY_DESC_TYPE_UPSCALE_GETJITTEROFFSET;
    q.index = index;
    q.phaseCount = phaseCount;
    q.pOutX = &x;
    q.pOutY = &y;
    ffxContext c = context_;
    return fn_->api.Query(&c, &q.header) == FFX_API_RETURN_OK;
}

bool Fsr::renderSizeForMode(VkExtent2D display, std::uint32_t mode, VkExtent2D& render) {
    if (context_ == nullptr) return false;
    ffxQueryDescUpscaleGetRenderResolutionFromQualityMode q{};
    q.header.type = FFX_API_QUERY_DESC_TYPE_UPSCALE_GETRENDERRESOLUTIONFROMQUALITYMODE;
    q.displayWidth = display.width;
    q.displayHeight = display.height;
    q.qualityMode = mode;
    q.pOutRenderWidth = &render.width;
    q.pOutRenderHeight = &render.height;
    ffxContext c = context_;
    return fn_->api.Query(&c, &q.header) == FFX_API_RETURN_OK;
}

std::string Fsr::versions(Context& ctx) {
    if (module_ == nullptr) return {};
    std::uint64_t count = 0;
    ffxQueryDescGetVersions q{};
    q.header.type = FFX_API_QUERY_DESC_TYPE_GET_VERSIONS;
    q.createDescType = FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE;
    q.device = ctx.device();
    q.outputCount = &count;
    if (fn_->api.Query(nullptr, &q.header) != FFX_API_RETURN_OK || count == 0) return {};
    std::vector<std::uint64_t> ids(count);
    std::vector<const char*> names(count);
    q.outputCount = &count;
    q.versionIds = ids.data();
    q.versionNames = names.data();
    if (fn_->api.Query(nullptr, &q.header) != FFX_API_RETURN_OK) return {};
    std::string s;
    for (std::uint64_t i = 0; i < count; ++i) {
        s += std::format("{}{}: {}", i == 0 ? "" : ", ", ids[i], names[i] != nullptr ? names[i] : "?");
    }
    return s;
}

}  // namespace rvk::vk
