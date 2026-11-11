#include "vk/fg_swapchain.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>
#include <format>
#include <mutex>
#include <set>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>

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

#include "core/iat_patch.h"
#include "core/stall_sampler.h"
#include "vk/context.h"
#include "vk/fg_trace.h"
#include "vk/framegen.h"

namespace rvk::vk {

struct FgSwapchain::Functions {
    ffxFunctions api{};
    ffxQueryDescSwapchainReplacementFunctionsVK rf{};  // filled after the swapchain context exists
};

// the callbacks the API calls (static: C function pointers)
struct FgCallbacks {
    static ffxReturnCode_t present(ffxCallbackDescFrameGenerationPresent* params, void* user) {
        return static_cast<ffxReturnCode_t>(static_cast<FgSwapchain*>(user)->onPresent(params));
    }
    // the swapchain's submits on the game queue go through the queue lock (the render thread submits on it too)
    static Context* ctx;
    static VkResult submit(uint32_t count, const VkSubmitInfo* submits, VkFence fence) {
        return ctx != nullptr ? ctx->queueSubmit(count, submits, fence) : VK_ERROR_DEVICE_LOST;
    }
};
Context* FgCallbacks::ctx = nullptr;

VkImageLayout fgLayoutOfState(std::uint32_t state) {
    switch (state) {
        case FFX_API_RESOURCE_STATE_UNORDERED_ACCESS: return VK_IMAGE_LAYOUT_GENERAL;
        case FFX_API_RESOURCE_STATE_COMPUTE_READ:
        case FFX_API_RESOURCE_STATE_PIXEL_READ:
        case FFX_API_RESOURCE_STATE_PIXEL_COMPUTE_READ: return VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        case FFX_API_RESOURCE_STATE_COPY_SRC: return VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        case FFX_API_RESOURCE_STATE_COPY_DEST: return VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        case FFX_API_RESOURCE_STATE_PRESENT: return VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
        case FFX_API_RESOURCE_STATE_RENDER_TARGET: return VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        default: return VK_IMAGE_LAYOUT_GENERAL;  // COMMON, GENERIC_READ
    }
}

FgGapStats fgGapStats(const std::vector<double>& gapsMs, double stallMs) {
    FgGapStats r;
    std::vector<double> v;
    std::vector<double> parity[2];
    for (std::size_t i = 0; i < gapsMs.size(); ++i) {
        if (gapsMs[i] > stallMs) {
            ++r.stalls;
            continue;
        }
        v.push_back(gapsMs[i]);
        parity[i & 1].push_back(gapsMs[i]);
    }
    const auto moments = [](const std::vector<double>& x, double& mean, double& sd) {
        mean = sd = 0.0;
        if (x.empty()) return;
        for (double a : x) mean += a;
        mean /= static_cast<double>(x.size());
        for (double a : x) sd += (a - mean) * (a - mean);
        sd = std::sqrt(sd / static_cast<double>(x.size()));
    };
    r.n = v.size();
    if (v.empty()) return r;
    moments(v, r.mean, r.stddev);
    moments(parity[0], r.evenMean, r.evenStd);
    moments(parity[1], r.oddMean, r.oddStd);
    std::sort(v.begin(), v.end());
    const auto at = [&](double q) { return v[std::min(v.size() - 1, static_cast<std::size_t>(q * static_cast<double>(v.size())))]; };
    r.p1 = at(0.01);
    r.p50 = at(0.5);
    r.p99 = at(0.99);
    r.max = v.back();
    return r;
}

namespace {

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
        spdlog::error("fg swapchain: {}", narrow(message));
    } else {
        spdlog::warn("fg swapchain: {}", narrow(message));
    }
}

// ---- AMD's named events (frame-generation.md 13, FG-1) ----
// The swapchain of amd_fidelityfx_framegeneration_vk.dll (SDK 2.4.0, FrameInterpolationSwapchainVK.cpp, init) creates its three
// auto-reset events with NAMES: CreateEvent(NULL, FALSE, .., "PresentEvent" / "InterpolationEvent" / "PacerEvent"). A name puts the
// event into the session's namespace, so every process using that swapchain (a second Risen with frame generation, any other
// Vulkan FSR 3 game) opens the SAME three kernel events: the other process's presenter / interpolation threads wait on them and
// steal our SetEvent (auto-reset: one waiter wakes), our presenter thread sleeps forever and our present thread waits without end
// for the replacement buffer that presenter would have released (the "stall"). The names serve nothing (the DLL uses the handles):
// the DLL's import of CreateEventW is pointed at a wrapper that creates every event unnamed (process-private).
// RVK_FG_SHARED_EVENTS=1 (Win32 environment) keeps the names: the A/B arm.
std::atomic<int> g_eventsPrivatized{0};
decltype(&CreateEventW) g_createEventW = nullptr;

