// F10-0b (docs/research/frame-ownership.md 7.31b): the portal walk's polygon clipping and beam trees, pure.

#include <array>
#include <cmath>
#include <cstring>
#include <vector>

#include "native/f10_math.h"
#include "native/f10_portal.h"
#include "test.h"

using namespace rvk::native::f10;

namespace {
const float kVerts[12] = {-1, -1, 10, 1, -1, 10, 1, 1, 10, -1, 1, 10};  // a 2 x 2 portal at z = 10, seen from the origin
PortalConsts testConsts() {
    PortalConsts k;
    k.epsClass = 0.01f;
    k.epsBehind = -0.01f;
    k.epsBox = 0.0f;
    k.epsLen = 1e-6f;
    return k;
}
std::array<float, 6> boxAt(float x, float y, float z, float h = 0.25f) { return {x - h, y - h, z - h, x + h, y + h, z + h}; }
}  // namespace

TEST(f10_portal_corner_tables_follow_the_selector_bits) {
    const CornerTables& t = cornerTables();
    CHECK(t.in[0] == 0 && t.in[1] == 1 && t.in[2] == 2 && t.out[0] == 3 && t.out[1] == 4 && t.out[2] == 5);  // selector 0
    CHECK(t.in[3] == 3 && t.in[4] == 1 && t.in[5] == 2 && t.out[3] == 0 && t.out[4] == 4 && t.out[5] == 5);  // selector 1: x negative
    CHECK(t.in[21] == 3 && t.in[22] == 4 && t.in[23] == 5 && t.out[21] == 0 && t.out[22] == 1 && t.out[23] == 2);  // selector 7
}

TEST(f10_portal_clip_poly_keeps_the_right_side_and_edge_ids) {
    const PortalConsts k = testConsts();
    Poly p;
    p.n = 4;
    std::memcpy(p.v, kVerts, sizeof(kVerts));
    for (int i = 0; i < 4; ++i) p.id[i] = i;
    const float plane[4] = {1, 0, 0, 0};  // x >= 0 is the front
    Poly front, back;
    clipPoly(p, plane, false, front, k);
    clipPoly(p, plane, true, back, k);
    CHECK(front.n >= 3 && back.n >= 3);
    for (int i = 0; i < front.n; ++i) CHECK(front.v[i][0] >= -k.epsClass);
    for (int i = 0; i < back.n; ++i) CHECK(back.v[i][0] <= k.epsClass);
    int clipEdges = 0;
    for (int i = 0; i < front.n; ++i) clipEdges += front.id[i] == -1 ? 1 : 0;
    CHECK(clipEdges == 1);  // exactly one new edge along the plane
    // a polygon wholly in front: the front copy is the whole polygon, the back copy is empty
    const float farPlane[4] = {1, 0, 0, 5};  // x >= -5
    clipPoly(p, farPlane, false, front, k);
    clipPoly(p, farPlane, true, back, k);
    CHECK(front.n == 4 && back.n == 0);
}

TEST(f10_portal_poly_makes_the_cone_beyond_the_portal) {
    const PortalConsts k = testConsts();
    NBeam src, dest;
    // the source beam: everything (one plane that every box is in front of)
    src.root = src.createNode();
    src.root->plane = src.appendPlane(0, 0, 1, 1000);
    const float cam[3] = {0, 0, 0}, normal[3] = {0, 0, 1}, view[4] = {0, 0, 1, 0};
    addPortalPoly(src, dest, kVerts, normal, cam, view, k);
    CHECK(dest.root != nullptr && !dest.overflow);
    const Beam b = dest.view();
    auto in = boxAt(0, 0, 20), side = boxAt(50, 0, 20), before = boxAt(0, 0, 5), outCone = boxAt(3, 0, 12), edgeBox = boxAt(1.9f, 0, 20, 0.5f);
    CHECK(beamCullBox(b, in.data()) != kCulled);
    CHECK(beamCullBox(b, side.data()) == kCulled);
    CHECK(beamCullBox(b, before.data()) == kCulled);  // between the camera and the portal plane
    CHECK(beamCullBox(b, outCone.data()) == kCulled);
    CHECK(beamCullBox(b, edgeBox.data()) != kCulled);  // crosses the cone's edge
}

TEST(f10_portal_poly_clipped_through_a_half_space_beam) {
    const PortalConsts k = testConsts();
    NBeam src, dest;
    src.root = src.createNode();
    src.root->plane = src.appendPlane(1, 0, 0, 0);  // only x >= 0 is visible
    const float cam[3] = {0, 0, 0}, normal[3] = {0, 0, 1}, view[4] = {0, 0, 1, 0};
    addPortalPoly(src, dest, kVerts, normal, cam, view, k);
    CHECK(dest.root != nullptr);
    const Beam b = dest.view();
    auto right = boxAt(1.0f, 0, 20, 0.25f), left = boxAt(-1.0f, 0, 20, 0.25f);
    CHECK(beamCullBox(b, right.data()) != kCulled);
    CHECK(beamCullBox(b, left.data()) == kCulled);
    // a source beam that sees nothing of the portal leaves the destination empty
    NBeam src2, dest2;
    src2.root = src2.createNode();
    src2.root->plane = src2.appendPlane(-1, 0, 0, -50);  // x <= -50
    addPortalPoly(src2, dest2, kVerts, normal, cam, view, k);
    CHECK(dest2.root == nullptr);
}

