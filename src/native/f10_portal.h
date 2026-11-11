#pragma once

// F10-0b (docs/research/frame-ownership.md 7.31b): the engine's portal-room walk written down as pure code (no engine calls; the data it reads is passed in).
// Sources, all decompiled / disassembled from Engine.dll this stage:
//   eCFrustumDatabase::ExtractFromPortalRooms 0x66db40 / ExtractFromPortalRoom 0x66daa0 (the walk), ExtractFromFrustum 0x66fa60 (the camera room's entry and the
//   outdoor entry of every room), eCBeamTree::AddPoly 0x67afb0 (the core: a convex polygon clipped through a beam tree, the visible fragments added to a
//   destination tree) with its three wrappers 0x67cb90 / 0x67acb0 / 0x67d120, ClipPoly helpers 0x67c120 (keep the back part) / 0x67c510 (keep the front part) /
//   0x67c900 (classify), CopyFrom 0x67c080 / 0x67c9e0, AddPlane 0x67d040, eCPortalAdmin::FindPortalRoom 0x672610.
// The beam tree is the engine's (eCBeamTree: root, 200 nodes of 0x18 bytes, a plane array of 0x14-byte planes {a b c d, selector}); NBeam keeps that layout so the
// cull functions of f10_math.h (beamCullBox / beamCullSphere) run on both.

#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <vector>

#include "native/f10_math.h"

namespace rvk::native::f10 {

// the float constants of the engine's data section (read once at run time; defaults are what the build we studied holds)
struct PortalConsts {
    float epsClass = 1.0f;    // DAT_1809f1698: a point within +-eps of a plane lies on it (clip thresholds); the classify "front" bound
    float epsBehind = -1.0f;  // DAT_1809f1854: the classify "behind" bound
    float epsBox = 0.0f;      // DAT_1809f16f8: the portal box's margin around the camera
    float epsLen = 1e-6f;     // SharedBase g_fEpsilon: the edge plane's normal length^2 above which it is normalised
};

struct NNode {
    std::int32_t plane;
    std::int32_t pad;
    NNode* inside;  // +8: the part behind the plane
    NNode* next;    // +0x10: the part in front of it
};
static_assert(sizeof(NNode) == 0x18);

// the box corner tables the engine's constructor writes at +0x12e8 / +0x1348 of every tree (selector bits 1 / 2 / 4 = the sign of the plane normal's x / y / z)
struct CornerTables {
    std::uint32_t in[24];
    std::uint32_t out[24];
};
inline const CornerTables& cornerTables() {
    static const CornerTables t = [] {
        CornerTables c{};
        for (int s = 0; s < 8; ++s)
            for (int k = 0; k < 3; ++k) {
                const bool bit = ((1 << k) & s) != 0;
                c.in[3 * s + k] = bit ? static_cast<std::uint32_t>(k + 3) : static_cast<std::uint32_t>(k);
                c.out[3 * s + k] = bit ? static_cast<std::uint32_t>(k) : static_cast<std::uint32_t>(k + 3);
            }
        return c;
    }();
    return t;
}

inline std::uint32_t planeSelector(float a, float b, float c) { return (c < 0.0f ? 4u : 0u) | (b < 0.0f ? 2u : 0u) | (a < 0.0f ? 1u : 0u); }

class NBeam {
public:
    static constexpr std::uint32_t kMaxNodes = 200;
    static constexpr std::uint32_t kMaxPlanes = 512;
    NNode* root = nullptr;
    std::uint32_t nodeCount = 0;
    std::uint32_t planeCount = 0;
    bool overflow = false;  // the engine would write through a null node (more than 200 nodes) or past the plane array: the native walk is not trusted for this view
    NNode nodes[kMaxNodes];
    BeamPlane planes[kMaxPlanes];

