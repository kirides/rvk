#include "engine/dep_events.h"

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <mutex>

#include <spdlog/spdlog.h>

#include "core/hooks.h"
#include "engine/memread.h"
#include "engine/symbols.h"

namespace rvk::engine::depevents {

namespace {

using engine::readAt;
using LoadFn = void (*)(void*, void*, void*);
using QueueFn = void (*)(void*, void*, int);
using FlushFn = unsigned (*)(void*, void*);
using SelfFn = void (*)(void*);
using ShaderFn = void (*)(void*, void*);

Hook<LoadFn> g_load;
Hook<QueueFn> g_queue;
Hook<FlushFn> g_flush;
Hook<SelfFn> g_lightmap;
Hook<ShaderFn> g_setShader;

bool g_active = false;
std::mutex g_mutex;
std::vector<const void*> g_queueObjs;
Counters g_c;
std::atomic<std::uint64_t> g_unchanged{0};

// Counters are written under the lock too (the hooks may run on another thread than the drain)
void note(const void* object, std::uint64_t Counters::*field) {
    std::lock_guard lock(g_mutex);
    ++(g_c.*field);
    g_queueObjs.push_back(object);
}

void detourLoad(void* self, void* res, void* file) {
    g_load.original(self, res, file);
    note(res, &Counters::loadData);
}
// the state word of an eCResource2 (+0x38): QueueData is called per op and frame for a resource that is not resident (245 k calls in 60 s at the savegame), most of them change nothing
int stateOf(const void* res) {
    int s = -1;
    readAt(res, 0x38, s);
    return s;
}

void detourQueue(void* self, void* res, int mode) {
    const int before = stateOf(res);
    g_queue.original(self, res, mode);
    if (stateOf(res) != before) note(res, &Counters::queueData);
    else ++g_unchanged;
}
unsigned detourFlush(void* self, void* res) {
    const unsigned r = g_flush.original(self, res);
    note(res, &Counters::flush);
    return r;
}
void detourLightmap(void* self) {
    g_lightmap.original(self);
    note(self, &Counters::lightmap);
}
void detourSetShader(void* self, void* shader) {
    g_setShader.original(self, shader);
    note(self, &Counters::setShader);
}

}  // namespace

bool install() {
    const auto& s = sym();
    bool ok = true;
    ok &= g_load.install("eCResourceCache2::LoadData", reinterpret_cast<LoadFn>(s.resCacheLoadData), &detourLoad);
    ok &= g_queue.install("eCResourceCache2::QueueData", reinterpret_cast<QueueFn>(s.resCacheQueueData), &detourQueue);
    ok &= g_flush.install("eCResourceCache2::FlushResource", reinterpret_cast<FlushFn>(s.resCacheFlushResource), &detourFlush);
    ok &= g_lightmap.install("eCMeshResource2::OnLightmapChanged", reinterpret_cast<SelfFn>(s.meshOnLightmapChanged), &detourLightmap);
    ok &= g_setShader.install("eCMaterialResource2::SetShader", reinterpret_cast<ShaderFn>(s.materialSetShader), &detourSetShader);
    g_active = ok;
    spdlog::info("engine: dependency event hooks {} (frame-ownership.md 7.30)", ok ? "installed" : "NOT installed: a symbol is missing");
    return ok;
}

bool active() { return g_active; }

void drain(std::vector<const void*>& out) {
    out.clear();
    {
        std::lock_guard lock(g_mutex);
        if (g_queueObjs.empty()) return;
        out.swap(g_queueObjs);
        g_c.drained += out.size();
    }
    std::sort(out.begin(), out.end());
    out.erase(std::unique(out.begin(), out.end()), out.end());
}

const Counters& counters() {
    g_c.queueUnchanged = g_unchanged.load(std::memory_order_relaxed);
    return g_c;
}

}  // namespace rvk::engine::depevents
