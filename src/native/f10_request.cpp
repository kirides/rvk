#include "native/f10_request.h"

#include <windows.h>
#include <immintrin.h>

#include <algorithm>
#include <chrono>
#include <cstring>
#include <format>
#include <string>
#include <unordered_map>
#include <unordered_set>

#include <spdlog/spdlog.h>

#include "core/hooks.h"
#include "core/settings.h"
#include "engine/memread.h"
#include "engine/symbols.h"
#include "native/static_compose.h"

// F10-1 (frame-ownership.md 14.31). Engine.dll RVAs (decompiled): eCFrustumElementItem::Request 0x671370, eCRenderContextMesh::RequestRenderOps 0x5c75e0,
// eCRenderOp2::Create 0x67f680, eCResourceCache2::CacheResource 0x6843e0, the op array's SetCount FUN_1800ae210.
namespace rvk::native::f10req {

// ---------------------------------------------------------------- pure parts

void meshOps(const SubIn* subs, int n, std::uint16_t mask, float fade, float one, const void* ctx, std::uint32_t key, std::vector<OpOut>& out) {
    const std::uint16_t u7 = fade == one ? mask : static_cast<std::uint16_t>(mask & 0xffe5u);
    const auto emit = [&](std::uint8_t list, std::uint32_t desc, float f, std::uint32_t sortId) {
        OpOut o;
        o.list = list;
        o.op.context = ctx;
        o.op.desc = desc;
        o.op.fade = f;
        o.op.key = key;
        o.op.sortId = sortId;
        out.push_back(o);
    };
    bool b9 = true, b10 = true;
    for (int i = 0; i < n; ++i) {
        b9 = b10;
        const SubIn& s = subs[i];
        if (s.material) {
            std::uint16_t u20 = 0;
            if (s.refracted) u20 = static_cast<std::uint16_t>(u7 & 0xffe1u);
            else if (s.blended || fade != one) u20 = static_cast<std::uint16_t>(u7 & 0xffc5u);
            else u20 = static_cast<std::uint16_t>(u7 & 0xffdbu);
            const auto d = static_cast<std::uint32_t>(i);
            if (u20 & 2u) emit(kListSolid, d, 1.0f, s.sortId);  // (Create's fade 1.0: the solid op keeps it)
            if (u20 & 4u) emit(kListAlpha, d, fade, s.sortId);
            if (u20 & 0x20u) emit(kListRefracted, d, fade, s.sortId);
            if (u20 & 0x200u) emit(kListSolid, d, fade, s.sortId);
            b9 = (u20 & 0x18u) != 0 ? b10 : false;
        }
        b10 = b9;
    }
    if ((u7 & 0x18u) != 0 && n > 0) {
        for (int i = 0; i < n; ++i) {
            const SubIn& s = subs[i];
            if (!s.material || s.blended || s.refracted) continue;
            if (b9) {
                emit(kListZ, 0xffu, fade, s.sortId);
                break;
            }
            emit(kListZ, static_cast<std::uint32_t>(i), fade, s.sortId);
        }
    }
}

std::uint32_t depthKey(const float sphere[4], const float plane[4], float scale) {
    // (movss / mulss / addss in the engine's order: 5c76e8 .. 5c773d)
    float a = sphere[2] * plane[1];
    const float b = plane[0] * sphere[1];
    const float c = sphere[3] * plane[2];
    a = a + b;
    a = a + c;
    a = a + plane[3];
    a = a - sphere[0];
    a = a * scale;
    std::int32_t k = _mm_cvttss_si32(_mm_set_ss(a));
    if (k > 0xffffff) k = 0xffffff;
    if (k < 0) k = 0;
    return static_cast<std::uint32_t>(k);
}

void mergeDeferred(const RawOp* engine, std::uint32_t n, const std::vector<Deferred>& d, std::vector<RawOp>& out) {
    out.clear();
    out.reserve(n + d.size());
    std::size_t j = 0;
    for (std::uint32_t i = 0; i < n; ++i) {
        while (j < d.size() && d[j].pos <= i) out.push_back(d[j++].op);
        out.push_back(engine[i]);
    }
    while (j < d.size()) out.push_back(d[j++].op);
}

void splitDeferred(const std::vector<RawOp>& full, const std::vector<std::uint32_t>& taken, std::vector<RawOp>& engine, std::vector<Deferred>& d) {
    engine.clear();
    d.clear();
    std::size_t t = 0;
    for (std::uint32_t k = 0; k < full.size(); ++k) {
        if (t < taken.size() && taken[t] == k) {
            d.push_back({static_cast<std::uint32_t>(engine.size()), full[k]});
            ++t;
        } else {
            engine.push_back(full[k]);
        }
    }
}

void expectedHead(const std::vector<const void*>& touches, std::vector<const void*>& head) {
    head.clear();
    for (auto it = touches.rbegin(); it != touches.rend(); ++it) {
        if (std::find(head.begin(), head.end(), *it) == head.end()) head.push_back(*it);
    }
}

// ---------------------------------------------------------------- engine side

namespace {

using U8 = std::uint8_t;
template <class T>
inline T rd(const void* p, std::size_t off) {
    T v;
    std::memcpy(&v, static_cast<const U8*>(p) + off, sizeof(T));
    return v;
}
inline const void* rdp(const void* p, std::size_t off) { return rd<const void*>(p, off); }

using RequestFn = bool (*)(void* item, void* db, std::uint16_t mask, float fade);
using CacheResFn = void (*)(void* cache, void* res);
using SetCountFn = void (*)(void* list, std::int32_t count, std::int32_t grow);
using IllumFn = void (*)(void* ps);
using LodFn = void (*)(void* ctx, void* db);
using CtxRequestFn = void (*)(void* ctx, void* db, float fade, std::uint16_t mask);
using BoolGetter = bool (*)(const void* shader);
using ByteGetter = std::uint8_t (*)(const void* shader);
using SphereFn = const float* (*)(const void* entity);
// a vtable slot (a `const void*` element) as a function pointer: clang rejects reinterpret_cast from const void* (casts away qualifiers)
template <class Fn> Fn slotFn(const void* slot) { return reinterpret_cast<Fn>(const_cast<void*>(slot)); }

Hook<RequestFn> g_request;
CacheResFn g_cacheResource = nullptr;
SetCountFn g_setCount = nullptr;
IllumFn g_updateIllum = nullptr;
const void* g_meshVtable = nullptr;
const float* g_epsilon = nullptr;
float g_one = 1.0f;
bool g_installed = false;

// the list offsets of eCFrustumDatabase by kListOffset index (only the four a mesh context writes are used)
inline std::uint32_t listOff(std::uint8_t l) { return kListOffset[l]; }

// ---- the main view the stage acts on, and the prediction of the next request ----
void* g_mainDb = nullptr;
bool g_gateOpen = false;
int g_checkInterval = 10;
std::uint64_t g_frameAtView = 0, g_presentsAtView = 0, g_presents = 0;
int g_trippedMode = 0;  // a difference / a fault turned the stage off (until the setting changes)
int g_lastSetting = 0;

// per request pass of the main database
struct Pass {
    void* db = nullptr;
    std::uint32_t frameCounter = 0;  // db +8 when the pass began
    int mode = 0;          // 0 passthrough, 1 live (port, deferral when `defer`), 2 compare (engine + dry port)
    bool defer = false;
    bool consumed[2] = {};
    std::vector<Deferred> deferred[2];  // z, solid
} g_pass;

struct Window {
    std::uint64_t passes = 0, livePasses = 0, comparePasses = 0, deferPasses = 0;
    std::uint64_t elements = 0, contexts = 0, ported = 0, engineCtx = 0;
    std::uint64_t notMesh = 0, hierarchy = 0, renderInstance = 0, notResident = 0, noResource = 0, tooMany = 0;
    std::uint64_t opsDeferred = 0, opsWritten = 0;
    std::uint64_t merged = 0, mergedViews = 0, unconsumed = 0, predictWrong = 0;
    std::uint64_t compared = 0, opsCompared = 0, differing = 0, lruChecked = 0, lruDiffering = 0, mergeChecks = 0, mergeDiffering = 0;
    std::uint64_t faults = 0;
    std::uint64_t engineColdNs = 0, portWarmNs = 0, comparedEngineFirst = 0;  // compare passes with the engine's slot first
    std::uint64_t portNs = 0, engineNs = 0;  // compare passes: the port (dry) vs the engine's slot, per context (the same contexts)
    std::uint64_t liveNs = 0, liveSampled = 0;  // live passes: prepare + the port + its touches + op writes (every 8th context)
} g_w;
// compare passes: the engine's cost per context class (vtable): contexts, ns of the RequestRenderOps slot, ns of UpdateLoD; per element the illumination update
struct ClassCost {
    std::uint64_t n = 0, slotNs = 0, lodNs = 0, eligible = 0;
};
std::unordered_map<const void*, ClassCost> g_classCost;
std::uint64_t g_illumNs = 0, g_illumCalls = 0, g_elemNs = 0, g_elemTimed = 0;
std::uint64_t g_runDiffering = 0, g_runCompared = 0, g_runActed = 0, g_runDeferred = 0;
int g_diffLog = 8;
std::chrono::steady_clock::time_point g_lastReport = std::chrono::steady_clock::now();

inline std::uint64_t nowNs() {
    LARGE_INTEGER c, f;
    QueryPerformanceCounter(&c);
    static const std::int64_t freq = [] {
        LARGE_INTEGER ff;
        QueryPerformanceFrequency(&ff);
        return ff.QuadPart;
    }();
    (void)f;
    return static_cast<std::uint64_t>(static_cast<double>(c.QuadPart) * 1e9 / static_cast<double>(freq));
}

int settingMode() {
    const auto& rs = settings::renderer();
    return rs.nativeOwnCollection != nullptr && settings::fullNow() ? static_cast<int>(rs.nativeOwnCollection->asInt()) : 0;  // (S-9: default on, Full only)
}

// ---- the port of eCRenderContextMesh::RequestRenderOps for an eligible context ----
constexpr int kMaxSubs = 64;
enum class Why : std::uint8_t { Ok, NotMesh, Hierarchy, RenderInstance, NotResident, NoResource, TooMany };

struct Prepared {
    const void* res = nullptr;
    void* cache = nullptr;
    int n = 0;
    SubIn subs[kMaxSubs];
    const void* mats[kMaxSubs];
    void* matCache[kMaxSubs];
    std::uint32_t key = 0;
};

// everything the port reads, before any touch (the eligibility: every touch is a CacheResource of a resident resource)
Why prepare(const void* ctx, const void* db, Prepared& p) {
    if (rdp(ctx, 0) != g_meshVtable) return Why::NotMesh;
    if (rd<std::int32_t>(ctx, 0x34) != 0) return Why::Hierarchy;
    p.res = rdp(ctx, 0x28);
    if (p.res == nullptr) return Why::NoResource;
    p.cache = const_cast<void*>(rdp(p.res, 0x30));
    if (p.cache == nullptr || rd<std::int32_t>(p.res, 0x38) != 3) return Why::NotResident;
    if (rd<std::uint8_t>(p.res, 0xa0) != 0) return Why::RenderInstance;
    const std::int32_t n = rd<std::int32_t>(p.res, 0x60);
    if (n > kMaxSubs) return Why::TooMany;
    p.n = std::max(n, 0);
    const U8* table = static_cast<const U8*>(rdp(p.res, 0x58));
    for (int i = 0; i < p.n; ++i) {
        const void* mat = rdp(table + static_cast<std::size_t>(i) * 0x48, 0x38);
        p.mats[i] = mat;
        p.matCache[i] = nullptr;
        SubIn& s = p.subs[i];
        s = SubIn{};
        if (mat == nullptr) continue;
        void* mc = const_cast<void*>(rdp(mat, 0x30));
        const std::int32_t st = rd<std::int32_t>(mat, 0x38);
        // (a material in state 1 / 2 would be QueueData'd: a load request; a cache-less one is not touched at all; both left to the engine)
        if (mc == nullptr || st != 3) return Why::NotResident;
        p.matCache[i] = mc;
        s.material = true;
        const void* sh = rdp(mat, 0x70);
        const void* const* vt = sh != nullptr ? static_cast<const void* const*>(rdp(sh, 0)) : nullptr;
        if (sh != nullptr) {
            s.refracted = slotFn<BoolGetter>(vt[0x1a8 / 8])(sh);
            s.blended = slotFn<BoolGetter>(vt[0x198 / 8])(sh);
        }
        const std::uint8_t add = sh != nullptr ? slotFn<ByteGetter>(vt[0x1b0 / 8])(sh) : 0;
        s.sortId = sortIdOf(rd<std::uint32_t>(mat, 0x78), sh != nullptr, add, sh != nullptr ? rd<std::int32_t>(sh, 0x7c) : 0);
    }
    // the view depth key from the entity's sphere (vtable +0x268: {r, cx, cy, cz})
    const void* entity = rdp(ctx, 0x10);
    const auto sphereFn = slotFn<SphereFn>(static_cast<const void* const*>(rdp(entity, 0))[0x268 / 8]);
    const float* sph = sphereFn(entity);
    float plane[4];
    std::memcpy(plane, static_cast<const U8*>(db) + 0x94, sizeof(plane));
    p.key = depthKey(sph, plane, rd<float>(db, 0xa4));
    return Why::Ok;
}

// the touch sequence of the engine's RequestRenderOps for a prepared context (resource, then every material in loop 1, then loop 2's up to its break)
void touchSequence(const Prepared& p, std::uint16_t mask, float fade, std::vector<std::pair<void*, const void*>>& seq) {
    seq.clear();
    seq.emplace_back(p.cache, p.res);
    for (int i = 0; i < p.n; ++i)
        if (p.mats[i] != nullptr) seq.emplace_back(p.matCache[i], p.mats[i]);
    const std::uint16_t u7 = fade == g_one ? mask : static_cast<std::uint16_t>(mask & 0xffe5u);
    if ((u7 & 0x18u) != 0 && p.n > 0) {
        // loop 2 touches every material until (and including) the one that takes the whole-mesh op, if the whole mesh is taken
        bool all = true;
        {
            // (the whole-mesh decision of meshOps: every material's ops kept z bits)
            bool b10 = true;
            for (int i = 0; i < p.n; ++i) {
                bool b9 = b10;
                const SubIn& s = p.subs[i];
                if (s.material) {
                    std::uint16_t u20 = s.refracted ? static_cast<std::uint16_t>(u7 & 0xffe1u)
                                        : (s.blended || fade != g_one) ? static_cast<std::uint16_t>(u7 & 0xffc5u)
                                                                       : static_cast<std::uint16_t>(u7 & 0xffdbu);
                    b9 = (u20 & 0x18u) != 0 ? b10 : false;
                }
                b10 = b9;
            }
            all = b10;
        }
        for (int i = 0; i < p.n; ++i) {
            const SubIn& s = p.subs[i];
            if (!s.material) continue;
            seq.emplace_back(p.matCache[i], p.mats[i]);
            if (!s.blended && !s.refracted && all) break;
        }
    }
}

// the engine list `l` of db: append one op (FUN_1800ae1e0 + Create's bytes)
inline void pushEngine(void* db, std::uint8_t l, const RawOp& op) {
    U8* list = static_cast<U8*>(db) + listOff(l);
    const std::int32_t c = rd<std::int32_t>(list, 0x10);
    g_setCount(list, c + 1, 0);
    U8* data = const_cast<U8*>(static_cast<const U8*>(rdp(list, 8)));
    std::memcpy(data + static_cast<std::size_t>(c) * sizeof(RawOp), &op, sizeof(RawOp));
}

std::vector<OpOut> g_ops;
std::vector<std::pair<void*, const void*>> g_touches;

// live: the port's touches + ops (deferral of z / solid when the pass defers)
void livePort(void* ctx, void* db, float fade, std::uint16_t m, const Prepared& p) {
    touchSequence(p, m, fade, g_touches);
    for (const auto& t : g_touches) g_cacheResource(t.first, const_cast<void*>(t.second));
    g_ops.clear();
    meshOps(p.subs, p.n, m, fade, g_one, ctx, p.key, g_ops);
    for (const OpOut& o : g_ops) {
        if (g_pass.defer && (o.list == kListZ || o.list == kListSolid)) {
            const std::uint32_t pos = static_cast<std::uint32_t>(rd<std::int32_t>(static_cast<U8*>(db) + listOff(o.list), 0x10));
            g_pass.deferred[o.list].push_back({pos, o.op});
            ++g_w.opsDeferred;
        } else {
            pushEngine(db, o.list, o.op);
            ++g_w.opsWritten;
        }
    }
}

void countWhy(Why why) {
    switch (why) {
        case Why::NotMesh: ++g_w.notMesh; break;
        case Why::Hierarchy: ++g_w.hierarchy; break;
        case Why::RenderInstance: ++g_w.renderInstance; break;
        case Why::NotResident: ++g_w.notResident; break;
        case Why::NoResource: ++g_w.noResource; break;
        case Why::TooMany: ++g_w.tooMany; break;
        default: break;
    }
    ++g_w.engineCtx;
}

std::string opText(const RawOp& o) { return std::format("ctx {} desc 0x{:x} fade {:.4f} key 0x{:x} sort 0x{:x}", o.context, o.desc, o.fade, o.key, o.sortId); }

// compare: the dry port vs the engine's slot for one context
void compareCtx(void* ctx, void* db, float fade, std::uint16_t m, CtxRequestFn engineSlot) {
    // (the order alternates per pass: the second of the two runs on warm caches; the window reports both orders)
    const bool engineFirst = (g_w.comparePasses & 1u) != 0;
    std::int32_t before[4];
    static constexpr std::uint8_t kL[4] = {kListZ, kListSolid, kListRefracted, kListAlpha};
    for (int k = 0; k < 4; ++k) before[k] = rd<std::int32_t>(static_cast<U8*>(db) + listOff(kL[k]), 0x10);
    Prepared p;
    std::vector<OpOut> mine;
    Why why = Why::Ok;
    std::uint64_t portNs = 0, engNs = 0;
    const auto runPort = [&] {
        const std::uint64_t a = nowNs();
        why = prepare(ctx, db, p);
        if (why == Why::Ok) {
            meshOps(p.subs, p.n, m, fade, g_one, ctx, p.key, mine);
            touchSequence(p, m, fade, g_touches);
        }
        portNs = nowNs() - a;
    };
    const auto runEngine = [&] {
        const std::uint64_t a = nowNs();
        engineSlot(ctx, db, fade, m);
        engNs = nowNs() - a;
    };
    if (engineFirst) {
        runEngine();
        runPort();
    } else {
        runPort();
        runEngine();
    }
    {
        ClassCost& cc = g_classCost[rdp(ctx, 0)];
        ++cc.n;
        cc.slotNs += engNs;
        if (why == Why::Ok) ++cc.eligible;
    }
    if (why != Why::Ok) {
        countWhy(why);
        return;
    }
    ++g_w.compared;
    if (engineFirst) {
        g_w.engineColdNs += engNs;
        g_w.portWarmNs += portNs;
        ++g_w.comparedEngineFirst;
    } else {
        g_w.portNs += portNs;
        g_w.engineNs += engNs;
    }
    bool diff = false;
    std::string what;
    for (int k = 0; k < 4; ++k) {
        const U8* list = static_cast<U8*>(db) + listOff(kL[k]);
        const std::int32_t after = rd<std::int32_t>(list, 0x10);
        const RawOp* data = static_cast<const RawOp*>(rdp(list, 8));
        std::vector<RawOp> mineL;
        for (const OpOut& o : mine)
            if (o.list == kL[k]) mineL.push_back(o.op);
        const std::int32_t added = after - before[k];
        g_w.opsCompared += static_cast<std::uint64_t>(std::max(added, 0));
        if (added != static_cast<std::int32_t>(mineL.size())) {
            diff = true;
            if (what.empty()) what = std::format("list {}: engine added {} ops, the port {}", name(static_cast<List>(kL[k])), added, mineL.size());
            continue;
        }
        for (std::int32_t i = 0; i < added; ++i) {
            if (!sameBytes(data[before[k] + i], mineL[static_cast<std::size_t>(i)])) {
                diff = true;
                if (what.empty()) what = std::format("list {} op {}: engine {} / port {}", name(static_cast<List>(kL[k])), i, opText(data[before[k] + i]), opText(mineL[static_cast<std::size_t>(i)]));
                break;
            }
        }
    }
    // the LRU: per cache, the head holds the touched resources in the expected order, each stamped with the cache's clock
    {
        std::unordered_map<void*, std::vector<const void*>> byCache;
        for (const auto& t : g_touches) byCache[t.first].push_back(t.second);
        std::vector<const void*> head;
        for (const auto& kv : byCache) {
            expectedHead(kv.second, head);
            const std::uint32_t clock = rd<std::uint32_t>(kv.first, 0x28);
            const void* r = rdp(kv.first, 0x18);
            ++g_w.lruChecked;
            bool bad = false;
            for (const void* e : head) {
                if (r != e || rd<std::uint32_t>(r, 0x20) != clock) {
                    bad = true;
                    break;
                }
                r = rdp(r, 0x10);
            }
            if (bad) {
                ++g_w.lruDiffering;
                diff = true;
                if (what.empty()) what = std::format("LRU of cache {}: the head does not hold the {} expected resources", kv.first, head.size());
            }
        }
    }
    if (diff) {
        ++g_w.differing;
        if (g_diffLog > 0) {
            --g_diffLog;
            spdlog::warn("native: own collection: DIFFERING context {} (resource {}, {} sub-meshes, mask 0x{:x}, fade {:.4f}): {}", ctx, p.res, p.n, m, fade, what);
        }
    }
}

// the request of one element (eCFrustumElementItem::Request, ported literally) for the main database
bool requestBody(void* item, void* db, std::uint16_t mask, float fade) {
    if ((rd<std::uint16_t>(item, 0x10) & mask) == 0) return true;
    const bool timed = g_pass.mode == 2;
    const std::uint64_t te0 = timed ? nowNs() : 0;
    if ((mask & 0x80u) != 0) {
        if (void* ps = const_cast<void*>(rdp(item, 0x20)); ps != nullptr) {
            g_updateIllum(ps);
            if (timed) {
                g_illumNs += nowNs() - te0;
                ++g_illumCalls;
            }
        }
    }
    void* ctx = const_cast<void*>(rdp(item, 0x18));
    if (ctx == nullptr || !(*g_epsilon <= fade)) return true;
    if ((mask & 0x40u) != 0) {
        const std::uint32_t frame = rd<std::uint32_t>(db, 8);
        std::memcpy(static_cast<U8*>(item) + 0xc, &frame, sizeof(frame));
    }
    ++g_w.elements;
    do {
        const void* const* vt = static_cast<const void* const*>(rdp(ctx, 0));
        const std::uint16_t f = rd<std::uint16_t>(ctx, 0x20);
        if ((f & mask & 1u) != 0) {
            const std::uint64_t tl = timed ? nowNs() : 0;
            slotFn<LodFn>(vt[0x30 / 8])(ctx, db);
            if (timed) g_classCost[vt].lodNs += nowNs() - tl;
        }
        const auto m = static_cast<std::uint16_t>(f & mask);
        const auto engineSlot = slotFn<CtxRequestFn>(vt[0x40 / 8]);
        ++g_w.contexts;
        if (g_pass.mode == 2) {
            compareCtx(ctx, db, fade, m, engineSlot);
        } else {
            static Prepared p;  // (engine thread only, not re-entered: no per-context initialisation of the 64 sub-mesh slots)
            // (the cost of the port, prepare included, sampled on every 8th context)
            const bool sample = (g_w.contexts & 7u) == 0;
            const std::uint64_t t0 = sample ? nowNs() : 0;
            const Why why = prepare(ctx, db, p);
            if (why == Why::Ok) {
                livePort(ctx, db, fade, m, p);
                if (sample) {
                    g_w.liveNs += nowNs() - t0;
                    ++g_w.liveSampled;
                }
                ++g_w.ported;
            } else {
                countWhy(why);
                engineSlot(ctx, db, fade, m);
            }
        }
        ctx = const_cast<void*>(rdp(ctx, 8));
    } while (ctx != nullptr);
    if (timed) {
        g_elemNs += nowNs() - te0;
        ++g_elemTimed;
    }
    return true;
}

bool guardedBody(void* item, void* db, std::uint16_t mask, float fade, bool& ok) {
    __try {
        ok = true;
        return requestBody(item, db, mask, fade);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        ok = false;
        return true;
    }
}

void beginPass(void* db) {
    const std::uint32_t fc = rd<std::uint32_t>(db, 8);
    if (g_pass.db == db && g_pass.frameCounter == fc) return;
    if (g_pass.db != nullptr && g_pass.mode == 1 && g_pass.defer) {
        for (int l = 0; l < 2; ++l)
            if (!g_pass.consumed[l] && !g_pass.deferred[l].empty()) ++g_w.unconsumed;
    }
    g_pass.db = db;
    g_pass.frameCounter = fc;
    g_pass.deferred[0].clear();
    g_pass.deferred[1].clear();
    g_pass.consumed[0] = g_pass.consumed[1] = false;
    const int want = settingMode();
    if (want != g_lastSetting) {
        g_lastSetting = want;
        g_trippedMode = 0;
    }
    int mode = want == 0 || g_trippedMode != 0 ? 0 : want == 2 ? 2 : 1;
    g_pass.defer = false;
    if (mode == 1) {
        // the frame the view will be recorded in (the device's frame counter at the main view's Execute): the presents since the last view
        const std::uint64_t frame = g_frameAtView + (g_presents - g_presentsAtView);
        const sc::LeanFrame k = sc::leanFrameKind(frame, g_checkInterval);
        if (k != sc::LeanFrame::Lean) mode = 2;  // (check / warm-up frames: the engine requests, the port is compared)
        else g_pass.defer = g_gateOpen;
    }
    g_pass.mode = mode;
    ++g_w.passes;
    if (mode == 1) ++g_w.livePasses;
    if (mode == 2) ++g_w.comparePasses;
    if (g_pass.defer) ++g_w.deferPasses;
}

bool detourRequest(void* item, void* db, std::uint16_t mask, float fade) {
    if (db == nullptr || db != g_mainDb || item == nullptr) return g_request.original(item, db, mask, fade);
    beginPass(db);
    if (g_pass.mode == 0) return g_request.original(item, db, mask, fade);
    bool ok = true;
    const bool r = guardedBody(item, db, mask, fade, ok);
    if (!ok) {
        ++g_w.faults;
        g_trippedMode = 1;
        spdlog::error("native: own collection: a fault in the native request of element {} (database {}): the stage is off until the setting changes", item, db);
    }
    return r;
}

}  // namespace

bool install() {
    const engine::Symbols& s = engine::sym();
    const HMODULE m = GetModuleHandleW(L"Engine.dll");
    if (m == nullptr) return false;
    const auto base = reinterpret_cast<std::uintptr_t>(m);
    // FUN_1800ae210 (bTValArray<eCRenderOp2>::SetCount): mov [rsp+0x20], rbp; push rsi; sub rsp, 0x40; mov esi, edx; mov rbp, rcx; test edx, edx
    static constexpr std::uint8_t kSetCount[16] = {0x48, 0x89, 0x6c, 0x24, 0x20, 0x56, 0x48, 0x83, 0xec, 0x40, 0x8b, 0xf2, 0x48, 0x8b, 0xe9, 0x85};
    std::uint8_t b[16] = {};
    if (!engine::safeRead(reinterpret_cast<const void*>(base + 0xae210), b, sizeof(b)) || std::memcmp(b, kSetCount, sizeof(b)) != 0) {
        spdlog::warn("native: own collection: the op array's SetCount (0xae210) does not match: not installed");
        return false;
    }
    g_setCount = reinterpret_cast<SetCountFn>(base + 0xae210);
    g_cacheResource = reinterpret_cast<CacheResFn>(GetProcAddress(m, "?CacheResource@eCResourceCache2@@AEAAXPEAVeCResource2@@@Z"));
    g_updateIllum = reinterpret_cast<IllumFn>(s.illumPsUpdateIllumination);
    g_meshVtable = s.ctxMeshVftable;
    const auto req = reinterpret_cast<RequestFn>(GetProcAddress(m, "?Request@eCFrustumElementItem@@QEAA_NAEAVeCFrustumDatabase@@GM@Z"));
    if (const HMODULE sb = GetModuleHandleW(L"SharedBase.dll")) g_epsilon = reinterpret_cast<const float*>(GetProcAddress(sb, "g_fEpsilon"));
    if (g_cacheResource == nullptr || g_updateIllum == nullptr || g_meshVtable == nullptr || req == nullptr || g_epsilon == nullptr) {
        spdlog::warn("native: own collection: an engine symbol is missing: not installed");
        return false;
    }
    g_one = *reinterpret_cast<const float*>(base + 0x9f1698);  // DAT_1809f1698
    if (!g_request.install("eCFrustumElementItem::Request (F10-1)", req, &detourRequest)) return false;
    g_installed = true;
    spdlog::info("native: own collection: installed (eCFrustumElementItem::Request; one {:.3f}, epsilon {:.3g})", g_one, *g_epsilon);
    return true;
}

void noteMainView(void* db, bool ownedFrame, bool gateOpen, std::uint64_t frame, int checkInterval) {
    if (g_pass.db == db && g_pass.mode == 1 && g_pass.defer != ownedFrame) ++g_w.predictWrong;
    g_mainDb = db;
    g_gateOpen = gateOpen;
    g_checkInterval = checkInterval;
    g_frameAtView = frame;
    g_presentsAtView = g_presents;
}

bool takeDeferred(void* db, std::uint8_t list, std::vector<Deferred>& out) {
    out.clear();
    if (list > 1 || db != g_pass.db || g_pass.mode != 1 || !g_pass.defer || g_pass.consumed[list]) return false;
    if (rd<std::uint32_t>(db, 8) != g_pass.frameCounter) return false;  // (another pass of the database: the ops are stale)
    g_pass.consumed[list] = true;
    out.swap(g_pass.deferred[list]);
    if (!out.empty()) {
        g_w.merged += out.size();
        ++g_w.mergedViews;
    }
    return !out.empty();
}

bool listSetCount(void* db, std::uint8_t list, std::int32_t count) {
    if (g_setCount == nullptr || db == nullptr) return false;
    g_setCount(static_cast<U8*>(db) + listOff(list), count, 0);
    return true;
}

void onPresent() {
    ++g_presents;
    if (!g_installed) return;
    const auto now = std::chrono::steady_clock::now();
    if (std::chrono::duration<double>(now - g_lastReport).count() < 5.0) return;
    g_lastReport = now;
    Window& w = g_w;
    g_runDiffering += w.differing;
    g_runCompared += w.compared;
    g_runActed += w.ported;
    g_runDeferred += w.opsDeferred;
    if (w.differing != 0 && g_trippedMode == 0 && settingMode() == 1) {
        g_trippedMode = 1;
        spdlog::warn("native: own collection: DIFFERING in the window: the stage is off until the setting changes");
    }
    if (w.passes != 0) {
        const double cpe = static_cast<double>(std::max<std::uint64_t>(w.comparedEngineFirst, 1));
        const double cpp = static_cast<double>(std::max<std::uint64_t>(w.compared - w.comparedEngineFirst, 1));
        spdlog::info("native: own collection (5 s): setting {}, {} main request passes (live {}, deferring {}, compare {}); {} elements, {} contexts: ported {} live, engine {} (not a mesh context {}, hierarchy {}, "
                     "render instances {}, not resident {}, no resource {}, > {} sub-meshes {}); ops: {} z / solid deferred, {} written to the engine's lists; deferred ops merged at Execute {} in {} views, "
                     "unconsumed passes {}, prediction wrong {}; compare: {} contexts, {} ops, DIFFERING {}, LRU heads {} checked {} differing; faults {}; cost per context: port {:.2f} us (dry) vs engine "
                     "{:.2f} us (port first) / engine {:.2f} us vs port {:.2f} us (engine first), live port {:.2f} us",
                     settingMode(), w.passes, w.livePasses, w.deferPasses, w.comparePasses, w.elements, w.contexts, w.ported, w.engineCtx, w.notMesh, w.hierarchy, w.renderInstance, w.notResident,
                     w.noResource, kMaxSubs, w.tooMany, w.opsDeferred, w.opsWritten, w.merged, w.mergedViews, w.unconsumed, w.predictWrong, w.compared, w.opsCompared, w.differing, w.lruChecked,
                     w.lruDiffering, w.faults, static_cast<double>(w.portNs) / cpp / 1e3, static_cast<double>(w.engineNs) / cpp / 1e3,
                     static_cast<double>(w.engineColdNs) / cpe / 1e3, static_cast<double>(w.portWarmNs) / cpe / 1e3,
                     static_cast<double>(w.liveNs) / static_cast<double>(std::max<std::uint64_t>(w.liveSampled, 1)) / 1e3);
    }
    if (!g_classCost.empty()) {
        std::vector<std::pair<const void*, ClassCost>> v(g_classCost.begin(), g_classCost.end());
        std::sort(v.begin(), v.end(), [](const auto& a, const auto& b) { return a.second.slotNs + a.second.lodNs > b.second.slotNs + b.second.lodNs; });
        const HMODULE eng = GetModuleHandleW(L"Engine.dll");
        const auto eb = reinterpret_cast<std::uintptr_t>(eng);
        std::string t;
        std::uint64_t slotAll = 0, lodAll = 0;
        for (const auto& kv : v) {
            slotAll += kv.second.slotNs;
            lodAll += kv.second.lodNs;
        }
        for (std::size_t i = 0; i < v.size() && i < 10; ++i) {
            const ClassCost& c = v[i].second;
            t += std::format("{}vt 0x{:x}: {} ctx ({} eligible) slot {:.0f} us + LOD {:.0f} us", i ? "; " : "", reinterpret_cast<std::uintptr_t>(v[i].first) - eb, c.n, c.eligible, c.slotNs / 1e3, c.lodNs / 1e3);
        }
        const double np = static_cast<double>(std::max<std::uint64_t>(w.comparePasses, 1));
        spdlog::info("native: own collection cost (5 s, compare passes {}): per view: element requests {:.0f} us total ({:.0f} elements), illumination updates {:.0f} us ({:.0f} calls), LOD updates {:.0f} us, "
                     "context RequestRenderOps {:.0f} us; by class (window totals): {}",
                     w.comparePasses, g_elemNs / np / 1e3, g_elemTimed / np, g_illumNs / np / 1e3, g_illumCalls / np, lodAll / np / 1e3, slotAll / np / 1e3, t);
        g_classCost.clear();
        g_illumNs = g_illumCalls = g_elemNs = g_elemTimed = 0;
    }
    w = Window{};
    g_diffLog = 8;
}

void* mainDb() { return g_mainDb; }

bool nextViewIsCheck() {
    const std::uint64_t frame = g_frameAtView + (g_presents - g_presentsAtView);
    return sc::leanFrameKind(frame, g_checkInterval) != sc::LeanFrame::Lean;
}

StageState stageState() {
    StageState s;
    s.setting = settingMode();
    s.armed = g_installed && g_trippedMode == 0 ? s.setting : 0;
    s.acted = g_runActed;
    s.deferred = g_runDeferred;
    s.compared = g_runCompared;
    s.differing = g_runDiffering;
    return s;
}

}  // namespace rvk::native::f10req
