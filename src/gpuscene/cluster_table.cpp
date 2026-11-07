#include "gpuscene/cluster_table.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>

#include <spdlog/spdlog.h>

#include "core/cluster_cull.h"
#include "core/paths.h"
#include "d3d9/device.h"

namespace rvk::gpuscene {

namespace {
double nowMs() { return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now().time_since_epoch()).count(); }
}  // namespace

ClusterStore& clusterstore() {
    static ClusterStore* s = new ClusterStore();  // (leaked like the other tables: the device goes first)
    return *s;
}

ClusterStore::ClusterStore() = default;

ClusterStore::~ClusterStore() { destroy(nullptr); }

void ClusterStore::setActive(d3d9::Device& dev, bool on) {
    if (on == active_) return;
    if (on) {
        active_ = true;
        spdlog::info("native: clusters on: static meshes of >= {} triangles are clustered ({} triangles per cluster at most), index copies in cluster order in the mesh arena, {} table rows", kLargeTriangles,
                     clusters::kMaxClusterTriangles, kRowCapacity);
        return;
    }
    active_ = false;
    destroy(&dev);
    spdlog::info("native: clusters off");
}

ClusterStore::Entry* ClusterStore::find(const Key& key) {
    const auto it = entries_.find(key);
    if (it == entries_.end()) return nullptr;
    Entry& e = *it->second;
    if (!e.accounted && e.state.load(std::memory_order_acquire) == State::Failed) {  // (a build that failed on the worker)
        e.accounted = true;
        --pending_;
        ++stats_.failedBuild;
        spdlog::warn("native: clusters: the build of range {} failed: {}", e.id, e.why != nullptr ? e.why : "?");
    }
    return &e;
}

ClusterStore::Entry* ClusterStore::request(const Key& key, std::uint64_t ibId, std::vector<std::uint32_t>&& idx, std::vector<float>&& pos, std::uint64_t srcHash) {
    if (pending_ >= kMaxPending) {
        ++stats_.queueFull;
        return nullptr;
    }
    auto e = std::make_shared<Entry>();
    e->key = key;
    e->id = nextId_++;
    e->ibId = ibId;
    e->idx = std::move(idx);
    e->pos = std::move(pos);
    e->srcHash = srcHash;
    entries_[key] = e;
    byId_[e->id] = e.get();
    ++pending_;
    ++stats_.requested;
    {
        const std::lock_guard lock(mutex_);
        if (!worker_.joinable()) worker_ = std::thread([this] { workerMain(); });
        queue_.push_back(e);
    }
    cv_.notify_one();
    return e.get();
}

void ClusterStore::workerMain() {
    for (;;) {
        std::shared_ptr<Entry> e;
        {
            std::unique_lock lock(mutex_);
            cv_.wait(lock, [this] { return quit_ || !queue_.empty(); });
            if (quit_) return;
            e = std::move(queue_.front());
            queue_.pop_front();
        }
        if (!e->dead.load(std::memory_order_acquire)) buildOne(*e);
        else e->state.store(State::Failed, std::memory_order_release);
    }
}

void ClusterStore::buildOne(Entry& e) {
    const double t0 = nowMs();
    const std::uint64_t h = clusters::contentHash(e.idx, e.pos);
    char name[32];
    std::snprintf(name, sizeof(name), "%016llx.cl", static_cast<unsigned long long>(h));
    const bool disk = !paths::dataDir().empty();  // (the tools have no data directory: no disk cache there)
    const std::filesystem::path file = disk ? paths::dataDir() / "cache" / "clusters" / name : std::filesystem::path();
    bool ok = false;
    if (disk && clusters::load(file, h, e.idx, e.pos, e.result)) {
        ok = true;
        e.fromDisk = true;
    } else if (clusters::build(e.idx, e.pos, e.result)) {
        std::string why;
        if (clusters::verify(e.idx, e.pos, e.result, &why)) {
            ok = true;
            if (disk) clusters::save(file, h, e.result);
        } else {
            e.why = "the cluster result failed its checks";
        }
    } else {
        e.why = "the cluster builder returned nothing";
    }
    if (ok) {
        // (the property the solid pass relies on: the same ordered triples)
        const std::vector<std::uint32_t> out = clusters::reorder(e.idx, e.result);
        if (clusters::tripleMultiset(out.data(), out.size() / 3) != clusters::tripleMultiset(e.idx.data(), e.idx.size() / 3)) {
            ok = false;
            e.why = "the reordered triangles differ from the engine's";
        }
    }
    e.contentHash = h;
    e.buildMs = nowMs() - t0;
    std::vector<std::uint32_t>().swap(e.idx);
    std::vector<float>().swap(e.pos);
    if (!ok) e.result = {};
    e.state.store(ok ? State::Built : State::Failed, std::memory_order_release);
}

