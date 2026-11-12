#include "native/f10_extract.h"

#include <windows.h>

#include <algorithm>
#include <chrono>
#include <cstring>
#include <format>
#include <memory>
#include <string>
#include <vector>

#include <spdlog/spdlog.h>

#include "core/hooks.h"
#include "core/settings.h"
#include "native/f10_math.h"
#include "native/f10_occluder.h"
#include "native/f10_portal.h"
#include "native/f10_request.h"

// F10-2 (frame-ownership.md 14.33). Engine.dll RVAs (decompiled / disassembled): eCFrustumDatabase::ExtractFromFrustum 0x66fa60, ExtractFromEntity 0x66d750,
// OnQueryNodes 0x66d960, FUN_18066da10 (a subtree inside the frustum), the sector tree walker FUN_18015c4a0, eCOccluder_PS::AddOccluder 0x675270, the occluder
// desc comparator 0x66e000, the engine's qsort 0x4a4aa0, the element array's SetCount FUN_18009bcd0, the beam plane array's SetCount FUN_18009bdd0,
// eCPortal::GetCell 0x6718e0, eCBeamTree::AddPoly 0x67d120 (the indexed form: ported in f10_portal.h's addPoly), AddPlane 0x67d040.
namespace rvk::native::f10x {

using namespace rvk::native::f10;

namespace {

constexpr std::uintptr_t kRvaExtract = 0x66fa60, kRvaAddOccluder = 0x675270, kRvaOccCompare = 0x66e000, kRvaQsort = 0x4a4aa0;
constexpr std::uintptr_t kRvaItemSetCount = 0x9bcd0, kRvaPlaneSetCount = 0x9bdd0, kRvaGetCell = 0x6718e0;
constexpr std::uintptr_t kRvaTreeModule = 0x957e0, kRvaCameraModule = 0x956c0, kRvaActiveCamera = 0xc62580, kRvaCounter = 0xc628c0;
constexpr std::uintptr_t kRvaPortalAdmin = 0xc89a68, kRvaRoomVft = 0x9ca870, kRvaNodeSphere = 0x6a6160, kRvaNodeBox = 0x6a63a0;
constexpr std::uintptr_t kRvaEpsBehind = 0x9f1854, kRvaEpsBox = 0x9f16f8, kRvaOne = 0x9f1698, kRvaMinArea = 0x9f1620;

using U8 = std::uint8_t;
template <class T>
inline T rd(const void* base, std::size_t off) {
    T v;
    std::memcpy(&v, static_cast<const U8*>(base) + off, sizeof(T));
    return v;
}
template <class T>
inline void wr(void* base, std::size_t off, T v) {
    std::memcpy(static_cast<U8*>(base) + off, &v, sizeof(T));
}
inline void* rdp(const void* base, std::size_t off) { return rd<void*>(base, off); }

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

using DbFn = void (*)(void*);
using AddOccFn = bool (*)(void* occ, void* filter, void* descs, void* idx);
using SetCountFn = void (*)(void* arr, std::int32_t count, std::int32_t grow);
using QsortFn = void (*)(void* base, std::size_t n, std::size_t w, int (*cmp)(const void*, const void*));
using CellFn = const void* (*)(const void*);
using ModuleFn = void* (*)();
using GetFn = const float* (*)(const void*);

Hook<DbFn> g_extract;
Hook<AddOccFn> g_addOcc;
std::uintptr_t g_base = 0;
SetCountFn g_itemSetCount = nullptr, g_planeSetCount = nullptr;
QsortFn g_qsort = nullptr;
CellFn g_getCell = nullptr;
ModuleFn g_treeModule = nullptr, g_cameraModule = nullptr;
int (*g_occCompare)(const void*, const void*) = nullptr;
PortalConsts g_k;
float g_epsilon = 1e-6f, g_pi = 3.14159265f, g_one = 1.0f, g_minArea = 0.4f;
bool g_installed = false;
int g_tripped = 0, g_lastSetting = 0;

int settingMode() {
    const auto& rs = settings::renderer();
    return rs.nativeOwnExtract != nullptr && settings::fullNow() ? static_cast<int>(rs.nativeOwnExtract->asInt()) : 0;  // (S-9: default on, Full only)
}

// ---- the AddOccluder recorder (compare passes: the engine's descs / indices before its sort) ----
bool g_recording = false;
std::vector<OccDesc> g_recDescs;
std::vector<std::uint32_t> g_recIdx;

bool detourAddOccluder(void* occ, void* filter, void* descs, void* idx) {
    const bool r = g_addOcc.original(occ, filter, descs, idx);
    if (g_recording && descs != nullptr && idx != nullptr) {
        const auto nd = std::max(rd<std::int32_t>(descs, 0x10), 0);
        const auto ni = std::max(rd<std::int32_t>(idx, 0x10), 0);
        g_recDescs.resize(static_cast<std::size_t>(nd));
        if (nd > 0) std::memcpy(g_recDescs.data(), rdp(descs, 8), sizeof(OccDesc) * static_cast<std::size_t>(nd));
        g_recIdx.resize(static_cast<std::size_t>(ni));
        if (ni > 0) std::memcpy(g_recIdx.data(), rdp(idx, 8), 4 * static_cast<std::size_t>(ni));
    }
    return r;
}

// ---- one native extraction ----
struct Run {
    void* db = nullptr;
    U8* d = nullptr;
    std::int32_t count0 = 0;  // the element list's count at the call (Extract resets it to 0)
    Filter f;
    float factor = 1.0f;
    bool noRange = false;
    std::uint32_t counter = 0;
    const void* admin = nullptr;
    NBeam* fin = nullptr;
    const NBeam* entry = nullptr;  // compare with the engine first: the frustum beam captured at the call (the engine changed db +0x118 since)
    bool beamChanged = false;
    std::vector<void*> items;
    std::vector<std::pair<U8*, std::uint32_t>> undo;  // the extract stamps written (entity, old value)
    std::vector<void*> occluders;                      // the occluders AddOccluder ran for (their +0x128 / +0x140 are zeroed on commit)
    std::vector<OccDesc> descs;
    std::vector<std::uint32_t> idx;
    std::vector<std::int64_t> stack;
    const char* abort = nullptr;
    // census
    std::uint32_t rooms = 0, roomsEntered = 0, occTested = 0, occAdded = 0, polysAdded = 0, treeNodes = 0, leaves = 0;
    bool cameraRoom = false;
    std::uint64_t inputHash = 0;  // the view's inputs at the call: camera position, view plane, the frustum beam's planes (the persistence census)
    std::uint64_t tPortal = 0, tOcc = 0, tRooms = 0, tTree = 0;
};

std::unique_ptr<NBeam> g_fin, g_frustum, g_tmp, g_entryBeam;

// eCFrustumElementItem::CullNode 0x6710e0 on live data (the sphere getter first, then the box, as the engine calls them)
int cullNode(const Run& r, const void* e) {
    const std::uint32_t fl = rd<std::uint32_t>(e, 0x28);
    if ((fl & 0x8000u) == 0) return kCulled;
    const float range = r.noRange ? 0.0f : rd<float>(e, 0xcc) * r.factor;
    const auto vt = static_cast<void* const*>(rdp(e, 0));
    // eCGeometryEntity::GetWorldNodeSphere 0x6a6160 / GetWorldNodeBoundary 0x6a63a0 are `lea rax, [rcx + 0xb8 / 0xa0]; ret`: read in place when the slots hold them
    const auto sphFn = reinterpret_cast<std::uintptr_t>(vt[0x268 / 8]);
    const float* sph = sphFn == g_base + kRvaNodeSphere ? reinterpret_cast<const float*>(static_cast<const U8*>(e) + 0xb8) : reinterpret_cast<GetFn>(vt[0x268 / 8])(e);
    // CullCamera(box, sphere, range) 0x67e8e0 with the box fetched only when the sphere tests pass (the getters are pure)
    if (!cullCameraSphere(r.f, sph[0], sph + 1)) return kCulled;
    int dist = kInside;
    if (range > 0.0f) {
        dist = cullCameraDistance(r.f, sph[0], sph + 1, range);
        if (dist == kCulled) return kCulled;
    }
    const auto boxFn = reinterpret_cast<std::uintptr_t>(vt[0x260 / 8]);
    const float* box = boxFn == g_base + kRvaNodeBox ? reinterpret_cast<const float*>(static_cast<const U8*>(e) + 0xa0) : reinterpret_cast<GetFn>(vt[0x260 / 8])(e);
    int b = beamCullBox(r.f.beam, box);
    if (b == kCulled) return kCulled;
    if (dist == kPartial && b == kInside) b = kPartial;
    return b;
}

// eCFrustumDatabase::ExtractFromEntity 0x66d750 (the filter callback is null: checked before the run)
void extract(Run& r, void* e, int status) {
    if (e == nullptr) return;
    U8* E = static_cast<U8*>(e);
    const std::uint32_t old = rd<std::uint32_t>(E, 0xe0);
    if (old == r.counter) return;
    if (rdp(E, 0xf0) == nullptr) return;
    const std::uint32_t fl = rd<std::uint32_t>(E, 0x28);
    if ((fl & 0x8026u) != 0x8026u || (fl & 8u) != 0) return;
    if (status != kInside && cullNode(r, E) == kCulled) return;
    r.undo.emplace_back(E, old);
    wr<std::uint32_t>(E, 0xe0, r.counter);
    r.items.push_back(E + 0xd8);
}

// the room graph of f10_portal.h's walkRooms over the engine's rooms / portals, extracting into the run
struct Graph {
    Run* r;
    static const U8* portalAt(const void* room, int i) { return static_cast<const U8*>(rdp(room, 0x20)) + 0x78 * static_cast<std::size_t>(i); }
    int portalCount(const void* room) const { return rd<std::int32_t>(room, 0x28); }
    PortalRef portal(const void* room, int i) const {
        const U8* p = portalAt(room, i);
        return {reinterpret_cast<const float*>(p), reinterpret_cast<const float*>(p + 0x30), reinterpret_cast<const float*>(p + 0x3c), reinterpret_cast<const float*>(p + 0x48)};
    }
    const void* portalRoom(const void* room, int i) const {
        const U8* p = portalAt(room, i);
        const void* cell = rdp(p, 0x58);
        if (cell == nullptr) cell = g_getCell(p);
        if (cell == nullptr) return nullptr;
        return rd<std::uintptr_t>(cell, 0) == g_base + kRvaRoomVft ? cell : nullptr;
    }
    void extractRoomEntity(const void* room) {
        if ((rd<std::uint8_t>(room, 0x18) & 0x10) == 0) extract(*r, rdp(room, 0x10), kInside);
    }
    void extractRoomList(const void* room, const Beam& b) {
        const int count = rd<std::int32_t>(room, 0x78);
        const U8* arr = static_cast<const U8*>(rdp(room, 0x70));
        for (int i = 0; i < count; ++i) {
            void* e = rdp(arr, 8 * static_cast<std::size_t>(i));
            const auto vt = static_cast<void* const*>(rdp(e, 0));
            const float* box = reinterpret_cast<std::uintptr_t>(vt[0x260 / 8]) == g_base + kRvaNodeBox ? reinterpret_cast<const float*>(static_cast<const U8*>(e) + 0xa0)
                                                                                                      : reinterpret_cast<GetFn>(vt[0x260 / 8])(e);
            if (beamCullBox(b, box) != kCulled) extract(*r, e, kInside);
        }
    }
};

// the sector tree query: FUN_18015c4a0 (an explicit stack; the children of a popped node in sibling order, those the callback descends pushed) with
// OnQueryNodes 0x66d960 and FUN_18066da10 (a subtree inside the frustum: every gated leaf extracted with status "inside")
struct Tree {
    Run* r;
    const U8* nodes;
    std::uint32_t mask, value;
    const U8* node(std::int64_t i) const { return nodes + static_cast<std::int64_t>(static_cast<std::int32_t>(i)) * 0x58; }
    void inside(const U8* n) {
        if ((n[0x50] & 4) != 0 && (rd<std::uint32_t>(n, 0x18) & mask) == value) extract(*r, rdp(n, 0x10), kInside);
        for (std::int64_t c = rd<std::int64_t>(n, 0x40); c != -1;) {
            const U8* cn = node(c);
            inside(cn);
            c = rd<std::int64_t>(cn, 0x30);
        }
    }
    bool query(std::int64_t i) {
        const U8* n = node(i);
        ++r->treeNodes;
        const float* sph = reinterpret_cast<const float*>(n);
        if ((n[0x50] & 4) != 0) {
            if ((rd<std::uint32_t>(n, 0x18) & mask) == value) {
                ++r->leaves;
                const int st = cullCameraLeaf(r->f, sph[0], sph + 1);
                if (st != kCulled) extract(*r, rdp(n, 0x10), st);
            }
            return false;
        }
        const int st = cullCameraLeaf(r->f, sph[0], sph + 1);
        if (st == kInside) inside(n);
        return st == kPartial;
    }
    void walk(const U8* tree) {
        auto& stack = r->stack;
        stack.clear();
        stack.push_back(rd<std::int64_t>(tree, 0x20));
        while (!stack.empty()) {
            const std::int64_t top = stack.back();
            stack.pop_back();
            for (std::int64_t c = rd<std::int64_t>(node(top), 0x40); c != -1;) {
                const U8* cn = node(c);
                if (query(c) && rd<std::int64_t>(cn, 0x40) != -1) stack.push_back(c);
                c = rd<std::int64_t>(cn, 0x30);
            }
        }
    }
};

void ensureBeams() {
    if (!g_fin) g_fin = std::make_unique<NBeam>();
    if (!g_frustum) g_frustum = std::make_unique<NBeam>();
    if (!g_tmp) g_tmp = std::make_unique<NBeam>();
}

// the port of ExtractFromFrustum's portal-mode body; on return r.abort == nullptr = the result is the engine's
void runBody(Run& r) {
    U8* d = r.d;
    const U8* tree = d + 0x118;
    NBeam& fin = *r.fin;
    const float* cam = r.f.camera;
    const float* viewPlane = reinterpret_cast<const float*>(d + 0x94);
    const void* admin = r.admin;
    Graph g{&r};
    detail::g_polyOverflow = false;

    // 1. the camera's room
    std::uint64_t t0 = qpc();
    const void* room0 = nullptr;
    {
        // eCPortalAdmin::FindPortalRoom (the port in f10_collect.cpp's terms; the same function)
        const int cnt = rd<std::int32_t>(admin, 0x28);
        const U8* rooms = static_cast<const U8*>(rdp(admin, 0x20));
        for (int i = 0; i < cnt && room0 == nullptr; ++i) {
            const void* room = rdp(rooms, 8 * static_cast<std::size_t>(i));
            const U8* node = static_cast<const U8*>(rdp(room, 0x58));
            if (node == nullptr) continue;
            const float* p = cam;
            const float rr = rd<float>(node, 0x24), cx = rd<float>(node, 0x28), cy = rd<float>(node, 0x2c), cz = rd<float>(node, 0x30);
            if (!((p[1] - cy) * (p[1] - cy) + (p[0] - cx) * (p[0] - cx) + (p[2] - cz) * (p[2] - cz) < rr * rr)) continue;
            bool next = false;
            bool found = false;
            while (rd<std::uint8_t>(node, 0x20) != 1) {
                const float cp = p[1] * rd<float>(node, 0x14) + p[0] * rd<float>(node, 0x10) + p[2] * rd<float>(node, 0x18) + rd<float>(node, 0x1c);
                if (0.0f <= cp) {
                    node = static_cast<const U8*>(rdp(node, 8));
                    if (node == nullptr) {
                        found = true;
                        break;
                    }
                } else {
                    node = static_cast<const U8*>(rdp(node, 0));
                }
                if (node == nullptr) {
                    next = true;
                    break;
                }
            }
            if (found) {
                room0 = room;
                break;
            }
            if (next) continue;
            const int pc = rd<std::int32_t>(node, 0x40);
            if (pc < 1) {
                room0 = room;
                break;
            }
            const U8* planes = static_cast<const U8*>(rdp(node, 0x38));
            bool in = true;
            for (int k = 0; k < pc && in; ++k) {
                const U8* pl = planes + 0x80 * static_cast<std::size_t>(k);
                if (p[1] * rd<float>(pl, 4) + p[0] * rd<float>(pl, 0) + p[2] * rd<float>(pl, 8) + rd<float>(pl, 0xc) < 0.0f) in = false;
            }
            if (in) room0 = room;
        }
    }
    fin.reset();
    if (room0 != nullptr) {
        r.cameraRoom = true;
        r.beamChanged = true;
        NBeam& frustum = *g_frustum;
        if (r.entry != nullptr) frustum.copyFrom(*r.entry);
        else frustum.copyFromEngine(static_cast<const BeamNode*>(rdp(tree, 0)), static_cast<const BeamPlane*>(rdp(tree, 0x12d8)), static_cast<std::uint32_t>(std::max(rd<std::int32_t>(tree, 0x12e0), 0)));
        if (frustum.overflow) {
            r.abort = "frustum beam larger than the native tree";
            return;
        }
        g.extractRoomEntity(room0);
        walkRooms(g, g_k, cam, viewPlane, room0, frustum, true, static_cast<const void*>(nullptr), fin);
        if (fin.root != nullptr) fin.addPlaneAtRoot(reinterpret_cast<const float*>(d + 0xb8));
    } else {
        if (r.entry != nullptr) fin.copyFrom(*r.entry);
        else fin.copyFromEngine(static_cast<const BeamNode*>(rdp(tree, 0)), static_cast<const BeamPlane*>(rdp(tree, 0x12d8)), static_cast<std::uint32_t>(std::max(rd<std::int32_t>(tree, 0x12e0), 0)));
    }
    r.f.beam = fin.view();
    std::uint64_t t1 = qpc();
    r.tPortal += t1 - t0;

    // 2. the occluders
    {
        const int n = rd<std::int32_t>(admin, 0x40);
        const U8* occs = static_cast<const U8*>(rdp(admin, 0x38));
        void* camMod = n > 0 ? g_cameraModule() : nullptr;
        for (int k = 0; k < n; ++k) {
            void* o = rdp(occs, 8 * static_cast<std::size_t>(k));
            void* ent = (rd<std::uint8_t>(o, 0x18) & 0x10) == 0 ? rdp(o, 0x10) : nullptr;
            if (ent == nullptr) {
                r.abort = "an occluder without an entity";
                return;
            }
            ++r.occTested;
            if (cullNode(r, ent) == kCulled) continue;
            const auto vt = static_cast<void* const*>(rdp(o, 0));
            if (reinterpret_cast<std::uintptr_t>(vt[0x2c8 / 8]) != g_base + kRvaAddOccluder) {
                r.abort = "an occluder class with its own AddOccluder";
                return;
            }
            if (camMod == nullptr) {
                r.abort = "no camera module";
                return;
            }
            const U8* eyeBase = rdp(camMod, 0x5f8) != nullptr ? static_cast<const U8*>(rdp(reinterpret_cast<const void*>(g_base + kRvaActiveCamera), 0)) : static_cast<const U8*>(o);
            if (eyeBase == nullptr) {
                r.abort = "no active camera";
                return;
            }
            const float* eye = reinterpret_cast<const float*>(eyeBase + 0x150);
            OccMesh m;
            m.verts = static_cast<const float*>(rdp(o, 0x60));
            m.faces = static_cast<const U8*>(rdp(o, 0x78));
            m.faceCount = rd<std::int32_t>(o, 0x80);
            m.edges = static_cast<const U8*>(rdp(o, 0x90));
            m.edgeCount = rd<std::int32_t>(o, 0x98);
            m.parts = static_cast<const U8*>(rdp(o, 0xa8));
            m.partCount = rd<std::int32_t>(o, 0xb0);
            OccFilter of;
            const U8* filter = d + 0x28;
            const U8* camObj = static_cast<const U8*>(rdp(filter, 0x10));
            std::memcpy(of.camera, camObj + 0x90, 12);
            std::memcpy(of.viewDir, filter + 0x38, 12);
            of.sphereRadius = rd<float>(filter, 0x44);
            std::memcpy(of.sphereCenter, filter + 0x48, 12);
            std::memcpy(of.viewPlane, filter + 0x6c, 16);
            of.beam = fin.view();
            of.epsilon = g_epsilon;
            of.pi = g_pi;
            of.minArea = g_minArea;
            of.one = g_one;
            if (!addOccluder(m, of, eye, r.descs, r.idx)) {
                r.abort = "an occluder's silhouette walk did not end";
                return;
            }
            r.occluders.push_back(o);
            ++r.occAdded;
        }
        if (!r.descs.empty()) {
            // the engine's qsort with the engine's comparator (ties keep the engine's order)
            std::vector<OccDesc> sorted = r.descs;
            g_qsort(sorted.data(), sorted.size(), sizeof(OccDesc), g_occCompare);
            std::vector<std::uint32_t> order;
            occluderAddOrder(static_cast<std::uint32_t>(sorted.size()), order);
            for (const std::uint32_t i : order) {
                const OccDesc& od = sorted[i];
                if (od.count > 40) {
                    r.abort = "an occluder polygon of more than 40 vertices";
                    return;
                }
                Poly poly;
                poly.n = static_cast<std::int32_t>(od.count);
                for (std::uint32_t v = 0; v < od.count; ++v) {
                    std::memcpy(poly.v[v], od.verts + 3 * static_cast<std::size_t>(r.idx[od.first + v]), 12);
                    poly.id[v] = static_cast<std::int32_t>(v);
                }
                AddInfo info;
                std::memcpy(info.plane, od.plane, 16);
                std::memcpy(info.origin, cam, 12);
                info.chainNext = false;
                info.dest = nullptr;
                addPoly(fin, &fin.root, poly, info, g_k);
                ++r.polysAdded;
                r.beamChanged = true;
            }
        }
    }
    r.f.beam = fin.view();
    std::uint64_t t2 = qpc();
    r.tOcc += t2 - t1;

    // 3. every room seen from outside through a portal that leads outdoors
    {
        NBeam& tmp = *g_tmp;
        const int roomCount = rd<std::int32_t>(admin, 0x28);
        const U8* rooms = static_cast<const U8*>(rdp(admin, 0x20));
        for (int ri = 0; ri < roomCount; ++ri) {
            const void* room = rdp(rooms, 8 * static_cast<std::size_t>(ri));
            const void* ent = (rd<std::uint8_t>(room, 0x18) & 0x10) == 0 ? rdp(room, 0x10) : nullptr;
            if (ent == nullptr) {
                r.abort = "a portal room without an entity";
                return;
            }
            ++r.rooms;
            if (cullNode(r, ent) == kCulled) continue;
            const int pc = g.portalCount(room);
            for (int pi = 0; pi < pc; ++pi) {
                if (g.portalRoom(room, pi) != nullptr) continue;
                const PortalRef p = g.portal(room, pi);
                if (!cameraOutsidePortalBox(p, cam, g_k.epsBox)) {
                    tmp.copyFrom(fin);
                    ++r.roomsEntered;
                    walkRooms(g, g_k, cam, viewPlane, room, tmp, false, static_cast<const void*>(nullptr), fin);
                } else if (0.0f <= portalFacing(p, cam)) {
                    const float rev[12] = {p.v[9], p.v[10], p.v[11], p.v[6], p.v[7], p.v[8], p.v[3], p.v[4], p.v[5], p.v[0], p.v[1], p.v[2]};
                    tmp.reset();
                    addPortalPoly(fin, tmp, rev, p.normal, cam, viewPlane, g_k);
                    ++r.roomsEntered;
                    walkRooms(g, g_k, cam, viewPlane, room, tmp, false, static_cast<const void*>(nullptr), fin);
                }
                if (tmp.overflow) {
                    r.abort = "a room beam larger than the native tree";
                    return;
                }
            }
        }
    }
    std::uint64_t t3 = qpc();
    r.tRooms += t3 - t2;
    if (fin.overflow || fin.nodeCount > 200) {
        r.abort = "the final beam needs more than the engine's 200 nodes";
        return;
    }
    if (detail::g_polyOverflow) {
        r.abort = "a clipped polygon of more than 40 vertices";
        return;
    }

    // 4. the sector tree query
    if (fin.root != nullptr) {
        const U8* treeObj = static_cast<const U8*>(g_treeModule());
        if (treeObj == nullptr) {
            r.abort = "no sector tree";
            return;
        }
        treeObj += 0x390;
        Tree t{&r, static_cast<const U8*>(rdp(treeObj, 0x30)), rd<std::uint32_t>(d, 0x15b0) | (d[0x15a3] != 0 ? 6u : 0u), rd<std::uint32_t>(d, 0x15ac)};
        t.walk(treeObj);
    }
    r.tTree += qpc() - t3;
}

bool guardedBody(Run* r) {
    __try {
        runBody(*r);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

void undoStamps(Run& r) {
    for (auto it = r.undo.rbegin(); it != r.undo.rend(); ++it) wr<std::uint32_t>(it->first, 0xe0, it->second);
    r.undo.clear();
}

// the run's result into the engine: the element list (appended to db +0x18 / +0x20), the final beam (db +0x118), the occluders' counters
void commit(Run& r) {
    U8* d = r.d;
    const std::int32_t count0 = rd<std::int32_t>(d, 0x20);
    if (!r.items.empty()) {
        g_itemSetCount(d + 0x10, count0 + static_cast<std::int32_t>(r.items.size()), 0);
        std::memcpy(static_cast<U8*>(rdp(d, 0x18)) + 8 * static_cast<std::size_t>(count0), r.items.data(), 8 * r.items.size());
    }
    if (r.beamChanged) {
        U8* t = d + 0x118;
        NBeam& fin = *r.fin;
        g_planeSetCount(t + 0x12d0, static_cast<std::int32_t>(fin.planeCount), 0);
        if (fin.planeCount != 0) std::memcpy(rdp(t, 0x12d8), fin.planes, sizeof(BeamPlane) * fin.planeCount);
        auto engineNode = [&](const NNode* n) -> void* { return n == nullptr ? nullptr : t + 8 + 0x18 * static_cast<std::size_t>(n - fin.nodes); };
        for (std::uint32_t i = 0; i < fin.nodeCount; ++i) {
            U8* en = t + 8 + 0x18 * static_cast<std::size_t>(i);
            wr<std::int32_t>(en, 0, fin.nodes[i].plane);
            wr<void*>(en, 8, engineNode(fin.nodes[i].inside));
            wr<void*>(en, 0x10, engineNode(fin.nodes[i].next));
        }
        wr<std::uint32_t>(t, 0x12c8, fin.nodeCount);
        wr<void*>(t, 0, engineNode(fin.root));
    }
    for (void* o : r.occluders) {
        wr<std::uint32_t>(o, 0x128, 0);
        wr<std::uint32_t>(o, 0x140, 0);
    }
}

// ---- statistics ----
struct Window {
    std::uint64_t calls = 0, mainCalls = 0, live = 0, compare = 0, fallbacks = 0, faults = 0;
    std::uint64_t compared = 0, differing = 0, listDiff = 0, beamDiff = 0, nodeCountDiff = 0, occDiff = 0, occCompared = 0, descsCompared = 0;
    std::uint64_t sameInput = 0, sameList = 0, sameListMoved = 0;  // persistence census: views whose inputs / element list equal the previous main view's
    std::uint64_t elements = 0, cameraRoom = 0, rooms = 0, roomsEntered = 0, occTested = 0, occAdded = 0, polys = 0, treeNodes = 0, leaves = 0;
    double liveUs = 0, dryUs = 0, engineUs = 0, commitUs = 0;
    std::uint64_t engineFirst = 0;
    double engineFirstUs = 0, dryAfterUs = 0;  // compare views with the engine first (alternating): its cost cold, the port's warm
    double portalUs = 0, occUs = 0, roomsUs = 0, treeUs = 0;
    std::string lastFallback;
} g_w;
std::uint64_t g_runLive = 0, g_runCompared = 0, g_runDiffering = 0;
int g_diffLog = 8;
std::chrono::steady_clock::time_point g_lastReport = std::chrono::steady_clock::now();

inline std::uint64_t fnv(std::uint64_t h, const void* p, std::size_t n) {
    const auto* b = static_cast<const U8*>(p);
    for (std::size_t i = 0; i < n; ++i) h = (h ^ b[i]) * 0x100000001b3ull;
    return h;
}
std::uint64_t g_lastInput = 0, g_lastList = 0;

void census(const Run& r) {
    const std::uint64_t listHash = fnv(0xcbf29ce484222325ull, r.items.data(), 8 * r.items.size());
    if (r.inputHash == g_lastInput) ++g_w.sameInput;
    if (listHash == g_lastList) {
        ++g_w.sameList;
        if (r.inputHash != g_lastInput) ++g_w.sameListMoved;
    }
    g_lastInput = r.inputHash;
    g_lastList = listHash;
    g_w.elements += r.items.size();
    g_w.cameraRoom += r.cameraRoom ? 1 : 0;
    g_w.rooms += r.rooms;
    g_w.roomsEntered += r.roomsEntered;
    g_w.occTested += r.occTested;
    g_w.occAdded += r.occAdded;
    g_w.polys += r.polysAdded;
    g_w.treeNodes += r.treeNodes;
    g_w.leaves += r.leaves;
    g_w.portalUs += qpcUs(r.tPortal);
    g_w.occUs += qpcUs(r.tOcc);
    g_w.roomsUs += qpcUs(r.tRooms);
    g_w.treeUs += qpcUs(r.tTree);
}

bool prepare(Run& r, void* db) {
    r.db = db;
    r.d = static_cast<U8*>(db);
    const U8* d = r.d;
    if (d[0x15a3] != 1) return false;                     // not in portal mode (the main view always is)
    if (rdp(d, 0x15b8) != nullptr) return false;          // the extract filter callback
    r.admin = rdp(reinterpret_cast<const void*>(g_base + kRvaPortalAdmin), 0);
    if (r.admin == nullptr || rd<std::uint8_t>(r.admin, 0x61) == 1) return false;  // (+0x61: the occluder debug list)
    if (rdp(d, 0x118) == nullptr) return false;           // no frustum beam (UpdateFrameDependencies did not build one)
    const U8* filter = d + 0x28;
    r.f.sphereRadius = rd<float>(filter, 0x44);
    std::memcpy(r.f.sphereCenter, filter + 0x48, 12);
    std::memcpy(r.f.aabbMin, filter + 0x54, 12);
    std::memcpy(r.f.aabbMax, filter + 0x60, 12);
    const U8* cam = static_cast<const U8*>(rdp(d, 0x14c0));
    if (cam == nullptr) return false;
    std::memcpy(r.f.camera, cam + 0x90, 12);
    r.count0 = rd<std::int32_t>(d, 0x20);
    r.factor = rd<float>(d, 0x15b4);
    r.noRange = d[0x15a9] != 0;
    r.counter = rd<std::uint32_t>(reinterpret_cast<const void*>(g_base + kRvaCounter), 0);
    {
        std::uint64_t h = fnv(0xcbf29ce484222325ull, r.f.camera, 12);
        h = fnv(h, d + 0x94, 0x14);
        const std::int32_t np = rd<std::int32_t>(d, 0x118 + 0x12e0);
        if (np > 0 && np < 4096) h = fnv(h, rdp(d, 0x118 + 0x12d8), 0x14 * static_cast<std::size_t>(np));
        r.inputHash = h;
    }
    ensureBeams();
    r.fin = g_fin.get();
    return true;
}

void compareWithEngine(Run& r, double engineUs) {
    ++g_w.compared;
    const U8* d = r.d;
    bool differ = false;
    // the element list
    const std::int32_t n = rd<std::int32_t>(d, 0x20);
    const auto* list = static_cast<void* const*>(rdp(d, 0x18));
    // (the engine's list started at the same count as the dry run: Extract resets it before the call)
    const std::int32_t start = r.count0;
    bool listEq = n - start == static_cast<std::int32_t>(r.items.size());
    if (listEq)
        for (std::size_t i = 0; i < r.items.size(); ++i)
            if (list[static_cast<std::size_t>(start) + i] != r.items[i]) {
                listEq = false;
                break;
            }
    if (!listEq) {
        ++g_w.listDiff;
        differ = true;
        if (g_diffLog > 0) {
            --g_diffLog;
            std::size_t first = 0;
            while (first < r.items.size() && static_cast<std::int32_t>(start + first) < n && list[static_cast<std::size_t>(start) + first] == r.items[first]) ++first;
            spdlog::warn("native: own extract: element list DIFFERS: engine {} native {} (first difference at {}; camera in a room {}, occluders {}, polygons {})", n - start, r.items.size(), first, r.cameraRoom, r.occAdded,
                         r.polysAdded);
        }
    }
    // the final beam
    const U8* t = d + 0x118;
    const bool beamEq = sameNode(static_cast<const BeamNode*>(rdp(t, 0)), static_cast<const BeamPlane*>(rdp(t, 0x12d8)), r.fin->root, r.fin->planes) &&
                        rd<std::int32_t>(t, 0x12e0) == static_cast<std::int32_t>(r.fin->planeCount);
    if (!beamEq) {
        ++g_w.beamDiff;
        differ = true;
        if (g_diffLog > 0) {
            --g_diffLog;
            spdlog::warn("native: own extract: final beam DIFFERS: engine {} nodes {} planes, native {} / {} (camera in a room {}, occluders {}, polygons {})", rd<std::uint32_t>(t, 0x12c8), rd<std::int32_t>(t, 0x12e0),
                         r.fin->nodeCount, r.fin->planeCount, r.cameraRoom, r.occAdded, r.polysAdded);
        }
    }
    if (rd<std::uint32_t>(t, 0x12c8) != r.fin->nodeCount) ++g_w.nodeCountDiff;
    // the occluders' descs and indices (before the sort)
    if (!r.descs.empty() || !g_recDescs.empty()) {
        ++g_w.occCompared;
        g_w.descsCompared += r.descs.size();
        bool eq = r.descs.size() == g_recDescs.size() && r.idx == g_recIdx;
        for (std::size_t i = 0; eq && i < r.descs.size(); ++i) {
            const OccDesc& a = r.descs[i];
            const OccDesc& b = g_recDescs[i];
            eq = a.verts == b.verts && std::memcmp(a.plane, b.plane, 16) == 0 && std::memcmp(&a.dist, &b.dist, 4) == 0 && a.first == b.first && a.count == b.count;
        }
        if (!eq) {
            ++g_w.occDiff;
            differ = true;
            if (g_diffLog > 0) {
                --g_diffLog;
                spdlog::warn("native: own extract: occluder silhouettes DIFFER: engine {} descs {} indices, native {} / {}", g_recDescs.size(), g_recIdx.size(), r.descs.size(), r.idx.size());
            }
        }
    }
    if (differ) ++g_w.differing;
    (void)engineUs;
}

void detourExtract(void* db) {
    ++g_w.calls;
    const int want = settingMode();
    if (want != g_lastSetting) {
        g_lastSetting = want;
        g_tripped = 0;
    }
    if (want == 0 || g_tripped != 0 || db == nullptr || db != f10req::mainDb()) return g_extract.original(db);
    ++g_w.mainCalls;
    const bool compare = want == 2 || f10req::nextViewIsCheck();
    Run r;
    if (!prepare(r, db)) {
        ++g_w.fallbacks;
        g_w.lastFallback = "not portal mode / filter callback / occluder debug / no beam";
        return g_extract.original(db);
    }
    // compare views alternate the order: the port first (dry) then the engine, or the engine first then the port dry from the captured entry beam
    static std::uint64_t compareViews = 0;
    const bool engineFirst = compare && (++compareViews & 1) == 0;
    std::unique_ptr<NBeam>& entry = g_entryBeam;
    if (engineFirst) {
        if (!entry) entry = std::make_unique<NBeam>();
        const U8* tree = r.d + 0x118;
        entry->copyFromEngine(static_cast<const BeamNode*>(rdp(tree, 0)), static_cast<const BeamPlane*>(rdp(tree, 0x12d8)), static_cast<std::uint32_t>(std::max(rd<std::int32_t>(tree, 0x12e0), 0)));
        if (entry->overflow) {
            ++g_w.fallbacks;
            g_w.lastFallback = "frustum beam larger than the native tree";
            return g_extract.original(db);
        }
        g_recDescs.clear();
        g_recIdx.clear();
        g_recording = true;
        const std::uint64_t e0 = qpc();
        g_extract.original(db);
        const std::uint64_t e1 = qpc();
        g_recording = false;
        r.entry = entry.get();
        r.counter ^= 0x40000000u;  // a stamp value no entity holds: the engine's stamps of this call do not hide anything from the dry port
        const bool ok = guardedBody(&r);
        const std::uint64_t e2 = qpc();
        undoStamps(r);
        if (!ok || r.abort != nullptr) {
            if (!ok) {
                ++g_w.faults;
                g_tripped = 1;
                spdlog::error("native: own extract: a fault in the native extraction (compare): the stage is off until the setting changes");
            } else {
                ++g_w.fallbacks;
                g_w.lastFallback = r.abort;
            }
            return;
        }
        census(r);
        ++g_w.compare;
        ++g_w.engineFirst;
        g_w.engineFirstUs += qpcUs(e1 - e0);
        g_w.dryAfterUs += qpcUs(e2 - e1);
        compareWithEngine(r, qpcUs(e1 - e0));
        return;
    }
    const std::uint64_t t0 = qpc();
    const bool ok = guardedBody(&r);
    const std::uint64_t t1 = qpc();
    if (!ok || r.abort != nullptr) {
        undoStamps(r);
        if (!ok) {
            ++g_w.faults;
            g_tripped = 1;
            spdlog::error("native: own extract: a fault in the native extraction: the stage is off until the setting changes");
        } else {
            ++g_w.fallbacks;
            g_w.lastFallback = r.abort;
        }
        return g_extract.original(db);
    }
    census(r);
    if (!compare) {
        commit(r);
        ++g_w.live;
        ++g_runLive;
        g_w.liveUs += qpcUs(qpc() - t0);
        g_w.commitUs += qpcUs(qpc() - t1);
        return;
    }
    // compare (port first): the stamps undone, the engine's function, the results compared
    undoStamps(r);
    ++g_w.compare;
    g_w.dryUs += qpcUs(t1 - t0);
    g_recDescs.clear();
    g_recIdx.clear();
    g_recording = true;
    const std::uint64_t e0 = qpc();
    g_extract.original(db);
    const std::uint64_t e1 = qpc();
    g_recording = false;
    g_w.engineUs += qpcUs(e1 - e0);
    compareWithEngine(r, qpcUs(e1 - e0));
}

void lazyInit(HMODULE m) {
    g_base = reinterpret_cast<std::uintptr_t>(m);
    g_itemSetCount = reinterpret_cast<SetCountFn>(g_base + kRvaItemSetCount);
    g_planeSetCount = reinterpret_cast<SetCountFn>(g_base + kRvaPlaneSetCount);
    g_qsort = reinterpret_cast<QsortFn>(g_base + kRvaQsort);
    g_occCompare = reinterpret_cast<int (*)(const void*, const void*)>(g_base + kRvaOccCompare);
    g_getCell = reinterpret_cast<CellFn>(g_base + kRvaGetCell);
    g_treeModule = reinterpret_cast<ModuleFn>(g_base + kRvaTreeModule);
    g_cameraModule = reinterpret_cast<ModuleFn>(g_base + kRvaCameraModule);
    g_one = rd<float>(reinterpret_cast<const void*>(g_base + kRvaOne), 0);
    g_minArea = rd<float>(reinterpret_cast<const void*>(g_base + kRvaMinArea), 0);
    if (const HMODULE sb = GetModuleHandleW(L"SharedBase.dll")) {
        if (const auto* p = reinterpret_cast<const float*>(GetProcAddress(sb, "g_fEpsilon"))) g_epsilon = *p;
        if (const auto* p = reinterpret_cast<const float*>(GetProcAddress(sb, "g_fPI"))) g_pi = *p;
    }
    g_k.epsClass = g_one;
    g_k.epsBehind = rd<float>(reinterpret_cast<const void*>(g_base + kRvaEpsBehind), 0);
    g_k.epsBox = rd<float>(reinterpret_cast<const void*>(g_base + kRvaEpsBox), 0);
    g_k.epsLen = g_epsilon;
}

bool bytesAre(std::uintptr_t addr, const std::uint8_t* b, std::size_t n) { return std::memcmp(reinterpret_cast<const void*>(addr), b, n) == 0; }

}  // namespace

bool install() {
    char v[8]{};
    if (GetEnvironmentVariableA("RVK_F10_COMPARE", v, sizeof(v)) != 0 && v[0] != '0') {
        spdlog::info("native: own extract: not installed (RVK_F10_COMPARE=1 holds ExtractFromFrustum)");
        return false;
    }
    const HMODULE m = GetModuleHandleW(L"Engine.dll");
    if (m == nullptr) return false;
    lazyInit(m);
    static constexpr std::uint8_t kExtract[] = {0x40, 0x55, 0x41, 0x55, 0x41, 0x56, 0x48, 0x8d, 0xac, 0x24, 0x20, 0xec, 0xff, 0xff};
    static constexpr std::uint8_t kAddOcc[] = {0x4c, 0x89, 0x4c, 0x24, 0x20, 0x4c, 0x89, 0x44, 0x24, 0x18, 0x48, 0x89, 0x54, 0x24, 0x10};
    static constexpr std::uint8_t kSetCount[] = {0x48, 0x89, 0x6c, 0x24, 0x20, 0x56, 0x48, 0x83, 0xec, 0x40, 0x8b, 0xf2, 0x48, 0x8b, 0xe9};
    static constexpr std::uint8_t kCompare[] = {0xf3, 0x0f, 0x10, 0x41, 0x18, 0xf3, 0x0f, 0x10, 0x4a, 0x18, 0x0f, 0x2f, 0xc1};
    static constexpr std::uint8_t kQsort[] = {0x41, 0x54, 0x41, 0x55, 0x41, 0x56, 0x48, 0x81, 0xec, 0x50, 0x04, 0x00, 0x00};
    if (!bytesAre(g_base + kRvaExtract, kExtract, sizeof(kExtract)) || !bytesAre(g_base + kRvaAddOccluder, kAddOcc, sizeof(kAddOcc)) ||
        !bytesAre(g_base + kRvaItemSetCount, kSetCount, sizeof(kSetCount)) || !bytesAre(g_base + kRvaPlaneSetCount, kSetCount, sizeof(kSetCount)) ||
        !bytesAre(g_base + kRvaOccCompare, kCompare, sizeof(kCompare)) || !bytesAre(g_base + kRvaQsort, kQsort, sizeof(kQsort))) {
        spdlog::warn("native: own extract: the engine's functions do not match this build: not installed");
        return false;
    }
    bool ok = g_extract.install("eCFrustumDatabase::ExtractFromFrustum (F10-2)", reinterpret_cast<DbFn>(g_base + kRvaExtract), &detourExtract);
    ok = ok && g_addOcc.install("eCOccluder_PS::AddOccluder (F10-2 compare)", reinterpret_cast<AddOccFn>(g_base + kRvaAddOccluder), &detourAddOccluder);
    g_installed = ok;
    spdlog::info("native: own extract: {} (one {:.3f}, min area {:.3f}, epsilon {:.3g}, pi {:.6f}; portal constants behind {:.4g}, box margin {:.4g})", ok ? "installed" : "NOT installed: a hook failed", g_one, g_minArea,
                 g_epsilon, g_pi, g_k.epsBehind, g_k.epsBox);
    return ok;
}

void onPresent() {
    if (!g_installed) return;
    const auto now = std::chrono::steady_clock::now();
    if (std::chrono::duration<double>(now - g_lastReport).count() < 5.0) return;
    g_lastReport = now;
    Window& w = g_w;
    g_runCompared += w.compared;
    g_runDiffering += w.differing;
    if (w.differing != 0 && g_tripped == 0 && settingMode() == 1) {
        g_tripped = 1;
        spdlog::warn("native: own extract: DIFFERING in the window: the stage is off until the setting changes");
    }
    if (w.mainCalls != 0) {
        const double views = static_cast<double>(std::max<std::uint64_t>(w.live + w.compare, 1));
        const double nl = static_cast<double>(std::max<std::uint64_t>(w.live, 1)), nc = static_cast<double>(std::max<std::uint64_t>(w.compare, 1));
        spdlog::info("native: own extract (5 s): setting {}, {} main views (live {}, compare {}, fallback {}{}{}, faults {}); compare: {} views, DIFFERING {} (element list {}, final beam {}, occluder "
                     "silhouettes {} of {} views / {} descs; node count only {}); per view: {:.0f} elements, camera in a room {:.0f} %, rooms {:.0f} ({:.1f} entered from outside), occluders {:.1f} tested "
                     "{:.2f} added {:.2f} polygons, tree nodes {:.0f} (leaves gated {:.0f}); cost per view us: native live {:.1f} (commit {:.1f}); compare, port first: port {:.1f} then engine {:.1f}; engine first ({} views): engine {:.1f} then port {:.1f}; native parts: portal walk {:.1f} "
                     "occluders {:.1f} rooms {:.1f} sector tree {:.1f}; persistence census: inputs equal to the previous view {}, element list equal {} (of them with moved inputs {})",
                     settingMode(), w.mainCalls, w.live, w.compare, w.fallbacks, w.fallbacks != 0 ? ": " : "", w.lastFallback, w.faults, w.compared, w.differing, w.listDiff, w.beamDiff, w.occDiff, w.occCompared,
                     w.descsCompared, w.nodeCountDiff, static_cast<double>(w.elements) / views, 100.0 * static_cast<double>(w.cameraRoom) / views, static_cast<double>(w.rooms) / views,
                     static_cast<double>(w.roomsEntered) / views, static_cast<double>(w.occTested) / views, static_cast<double>(w.occAdded) / views, static_cast<double>(w.polys) / views,
                     static_cast<double>(w.treeNodes) / views, static_cast<double>(w.leaves) / views, w.liveUs / nl, w.commitUs / nl, w.dryUs / std::max(1.0, nc - static_cast<double>(w.engineFirst)), w.engineUs / std::max(1.0, nc - static_cast<double>(w.engineFirst)), w.engineFirst,
                     w.engineFirstUs / std::max<double>(1.0, static_cast<double>(w.engineFirst)), w.dryAfterUs / std::max<double>(1.0, static_cast<double>(w.engineFirst)), w.portalUs / views, w.occUs / views,
                     w.roomsUs / views, w.treeUs / views, w.sameInput, w.sameList, w.sameListMoved);
    }
    w = Window{};
    g_diffLog = 8;
}

StageState stageState() {
    StageState s;
    s.setting = settingMode();
    s.armed = g_installed && g_tripped == 0 ? s.setting : 0;
    s.live = g_runLive;
    s.compared = g_runCompared + g_w.compared;
    s.differing = g_runDiffering + g_w.differing;
    return s;
}

}  // namespace rvk::native::f10x
