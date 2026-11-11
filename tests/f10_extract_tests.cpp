// F10-2 (docs/research/frame-ownership.md 14.33): the occluder port (eCOccluder_PS::AddOccluder) and ExtractFromFrustum's occluder add order, pure.

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <map>
#include <set>
#include <utility>
#include <vector>

#include "native/f10_math.h"
#include "native/f10_occluder.h"
#include "native/f10_portal.h"
#include "test.h"

using namespace rvk::native::f10;

namespace {

// a cube [-1, 1]^3 as the engine's occluder mesh: 12 triangles (outward normals, counter-clockwise seen from outside), the edges with their two faces,
// each face's three edges in its vertex order (v0 v1, v1 v2, v2 v0), one part holding everything
struct CubeMesh {
    std::vector<float> verts;
    std::vector<std::uint8_t> faces, edges, parts;
    OccMesh mesh() const {
        OccMesh m;
        m.verts = verts.data();
        m.faces = faces.data();
        m.faceCount = static_cast<std::int32_t>(faces.size() / 0x2c);
        m.edges = edges.data();
        m.edgeCount = static_cast<std::int32_t>(edges.size() / 0x14);
        m.parts = parts.data();
        m.partCount = 1;
        return m;
    }
};
template <class T>
void put(std::vector<std::uint8_t>& b, std::size_t off, T v) {
    std::memcpy(b.data() + off, &v, sizeof(T));
}

CubeMesh makeCube() {
    CubeMesh c;
    for (int i = 0; i < 8; ++i) {
        c.verts.push_back((i & 1) ? 1.0f : -1.0f);
        c.verts.push_back((i & 2) ? 1.0f : -1.0f);
        c.verts.push_back((i & 4) ? 1.0f : -1.0f);
    }
    // quads (counter-clockwise from outside) with their normals
    const int quads[6][4] = {{0, 2, 3, 1}, {4, 5, 7, 6}, {0, 1, 5, 4}, {2, 6, 7, 3}, {0, 4, 6, 2}, {1, 3, 7, 5}};
    const float normals[6][3] = {{0, 0, -1}, {0, 0, 1}, {0, -1, 0}, {0, 1, 0}, {-1, 0, 0}, {1, 0, 0}};
    std::vector<std::array<int, 3>> tris;
    std::vector<int> triQuad;
    for (int q = 0; q < 6; ++q) {
        tris.push_back({quads[q][0], quads[q][1], quads[q][2]});
        tris.push_back({quads[q][0], quads[q][2], quads[q][3]});
        triQuad.push_back(q);
        triQuad.push_back(q);
    }
    std::map<std::pair<int, int>, int> edgeOf;
    std::vector<std::array<int, 4>> edges;  // v0 v1 f0 f1
    std::vector<std::array<int, 3>> faceEdges(tris.size());
    for (std::size_t f = 0; f < tris.size(); ++f)
        for (int k = 0; k < 3; ++k) {
            const int a = tris[f][k], b = tris[f][(k + 1) % 3];
            const auto key = std::make_pair(std::min(a, b), std::max(a, b));
            auto it = edgeOf.find(key);
            if (it == edgeOf.end()) {
                edgeOf[key] = static_cast<int>(edges.size());
                edges.push_back({a, b, static_cast<int>(f), -1});
                faceEdges[f][k] = static_cast<int>(edges.size()) - 1;
            } else {
                edges[static_cast<std::size_t>(it->second)][3] = static_cast<int>(f);
                faceEdges[f][k] = it->second;
            }
        }
    c.faces.assign(0x2c * tris.size(), 0);
    for (std::size_t f = 0; f < tris.size(); ++f) {
        const std::size_t o = 0x2c * f;
        put<std::int32_t>(c.faces, o + 0, tris[f][0]);
        put<std::int32_t>(c.faces, o + 4, tris[f][1]);
        put<std::int32_t>(c.faces, o + 8, tris[f][2]);
        for (int k = 0; k < 3; ++k) put<std::int32_t>(c.faces, o + 0x10 + 4 * static_cast<std::size_t>(k), faceEdges[f][k]);
        for (int k = 0; k < 3; ++k) put<float>(c.faces, o + 0x1c + 4 * static_cast<std::size_t>(k), normals[triQuad[f]][k]);
        put<float>(c.faces, o + 0x28, 2.0f);
    }
    c.edges.assign(0x14 * edges.size(), 0);
    for (std::size_t e = 0; e < edges.size(); ++e)
        for (int k = 0; k < 4; ++k) put<std::int32_t>(c.edges, 0x14 * e + 4 * static_cast<std::size_t>(k), edges[e][k]);
    c.parts.assign(0x20, 0);
    put<float>(c.parts, 0, 1.75f);  // sphere r, centre 0
    put<std::uint32_t>(c.parts, 0x10, 0);
    put<std::uint32_t>(c.parts, 0x14, static_cast<std::uint32_t>(tris.size()));
    put<std::uint32_t>(c.parts, 0x18, 0);
    put<std::uint32_t>(c.parts, 0x1c, static_cast<std::uint32_t>(edges.size()));
    return c;
}

// a beam that holds everything in front of z = -100 (one plane: z + 100 >= 0)
struct OpenBeam {
    NBeam b;
    OpenBeam() {
        b.reset();
        const float p[4] = {0, 0, 1, 100};
        b.addPlaneAtRoot(p);
    }
};

OccFilter filterAt(const float* eye, const NBeam& beam) {
    OccFilter f;
    std::memcpy(f.camera, eye, 12);
    f.viewDir[0] = 0;
    f.viewDir[1] = 0;
    f.viewDir[2] = 1;
    f.sphereRadius = 1000.0f;
    std::memcpy(f.sphereCenter, eye, 12);
    f.viewPlane[0] = 0;
    f.viewPlane[1] = 0;
    f.viewPlane[2] = 1;
    f.viewPlane[3] = -eye[2];
    f.beam = beam.view();
    return f;
}

}  // namespace