std::uint32_t ClusterStore::allocRows(std::uint32_t n) {
    for (auto it = freeRows_.begin(); it != freeRows_.end(); ++it) {
        if (it->second < n) continue;
        const std::uint32_t first = it->first;
        it->first += n;
        it->second -= n;
        if (it->second == 0) freeRows_.erase(it);
        return first;
    }
    if (rowsHigh_ + n > kRowCapacity) return ~0u;
    const std::uint32_t first = rowsHigh_;
    rowsHigh_ += n;
    return first;
}

void ClusterStore::freeRows(std::uint32_t first, std::uint32_t n) {
    if (n == 0) return;
    // (the rows are zeroed and left for the next flush: nothing reads a freed range)
    for (std::uint32_t i = 0; i < n; ++i) {
        table_->shadow()[first + i] = {};
        table_->mark(first + i);
    }
    auto it = std::lower_bound(freeRows_.begin(), freeRows_.end(), std::make_pair(first, 0u));
    it = freeRows_.insert(it, {first, n});
    if (it + 1 != freeRows_.end() && it->first + it->second == (it + 1)->first) {
        it->second += (it + 1)->second;
        freeRows_.erase(it + 1);
    }
    if (it != freeRows_.begin() && (it - 1)->first + (it - 1)->second == it->first) {
        (it - 1)->second += it->second;
        freeRows_.erase(it);
    }
}

bool ClusterStore::ensureTable(d3d9::Device& dev) {
    if (table_) return table_->ready();
    if (tableFailed_) return false;
    table_ = std::make_unique<GpuTable<ClusterRow>>();
    table_->init(kRowCapacity);
    if (!table_->create(dev, kRowCapacity, "cluster table")) {
        tableFailed_ = true;
        table_.reset();
        spdlog::warn("native: clusters: the cluster table could not be created");
        return false;
    }
    return true;
}

