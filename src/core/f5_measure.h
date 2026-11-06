#pragma once

// F5-0 measurements (docs/research/frame-ownership.md 7.22, `RVK_F5_MEASURE=1` through the Win32 environment, read once: off = one bool test per draw).
// What a GPU-driven cull / cluster stage (F5) could save: triangles submitted per view kind and mesh-size class, the share of large static meshes' triangles
// in ~128-triangle clusters outside the frustum / back-facing (frustum planes + normal cones, on the CPU, from the buffers' CPU copies), the number of
// visible-cluster runs per draw, a triangle screen-size histogram of the main view's z pass (exact, per triangle, sampled frames) and the mesh memory of
// "every LOD resident". No behaviour change; written by the engine thread only. Pure geometry here (tests/f5_tests.cpp), the Device side in d3d9/f5_note.cpp.

#include <array>
#include <cstdint>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace rvk::f5 {

bool enabled();

// ---- geometry -------------------------------------------------------------------------------------------------------------------

struct Mesh {
    std::vector<float> pos;            // xyz per vertex
    std::vector<std::uint32_t> idx;    // triangle list, vertex ids into pos
    std::size_t vertices() const { return pos.size() / 3; }
    std::size_t triangles() const { return idx.size() / 3; }
};

struct Cluster {
    float c[3] = {}, r = 0.0f;       // bounding sphere (object space)
    float axis[3] = {}, cutoff = 1.0f;  // normal cone (meshopt: back-facing when dot(c - eye, axis) >= cutoff * |c - eye| + r)
    std::uint32_t firstTri = 0, tris = 0;  // into ClusterSet::tri (3 vertex ids per triangle, clusters contiguous)
};

enum Variant : int { kMeshopt = 0, kSpatial = 1, kVariants = 2 };

struct ClusterSet {
    std::vector<Cluster> list;                  // in the builder's order
    std::vector<std::uint32_t> mortonOrder;     // cluster ids sorted along a Morton curve of the centres (the BVH-like order)
    std::vector<std::uint32_t> tri;             // 3 vertex ids per triangle, ordered by cluster
};

// ~128-triangle clusters (max 128 vertices): kMeshopt = meshopt_buildMeshlets (cone weight 0.5), kSpatial = meshopt_buildMeshletsSpatial
ClusterSet buildClusters(const Mesh& mesh, Variant v);

// One draw seen by one view: the object-to-clip rows, the frustum planes in object space (world-metric, inside = dot >= 0) and the eye in object space.
struct ObjView {
    bool valid = false;
    float clip[4][4] = {};            // clip_i = dot(clip[i], (p, 1)), D3D z in [0, w]
    float planes[6][4] = {};          // object space, normal scaled by the world->object scale (see sphereOutside)
    float normalLen[6] = {};
    float eye[3] = {}, dir[3] = {};   // object space: the eye (perspective) or the view direction (ortho)
    bool ortho = false;
    bool mirrored = false;            // negative determinant of the world matrix
    int cull = 3;                     // D3DCULL: 1 none, 2 cw, 3 ccw
    float vpW = 1.0f, vpH = 1.0f;
};

// regs = VS c0..c3 (clip_i = dot(regs[i], (world, 1))), world = 3 rows of 4 (x_w = m[0] x + m[1] y + m[2] z + m[3])
bool makeObjView(const float regs[4][4], const float world[12], int cull, float vpW, float vpH, ObjView& out);

bool sphereOutside(const ObjView& v, const float c[3], float r);
// the cone's verdict for the draw's culling; sign selects the winding convention (+1 / -1)
bool coneBackFacing(const ObjView& v, const Cluster& cl, int sign);

struct ClusterEval {
    std::uint64_t tris = 0, triF = 0, triBackOnly[2] = {}, clusters = 0, clustersF = 0, clustersKept[2] = {};
    std::uint64_t runs[2][2] = {};      // [sign][order: 0 builder, 1 morton]: runs of visible clusters
    std::uint64_t keptTris[2] = {};     // triangles in visible clusters (frustum + cone)
};
// signs: index 0 = +1, 1 = -1 (both evaluated; the caller picks by the exact comparison)
void evalClusters(const ClusterSet& set, const ObjView& v, ClusterEval& out);

// ---- exact per-triangle classification (the hardware's view of a draw) ---------------------------------------------------------