TEST(f10x_occluder_cube_face_on_gives_one_quad_silhouette) {
    const CubeMesh c = makeCube();
    OpenBeam ob;
    const float eye[3] = {0.0f, 0.0f, -3.0f};
    const OccFilter f = filterAt(eye, ob.b);
    std::vector<OccDesc> descs;
    std::vector<std::uint32_t> idx;
    CHECK(addOccluder(c.mesh(), f, eye, descs, idx));
    CHECK(descs.size() == 1);
    if (descs.size() != 1) return;
    CHECK(descs[0].first == 0 && descs[0].count == 4 && idx.size() == 4);
    // the silhouette of the face z = -1: its four corners, each once
    const std::set<std::uint32_t> got(idx.begin(), idx.end());
    CHECK(got == (std::set<std::uint32_t>{0, 1, 2, 3}));
    // the plane passes through the farthest loop vertex along the view plane (all at z = -1: distance 2 (the area factor wants the occluder close), the first one found wins)
    CHECK(descs[0].dist == 2.0f);
    const float* v = c.verts.data() + 3 * idx[0];
    const float* n = descs[0].plane;
    const float lenSq = n[0] * n[0] + n[1] * n[1] + n[2] * n[2];
    CHECK(lenSq > 0.999f && lenSq < 1.001f);
    CHECK(std::abs(n[0] * v[0] + n[1] * v[1] + n[2] * v[2] + n[3]) < 1e-5f);
    CHECK(descs[0].verts == c.verts.data());
}

TEST(f10x_occluder_small_or_culled_parts_add_nothing) {
    const CubeMesh c = makeCube();
    OpenBeam ob;
    {
        // far away: the area factor (r / d)^2 * pi drops below 0.4
        const float eye[3] = {0.0f, 0.0f, -1000.0f};
        OccFilter f = filterAt(eye, ob.b);
        std::vector<OccDesc> d;
        std::vector<std::uint32_t> i;
        CHECK(addOccluder(c.mesh(), f, eye, d, i));
        CHECK(d.empty() && i.empty());
    }
    {
        // outside the frustum's bounding sphere
        const float eye[3] = {0.0f, 0.0f, -10.0f};
        OccFilter f = filterAt(eye, ob.b);
        f.sphereRadius = 1.0f;
        f.sphereCenter[0] = 100.0f;
        std::vector<OccDesc> d;
        std::vector<std::uint32_t> i;
        CHECK(addOccluder(c.mesh(), f, eye, d, i));
        CHECK(d.empty());
    }
    {
        // culled by the beam (everything behind z = -100 ... the plane flipped: only z <= -100 is kept)
        NBeam b;
        b.reset();
        const float p[4] = {0, 0, -1, -100};
        b.addPlaneAtRoot(p);
        const float eye[3] = {0.0f, 0.0f, -10.0f};
        OccFilter f = filterAt(eye, b);
        std::vector<OccDesc> d;
        std::vector<std::uint32_t> i;
        CHECK(addOccluder(c.mesh(), f, eye, d, i));
        CHECK(d.empty());
    }
}

TEST(f10x_occluder_corner_view_silhouette_is_a_hexagon) {
    const CubeMesh c = makeCube();
    OpenBeam ob;
    const float eye[3] = {-2.0f, -2.0f, -2.0f};
    OccFilter f = filterAt(eye, ob.b);
    std::vector<OccDesc> descs;
    std::vector<std::uint32_t> idx;
    CHECK(addOccluder(c.mesh(), f, eye, descs, idx));
    CHECK(descs.size() == 1);
    if (descs.size() != 1) return;
    CHECK(descs[0].count == 6);
    const std::set<std::uint32_t> got(idx.begin(), idx.end());
    CHECK(got.size() == 6 && !got.count(0) && !got.count(7));  // the near and the far corner are not on the silhouette
}

TEST(f10x_occluder_add_order_is_the_engines_half_loop) {
    std::vector<std::uint32_t> o;
    occluderAddOrder(0, o);
    CHECK(o.empty());
    occluderAddOrder(1, o);
    CHECK(o == (std::vector<std::uint32_t>{0}));
    occluderAddOrder(2, o);
    CHECK(o == (std::vector<std::uint32_t>{1}));
    occluderAddOrder(3, o);
    CHECK(o == (std::vector<std::uint32_t>{2, 1}));
    occluderAddOrder(4, o);
    CHECK(o == (std::vector<std::uint32_t>{3, 2}));
    occluderAddOrder(5, o);
    CHECK(o == (std::vector<std::uint32_t>{4, 3, 2}));
}

TEST(f10x_occluder_area_factor_clamps) {
    OccFilter f;
    f.viewDir[2] = 1.0f;
    const float near[4] = {10.0f, 0.0f, 0.0f, 1.0f};  // r 10 at distance 1: clamped to 1
    CHECK(occAreaFactor(f, near) == 1.0f);
    const float behind[4] = {1.0f, 0.0f, 0.0f, -5.0f};  // behind the camera: the epsilon floor, then the clamp
    CHECK(occAreaFactor(f, behind) == 1.0f);
    const float far[4] = {1.0f, 0.0f, 0.0f, 10.0f};  // (1 / 10)^2 * pi
    const float a = occAreaFactor(f, far);
    CHECK(a > 0.0314f && a < 0.0315f);
}
