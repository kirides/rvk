// F5-1 cluster build (src/core/cluster_build.*, docs/research/frame-ownership.md 7.33): clusters tile the triangle list, the reordered list holds exactly the engine's ordered
// index triples (no rotation, no loss), the content hash and the disk cache.

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <vector>

#include "core/cluster_build.h"
#include "test.h"

using namespace rvk::clusters;

namespace {
struct Mesh {
    std::vector<float> pos;
    std::vector<std::uint32_t> idx;
};
// an n x n grid of quads in the xz plane with a height field (so the clusters have a real extent), two triangles per quad
Mesh grid(int n, bool rotate = false) {
    Mesh m;
    for (int z = 0; z <= n; ++z) {
        for (int x = 0; x <= n; ++x) {
            m.pos.push_back(static_cast<float>(x));
            m.pos.push_back(2.0f * std::sin(0.3f * static_cast<float>(x)) * std::cos(0.2f * static_cast<float>(z)));
            m.pos.push_back(static_cast<float>(z));
        }
    }
    for (int z = 0; z < n; ++z) {
        for (int x = 0; x < n; ++x) {
            const std::uint32_t a = static_cast<std::uint32_t>(z * (n + 1) + x), b = a + 1, c = a + static_cast<std::uint32_t>(n + 1), d = c + 1;
            if (rotate && ((x + z) & 1)) m.idx.insert(m.idx.end(), {b, a, c, c, d, b});  // rotated: b a c = (a c b)'s rotation cycle? (a, c, b) -> as (b, a, c)
            else m.idx.insert(m.idx.end(), {a, c, b, b, c, d});
        }
    }
    return m;
}
std::vector<std::array<std::uint32_t, 3>> triples(const std::vector<std::uint32_t>& idx) {
    std::vector<std::array<std::uint32_t, 3>> t;
    for (std::size_t i = 0; i + 2 < idx.size(); i += 3) t.push_back({idx[i], idx[i + 1], idx[i + 2]});
    std::sort(t.begin(), t.end());
    return t;
}
}  // namespace

TEST(clusters_tile_the_list_and_keep_every_ordered_triple) {
    for (const bool rotate : {false, true}) {
        const Mesh m = grid(60, rotate);
        Result r;
        CHECK(build(m.idx, m.pos, r));
        std::string why;
        CHECK(verify(m.idx, m.pos, r, &why));
        const std::vector<std::uint32_t> out = reorder(m.idx, r);
        CHECK(out.size() == m.idx.size());
        // the multiset of ORDERED triples is the original's: no triangle lost, none rotated
        CHECK(triples(out) == triples(m.idx));
        CHECK(tripleMultiset(out.data(), out.size() / 3) == tripleMultiset(m.idx.data(), m.idx.size() / 3));
        std::uint32_t tris = 0;
        for (const Cluster& c : r.clusters) {
            CHECK(c.tris >= 1 && c.tris <= kMaxClusterTriangles);
            tris += c.tris;
        }
        CHECK(tris == m.idx.size() / 3);
        CHECK(r.clusters.size() >= (m.idx.size() / 3) / kMaxClusterTriangles);
        // spatial: a cluster of a 7,200-triangle plane is far smaller than the plane
        float maxR = 0.0f;
        for (const Cluster& c : r.clusters) maxR = std::max(maxR, c.r);
        CHECK(maxR < 20.0f);
    }
}

TEST(clusters_handle_duplicate_and_degenerate_triangles) {
    Mesh m = grid(24);
    // the same triangle three times, a degenerate (a, a, b) and a rotated copy of an existing one
    const std::vector<std::uint32_t> extra = {0, 25, 1, 0, 25, 1, 0, 25, 1, 5, 5, 6, 25, 1, 0};
    m.idx.insert(m.idx.end(), extra.begin(), extra.end());
    Result r;
    CHECK(build(m.idx, m.pos, r));
    CHECK(verify(m.idx, m.pos, r));
    CHECK(triples(reorder(m.idx, r)) == triples(m.idx));
}

