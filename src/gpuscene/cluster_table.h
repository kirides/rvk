#pragma once

// F5-1 clusters of large static meshes (docs/research/frame-ownership.md 7.33, setting native.clusters; the build is src/core/cluster_build.*).
// For a draw of a static mesh above the size threshold (>= 1,024 triangles, rigid, opaque z / solid / shadow depth) the store keeps, per distinct draw range
//   * the cluster order of its triangles (built on a worker thread from copies of the range's indices and positions, cached on disk by content hash),
//   * a COPY of the range's indices in cluster order in the mesh arena (the engine's own copy stays as it is: other draws, the RT scene and the vertex captures read
//     it; a draw of the range has its packet pointed at the copy: same triangles, same vertex order of every triangle, so the depth EQUAL of the solid pass holds),
//   * the cluster rows in a GPU table (one 32-byte row per cluster: bounding sphere, first index in the arena buffer, index count, range id) = what F5-2's cull reads.
// Lifetime: an entry lives with the engine index buffer's static GPU copy (`bufferChanged` on a rename / destruction frees the arena copy, deferred, and the rows).
// A retained draw (packet cache / owned records) keeps the packet it was captured with: one that was captured while the range's build was pending is remade when
// the build finishes (`epoch`).
// Everything is made on the first draw that asks: with the setting off nothing exists (no thread, no buffers).

#include <array>
#include <atomic>
#include <condition_variable>
#include <deque>
#include <memory>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <vector>

#include "core/cluster_build.h"
#include "gpuscene/illumination_table.h"
#include "rhi/game_device.h"

namespace rvk::d3d9 {
class Device;
}

namespace rvk::gpuscene {

// GPU: one row per cluster (the cull's input). The cluster's triangles are indices [firstIndex, firstIndex + indexCount) of the arena buffer of the range's copy.
struct ClusterRow {
    float c[3];
    float r;
    std::uint32_t firstIndex;
    std::uint32_t indexCount;
    std::uint32_t range;  // the entry id (ClusterStore::Entry::id, low 32 bits)
    std::uint32_t pad;
};
static_assert(sizeof(ClusterRow) == 32);

struct ClusterKeyHash {
    std::size_t operator()(const std::array<std::uint64_t, 6>& k) const {
        std::uint64_t h = 0x9E3779B97F4A7C15ull;
        for (const std::uint64_t v : k) h = (h ^ (v + 0x9E3779B97F4A7C15ull + (h << 6) + (h >> 2))) * 0xff51afd7ed558ccdull;
        return static_cast<std::size_t>(h);
    }
};

class ClusterStore {
public:
    static constexpr std::uint32_t kRowCapacity = 65536;      // clusters (2 MiB)
    static constexpr std::uint64_t kMaxArenaBytes = 192ull << 20;  // the index copies
    static constexpr std::uint32_t kMaxPending = 256;
    static constexpr std::uint32_t kLargeTriangles = 1024;     // the size threshold (frame-ownership.md 7.22)

    using Key = std::array<std::uint64_t, 6>;
    enum class State : std::uint8_t { Pending, Built, Ready, Failed };

    struct Entry {
        Key key{};
        std::uint64_t id = 0, ibId = 0;
        std::atomic<State> state{State::Pending};
        std::atomic<bool> dead{false};
        bool accounted = false;          // no longer counted in `pending_` (Ready, Failed, or released)
        // the job (engine thread -> worker; released by the worker)
        std::vector<std::uint32_t> idx;  // local vertex ids
        std::vector<float> pos;
        std::uint64_t srcHash = 0;       // hash64 of the range's index bytes at the request
        // the result (worker -> engine thread)
        clusters::Result result;
        std::uint64_t contentHash = 0;
        bool fromDisk = false;
        double buildMs = 0.0;
        const char* why = nullptr;       // Failed: the reason (a literal)
        // GPU side (render thread)
        rhi::MeshAlloc alloc;
        std::uint32_t isz = 0, triangles = 0, rowFirst = 0, rowCount = 0;
        std::uint64_t arenaBytes = 0;
        bool cullOk = false;  // F5-2: the rows tile the copy's indices contiguously from its first index and there are at most clustercull::kMaxRows of them
    };

    ClusterStore();
    ~ClusterStore();

    bool active() const { return active_; }
    // the setting (render thread); off = what exists goes (arena copies freed deferred, rows released, the worker stopped)
    void setActive(d3d9::Device& dev, bool on);

