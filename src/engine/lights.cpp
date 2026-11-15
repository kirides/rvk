#include "engine/lights.h"

#include <windows.h>
#include <emmintrin.h>
#include <xmmintrin.h>

#include <atomic>
#include <cmath>
#include <algorithm>
#include <cstring>
#include <mutex>
#include <thread>

#include <spdlog/spdlog.h>

#include "core/hooks.h"
#include "engine/memread.h"
#include "engine/symbols.h"

namespace rvk::engine::lights {
namespace {

// eCIlluminationAdmin: dynamic lights (eCDynamicLight_PS*) data at +0x40, count at +0x48; the static lights' sphere tree at
// +0x68 (eTSphereTree<eCStaticLight_PS*>): root index at tree +0x20, node array pointer at tree +0x30, 0x50-byte nodes:
// sphere (x, y, z, r) at +0, payload at +0x10, next sibling index at +0x28, first child index at +0x38, flags at +0x48
// (bit 4: a leaf with a light payload). Read from FUN_18014f080 / FUN_180576cb0 (FindWightedStaticLights' walk)
constexpr std::size_t kAdminDynData = 0x40, kAdminDynCount = 0x48, kAdminTree = 0x68;
constexpr std::size_t kTreeRoot = 0x20, kTreeNodes = 0x30, kNodeSize = 0x50;
constexpr std::size_t kNodeSphere = 0x0, kNodePayload = 0x10, kNodeSibling = 0x28, kNodeChild = 0x38, kNodeFlags = 0x48;
// eCStaticLight_PS: colour (rgb) at +0x30, intensity +0x40, range +0x44; position: eCStaticPointLight_PS +0x6c,
// eCStaticAmbientLight_PS +0x58. The engine's colour constant = rgb x intensity x DAT_1809f1730 (read at run time)
constexpr std::size_t kLightRgb = 0x30, kLightIntensity = 0x40, kLightRange = 0x44, kPointPos = 0x6c, kAmbientPos = 0x58;
constexpr std::size_t kScaleRva = 0x9f1730;
// eSShaderMaterialLightList: count byte +0x34, entry i at i * 0x40: position xyz +0x38, 1 / R^2 +0x44, colour +0x58
constexpr std::size_t kListCount = 0x34, kListPos = 0x38, kListInvR2 = 0x44, kListColour = 0x58;
constexpr std::uint32_t kMaxNodes = 60000;
constexpr float kDrawShare = 0.5f;

using FindFn = int (*)(void* self, void* out, unsigned long n, const void* box, void* room);
using RegisterFn = int (*)(void* self, void* light);
using AddFn = void (*)(void* self, void* list);

Hook<FindFn> g_find;
Hook<RegisterFn> g_register;
std::atomic<void*> g_admin{nullptr};
std::atomic<std::uint64_t> g_adminLogged{0};

void noteAdmin(void* self) {
    if (self == nullptr || g_admin.load(std::memory_order_relaxed) == self) return;
    g_admin.store(self, std::memory_order_relaxed);
    if (g_adminLogged.fetch_add(1) < 4) spdlog::info("engine: illumination admin {}", self);
}

int detourFind(void* self, void* out, unsigned long n, const void* box, void* room) {
    noteAdmin(self);
    return g_find.original(self, out, n, box, room);
}

int detourRegister(void* self, void* light) {
    noteAdmin(self);
    return g_register.original(self, light);
}

bool callAdd(AddFn fn, void* light, void* list) {
    __try {
        fn(light, list);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

float staticScale() {
    static float scale = [] {
        float v = 1.0f;
        HMODULE m = GetModuleHandleW(L"Engine.dll");
        if (m != nullptr && !readAt(m, kScaleRva, v)) v = 1.0f;
        if (!(v > 0.0f && v < 100.0f)) v = 1.0f;
        spdlog::info("engine: static light colour scale {:.4f}", v);
        return v;
    }();
    return scale;
}

}  // namespace

namespace {

// The static lights change only when the engine caches a cell in or out: the walk of the sphere tree (a few thousand SEH-guarded node reads, 0.1 - 0.35 ms
// in a town) is kept between calls and re-run when the tree's own header changes (root / node array / node count words at +0x20, +0x30, +0x38), the query
// leaves the walked sphere, or every kRewalkCalls calls (a safety net for an in-place change); each call filters the kept list by its own centre and
// radius (same lights, same order as a walk with that radius: the walk visits in tree order and only prunes).
struct StaticCache {
    std::vector<const char*> lights;  // the payload of every static light within `radius` of `center` at walk time; its values are re-read on every call
    std::vector<float> pos;           // each kept light's position at walk time (3 floats): the identity check of a payload pointer (a static light never moves; freed memory reused by another light shows another position)
    float center[3]{};
    float radius = 0.0f;             // the walk's radius (query radius + margin)
    std::uint64_t sig[3]{};
    std::uint32_t calls = 0;         // calls served since the walk
    std::uint64_t walks = 0;
    Stats walkStats;                 // nodes / staticTotal / unknown of the walk
    bool valid = false;
};
StaticCache g_cache;
std::mutex g_cacheMutex;
thread_local bool t_lightWorker = false;     // the light worker thread (the only one that waits for g_cacheMutex)
std::atomic<std::uint64_t> g_cacheBusy{0}, g_staleRewalks{0};  // (g_staleRewalks: a cached payload whose position changed: rewalked)
std::atomic<bool> g_invalidateCache{false};                     // the light table's backstop found the cache differing from a fresh walk: the next collect rewalks   // collects that found the cache locked by the worker and walked privately
constexpr std::uint32_t kRewalkCalls = 150;
constexpr float kWalkMargin = 4000.0f;

// One static light's current values (one guarded read of the object's first 0x78 bytes: vtable, colour, intensity, range, both position slots).
// 0 = not a readable static light, 1 = readable but not usable (range / intensity), 2 = usable
int readStaticLight(const void* payload, const void* ptfv, const void* ambfv, float scale, WorldLight& l) {
    alignas(8) std::uint8_t b[0x78];
    if (payload == nullptr || !safeReadN<sizeof(b)>(payload, b)) return 0;
    const void* vt;
    std::memcpy(&vt, b, sizeof(vt));
    if (vt != ptfv && vt != ambfv) return 0;
    float rgb[3], intensity, range, pos[3];
    std::memcpy(rgb, b + kLightRgb, sizeof(rgb));
    std::memcpy(&intensity, b + kLightIntensity, sizeof(intensity));
    std::memcpy(&range, b + kLightRange, sizeof(range));
    std::memcpy(pos, b + (vt == ptfv ? kPointPos : kAmbientPos), sizeof(pos));
    // the draws' constants carry half of rgb x intensity x scale (measured: ratio 0.49-0.50, rtgi.md 19)
    const float k = intensity * scale * kDrawShare;
    if (!(range > 0.0f && range < 1e6f && k > 0.0f && std::isfinite(k))) return 1;
    l = WorldLight{};
    std::memcpy(l.pos, pos, sizeof(pos));
    l.range = range;
    l.ambient = vt == ambfv;
    for (int c = 0; c < 3; ++c) l.col[c] = rgb[c] * k;
    return 2;
}

// WS15: the per-call re-read of the kept static lights (2,500 at pub_interior, every frame) in ONE guarded region instead of one SEH call + 120-byte copy per light: the same
// fields, the same arithmetic, the same filter as readStaticLight + `reaches`, written to `out` (capacity n). Plain data only (a function holding a __try cannot unwind objects).
// -2 = a payload's position differs from the walk's (the pointer is stale: the caller rewalks). -1 = a read faulted somewhere (a freed light: the caller redoes the whole list with the per-light guarded reads, which skip the unreadable ones as before)
std::atomic<bool> g_fastStaticOffA{false};
int readStaticLightsFast(const char* const* payloads, const float* walkPos, std::size_t n, const void* ptfv, const void* ambfv, float scale, const float center[3], float radius, WorldLight* out) {
    int kept = 0;
    __try {
        for (std::size_t i = 0; i < n; ++i) {
            const char* p = payloads[i];
            if (p == nullptr) continue;
            const void* vt = *reinterpret_cast<const void* const volatile*>(p);
            if (vt != ptfv && vt != ambfv) continue;
            const float intensity = *reinterpret_cast<const volatile float*>(p + kLightIntensity);
            const float range = *reinterpret_cast<const volatile float*>(p + kLightRange);
            const float k = intensity * scale * kDrawShare;
            if (!(range > 0.0f && range < 1e6f && k > 0.0f && std::isfinite(k))) continue;
            const volatile float* rgb = reinterpret_cast<const volatile float*>(p + kLightRgb);
            const volatile float* pos = reinterpret_cast<const volatile float*>(p + (vt == ptfv ? kPointPos : kAmbientPos));
            WorldLight& l = out[kept];
            l.pos[0] = pos[0];
            l.pos[1] = pos[1];
            l.pos[2] = pos[2];
            if (l.pos[0] != walkPos[3 * i] || l.pos[1] != walkPos[3 * i + 1] || l.pos[2] != walkPos[3 * i + 2]) return -2;  // not the light the walk saw (a stale payload)
            l.range = range;
            l.ambient = vt == ambfv;
            l.dynamic = false;
            l.col[0] = rgb[0] * k;
            l.col[1] = rgb[1] * k;
            l.col[2] = rgb[2] * k;
            const float dx = l.pos[0] - center[0], dy = l.pos[1] - center[1], dz = l.pos[2] - center[2];
            const float reach = radius + l.range;
            if (dx * dx + dy * dy + dz * dz <= reach * reach) ++kept;
        }
    } __except (1 /* EXCEPTION_EXECUTE_HANDLER */) {
        return -1;
    }
    return kept;
}

void walkStatics(const char* admin, const float center[3], float radius, StaticCache& c) {
    c.lights.clear();
    c.pos.clear();
    c.walkStats = {};
    Stats& stats = c.walkStats;
    const auto& s = sym();
    const auto reaches = [&](const float p[3], float r) {
        const float dx = p[0] - center[0], dy = p[1] - center[1], dz = p[2] - center[2];
        const float reach = radius + r;
        return dx * dx + dy * dy + dz * dz <= reach * reach;
    };
    const void* ptfv = s.staticPointLightVftable;
    const void* ambfv = s.staticAmbientLightVftable;
    const float scale = staticScale();
    const char* tree = admin + kAdminTree;
    std::int64_t root = -1;
    const char* nodes = nullptr;
    if (readAt(tree, kTreeRoot, root) && readAt(tree, kTreeNodes, nodes) && nodes != nullptr && root >= 0) {
        std::vector<std::int64_t> stack;
        stack.reserve(256);
        stack.push_back(root);
        while (!stack.empty() && stats.nodes < kMaxNodes) {
            const std::int64_t idx = stack.back();
            stack.pop_back();
            std::int64_t child = -1;
            if (!readAt(nodes + idx * kNodeSize, kNodeChild, child)) continue;
            while (child != -1 && stats.nodes < kMaxNodes) {
                ++stats.nodes;
                const char* n = nodes + child * kNodeSize;
                // the node's 80 bytes in one guarded copy (one element of the node array: always whole)
                alignas(8) std::uint8_t nb[kNodeSize];
                if (!safeReadN<kNodeSize>(n, nb)) break;
                const std::uint8_t flags = nb[kNodeFlags];
                float sphere[4];
                std::int64_t next, firstChild;
                const void* payload;
                std::memcpy(sphere, nb + kNodeSphere, sizeof(sphere));
                std::memcpy(&next, nb + kNodeSibling, sizeof(next));
                std::memcpy(&firstChild, nb + kNodeChild, sizeof(firstChild));
                std::memcpy(&payload, nb + kNodePayload, sizeof(payload));
                bool descend = true;
                if ((flags & 4) != 0) {
                    if (payload != nullptr) {
                        WorldLight l;
                        const int r = readStaticLight(payload, ptfv, ambfv, scale, l);
                        if (r == 0) {
                            ++stats.unknown;
                        } else {
                            ++stats.staticTotal;
                            if (r == 2 && reaches(l.pos, l.range)) {
                                c.lights.push_back(static_cast<const char*>(payload));
                                c.pos.insert(c.pos.end(), l.pos, l.pos + 3);
                            }
                        }
                    }
                } else if (std::isfinite(sphere[0]) && std::isfinite(sphere[3])) {
                    descend = reaches(sphere, sphere[3]);
                }
                if (descend && firstChild != -1) stack.push_back(child);
                child = next;
            }
        }
    }
}

}  // namespace

void invalidateStaticCache() { g_invalidateCache.store(true, std::memory_order_relaxed); }

void setFastStatic(bool on) { g_fastStaticOffA.store(!on, std::memory_order_relaxed); }

bool collectStatics(const float center[3], float radius, std::vector<WorldLight>& out, Stats& stats) {
    out.clear();
    stats = {};
    char* admin = static_cast<char*>(g_admin.load(std::memory_order_relaxed));
    if (admin == nullptr) return false;
    stats.adminSeen = true;
    const auto& s = sym();
    const auto reaches = [&](const float p[3], float r) {
        const float dx = p[0] - center[0], dy = p[1] - center[1], dz = p[2] - center[2];
        const float reach = radius + r;
        return dx * dx + dy * dy + dz * dz <= reach * reach;
    };

    // static lights: the sphere tree (walked only when it changed, see StaticCache).
    // WS22 review: only the light worker may wait for the cache's mutex. Every other caller (the engine thread's inline fallback, the GI) try-locks and, when the worker holds it
    // (a below-normal thread preempted mid-walk under a compile storm = a priority inversion on the render thread), walks into a private cache of its own: same lights, same order.
    std::unique_lock<std::mutex> lock(g_cacheMutex, std::defer_lock);
    StaticCache privateCache;
    bool usePrivate = false;
    if (t_lightWorker) {
        lock.lock();
    } else if (!lock.try_lock()) {
        usePrivate = true;
        g_cacheBusy.fetch_add(1, std::memory_order_relaxed);
    }
    {
        StaticCache& c = usePrivate ? privateCache : g_cache;
        const char* tree = admin + kAdminTree;
        std::uint64_t sig[3]{};
        readAt(tree, kTreeRoot, sig[0]);
        readAt(tree, kTreeNodes, sig[1]);
        readAt(tree, 0x38, sig[2]);
        // (a payload that stopped being the light the walk saw rewalks once and reads again; the light table's backstop can also demand a rewalk)
        if (!usePrivate && g_invalidateCache.exchange(false, std::memory_order_relaxed)) c.valid = false;
        for (int attempt = 0;; ++attempt) {
        out.clear();
        stats = {};
        stats.adminSeen = true;
        const float dx = center[0] - c.center[0], dy = center[1] - c.center[1], dz = center[2] - c.center[2];
        const bool inside = c.valid && std::sqrt(dx * dx + dy * dy + dz * dz) + radius <= c.radius;
        const bool reuse = inside && std::memcmp(sig, c.sig, sizeof(sig)) == 0 && c.calls < kRewalkCalls;
        if (!reuse) {
            // the walk's radius never shrinks (the GI asks for 16,000, the light table for 96,000: one walk serves both)
            const float want = radius + kWalkMargin;
            const float walkRadius = c.valid ? std::max(want, c.radius) : want;
            walkStatics(admin, center, walkRadius, c);
            std::memcpy(c.center, center, sizeof(c.center));
            c.radius = walkRadius;
            std::memcpy(c.sig, sig, sizeof(sig));
            c.calls = 0;
            c.valid = true;
            ++c.walks;
        }
        ++c.calls;
        stats.nodes = c.walkStats.nodes;
        stats.staticTotal = c.walkStats.staticTotal;
        stats.unknown = c.walkStats.unknown;
        out.reserve(c.lights.size());
        const float scale = staticScale();
        const void* ptfv = s.staticPointLightVftable;
        const void* ambfv = s.staticAmbientLightVftable;
        // (WS13: the lights are separate heap objects: the loads below were one cache miss after the other; every light's two lines are requested first, so the
        // misses overlap. A prefetch of an unmapped address does not fault; the values read are the same)
        for (const char* payload : c.lights) {
            _mm_prefetch(payload, _MM_HINT_T0);
            _mm_prefetch(payload + 64, _MM_HINT_T0);
        }
        static thread_local std::vector<WorldLight> fast;
        int kept = -1;
        if (!g_fastStaticOffA.load(std::memory_order_relaxed) && !c.lights.empty()) {
            if (fast.size() < c.lights.size()) fast.resize(c.lights.size());
            kept = readStaticLightsFast(c.lights.data(), c.pos.data(), c.lights.size(), ptfv, ambfv, scale, center, radius, fast.data());
        }
        bool stale = kept == -2;
        if (kept >= 0) {
            stats.staticNear += static_cast<std::uint32_t>(kept);
            out.insert(out.end(), fast.begin(), fast.begin() + kept);
        } else if (!stale) {
            for (std::size_t i = 0; i < c.lights.size(); ++i) {
                WorldLight l;
                if (readStaticLight(c.lights[i], ptfv, ambfv, scale, l) != 2) continue;
                if (l.pos[0] != c.pos[3 * i] || l.pos[1] != c.pos[3 * i + 1] || l.pos[2] != c.pos[3 * i + 2]) {
                    stale = true;
                    break;
                }
                if (!reaches(l.pos, l.range)) continue;
                ++stats.staticNear;
                out.push_back(l);
            }
        }
        if (stale && attempt == 0) {
            g_staleRewalks.fetch_add(1, std::memory_order_relaxed);
            c.valid = false;
            continue;
        }
        break;
        }
    }
    return true;
}

bool collectDynamics(const float center[3], float radius, std::vector<WorldLight>& out, Stats& stats) {
    char* admin = static_cast<char*>(g_admin.load(std::memory_order_relaxed));
    if (admin == nullptr) return false;
    const auto& s = sym();
    const auto reaches = [&](const float p[3], float r) {
        const float dx = p[0] - center[0], dy = p[1] - center[1], dz = p[2] - center[2];
        const float reach = radius + r;
        return dx * dx + dy * dy + dz * dz <= reach * reach;
    };
    // dynamic lights: the engine's own light list entry of each (flicker / effects included)
    const void* pfv = s.pointLightVftable;
    const auto add = reinterpret_cast<AddFn>(s.pointLightAddToLightList);
    const char* data = nullptr;
    std::uint32_t count = 0;
    if (pfv != nullptr && add != nullptr && readAt(admin, kAdminDynData, data) && readAt(admin, kAdminDynCount, count) &&
        data != nullptr && count > 0 && count < 4096) {
        stats.dynamicRaw = count;
        for (std::uint32_t i = 0; i < count; ++i) {
            void* light = nullptr;
            const void* vt = nullptr;
            if (!readAt(data, i * 8, light) || !readAt(light, 0, vt)) continue;
            if (i < 4) stats.dynamicVt[i] = reinterpret_cast<std::uintptr_t>(vt);
            if (vt != pfv) continue;
            ++stats.dynamicTotal;
            alignas(16) char list[0x200]{};
            if (!callAdd(add, light, list)) continue;
            if (list[kListCount] == 0) continue;  // disabled
            float pos[3]{}, col[3]{}, invR2 = 0.0f;
            std::memcpy(pos, list + kListPos, sizeof(pos));
            std::memcpy(&invR2, list + kListInvR2, sizeof(invR2));
            std::memcpy(col, list + kListColour, sizeof(col));
            if (!(invR2 > 0.0f) || !std::isfinite(invR2) || !(col[0] + col[1] + col[2] > 0.0f)) continue;
            const float range = 1.0f / std::sqrt(invR2);
            if (!reaches(pos, range)) continue;
            ++stats.dynamicNear;
            WorldLight l;
            std::memcpy(l.pos, pos, sizeof(pos));
            l.range = range;
            std::memcpy(l.col, col, sizeof(col));
            l.dynamic = true;
            out.push_back(l);
        }
    }
    return true;
}

bool collect(const float center[3], float radius, std::vector<WorldLight>& out, Stats& stats) {
    return collectStatics(center, radius, out, stats) && collectDynamics(center, radius, out, stats);
}

// The pre-cache static walk, verbatim (RVK_LIGHTS_VERIFY compares the cached path against it)
bool collectStaticsReference(const float center[3], float radius, std::vector<WorldLight>& out) {
    out.clear();
    Stats stats;
    char* admin = static_cast<char*>(g_admin.load(std::memory_order_relaxed));
    if (admin == nullptr) return false;
    const auto& s = sym();
    const auto reaches = [&](const float p[3], float r) {
        const float dx = p[0] - center[0], dy = p[1] - center[1], dz = p[2] - center[2];
        const float reach = radius + r;
        return dx * dx + dy * dy + dz * dz <= reach * reach;
    };
    // static lights: the sphere tree
    const void* ptfv = s.staticPointLightVftable;
    const void* ambfv = s.staticAmbientLightVftable;
    const float scale = staticScale();
    const char* tree = admin + kAdminTree;
    std::int64_t root = -1;
    const char* nodes = nullptr;
    if (readAt(tree, kTreeRoot, root) && readAt(tree, kTreeNodes, nodes) && nodes != nullptr && root >= 0) {
        std::vector<std::int64_t> stack;
        stack.reserve(256);
        stack.push_back(root);
        while (!stack.empty() && stats.nodes < kMaxNodes) {
            const std::int64_t idx = stack.back();
            stack.pop_back();
            std::int64_t child = -1;
            if (!readAt(nodes + idx * kNodeSize, kNodeChild, child)) continue;
            while (child != -1 && stats.nodes < kMaxNodes) {
                ++stats.nodes;
                const char* n = nodes + child * kNodeSize;
                // the node's 80 bytes in one guarded copy (one element of the node array: always whole)
                alignas(8) std::uint8_t nb[kNodeSize];
                if (!safeReadN<kNodeSize>(n, nb)) break;
                const std::uint8_t flags = nb[kNodeFlags];
                float sphere[4];
                std::int64_t next, firstChild;
                const void* payload;
                std::memcpy(sphere, nb + kNodeSphere, sizeof(sphere));
                std::memcpy(&next, nb + kNodeSibling, sizeof(next));
                std::memcpy(&firstChild, nb + kNodeChild, sizeof(firstChild));
                std::memcpy(&payload, nb + kNodePayload, sizeof(payload));
                bool descend = true;
                if ((flags & 4) != 0) {
                    if (payload != nullptr) {
                        const void* vt = nullptr;
                        float pos[3]{}, rgb[3]{}, intensity = 0.0f, range = 0.0f;
                        if (readAt(payload, 0, vt) && (vt == ptfv || vt == ambfv) && readAt(payload, kLightRgb, rgb) &&
                            readAt(payload, kLightIntensity, intensity) && readAt(payload, kLightRange, range) &&
                            readAt(payload, vt == ptfv ? kPointPos : kAmbientPos, pos)) {
                            ++stats.staticTotal;
                            // the draws' constants carry half of rgb x intensity x scale (measured: ratio 0.49-0.50, rtgi.md 19)
                            const float k = intensity * scale * kDrawShare;
                            if (range > 0.0f && range < 1e6f && k > 0.0f && std::isfinite(k) && reaches(pos, range)) {
                                ++stats.staticNear;
                                WorldLight l;
                                std::memcpy(l.pos, pos, sizeof(pos));
                                l.range = range;
                                l.ambient = vt == ambfv;
                                for (int c = 0; c < 3; ++c) l.col[c] = rgb[c] * k;
                                out.push_back(l);
                            }
                        } else {
                            ++stats.unknown;
                        }
                    }
                } else if (std::isfinite(sphere[0]) && std::isfinite(sphere[3])) {
                    descend = reaches(sphere, sphere[3]);
                }
                if (descend && firstChild != -1) stack.push_back(child);
                child = next;
            }
        }
    }

    return true;
}

// ---- WS22: the static half of collect() on a persistent worker (frame-ownership.md 16.8) ----------------------------------------------------------------------
//
// Shared variables and their synchronisation (the whole protocol; the engine thread = the thread that kicks / takes, the worker = `rvk light collect`):
//  * g_state (atomic<uint32>): Idle -> Pending (engine, CAS, release; the job's inputs are written before) -> Running (worker, CAS acquire) -> Done (worker, CAS release;
//    the job's outputs are written before) -> Idle (engine, CAS, after it consumed or discarded the outputs). Pending -> Idle is the engine's cancel (CAS: it loses to the
//    worker's Running). Quit (engine, exchange) is terminal: every other transition is a CAS from a named state, so nothing leaves it. The worker sleeps in atomic::wait(g_state)
//    (WaitOnAddress: no spinning); the kick notifies.
//  * g_job.{center, radius, frame, hash} are written by the engine thread only while the state is Idle / Done and read by the worker only while it is Running; g_job.{out, stats,
//    hash value, workerUs, ok} are written by the worker only while it is Running and read / swapped by the engine thread only after it observed Done (acquire).
//  * g_on / g_radius / g_hashFn: configuration written and read by the engine thread (the light table's configure / the BeginZPass detour); relaxed atomics for the overlay command.
//  * everything the worker reads in the engine's memory (the light tree, the payloads) is read under SEH guards; the evidence that no engine thread writes it inside the kick -> take
//    window is in engine-gfx.md (WS22). The worker never calls the engine: collectStatics only reads (the dynamic half, which calls AddToLightList, stays on the engine thread).
//  * the static cache (g_cache) is behind g_cacheMutex as before; the worker takes it like any other caller.
namespace {
constexpr float kAsyncMargin = 2000.0f, kAsyncMoveMax = 1500.0f;  // the kick's centre is the previous frame's camera: the walk reaches kAsyncMargin further, the take accepts a move up to kAsyncMoveMax
enum : std::uint32_t { kIdle = 0, kPending = 1, kRunning = 2, kDone = 3, kQuit = 4 };
struct Job {
    float center[3]{};
    float radius = 0.0f;
    std::uint64_t frame = 0;
    StaticsHashFn hash = nullptr;
    std::vector<WorldLight> out;
    Stats stats;
    std::uint64_t hashValue = 0;
    double workerUs = 0.0;
    bool ok = false;
};
std::atomic<std::uint32_t> g_state{kIdle};
Job g_job;
std::atomic<bool> g_on{false};
std::atomic<float> g_radius{96000.0f};
std::atomic<StaticsHashFn> g_hashFn{nullptr};
std::thread* g_thread = nullptr;  // created on the first kick, leaked unless asyncShutdown() joins it (a joinable std::thread in a static would terminate at DLL unload)
AsyncCounters g_ctr;               // engine thread only

double nowUs() {
    static const double freq = [] { LARGE_INTEGER f; QueryPerformanceFrequency(&f); return static_cast<double>(f.QuadPart); }();
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return static_cast<double>(t.QuadPart) * 1e6 / freq;
}

void workerMain() {
    t_lightWorker = true;
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
    if (const auto set = reinterpret_cast<HRESULT(WINAPI*)(HANDLE, PCWSTR)>(GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "SetThreadDescription")); set != nullptr) {
        set(GetCurrentThread(), L"rvk light collect");
    }
    for (;;) {
        std::uint32_t s = g_state.load(std::memory_order_acquire);
        if (s == kQuit) return;
        if (s == kPending) {
            if (!g_state.compare_exchange_strong(s, kRunning, std::memory_order_acq_rel)) continue;
            const double t0 = nowUs();
            Job& j = g_job;
            j.ok = collectStatics(j.center, j.radius, j.out, j.stats);
            j.hashValue = j.ok && j.hash != nullptr ? j.hash(j.out.data(), j.out.size()) : 0;
            j.workerUs = nowUs() - t0;
            std::uint32_t r = kRunning;
            g_state.compare_exchange_strong(r, kDone, std::memory_order_acq_rel);  // (fails only after asyncShutdown's Quit)
            continue;
        }
        g_state.wait(s, std::memory_order_acquire);
    }
}
}  // namespace

void asyncConfigure(bool on, float radius, StaticsHashFn hash) {
    g_radius.store(radius, std::memory_order_relaxed);
    g_hashFn.store(hash, std::memory_order_relaxed);
    g_on.store(on, std::memory_order_relaxed);
}

bool asyncActive() { return g_on.load(std::memory_order_relaxed); }

void asyncKick(const float center[3], std::uint64_t frame) {
    if (!g_on.load(std::memory_order_relaxed)) return;
    std::uint32_t s = g_state.load(std::memory_order_acquire);
    if (s != kIdle && g_job.frame == frame) return;  // kicked in this frame already (a second scene job)
    if (s == kDone) {  // an outcome nobody took (the table did not run in that frame): discarded
        if (!g_state.compare_exchange_strong(s, kIdle, std::memory_order_acq_rel)) return;
        ++g_ctr.discarded;
    } else if (s != kIdle) {
        ++g_ctr.kickBusy;  // the previous job is still pending / running (a stalled worker): this frame runs inline
        return;
    }
    if (g_thread == nullptr) g_thread = new std::thread(workerMain);
    Job& j = g_job;
    std::memcpy(j.center, center, sizeof(j.center));
    j.radius = g_radius.load(std::memory_order_relaxed) + kAsyncMargin;  // (a superset: the take filters it to the real centre / radius, same predicate and order)
    j.frame = frame;
    j.hash = g_hashFn.load(std::memory_order_relaxed);
    std::uint32_t idle = kIdle;
    if (!g_state.compare_exchange_strong(idle, kPending, std::memory_order_acq_rel)) return;
    ++g_ctr.kicks;
    g_state.notify_one();
}

bool asyncTake(const float center[3], float radius, std::uint64_t frame, std::vector<WorldLight>& out, Stats& stats, std::uint64_t& hash) {
    std::uint32_t s = g_state.load(std::memory_order_acquire);
    if (s == kIdle || s == kQuit) {
        ++g_ctr.noKick;
        return false;
    }
    if (s == kPending) {  // the worker never got to it: cancel and run inline
        if (g_state.compare_exchange_strong(s, kIdle, std::memory_order_acq_rel)) {
            ++g_ctr.notStarted;
            return false;
        }
    }
    if (s != kDone) {
        // running: it needs tens of microseconds; wait a bounded time (the engine thread has nothing else to do with this build), then give up
        const double t0 = nowUs();
        while ((s = g_state.load(std::memory_order_acquire)) == kRunning && nowUs() - t0 < 150.0) _mm_pause();
        const double w = nowUs() - t0;
        g_ctr.waitUs += w;
        g_ctr.waitMaxUs = std::max(g_ctr.waitMaxUs, w);
        ++g_ctr.waits;
        if (s != kDone) {
            ++g_ctr.timedOut;
            return false;
        }
    }
    // Done
    Job& j = g_job;
    const float mx = center[0] - j.center[0], my = center[1] - j.center[1], mz = center[2] - j.center[2];
    const bool same = j.ok && j.frame == frame && j.radius == radius + kAsyncMargin && mx * mx + my * my + mz * mz <= kAsyncMoveMax * kAsyncMoveMax;
    if (same) {
        // the worker's list holds every static light that reaches the kick's centre with margin; the true query keeps those that reach `center` (the predicate of readStaticLightsFast),
        // in the same order. When all of them do (radius 96,000 covers the island) the list and its hash are used as they are.
        std::size_t kept = 0;
        for (const WorldLight& l : j.out) {
            const float dx = l.pos[0] - center[0], dy = l.pos[1] - center[1], dz = l.pos[2] - center[2];
            const float reach = radius + l.range;
            if (dx * dx + dy * dy + dz * dz <= reach * reach) ++kept;
        }
        out.swap(j.out);
        stats = j.stats;
        hash = j.hashValue;
        if (kept != out.size()) {
            std::size_t w = 0;
            for (const WorldLight& l : out) {
                const float dx = l.pos[0] - center[0], dy = l.pos[1] - center[1], dz = l.pos[2] - center[2];
                const float reach = radius + l.range;
                if (dx * dx + dy * dy + dz * dz <= reach * reach) out[w++] = l;
            }
            out.resize(w);
            hash = j.hash != nullptr ? j.hash(out.data(), out.size()) : 0;
            ++g_ctr.refiltered;
        }
        stats.staticNear = static_cast<std::uint32_t>(out.size());
        g_ctr.workerUs += j.workerUs;
        ++g_ctr.hits;
    } else {
        ++g_ctr.mismatch;
    }
    std::uint32_t d = kDone;
    g_state.compare_exchange_strong(d, kIdle, std::memory_order_acq_rel);
    return same;
}

AsyncCounters asyncCountersTake() {
    AsyncCounters c = g_ctr;
    c.cacheBusy = g_cacheBusy.exchange(0, std::memory_order_relaxed);
    c.staleRewalks = g_staleRewalks.exchange(0, std::memory_order_relaxed);
    g_ctr = {};
    return c;
}

void asyncShutdown() {
    g_on.store(false, std::memory_order_relaxed);
    if (g_thread == nullptr) return;
    g_state.store(kQuit, std::memory_order_release);
    g_state.notify_all();
    if (g_thread->joinable()) g_thread->join();
    delete g_thread;
    g_thread = nullptr;
    g_state.store(kIdle, std::memory_order_release);
}

bool install() {
    const auto& s = sym();
    bool ok = false;
    if (s.illuminationFindWighted != nullptr) {
        ok = g_find.install("eCIlluminationAdmin::FindWightedStaticLights", reinterpret_cast<FindFn>(s.illuminationFindWighted), &detourFind) || ok;
    }
    if (s.illuminationRegisterDynamic != nullptr) {
        ok = g_register.install("eCIlluminationAdmin::RegisterDynamicLightPS", reinterpret_cast<RegisterFn>(s.illuminationRegisterDynamic),
                                &detourRegister) || ok;
    }
    if (!ok) spdlog::warn("engine: the illumination admin's symbols were not found: the GI sees only the drawn objects' lights");
    return ok;
}

}  // namespace rvk::engine::lights
