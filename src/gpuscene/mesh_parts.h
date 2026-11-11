#pragma once

// The MeshPart + Layout tables (docs/research/frame-ownership.md F0 / 1.2 / 1.4; layout and record formats: gpuscene/mesh_layout.h; GPU side
// shaders/include/rvk_mesh.hlsli). Setting `native.mesh_parts` (Debug, restart, default off; Full only: only the static composition's draws feed it):
//   * a part is created at the first sight of a sub-mesh's index range among the composed draws (StaticOps::issue, after the draw: the buffers have their GPU
//     copies by then), keyed by (sub-mesh, first index, primitive count): the main view's z / solid draws and the shadow views' z draws; draws without an
//     eCSubMesh (actors, trees, vegetation) and CPU-generated geometry are not parts yet;
//   * a part names its buffers' mesh-arena addresses (Buffer::arenaAddress; 0 and flag `in arena` clear for ring-copied buffers / no device addresses / not
//     uploaded yet: such a part keeps the CPU description and is counted by reason);
//   * freed when one of its buffers gets a new GPU copy or is released (DrawObserver::bufferChanged), or when not drawn for kIdleFrames frames;
//   * layouts are deduplicated by content (the declaration's elements).
// Verification (F0 gate): every composed draw is compared with its record (index range, base vertex, every stream's address / stride, layout, material key)
// and, for a draw issued fresh, with the device's actual bindings: rvk.log `native: mesh parts (5 s): ... DIFFERING N`, must stay 0. Every 240th frame the
// tables are read back and compared with the CPU copy, and a compute shader (shaders/test/mesh_fetch.comp.hlsl) fetches indices / positions / normals /
// uv0 / colour 0 of a few hundred random (part, vertex) pairs through rvk_mesh.hlsli and compares them with the CPU decode of the same vertices.

#include <array>
#include <cstdint>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <vector>

#include "d3d9/device.h"
#include "gpuscene/illumination_table.h"
#include "gpuscene/mesh_layout.h"
#include "rhi/game_device.h"

namespace rvk::gpuscene {

// what a draw is, as far as the part table cares (filled by native/static_ops from the composed DrawDesc or from the device's bindings)
struct DrawGeometry {
    const d3d9::VertexDecl* decl = nullptr;
    d3d9::Buffer* ib = nullptr;
    std::array<d3d9::Buffer*, 3> vb{};
    std::array<std::uint32_t, 3> offset{}, stride{};
    bool indexed = true;
    std::uint32_t minIndex = 0, numVertices = 0, startIndex = 0, primCount = 0;
    std::int32_t baseVertex = 0;
    const void* subMesh = nullptr;
    const void* material = nullptr;
    const void* resource = nullptr;
};

struct PartStats {
    std::uint64_t draws = 0, checked = 0, differing = 0, checkedBound = 0, replayed = 0, lateResolved = 0;
    std::uint64_t created = 0, droppedByBuffer = 0, idleFreed = 0, overflow = 0;
    std::uint64_t skippedNoSubMesh = 0, skippedUnindexed = 0, skippedNoBuffer = 0;
    std::uint64_t fetchTests = 0, fetchCases = 0, fetchDiffering = 0, fetchSkipped = 0;
};

class MeshPartTable : public d3d9::DrawObserver {
public:
    MeshPartTable();
    ~MeshPartTable() override;
    static constexpr std::uint32_t kPartCapacity = 16384;  // slots (2 MiB)
    static constexpr std::uint32_t kLayoutCapacity = 256;
    static constexpr std::uint64_t kIdleFrames = 1800;
    static constexpr std::uint32_t kFetchCases = 256;

    void setActive(bool on) { active_ = on; }
    bool active() const { return active_; }

    // After a composed draw was issued (engine thread). `desc` = the composition, `bound` = the device's actual bindings (null: the draw was replayed from a
    // retained packet, the device state was not set). `cls` = the draw's class index (for the per-class counts).
    void checkDraw(d3d9::Device& dev, const DrawGeometry& desc, const DrawGeometry* bound, std::uint32_t cls);

    // once per present (engine thread, before the frame's draws are submitted): uploads, read-back verify, the fetch test, idle sweep, 5 s log
    void flush(d3d9::Device& dev, bool verify);
    void releaseGpu();  // device shutdown (gpuscene::releaseGpuObjects): the buffers and the fetch pipeline go before the device

    rhi::GpuRef partRef() const { return parts_.ref(); }
    rhi::GpuRef layoutRef() const { return layouts_.ref(); }
    const PartStats& stats() const { return stats_; }
    std::uint32_t liveParts() const { return liveParts_; }
    std::uint32_t layoutCount() const { return static_cast<std::uint32_t>(layoutEls_.size()); }