bool ClusterStore::upload(d3d9::Device& dev, Entry& e, const std::vector<std::uint8_t>& src, std::uint32_t isz) {
    const auto fail = [&](std::uint64_t& counter, const char* why) {
        ++counter;
        e.why = why;
        e.result = {};
        e.state.store(State::Failed, std::memory_order_release);
        if (!e.accounted) --pending_;
        e.accounted = true;
        return false;
    };
    if (e.state.load(std::memory_order_acquire) != State::Built) return false;
    const std::uint32_t tris = static_cast<std::uint32_t>(e.result.perm.size());
    const std::uint64_t bytes = static_cast<std::uint64_t>(tris) * 3 * isz;
    if (isz != 2 && isz != 4) return fail(stats_.failedData, "index size");
    if (src.size() != bytes || clusters::hash64(src.data(), src.size()) != e.srcHash) return fail(stats_.failedData, "the engine's index data changed since the request");
    if (arenaBytes_ + bytes > kMaxArenaBytes) return fail(stats_.failedMemory, "arena budget");
    if (!ensureTable(dev)) return fail(stats_.failedTable, "no table");
    const std::uint32_t n = static_cast<std::uint32_t>(e.result.clusters.size());
    const std::uint32_t rows = allocRows(n);
    if (rows == ~0u) return fail(stats_.failedTable, "the cluster table is full");
    rhi::GameDevice& game = dev.game();
    const rhi::MeshAlloc na = game.allocMesh(bytes, isz);
    if (!na.valid()) {
        freeRows(rows, n);
        return fail(stats_.failedUpload, "the mesh arena is full");
    }
    rhi::RingAlloc staging = game.allocRing(bytes, 16);
    if (staging.ptr == nullptr) {  // (the ring is full this frame: tried again at the next draw of the range)
        game.freeMesh(na, false);
        freeRows(rows, n);
        return false;
    }
    // the clustered bytes made in CPU memory (the ring may be write-combined: never read), checked against the engine's triangles, then copied
    std::vector<std::uint8_t> out(static_cast<std::size_t>(bytes));
    for (std::uint32_t k = 0; k < tris; ++k) std::memcpy(&out[static_cast<std::size_t>(k) * 3 * isz], &src[static_cast<std::size_t>(e.result.perm[k]) * 3 * isz], 3 * isz);
    {
        std::vector<std::uint32_t> a(static_cast<std::size_t>(tris) * 3), b(static_cast<std::size_t>(tris) * 3);
        for (std::size_t i = 0; i < a.size(); ++i) {
            if (isz == 4) {
                std::memcpy(&a[i], &src[i * 4], 4);
                std::memcpy(&b[i], &out[i * 4], 4);
            } else {
                std::uint16_t x, y;
                std::memcpy(&x, &src[i * 2], 2);
                std::memcpy(&y, &out[i * 2], 2);
                a[i] = x;
                b[i] = y;
            }
        }
        ++stats_.multisetChecked;
        if (clusters::tripleMultiset(a.data(), tris) != clusters::tripleMultiset(b.data(), tris)) {
            ++stats_.multisetBad;
            spdlog::error("native: clusters: the clustered copy of range {} does not hold the engine's triangles", e.id);
            game.freeMesh(na, false);
            freeRows(rows, n);
            return fail(stats_.failedData, "multiset differs");
        }
    }
    std::memcpy(staging.ptr, out.data(), out.size());
    rhi::CommandList& list = dev.uploadList();
    list.copyBuffer(*na.buffer, na.offset, game.ringBuffer(staging), staging.offset, bytes);
    // the read-back verification of the arena copy (Vulkan; the first uploads, then every 8th)
    const bool verify = game.caps().backend == rhi::Backend::Vulkan && (uploads_ < kVerifyFirst || uploads_ % 8 == 0);
    ++uploads_;
    if (verify) {
        Check& c = checks_[game.frameSlot() % rhi::kFramesInFlight];
        if (!c.busy) {
            c.buffer = dev.rhi()->createBuffer({bytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT, rhi::Memory::Readback, "cluster copy verify"});
            if (c.buffer) {
                list.memoryBarrier(VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_READ_BIT);
                list.copyBuffer(*c.buffer, 0, *na.buffer, na.offset, bytes);
                c.expect = out;
                c.frameValue = game.frameValue();
                c.id = e.id;
                c.busy = true;
            }
        } else {
            ++stats_.readbackSkipped;
        }
    }
    // the table rows
    const std::uint32_t firstIndex = static_cast<std::uint32_t>(na.offset / isz);
    for (std::uint32_t i = 0; i < n; ++i) {
        const clusters::Cluster& c = e.result.clusters[i];
        ClusterRow& r = table_->shadow()[rows + i];
        r.c[0] = c.c[0], r.c[1] = c.c[1], r.c[2] = c.c[2];
        r.r = c.r;
        r.firstIndex = firstIndex + c.firstTri * 3;
        r.indexCount = c.tris * 3;
        r.range = static_cast<std::uint32_t>(e.id);
        r.pad = 0;
        table_->mark(rows + i);
    }
    {
        // F5-2: the rows must tile the copy's index range (contiguous, in order) for the GPU cull's runs to be index ranges
        bool tile = n != 0 && n <= clustercull::kMaxRows;
        std::uint32_t next = firstIndex;
        for (std::uint32_t i = 0; i < n && tile; ++i) {
            const ClusterRow& r = table_->shadow()[rows + i];
            tile = r.firstIndex == next && r.indexCount != 0;
            next += r.indexCount;
        }
        e.cullOk = tile && next == firstIndex + tris * 3;
    }
    e.alloc = na;
    e.isz = isz;
    e.triangles = tris;
    e.rowFirst = rows;
    e.rowCount = n;
    e.arenaBytes = bytes;
    arenaBytes_ += bytes;
    rowsUsed_ += n;
    ++stats_.uploaded;
    stats_.clusters += n;
    stats_.triangles += tris;
    stats_.arenaBytes += bytes;
    e.result.perm.clear();
    e.result.perm.shrink_to_fit();
    e.state.store(State::Ready, std::memory_order_release);
    if (!e.accounted) --pending_;
    e.accounted = true;
    epoch_.fetch_add(1, std::memory_order_release);
    return true;
}

bool ClusterStore::cullInfo(std::uint64_t id, CullInfo& out) const {
    const auto it = byId_.find(id);
    if (it == byId_.end() || !table_ || !table_->ready()) return false;
    const Entry& e = *it->second;
    if (e.state.load(std::memory_order_acquire) != State::Ready || !e.cullOk || e.rowFirst + e.rowCount > kRowCapacity) return false;
    out.rowFirst = e.rowFirst;
    out.rowCount = e.rowCount;
    out.firstIndex = e.isz != 0 ? static_cast<std::uint32_t>(e.alloc.offset / e.isz) : 0;
    out.indexCount = e.triangles * 3;
    return true;
}

const ClusterRow* ClusterStore::rowsAt(std::uint32_t first, std::uint32_t count) const {
    if (!table_ || static_cast<std::uint64_t>(first) + count > table_->shadow().size()) return nullptr;
    return table_->shadow().data() + first;
}

void ClusterStore::releaseEntry(d3d9::Device* dev, Entry& e) {
    e.dead.store(true, std::memory_order_release);
    byId_.erase(e.id);
    const State s = e.state.load(std::memory_order_acquire);
    if (!e.accounted) --pending_;
    e.accounted = true;
    if (s == State::Ready) {
        if (dev != nullptr) dev->game().freeMesh(e.alloc, true);
        if (table_) freeRows(e.rowFirst, e.rowCount);
        arenaBytes_ -= e.arenaBytes;
        rowsUsed_ -= e.rowCount;
        ++stats_.freed;
    }
    e.state.store(State::Failed, std::memory_order_release);
}