inline constexpr int kHistBins = 5;  // projected area in pixels: < 1, 1-4, 4-16, 16-64, > 64
struct TriExact {
    std::uint64_t tris = 0, frustumOut = 0, backface = 0, nearClipped = 0, visible = 0;
    std::uint64_t hist[kHistBins] = {};  // among triangles that are inside the frustum and front-facing (near-clipped ones count in the last bin)
    // cone validation per sign: a cluster called back-facing by the cone test that holds a front-facing, fully inside triangle
    std::uint64_t coneBackClusters[2] = {}, coneViolations[2] = {};
};
// per-triangle clip classification of `tris` (3 vertex ids each) of `mesh`; clusters (optional) get the cone verdict validated
void evalExact(const Mesh& mesh, const std::uint32_t* tris, std::size_t triCount, const ObjView& v, TriExact& out, const ClusterSet* set);

// ---- counters -------------------------------------------------------------------------------------------------------------------

inline constexpr int kViewKeys = 14;  // 0 main z, 1 main solid, 2 main alpha (refracted / alpha), 3 sky cube, 4 other, 5.. shadow views 0..8
inline constexpr int kClasses = 5;    // triangles per draw: < 256, 256-1K, 1K-4K, 4K-16K, > 16K
inline constexpr int kKinds = 20;

struct ViewAgg {
    std::uint64_t frames = 0, draws = 0, tris = 0, indexCount = 0, vertexRange = 0, instancedTris = 0;
    std::uint64_t classDraws[kClasses] = {}, classTris[kClasses] = {};
    std::uint64_t kindTris[kKinds] = {}, kindDraws[kKinds] = {};
    std::uint32_t vpW = 0, vpH = 0;
    // clusters of large static meshes (>= 1K triangles per draw, rigid, indexed): the CPU estimate
    std::uint64_t cDraws = 0, cDrawsAllOut[kVariants][2] = {};
    ClusterEval cev[kVariants];
    std::uint64_t cDrawsWithVisible[kVariants][2] = {};
    std::uint64_t mirrored = 0;
};

struct Counters {
    std::uint64_t curFrame = ~0ull;
    std::uint32_t frameKeyMask = 0;
    ViewAgg view[kViewKeys];
    // exact classification, main z pass, sampled frames
    std::uint64_t sampledFrames = 0;
    TriExact exactAll, exactLarge, exactShadow;
    ClusterEval sampled[kVariants], sampledShadow[kVariants];  // the cluster estimate of exactly the draws the exact classes covered (same frames)
    std::uint64_t shadowSeq = 0, shadowMarkFrame = ~0ull;
    std::uint64_t exactDraws = 0, exactBad = 0, exactMeshBytes = 0;
    // cluster builds
    std::uint64_t builds = 0, buildNs = 0, buildTris = 0, buildSkipped = 0;
    double vpMatchMax = 0.0;
    std::uint64_t vpMatchChecks = 0, vpMatchBad = 0;
    // LOD memory (main z / solid contexts of the frame)
    struct LodSnap {
        std::uint8_t n = 0;
        std::uint32_t size[4] = {};
        std::uint8_t loaded[4] = {};
        std::int8_t chosen = -1;
    };
    std::unordered_map<const void*, LodSnap> frameLods, lastLods;
    std::unordered_set<const void*> frameCtx;                    // contexts seen this frame
    std::unordered_map<std::uint64_t, std::uint32_t> frameBuffers;  // distinct vertex / index buffers of the main view's static draws: id -> bytes
    std::uint64_t lodFrames = 0, lodPs = 0, lodPsMulti = 0;
    double ratioSum[5][4] = {};  // [n][b]: size_b / size_0 over meshes with both loaded
    std::uint64_t ratioCnt[5][4] = {};
    std::uint64_t chosenBytesSum = 0, loadedBytesSum = 0, estAllBytesSum = 0, unloadedSiblings = 0, siblingsTotal = 0, drawnBufferBytesSum = 0;
    std::uint64_t lastReportNs = 0;
};

Counters& counters();
inline int sizeClass(std::uint32_t prims) { return prims < 256 ? 0 : prims < 1024 ? 1 : prims < 4096 ? 2 : prims < 16384 ? 3 : 4; }

void report();     // logs the window (rvk.log `native: f5 measure ...`) and resets the per-window sums (the caches stay)
void closeLodFrame();

}  // namespace rvk::f5