TEST(clusters_reject_unusable_input) {
    Result r;
    Mesh m = grid(8);
    CHECK(!build({}, m.pos, r));
    CHECK(!build({0, 1}, m.pos, r));                      // not a triangle list
    CHECK(!build({0, 1, 100000}, m.pos, r));              // a vertex that does not exist
    CHECK(!build(m.idx, {0.0f, 0.0f, 0.0f}, r));          // fewer than 3 vertices
}

TEST(clusters_verify_catches_a_broken_result) {
    const Mesh m = grid(30);
    Result r;
    CHECK(build(m.idx, m.pos, r));
    CHECK(verify(m.idx, m.pos, r));
    Result a = r;
    a.perm[3] = a.perm[4];  // not a permutation
    CHECK(!verify(m.idx, m.pos, a));
    Result b = r;
    b.clusters[1].firstTri += 1;  // a hole
    CHECK(!verify(m.idx, m.pos, b));
    Result c = r;
    c.clusters[0].r = 0.001f;  // a sphere that does not hold its vertices
    CHECK(!verify(m.idx, m.pos, c));
    Result d = r;
    d.clusters.pop_back();  // does not cover the list
    CHECK(!verify(m.idx, m.pos, d));
}

TEST(clusters_content_hash_follows_the_content) {
    Mesh m = grid(20);
    const std::uint64_t h = contentHash(m.idx, m.pos);
    CHECK(h == contentHash(m.idx, m.pos));
    Mesh a = m;
    std::swap(a.idx[3], a.idx[4]);  // a rotated triangle is another mesh
    CHECK(contentHash(a.idx, a.pos) != h);
    Mesh b = m;
    b.pos[10] += 0.5f;
    CHECK(contentHash(b.idx, b.pos) != h);
    CHECK(hash64("abc", 3, 1) != hash64("abc", 3, 2));
    CHECK(hash64("abcdefghij", 10, 0) != hash64("abcdefghik", 10, 0));
}

TEST(clusters_disk_cache_round_trips_and_refuses_the_wrong_file) {
    namespace fs = std::filesystem;
    const fs::path dir = fs::temp_directory_path() / "rvk_cluster_test";
    std::error_code ec;
    fs::remove_all(dir, ec);
    const Mesh m = grid(40);
    Result r;
    CHECK(build(m.idx, m.pos, r));
    const std::uint64_t h = contentHash(m.idx, m.pos);
    const fs::path f = dir / "a.cl";
    CHECK(save(f, h, r));
    Result l;
    CHECK(load(f, h, m.idx, m.pos, l));
    CHECK(l.perm == r.perm && l.clusters.size() == r.clusters.size());
    CHECK(std::memcmp(l.clusters.data(), r.clusters.data(), r.clusters.size() * sizeof(Cluster)) == 0);
    CHECK(!load(f, h + 1, m.idx, m.pos, l));  // another hash
    Mesh other = grid(41);
    CHECK(!load(f, h, other.idx, other.pos, l));  // another mesh under the same name
    // a truncated file and a flipped byte
    {
        const auto size = fs::file_size(f);
        fs::resize_file(f, size - 5);
        CHECK(!load(f, h, m.idx, m.pos, l));
    }
    CHECK(save(f, h, r));
    {
        std::fstream s(f, std::ios::in | std::ios::out | std::ios::binary);
        s.seekp(30);
        const char junk = static_cast<char>(0xff);
        s.write(&junk, 1);  // inside the permutation: no longer a permutation (or the same, by chance: reload verifies either way)
    }
    Result bad;
    const bool ok = load(f, h, m.idx, m.pos, bad);
    CHECK(!ok || verify(m.idx, m.pos, bad));
    fs::remove_all(dir, ec);
}
