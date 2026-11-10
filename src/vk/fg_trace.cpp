#include "vk/fg_trace.h"

#include <array>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <format>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <spdlog/spdlog.h>

#include "core/iat_patch.h"

namespace rvk::vk::fgtrace {

namespace {

bool envOn() {
    char buf[8] = {};
    const DWORD n = GetEnvironmentVariableA("RVK_FG_TRACE", buf, sizeof(buf));
    return n > 0 && buf[0] != '0';
}

// read on first use: run.ps1's -Environment values are set by the proxy after the static initialisers ran
bool on() {
    static const bool v = envOn();
    return v;
}

double nowMs() {
    static const auto t0 = std::chrono::steady_clock::now();
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
}

constexpr std::size_t kRing = 2048;
constexpr int kMaxSem = 6;

struct SemRef {
    VkSemaphore s = VK_NULL_HANDLE;
    std::uint64_t v = 0;
};

struct Event {
    double t = 0;
    DWORD thread = 0;
    const char* who = "";
    const char* kind = "";
    VkQueue queue = VK_NULL_HANDLE;
    int nWait = 0, nSignal = 0;
    SemRef wait[kMaxSem];
    SemRef signal[kMaxSem];
    std::uint64_t a = 0, b = 0;  // fence / image index / timeout / duration (us)
    int result = 0;
};

struct InWait {
    double since = 0;
    const char* what = "";
    SemRef sem;
    VkQueue queue = VK_NULL_HANDLE;
};

std::mutex g_mutex;
std::array<Event, kRing> g_ring;
std::uint64_t g_count = 0;
std::unordered_map<VkSemaphore, std::pair<bool, int>> g_sems;  // timeline?, id
std::unordered_map<VkQueue, std::string> g_queues;
std::unordered_map<DWORD, InWait> g_waits;
VkDevice g_device = VK_NULL_HANDLE;
int g_nextSem = 0;

// original entry points of the patched module
PFN_vkQueueSubmit o_submit = nullptr;
PFN_vkQueuePresentKHR o_present = nullptr;
PFN_vkAcquireNextImageKHR o_acquire = nullptr;
PFN_vkWaitSemaphores o_waitSems = nullptr;
PFN_vkQueueWaitIdle o_queueWaitIdle = nullptr;
PFN_vkDeviceWaitIdle o_deviceWaitIdle = nullptr;
PFN_vkCreateSemaphore o_createSem = nullptr;

int semId(VkSemaphore s) {
    auto it = g_sems.find(s);
    if (it != g_sems.end()) return it->second.second;
    const int id = g_nextSem++;
    g_sems[s] = {false, id};
    return id;
}

Event& push() {
    Event& e = g_ring[g_count % kRing];
    ++g_count;
    e = Event{};
    e.t = nowMs();
    e.thread = GetCurrentThreadId();
    return e;
}

void fillSubmit(Event& e, const VkSubmitInfo& si) {
    const VkTimelineSemaphoreSubmitInfo* tl = nullptr;
    for (auto* p = static_cast<const VkBaseInStructure*>(si.pNext); p != nullptr; p = p->pNext) {
        if (p->sType == VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO) tl = reinterpret_cast<const VkTimelineSemaphoreSubmitInfo*>(p);
    }
    for (std::uint32_t i = 0; i < si.waitSemaphoreCount && e.nWait < kMaxSem; ++i) {
        semId(si.pWaitSemaphores[i]);
        e.wait[e.nWait++] = {si.pWaitSemaphores[i], tl != nullptr && i < tl->waitSemaphoreValueCount && tl->pWaitSemaphoreValues ? tl->pWaitSemaphoreValues[i] : 0};
    }
    for (std::uint32_t i = 0; i < si.signalSemaphoreCount && e.nSignal < kMaxSem; ++i) {
        semId(si.pSignalSemaphores[i]);
        e.signal[e.nSignal++] = {si.pSignalSemaphores[i],
                                 tl != nullptr && i < tl->signalSemaphoreValueCount && tl->pSignalSemaphoreValues ? tl->pSignalSemaphoreValues[i] : 0};
    }
    e.b = si.commandBufferCount;
}

VKAPI_ATTR VkResult VKAPI_CALL h_submit(VkQueue queue, std::uint32_t count, const VkSubmitInfo* submits, VkFence fence) {
    const VkResult r = o_submit(queue, count, submits, fence);
    recordSubmit("amd", queue, count, submits, fence, r);
    return r;
}

VKAPI_ATTR VkResult VKAPI_CALL h_present(VkQueue queue, const VkPresentInfoKHR* info) {
    {
        const std::lock_guard lock(g_mutex);
        Event& e = push();
        e.who = "amd";
        e.kind = "present>";
        e.queue = queue;
        for (std::uint32_t i = 0; info != nullptr && i < info->waitSemaphoreCount && e.nWait < kMaxSem; ++i) {
            semId(info->pWaitSemaphores[i]);
            e.wait[e.nWait++] = {info->pWaitSemaphores[i], 0};
        }
        e.a = info != nullptr && info->pImageIndices != nullptr ? info->pImageIndices[0] : 0;
        g_waits[GetCurrentThreadId()] = {e.t, "vkQueuePresentKHR", {}, queue};
    }
    const double t0 = nowMs();
    const VkResult r = o_present(queue, info);
    const std::lock_guard lock(g_mutex);
    g_waits.erase(GetCurrentThreadId());
    Event& e = push();
    e.who = "amd";
    e.kind = "present<";
    e.queue = queue;
    e.b = static_cast<std::uint64_t>((nowMs() - t0) * 1000.0);
    e.result = r;
    return r;
}

VKAPI_ATTR VkResult VKAPI_CALL h_acquire(VkDevice device, VkSwapchainKHR sc, std::uint64_t timeout, VkSemaphore sem, VkFence fence, std::uint32_t* index) {
    {
        const std::lock_guard lock(g_mutex);
        g_waits[GetCurrentThreadId()] = {nowMs(), "vkAcquireNextImageKHR", {}, VK_NULL_HANDLE};
    }
    const double t0 = nowMs();
    const VkResult r = o_acquire(device, sc, timeout, sem, fence, index);
    const std::lock_guard lock(g_mutex);
    g_waits.erase(GetCurrentThreadId());
    Event& e = push();
    e.who = "amd";
    e.kind = "acquire";
    if (sem != VK_NULL_HANDLE) {
        semId(sem);
        e.signal[e.nSignal++] = {sem, 0};
    }
    e.a = index != nullptr ? *index : ~0ull;
    e.b = static_cast<std::uint64_t>((nowMs() - t0) * 1000.0);
    e.result = r;
    return r;
}

VKAPI_ATTR VkResult VKAPI_CALL h_waitSems(VkDevice device, const VkSemaphoreWaitInfo* info, std::uint64_t timeout) {
    SemRef ref;
    if (info != nullptr && info->semaphoreCount > 0) ref = {info->pSemaphores[0], info->pValues[0]};
    const bool longWait = timeout > 50'000'000ull;  // the 1 ms polls of a wait callback are not recorded one by one
    if (longWait) {
        const std::lock_guard lock(g_mutex);
        semId(ref.s);
        g_waits[GetCurrentThreadId()] = {nowMs(), "vkWaitSemaphores", ref, VK_NULL_HANDLE};
    }
    const double t0 = nowMs();
    const VkResult r = o_waitSems(device, info, timeout);
    const double ms = nowMs() - t0;
    if (longWait || ms > 2.0) {
        const std::lock_guard lock(g_mutex);
        if (longWait) g_waits.erase(GetCurrentThreadId());
        Event& e = push();
        e.who = "amd";
        e.kind = "waitSem";
        semId(ref.s);
        e.wait[e.nWait++] = ref;
        e.a = timeout;
        e.b = static_cast<std::uint64_t>(ms * 1000.0);
        e.result = r;
    }
    return r;
}

VKAPI_ATTR VkResult VKAPI_CALL h_queueWaitIdle(VkQueue queue) {
    {
        const std::lock_guard lock(g_mutex);
        g_waits[GetCurrentThreadId()] = {nowMs(), "vkQueueWaitIdle", {}, queue};
    }
    const double t0 = nowMs();
    const VkResult r = o_queueWaitIdle(queue);
    const std::lock_guard lock(g_mutex);
    g_waits.erase(GetCurrentThreadId());
    Event& e = push();
    e.who = "amd";
    e.kind = "queueWaitIdle";
    e.queue = queue;
    e.b = static_cast<std::uint64_t>((nowMs() - t0) * 1000.0);
    e.result = r;
    return r;
}

VKAPI_ATTR VkResult VKAPI_CALL h_deviceWaitIdle(VkDevice device) {
    {
        const std::lock_guard lock(g_mutex);
        g_waits[GetCurrentThreadId()] = {nowMs(), "vkDeviceWaitIdle", {}, VK_NULL_HANDLE};
    }
    const VkResult r = o_deviceWaitIdle(device);
    const std::lock_guard lock(g_mutex);
    g_waits.erase(GetCurrentThreadId());
    Event& e = push();
    e.who = "amd";
    e.kind = "deviceWaitIdle";
    e.result = r;
    return r;
}

VKAPI_ATTR VkResult VKAPI_CALL h_createSem(VkDevice device, const VkSemaphoreCreateInfo* ci, const VkAllocationCallbacks* alloc, VkSemaphore* out) {
    const VkResult r = o_createSem(device, ci, alloc, out);
    if (r == VK_SUCCESS && out != nullptr) {
        bool timeline = false;
        std::uint64_t initial = 0;
        for (auto* p = ci != nullptr ? static_cast<const VkBaseInStructure*>(ci->pNext) : nullptr; p != nullptr; p = p->pNext) {
            if (p->sType == VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO) {
                const auto* ti = reinterpret_cast<const VkSemaphoreTypeCreateInfo*>(p);
                timeline = ti->semaphoreType == VK_SEMAPHORE_TYPE_TIMELINE;
                initial = ti->initialValue;
            }
        }
        const std::lock_guard lock(g_mutex);
        g_sems[*out] = {timeline, g_nextSem++};
        Event& e = push();
        e.who = "amd";
        e.kind = timeline ? "createTimeline" : "createBinary";
        e.signal[e.nSignal++] = {*out, initial};
    }
    return r;
}

bool patch(HMODULE module, const char* name, void* hook, void** original) { return iat::patchImport(module, "vulkan-1.dll", name, hook, original); }

}  // namespace

bool enabled() { return on(); }

void install(void* module, VkDevice device) {
    if (!on() || module == nullptr) return;
    static std::atomic<void*> done{nullptr};
    if (done.exchange(module) == module) return;
    if (device != VK_NULL_HANDLE) g_device = device;
    auto* m = static_cast<HMODULE>(module);
    int n = 0;
    n += patch(m, "vkQueueSubmit", reinterpret_cast<void*>(&h_submit), reinterpret_cast<void**>(&o_submit));
    n += patch(m, "vkQueuePresentKHR", reinterpret_cast<void*>(&h_present), reinterpret_cast<void**>(&o_present));
    n += patch(m, "vkAcquireNextImageKHR", reinterpret_cast<void*>(&h_acquire), reinterpret_cast<void**>(&o_acquire));
    n += patch(m, "vkWaitSemaphores", reinterpret_cast<void*>(&h_waitSems), reinterpret_cast<void**>(&o_waitSems));
    n += patch(m, "vkQueueWaitIdle", reinterpret_cast<void*>(&h_queueWaitIdle), reinterpret_cast<void**>(&o_queueWaitIdle));
    n += patch(m, "vkDeviceWaitIdle", reinterpret_cast<void*>(&h_deviceWaitIdle), reinterpret_cast<void**>(&o_deviceWaitIdle));
    n += patch(m, "vkCreateSemaphore", reinterpret_cast<void*>(&h_createSem), reinterpret_cast<void**>(&o_createSem));
    spdlog::info("fgtrace: {} vulkan-1 imports of the frame generation DLL patched (RVK_FG_TRACE)", n);
}

void setDevice(VkDevice device) {
    if (on()) g_device = device;
}

void nameQueue(VkQueue queue, const char* name) {
    if (!on() || queue == VK_NULL_HANDLE) return;
    const std::lock_guard lock(g_mutex);
    g_queues[queue] = name;
}

void recordSubmit2(const char* who, VkQueue queue, const VkSubmitInfo2& si, VkFence fence, VkResult result) {
    if (!on()) return;
    const std::lock_guard lock(g_mutex);
    Event& e = push();
    e.who = who;
    e.kind = "submit2";
    e.queue = queue;
    for (std::uint32_t i = 0; i < si.waitSemaphoreInfoCount && e.nWait < kMaxSem; ++i) {
        semId(si.pWaitSemaphoreInfos[i].semaphore);
        e.wait[e.nWait++] = {si.pWaitSemaphoreInfos[i].semaphore, si.pWaitSemaphoreInfos[i].value};
    }
    for (std::uint32_t i = 0; i < si.signalSemaphoreInfoCount && e.nSignal < kMaxSem; ++i) {
        semId(si.pSignalSemaphoreInfos[i].semaphore);
        e.signal[e.nSignal++] = {si.pSignalSemaphoreInfos[i].semaphore, si.pSignalSemaphoreInfos[i].value};
    }
    e.a = reinterpret_cast<std::uint64_t>(fence);
    e.b = si.commandBufferInfoCount;
    e.result = result;
}

void recordSubmit(const char* who, VkQueue queue, std::uint32_t count, const VkSubmitInfo* submits, VkFence fence, VkResult result) {
    if (!on()) return;
    const std::lock_guard lock(g_mutex);
    for (std::uint32_t i = 0; i < count; ++i) {
        Event& e = push();
        e.who = who;
        e.kind = "submit";
        e.queue = queue;
        fillSubmit(e, submits[i]);
        e.a = i + 1 == count ? reinterpret_cast<std::uint64_t>(fence) : 0;
        e.result = result;
    }
    if (count == 0) {
        Event& e = push();
        e.who = who;
        e.kind = "submit(0)";
        e.queue = queue;
        e.a = reinterpret_cast<std::uint64_t>(fence);
        e.result = result;
    }
}

void note(const char* what, std::uint64_t a, std::uint64_t b) {
    if (!on()) return;
    const std::lock_guard lock(g_mutex);
    Event& e = push();
    e.who = "rvk";
    e.kind = what;
    e.a = a;
    e.b = b;
}

void dump(const char* why) {
    if (!on()) return;
    // copy under the lock, format outside (spdlog may block)
    std::vector<Event> events;
    std::vector<std::pair<VkSemaphore, std::pair<bool, int>>> sems;
    std::vector<std::pair<DWORD, InWait>> waits;
    std::unordered_map<VkQueue, std::string> queues;
    std::unordered_map<VkSemaphore, std::pair<bool, int>> semMap;
    {
        const std::lock_guard lock(g_mutex);
        const std::uint64_t n = std::min<std::uint64_t>(g_count, 400);
        for (std::uint64_t i = g_count - n; i < g_count; ++i) events.push_back(g_ring[i % kRing]);
        for (const auto& s : g_sems) sems.push_back(s);
        for (const auto& w : g_waits) waits.push_back(w);
        queues = g_queues;
        semMap = g_sems;
    }
    const double t = nowMs();
    spdlog::error("fgtrace: dump ({}) at {:.1f} ms: {} events recorded, last {} follow", why, t, g_count, events.size());
    const auto sn = [&](VkSemaphore s, std::uint64_t v) {
        auto it = semMap.find(s);
        if (it == semMap.end()) return std::format("S?{:x}", reinterpret_cast<std::uintptr_t>(s));
        return it->second.first ? std::format("T{}={}", it->second.second, v) : std::format("B{}", it->second.second);
    };
    const auto qn = [&](VkQueue q) -> std::string {
        if (q == VK_NULL_HANDLE) return "-";
        auto it = queues.find(q);
        return it != queues.end() ? it->second : std::format("q{:x}", reinterpret_cast<std::uintptr_t>(q));
    };
    for (const auto& [s, info] : sems) {
        if (!info.first || g_device == VK_NULL_HANDLE) continue;
        std::uint64_t v = 0;
        const VkResult r = vkGetSemaphoreCounterValue(g_device, s, &v);
        spdlog::error("fgtrace: timeline T{} value {} ({})", info.second, v, static_cast<int>(r));
    }
    for (const auto& [id, w] : waits) {
        spdlog::error("fgtrace: thread {} in {} for {:.1f} ms (queue {}, {})", id, w.what, t - w.since, qn(w.queue),
                      w.sem.s != VK_NULL_HANDLE ? sn(w.sem.s, w.sem.v) : std::string("-"));
    }
    for (const Event& e : events) {
        std::string w, s;
        for (int i = 0; i < e.nWait; ++i) w += (i ? "," : "") + sn(e.wait[i].s, e.wait[i].v);
        for (int i = 0; i < e.nSignal; ++i) s += (i ? "," : "") + sn(e.signal[i].s, e.signal[i].v);
        spdlog::error("fgtrace: {:.2f} t{} {} {} q={} wait[{}] signal[{}] a={} b={} r={}", e.t, e.thread, e.who, e.kind, qn(e.queue), w, s, e.a, e.b,
                      e.result);
    }
}

}  // namespace rvk::vk::fgtrace