void ClusterStore::bufferChanged(std::uint64_t ibId) {
    if (!active_) return;
    const std::lock_guard lock(goneMutex_);
    gone_.push_back(ibId);
    goneAny_.store(true, std::memory_order_release);
}

void ClusterStore::drain(d3d9::Device& dev) {
    if (!goneAny_.load(std::memory_order_acquire)) return;
    std::vector<std::uint64_t> ids;
    {
        const std::lock_guard lock(goneMutex_);
        ids.swap(gone_);
        goneAny_.store(false, std::memory_order_release);
    }
    for (auto it = entries_.begin(); it != entries_.end();) {
        if (std::find(ids.begin(), ids.end(), it->second->ibId) != ids.end()) {
            releaseEntry(&dev, *it->second);
            it = entries_.erase(it);
        } else {
            ++it;
        }
    }
}

void ClusterStore::flush(d3d9::Device& dev) {
    ++frame_;
    drain(dev);
    rhi::GameDevice& game = dev.game();
    // finished read-backs
    for (Check& c : checks_) {
        if (!c.busy || game.completedFrame() < c.frameValue) continue;
        c.busy = false;
        c.buffer->invalidate();
        ++stats_.readbackChecked;
        if (std::memcmp(c.buffer->map(), c.expect.data(), c.expect.size()) != 0) {
            ++stats_.readbackBad;
            spdlog::error("native: clusters: DIFFERING arena copy of range {} (read back from the GPU)", c.id);
        }
        c.buffer.reset();
        c.expect.clear();
    }
    if (table_ && table_->ready()) {
        table_->flush(dev, frame_ % 240 == 0, std::max(rowsHigh_, 1u));
        table_->collect(game, "cluster table");
    }
    report();
}

void ClusterStore::report() {
    const double now = nowMs() / 1000.0;
    if (now - lastReport_ < 5.0) return;
    lastReport_ = now;
    if (stats_.requested == 0) return;
    // a finished build that was never uploaded (no draw asked again) is still counted from the entries
    std::uint32_t pend = 0, built = 0, ready = 0, failed = 0;
    std::uint64_t diskHits = 0, builds = 0;
    double ms = 0.0, msMax = 0.0;
    for (const auto& kv : entries_) {
        const Entry& e = *kv.second;
        switch (e.state.load(std::memory_order_acquire)) {
        case State::Pending: ++pend; break;
        case State::Built: ++built; [[fallthrough]];
        case State::Ready:
            if (e.state.load() == State::Ready) ++ready;
            ++builds;
            diskHits += e.fromDisk ? 1 : 0;
            if (!e.fromDisk) {
                ms += e.buildMs;
                msMax = std::max(msMax, e.buildMs);
            }
            break;
        case State::Failed: ++failed; break;
        }
    }
    const Stats& s = stats_;
    const Stats& l = last_;
    spdlog::info("native: clusters (5 s): {} ranges (pending {}, built {}, ready {}, failed {}); builds {} ({} from the disk cache, {:.1f} ms of builds, max {:.1f}); uploaded {} ranges / {} clusters / {} triangles (+{} ranges in 5 s), "
                 "arena copies {:.2f} MiB, table {} / {} rows; failures: build {}, data {}, table {}, memory {}, arena {}; queue full {}; draws using a copy {} (+{}); multiset checks {} bad {}; "
                 "read-backs {} DIFFERING {} (skipped {})",
                 entries_.size(), pend, built, ready, failed, builds, diskHits, ms, msMax, s.uploaded, s.clusters, s.triangles, s.uploaded - l.uploaded, static_cast<double>(arenaBytes_) / (1 << 20), rowsUsed_,
                 kRowCapacity, s.failedBuild, s.failedData, s.failedTable, s.failedMemory, s.failedUpload, s.queueFull, s.perDrawUsed, s.perDrawUsed - l.perDrawUsed, s.multisetChecked, s.multisetBad,
                 s.readbackChecked, s.readbackBad, s.readbackSkipped);
    last_ = stats_;
}

void ClusterStore::destroy(d3d9::Device* dev) {
    {
        const std::lock_guard lock(mutex_);
        quit_ = true;
    }
    cv_.notify_all();
    if (worker_.joinable()) worker_.join();
    quit_ = false;
    queue_.clear();
    for (auto& kv : entries_) releaseEntry(dev, *kv.second);
    entries_.clear();
    byId_.clear();
    for (Check& c : checks_) c = Check{};
    table_.reset();
    tableFailed_ = false;
    freeRows_.clear();
    rowsUsed_ = rowsHigh_ = 0;
    arenaBytes_ = 0;
    pending_ = 0;
    epoch_.fetch_add(1, std::memory_order_release);
}

}  // namespace rvk::gpuscene
