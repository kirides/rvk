#include "native/owned_observe.h"

#include <windows.h>

#include <algorithm>
#include <chrono>
#include <cstring>
#include <format>
#include <memory>
#include <string>
#include <unordered_map>

#include <spdlog/spdlog.h>

#include "core/hooks.h"
#include "engine/memread.h"
#include "engine/scene_table.h"
#include "engine/symbols.h"

namespace rvk::native::owned_observe {

namespace {

using engine::readAt;

constexpr std::size_t kHooks = 5;
constexpr std::size_t kViews = kViewKindCount;

struct HookSite {
    const char* name;
    OpKind kind;
    std::uint32_t requestRva;
};

// the five contexts and the RequestRenderOps each must hold in vtable slot +0x40 (D-contexts.md 0.1, B-frustum-db.md 2.5)
const HookSite kSites[kHooks] = {
    {"eCRenderContextMesh::RequestRenderOps", OpKind::Mesh, 0x5c75e0},
    {"eCRenderContextSpeedTree::RequestRenderOps", OpKind::SpeedTree, 0x6ee4e0},
    {"eCRenderContextSpeedTreeBB::RequestRenderOps", OpKind::SpeedTreeBB, 0x6ee9b0},
    {"eCRenderContextVegetation::RequestRenderOps", OpKind::Vegetation, 0x70d4f0},
    {"eCRenderContextAnimation2::RequestRenderOps", OpKind::Animation, 0x4ddd00},
};

// contexts that no RequestRenderOps hook covers: their ops are the residue of a list (vtable RVA, class)
struct OtherContext {
    std::uint32_t vtableRva;
    OpKind kind;
};
constexpr OtherContext kOthers[] = {
    {0x9b7180, OpKind::Particle},     {0x9e2448, OpKind::Strip},         {0x9abd10, OpKind::LightStreaks}, {0x982810, OpKind::Billboard},
    {0x9e7a18, OpKind::Precipitation}, {0x9ddd90, OpKind::Skydome},      {0x9c9b00, OpKind::Debug},
};

using RequestFn = std::uint64_t (*)(void* ctx, void* db, float fade, std::uint32_t mask);
Hook<RequestFn> g_hook[kHooks];
bool g_installed = false;
bool g_on = false;
const std::uint8_t* g_base = nullptr;
std::array<const void*, kHooks> g_vtables{};  // the five contexts' vtables (kind classification of list entries)
DWORD g_thread = 0;
std::uint64_t g_otherThread = 0;

// the requests of one database since its lists were last cleared
struct DbState {
    const void* db = nullptr;
    std::vector<Request> reqs;
    std::array<std::uint32_t, kLists> lastAfter{};
};
std::vector<std::unique_ptr<DbState>> g_dbs;
DbState* g_lastDb = nullptr;

struct Stats {
    std::uint64_t views = 0, requests = 0, withOps = 0, ops = 0, unmatchedOps = 0, unmatchedRequests = 0, unresolved = 0, tapDiffers = 0;
    std::uint64_t fading = 0, lodResolved = 0, lodTotal = 0, lodNull = 0;
    std::array<std::uint64_t, kOpKindCount> byKind{}, residue{}, residueOther{};
};
std::array<Stats, kViews> g_win{}, g_run{};
std::uint64_t g_abandoned = 0, g_abandonedWin = 0, g_noRequests = 0, g_noRequestsWin = 0;
double g_viewMs = 0.0;
std::array<View, kViews> g_last;
int g_named = 0;

DbState* stateFor(const void* db) {
    if (g_lastDb != nullptr && g_lastDb->db == db) return g_lastDb;
    for (auto& s : g_dbs) {
        if (s->db == db) return g_lastDb = s.get();
    }
    g_dbs.push_back(std::make_unique<DbState>());
    g_dbs.back()->db = db;
    return g_lastDb = g_dbs.back().get();
}

std::uint32_t listCount(const void* db, std::size_t i) {
    std::int32_t n = 0;
    std::memcpy(&n, static_cast<const std::uint8_t*>(db) + kOffsets[i] + 0x10, 4);
    return n > 0 ? static_cast<std::uint32_t>(n) : 0u;
}

std::uint64_t run(std::size_t site, void* ctx, void* db, float fade, std::uint32_t mask) {
    if (!g_on || db == nullptr) return g_hook[site].original(ctx, db, fade, mask);
    if (GetCurrentThreadId() != g_thread) {
        if (g_thread == 0) g_thread = GetCurrentThreadId();
        else ++g_otherThread;
    }
    Request r;
    r.ctx = ctx;
    r.kind = kSites[site].kind;
    r.fade = fade;
    r.mask = static_cast<std::uint16_t>(mask);
    std::memcpy(&r.entity, static_cast<const std::uint8_t*>(ctx) + 0x10, 8);
    if (r.kind == OpKind::Mesh) std::memcpy(&r.lodResource, static_cast<const std::uint8_t*>(ctx) + 0x28, 8);
    for (std::size_t i = 0; i < kLists; ++i) r.before[i] = listCount(db, i);
    const std::uint64_t result = g_hook[site].original(ctx, db, fade, mask);
    DbState* st = stateFor(db);
    for (std::size_t i = 0; i < kLists; ++i) r.after[i] = listCount(db, i);
    // a list smaller than after our last request: the engine cleared the lists (a new Request round) without a view having closed
    // the previous requests
    for (std::size_t i = 0; i < kLists; ++i) {
        if (r.before[i] < st->lastAfter[i]) {
            g_abandoned += st->reqs.size();
            g_abandonedWin += st->reqs.size();
            st->reqs.clear();
            st->lastAfter = {};
            break;
        }
    }
    st->lastAfter = r.after;
    st->reqs.push_back(r);
    return result;
}

template <std::size_t I>
std::uint64_t detour(void* ctx, void* db, float fade, std::uint32_t mask) {
    return run(I, ctx, db, fade, mask);
}
constexpr RequestFn kDetours[kHooks] = {&detour<0>, &detour<1>, &detour<2>, &detour<3>, &detour<4>};

// the class of a context from its vtable (the five hooked ones + the documented others)
OpKind classOf(const void* ctx) {
    const void* vt = nullptr;
    if (!readAt(ctx, 0, vt) || vt == nullptr) return OpKind::Unknown;
    static std::unordered_map<const void*, OpKind> cache;
    if (const auto it = cache.find(vt); it != cache.end()) return it->second;
    OpKind k = OpKind::Unknown;
    for (std::size_t i = 0; i < kHooks; ++i) {
        if (vt == g_vtables[i]) k = kSites[i].kind;
    }
    if (k == OpKind::Unknown && g_base != nullptr) {
        for (const OtherContext& o : kOthers) {
            if (vt == g_base + o.vtableRva) k = o.kind;
        }
    }
    if (k == OpKind::Unknown) spdlog::info("native: owned observe: a context of vtable {} (RVA {:#x}) is none of the known classes", vt, static_cast<const std::uint8_t*>(vt) - g_base);
    cache.emplace(vt, k);
    return k;
}

bool hooked(OpKind k) {
    return k == OpKind::Mesh || k == OpKind::SpeedTree || k == OpKind::SpeedTreeBB || k == OpKind::Vegetation || k == OpKind::Animation;
}

std::int32_t meshLod(const Request& r) {
    if (r.lodResource == nullptr) return -1;
    const std::uint8_t* ps = nullptr;
    std::uint8_t n = 0;
    if (!readAt(r.ctx, 0x18, ps) || !readAt(ps, 0x6c, n)) return -1;
    if (n <= 1) return 0;  // UpdateLoD returns at once for one level: the context's single resource
    for (std::uint8_t b = 0; b < n; ++b) {
        const void* res = nullptr;
        if (readAt(ps, 0x40 + 8u * b, res) && res == r.lodResource) return b;
    }
    return -1;
}

std::int32_t actorLod(const Request& r) {
    const std::uint8_t* ps = nullptr;
    const std::uint8_t* actor = nullptr;
    std::int32_t lod = -1;
    if (readAt(r.ctx, 0x18, ps) && readAt(ps, 0x38, actor) && actor != nullptr && readAt(actor, 0x250, lod)) return lod;
    return -1;
}

const char* listName(std::size_t i) {
    static constexpr const char* names[kLists] = {"prepass", "alpha", "solid", "z", "sky", "late", "debug", "refracted"};
    return names[i];
}

std::string describe(const Request& r) {
    std::string lists;
    for (std::size_t i = 0; i < kLists; ++i) {
        if (r.after[i] != r.before[i]) lists += std::format(" {} {}..{}", listName(i), r.before[i], r.after[i]);
    }
    std::array<float, 3> pos{};
    readAt(r.entity, 0x60 + 0x30, pos);
    return std::format("{} ctx {} entity {} (slot {}, at {:.0f} {:.0f} {:.0f}) lod {} fade {:.2f} mask {:#x}:{}", name(r.kind), r.ctx, r.entity,
                       r.slot == UINT32_MAX ? -1 : static_cast<std::int64_t>(r.slot), pos[0], pos[1], pos[2], r.lod, r.fade, r.mask,
                       lists.empty() ? " no ops" : lists);
}

void nameCase(const char* what, ViewKind view, std::uint64_t frame, const std::string& text) {
    if (g_named >= 12) return;
    ++g_named;
    spdlog::info("native: owned observe: {} (frame {}, {} view): {}", what, frame, name(view), text);
}

}  // namespace

bool install() {
    const auto& s = engine::sym();
    g_base = reinterpret_cast<const std::uint8_t*>(GetModuleHandleW(L"Engine.dll"));
    const void* vtables[kHooks] = {s.ctxMeshVftable, s.ctxSpeedTreeVftable, s.ctxSpeedTreeBBVftable, s.ctxVegetationVftable, s.ctxAnimationVftable};
    int n = 0;
    for (std::size_t i = 0; i < kHooks; ++i) {
        g_vtables[i] = vtables[i];
        const void* fn = nullptr;
        if (g_base == nullptr || vtables[i] == nullptr || !readAt(vtables[i], 0x40, fn) || fn != g_base + kSites[i].requestRva) {
            spdlog::warn("native: owned observe: {}: vtable slot +0x40 is not at RVA {:#x} (found {}): not hooked", kSites[i].name, kSites[i].requestRva, fn);
            continue;
        }
        if (g_hook[i].install(kSites[i].name, reinterpret_cast<RequestFn>(const_cast<void*>(fn)), kDetours[i])) ++n;
    }
    g_installed = n > 0;
    spdlog::info("native: owned observe: {} of {} RequestRenderOps hooks installed (observe only: the originals always run)", n, kHooks);
    return g_installed;
}

bool installed() { return g_installed; }

void setEnabled(bool on) {
    if (on && !g_installed) {
        spdlog::warn("native: owned observe: the hooks are installed at start only when native.owned_observe is on then: restart the game");
        return;
    }
    if (on == g_on) return;
    g_on = on;
    for (auto& s : g_dbs) {
        s->reqs.clear();
        s->lastAfter = {};
    }
    spdlog::info("native: owned observe {}", on ? "on" : "off");
}

bool enabled() { return g_on; }

const View& lastView(ViewKind kind) { return g_last[static_cast<std::size_t>(kind)]; }

void viewBegin(void* db, ViewKind kind, std::uint64_t frame, const FrameDesc& desc) {
    if (!g_on || db == nullptr) return;
    const auto t0 = std::chrono::steady_clock::now();
    const std::size_t k = static_cast<std::size_t>(kind);
    Stats& w = g_win[k];
    DbState* st = nullptr;
    for (auto& s : g_dbs) {
        if (s->db == db) st = s.get();
    }
    if (st == nullptr || st->reqs.empty()) {
        ++g_noRequests;
        ++g_noRequestsWin;
        nameCase("a view with no recorded request", kind, frame, std::format("db {}", db));
        if (st != nullptr) st->lastAfter = {};
        return;
    }
    ++w.views;
    View& view = g_last[k];
    view.frame = frame;
    view.db = db;
    view.kind = kind;
    view.requests.swap(st->reqs);
    st->reqs.clear();
    st->lastAfter = {};

    // the live lists
    struct Live {
        const std::uint8_t* data = nullptr;
        std::uint32_t count = 0;
    };
    std::array<Live, kLists> live{};
    static std::array<std::vector<std::uint8_t>, kLists> covered;
    for (std::size_t i = 0; i < kLists; ++i) {
        std::int32_t n = 0;
        if (readAt(db, kOffsets[i] + 8, live[i].data) && readAt(db, kOffsets[i] + 0x10, n) && n > 0 && live[i].data != nullptr && n < (1 << 20)) {
            live[i].count = static_cast<std::uint32_t>(n);
        } else {
            live[i].data = nullptr;
        }
        covered[i].assign(live[i].count, 0);
    }
    // the lists the tap snapshots: a main view's seven (not the pre-pass list), a shadow view's z list
    const auto inDesc = [&](std::size_t i) { return kind == ViewKind::Main ? i != 0 : kOffsets[i] == 0x1518; };

    std::array<std::uint64_t, kOpKindCount> requested{};  // ops the requests appended in the tap's lists, per class
    for (Request& r : view.requests) {
        ++w.requests;
        ++w.byKind[static_cast<std::size_t>(r.kind)];
        if (r.fade < 1.0f) ++w.fading;
        const std::uint32_t ops = r.ops();
        if (r.kind == OpKind::Mesh) {
            r.lod = meshLod(r);
            if (r.lodResource == nullptr) {
                ++w.lodNull;  // no loaded LOD resource (UpdateLoD left ctx +0x28 null): the request appends nothing
            } else {
                ++w.lodTotal;
                if (r.lod >= 0) ++w.lodResolved;
            }
        } else if (r.kind == OpKind::Animation) {
            r.lod = actorLod(r);
        }
        if (ops == 0) continue;
        ++w.withOps;
        w.ops += ops;
        r.slot = engine::scenetable::find(r.entity);
        if (r.slot == UINT32_MAX) {
            ++w.unresolved;
            nameCase("unresolved entity", kind, frame, describe(r));
        }
        bool ok = true;
        for (std::size_t i = 0; i < kLists; ++i) {
            if (r.after[i] <= r.before[i]) continue;
            if (inDesc(i)) requested[static_cast<std::size_t>(r.kind)] += r.after[i] - r.before[i];
            if (r.after[i] > live[i].count) {
                ok = false;
                continue;
            }
            for (std::uint32_t p = r.before[i]; p < r.after[i]; ++p) {
                const void* c = nullptr;
                if (!readAt(live[i].data + std::size_t{p} * sizeof(RawOp), 0, c) || c != r.ctx) ok = false;
                covered[i][p] = 1;
            }
        }
        if (!ok) {
            ++w.unmatchedRequests;
            nameCase("unmatched request (the lists hold other ops at its range)", kind, frame, describe(r));
        }
    }
    // list entries no request covers: ops of a hooked class = unmatched ops; the others = the residue classes
    for (std::size_t i = 0; i < kLists; ++i) {
        for (std::uint32_t p = 0; p < live[i].count; ++p) {
            if (covered[i][p] != 0) continue;
            const void* c = nullptr;
            readAt(live[i].data + std::size_t{p} * sizeof(RawOp), 0, c);
            const OpKind ck = classOf(c);
            if (hooked(ck)) {
                ++w.unmatchedOps;
                const void* entity = nullptr;
                readAt(c, 0x10, entity);
                nameCase("unmatched op", kind, frame, std::format("{} ctx {} entity {} in the {} list at {}", name(ck), c, entity, listName(i), p));
            } else if (inDesc(i)) {
                ++w.residue[static_cast<std::size_t>(ck)];
            } else {
                ++w.residueOther[static_cast<std::size_t>(ck)];  // a list the tap does not snapshot (the pre-pass lights' list; a shadow view's other lists)
            }
        }
    }
    // against the tap's snapshot: the ops of the five classes per class
    std::array<std::uint64_t, kOpKindCount> tapped{};
    for (const Op& op : desc.ops) ++tapped[static_cast<std::size_t>(op.kind)];
    for (std::size_t c = 0; c < kOpKindCount; ++c) {
        if (!hooked(static_cast<OpKind>(c)) || tapped[c] == requested[c]) continue;
        ++w.tapDiffers;
        nameCase("the frame tap's op count differs from the requests'", kind, frame,
                 std::format("{}: tap {} ops, requests appended {}", name(static_cast<OpKind>(c)), tapped[c], requested[c]));
    }
    g_viewMs += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
}

void report() {
    if (!g_on) return;
    bool any = false;
    for (std::size_t k = 0; k < kViews; ++k) {
        const Stats& w = g_win[k];
        if (w.views == 0) continue;
        any = true;
        std::string residue, other;
        for (std::size_t c = 0; c < kOpKindCount; ++c) {
            if (w.residue[c] != 0) residue += std::format(" {} {}", name(static_cast<OpKind>(c)), w.residue[c]);
            if (w.residueOther[c] != 0) other += std::format(" {} {}", name(static_cast<OpKind>(c)), w.residueOther[c]);
        }
        spdlog::info("native: owned observe (5 s) [{}]: {} views, requests {} (mesh {}, skinned {}, SpeedTree {}, tree billboard {}, vegetation {}; {} with ops, {} fading), "
                     "ops appended {}, unmatched ops {}, unmatched requests {}, unresolved entities {}, tap differs {}; mesh LOD index resolved {} of {} (+ {} without a loaded LOD); "
                     "ops of other classes (expected residue):{}; in lists the tap does not snapshot:{}",
                     name(static_cast<ViewKind>(k)), w.views, w.requests, w.byKind[static_cast<std::size_t>(OpKind::Mesh)],
                     w.byKind[static_cast<std::size_t>(OpKind::Animation)], w.byKind[static_cast<std::size_t>(OpKind::SpeedTree)],
                     w.byKind[static_cast<std::size_t>(OpKind::SpeedTreeBB)], w.byKind[static_cast<std::size_t>(OpKind::Vegetation)], w.withOps, w.fading,
                     w.ops, w.unmatchedOps, w.unmatchedRequests, w.unresolved, w.tapDiffers, w.lodResolved, w.lodTotal, w.lodNull, residue.empty() ? " none" : residue, other.empty() ? " none" : other);
        Stats& t = g_run[k];
        t.views += w.views, t.requests += w.requests, t.withOps += w.withOps, t.ops += w.ops, t.unmatchedOps += w.unmatchedOps;
        t.unmatchedRequests += w.unmatchedRequests, t.unresolved += w.unresolved, t.tapDiffers += w.tapDiffers;
        g_win[k] = {};
    }
    if (!any) spdlog::info("native: owned observe (5 s): no view closed");
    spdlog::info("native: owned observe (5 s): views without requests {} (run {}), requests dropped when the lists were cleared before a view {} (run {}), "
                 "requests from another thread {}, viewBegin work {:.2f} ms",
                 g_noRequestsWin, g_noRequests, g_abandonedWin, g_abandoned, g_otherThread, g_viewMs);
    g_noRequestsWin = 0;
    g_abandonedWin = 0;
    g_viewMs = 0.0;
}

}  // namespace rvk::native::owned_observe