    // DrawObserver (only the buffer notifications are used)
    bool observes(d3d9::draws::Kind, d3d9::scene::Phase) const override { return false; }
    void observe(d3d9::Device&, const d3d9::ReplaceQuery&) override {}
    void bufferChanged(std::uint64_t bufferId) override;
    // it reads no draw state (the table is fed by the static composition's checkDraw; the observer slot only carries the buffer notifications), so it must not
    // gate the owned path: the default Registers::All did from its first composed draw on, and nothing was owned while native.mesh_parts was on
    // (frame-ownership.md 14.18). `RVK_MESHPARTS_GATE=1` = the old declaration (A/B of the gate)
    Registers registersRead() const override;
    const char* observerName() const override { return "mesh part table"; }

private:
    struct Key {
        const void* sub;
        std::uint32_t first, count;
        bool operator==(const Key&) const = default;
    };
    struct KeyHash {
        std::size_t operator()(const Key& k) const {
            return std::hash<const void*>()(k.sub) ^ (static_cast<std::size_t>(k.first) * 0x9E3779B97F4A7C15ull) ^ (static_cast<std::size_t>(k.count) << 17);
        }
    };
    enum Reason : std::uint8_t { kInArena = 0, kRingCopied = 1, kNoAddress = 2, kNoBuffer = 3, kReasons = 4 };
    struct PartCpu {
        Key key{};
        std::array<std::uint64_t, 4> ids{};  // buffer ids: ib, vb0..2 (0 = none)
        d3d9::Buffer* ib = nullptr;
        std::array<d3d9::Buffer*, 3> vb{};
        std::array<std::uint32_t, 3> offset{}, stride{};
        std::int32_t base = 0;
        std::uint64_t seen = 0;
        std::uint8_t cls = 0, reason = kInArena;
        bool live = false;
    };
    // all under mutex_
    std::uint32_t layoutFor(const d3d9::VertexDecl* decl);
    PartRecord makeRecord(const DrawGeometry& g, std::uint32_t layout, std::uint8_t& reason);
    void dropPart(std::uint32_t slot, bool byBuffer);
    void setIds(std::uint32_t slot, const DrawGeometry& g);
    void runFetchTest(d3d9::Device& dev);
    void collectFetch(d3d9::Device& dev);
    void report();

    bool active_ = false;
    bool observing_ = false;
    std::mutex mutex_;
    GpuTable<PartRecord> parts_;
    GpuTable<LayoutRecord> layouts_;
    std::unordered_map<Key, std::uint32_t, KeyHash> slotOf_;
    std::unordered_multimap<std::uint64_t, std::uint32_t> byBuffer_;
    std::vector<PartCpu> cpu_;
    std::vector<std::uint32_t> freeSlots_;
    std::uint32_t used_ = 0, liveParts_ = 0, peakParts_ = 0;
    std::vector<std::vector<d3d9::VertexElement>> layoutEls_;
    std::unordered_multimap<std::uint64_t, std::uint32_t> layoutByHash_;
    std::unordered_map<std::uint64_t, std::uint32_t> layoutOfDecl_;  // VertexDecl::id -> layout id
    std::uint64_t frame_ = 0;
    PartStats stats_;
    std::array<std::array<std::uint64_t, kReasons>, 8> classReason_{};  // (parts created: class x reason)
    std::array<std::uint64_t, 8> classDraws_{};
    double lastReport_ = 0.0;
    int differingLogged_ = 0, fetchLogged_ = 0;
    bool gpuFailed_ = false;
    std::uint64_t lastSlots_ = 0, lastBytes_ = 0, lastFrame_ = 0;

    // the fetch test: pipeline (built in the background), cases + results per frame slot
    struct Fetch {
        std::unique_ptr<rhi::Buffer> out;       // device memory the shader writes
        std::unique_ptr<rhi::Buffer> readback;  // per frame slot
        std::uint64_t frameValue = 0;
        std::uint32_t count = 0;
        bool busy = false;
        struct Expect {
            std::uint32_t index = 0, valid = 0;
            float pos[4]{}, nrm[4]{}, uv[4]{}, col[4]{};
            std::uint32_t slot = 0, vertex = 0, k = 0;
        };
        std::vector<Expect> expect;
    };
    Fetch fetch_[rhi::kFramesInFlight];
    struct FetchPipeline;
    std::unique_ptr<FetchPipeline> pipeline_;
};

MeshPartTable& meshparts();

}  // namespace rvk::gpuscene