HANDLE WINAPI privateCreateEventW(LPSECURITY_ATTRIBUTES sa, BOOL manualReset, BOOL initialState, LPCWSTR name) {
    if (name != nullptr) {
        name = nullptr;
        g_eventsPrivatized.fetch_add(1);
    }
    return g_createEventW(sa, manualReset, initialState, name);
}

bool sharedEventsWanted() {
    char buf[8] = {};
    const DWORD n = GetEnvironmentVariableA("RVK_FG_SHARED_EVENTS", buf, sizeof(buf));
    return n > 0 && buf[0] != '0';
}

// ---- AMD's semaphores (frame-ownership.md 14.26) ----
// The swapchain of amd_fidelityfx_framegeneration_vk.dll (SDK 2.4.0) can leave its "AMD FSR FrameRenderedSemaphore <n>" semaphores alive when it is destroyed (two in
// some sessions: the validation layer's leaked-objects report at vkDestroyDevice). Its swapchain part imports vkCreateSemaphore / vkDestroySemaphore /
// vkGetDeviceProcAddr from vulkan-1.dll (FgSwapchain::load points those imports at the trackers below; the device proc address the contexts get serves the effect's
// backend table and is the tracker too: vk/framegen.cpp takes fgTrackedDeviceProcAddr). The trackers record the semaphores the DLL creates and the names it gives them
// (vkSetDebugUtilsObjectNameEXT through the tracked proc lookups). Once the swapchain context and the swapchain are destroyed, a semaphore named "AMD FSR
// FrameRenderedSemaphore ..." that is still alive belongs to no live swapchain and is destroyed by us (any other live semaphore is left alone: the generator's own
// objects); a later vkDestroySemaphore of the DLL on such a handle is ignored (no double destroy). RVK_FG_KEEP_SEMAPHORES=1 (Win32 environment) leaves them to the
// DLL: the A/B arm.
std::mutex g_semMutex;
std::unordered_map<VkSemaphore, std::string> g_semLive;  // created through the wrapper, not destroyed yet -> its debug name ("" = none)
std::unordered_set<VkSemaphore> g_semReclaimed;          // destroyed by us after the swapchain went
PFN_vkCreateSemaphore g_vkCreateSemaphore = nullptr;
PFN_vkDestroySemaphore g_vkDestroySemaphore = nullptr;
PFN_vkSetDebugUtilsObjectNameEXT g_vkSetName = nullptr;
std::atomic<std::uint64_t> g_semCreated{0}, g_semDestroyed{0}, g_procLookups{0};  // (through the wrapper, this process)
constexpr std::string_view kSwapchainSemaphore = "AMD FSR FrameRenderedSemaphore";
std::set<std::string> g_procNames;  // (the names the DLL looked up: diagnostics)

VKAPI_ATTR VkResult VKAPI_CALL trackedCreateSemaphore(VkDevice device, const VkSemaphoreCreateInfo* ci, const VkAllocationCallbacks* a, VkSemaphore* out) {
    const VkResult r = g_vkCreateSemaphore(device, ci, a, out);
    if (r == VK_SUCCESS && out != nullptr && *out != VK_NULL_HANDLE) {
        const std::lock_guard lock(g_semMutex);
        g_semLive[*out].clear();
        g_semReclaimed.erase(*out);
        ++g_semCreated;
    }
    return r;
}

VKAPI_ATTR void VKAPI_CALL trackedDestroySemaphore(VkDevice device, VkSemaphore sem, const VkAllocationCallbacks* a) {
    {
        const std::lock_guard lock(g_semMutex);
        if (g_semReclaimed.erase(sem) != 0) return;  // (already destroyed by reclaimSemaphores)
        if (g_semLive.erase(sem) != 0) ++g_semDestroyed;
    }
    g_vkDestroySemaphore(device, sem, a);
}

VKAPI_ATTR VkResult VKAPI_CALL trackedSetName(VkDevice device, const VkDebugUtilsObjectNameInfoEXT* info) {
    if (info != nullptr && info->objectType == VK_OBJECT_TYPE_SEMAPHORE && info->pObjectName != nullptr) {
        const std::lock_guard lock(g_semMutex);
        const auto it = g_semLive.find(reinterpret_cast<VkSemaphore>(info->objectHandle));
        if (it != g_semLive.end()) it->second = info->pObjectName;
    }
    return g_vkSetName != nullptr ? g_vkSetName(device, info) : VK_SUCCESS;
}

VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL trackedDeviceProcAddr(VkDevice device, const char* name);

VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL trackedDeviceProcAddr(VkDevice device, const char* name) {
    ++g_procLookups;
    if (name == nullptr) return vkGetDeviceProcAddr(device, name);
    {
        const std::lock_guard lock(g_semMutex);
        g_procNames.insert(name);
    }
    if (std::strcmp(name, "vkGetDeviceProcAddr") == 0) return reinterpret_cast<PFN_vkVoidFunction>(&trackedDeviceProcAddr);  // (the DLL's later lookups stay ours)
    if (std::strcmp(name, "vkCreateSemaphore") == 0) {
        if (g_vkCreateSemaphore == nullptr) g_vkCreateSemaphore = reinterpret_cast<PFN_vkCreateSemaphore>(vkGetDeviceProcAddr(device, name));
        return g_vkCreateSemaphore != nullptr ? reinterpret_cast<PFN_vkVoidFunction>(&trackedCreateSemaphore) : nullptr;
    }
    if (std::strcmp(name, "vkDestroySemaphore") == 0) {
        if (g_vkDestroySemaphore == nullptr) g_vkDestroySemaphore = reinterpret_cast<PFN_vkDestroySemaphore>(vkGetDeviceProcAddr(device, name));
        return g_vkDestroySemaphore != nullptr ? reinterpret_cast<PFN_vkVoidFunction>(&trackedDestroySemaphore) : nullptr;
    }
    if (std::strcmp(name, "vkSetDebugUtilsObjectNameEXT") == 0) {
        if (g_vkSetName == nullptr) g_vkSetName = reinterpret_cast<PFN_vkSetDebugUtilsObjectNameEXT>(vkGetDeviceProcAddr(device, name));
        return g_vkSetName != nullptr ? reinterpret_cast<PFN_vkVoidFunction>(&trackedSetName) : nullptr;
    }
    return vkGetDeviceProcAddr(device, name);
}

bool keepSemaphoresWanted() {
    char buf[8] = {};
    const DWORD n = GetEnvironmentVariableA("RVK_FG_KEEP_SEMAPHORES", buf, sizeof(buf));
    return n > 0 && buf[0] != '0';
}

// after the swapchain context and the swapchain are destroyed: the swapchain's semaphores the DLL left alive
void reclaimSemaphores(Context& ctx) {
    std::vector<VkSemaphore> left;
    std::size_t others = 0;
    {
        const std::lock_guard lock(g_semMutex);
        for (auto it = g_semLive.begin(); it != g_semLive.end();) {
            if (std::string_view(it->second).starts_with(kSwapchainSemaphore)) {
                left.push_back(it->first);
                it = g_semLive.erase(it);
            } else {
                ++others;
                ++it;
            }
        }
    }
    std::size_t names = 0;
    bool askedCreate = false;
    {
        const std::lock_guard lock(g_semMutex);
        names = g_procNames.size();
        askedCreate = g_procNames.contains("vkCreateSemaphore");
        std::string all;
        for (const std::string& n : g_procNames) all += (all.empty() ? "" : " ") + n;
        spdlog::info("fg swapchain: names the DLL looked up: {}", all);
    }
    spdlog::info("fg swapchain: semaphores of the DLL: {} created, {} destroyed by it, {} of the swapchain left alive after it, {} others alive; {} proc lookups of {} "
                 "names (vkCreateSemaphore {})",
                 g_semCreated.load(), g_semDestroyed.load(), left.size(), others, g_procLookups.load(), names, askedCreate ? "asked" : "never asked");
    if (left.empty()) return;
    if (keepSemaphoresWanted() || g_vkDestroySemaphore == nullptr) {
        spdlog::info("fg swapchain: {} semaphores of the swapchain left alive by the DLL (RVK_FG_KEEP_SEMAPHORES)", left.size());
        return;
    }
    // (the swapchain's destroy waited for its presents and its queues; the callers waited the device idle before: Swapchain::create / releaseFrameGen / destroy)
    {
        const std::lock_guard lock(g_semMutex);
        for (const VkSemaphore s : left) g_semReclaimed.insert(s);
    }
    for (const VkSemaphore s : left) g_vkDestroySemaphore(ctx.device(), s, nullptr);
    spdlog::info("fg swapchain: {} semaphores of the swapchain destroyed", left.size());
}