TEST(f10_portal_beam_copy_is_structurally_equal) {
    const PortalConsts k = testConsts();
    NBeam src, dest, copy;
    src.root = src.createNode();
    src.root->plane = src.appendPlane(0, 0, 1, 1000);
    const float cam[3] = {0, 0, 0}, normal[3] = {0, 0, 1}, view[4] = {0, 0, 1, 0};
    addPortalPoly(src, dest, kVerts, normal, cam, view, k);
    copy.copyFrom(dest);
    CHECK(copy.nodeCount == dest.nodeCount && copy.planeCount == dest.planeCount);
    CHECK(sameNode(reinterpret_cast<const BeamNode*>(dest.root), dest.planes, copy.root, copy.planes));
    // the same from the engine's node layout
    NBeam fromEngine;
    fromEngine.copyFromEngine(reinterpret_cast<const BeamNode*>(dest.root), dest.planes, dest.planeCount);
    CHECK(sameNode(reinterpret_cast<const BeamNode*>(dest.root), dest.planes, fromEngine.root, fromEngine.planes));
    // AddPlane ANDs a plane in front of everything
    const float plane[4] = {0, 1, 0, 0};
    copy.addPlaneAtRoot(plane);
    auto below = boxAt(0, -3, 20);
    CHECK(beamCullBox(copy.view(), below.data()) == kCulled);
}

namespace {
// rooms are 1-based ids; a portal's `to` is a room id, 0 = outdoors
struct ToyGraph {
    struct P {
        float v[12], n[3], bmin[3], bmax[3];
        int to;
    };
    std::vector<P> rooms[2];
    std::vector<std::array<float, 6>> entities[2];
    std::vector<int> listed[2];     // per room: indices of the entities that passed the beam
    std::vector<int> roomEntityOf;  // rooms whose own entity was extracted
    int portalCount(int r) { return static_cast<int>(rooms[r - 1].size()); }
    PortalRef portal(int r, int i) {
        const P& p = rooms[r - 1][static_cast<std::size_t>(i)];
        return {p.v, p.n, p.bmin, p.bmax};
    }
    int portalRoom(int r, int i) { return rooms[r - 1][static_cast<std::size_t>(i)].to; }
    void extractRoomEntity(int r) { roomEntityOf.push_back(r); }
    void extractRoomList(int r, const Beam& b) {
        for (std::size_t i = 0; i < entities[r - 1].size(); ++i)
            if (beamCullBox(b, entities[r - 1][i].data()) != kCulled) listed[r - 1].push_back(static_cast<int>(i));
    }
};
ToyGraph::P makePortal(float z, float halfW, int to, float nz) {
    ToyGraph::P p{};
    const float v[12] = {-halfW, -halfW, z, halfW, -halfW, z, halfW, halfW, z, -halfW, halfW, z};
    std::memcpy(p.v, v, sizeof(v));
    p.n[0] = 0;
    p.n[1] = 0;
    p.n[2] = nz;
    p.bmin[0] = p.bmin[1] = -halfW;
    p.bmax[0] = p.bmax[1] = halfW;
    p.bmin[2] = z - 0.1f;
    p.bmax[2] = z + 0.1f;
    p.to = to;
    return p;
}
}  // namespace

TEST(f10_portal_walk_visits_the_rooms_seen_through_portals) {
    const PortalConsts k = testConsts();
    ToyGraph g;
    // room A (1) has the portal to room B (2); the portal faces the camera (normal -z: (v0 - cam) . n <= 0)
    g.rooms[0].push_back(makePortal(10, 1, 2, -1));
    // room B (2): a portal back to A (faces away from the camera: skipped) and a window to the outdoors at z = 30
    g.rooms[1].push_back(makePortal(10, 1, 1, +1));
    g.rooms[1].push_back(makePortal(30, 1.5f, 0, -1));
    g.entities[0] = {boxAt(0, 0, 5)};
    g.entities[1] = {boxAt(0, 0, 20), boxAt(40, 0, 20), boxAt(0, 0, 40)};
    NBeam frustum, fin;
    frustum.root = frustum.createNode();
    frustum.root->plane = frustum.appendPlane(0, 0, 1, 1000);
    const float cam[3] = {0, 0, 0}, view[4] = {0, 0, 1, 0};
    walkRooms<ToyGraph, int>(g, k, cam, view, 1, frustum, true, 0, fin);
    CHECK(g.listed[0].size() == 1);                         // A's list is tested against the frustum
    CHECK(g.listed[1].size() == 2 && g.listed[1][0] == 0 && g.listed[1][1] == 2);  // B: the entities inside the cone through the portal (not the side one)
    CHECK(g.roomEntityOf.size() == 1 && g.roomEntityOf[0] == 2);
    CHECK(fin.root != nullptr);  // the window to the outdoors added its polygon to the final beam
    auto through = boxAt(0, 0, 60), aside = boxAt(25, 0, 60), nearBox = boxAt(0, 0, 15);
    CHECK(beamCullBox(fin.view(), through.data()) != kCulled);
    CHECK(beamCullBox(fin.view(), aside.data()) == kCulled);
    CHECK(beamCullBox(fin.view(), nearBox.data()) == kCulled);
}

TEST(f10_portal_camera_inside_a_portal_box_passes_the_beam_on) {
    const PortalConsts k = testConsts();
    ToyGraph g;
    ToyGraph::P p = makePortal(0.05f, 1, 0, -1);  // the camera is within the portal's box: no clipping, the whole beam is copied to the final tree
    p.bmin[2] = -1;
    p.bmax[2] = 1;
    g.rooms[0].push_back(p);
    NBeam frustum, fin;
    frustum.root = frustum.createNode();
    frustum.root->plane = frustum.appendPlane(0, 0, 1, 1000);
    const float cam[3] = {0, 0, 0}, view[4] = {0, 0, 1, 0};
    walkRooms<ToyGraph, int>(g, k, cam, view, 1, frustum, true, 0, fin);
    CHECK(fin.root != nullptr && fin.nodeCount == frustum.nodeCount && fin.planeCount == frustum.planeCount);
}
