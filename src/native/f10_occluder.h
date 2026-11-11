#pragma once

// F10-2 (docs/research/frame-ownership.md 14.33): eCOccluder_PS::AddOccluder 0x675270 written down as pure code (disassembled this stage; no engine calls).
// Per part of the occluder mesh (0x20 bytes: bounding sphere {r, centre}, face first / count, edge first / count) that passes the area factor (>= 0.4,
// eCFrustumFilter::CalculateAreaFactor 0x67e9a0), the frustum's bounding sphere and the beam tree's sphere cull: every face is classified against the eye
// (back = (vertex - eye) . normal < 0), then every unvisited silhouette edge (its two faces differ) starts a loop that walks the silhouette around the part:
// in the current face the next edge after the current one; a silhouette edge appends a vertex (v0 when its face 0 is back-facing, else v1) and closes the
// loop when its other vertex is the loop's first; a non-silhouette edge moves to the face across it. The faces met on the way contribute normal x weight
// (+0x1c, +0x28) once each. A closed loop of >= 3 vertices becomes a desc {vertex array, normalized normal, d through the vertex farthest along the view
// plane, that distance, index range}; shorter ones are dropped. ExtractFromFrustum sorts the descs by the distance (descending, the engine's qsort) and
// adds ceil(n / 2) of them, the last ones first (the engine's loop pops while it counts up), to the final beam (eCBeamTree::AddPoly 0x67d120).
//
// Engine layout (eCOccluder_PS): +0x60 vertices (12 bytes), +0x78 faces (0x2c: +4 a vertex index, +0x10 three edge indices, +0x1c normal, +0x28 weight),
// +0x80 face count, +0x90 edges (0x14: +0 v0, +4 v1, +8 face 0, +0xc face 1), +0x98 edge count, +0xa8 parts, +0xb0 part count.

#include <cmath>
#include <cstdint>
#include <cstring>
#include <vector>

#include "native/f10_math.h"