void imageBarrier(VkCommandBuffer cmd, VkImage image, VkImageLayout from, VkImageLayout to, VkPipelineStageFlags2 dstStage,
                  VkAccessFlags2 dstAccess) {
    VkImageMemoryBarrier2 b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
    b.srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    b.srcAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT;
    b.dstStageMask = dstStage;
    b.dstAccessMask = dstAccess;
    b.oldLayout = from;
    b.newLayout = to;
    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = image;
    b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    VkDependencyInfo di{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    di.imageMemoryBarrierCount = 1;
    di.pImageMemoryBarriers = &b;
    vkCmdPipelineBarrier2(cmd, &di);
}

constexpr VkAccessFlags2 kAnyAccess = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT;

}  // namespace

PFN_vkGetDeviceProcAddr fgTrackedDeviceProcAddr() { return &trackedDeviceProcAddr; }

FgSwapchain::~FgSwapchain() {
    stopSampler();
    destroy();
    delete fn_;
    fn_ = nullptr;
    if (module_ != nullptr) {
        FreeLibrary(static_cast<HMODULE>(module_));
        module_ = nullptr;
    }
    if (effect_ != nullptr) {
        FreeLibrary(static_cast<HMODULE>(effect_));
        effect_ = nullptr;
    }
}

int FgSwapchain::privatizedEvents() { return g_eventsPrivatized.load(); }

bool FgSwapchain::eventsPrivate(std::string* why) const {
    const auto fail = [&](std::string w) {
        if (why != nullptr) *why = std::move(w);
        return false;
    };
    if (effect_ == nullptr) return fail("the frame generation DLL is not loaded");
    void* target = iat::importTarget(effect_, "kernel32.dll", "CreateEventW");
    if (target == nullptr) return fail("the DLL imports no CreateEventW");
    if (target != reinterpret_cast<void*>(&privateCreateEventW)) return fail("its CreateEventW import is not the wrapper");
    // what the DLL's own call does: a name must not reach the kernel
    const std::wstring name = L"rvk_fg_event_probe_" + std::to_wstring(GetCurrentProcessId());
    HANDLE e = reinterpret_cast<decltype(&CreateEventW)>(target)(nullptr, FALSE, FALSE, name.c_str());
    if (e == nullptr) return fail("the wrapper made no event");
    HANDLE byName = OpenEventW(SYNCHRONIZE, FALSE, name.c_str());
    const bool named = byName != nullptr;
    if (byName != nullptr) CloseHandle(byName);
    CloseHandle(e);
    if (named) return fail("the event can be opened by its name");
    return true;
}

