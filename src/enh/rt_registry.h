#pragma once

// Bookkeeping of the ray tracing scene (enh/rt_scene, docs/research/rt-reflections.md 3), pure logic (unit tests:
// tests/rt_tests.cpp): BLAS keys (one BLAS per engine draw range of a static VB / IB generation), instance keys (the BLAS +
// the quantised world rows), the persistent instance registry (instances stay while unseen for a while, a moved object
// replaces its old instance at once, far / stale ones age out), the per-frame build budget, the TLAS selection under a cap.
//
// The design follows GD3D11's D3D12 RT scene (E:\git\github\GD3D11\D3D11Engine\D3D12Engine\D3D12RayTracing.cpp, the
// user's own code): BLAS cache by source buffers, 48 builds / 400k triangles per frame (the first always goes through),
// a memory budget with LRU eviction back to 85 %, a TLAS rebuilt every frame from a persistent instance list.

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <unordered_map>
#include <vector>

namespace rvk::enh::rt {

// one BLAS = one indexed draw range of the engine (triangle list) on the current generation of its static buffers
struct BlasKey {
    std::uint64_t vb = 0, ib = 0;          // d3d9 resource ids
    std::uint32_t vbGen = 0, ibGen = 0;    // their generations (a new GPU copy = a new key)
    std::uint32_t vertexOffset = 0;        // stream 0 offset (bytes)
    std::uint32_t stride = 0;
    std::uint32_t first = 0;               // start index
    std::uint32_t primitives = 0;          // triangles
    std::int32_t baseVertex = 0;
    std::uint32_t index32 = 0;
    bool operator==(const BlasKey&) const = default;
};
std::uint64_t hash(const BlasKey& k);
struct BlasKeyHash {
    std::size_t operator()(const BlasKey& k) const { return static_cast<std::size_t>(hash(k)); }
};

// 3x4 row-major object-to-world rows = VkTransformMatrixKHR::matrix (the material VS's g_vInstanceData rows as they are)
using Rows = std::array<float, 12>;
bool finite(const Rows& r);

// an instance's identity: its BLAS and the transform, quantised (translation to 1/16 unit, the 3x3 part to 1/4096), so the
// same object seen in the depth pre-pass, the solid pass and a shadow cascade is one instance
std::uint64_t instanceKey(std::uint64_t blasId, const Rows& rows);
// the engine object part a plain draw's instance belongs to: entity + the render op's sub-mesh index / hierarchy node
// (draws::Tag::opDesc), not the sub-mesh object, so a level-of-detail switch (another mesh resource, the same part) replaces
// the old instance at once; 0 = none (instanced batches, untagged draws)
std::uint64_t ownerKey(const void* entity, std::uint32_t part);

struct Aabb {
    float min[3]{0.0f, 0.0f, 0.0f}, max[3]{-1.0f, -1.0f, -1.0f};
    bool valid() const { return min[0] <= max[0] && min[1] <= max[1] && min[2] <= max[2]; }
    void add(const float p[3]);
};
struct Sphere {
    float c[3]{};
    float r = 0.0f;
};
// world-space bounding sphere of an object-space box under `rows` (the box's corners transformed)
Sphere worldSphere(const Aabb& box, const Rows& rows);

struct Limits {
    std::uint32_t maxInstances = 16384;        // TLAS capacity (instances = geometry records: one geometry per BLAS)
    std::uint32_t maxRegistered = 32768;       // registry cap: beyond it the longest unseen instances go first
    std::uint32_t buildsPerFrame = 48;         // BLAS builds per frame ...
    std::uint64_t trianglesPerFrame = 400000;  // ... and their triangles (the first build of a frame always goes through)
    std::uint64_t memoryBudget = 512ull << 20; // BLAS storage; over it unused BLASes are evicted (LRU) back to 85 %
    std::uint32_t unseenFrames = 600;          // an instance unseen for this many frames AND ...
    double unseenSeconds = 20.0;               // ... this many seconds is dropped
    std::uint32_t blasUnusedFrames = 3600;     // a BLAS without instances for this many frames AND ...
    double blasUnusedSeconds = 60.0;           // ... seconds is dropped (even under the budget)
    float radius = 30000.0f;                   // instances whose bounds lie farther from the camera are dropped (world units)
};

// the build budget of one frame
class BuildBudget {
public:
    BuildBudget(std::uint32_t maxBuilds, std::uint64_t maxTriangles) : maxBuilds_(maxBuilds), maxTriangles_(maxTriangles) {}
    // whether a build of `triangles` fits (and takes it); the first of a frame always fits
    bool take(std::uint64_t triangles);
    std::uint32_t builds() const { return builds_; }
    std::uint64_t triangles() const { return triangles_; }

private:
    std::uint32_t maxBuilds_;
    std::uint64_t maxTriangles_;
    std::uint32_t builds_ = 0;
    std::uint64_t triangles_ = 0;
};

class Registry {
public:
    struct Instance {
        std::uint64_t key = 0;
        std::uint64_t blas = 0;    // BLAS id (the owner's table)
        std::uint64_t owner = 0;   // ownerKey or 0
        Rows rows{};
        std::uint32_t id = 0;      // stable id while it lives (debug colours)
        std::uint32_t flags = 0;   // the caller's (record flags), OR-ed over sightings
        std::uint32_t data = 0;    // the caller's (the alpha test word), the latest non-zero
        std::uint8_t views = 0;    // bit per view it was seen in (the caller's numbering)
        // the caller's material of this instance (enh/rt_material id, 0 = none) and the rank of the draw that gave it; the
        // frame + rank of the last draw that counted (the first per frame and rank: layered materials keep their base)
        std::uint32_t material = 0;
        std::uint8_t materialRank = 0;
        std::uint8_t sightRank = 0;
        std::uint64_t sightFrame = 0;
        std::uint64_t firstFrame = 0, lastFrame = 0;
        double lastTime = 0.0;
    };
    struct Counters {
        std::uint64_t added = 0, replaced = 0, aged = 0, tooFar = 0, capped = 0, dropped = 0;
    };