namespace rvk::native::f10 {

// eCOccluder_PS::eSOccluderDesc (0x28 bytes, the engine's layout: the compare reads the engine's array with it)
struct OccDesc {
    const float* verts = nullptr;  // +0
    float plane[4]{};              // +8: normal, d
    float dist = 0.0f;             // +0x18
    std::uint32_t first = 0;       // +0x1c: into the index array
    std::uint32_t count = 0;       // +0x20
    std::uint32_t pad = 0;
};
static_assert(sizeof(OccDesc) == 0x28);

struct OccMesh {
    const float* verts = nullptr;
    const std::uint8_t* faces = nullptr;  // 0x2c each
    std::int32_t faceCount = 0;
    const std::uint8_t* edges = nullptr;  // 0x14 each
    std::int32_t edgeCount = 0;
    const std::uint8_t* parts = nullptr;  // 0x20 each
    std::int32_t partCount = 0;
};

// what AddOccluder reads of the eCFrustumFilter (db +0x28)
struct OccFilter {
    float camera[3]{};     // filter +0x10 -> camera +0x90 (CalculateAreaFactor)
    float viewDir[3]{};    // filter +0x38
    float sphereRadius = 0.0f;  // +0x44
    float sphereCenter[3]{};    // +0x48
    float viewPlane[4]{};       // +0x6c (= db +0x94)
    Beam beam;                  // +0xf0
    float epsilon = 1e-6f;      // SharedBase g_fEpsilon
    float pi = 3.14159265f;     // SharedBase g_fPI
    float minArea = 0.4f;       // DAT_1809f1620
    float one = 1.0f;           // the 1.0 of CalculateAreaFactor's clamp and of bCVector::Normalize
};

namespace occ_detail {
template <class T>
inline T at(const std::uint8_t* p, std::size_t off) {
    T v;
    std::memcpy(&v, p + off, sizeof(T));
    return v;
}
inline bool bit(const std::vector<std::uint8_t>& b, std::int32_t i) { return i >= 0 && static_cast<std::size_t>(i) < b.size() && b[static_cast<std::size_t>(i)] != 0; }
inline void setBit(std::vector<std::uint8_t>& b, std::int32_t i) {
    if (i >= 0 && static_cast<std::size_t>(i) < b.size()) b[static_cast<std::size_t>(i)] = 1;
}
}  // namespace occ_detail

// eCFrustumFilter::CalculateAreaFactor 0x67e9a0 (sphere = {r, centre})
inline float occAreaFactor(const OccFilter& f, const float* sphere) {
    const float v[3] = {sphere[1] - f.camera[0], sphere[2] - f.camera[1], sphere[3] - f.camera[2]};
    const float dot = (v[1] * f.viewDir[1] + v[0] * f.viewDir[0]) + v[2] * f.viewDir[2];
    const float den = f.epsilon > dot ? f.epsilon : dot;  // maxss(eps, dot)
    const float q = sphere[0] / den;
    const float x = (q * f.pi) * q;
    const float m = f.one < x ? f.one : x;  // minss(1, x)
    return 0.0f > m ? 0.0f : m;
}

// the result of one occluder; false = the walk did not terminate within a sane bound (a broken mesh: the caller falls back to the engine)
inline bool addOccluder(const OccMesh& m, const OccFilter& f, const float* eye, std::vector<OccDesc>& descs, std::vector<std::uint32_t>& idx) {
    using namespace occ_detail;
    std::vector<std::uint8_t> back, visited, faceSeen;
    for (std::int32_t pi = 0; pi < m.partCount; ++pi) {
        const std::uint8_t* part = m.parts + 0x20 * static_cast<std::size_t>(pi);
        const float sph[4] = {at<float>(part, 0), at<float>(part, 4), at<float>(part, 8), at<float>(part, 0xc)};
        if (f.minArea > occAreaFactor(f, sph)) continue;
        {
            const float v[3] = {sph[1] - f.sphereCenter[0], sph[2] - f.sphereCenter[1], sph[3] - f.sphereCenter[2]};
            const float len2 = (v[0] * v[0] + v[1] * v[1]) + v[2] * v[2];
            const float rr = (f.sphereRadius + sph[0]) * (f.sphereRadius + sph[0]);
            if (!(rr >= len2)) continue;
        }
        if (beamCullSphere(f.beam, sph[0], sph + 1) == kCulled) continue;
        visited.assign(static_cast<std::size_t>(std::max(m.edgeCount, 0)), 0);
        faceSeen.assign(static_cast<std::size_t>(std::max(m.faceCount, 0)), 0);
        back.assign(static_cast<std::size_t>(std::max(m.faceCount, 0)), 0);
        const auto faceFirst = at<std::uint32_t>(part, 0x10), faceN = at<std::uint32_t>(part, 0x14);
        for (std::uint32_t fi = faceFirst; fi < faceFirst + faceN; ++fi) {
            const std::uint8_t* face = m.faces + 0x2c * static_cast<std::size_t>(fi);
            const float* vtx = m.verts + 3 * static_cast<std::size_t>(at<std::int32_t>(face, 4));
            const float d[3] = {vtx[0] - eye[0], vtx[1] - eye[1], vtx[2] - eye[2]};
            const float dot = (d[1] * at<float>(face, 0x20) + d[0] * at<float>(face, 0x1c)) + d[2] * at<float>(face, 0x24);
            if (0.0f > dot) setBit(back, static_cast<std::int32_t>(fi));
        }
        auto loopStart = static_cast<std::uint32_t>(idx.size());
        float acc[3] = {0.0f, 0.0f, 0.0f};
        const auto edgeFirst = at<std::uint32_t>(part, 0x18), edgeN = at<std::uint32_t>(part, 0x1c);
        for (std::uint32_t e = edgeFirst; e < edgeFirst + edgeN; ++e) {
            if (bit(visited, static_cast<std::int32_t>(e))) continue;
            const std::uint8_t* edge = m.edges + 0x14 * static_cast<std::size_t>(e);
            const bool b0 = bit(back, at<std::int32_t>(edge, 8)), b1 = bit(back, at<std::int32_t>(edge, 0xc));
            if (b0 == b1) continue;
            setBit(visited, static_cast<std::int32_t>(e));
            std::int32_t face = b0 ? at<std::int32_t>(edge, 8) : at<std::int32_t>(edge, 0xc);
            idx.push_back(b0 ? at<std::uint32_t>(edge, 0) : at<std::uint32_t>(edge, 4));
            std::int32_t cur = static_cast<std::int32_t>(e);
            bool closed = false;
            for (int guard = 0; !closed; ++guard) {
                if (guard > 1 << 20 || face < 0 || face >= m.faceCount) return false;
                const std::uint8_t* fp = m.faces + 0x2c * static_cast<std::size_t>(face);
                if (!bit(faceSeen, face)) {
                    const float w = at<float>(fp, 0x28);
                    acc[0] += at<float>(fp, 0x1c) * w;
                    acc[1] += at<float>(fp, 0x20) * w;
                    acc[2] += at<float>(fp, 0x24) * w;
                    setBit(faceSeen, face);
                }
                int k = 0;
                while (k < 2 && at<std::int32_t>(fp, 0x10 + 4 * static_cast<std::size_t>(k)) != cur) ++k;
                ++k;
                for (;;) {
                    if (++guard > 1 << 20) return false;
                    const std::int32_t e2 = at<std::int32_t>(fp, 0x10 + 4 * static_cast<std::size_t>(k % 3));
                    if (e2 < 0 || e2 >= m.edgeCount) return false;
                    const std::uint8_t* ep = m.edges + 0x14 * static_cast<std::size_t>(e2);
                    const bool c0 = bit(back, at<std::int32_t>(ep, 8)), c1 = bit(back, at<std::int32_t>(ep, 0xc));
                    if (c0 != c1) {
                        setBit(visited, e2);
                        std::uint32_t other;
                        if (c0) {
                            idx.push_back(at<std::uint32_t>(ep, 0));
                            other = at<std::uint32_t>(ep, 4);
                        } else {
                            idx.push_back(at<std::uint32_t>(ep, 4));
                            other = at<std::uint32_t>(ep, 0);
                        }
                        if (idx[loopStart] == other) {
                            closed = true;
                            break;
                        }
                        ++k;
                        continue;
                    }
                    face = at<std::int32_t>(ep, 8) == face ? at<std::int32_t>(ep, 0xc) : at<std::int32_t>(ep, 8);
                    cur = e2;
                    break;
                }
            }
            const auto n = static_cast<std::uint32_t>(idx.size()) - loopStart;
            if (static_cast<std::int32_t>(n) <= 2) {
                idx.resize(loopStart);
            } else {
                OccDesc d;
                d.verts = m.verts;
                d.first = loopStart;
                d.count = n;
                d.dist = 0.0f;
                std::uint32_t farIdx = 0;
                for (std::size_t i = loopStart; i < idx.size(); ++i) {
                    const float* v = m.verts + 3 * static_cast<std::size_t>(idx[i]);
                    const float pd = ((f.viewPlane[1] * v[1] + f.viewPlane[0] * v[0]) + f.viewPlane[2] * v[2]) + f.viewPlane[3];
                    if (pd > d.dist) {
                        d.dist = pd;
                        farIdx = idx[i];
                    }
                }
                // bCVector::Normalize (SharedBase): sqrt of the squared length, a reciprocal multiply when > 0
                const float len = std::sqrt((acc[0] * acc[0] + acc[1] * acc[1]) + acc[2] * acc[2]);
                if (0.0f < len) {
                    const float s = f.one / len;
                    acc[0] *= s;
                    acc[1] *= s;
                    acc[2] *= s;
                }
                d.plane[0] = acc[0];
                d.plane[1] = acc[1];
                d.plane[2] = acc[2];
                const float* v = m.verts + 3 * static_cast<std::size_t>(farIdx);
                d.plane[3] = ((-(acc[0] * v[0])) - acc[1] * v[1]) - acc[2] * v[2];
                descs.push_back(d);
                loopStart = static_cast<std::uint32_t>(idx.size());
            }
            acc[0] = acc[1] = acc[2] = 0.0f;
        }
    }
    return true;
}

// which descs ExtractFromFrustum adds, in its order (after the sort): the last ceil(n / 2), the last one first
inline void occluderAddOrder(std::uint32_t n, std::vector<std::uint32_t>& out) {
    out.clear();
    std::uint32_t remaining = n;
    for (std::uint32_t i = 0; i < remaining;) {
        --remaining;
        out.push_back(remaining);
        ++i;
    }
}

}  // namespace rvk::native::f10
