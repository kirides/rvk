#pragma once

// F5-1 cluster build (docs/research/frame-ownership.md 7.33, setting native.clusters): the pure part. A draw's index range (a triangle list over local vertex ids)
// and its vertex positions become ~128-triangle clusters (meshoptimizer's buildMeshlets, <= 128 vertices): `perm[k]` = the original triangle placed at position k,
// so that every cluster is a contiguous run of triangles, and one bounding sphere per cluster (frustum culling only: a normal cone is worthless at this size,
// 7.22). The reordered list holds exactly the engine's ORIGINAL ordered index triples (no rotation: the rasterizer's depth of a triangle depends on its vertex
// order, and the solid pass draws with depth EQUAL against the z pre-pass): `verify` checks it. The result is cached on disk keyed by the content hash.
// Pure (no device, no engine): tests/cluster_tests.cpp.

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace rvk::clusters {

inline constexpr std::uint32_t kMaxClusterTriangles = 128;
inline constexpr std::uint32_t kMaxClusterVertices = 128;
inline constexpr std::uint32_t kBuilderVersion = 1;  // bumped when the output of build() changes (part of the content hash)

struct Cluster {
    float c[3] = {};          // bounding sphere centre, in the vertex positions' space
    float r = 0.0f;
    std::uint32_t firstTri = 0, tris = 0;  // into the reordered triangle list
};

struct Result {
    std::vector<std::uint32_t> perm;  // perm[k] = original triangle index at position k (a permutation of [0, triangles))
    std::vector<Cluster> clusters;    // contiguous, in order, covering [0, triangles)
};

// idx: 3 local vertex ids per triangle (< pos.size() / 3), pos: xyz per vertex. False: nothing usable (too small, a triangle meshopt did not return ...)
bool build(const std::vector<std::uint32_t>& idx, const std::vector<float>& pos, Result& out);

// the checks of a result against its input; false + `why`: not a permutation / clusters do not tile the list / a vertex of a cluster's triangles lies outside its sphere
bool verify(const std::vector<std::uint32_t>& idx, const std::vector<float>& pos, const Result& r, std::string* why = nullptr);

// the triangles of `idx` in the result's order (3 ids each)
std::vector<std::uint32_t> reorder(const std::vector<std::uint32_t>& idx, const Result& r);

// order-independent multiset digest of the ordered index triples of a triangle list (sum of a mix of each triple): equal for two lists holding the same triangles
std::uint64_t tripleMultiset(const std::uint32_t* idx, std::size_t triangles);

// the content hash (builder version + indices + positions)
std::uint64_t contentHash(const std::vector<std::uint32_t>& idx, const std::vector<float>& pos);
std::uint64_t hash64(const void* data, std::size_t bytes, std::uint64_t seed = 0);

// disk cache: one file per content hash. load validates the hash, the sizes and verify()
bool save(const std::filesystem::path& file, std::uint64_t hash, const Result& r);
bool load(const std::filesystem::path& file, std::uint64_t hash, const std::vector<std::uint32_t>& idx, const std::vector<float>& pos, Result& out);

}  // namespace rvk::clusters
