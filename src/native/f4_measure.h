#pragma once

// Frame ownership stage F4-0 (docs/research/frame-ownership.md 10, 7.15): measurements of the SpeedTree / tree billboard ops, no behaviour change.
// Enabled by the environment variable RVK_F4_MEASURE=1 (read once with GetEnvironmentVariableA; off = one bool test per call site). Render thread only.
//   * QPC phase timers of composeTree split by main solid / main z / shadow z,
//   * a census (unique trees per view, ops, (op, type, LOD) buckets, calls under the F4 cut rule vs today's batches, instances per bucket,
//     two-bucket leaves, trees without an entity, sun values, illumination classes, hardware / software instancing),
//   * invalidation rates of the per-tree / per-render-buffer / wind inputs (the 7.12 format: changed / pairs),
//   * the CTAB of the tree vertex / pixel shaders actually drawn and the stream-1 (vInstance) vertex buffer,
//   * billboards: ops / draws per view and the invalidation of their constant block.
// Log: `native: f4 measure (5 s) <section>: ...` every 5 s (raw sums: add the windows up), `native: f4 ctab ...` / `native: f4 stream1 ...` once per object.

#include <cstddef>
#include <cstdint>

namespace rvk::native::f4m {

namespace detail {
extern bool g_on;
}
inline bool on() { return detail::g_on; }
void init();  // reads the environment once (call at an op)

enum Cat : int { kMainSolid, kMainZ, kShadowZ, kCats };

// phases of composeTree (a phase scope pauses the one it is nested in)
enum Phase : int {
    kQueue,     // queue copy + sort (+ the lean sort)
    kSnapshot,  // per-tree snapshot reads (LOD fields)
    kLight,     // readLight (illumination reads, batch light inputs)
    kRows,      // entityWorld + the instance rows (world, alpha, instance data)
    kPlan,      // planTreeBatches (+ the cut statistics)
    kDraw,      // drawBatch: materialDraws / specialPassDraw (composition of the draws)
    kOther,     // the rest of an op
    kIssue,     // StaticOps::issue() of a tree op (outside composeTree)
    kCensus,    // this module's own work (not part of the op)
    kPhases
};
int enter(int phase);  // charges the time since the last switch to the phase being left; returns it (-1: no op active)
struct PhaseScope {
    int prev = -1;
    explicit PhaseScope(Phase p) {
        if (on()) prev = enter(p);
    }
    ~PhaseScope() {
        if (on() && prev >= 0) enter(prev);
    }
    PhaseScope(const PhaseScope&) = delete;
    PhaseScope& operator=(const PhaseScope&) = delete;
};

struct OpScope {
    explicit OpScope(const std::size_t* drawCount, std::uint64_t frame, std::uint32_t viewKind, std::int32_t cascade, const void* db, const void* desc, bool zpass,
                     bool shadow);
    ~OpScope();
    OpScope(const OpScope&) = delete;
    OpScope& operator=(const OpScope&) = delete;
    bool active = false;
};

// StaticOps::issue() of an op (single hook in static_ops.cpp): kind 1 = a SpeedTree op, 2 = a tree billboard op, 0 = any other op (not timed)
struct IssueScope {
    std::int64_t t0 = 0;
    int kind = 0;  // 0 nothing, 1 SpeedTree op, 2 tree billboard op
    explicit IssueScope(int kind);
    ~IssueScope();
    IssueScope(const IssueScope&) = delete;
    IssueScope& operator=(const IssueScope&) = delete;
};

struct OpInfo {
    std::uint32_t type = 0;       // 0 branches, 1 fronds, 2 leaf cards, 3 leaf meshes
    bool hardware = false;        // admin +0x320
    std::uint8_t limit = 0;       // rb +0x128 + type
    const std::uint8_t* rb = nullptr;
    const std::uint8_t* admin = nullptr;
    const std::uint8_t* queue = nullptr;  // n entries of 0x18 bytes (tree, illumination, key), sorted
    std::int32_t n = 0;
    std::int32_t leafLods = 0;
    const void* shader = nullptr;  // the material's shader (pipeline proxy)
    const void* vb = nullptr;      // stream 0 buffer, index buffer, primitive type
    const void* ib = nullptr;
    std::uint32_t primType = 0;
};
void census(const OpInfo& op);                              // once per op, after the queue is sorted and the geometry is known
void batches(std::uint32_t planned, std::uint32_t instances);  // a planTreeBatches result (today's batches of the op)
// a draw composed for a tree op (vertex / pixel shader, the streams)
void draw(const void* vs, const void* ps, const void* stream1Buffer, std::uint32_t stream1Offset, std::uint32_t stream1Stride, std::uint32_t stream1Freq,
          std::uint32_t stream0Freq);

// ---- billboards (composeTreeBillboard) ----
struct BbScope {
    explicit BbScope(const std::size_t* drawCount, std::uint64_t frame, std::uint32_t viewKind, std::int32_t cascade, const void* db, const void* desc, bool zpass,
                     bool shadow);
    ~BbScope();
    BbScope(const BbScope&) = delete;
    BbScope& operator=(const BbScope&) = delete;
    bool active = false;
};
void bbConstants(const void* ps, std::uint32_t desc, const void* rows, std::size_t bytes);

}  // namespace rvk::native::f4m
