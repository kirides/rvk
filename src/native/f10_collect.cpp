#include "native/f10_collect.h"

#include <windows.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <unordered_map>
#include <unordered_set>

#include <spdlog/spdlog.h>

#include "core/hooks.h"
#include "engine/lod.h"
#include "engine/scene_table.h"
#include "native/f10_math.h"

// Engine.dll RVAs (all decompiled; section 7.31): eCFrustumDatabase::Request 0x66e8c0, ExtractFromFrustum 0x66fa60, eCFrustumElementItem::CalculateFadeOut 0x671220,
// eCRenderContextMesh::UpdateLoD 0x5c74b0 (the vtable slot +0x30 of mesh contexts), OnQueryNodes 0x66d960, the sector tree's query walker 0x15c4a0.
namespace rvk::native::f10 {

namespace {

constexpr std::uintptr_t kRvaGetRoom = 0x6714c0;  // eCFrustumElementItem::GetPortalRoom(item)
constexpr std::uintptr_t kRvaRequest = 0x66e8c0, kRvaExtract = 0x66fa60, kRvaFade = 0x671220, kRvaUpdateLoD = 0x5c74b0, kRvaOnQueryNodes = 0x66d960, kRvaTreeWalk = 0x15c4a0;
constexpr std::uintptr_t kRvaNearFraction = 0x9f1674, kRvaOne = 0x9f1698;  // DAT_1809f1674 / DAT_1809f1698 (floats)

// ---- engine layout (decompiled) ----
// eCGeometryEntity: +0x28 flags, +0xc8 base fade, +0xcc ViewRange, +0xd8 the eCFrustumElementItem (+0 entity, +8 extract stamp, +0xc visible stamp, +0x10 mask u16,
// +0x18 first context, +0x20 illumination PS), vtable +0x260 box, +0x268 sphere. Context: +8 next, +0x10 entity, +0x18 mesh PS, +0x20 mask u16, +0x28 LOD
// resource, +0x34 hierarchy level. eCMesh_PS: +0x40 resources, +0x60 ranges, +0x6c count. Resource: +0x10 / +0x18 LRU links, +0x20 last use, +0x24 busy, +0x30 cache,
// +0x38 state, +0x58 sub-mesh table (0x48 each: +0x38 material, +0x40 node table), +0x60 sub-mesh count; cache +0x28 the clock.
// eCFrustumDatabase: +8 frame counter, +0x18 element array, +0x20 count, +0x28 filter (+0x10 camera, +0x44 radius, +0x48 centre, +0x54 aabb min, +0x60 aabb max, +0xf0 beam tree),
// +0x118 the beam tree (= the filter's), +0x14c0 camera (+0x90 position), +0x15b4 view range factor, byte +0x15a9 no view range, +0x15b8 extract filter callback.
constexpr std::size_t kLists[] = {0x14d0, 0x14e8, 0x1500, 0x1518, 0x1530, 0x1548, 0x1560, 0x1578};

using U8 = const std::uint8_t;
template <class T>
inline T rd(const void* base, std::size_t off) {
    T v;
    std::memcpy(&v, static_cast<U8*>(base) + off, sizeof(T));
    return v;
}
inline const void* rdp(const void* base, std::size_t off) { return rd<const void*>(base, off); }

inline std::uint64_t qpc() {
    LARGE_INTEGER v;
    QueryPerformanceCounter(&v);
    return static_cast<std::uint64_t>(v.QuadPart);
}
inline double qpcUs(std::uint64_t d) {
    static const double f = [] {
        LARGE_INTEGER q;
        QueryPerformanceFrequency(&q);
        return 1e6 / static_cast<double>(q.QuadPart);
    }();
    return static_cast<double>(d) * f;
}

bool g_enabled = false;
unsigned g_f10Exp = 0;  // RVK_F10_EXP bit mask: experiments of the stage (1 = the room path also applies CullNode)
bool g_dead = false;  // a fault in the compare: switched off
std::uintptr_t g_base = 0;
float g_epsilon = 1e-6f, g_nearFraction = 0.8f, g_one = 1.0f;

using DbFn = void (*)(void*);
using TreeFn = void (*)(std::int64_t, void*, void*);
using FadeFn = float (*)(const void*, const void*);
using RoomFn = const void* (*)(const void*);
RoomFn g_getRoom = nullptr;
std::unordered_set<const void*> g_engineRooms;  // portal rooms of the elements the engine extracted this view (STAGE 0: room visibility is taken from the engine, not derived)
Hook<DbFn> g_request, g_extract;
Hook<TreeFn> g_treeWalk;
FadeFn g_engineFade = nullptr;
std::uintptr_t g_meshLodFn = 0;
std::uint64_t g_extractNs = 0, g_requestNs = 0;  // of the current Request (extract runs inside it)
const void* g_tree = nullptr;
std::uint32_t g_leafMask = 0, g_leafValue = 0;  // of the database being compared (set per view from g_leafByDb)
std::unordered_map<const void*, std::pair<std::uint32_t, std::uint32_t>> g_leafByDb;  // the query's mask / value per database (ctx +0x10 / +0x14)

// ---- the persistent scene ----
struct Cache {
    std::uint64_t stamp = 0;  // scenetable::stamp at the last refresh; 0 = free
    float sph[4]{};           // radius, centre
    float box[6]{};
    float range = 0.0f, base = 0.0f;
    std::uint32_t flags = 0;
    std::uint32_t nodeFlags = 0xffffffffu;  // the sector tree leaf's flags (node +0x18), read from the engine's tree each compared view (0xffffffff = no leaf): DIAGNOSTIC input, not yet derived natively
    std::uint32_t ctxHead = 0;  // 1 when the entity has contexts (+0xf0 != 0)
    const void* entity = nullptr;
    const void* room = nullptr;  // eCFrustumElementItem::GetPortalRoom
};
std::vector<Cache> g_scene;
std::uint32_t g_scrubPos = 0;

bool readLive(const void* e, Cache& c) {
    c.entity = e;
    c.flags = rd<std::uint32_t>(e, 0x28);
    c.base = rd<float>(e, 0xc8);
    c.range = rd<float>(e, 0xcc);
    c.ctxHead = rdp(e, 0xf0) != nullptr ? 1u : 0u;
    c.room = g_getRoom != nullptr ? g_getRoom(static_cast<U8*>(const_cast<void*>(e)) + 0xd8) : nullptr;
    const auto vt = static_cast<const void* const*>(rdp(e, 0));
    using GetFn = const float* (*)(const void*);
    const float* box = reinterpret_cast<GetFn>(vt[0x260 / 8])(e);
    const float* sph = reinterpret_cast<GetFn>(vt[0x268 / 8])(e);
    std::memcpy(c.box, box, sizeof(c.box));
    std::memcpy(c.sph, sph, sizeof(c.sph));
    return true;
}
bool sameLive(const Cache& a, const Cache& b) {
    return a.flags == b.flags && a.ctxHead == b.ctxHead && a.base == b.base && a.range == b.range && std::memcmp(a.box, b.box, sizeof(a.box)) == 0 && std::memcmp(a.sph, b.sph, sizeof(a.sph)) == 0;
}

// ---- statistics ----
enum Cause : int { kCauseStale, kCauseTreeSphere, kCauseLeafMask, kCauseNoNode, kCauseFilterCb, kCauseOther, kCauses };
constexpr const char* kCauseNames[] = {"stale cache (flags / bounds changed without a signal)", "tree leaf sphere != entity sphere", "leaf flag mask (node flags)", "no tree node", "extract filter callback", "unexplained"};

struct ClassStats {
    std::uint64_t engine = 0, native = 0, missing = 0, extra = 0, fadeCommon = 0, fadeDiff = 0, reqDiff = 0, lodCommon = 0, lodDiff = 0;
};
struct Window {
    std::uint64_t frames = 0, frameDiffFree = 0, elemEngine = 0, elemNative = 0, missing = 0, extra = 0, unknown = 0, requestedEngine = 0, requestedNative = 0, reqDiff = 0;
    std::uint64_t fadeCommon = 0, fadeDiff = 0, lodCommon = 0, lodDiff = 0, lodSingle = 0, ctxNonMesh = 0;
    std::uint64_t nodePairs = 0, nodeEq = 0, nodeDiff = 0, nativeOnlyCtx = 0, engineOnlyCtx = 0;
    std::uint64_t cause[2][kCauses]{};
    std::uint64_t scrubChecked = 0, scrubStale = 0, refreshed = 0, filterCb = 0, portalMode = 0;
    double fadeMaxDelta = 0.0;
    // timers (us summed)
    double engineExtractUs = 0, engineRequestUs = 0, nativeCullUs = 0, nativeFadeLodUs = 0, nativeNodesUs = 0, maintUs = 0, compareUs = 0;
    // M3 snapshot
    std::uint64_t snapElems = 0, snapStamped = 0, snapResources = 0, snapTouched = 0, snapBusy = 0, snapIllumDirty = 0, snapNotResident = 0, snapDigest = 0;
    std::map<std::uint32_t, ClassStats> byClass;  // key = context vtable RVA (0 = no context)
};
struct DbState {
    Window w;
    std::uint64_t calls = 0;
};
std::unordered_map<const void*, DbState> g_dbs;
std::chrono::steady_clock::time_point g_lastReport = std::chrono::steady_clock::now();
int g_diffLogBudget = 24;
std::vector<SideEffectRec> g_snapshot;
std::uint64_t g_frameNo = 0;

inline std::uint32_t classOf(const void* entity) {
    const void* ctx = rdp(entity, 0xf0);
    if (ctx == nullptr) return 0;
    const auto vt = rd<std::uintptr_t>(ctx, 0);
    return static_cast<std::uint32_t>(vt - g_base);
}
inline bool isMeshCtx(const void* ctx) {
    const auto vt = static_cast<const std::uintptr_t*>(rdp(ctx, 0));
    return vt[6] == g_meshLodFn;
}

Filter readFilter(const void* db) {
    Filter f;
    const U8* d = static_cast<U8*>(db);
    const U8* filter = d + 0x28;
    const U8* tree = d + 0x118;
    f.beam.root = static_cast<const BeamNode*>(rdp(tree, 0));
    f.beam.planes = static_cast<const BeamPlane*>(rdp(tree, 0x12d8));
    f.beam.cornerIn = reinterpret_cast<const std::uint32_t*>(tree + 0x12e8);
    f.beam.cornerOut = reinterpret_cast<const std::uint32_t*>(tree + 0x1348);
    f.sphereRadius = rd<float>(filter, 0x44);
    std::memcpy(f.sphereCenter, filter + 0x48, 12);
    std::memcpy(f.aabbMin, filter + 0x54, 12);
    std::memcpy(f.aabbMax, filter + 0x60, 12);
    const U8* cam = static_cast<U8*>(const_cast<void*>(rdp(db, 0x14c0)));
    std::memcpy(f.camera, cam + 0x90, 12);
    return f;
}

struct NativeVis {
    std::uint32_t slot;
    int status;
    float fade;
};
struct NativeCtx {
    const void* ctx;
    int chosen;  // LOD index picked (-1 = none resident / single LOD)
    bool multi;  // the UpdateLoD ran (>= 2 LODs)
    std::uint32_t nodeBegin, nodeCount;  // into g_nodePool: per sub-mesh: (sub, count, nodes...)
};
std::vector<NativeVis> g_vis;
std::vector<NativeCtx> g_nctx;
std::vector<std::uint32_t> g_nodePool;
std::vector<std::uint8_t> g_engSeen, g_natSeen, g_engReq;

// the native collection of one view (timed pieces are returned)
void nativeCollect(const void* db, const Filter& f, std::uint16_t mask, float factor, bool noRange, bool& cb, double& cullUs, double& fadeLodUs, double& nodesUs) {
    const std::uint64_t t0 = qpc();
    g_vis.clear();
    const auto n = static_cast<std::uint32_t>(g_scene.size());
    cb = rdp(db, 0x15b8) != nullptr;
    const bool portal = static_cast<const std::uint8_t*>(db)[0x15a3] == 1;
    for (std::uint32_t i = 0; i < n; ++i) {
        const Cache& c = g_scene[i];
        if (c.stamp == 0 || c.ctxHead == 0 || !entityExtractable(c.flags)) continue;
        // the sector tree's leaf gate (OnQueryNodes); in portal mode the leaves with node flag bits 1 / 2 are the portal rooms' entities (ExtractFromPortalRooms: no mask, the room's own cull; here the plain cull)
        if (c.nodeFlags == 0xffffffffu) continue;
        if (portal && (c.nodeFlags & 6u) != 0) {
            // ExtractFromPortalRoom: the entity's box against the room's beam (here: the view's final beam, a superset of the room-clipped ones), then ExtractFromEntity status 1 = no CullNode
            if (c.room == nullptr || g_engineRooms.find(c.room) == g_engineRooms.end()) continue;
            if (beamCullBox(f.beam, c.box) == kCulled) continue;
            if (g_f10Exp & 1) {
                const float range = noRange ? 0.0f : c.range * factor;
                if (cullCameraFull(f, c.box, c.sph[0], c.sph + 1, range) == kCulled) continue;
            }
            g_vis.push_back({i, kInside, 0.0f});
            continue;
        } else if ((c.nodeFlags & g_leafMask) != g_leafValue) {
            continue;
        }
        int st = cullCameraLeaf(f, c.sph[0], c.sph + 1);
        if (st == kCulled) continue;
        if (st != kInside) {
            // CullNode: the view range multiplied by the database's factor (0 = the engine's "no range" byte)
            const float range = noRange ? 0.0f : c.range * factor;
            if ((c.flags & 0x8000u) == 0) continue;
            st = cullCameraFull(f, c.box, c.sph[0], c.sph + 1, range);
            if (st == kCulled) continue;
        }
        g_vis.push_back({i, st, 0.0f});
    }
    const std::uint64_t t1 = qpc();
    // fade + request + LOD pick
    g_nctx.clear();
    g_nodePool.clear();
    const float meshScale = engine::lod::scales().meshes;
    std::uint64_t nodeTicks = 0;
    for (NativeVis& v : g_vis) {
        const Cache& c = g_scene[v.slot];
        v.fade = calculateFade(c.base, c.range, factor, noRange, f.camera, c.sph + 1, g_epsilon, g_nearFraction, g_one);
        if (!(v.fade > g_epsilon)) continue;
        const void* item = static_cast<U8*>(c.entity) + 0xd8;
        if ((rd<std::uint16_t>(item, 0x10) & mask) == 0) continue;
        for (const void* ctx = rdp(item, 0x18); ctx != nullptr; ctx = rdp(ctx, 8)) {
            const std::uint16_t cm = rd<std::uint16_t>(ctx, 0x20);
            if (!(cm & mask & 1)) continue;
            NativeCtx nc{ctx, -1, false, 0, 0};
            if (isMeshCtx(ctx)) {
                const void* ps = rdp(ctx, 0x18);
                const int count = rd<std::uint8_t>(ps, 0x6c);
                if (count >= 2 && count <= 4) {
                    float ranges[4];
                    for (int k = 0; k < count - 1; ++k) ranges[k] = rd<float>(ps, 0x60 + 4 * k) * meshScale;
                    const LodPick pick = pickLod(count, ranges, f.camera, c.sph + 1, INFINITY, [&](int i) { return rd<std::int32_t>(rdp(ps, 0x40 + 8 * static_cast<std::size_t>(i)), 0x38) == 3; });
                    nc.chosen = pick.chosen;
                    nc.multi = true;
                }
                // hierarchy node sets of the resident resource this view will use (the engine's pick: the compare reads ctx +0x28; the native pick otherwise)
                const int level = rd<std::int32_t>(ctx, 0x34);
                if (level > 0) {
                    const std::uint64_t n0 = qpc();
                    const void* res = nc.multi ? (nc.chosen >= 0 ? rdp(ps, 0x40 + 8 * static_cast<std::size_t>(nc.chosen)) : nullptr) : rdp(ctx, 0x28);
                    if (res != nullptr && rd<std::int32_t>(res, 0x38) == 3) {
                        nc.nodeBegin = static_cast<std::uint32_t>(g_nodePool.size());
                        const int subs = rd<std::int32_t>(res, 0x60);
                        const U8* table = static_cast<U8*>(const_cast<void*>(rdp(res, 0x58)));
                        for (int s = 0; s < subs && s < 64; ++s) {
                            const void* nodes = rdp(table + 0x48 * static_cast<std::size_t>(s), 0x40);
                            const std::size_t at = g_nodePool.size();
                            g_nodePool.push_back(static_cast<std::uint32_t>(s));
                            g_nodePool.push_back(0);
                            if (rdp(table + 0x48 * static_cast<std::size_t>(s), 0x38) != nullptr && nodes != nullptr) {
                                std::uint32_t cnt = 0;
                                walkNodes(static_cast<const RawNode*>(nodes), level, [&](const float* box) { return cullCameraBox(f, box); },
                                          [&](std::uint32_t idx) {
                                              g_nodePool.push_back(idx);
                                              ++cnt;
                                          });
                                g_nodePool[at + 1] = cnt;
                            }
                        }
                        nc.nodeCount = static_cast<std::uint32_t>(g_nodePool.size()) - nc.nodeBegin;
                    }
                    nodeTicks += qpc() - n0;
                }
            }
            g_nctx.push_back(nc);
        }
    }
    const std::uint64_t t2 = qpc();
    cullUs = qpcUs(t1 - t0);
    nodesUs = qpcUs(nodeTicks);
    fadeLodUs = qpcUs(t2 - t1) - nodesUs;
}

// the beam tree: leaf lookups for the diagnosis (built lazily, once per compared frame with differences)
struct NodeInfo {
    float sph[4];
    std::uint32_t flags;
};
std::unordered_map<const void*, NodeInfo> g_nodes;
bool g_nodesBuilt = false;
void buildNodes() {
    g_nodesBuilt = true;
    g_nodes.clear();
    if (g_tree == nullptr) return;
    const U8* nodes = static_cast<U8*>(const_cast<void*>(rdp(g_tree, 0x30)));
    std::vector<std::int64_t> stack{rd<std::int64_t>(g_tree, 0x20)};
    while (!stack.empty() && g_nodes.size() < (1u << 20)) {
        const std::int64_t idx = stack.back();
        stack.pop_back();
        const U8* n = nodes + idx * 0x58;
        for (std::int64_t ch = rd<std::int64_t>(n, 0x40); ch != -1;) {
            const U8* c = nodes + ch * 0x58;
            if (rd<std::uint8_t>(c, 0x50) & 4) {
                NodeInfo ni;
                std::memcpy(ni.sph, c, 16);
                ni.flags = rd<std::uint32_t>(c, 0x18);
                g_nodes[rdp(c, 0x10)] = ni;
            }
            if (rd<std::int64_t>(c, 0x40) != -1) stack.push_back(ch);
            ch = rd<std::int64_t>(c, 0x30);
        }
    }
}

const char* describe(char* buf, std::size_t n, const void* db, std::uint32_t slot, bool inEngine, const Filter& f, float factor, bool noRange, Cause& cause) {
    const Cache& c = g_scene[slot];
    Cache live;
    readLive(c.entity, live);
    const bool stale = !sameLive(c, live);
    // native verdict with the live values
    auto verdict = [&](const Cache& k) {
        if (k.ctxHead == 0 || !entityExtractable(k.flags)) return false;
        int st = cullCameraLeaf(f, k.sph[0], k.sph + 1);
        if (st == kCulled) return false;
        if (st != kInside) {
            if ((k.flags & 0x8000u) == 0) return false;
            st = cullCameraFull(f, k.box, k.sph[0], k.sph + 1, noRange ? 0.0f : k.range * factor);
            if (st == kCulled) return false;
        }
        return true;
    };
    const bool nativeCached = verdict(c), nativeLive = verdict(live);
    if (!g_nodesBuilt) buildNodes();
    const auto it = g_nodes.find(c.entity);
    const char* nodeNote = "no node";
    cause = kCauseOther;
    int nodeStatus = -1;
    if (it != g_nodes.end()) {
        nodeNote = "node";
        nodeStatus = cullCameraLeaf(f, it->second.sph[0], it->second.sph + 1);
    }
    if (stale && nativeLive == inEngine) cause = kCauseStale;
    else if (it == g_nodes.end()) cause = kCauseNoNode;
    else if (std::memcmp(it->second.sph, live.sph, sizeof(live.sph)) != 0) cause = kCauseTreeSphere;
    else if ((it->second.flags & g_leafMask) != g_leafValue) cause = kCauseLeafMask;
    else if (rdp(db, 0x15b8) != nullptr) cause = kCauseFilterCb;
    const float dx = live.sph[1] - f.camera[0], dy = live.sph[2] - f.camera[1], dz = live.sph[3] - f.camera[2];
    std::snprintf(buf, n,
                  "%s entity %p room %p (visible-set %d, beam %d) class 0x%x flags 0x%x (live 0x%x) ctx %u native cached %d live %d, %s%s flags 0x%x leaf status %d, sphere r %.1f d %.0f range %.0f -> %s", inEngine ? "missing" : "extra",
                  c.entity, c.room, c.room != nullptr && g_engineRooms.count(c.room) ? 1 : 0, beamCullBox(f.beam, c.box), classOf(c.entity), c.flags, live.flags, c.ctxHead, nativeCached ? 1 : 0, nativeLive ? 1 : 0, nodeNote, stale ? " STALE" : "", it != g_nodes.end() ? it->second.flags : 0u, nodeStatus,
                  live.sph[0], std::sqrt(dx * dx + dy * dy + dz * dz), live.range * (noRange ? 0.0f : factor), kCauseNames[cause]);
    return buf;
}

std::uint64_t mix(std::uint64_t h, std::uint64_t v) {
    h ^= v + 0x9E3779B97F4A7C15ull + (h << 6) + (h >> 2);
    return h;
}

// the compare of one view; db = the view's database, called right after the engine's Request
void compareView(void* db) {
    DbState& st = g_dbs[db];
    Window& w = st.w;
    ++st.calls;
    ++w.frames;
    ++g_frameNo;
    if (g_base == 0) return;
    const std::uint64_t tm = qpc();

    // -- maintenance: the scene follows the table's slots (refresh = stamp changed, + a rolling scrub of live reads)
    const std::uint32_t cap = engine::scenetable::capacity();
    if (g_scene.size() < cap) g_scene.resize(cap);
    for (std::uint32_t i = 0; i < cap; ++i) {
        const std::uint64_t s = engine::scenetable::stamp(i);
        Cache& c = g_scene[i];
        if (s == c.stamp) continue;
        c.stamp = s;
        ++w.refreshed;
        if (s == 0) {
            c.entity = nullptr;
            c.ctxHead = 0;
            continue;
        }
        readLive(engine::scenetable::slot(i)->entity, c);
    }
    std::uint32_t scrub = std::max<std::uint32_t>(16, cap / 32);
    for (std::uint32_t k = 0; k < scrub && cap != 0; ++k) {
        const std::uint32_t i = (g_scrubPos++) % cap;
        Cache& c = g_scene[i];
        if (c.stamp == 0) continue;
        Cache live;
        readLive(c.entity, live);
        live.stamp = c.stamp;
        ++w.scrubChecked;
        if (!sameLive(c, live)) {
            ++w.scrubStale;
            c = live;
        }
    }
    w.maintUs += qpcUs(qpc() - tm);

    // -- the leaf flags of the engine's sector tree (diagnostic input)
    if (const auto lf = g_leafByDb.find(db); lf != g_leafByDb.end()) {
        g_leafMask = lf->second.first;
        g_leafValue = lf->second.second;
    }
    g_nodesBuilt = false;
    buildNodes();
    for (std::uint32_t i = 0; i < cap; ++i) {
        Cache& c = g_scene[i];
        if (c.stamp == 0) continue;
        const auto it = g_nodes.find(c.entity);
        c.nodeFlags = it != g_nodes.end() ? it->second.flags : 0xffffffffu;
    }

    {
        static bool logged = false;
        if (!logged && g_tree != nullptr) {
            logged = true;
            spdlog::info("native: f10 sector tree leaf gate: node flags & 0x{:x} == 0x{:x}; {} leaves", g_leafMask, g_leafValue, g_nodes.size());
        }
    }

    g_engineRooms.clear();
    {
        const int cnt = rd<std::int32_t>(db, 0x20);
        const U8* lst = static_cast<U8*>(const_cast<void*>(rdp(db, 0x18)));
        for (int i = 0; i < cnt; ++i) {
            const void* it = rdp(lst, 8 * static_cast<std::size_t>(i));
            const std::uint32_t sl = engine::scenetable::find(rdp(it, 0));
            if (sl >= g_scene.size() || sl == UINT32_MAX || (g_scene[sl].nodeFlags & 6u) == 0 || g_scene[sl].nodeFlags == 0xffffffffu) continue;  // only room-path elements name a visible room
            if (const void* room = g_getRoom(it)) g_engineRooms.insert(room);
        }
    }

    // -- the native collection
    const Filter f = readFilter(db);
    const U8* d = static_cast<U8*>(db);
    const float factor = rd<float>(d, 0x15b4);
    const bool noRange = d[0x15a9] != 0;
    const std::uint16_t mask = requestMask(d[0x1591] == 1, d[0x15a7] == 1, d[0x15a5] == 1, d[0x15a1] == 1, d[0x15a8] == 1, d[0x1590] == 1, d[0x15a4] == 1);
    if (d[0x15a3] == 1) ++w.portalMode;
    bool cb = false;
    double cullUs = 0, fadeLodUs = 0, nodesUs = 0;
    nativeCollect(db, f, mask, factor, noRange, cb, cullUs, fadeLodUs, nodesUs);
    w.nativeCullUs += cullUs;
    w.nativeFadeLodUs += fadeLodUs;
    w.nativeNodesUs += nodesUs;
    if (cb) ++w.filterCb;

    // -- the compare (not part of the native cost)
    const std::uint64_t tc = qpc();
    g_engSeen.assign(cap, 0);
    g_natSeen.assign(cap, 0);
    g_engReq.assign(cap, 0);
    std::uint64_t diffsThisFrame = 0;
    char buf[512];

    // native sets
    std::unordered_map<const void*, const NativeCtx*> nctx;
    for (const NativeVis& v : g_vis) g_natSeen[v.slot] = 1;
    for (const NativeCtx& n : g_nctx) nctx[n.ctx] = &n;
    w.elemNative += g_vis.size();

    // engine set
    const int count = rd<std::int32_t>(d, 0x20);
    const U8* list = static_cast<U8*>(const_cast<void*>(rdp(d, 0x18)));
    w.elemEngine += static_cast<std::uint64_t>(count);
    g_snapshot.clear();
    const std::uint32_t frameCounter = rd<std::uint32_t>(d, 8);
    for (int i = 0; i < count; ++i) {
        const void* item = rdp(list, 8 * static_cast<std::size_t>(i));
        const void* entity = rdp(item, 0);
        const std::uint32_t slot = engine::scenetable::find(entity);
        if (slot == UINT32_MAX || slot >= cap) {
            ++w.unknown;
            continue;
        }
        g_engSeen[slot] = 1;
        const std::uint32_t cls = classOf(entity);
        ClassStats& cs = w.byClass[cls];
        ++cs.engine;
        const float eFade = g_engineFade(item, db);
        const bool eReq = eFade > g_epsilon && (rd<std::uint16_t>(item, 0x10) & mask) != 0;
        w.requestedEngine += eReq ? 1 : 0;
        if (!g_natSeen[slot]) {
            ++w.missing;
            ++cs.missing;
            ++diffsThisFrame;
            Cause cause;
            const char* s = describe(buf, sizeof(buf), db, slot, true, f, factor, noRange, cause);
            ++w.cause[0][cause];
            if (g_diffLogBudget > 0) {
                --g_diffLogBudget;
                spdlog::info("native: f10 diff: {}", s);
            }
            continue;
        }
        ++cs.native;
        g_engReq[slot] = eReq ? 1 : 0;
        if (eReq) {
            // M3 snapshot of the engine's request
            SideEffectRec r;
            r.entity = reinterpret_cast<std::uint64_t>(entity);
            r.visibleStamp = rd<std::uint32_t>(item, 0xc) == frameCounter ? 1u : 0u;
            const void* illum = rdp(item, 0x20);
            r.illumDirty = illum != nullptr ? rd<std::uint8_t>(illum, 0x188) : 0;
            int ci = 0;
            for (const void* ctx = rdp(item, 0x18); ctx != nullptr; ctx = rdp(ctx, 8)) {
                if (!isMeshCtx(ctx)) continue;
                const void* res = rdp(ctx, 0x28);
                r.lodRes += reinterpret_cast<std::uint64_t>(res);
                if (res != nullptr) {
                    const std::int32_t state = rd<std::int32_t>(res, 0x38);
                    if (state == 3) r.lodStateMask |= 1u << (ci & 31);
                    r.busySum += rd<std::uint32_t>(res, 0x24);
                    const void* cache = rdp(res, 0x30);
                    const std::uint32_t clock = cache != nullptr ? rd<std::uint32_t>(cache, 0x28) : 0;
                    ++r.resources;
                    if (cache != nullptr && state == 3 && rd<std::uint32_t>(res, 0x20) == clock) ++r.touched;
                    if (state == 3) {
                        const int subs = rd<std::int32_t>(res, 0x60);
                        const U8* table = static_cast<U8*>(const_cast<void*>(rdp(res, 0x58)));
                        for (int s = 0; s < subs && s < 64; ++s) {
                            const void* mat = rdp(table + 0x48 * static_cast<std::size_t>(s), 0x38);
                            if (mat == nullptr) continue;
                            ++r.resources;
                            const void* mc = rdp(mat, 0x30);
                            if (mc != nullptr && rd<std::int32_t>(mat, 0x38) == 3 && rd<std::uint32_t>(mat, 0x20) == rd<std::uint32_t>(mc, 0x28)) ++r.touched;
                            r.busySum += rd<std::uint32_t>(mat, 0x24);
                        }
                    }
                }
                ++ci;
            }
            g_snapshot.push_back(r);
        }
    }
    // extras + fade / requested compare over native elements
    for (const NativeVis& v : g_vis) {
        const Cache& c = g_scene[v.slot];
        const std::uint32_t cls = classOf(c.entity);
        ClassStats& cs = w.byClass[cls];
        const bool nReq = v.fade > g_epsilon && (rd<std::uint16_t>(static_cast<U8*>(c.entity) + 0xd8, 0x10) & mask) != 0;
        w.requestedNative += nReq ? 1 : 0;
        if (!g_engSeen[v.slot]) {
            ++w.extra;
            ++cs.extra;
            ++diffsThisFrame;
            ++cs.native;
            Cause cause;
            const char* s = describe(buf, sizeof(buf), db, v.slot, false, f, factor, noRange, cause);
            ++w.cause[1][cause];
            if (g_diffLogBudget > 0) {
                --g_diffLogBudget;
                spdlog::info("native: f10 diff: {}", s);
            }
            continue;
        }
        const void* item = static_cast<U8*>(c.entity) + 0xd8;
        const float eFade = g_engineFade(item, db);
        ++w.fadeCommon;
        ++cs.fadeCommon;
        const double delta = std::fabs(static_cast<double>(eFade) - static_cast<double>(v.fade));
        if (delta > 1e-4) {
            ++w.fadeDiff;
            ++cs.fadeDiff;
            ++diffsThisFrame;
            if (g_diffLogBudget > 0) {
                --g_diffLogBudget;
                spdlog::info("native: f10 diff: fade entity {} class 0x{:x} engine {:.4f} native {:.4f}", c.entity, cls, eFade, v.fade);
            }
        }
        w.fadeMaxDelta = std::max(w.fadeMaxDelta, delta);
        const bool eReq = eFade > g_epsilon && (rd<std::uint16_t>(item, 0x10) & mask) != 0;
        if (eReq != nReq) {
            ++w.reqDiff;
            ++cs.reqDiff;
            ++diffsThisFrame;
        }
    }
    // LOD picks + hierarchy nodes of the contexts both sides requested
    std::unordered_map<const void*, std::vector<std::uint32_t>> engOps;  // ctx -> descs, only contexts the native side walked nodes for
    {
        std::unordered_map<const void*, bool> wanted;
        for (const NativeCtx& n : g_nctx)
            if (n.nodeCount != 0 || (n.multi == false && rd<std::int32_t>(n.ctx, 0x34) > 0)) wanted[n.ctx] = true;
        std::unordered_map<const void*, bool> hierSeen;
        for (const std::size_t base : kLists) {
            const std::int32_t cnt = rd<std::int32_t>(d, base + 0x10);
            const U8* arr = static_cast<U8*>(const_cast<void*>(rdp(d, base + 8)));
            if (cnt <= 0 || arr == nullptr) continue;
            for (std::int32_t k = 0; k < cnt; ++k) {
                const void* ctx = rdp(arr, 0x18 * static_cast<std::size_t>(k));
                auto h = hierSeen.find(ctx);
                if (h == hierSeen.end()) {
                    bool hier = false;
                    if (ctx != nullptr && isMeshCtx(ctx)) hier = rd<std::int32_t>(ctx, 0x34) > 0;
                    h = hierSeen.emplace(ctx, hier).first;
                }
                if (h->second) engOps[ctx].push_back(rd<std::uint32_t>(arr, 0x18 * static_cast<std::size_t>(k) + 8));
            }
        }
    }
    for (const NativeCtx& n : g_nctx) {
        const void* entity = rdp(n.ctx, 0x10);
        const std::uint32_t slot = engine::scenetable::find(entity);
        if (slot == UINT32_MAX || slot >= cap || !g_engSeen[slot] || !g_engReq[slot]) continue;
        ClassStats& cs = w.byClass[classOf(entity)];
        if (!isMeshCtx(n.ctx)) {
            ++w.ctxNonMesh;
            continue;
        }
        const void* eLod = rdp(n.ctx, 0x28);
        if (!n.multi) {
            ++w.lodSingle;
        } else {
            const void* ps = rdp(n.ctx, 0x18);
            const void* nLod = n.chosen >= 0 ? rdp(ps, 0x40 + 8 * static_cast<std::size_t>(n.chosen)) : nullptr;
            ++w.lodCommon;
            ++cs.lodCommon;
            if (nLod != eLod) {
                ++w.lodDiff;
                ++cs.lodDiff;
                ++diffsThisFrame;
                if (g_diffLogBudget > 0) {
                    --g_diffLogBudget;
                    spdlog::info("native: f10 diff: LOD ctx {} entity {} engine res {} native res {} (picked index {})", n.ctx, entity, eLod, nLod, n.chosen);
                }
            }
        }
        // nodes
        const auto eo = engOps.find(n.ctx);
        if (n.nodeCount == 0) {
            if (eo != engOps.end()) ++w.engineOnlyCtx;
            continue;
        }
        if (eo == engOps.end()) {
            ++w.nativeOnlyCtx;  // no ops: not requested / caps without a list / the LOD resource not resident for the engine
            continue;
        }
        std::map<std::uint32_t, std::vector<std::uint32_t>> engSub;
        for (const std::uint32_t desc : eo->second) engSub[desc & 0xff].push_back(desc >> 8);
        for (auto& kv : engSub) std::sort(kv.second.begin(), kv.second.end());
        for (std::uint32_t at = n.nodeBegin; at < n.nodeBegin + n.nodeCount;) {
            const std::uint32_t sub = g_nodePool[at], cnt = g_nodePool[at + 1];
            std::vector<std::uint32_t> nat(g_nodePool.begin() + at + 2, g_nodePool.begin() + at + 2 + cnt);
            at += 2 + cnt;
            const auto es = engSub.find(sub);
            if (es == engSub.end()) continue;  // no ops of this sub-mesh (caps)
            auto e = es->second;
            e.erase(std::unique(e.begin(), e.end()), e.end());
            std::sort(nat.begin(), nat.end());
            ++w.nodePairs;
            if (e == nat) {
                ++w.nodeEq;
            } else {
                ++w.nodeDiff;
                ++diffsThisFrame;
                if (g_diffLogBudget > 0) {
                    --g_diffLogBudget;
                    spdlog::info("native: f10 diff: nodes ctx {} entity {} sub {} engine {} nodes native {} nodes (LOD engine {} native-pick index {})", n.ctx, entity, sub, e.size(), nat.size(), eLod, n.chosen);
                }
            }
        }
    }
    if (diffsThisFrame == 0) ++w.frameDiffFree;
    // snapshot aggregates
    for (const SideEffectRec& r : g_snapshot) {
        ++w.snapElems;
        w.snapStamped += r.visibleStamp;
        w.snapResources += r.resources;
        w.snapTouched += r.touched;
        w.snapBusy += r.busySum != 0 ? 1 : 0;
        w.snapIllumDirty += r.illumDirty != 0 ? 1 : 0;
        w.snapNotResident += (r.lodRes != 0 && r.lodStateMask == 0) ? 1 : 0;
    }
    w.snapDigest = digest(g_snapshot);
    w.compareUs += qpcUs(qpc() - tc);
}

bool guarded(void (*fn)(void*), void* arg) {
    __try {
        fn(arg);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

void detourRequest(void* db) {
    g_extractNs = 0;
    const std::uint64_t t0 = qpc();
    g_request.original(db);
    const std::uint64_t t1 = qpc();
    if (g_dead || db == nullptr) return;
    Window& w = g_dbs[db].w;
    w.engineRequestUs += qpcUs(t1 - t0);
    const int count = rd<std::int32_t>(db, 0x20);
    if (count < 16) return;  // a view with nothing in it (preview, UI): not the main view
    if (!guarded(&compareView, db)) {
        spdlog::warn("native: f10 compare faulted: switched off");
        g_dead = true;
    }
}

void detourExtract(void* db) {
    const std::uint64_t t0 = qpc();
    g_extract.original(db);
    if (db != nullptr) g_dbs[db].w.engineExtractUs += qpcUs(qpc() - t0);
}

void detourTreeWalk(std::int64_t tree, void* fn, void* ctx) {
    if (reinterpret_cast<std::uintptr_t>(fn) == g_base + kRvaOnQueryNodes && ctx != nullptr) {
        g_tree = reinterpret_cast<const void*>(tree);
        g_leafByDb[rdp(ctx, 0)] = {rd<std::uint32_t>(ctx, 0x10), rd<std::uint32_t>(ctx, 0x14)};
    }
    g_treeWalk.original(tree, fn, ctx);
}

void lazyInit() {
    if (g_base != 0) return;
    const HMODULE m = GetModuleHandleW(L"Engine.dll");
    if (m == nullptr) return;
    g_base = reinterpret_cast<std::uintptr_t>(m);
    g_meshLodFn = g_base + kRvaUpdateLoD;
    g_engineFade = reinterpret_cast<FadeFn>(g_base + kRvaFade);
    g_getRoom = reinterpret_cast<RoomFn>(g_base + kRvaGetRoom);
    g_nearFraction = rd<float>(reinterpret_cast<const void*>(g_base + kRvaNearFraction), 0);
    g_one = rd<float>(reinterpret_cast<const void*>(g_base + kRvaOne), 0);
    if (const HMODULE sb = GetModuleHandleW(L"SharedBase.dll")) {
        if (const auto* p = reinterpret_cast<const float*>(GetProcAddress(sb, "g_fEpsilon"))) g_epsilon = *p;
    }
    spdlog::info("native: f10 compare: epsilon {:.3g}, near fraction {:.3f}, one {:.3f}", g_epsilon, g_nearFraction, g_one);
}

}  // namespace

bool enabled() { return g_enabled; }

SideEffectDiff diffSideEffects(const std::vector<SideEffectRec>& a, const std::vector<SideEffectRec>& b) {
    SideEffectDiff d;
    std::unordered_map<std::uint64_t, const SideEffectRec*> ma;
    for (const auto& r : a) ma[r.entity] = &r;
    std::unordered_map<std::uint64_t, bool> seen;
    for (const auto& r : b) {
        const auto it = ma.find(r.entity);
        if (it == ma.end()) {
            ++d.onlyB;
            continue;
        }
        seen[r.entity] = true;
        ++d.common;
        const SideEffectRec& x = *it->second;
        d.stamp += x.visibleStamp != r.visibleStamp;
        d.lod += x.lodRes != r.lodRes;
        d.state += x.lodStateMask != r.lodStateMask;
        d.busy += x.busySum != r.busySum;
        d.touched += (x.touched != r.touched || x.resources != r.resources);
        d.illum += x.illumDirty != r.illumDirty;
    }
    for (const auto& r : a)
        if (!seen.count(r.entity)) ++d.onlyA;
    return d;
}

std::uint64_t digest(const std::vector<SideEffectRec>& s) {
    std::uint64_t acc = 0;  // order independent: a sum of per-record hashes
    for (const SideEffectRec& r : s) {
        std::uint64_t h = mix(0, r.entity);
        h = mix(h, r.visibleStamp);
        h = mix(h, r.lodRes);
        h = mix(h, r.lodStateMask);
        h = mix(h, r.busySum);
        h = mix(h, r.touched);
        h = mix(h, r.resources);
        h = mix(h, r.illumDirty);
        acc += h;
    }
    return acc;
}

const std::vector<SideEffectRec>& lastSnapshot() { return g_snapshot; }

bool install() {
    char v[8]{};
    if (GetEnvironmentVariableA("RVK_F10_COMPARE", v, sizeof(v)) == 0 || v[0] == '0') return false;
    const HMODULE m = GetModuleHandleW(L"Engine.dll");
    if (m == nullptr) return false;
    const auto base = reinterpret_cast<std::uintptr_t>(m);
    bool ok = g_request.install("f10 eCFrustumDatabase::Request", reinterpret_cast<DbFn>(base + kRvaRequest), &detourRequest);
    ok &= g_extract.install("f10 eCFrustumDatabase::ExtractFromFrustum", reinterpret_cast<DbFn>(base + kRvaExtract), &detourExtract);
    ok &= g_treeWalk.install("f10 sector tree query", reinterpret_cast<TreeFn>(base + kRvaTreeWalk), &detourTreeWalk);
    g_enabled = ok;
    if (GetEnvironmentVariableA("RVK_F10_EXP", v, sizeof(v)) != 0) g_f10Exp = static_cast<unsigned>(std::atoi(v));
    spdlog::info("native: f10 collect compare {} (RVK_F10_COMPARE)", ok ? "armed" : "NOT armed: a hook failed");
    return ok;
}

void onPresent() {
    if (!g_enabled) return;
    lazyInit();
    const auto now = std::chrono::steady_clock::now();
    if (std::chrono::duration<double>(now - g_lastReport).count() < 5.0) return;
    g_lastReport = now;
    for (auto& kv : g_dbs) {
        DbState& s = kv.second;
        Window& w = s.w;
        if (w.frames == 0) continue;
        const double n = static_cast<double>(w.frames);
        spdlog::info("native: f10 collect compare (5 s) db {}: {} views, {:.0f} elements per view (native {:.0f}, {} unknown to the scene table), missing {} extra {} (frames without any difference {}), requested engine {} native {} (differ {}), "
                     "fade {} common {} differ (max delta {:.5f}), LOD {} multi-LOD contexts {} differ (single-LOD {}, other context classes {}), nodes {} (ctx, sub-mesh) pairs {} equal {} differ (native-only contexts {}, engine-only {}), "
                     "portal mode {} extract filter callback {} scrub {} / {} stale, {} entries refreshed",
                     kv.first, w.frames, w.elemEngine / n, w.elemNative / n, w.unknown, w.missing, w.extra, w.frameDiffFree, w.requestedEngine, w.requestedNative, w.reqDiff, w.fadeCommon, w.fadeDiff, w.fadeMaxDelta, w.lodCommon, w.lodDiff,
                     w.lodSingle, w.ctxNonMesh, w.nodePairs, w.nodeEq, w.nodeDiff, w.nativeOnlyCtx, w.engineOnlyCtx, w.portalMode, w.filterCb, w.scrubStale, w.scrubChecked, w.refreshed);
        for (const auto& kv2 : w.byClass) {
            const ClassStats& c = kv2.second;
            spdlog::info("native: f10 class vt 0x{:x}: engine {} native-common {} missing {} extra {} fade differ {} requested differ {} LOD {} differ {}", kv2.first, c.engine, c.native, c.missing, c.extra, c.fadeDiff, c.reqDiff, c.lodCommon, c.lodDiff);
        }
        if (w.missing != 0 || w.extra != 0) {
            std::string causes;
            for (int k = 0; k < kCauses; ++k)
                if (w.cause[0][k] != 0 || w.cause[1][k] != 0) causes += fmt::format("{}: missing {} extra {}; ", kCauseNames[k], w.cause[0][k], w.cause[1][k]);
            spdlog::info("native: f10 causes (5 s): {}", causes);
        }
        spdlog::info("native: f10 cost (5 s) per view, us: engine ExtractFromFrustum {:.1f} (a separate call earlier in the frame) + Request {:.1f} (fade / UpdateLoD / RequestRenderOps) = {:.1f}; native cull {:.1f} + fade / request / LOD {:.1f} + hierarchy nodes {:.1f} = {:.1f}; "
                     "scene maintenance {:.1f} (refresh + scrub); compare overhead {:.1f} (not part of either)",
                     w.engineExtractUs / n, w.engineRequestUs / n, (w.engineExtractUs + w.engineRequestUs) / n, w.nativeCullUs / n, w.nativeFadeLodUs / n, w.nativeNodesUs / n, (w.nativeCullUs + w.nativeFadeLodUs + w.nativeNodesUs) / n, w.maintUs / n, w.compareUs / n);
        const double e = std::max<double>(1.0, static_cast<double>(w.snapElems));
        spdlog::info("native: f10 side effects (5 s, M3 snapshot of the engine's requested elements): {} elements over {} views, visible stamp = frame {} ({:.1f} %), resources {} touched this frame {} ({:.1f} %), busy counter non-zero {}, "
                     "illumination still dirty {}, LOD resource not resident {}, digest of the last view {:016x}",
                     w.snapElems, w.frames, w.snapStamped, 100.0 * static_cast<double>(w.snapStamped) / e, w.snapResources, w.snapTouched,
                     100.0 * static_cast<double>(w.snapTouched) / std::max<double>(1.0, static_cast<double>(w.snapResources)), w.snapBusy, w.snapIllumDirty, w.snapNotResident, w.snapDigest);
        w = Window{};
    }
}

}  // namespace rvk::native::f10