    // render thread: the entry of a draw range (null: none yet)
    Entry* find(const Key& key);
    // render thread: a new Pending entry + its build job (null: the queue is full)
    Entry* request(const Key& key, std::uint64_t ibId, std::vector<std::uint32_t>&& idx, std::vector<float>&& pos, std::uint64_t srcHash);
    // render thread: the arena copy's read-back verification (Vulkan: the arena allows TRANSFER_SRC): up to this many uploads are compared, then every 8th
    static constexpr std::uint32_t kVerifyFirst = 24;
    // render thread: Built -> Ready: the arena copy of the range's indices in cluster order + the table rows. `src` = the range's index bytes read from the engine's CPU copy
    // NOW (3 x isz per triangle); a mismatch with the request's hash = Failed. False: Failed or not possible now (the entry's state says which)
    bool upload(d3d9::Device& dev, Entry& e, const std::vector<std::uint8_t>& src, std::uint32_t isz);
    // the engine index buffer's static copy is gone / replaced: its entries go. Any thread (queued); the render thread frees them in drain() (every clustered draw and every present)
    void bufferChanged(std::uint64_t ibId);
    void drain(d3d9::Device& dev);
    // bumped whenever an entry becomes Ready (statistics)
    std::uint64_t epoch() const { return epoch_; }
    // a retained draw / op record that was captured while entry `id` was Pending (`used` false) or drew with its copy (`used` true) asks whether that is still so; it is
    // remade when not (the build settled, or the entry is gone: its copy was freed / the setting went off) (render thread)
    bool stale(std::uint64_t id, bool used) const {
        const auto it = byId_.find(id);
        if (it == byId_.end()) return true;
        const State s = it->second->state.load(std::memory_order_acquire);
        return used ? s != State::Ready : s != State::Pending;
    }
    // F5-2: what a record needs to cull the range's draw on the GPU (a Ready entry whose rows tile its copy: `firstIndex` / `indexCount` = the whole copy's range)
    struct CullInfo {
        std::uint32_t rowFirst = 0, rowCount = 0, firstIndex = 0, indexCount = 0;
    };
    bool cullInfo(std::uint64_t id, CullInfo& out) const;
    // the CPU twin of rows [first, first + count) (null when out of range / no table): the check frames' exact evaluation
    const ClusterRow* rowsAt(std::uint32_t first, std::uint32_t count) const;
    // the table's GPU reference (F5-2); null before the first upload
    rhi::GpuRef tableRef() const { return table_ ? table_->ref() : rhi::GpuRef{}; }
    // once per present: finished builds (the 5 s line), the table's upload + read-back check
    void flush(d3d9::Device& dev);
    void destroy(d3d9::Device* dev);

    struct Stats {
        std::uint64_t requested = 0, queueFull = 0, built = 0, fromDisk = 0, failedBuild = 0, failedUpload = 0, failedTable = 0, failedMemory = 0, failedData = 0;
        std::uint64_t uploaded = 0, clusters = 0, triangles = 0, arenaBytes = 0, freed = 0;
        std::uint64_t multisetChecked = 0, multisetBad = 0, perDrawUsed = 0, readbackChecked = 0, readbackBad = 0, readbackSkipped = 0;
        double buildMsSum = 0.0, buildMsMax = 0.0;
    };
    Stats& stats() { return stats_; }
    // counted by the device: draws that took the clustered copy
    void countUse() { ++stats_.perDrawUsed; }
    std::uint32_t rowsUsed() const { return rowsUsed_; }
    std::uint64_t arenaBytes() const { return arenaBytes_; }

private:
    void workerMain();
    void buildOne(Entry& e);
    void releaseEntry(d3d9::Device* dev, Entry& e);
    std::uint32_t allocRows(std::uint32_t n);
    void freeRows(std::uint32_t first, std::uint32_t n);
    bool ensureTable(d3d9::Device& dev);
    void report();

    bool active_ = false;
    std::mutex goneMutex_;
    std::vector<std::uint64_t> gone_;
    std::atomic<bool> goneAny_{false};
    std::unordered_map<Key, std::shared_ptr<Entry>, ClusterKeyHash> entries_;
    std::unordered_map<std::uint64_t, Entry*> byId_;
    std::uint64_t nextId_ = 1;
    std::atomic<std::uint64_t> epoch_{1};
    std::uint32_t pending_ = 0;
    // the worker
    std::thread worker_;
    std::mutex mutex_;
    std::condition_variable cv_;
    std::deque<std::shared_ptr<Entry>> queue_;
    bool quit_ = false;
    // the table
    std::unique_ptr<GpuTable<ClusterRow>> table_;
    bool tableFailed_ = false;
    std::vector<std::pair<std::uint32_t, std::uint32_t>> freeRows_;  // (first, count), sorted
    std::uint32_t rowsUsed_ = 0, rowsHigh_ = 0;
    std::uint64_t arenaBytes_ = 0;
    std::uint64_t frame_ = 0;
    struct Check {
        std::unique_ptr<rhi::Buffer> buffer;
        std::vector<std::uint8_t> expect;
        std::uint64_t frameValue = 0;
        std::uint64_t id = 0;
        bool busy = false;
    };
    Check checks_[rhi::kFramesInFlight];
    std::uint64_t uploads_ = 0;
    Stats stats_{}, last_{};
    double lastReport_ = 0.0;
};

ClusterStore& clusterstore();

}  // namespace rvk::gpuscene
