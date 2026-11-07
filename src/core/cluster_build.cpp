#include "core/cluster_build.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>

#include <meshoptimizer.h>

namespace rvk::clusters {

namespace {

inline std::uint64_t mix(std::uint64_t h) {
    h ^= h >> 33;
    h *= 0xff51afd7ed558ccdull;
    h ^= h >> 33;
    h *= 0xc4ceb9fe1a85ec53ull;
    h ^= h >> 33;
    return h;
}

struct Item {
    std::uint32_t k[3];
    std::uint32_t orig;
};
inline bool keyLess(const std::uint32_t* a, const std::uint32_t* b) {
    if (a[0] != b[0]) return a[0] < b[0];
    if (a[1] != b[1]) return a[1] < b[1];
    return a[2] < b[2];
}
inline void sorted3(std::uint32_t a, std::uint32_t b, std::uint32_t c, std::uint32_t* k) {
    if (a > b) std::swap(a, b);
    if (b > c) std::swap(b, c);
    if (a > b) std::swap(a, b);
    k[0] = a, k[1] = b, k[2] = c;
}

constexpr char kMagic[4] = {'R', 'C', 'L', '1'};
struct FileHeader {
    char magic[4];
    std::uint32_t version;
    std::uint64_t hash;
    std::uint32_t triangles, clusters;
};
struct FileCluster {
    float c[3], r;
    std::uint32_t firstTri, tris;
};
static_assert(sizeof(FileCluster) == 24 && sizeof(FileHeader) == 24);

}  // namespace

std::uint64_t hash64(const void* data, std::size_t bytes, std::uint64_t seed) {
    const auto* p = static_cast<const std::uint8_t*>(data);
    std::uint64_t h = mix(seed ^ (static_cast<std::uint64_t>(bytes) * 0x9E3779B97F4A7C15ull));
    std::size_t i = 0;
    for (; i + 8 <= bytes; i += 8) {
        std::uint64_t w;
        std::memcpy(&w, p + i, 8);
        h = (h ^ mix(w + i)) * 0x9E3779B97F4A7C15ull;
        h ^= h >> 29;
    }
    std::uint64_t tail = 0;
    if (i < bytes) std::memcpy(&tail, p + i, bytes - i);
    h = (h ^ mix(tail + 0x1234567ull)) * 0x9E3779B97F4A7C15ull;
    return mix(h);
}

std::uint64_t contentHash(const std::vector<std::uint32_t>& idx, const std::vector<float>& pos) {
    std::uint64_t h = hash64(&kBuilderVersion, sizeof(kBuilderVersion), 0x636c7573ull);
    h = hash64(idx.data(), idx.size() * sizeof(std::uint32_t), h);
    return hash64(pos.data(), pos.size() * sizeof(float), h);
}

std::uint64_t tripleMultiset(const std::uint32_t* idx, std::size_t triangles) {
    std::uint64_t sum = 0;
    for (std::size_t t = 0; t < triangles; ++t) {
        // (ordered triples: a rotated triangle is another element)
        sum += mix((static_cast<std::uint64_t>(idx[3 * t]) << 32 | idx[3 * t + 1]) * 0x9E3779B97F4A7C15ull ^ (static_cast<std::uint64_t>(idx[3 * t + 2]) + 0x7F4A7C15ull));
    }
    return sum;
}

bool build(const std::vector<std::uint32_t>& idx, const std::vector<float>& pos, Result& out) {
    out = {};
    const std::size_t indexCount = idx.size(), vertexCount = pos.size() / 3;
    if (indexCount < 3 || indexCount % 3 != 0 || vertexCount < 3) return false;
    for (const std::uint32_t v : idx) {
        if (v >= vertexCount) return false;
    }
    const std::size_t tris = indexCount / 3;
    const std::size_t bound = meshopt_buildMeshletsBound(indexCount, kMaxClusterVertices, kMaxClusterTriangles);
    std::vector<meshopt_Meshlet> meshlets(bound);
    std::vector<unsigned int> mv(bound * kMaxClusterVertices);
    std::vector<unsigned char> mt(bound * kMaxClusterTriangles * 3);
    const std::size_t count = meshopt_buildMeshlets(meshlets.data(), mv.data(), mt.data(), idx.data(), indexCount, pos.data(), vertexCount, 3 * sizeof(float), kMaxClusterVertices,
                                                    kMaxClusterTriangles, 0.5f);
    if (count == 0) return false;
    // the original triangles by their sorted vertex triple (the builder may rotate a triangle: the result must hold the original ordered triple)
    std::vector<Item> items(tris);
    for (std::size_t t = 0; t < tris; ++t) {
        sorted3(idx[3 * t], idx[3 * t + 1], idx[3 * t + 2], items[t].k);
        items[t].orig = static_cast<std::uint32_t>(t);
    }
    std::sort(items.begin(), items.end(), [](const Item& a, const Item& b) { return keyLess(a.k, b.k) || (!keyLess(b.k, a.k) && a.orig < b.orig); });
    std::vector<std::uint32_t> used(tris, 0);  // per group head: how many of the group are taken
    out.perm.reserve(tris);
    out.clusters.reserve(count);
    for (std::size_t i = 0; i < count; ++i) {
        const meshopt_Meshlet& m = meshlets[i];
        const meshopt_Bounds b = meshopt_computeMeshletBounds(&mv[m.vertex_offset], &mt[m.triangle_offset], m.triangle_count, pos.data(), vertexCount, 3 * sizeof(float));
        Cluster c;
        std::memcpy(c.c, b.center, sizeof(c.c));
        c.r = b.radius;
        c.firstTri = static_cast<std::uint32_t>(out.perm.size());
        c.tris = m.triangle_count;
        for (unsigned t = 0; t < m.triangle_count; ++t) {
            std::uint32_t key[3];
            const unsigned char* tr = &mt[m.triangle_offset + 3 * t];
            sorted3(mv[m.vertex_offset + tr[0]], mv[m.vertex_offset + tr[1]], mv[m.vertex_offset + tr[2]], key);
            // first item of the group (lower bound), then the next unused one of it
            std::size_t lo = 0, hi = items.size();
            while (lo < hi) {
                const std::size_t mid = (lo + hi) / 2;
                if (keyLess(items[mid].k, key)) lo = mid + 1;
                else hi = mid;
            }
            const std::size_t at = lo + used[lo];
            if (lo >= items.size() || at >= items.size() || std::memcmp(items[at].k, key, sizeof(key)) != 0) {
                out = {};
                return false;  // (a triangle the input does not hold)
            }
            ++used[lo];
            out.perm.push_back(items[at].orig);
        }
        out.clusters.push_back(c);
    }
    if (out.perm.size() != tris) {
        out = {};
        return false;
    }
    return true;
}

bool verify(const std::vector<std::uint32_t>& idx, const std::vector<float>& pos, const Result& r, std::string* why) {
    const auto fail = [&](const char* w) {
        if (why != nullptr) *why = w;
        return false;
    };
    const std::size_t tris = idx.size() / 3;
    if (r.perm.size() != tris || idx.size() % 3 != 0) return fail("triangle count");
    std::vector<std::uint8_t> seen(tris, 0);
    for (const std::uint32_t p : r.perm) {
        if (p >= tris || seen[p] != 0) return fail("not a permutation");
        seen[p] = 1;
    }
    std::uint32_t next = 0;
    const std::size_t vertexCount = pos.size() / 3;
    for (const Cluster& c : r.clusters) {
        if (c.firstTri != next || c.tris == 0 || c.tris > kMaxClusterTriangles) return fail("clusters do not tile the list");
        if (!std::isfinite(c.r) || c.r < 0.0f) return fail("bad radius");
        for (std::uint32_t t = c.firstTri; t < c.firstTri + c.tris; ++t) {
            for (int k = 0; k < 3; ++k) {
                const std::uint32_t v = idx[3 * static_cast<std::size_t>(r.perm[t]) + k];
                if (v >= vertexCount) return fail("vertex out of range");
                const float dx = pos[3 * v] - c.c[0], dy = pos[3 * v + 1] - c.c[1], dz = pos[3 * v + 2] - c.c[2];
                const float d = std::sqrt(dx * dx + dy * dy + dz * dz);
                if (!(d <= c.r * 1.0001f + 1e-4f * (1.0f + c.r))) return fail("a vertex lies outside its cluster's sphere");
            }
        }
        next += c.tris;
    }
    if (next != tris) return fail("clusters do not cover the list");
    return true;
}

std::vector<std::uint32_t> reorder(const std::vector<std::uint32_t>& idx, const Result& r) {
    std::vector<std::uint32_t> out(r.perm.size() * 3);
    for (std::size_t k = 0; k < r.perm.size(); ++k) std::memcpy(&out[3 * k], &idx[3 * static_cast<std::size_t>(r.perm[k])], 12);
    return out;
}

bool save(const std::filesystem::path& file, std::uint64_t hash, const Result& r) {
    std::error_code ec;
    std::filesystem::create_directories(file.parent_path(), ec);
    const std::filesystem::path tmp = file.string() + ".tmp";
    {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        if (!f) return false;
        FileHeader h{};
        std::memcpy(h.magic, kMagic, 4);
        h.version = kBuilderVersion;
        h.hash = hash;
        h.triangles = static_cast<std::uint32_t>(r.perm.size());
        h.clusters = static_cast<std::uint32_t>(r.clusters.size());
        f.write(reinterpret_cast<const char*>(&h), sizeof(h));
        f.write(reinterpret_cast<const char*>(r.perm.data()), static_cast<std::streamsize>(r.perm.size() * 4));
        for (const Cluster& c : r.clusters) {
            FileCluster fc{{c.c[0], c.c[1], c.c[2]}, c.r, c.firstTri, c.tris};
            f.write(reinterpret_cast<const char*>(&fc), sizeof(fc));
        }
        if (!f) return false;
    }
    std::filesystem::rename(tmp, file, ec);
    return !ec;
}

bool load(const std::filesystem::path& file, std::uint64_t hash, const std::vector<std::uint32_t>& idx, const std::vector<float>& pos, Result& out) {
    out = {};
    std::ifstream f(file, std::ios::binary | std::ios::ate);
    if (!f) return false;
    const std::streamsize size = f.tellg();
    f.seekg(0);
    FileHeader h{};
    if (size < static_cast<std::streamsize>(sizeof(h))) return false;
    f.read(reinterpret_cast<char*>(&h), sizeof(h));
    if (std::memcmp(h.magic, kMagic, 4) != 0 || h.version != kBuilderVersion || h.hash != hash || h.triangles != idx.size() / 3) return false;
    const std::uint64_t want = sizeof(h) + static_cast<std::uint64_t>(h.triangles) * 4 + static_cast<std::uint64_t>(h.clusters) * sizeof(FileCluster);
    if (static_cast<std::uint64_t>(size) != want) return false;
    out.perm.resize(h.triangles);
    f.read(reinterpret_cast<char*>(out.perm.data()), static_cast<std::streamsize>(h.triangles) * 4);
    out.clusters.resize(h.clusters);
    for (Cluster& c : out.clusters) {
        FileCluster fc;
        f.read(reinterpret_cast<char*>(&fc), sizeof(fc));
        std::memcpy(c.c, fc.c, sizeof(c.c));
        c.r = fc.r;
        c.firstTri = fc.firstTri;
        c.tris = fc.tris;
    }
    if (!f || !verify(idx, pos, out)) {
        out = {};
        return false;
    }
    return true;
}

}  // namespace rvk::clusters