    NBeam() = default;
    NBeam(const NBeam&) = delete;
    NBeam& operator=(const NBeam&) = delete;
    void reset() {
        root = nullptr;
        nodeCount = 0;
        planeCount = 0;
        overflow = false;
    }
    NNode* createNode() {
        if (nodeCount >= kMaxNodes) {
            overflow = true;
            return nullptr;
        }
        NNode* n = &nodes[nodeCount++];
        n->inside = nullptr;
        n->next = nullptr;
        return n;
    }
    // eCBeamTree::AddPlane-like: append a plane, returns its index
    int appendPlane(float a, float b, float c, float d) {
        if (planeCount >= kMaxPlanes) {
            overflow = true;
            return 0;
        }
        BeamPlane& p = planes[planeCount];
        p.a = a;
        p.b = b;
        p.c = c;
        p.d = d;
        p.selector = planeSelector(a, b, c);
        return static_cast<int>(planeCount++);
    }
    Beam view() const {
        Beam v;
        v.root = reinterpret_cast<const BeamNode*>(root);
        v.planes = planes;
        v.cornerIn = cornerTables().in;
        v.cornerOut = cornerTables().out;
        return v;
    }
    // eCBeamTree::CopyFrom(node): preorder, the inside child first, then the next child (the engine's node numbering)
    void copyNode(NNode** dst, const NNode* src) {
        NNode* n = createNode();
        *dst = n;
        if (n == nullptr) return;
        n->plane = src->plane;
        if (src->inside != nullptr) copyNode(&n->inside, src->inside);
        if (src->next != nullptr) copyNode(&n->next, src->next);
    }
    // eCBeamTree::CopyFrom(tree) 0x67c9e0: the planes (selector included) and the node tree
    void copyFrom(const NBeam& o) {
        reset();
        planeCount = o.planeCount;
        std::memcpy(planes, o.planes, sizeof(BeamPlane) * o.planeCount);
        if (o.root != nullptr) copyNode(&root, o.root);
    }
    // the same from an engine tree (node = the engine's layout, planes = its array)
    void copyFromEngine(const BeamNode* srcRoot, const BeamPlane* srcPlanes, std::uint32_t srcPlaneCount) {
        reset();
        if (srcPlaneCount > kMaxPlanes) {
            overflow = true;
            srcPlaneCount = kMaxPlanes;
        }
        planeCount = srcPlaneCount;
        std::memcpy(planes, srcPlanes, sizeof(BeamPlane) * srcPlaneCount);
        if (srcRoot != nullptr) copyEngineNode(&root, srcRoot);
    }
    // eCBeamTree::AddPlane 0x67d040: AND with a plane (a new root whose next child is the old root)
    void addPlaneAtRoot(const float* plane4) {
        const int idx = appendPlane(plane4[0], plane4[1], plane4[2], plane4[3]);
        NNode* n = createNode();
        if (n == nullptr) return;
        n->next = root;
        n->plane = idx;
        root = n;
    }
    // eCBeamTree::Init(matrix, portalMode) is not ported: the frustum tree is read from the engine (documented in 7.31b)

private:
    void copyEngineNode(NNode** dst, const BeamNode* src) {
        NNode* n = createNode();
        *dst = n;
        if (n == nullptr) return;
        n->plane = src->plane;
        if (src->inside != nullptr) copyEngineNode(&n->inside, src->inside);
        if (src->next != nullptr) copyEngineNode(&n->next, src->next);
    }
};

// ---- eCBeamTree::eSOccluderPoly / eSAddPolyInfo ----
struct Poly {
    float v[40][3];       // +0
    std::int32_t id[40];  // +0x1e0: the id of the edge that starts at vertex k (-1 = a clip edge)
    std::int32_t n = 0;   // +0x280
};
static_assert(sizeof(Poly) == 0x284);

struct AddInfo {
    std::int32_t edgePlane[40];  // +0: the plane index of each edge id (-1 = not made yet)
    std::int32_t polyPlane = -1; // +0xa0
    float plane[4]{};            // +0xa4: the polygon's own plane
    float origin[3]{};           // +0xb4: the camera (the edge planes pass through it)
    bool chainNext = false;      // +0xc0: the chain of edge planes continues through the "next" (front) child instead of "inside"
    NBeam* dest = nullptr;       // +0xc8: when set, fragments that reach an empty slot are added to this tree instead
    bool destChainNext = false;  // +0xd0
    AddInfo() { std::memset(edgePlane, 0xff, sizeof(edgePlane)); }
};

// 0x67c900: 0 = every vertex in front (beyond +eps), 1 = every vertex behind, 2 = both, 3 = on the plane / empty
inline int classifyPoly(const Poly& p, const float* pl, const PortalConsts& k) {
    int front = 0, behind = 0;
    for (int i = 0; i < p.n; ++i) {
        const float cp = p.v[i][1] * pl[1] + p.v[i][0] * pl[0] + p.v[i][2] * pl[2] + pl[3];
        if (cp <= k.epsClass) {
            if (cp < k.epsBehind) ++behind;
        } else {
            ++front;
        }
    }
    if (p.n > 0) {
        if (front > 0 && behind == 0) return 0;
        if (behind > 0 && front == 0) return 1;
        if (behind != 0 || front != 0) return 2;
    }
    return 3;
}

namespace detail {
inline void intersect(const float* cur, const float* nxt, float t, float* out) {
    const float diff[3] = {nxt[0] - cur[0], nxt[1] - cur[1], nxt[2] - cur[2]};
    out[0] = cur[0] + diff[0] * t;
    out[1] = cur[1] + diff[1] * t;
    out[2] = cur[2] + diff[2] * t;
}
// set when a clip would write a 41st vertex (the engine's fixed polygon would overflow): the caller does not trust the walk (F10-2 falls back to the engine)
inline thread_local bool g_polyOverflow = false;
inline void emit(Poly& out, const float* v, std::int32_t id) {
    if (out.n >= 40) {
        g_polyOverflow = true;
        return;
    }
    std::memcpy(out.v[out.n], v, 12);
    out.id[out.n] = id;
    ++out.n;
}
}  // namespace detail

// FUN_18067c120 (keepBack = true: the part of the polygon behind the plane) and FUN_18067c510 (false: the part in front); out.n = 0 when nothing remains
inline void clipPoly(const Poly& p, const float* pl, bool keepBack, Poly& out, const PortalConsts& k) {
    out.n = 0;
    const int c = classifyPoly(p, pl, k);
    if (keepBack ? (c == 0 || c == 3) : (c == 1 || c == 3)) return;
    if (keepBack ? c == 1 : c == 0) {
        out = p;
        return;
    }
    const float lo = (-pl[3]) - k.epsClass, hi = k.epsClass - pl[3];
    float d0 = p.v[1 - 1][1] * pl[1] + pl[0] * p.v[0][0] + p.v[0][2] * pl[2];
    const int n = p.n;
    for (int i = 0; i < n;) {
        const int j = (i + 1) % n;
        const float* cur = p.v[i];
        const float* nxt = p.v[j];
        const float d1 = nxt[1] * pl[1] + nxt[0] * pl[0] + nxt[2] * pl[2];
        float ip[3];
        auto cross = [&]() {
            const float t = (pl[3] + d0) / (d0 - d1);
            detail::intersect(cur, nxt, t, ip);
        };
        if (keepBack) {
            if (d1 >= lo) {
                if (d1 <= hi) {
                    detail::emit(out, nxt, p.id[j]);  // on the plane: kept
                } else if (d0 < lo) {                 // in front, the previous vertex behind: a clip edge starts
                    cross();
                    detail::emit(out, ip, -1);
                }
            } else {  // behind
                if (d0 > hi) {
                    cross();
                    detail::emit(out, ip, p.id[i]);
                }
                detail::emit(out, nxt, p.id[j]);
            }
        } else {
            if (d1 >= lo) {
                if (d1 <= hi) {
                    detail::emit(out, nxt, p.id[j]);
                } else {  // in front
                    if (d0 < lo) {
                        cross();
                        detail::emit(out, ip, p.id[i]);
                    }
                    detail::emit(out, nxt, p.id[j]);
                }
            } else if (d0 > hi) {  // behind, the previous vertex in front: the polygon leaves the front side
                cross();
                detail::emit(out, ip, -1);
            }
        }
        d0 = d1;
        i = i + 1;
    }
    if (out.n < 3) out.n = 0;
}

namespace detail {
inline void normalizeVec3(float* v) {
    const float len = std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
    if (0.0f < len) {
        const float s = 1.0f / len;
        v[0] *= s;
        v[1] *= s;
        v[2] *= s;
    }
}
}  // namespace detail

// 0x67afb0: add the polygon to `tree`, starting at *slot (see the file header). `tree` supplies the planes and the node pool of every write; when
// info.dest != null the walk is read-only on `tree` and the visible fragments go to info.dest
inline void addPoly(NBeam& tree, NNode** slot, Poly& poly, AddInfo& info, const PortalConsts& k) {
    if (poly.n <= 0) return;
    NNode* node = *slot;
    if (node == nullptr) {
        if (info.dest == nullptr) {
            NNode** chain = slot;
            const int n = poly.n;
            for (int e = 0; e < n; ++e) {
                const std::int32_t id = poly.id[e];
                if (id == -1) continue;
                const int next = (e + 1) % n;
                NNode* nd = tree.createNode();
                *chain = nd;
                if (nd == nullptr) return;
                int pi = info.edgePlane[id];
                if (pi == -1) {
                    pi = static_cast<int>(tree.planeCount);
                    info.edgePlane[id] = pi;
                    const float a[3] = {poly.v[e][0] - info.origin[0], poly.v[e][1] - info.origin[1], poly.v[e][2] - info.origin[2]};
                    const float b[3] = {poly.v[next][0] - poly.v[e][0], poly.v[next][1] - poly.v[e][1], poly.v[next][2] - poly.v[e][2]};
                    float nrm[3] = {b[2] * a[1] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - b[0] * a[1]};
                    const float len2 = nrm[0] * nrm[0] + nrm[1] * nrm[1] + nrm[2] * nrm[2];
                    if (k.epsLen < len2) detail::normalizeVec3(nrm);
                    const float d = ((-(nrm[0] * info.origin[0])) - nrm[1] * info.origin[1]) - nrm[2] * info.origin[2];
                    tree.appendPlane(nrm[0], nrm[1], nrm[2], d);
                }
                nd->plane = pi;
                chain = info.chainNext ? &nd->next : &nd->inside;
            }
            NNode* leaf = tree.createNode();
            *chain = leaf;
            if (leaf == nullptr) return;
            int pp = info.polyPlane;
            if (pp == -1) {
                pp = tree.appendPlane(info.plane[0], info.plane[1], info.plane[2], info.plane[3]);
                info.polyPlane = pp;
            }
            leaf->plane = pp;
        } else {
            // the visible fragment of a clip goes to the destination tree, its edges numbered afresh
            AddInfo info2;
            std::memcpy(info2.plane, info.plane, sizeof(info2.plane));
            std::memcpy(info2.origin, info.origin, sizeof(info2.origin));
            info2.chainNext = info.destChainNext;
            info2.dest = nullptr;
            info2.destChainNext = false;
            for (int e = 0; e < poly.n; ++e) poly.id[e] = e;
            addPoly(*info.dest, &info.dest->root, poly, info2, k);
        }
        return;
    }
    if (node->inside != nullptr || info.chainNext) {
        Poly part;
        clipPoly(poly, &tree.planes[node->plane].a, true, part, k);
        if (part.n > 0) addPoly(tree, &node->inside, part, info, k);
    }
    if (node->next != nullptr || !info.chainNext) {
        Poly part;
        clipPoly(poly, &tree.planes[node->plane].a, false, part, k);
        if (part.n > 0) addPoly(tree, &node->next, part, info, k);
    }
}

// 0x67cb90: a portal polygon (4 vertices) seen from `origin`, clipped through `src`, the visible part added to `dest` (chain through "next", as the engine's callers pass)
inline void addPortalPoly(NBeam& src, NBeam& dest, const float* verts4, const float* normal, const float* origin, const float* viewPlane, const PortalConsts& k) {
    Poly poly;
    poly.n = 4;
    std::memcpy(poly.v, verts4, 4 * 12);
    int best = 0;
    for (int i = 0; i < 4; ++i) {
        poly.id[i] = i;
        if (0.0f < viewPlane[0] * poly.v[i][0] + viewPlane[1] * poly.v[i][1] + viewPlane[2] * poly.v[i][2] + viewPlane[3]) best = i;
    }
    AddInfo info;
    info.origin[0] = origin[0];
    info.origin[1] = origin[1];
    info.origin[2] = origin[2];
    float pl[4] = {normal[0], normal[1], normal[2], 0.0f};
    pl[3] = ((-(normal[0] * poly.v[best][0])) - normal[1] * poly.v[best][1]) - normal[2] * poly.v[best][2];
    // bCPlane::Normalize: a reciprocal multiply of all four
    const float len = std::sqrt(pl[0] * pl[0] + pl[1] * pl[1] + pl[2] * pl[2]);
    const float s = 1.0f / len;
    pl[0] *= s;
    pl[1] *= s;
    pl[2] *= s;
    pl[3] *= s;
    std::memcpy(info.plane, pl, sizeof(pl));
    info.chainNext = false;
    info.dest = &dest;
    info.destChainNext = true;
    addPoly(src, &src.root, poly, info, k);
}

// ---- the room walk ----
// A room graph adapter G provides (rooms are opaque ids, 0 = none):
//   int portalCount(Room); PortalRef portal(Room, int); Room portalRoom(Room, int)  (0: the portal leads outdoors / to a cell that is not a room)
//   void extractRoomEntity(Room)                (ExtractFromEntity(room's entity, status 1))
//   void extractRoomList(Room, const Beam&)     (ExtractFromPortalRoom: each listed entity whose box is not culled by the beam)
struct PortalRef {
    const float* v;  // 4 vertices, 12 floats
    const float* normal;
    const float* boxMin;
    const float* boxMax;
};

inline bool cameraOutsidePortalBox(const PortalRef& p, const float* cam, float eps) {
    return !(cam[0] > p.boxMin[0] - eps && cam[1] > p.boxMin[1] - eps && cam[2] > p.boxMin[2] - eps && p.boxMax[0] + eps > cam[0] && p.boxMax[1] + eps > cam[1] &&
             p.boxMax[2] + eps > cam[2]);
}

// (v0 - camera) . normal with the engine's order of operations
inline float portalFacing(const PortalRef& p, const float* cam) {
    const float d[3] = {p.v[0] - cam[0], p.v[1] - cam[1], p.v[2] - cam[2]};
    return d[1] * p.normal[1] + d[0] * p.normal[0] + d[2] * p.normal[2];
}

template <class G, class Room>
void walkRooms(G& g, const PortalConsts& k, const float* cam, const float* viewPlane, Room room, NBeam& beam, bool build, Room from, NBeam& finalBeam, int depth = 0) {
    if (beam.root == nullptr || depth > 64) return;
    g.extractRoomList(room, beam.view());
    const int n = g.portalCount(room);
    for (int i = 0; i < n; ++i) {
        const PortalRef p = g.portal(room, i);
        if (!cameraOutsidePortalBox(p, cam, k.epsBox)) {
            const Room r = g.portalRoom(room, i);
            if (r == Room{}) {
                if (build) finalBeam.copyFrom(beam);
            } else if (r != from) {
                NBeam tmp;
                tmp.copyFrom(beam);
                g.extractRoomEntity(r);
                walkRooms(g, k, cam, viewPlane, r, tmp, build, room, finalBeam, depth + 1);
            }
            continue;
        }
        if (0.0f < portalFacing(p, cam)) continue;
        const Room r = g.portalRoom(room, i);
        const float inv[3] = {-p.normal[0], -p.normal[1], -p.normal[2]};
        if (r == Room{}) {
            if (build) addPortalPoly(beam, finalBeam, p.v, inv, cam, viewPlane, k);
        } else {
            NBeam tmp;
            addPortalPoly(beam, tmp, p.v, inv, cam, viewPlane, k);
            g.extractRoomEntity(r);
            walkRooms(g, k, cam, viewPlane, r, tmp, build, room, finalBeam, depth + 1);
        }
    }
}

// eCPortalAdmin::FindPortalRoom 0x672610 over plain data: rooms hold a bounding sphere {r, x, y, z} and a BSP of leaf planes; here the adapter does the lookup
// (the walk needs only the result), so only the point test of a leaf's planes is shared
inline bool pointInPlanes(const float* planes, int count, const float* p) {
    for (int i = 0; i < count; ++i) {
        const float* pl = planes + 32 * i;  // 0x80-byte planes, the first 4 floats are a b c d
        if (p[1] * pl[1] + p[0] * pl[0] + p[2] * pl[2] + pl[3] < 0.0f) return false;
    }
    return true;
}

// structural equality of two beam trees (planes compared by value, nodes by shape): the check of a native final beam against the engine's
inline bool sameNode(const BeamNode* a, const BeamPlane* pa, const NNode* b, const BeamPlane* pb) {
    if ((a == nullptr) != (b == nullptr)) return false;
    if (a == nullptr) return true;
    const BeamPlane& x = pa[a->plane];
    const BeamPlane& y = pb[b->plane];
    if (x.a != y.a || x.b != y.b || x.c != y.c || x.d != y.d) return false;
    return sameNode(a->inside, pa, b->inside, pb) && sameNode(a->next, pa, b->next, pb);
}

}  // namespace rvk::native::f10