bool FgSwapchain::load(const std::filesystem::path& dll) {
    if (module_ != nullptr) return true;
    HMODULE m = LoadLibraryExW(dll.c_str(), nullptr, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
    if (m == nullptr) {
        spdlog::warn("fg swapchain: cannot load {} (error {})", dll.string(), GetLastError());
        return false;
    }
    auto* f = new Functions;
    ffxLoadFunctions(&f->api, m);
    if (f->api.CreateContext == nullptr || f->api.DestroyContext == nullptr || f->api.Configure == nullptr || f->api.Query == nullptr ||
        f->api.Dispatch == nullptr) {
        spdlog::warn("fg swapchain: {} is not an FFX API DLL (entry points missing)", dll.string());
        delete f;
        FreeLibrary(m);
        return false;
    }
    // the runtime's messages of this effect (AMD's own validation of our calls)
    ffxConfigureDescGlobalDebug dbg{};
    dbg.header.type = FFX_API_CONFIGURE_DESC_TYPE_GLOBALDEBUG;
    dbg.effectId = FFX_API_EFFECT_ID_FRAMEGENERATIONSWAPCHAIN_VK;
    dbg.fpMessage = ffxMessage;
    dbg.debugLevel = FFX_API_CONFIGURE_GLOBALDEBUG_LEVEL_WARNINGS;
    f->api.Configure(nullptr, &dbg.header);
    module_ = m;
    fn_ = f;
    // the effect DLL beside the loader, mapped now (the loader maps the same file when the first context is made): its imports are
    // patched before any of its code runs
    HMODULE fx = LoadLibraryExW((dll.parent_path() / L"amd_fidelityfx_framegeneration_vk.dll").c_str(), nullptr,
                                LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
    if (fx == nullptr) {
        spdlog::warn("fg swapchain: amd_fidelityfx_framegeneration_vk.dll does not load beside the loader (error {}): its events stay named",
                     GetLastError());
    } else {
        effect_ = fx;
        if (sharedEventsWanted()) {
            spdlog::warn("fg swapchain: RVK_FG_SHARED_EVENTS: AMD's swapchain events keep their session-wide names (A/B arm)");
        } else if (!iat::patchImport(fx, "kernel32.dll", "CreateEventW", reinterpret_cast<void*>(&privateCreateEventW),
                                     reinterpret_cast<void**>(&g_createEventW))) {
            spdlog::warn("fg swapchain: the frame generation DLL's CreateEventW import was not found: its events stay named");
        }
        // 14.26: the swapchain part of the DLL imports its Vulkan functions from vulkan-1.dll (the device proc address of the contexts serves only the effect's
        // backend table): its semaphore calls and its own proc lookups (the debug names) go through the trackers
        if (!keepSemaphoresWanted()) {
            int n = 0;
            n += iat::patchImport(fx, "vulkan-1.dll", "vkCreateSemaphore", reinterpret_cast<void*>(&trackedCreateSemaphore), reinterpret_cast<void**>(&g_vkCreateSemaphore)) ? 1 : 0;
            n += iat::patchImport(fx, "vulkan-1.dll", "vkDestroySemaphore", reinterpret_cast<void*>(&trackedDestroySemaphore), reinterpret_cast<void**>(&g_vkDestroySemaphore)) ? 1 : 0;
            PFN_vkGetDeviceProcAddr realGdpa = nullptr;
            n += iat::patchImport(fx, "vulkan-1.dll", "vkGetDeviceProcAddr", reinterpret_cast<void*>(&trackedDeviceProcAddr), reinterpret_cast<void**>(&realGdpa)) ? 1 : 0;
            spdlog::info("fg swapchain: {} of 3 vulkan-1 imports of the frame generation DLL tracked (its semaphores)", n);
        }
        if (fgtrace::enabled()) {
            fgtrace::install(fx, VK_NULL_HANDLE);
            diag::setHangReporter(&fgtrace::dump);
        }
    }
    return true;
}

bool FgSwapchain::create(Context& ctx, const VkSwapchainCreateInfoKHR& ci, std::string* reason) {
    const auto fail = [&](std::string why) {
        spdlog::warn("fg swapchain: not created: {}", why);
        if (reason != nullptr) *reason = std::move(why);
        return false;
    };
    if (module_ == nullptr) return fail("the FidelityFX loader is not loaded");
    if (swapchainContext_ != nullptr) return fail("already created");
    const FrameGenQueues& q = ctx.frameGenQueues();
    if (!q.created) return fail(q.reason.empty() ? "the extra queues do not exist" : q.reason);

    // the provider: pinned, never the driver's "4.0.0 *" (crashes in its destroy)
    VkDeviceContext dc{};
    dc.vkDevice = ctx.device();
    dc.vkPhysicalDevice = ctx.physicalDevice();
    dc.vkDeviceProcAddr = trackedDeviceProcAddr;  // (14.26: tracks the swapchain's semaphores, reclaimSemaphores)
    std::vector<FrameGenVersion> versions;
    {
        std::uint64_t count = 0;
        ffxQueryDescGetVersions qd{};
        qd.header.type = FFX_API_QUERY_DESC_TYPE_GET_VERSIONS;
        qd.createDescType = FFX_API_CREATE_CONTEXT_DESC_TYPE_FGSWAPCHAIN_VK;
        qd.device = &dc;
        qd.outputCount = &count;
        if (fn_->api.Query(nullptr, &qd.header) == FFX_API_RETURN_OK && count > 0) {
            std::vector<std::uint64_t> ids(count);
            std::vector<const char*> names(count);
            qd.outputCount = &count;
            qd.versionIds = ids.data();
            qd.versionNames = names.data();
            if (fn_->api.Query(nullptr, &qd.header) == FFX_API_RETURN_OK) {
                for (std::uint64_t i = 0; i < count && i < ids.size(); ++i) {
                    versions.push_back(parseFrameGenVersion(ids[i], names[i] != nullptr ? names[i] : ""));
                }
            }
        }
    }
    const FrameGenVersion* pin = pickSwapchainProvider(versions);
    if (pin == nullptr) return fail(std::format("the API offers no usable swapchain provider ({})", describeVersions(versions)));

    VkSwapchainKHR sc = VK_NULL_HANDLE;
    ffxCreateContextDescFrameGenerationSwapChainVK cs{};
    cs.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_FGSWAPCHAIN_VK;
    cs.physicalDevice = ctx.physicalDevice();
    cs.device = ctx.device();
    cs.swapchain = &sc;
    cs.allocator = nullptr;
    cs.createInfo = ci;
    cs.createInfo.pNext = nullptr;
    cs.createInfo.oldSwapchain = VK_NULL_HANDLE;
    FgCallbacks::ctx = &ctx;
    cs.gameQueue = {ctx.queue(), ctx.queueFamily(), &FgCallbacks::submit};
    cs.asyncComputeQueue = {q.async, q.asyncFamily, nullptr};
    cs.presentQueue = {q.present, q.presentFamily, nullptr};
    cs.imageAcquireQueue = {q.acquire, q.acquireFamily, nullptr};
    ffxCreateContextDescFrameGenerationSwapChainVersionVK ver{};
    ver.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATIONSWAPCHAIN_VERSION_VK;
    ver.version = FFX_FRAMEGENERATION_SWAPCHAIN_VK_VERSION;
    ffxOverrideVersion ovr{};
    ovr.header.type = FFX_API_DESC_TYPE_OVERRIDE_VERSION;
    ovr.versionId = pin->id;
    cs.header.pNext = &ver.header;
    ver.header.pNext = &ovr.header;
    ovr.header.pNext = nullptr;

    ffxContext c = nullptr;
    const ffxReturnCode_t rc = fn_->api.CreateContext(&c, &cs.header, nullptr);
    if (rc != FFX_API_RETURN_OK || c == nullptr || sc == VK_NULL_HANDLE) {
        return fail(std::format("ffxCreateContext(swapchain) returned {} (provider '{}')", rc, pin->name));
    }
    fn_->rf = {};
    fn_->rf.header.type = FFX_API_QUERY_DESC_TYPE_FGSWAPCHAIN_FUNCTIONS_VK;
    if (fn_->api.Query(&c, &fn_->rf.header) != FFX_API_RETURN_OK || fn_->rf.pOutQueuePresentKHR == nullptr ||
        fn_->rf.pOutAcquireNextImageKHR == nullptr || fn_->rf.pOutGetSwapchainImagesKHR == nullptr) {
        fn_->api.DestroyContext(&c, nullptr);
        return fail("the swapchain's replacement functions are not available");
    }
    ffxQueryGetProviderVersion pv{};
    pv.header.type = FFX_API_QUERY_DESC_TYPE_GET_PROVIDER_VERSION;
    provider_ = pin->name;
    if (fn_->api.Query(&c, &pv.header) == FFX_API_RETURN_OK && pv.versionName != nullptr) provider_ = pv.versionName;
    swapchainContext_ = c;
    swapchain_ = sc;
    ctx_ = &ctx;
    format_ = ci.imageFormat;
    spdlog::info("fg swapchain: created {}x{}, {} app-facing images, present mode {}, provider '{}' (offered: {}); named events made "
                 "process-private so far: {}",
                 ci.imageExtent.width, ci.imageExtent.height, ci.minImageCount, static_cast<int>(ci.presentMode), provider_,
                 describeVersions(versions), g_eventsPrivatized.load());
    return true;
}

VkResult FgSwapchain::recreate(const VkSwapchainCreateInfoKHR& ci) {
    if (swapchain_ == VK_NULL_HANDLE || fn_ == nullptr) return VK_ERROR_INITIALIZATION_FAILED;
    VkSwapchainCreateInfoKHR info = ci;
    info.pNext = nullptr;
    info.oldSwapchain = VK_NULL_HANDLE;
    waitForPresents();
    destroyViews();
    const std::lock_guard lock(mutex_);
    VkSwapchainKHR sc = swapchain_;
    const VkResult r = fn_->rf.pOutCreateSwapchainFFXAPI(ctx_->device(), &info, nullptr, &sc, reinterpret_cast<void*>(swapchain_));
    if (r == VK_SUCCESS) {
        swapchain_ = sc;
        format_ = ci.imageFormat;
    }
    return r;
}

void FgSwapchain::waitForPresents() {
    if (swapchainContext_ == nullptr || fn_ == nullptr) return;
    ffxDispatchDescFrameGenerationSwapChainWaitForPresentsVK w{};
    w.header.type = FFX_API_DISPATCH_DESC_TYPE_FGSWAPCHAIN_WAIT_FOR_PRESENTS_VK;
    ffxContext c = swapchainContext_;
    fn_->api.Dispatch(&c, &w.header);
}

void FgSwapchain::destroy() {
    if (swapchainContext_ == nullptr || fn_ == nullptr) {
        swapchain_ = VK_NULL_HANDLE;
        return;
    }
    stopSampler();
    spdlog::info("fg swapchain: destroying (waiting for the presents)");
    waitForPresents();
    destroyViews();
    {
        const std::lock_guard lock(mutex_);
        spdlog::info("fg swapchain: destroying the swapchain context");
        ffxContext c = swapchainContext_;
        fn_->api.DestroyContext(&c, nullptr);  // documented order: the swapchain context first ...
        swapchainContext_ = nullptr;
        fn_->rf.pOutDestroySwapchainFFXAPI(ctx_->device(), swapchain_, nullptr, nullptr);  // ... then the swapchain
        swapchain_ = VK_NULL_HANDLE;
    }
    if (ctx_ != nullptr) reclaimSemaphores(*ctx_);
    spdlog::info("fg swapchain: destroyed");
}

std::vector<VkImage> FgSwapchain::images() const {
    std::vector<VkImage> v;
    if (swapchain_ == VK_NULL_HANDLE) return v;
    std::uint32_t n = 0;
    fn_->rf.pOutGetSwapchainImagesKHR(ctx_->device(), swapchain_, &n, nullptr);
    v.resize(n);
    fn_->rf.pOutGetSwapchainImagesKHR(ctx_->device(), swapchain_, &n, v.data());
    v.resize(n);
    return v;
}

VkResult FgSwapchain::acquire(VkSemaphore signal, std::uint32_t& imageIndex) {
    const VkResult r = fn_->rf.pOutAcquireNextImageKHR(ctx_->device(), swapchain_, UINT64_MAX, signal, VK_NULL_HANDLE, &imageIndex);
    fgtrace::note("appAcquire", imageIndex, static_cast<std::uint64_t>(static_cast<std::int64_t>(r)));
    return r;
}

VkResult FgSwapchain::present(VkQueue queue, const VkPresentInfoKHR& info) {
    // the interpolation dispatch (frame generation callback) runs inside: the frame generation context must not be used meanwhile
    const std::lock_guard lock(mutex_);
    fgtrace::note("queuePresent>", info.pImageIndices != nullptr ? info.pImageIndices[0] : 0);
    const VkResult r = fn_->rf.pOutQueuePresentKHR(queue, &info);
    fgtrace::note("queuePresent<", static_cast<std::uint64_t>(static_cast<std::int64_t>(r)));
    return r;
}

std::uint64_t FgSwapchain::realPresentCount() const {
    if (swapchain_ == VK_NULL_HANDLE || fn_ == nullptr || fn_->rf.pOutGetLastPresentCountFFXAPI == nullptr) return 0;
    return fn_->rf.pOutGetLastPresentCountFFXAPI(swapchain_);
}

// ---- the UI / present callback --------------------------------------------------------------------------------------------------

const void* FgSwapchain::presentCallback() const { return reinterpret_cast<const void*>(&FgCallbacks::present); }

void FgSwapchain::setUi(FgUiHook hook) {
    const std::lock_guard lock(uiMutex_);
    ui_ = std::move(hook);
}

VkImageView FgSwapchain::viewOf(VkImage image, VkFormat format) {
    const std::lock_guard lock(counterMutex_);
    if (const auto it = views_.find(image); it != views_.end()) return it->second;
    VkImageViewCreateInfo vci{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    vci.image = image;
    vci.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vci.format = format;
    vci.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    VkImageView v = VK_NULL_HANDLE;
    if (vkCreateImageView(ctx_->device(), &vci, nullptr, &v) != VK_SUCCESS) return VK_NULL_HANDLE;
    views_[image] = v;
    return v;
}

void FgSwapchain::destroyViews() {
    const std::lock_guard lock(counterMutex_);
    for (auto& [image, view] : views_) vkDestroyImageView(ctx_->device(), view, nullptr);
    views_.clear();
}

// Copies the frame to show (the real or the generated one) into the real swapchain image and draws the UI on top: the SDK's
// default UI composition does the same copy (ffxFrameInterpolationUiComposition). Barriers take the image states the API reports.
long FgSwapchain::onPresent(void* paramsPtr) {
    const auto t0 = std::chrono::steady_clock::now();
    auto* p = static_cast<ffxCallbackDescFrameGenerationPresent*>(paramsPtr);
    VkCommandBuffer cmd = static_cast<VkCommandBuffer>(p->commandList);
    VkImage src = static_cast<VkImage>(p->currentBackBuffer.resource);
    VkImage dst = static_cast<VkImage>(p->outputSwapChainBuffer.resource);
    if (cmd == VK_NULL_HANDLE || src == VK_NULL_HANDLE || dst == VK_NULL_HANDLE) return FFX_API_RETURN_ERROR_PARAMETER;
    const VkImageLayout srcLayout = fgLayoutOfState(p->currentBackBuffer.state);
    const VkImageLayout dstLayout = fgLayoutOfState(p->outputSwapChainBuffer.state);
    const VkExtent2D extent{p->outputSwapChainBuffer.description.width, p->outputSwapChainBuffer.description.height};
    const VkExtent2D srcExtent{p->currentBackBuffer.description.width, p->currentBackBuffer.description.height};

    bool ui = false;
    {
        const std::lock_guard lock(uiMutex_);
        ui = ui_.wants && ui_.record && ui_.wants();
    }

    imageBarrier(cmd, src, srcLayout, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_READ_BIT);
    imageBarrier(cmd, dst, dstLayout, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT);
    if (srcExtent.width == extent.width && srcExtent.height == extent.height) {
        const VkImageCopy region{{VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, {0, 0, 0}, {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, {0, 0, 0},
                                 {extent.width, extent.height, 1}};
        vkCmdCopyImage(cmd, src, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, dst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
    } else {
        VkImageBlit2 region{VK_STRUCTURE_TYPE_IMAGE_BLIT_2};
        region.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        region.srcOffsets[1] = {static_cast<int32_t>(srcExtent.width), static_cast<int32_t>(srcExtent.height), 1};
        region.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        region.dstOffsets[1] = {static_cast<int32_t>(extent.width), static_cast<int32_t>(extent.height), 1};
        VkBlitImageInfo2 bi{VK_STRUCTURE_TYPE_BLIT_IMAGE_INFO_2};
        bi.srcImage = src;
        bi.srcImageLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        bi.dstImage = dst;
        bi.dstImageLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        bi.regionCount = 1;
        bi.pRegions = &region;
        bi.filter = VK_FILTER_LINEAR;
        vkCmdBlitImage2(cmd, &bi);
    }
    if (ui) {
        VkImageView view = viewOf(dst, format_);
        if (view != VK_NULL_HANDLE) {
            imageBarrier(cmd, dst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                         VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                         VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT);
            VkRenderingAttachmentInfo att{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
            att.imageView = view;
            att.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
            att.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
            att.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
            VkRenderingInfo ri{VK_STRUCTURE_TYPE_RENDERING_INFO};
            ri.renderArea = {{0, 0}, extent};
            ri.layerCount = 1;
            ri.colorAttachmentCount = 1;
            ri.pColorAttachments = &att;
            vkCmdBeginRendering(cmd, &ri);
            {
                const std::lock_guard lock(uiMutex_);
                if (ui_.record) ui_.record(cmd, view, extent);
            }
            vkCmdEndRendering(cmd);
            imageBarrier(cmd, dst, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, dstLayout, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, kAnyAccess);
        } else {
            ui = false;
        }
    }
    if (!ui) imageBarrier(cmd, dst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, dstLayout, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, kAnyAccess);
    imageBarrier(cmd, src, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, srcLayout, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, kAnyAccess);

    const std::lock_guard lock(counterMutex_);
    ++counters_.callbacks;
    if (p->isGeneratedFrame) ++counters_.generatedFrames;
    if (ui) ++counters_.uiFrames;
    counters_.callbackMs += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    return FFX_API_RETURN_OK;
}

FgSwapchain::Counters FgSwapchain::takeCounters() {
    const std::lock_guard lock(counterMutex_);
    Counters c = counters_;
    counters_ = {};
    return c;
}

// ---- diagnostics ------------------------------------------------------------------------------------------------------------------

void FgSwapchain::startSampler() {
    if (sampling_.exchange(true) || swapchain_ == VK_NULL_HANDLE) return;
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

void FgSwapchain::stopSampler() {
    sampling_ = false;
    if (sampler_.joinable()) sampler_.join();
}

std::vector<FgPresentSample> FgSwapchain::takeSamples() {
    const std::lock_guard lock(sampleMutex_);
    std::vector<FgPresentSample> v;
    v.swap(samples_);
    return v;
}

}  // namespace rvk::vk