    // A sighting at `frame` / `time` in `view` (a bit): refreshes the instance of `key` or creates it. With an owner, the
    // owner's latest instance in the same view is replaced when it was not seen in this frame (the object moved / switched
    // its level of detail); one seen in this frame stays (an owner with several instances at once, another level of detail
    // in the other view). Returns the instance (pointer valid until the next change) and whether it is new.
    std::pair<Instance*, bool> see(std::uint64_t key, std::uint64_t blas, const Rows& rows, std::uint64_t owner, std::uint32_t flags,
                                   std::uint32_t data, std::uint8_t view, std::uint64_t frame, double time);
    // Drops instances unseen for limits.unseenFrames frames and limits.unseenSeconds, those farther than limits.radius from
    // `camera` (by the world sphere of bounds(blas); its translation when the bounds are unknown), and beyond
    // limits.maxRegistered the longest unseen. Returns how many were dropped.
    std::size_t age(const float camera[3], std::uint64_t frame, double time, const Limits& limits,
                    const std::function<const Aabb*(std::uint64_t)>& bounds);
    // drops every instance of a BLAS (its buffers changed / died); returns how many
    std::size_t removeBlas(std::uint64_t blas);
    void clear();
    std::size_t size() const { return list_.size(); }
    const std::vector<Instance>& instances() const { return list_; }
    const Instance* find(std::uint64_t key) const;
    Instance* find(std::uint64_t key);  // (pointer valid until the next change)
    // an instance only `view` saw while its owner has a live instance seen in `preferred` (the cascades draw some objects at
    // another level of detail than the main view: the TLAS takes the main view's, not both)
    bool supersededBy(const Instance& in, std::uint8_t preferred) const;
    const Counters& counters() const { return counters_; }

    // hooks for the owner's reference counts (called for every instance added / removed)
    std::function<void(const Instance&)> onAdded;
    std::function<void(const Instance&)> onRemoved;

private:
    static std::uint64_t ownerSlot(std::uint64_t owner, std::uint8_t view);
    void removeAt(std::size_t i);
    std::vector<Instance> list_;
    std::unordered_map<std::uint64_t, std::size_t> index_;   // key -> list_ index
    std::unordered_map<std::uint64_t, std::uint64_t> owners_; // owner x view -> the key of its latest instance there
    std::uint32_t nextId_ = 1;
    Counters counters_;
};

// The instances that go into the TLAS: all when they fit `max`, else the ones seen most recently, nearest first among
// equals (indices into `list`); `take(instance)` filters (BLAS not built yet, superseded levels of detail)
std::vector<std::uint32_t> selectForTlas(const std::vector<Registry::Instance>& list, const float camera[3], std::uint32_t max,
                                         const std::function<bool(const Registry::Instance&)>& take);

}  // namespace rvk::enh::rt
