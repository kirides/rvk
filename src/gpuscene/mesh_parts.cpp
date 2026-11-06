#include "gpuscene/mesh_parts.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <format>
#include <future>
#include <string>

#include <spdlog/spdlog.h>

#include "shader/embedded.h"

namespace rvk::gpuscene {

namespace {
constexpr std::uint64_t kVerifyEvery = 240;
constexpr std::uint32_t kNone = 0xffffffffu;
constexpr const char* kClassNames[8] = {"z", "base", "additive", "z-fill", "overlay", "non-op", "sky", "other"};
constexpr const char* kReasonNames[4] = {"in arena", "ring-copied", "no arena address", "no buffer"};

void putPair(std::uint32_t* dst, std::uint64_t v) {
    dst[0] = static_cast<std::uint32_t>(v);
    dst[1] = static_cast<std::uint32_t>(v >> 32);
}
}  // namespace

struct MeshPartTable::FetchPipeline {
    std::future<std::unique_ptr<rhi::Pipeline>> building;
    std::unique_ptr<rhi::Pipeline> pipe;
    bool failed = false;
};

MeshPartTable::MeshPartTable() = default;
MeshPartTable::~MeshPartTable() = default;

MeshPartTable& meshparts() {
    static MeshPartTable t;
    return t;
}

std::uint32_t MeshPartTable::layoutFor(const d3d9::VertexDecl* decl) {
    if (decl == nullptr) return kNone;
    if (const auto it = layoutOfDecl_.find(decl->id()); it != layoutOfDecl_.end()) return it->second;
    const auto& els = decl->elements();
    const std::uint64_t h = hashElements(els);
    std::uint32_t id = kNone;
    const auto range = layoutByHash_.equal_range(h);
    for (auto it = range.first; it != range.second; ++it) {
        const auto& have = layoutEls_[it->second];
        if (have.size() == els.size() && std::memcmp(have.data(), els.data(), els.size() * sizeof(d3d9::VertexElement)) == 0) {
            id = it->second;
            break;
        }
    }
    if (id == kNone) {
        if (layoutEls_.size() >= kLayoutCapacity) return kNone;
        id = static_cast<std::uint32_t>(layoutEls_.size());
        layoutEls_.push_back(els);
        layoutByHash_.emplace(h, id);
        layouts_.shadow()[id] = buildLayout(els);
        layouts_.mark(id);
        const LayoutRecord& r = layouts_.shadow()[id];
        std::string desc;
        for (const auto& e : els) {
            desc += std::format(" s{}+{}:t{}:u{}.{}", e.stream, e.offset, e.type, e.usage, e.usageIndex);
        }
        spdlog::info("native: mesh parts: layout {} = {} elements{} ({} not expressible)", id, els.size(), desc, r.info[3]);
    }
    layoutOfDecl_[decl->id()] = id;
    return id;
}

PartRecord MeshPartTable::makeRecord(const DrawGeometry& g, std::uint32_t layout, std::uint8_t& reason) {
    PartRecord r{};
    reason = kInArena;
    const auto note = [&](std::uint8_t why) { reason = std::max(reason, why); };
    const auto resolve = [&](d3d9::Buffer* b, std::uint32_t offset, std::uint32_t* pair) {
        if (b == nullptr) {
            note(kNoBuffer);
            return;
        }
        if (b->ringCopied()) {
            note(kRingCopied);
            return;
        }
        const rhi::GpuAddr a = b->arenaAddress();
        if (a.null()) {
            note(kNoAddress);
            return;
        }
        const auto w = a.ref.plus(offset).pair();
        pair[0] = w[0];
        pair[1] = w[1];
    };
    r.info[0] = 1;
    r.info[1] = layout;
    std::uint32_t flags = 0;
    resolve(g.ib, 0, &r.index[0]);
    r.index[2] = g.startIndex;
    r.index[3] = g.primCount * 3;
    const bool i32 = g.ib != nullptr && g.ib->index32();
    r.range[0] = static_cast<std::uint32_t>(g.baseVertex);
    r.range[1] = g.minIndex;
    r.range[2] = g.numVertices;
    r.range[3] = i32 ? 4u : 2u;
    if (i32) flags |= kPartIndex32;
    for (std::uint32_t i = 0; i < 3; ++i) {
        if (g.vb[i] == nullptr) continue;
        resolve(g.vb[i], g.offset[i], &r.stream[i][0]);
        r.stream[i][2] = g.stride[i];
        r.stream[i][3] = 1;
        flags |= 4u << i;
    }
    if (g.vb[0] == nullptr) note(kNoBuffer);
    if (reason == kInArena) flags |= kPartInArena;
    r.info[3] = flags;
    putPair(&r.material[0], reinterpret_cast<std::uintptr_t>(g.material));
    putPair(&r.material[2], reinterpret_cast<std::uintptr_t>(g.subMesh));
    putPair(&r.owner[0], reinterpret_cast<std::uintptr_t>(g.resource));
    return r;
}

void MeshPartTable::setIds(std::uint32_t slot, const DrawGeometry& g) {
    PartCpu& c = cpu_[slot];
    for (std::uint32_t i = 0; i < 4; ++i) {
        if (c.ids[i] == 0) continue;
        const auto range = byBuffer_.equal_range(c.ids[i]);
        for (auto it = range.first; it != range.second;) {
            if (it->second == slot) it = byBuffer_.erase(it);
            else ++it;
        }
    }
    c.ib = g.ib;
    c.vb = g.vb;
    c.offset = g.offset;
    c.stride = g.stride;
    c.base = g.baseVertex;
    c.ids = {g.ib != nullptr ? g.ib->id() : 0, g.vb[0] != nullptr ? g.vb[0]->id() : 0, g.vb[1] != nullptr ? g.vb[1]->id() : 0, g.vb[2] != nullptr ? g.vb[2]->id() : 0};
    for (std::uint32_t i = 0; i < 4; ++i) {
        if (c.ids[i] != 0) byBuffer_.emplace(c.ids[i], slot);
    }
}

void MeshPartTable::dropPart(std::uint32_t slot, bool byBuffer) {
    PartCpu& c = cpu_[slot];
    if (!c.live) return;
    for (std::uint32_t i = 0; i < 4; ++i) {
        if (c.ids[i] == 0) continue;
        const auto range = byBuffer_.equal_range(c.ids[i]);
        for (auto it = range.first; it != range.second;) {
            if (it->second == slot) it = byBuffer_.erase(it);
            else ++it;
        }
    }
    slotOf_.erase(c.key);
    c = {};
    std::memset(&parts_.shadow()[slot], 0, sizeof(PartRecord));
    parts_.mark(slot);
    freeSlots_.push_back(slot);
    --liveParts_;
    if (byBuffer) ++stats_.droppedByBuffer;
    else ++stats_.idleFreed;
}

void MeshPartTable::bufferChanged(std::uint64_t bufferId) {
    if (!active_) return;
    const std::lock_guard lock(mutex_);
    const auto range = byBuffer_.equal_range(bufferId);
    if (range.first == range.second) return;
    std::vector<std::uint32_t> slots;
    for (auto it = range.first; it != range.second; ++it) slots.push_back(it->second);
    for (const std::uint32_t s : slots) dropPart(s, true);
}

void MeshPartTable::checkDraw(d3d9::Device& dev, const DrawGeometry& desc, const DrawGeometry* bound, std::uint32_t cls) {
    if (!active_) return;
    const std::lock_guard lock(mutex_);
    ++stats_.draws;
    cls = std::min<std::uint32_t>(cls, 7);
    ++classDraws_[cls];
    if (!desc.indexed) {
        ++stats_.skippedUnindexed;
        return;
    }
    if (desc.subMesh == nullptr) {
        ++stats_.skippedNoSubMesh;
        return;
    }
    if (desc.ib == nullptr || desc.vb[0] == nullptr || desc.decl == nullptr) {
        ++stats_.skippedNoBuffer;
        return;
    }
    if (!observing_) {
        dev.addDrawObserver(this);
        observing_ = true;
    }
    if (cpu_.empty()) {
        cpu_.resize(kPartCapacity);
        parts_.init(kPartCapacity);
        layouts_.init(kLayoutCapacity);
    }
    const DrawGeometry& actual = bound != nullptr ? *bound : desc;
    if (bound == nullptr) ++stats_.replayed;
    const std::uint32_t layoutActual = layoutFor(actual.decl);
    if (layoutActual == kNone) {
        ++stats_.overflow;
        return;
    }
    std::uint8_t reasonActual = 0;
    PartRecord cur = makeRecord(actual, layoutActual, reasonActual);
    const Key key{desc.subMesh, desc.startIndex, desc.primCount};
    std::uint32_t slot = kNone;
    if (const auto it = slotOf_.find(key); it != slotOf_.end()) {
        slot = it->second;
    } else {
        if (!freeSlots_.empty()) {
            slot = freeSlots_.back();
            freeSlots_.pop_back();
        } else if (used_ < kPartCapacity) {
            slot = used_++;
        } else {
            ++stats_.overflow;
            return;
        }
        const std::uint32_t layoutDesc = layoutFor(desc.decl);
        std::uint8_t reason = 0;
        PartRecord rec = makeRecord(desc, layoutDesc, reason);
        parts_.shadow()[slot] = rec;
        parts_.mark(slot);
        PartCpu& c = cpu_[slot];
        c = {};
        c.key = key;
        c.live = true;
        c.cls = static_cast<std::uint8_t>(cls);
        c.reason = reason;
        c.seen = frame_;
        slotOf_[key] = slot;
        setIds(slot, desc);
        ++liveParts_;
        peakParts_ = std::max(peakParts_, liveParts_);
        ++stats_.created;
        ++classReason_[cls][reason];
    }
    PartCpu& c = cpu_[slot];
    c.seen = frame_;
    PartRecord& rec = parts_.shadow()[slot];
    PartRecord a = rec, b = cur;
    a.info[2] = b.info[2] = 0;
    ++stats_.checked;
    if (bound != nullptr) ++stats_.checkedBound;
    if (std::memcmp(&a, &b, sizeof(a)) == 0) return;
    // the same part, only its addresses were not resolvable when it was created (the buffer had no GPU copy yet): not a difference
    PartRecord a2 = a;
    std::memcpy(a2.index, b.index, 8);
    for (int i = 0; i < 3; ++i) std::memcpy(a2.stream[i], b.stream[i], 8);
    a2.info[3] = b.info[3];
    const bool late = (a.info[3] & kPartInArena) == 0 && std::memcmp(&a2, &b, sizeof(a2)) == 0;
    if (late) {
        ++stats_.lateResolved;
    } else {
        ++stats_.differing;
        if (differingLogged_ < 8) {
            ++differingLogged_;
            spdlog::error("native: mesh parts: DIFFERING draw: sub-mesh {} first {} prims {} slot {} (class {}): record layout {} flags {:#x} index {:#x}{:08x} first {} count {} "
                          "base {} min {} n {} isize {} s0 {:#x}{:08x}+{} s1 {:#x}{:08x}+{} mat {:#x}{:08x} | draw: layout {} flags {:#x} index {:#x}{:08x} first {} count {} "
                          "base {} min {} n {} isize {} s0 {:#x}{:08x}+{} s1 {:#x}{:08x}+{} mat {:#x}{:08x}{}",
                          desc.subMesh, desc.startIndex, desc.primCount, slot, kClassNames[cls], a.info[1], a.info[3], a.index[1], a.index[0], a.index[2], a.index[3],
                          static_cast<std::int32_t>(a.range[0]), a.range[1], a.range[2], a.range[3], a.stream[0][1], a.stream[0][0], a.stream[0][2], a.stream[1][1],
                          a.stream[1][0], a.stream[1][2], a.material[1], a.material[0], b.info[1], b.info[3], b.index[1], b.index[0], b.index[2], b.index[3],
                          static_cast<std::int32_t>(b.range[0]), b.range[1], b.range[2], b.range[3], b.stream[0][1], b.stream[0][0], b.stream[0][2], b.stream[1][1],
                          b.stream[1][0], b.stream[1][2], b.material[1], b.material[0], bound != nullptr ? "" : " (replayed: the draw's own description)");
        }
    }
    cur.info[2] = rec.info[2] + 1;
    rec = cur;
    parts_.mark(slot);
    setIds(slot, actual);
    c.reason = reasonActual;
}

void MeshPartTable::flush(d3d9::Device& dev, bool verify) {
    ++frame_;
    if (!active_) return;
    const std::lock_guard lock(mutex_);
    if (used_ == 0) return;
    const std::uint32_t nLayouts = static_cast<std::uint32_t>(layoutEls_.size());
    if (!gpuFailed_ && (!parts_.ready() || !layouts_.ready())) {
        if (!layouts_.ready() && !layouts_.create(dev, kLayoutCapacity, "mesh layouts")) gpuFailed_ = true;
        else if (!parts_.ready() && !parts_.create(dev, kPartCapacity, "mesh parts")) gpuFailed_ = true;
        if (gpuFailed_) {
            spdlog::warn("native: mesh parts: the GPU buffers could not be created (the CPU records and the draw check keep running)");
        } else {
            layouts_.markAll(nLayouts);
            parts_.markAll(used_);
            // the fetch test's pipeline in the background
            if (rhi::Device* rdev = dev.rhi(); rdev != nullptr && !pipeline_) {
                pipeline_ = std::make_unique<FetchPipeline>();
                pipeline_->building = std::async(std::launch::async, [rdev]() {
                    rhi::ComputePipelineDesc d;
                    d.cs.spirv = shader::embedded::spirv("test/mesh_fetch.comp");
                    d.cs.dxil = shader::embedded::dxilOnDemand("test/mesh_fetch.comp");
                    d.name = "mesh parts fetch test";
                    return rdev->createComputePipeline(d);
                });
            }
        }
    }
    if (frame_ % 300 == 0) {
        for (std::uint32_t s = 0; s < used_; ++s) {
            if (cpu_[s].live && frame_ - cpu_[s].seen >= kIdleFrames) dropPart(s, false);
        }
    }
    if (!gpuFailed_) {
        const bool verifyNow = verify && frame_ % kVerifyEvery == 0;
        layouts_.flush(dev, verifyNow, nLayouts);
        parts_.flush(dev, verifyNow, used_);
        layouts_.collect(dev.game(), "mesh layouts");
        parts_.collect(dev.game(), "mesh parts");
        collectFetch(dev);
        if (verifyNow) runFetchTest(dev);
    }
    report();
}

void MeshPartTable::runFetchTest(d3d9::Device& dev) {
    if (!pipeline_ || pipeline_->failed) return;
    if (!pipeline_->pipe) {
        if (!pipeline_->building.valid()) return;
        if (pipeline_->building.wait_for(std::chrono::seconds(0)) != std::future_status::ready) return;
        pipeline_->pipe = pipeline_->building.get();
        if (!pipeline_->pipe) {
            pipeline_->failed = true;
            spdlog::warn("native: mesh parts: the fetch test's pipeline could not be built");
            return;
        }
    }
    rhi::GameDevice& game = dev.game();
    Fetch& f = fetch_[game.frameSlot() % rhi::kFramesInFlight];
    if (f.busy) return;
    // candidates: live parts drawn recently whose buffers are in the arena
    std::vector<std::uint32_t> cand;
    for (std::uint32_t s = 0; s < used_; ++s) {
        const PartCpu& c = cpu_[s];
        if (!c.live || frame_ - c.seen > 60) continue;
        const PartRecord& r = parts_.shadow()[s];
        if ((r.info[3] & kPartInArena) == 0 || r.range[2] == 0 || r.index[3] == 0) continue;
        cand.push_back(s);
    }
    if (cand.empty()) return;
    std::uint32_t seed = static_cast<std::uint32_t>(frame_ * 2654435761u) | 1u;
    const auto rnd = [&seed]() {
        seed = seed * 1664525u + 1013904223u;
        return seed >> 8;
    };
    rhi::RingAlloc a = game.allocRing(static_cast<std::uint64_t>(kFetchCases) * 16, 16);
    if (a.ptr == nullptr) return;
    auto* dst = static_cast<std::uint32_t*>(a.ptr);
    f.expect.clear();
    for (std::uint32_t n = 0; n < kFetchCases * 2 && f.expect.size() < kFetchCases; ++n) {
        const std::uint32_t slot = cand[rnd() % cand.size()];
        const PartCpu& c = cpu_[slot];
        const PartRecord& r = parts_.shadow()[slot];
        Fetch::Expect e;
        e.slot = slot;
        e.k = rnd() % r.index[3];
        const std::uint32_t isz = r.range[3];
        std::uint32_t idx = 0;
        if (!c.ib->readCpu((r.index[2] + e.k) * isz, isz, &idx)) {
            ++stats_.fetchSkipped;
            continue;
        }
        if (isz == 2) idx &= 0xffffu;
        e.index = idx;
        e.vertex = (rnd() & 1) != 0 ? idx : r.range[1] + rnd() % r.range[2];
        const LayoutRecord& lay = layouts_.shadow()[r.info[1]];
        bool ok = true;
        const auto fetch = [&](std::uint32_t semantic, float out[4]) {
            out[0] = out[1] = out[2] = out[3] = 0.0f;
            const std::uint32_t w = lay.slot[semantic];
            if (w == 0) return;
            const std::uint32_t stream = slotStream(w);
            if (stream > 2 || c.vb[stream] == nullptr) return;
            const std::uint32_t type = slotType(w), bytes = declTypeBytes(type);
            std::uint8_t raw[16]{};
            const std::uint32_t at = c.offset[stream] + static_cast<std::uint32_t>(static_cast<std::int64_t>(e.vertex) + c.base) * c.stride[stream] + slotOffset(w);
            if (!c.vb[stream]->readCpu(at, bytes, raw)) {
                ok = false;
                return;
            }
            decodeElement(type, raw, out);
        };
        fetch(kSlotPosition, e.pos);
        fetch(kSlotNormal, e.nrm);
        fetch(kSlotTexcoord0, e.uv);
        fetch(kSlotColor0, e.col);
        if (!ok) {
            ++stats_.fetchSkipped;
            continue;
        }
        // (the shader writes w = 1 for position / normal, (xy, 0, 1) for uv; colour as is, float4(0) when absent)
        e.pos[3] = 1.0f;
        e.nrm[3] = 1.0f;
        e.uv[2] = 0.0f;
        e.uv[3] = 1.0f;
        if (lay.slot[kSlotPosition] == 0) e.pos[0] = e.pos[1] = e.pos[2] = 0.0f;
        const std::size_t i = f.expect.size();
        dst[i * 4 + 0] = slot;
        dst[i * 4 + 1] = e.vertex;
        dst[i * 4 + 2] = e.k;
        dst[i * 4 + 3] = 0;
        f.expect.push_back(e);
    }
    if (f.expect.empty()) return;
    f.count = static_cast<std::uint32_t>(f.expect.size());
    rhi::Device* rdev = dev.rhi();
    const std::uint64_t outBytes = static_cast<std::uint64_t>(kFetchCases) * 80;
    if (!f.out) f.out = rdev->createBuffer({outBytes, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT, rhi::Memory::Device, "mesh parts fetch out"});
    if (!f.readback) f.readback = rdev->createBuffer({outBytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT, rhi::Memory::Readback, "mesh parts fetch readback"});
    if (!f.out || !f.readback) return;
    struct Push {
        std::uint32_t parts[2], layouts[2], cases[2], dst[2];
        std::uint32_t count, pad[3];
    } push{};
    const auto pr = parts_.ref().pair();
    const auto lr = layouts_.ref().pair();
    const auto cr = a.ref.pair();
    const auto dr = f.out->shaderRefRw(0);
    push.parts[0] = pr[0];
    push.parts[1] = pr[1];
    push.layouts[0] = lr[0];
    push.layouts[1] = lr[1];
    push.cases[0] = cr[0];
    push.cases[1] = cr[1];
    push.dst[0] = dr[0];
    push.dst[1] = dr[1];
    push.count = f.count;
    static_assert(sizeof(Push) == 48);
    rhi::CommandList& list = dev.uploadList();
    // the arena's copies of this frame (recorded earlier into this list) are complete before the shader reads them
    list.memoryBarrier(VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                       VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_READ_BIT);
    list.bufferBarrier(*f.out, rhi::BufferAccess::ShaderWrite);
    list.bindPipeline(*pipeline_->pipe);
    list.setPushConstants(&push, sizeof(push));
    list.dispatch((f.count + 63) / 64, 1, 1);
    list.bufferBarrier(*f.out, rhi::BufferAccess::ShaderRead);
    list.copyBuffer(*f.readback, 0, *f.out, 0, static_cast<std::uint64_t>(f.count) * 80);
    f.frameValue = game.frameValue();
    f.busy = true;
}

void MeshPartTable::collectFetch(d3d9::Device& dev) {
    rhi::GameDevice& game = dev.game();
    for (Fetch& f : fetch_) {
        if (!f.busy || game.completedFrame() < f.frameValue) continue;
        f.busy = false;
        f.readback->invalidate();
        const auto* got = static_cast<const std::uint32_t*>(f.readback->map());
        std::uint64_t bad = 0;
        const auto close = [](float a, float b) { return std::fabs(a - b) <= 1e-5f * std::max(1.0f, std::fabs(b)); };
        for (std::size_t i = 0; i < f.expect.size(); ++i) {
            const Fetch::Expect& e = f.expect[i];
            const std::uint32_t* w = got + i * 20;
            float v[16];
            std::memcpy(v, w + 4, sizeof(v));
            bool same = w[0] == e.index && w[1] == 1;
            for (int k = 0; k < 4; ++k) same = same && close(v[k], e.pos[k]) && close(v[4 + k], e.nrm[k]) && close(v[8 + k], e.uv[k]) && close(v[12 + k], e.col[k]);
            ++stats_.fetchCases;
            if (same) continue;
            ++bad;
            if (fetchLogged_ < 8) {
                ++fetchLogged_;
                spdlog::error("native: mesh parts: fetch DIFFERING: part {} vertex {} k {}: index gpu {} cpu {}; pos gpu ({:g} {:g} {:g}) cpu ({:g} {:g} {:g}); nrm gpu ({:g} {:g} {:g}) cpu "
                              "({:g} {:g} {:g}); uv gpu ({:g} {:g}) cpu ({:g} {:g}); col gpu ({:g} {:g} {:g} {:g}) cpu ({:g} {:g} {:g} {:g})",
                              e.slot, e.vertex, e.k, w[0], e.index, v[0], v[1], v[2], e.pos[0], e.pos[1], e.pos[2], v[4], v[5], v[6], e.nrm[0], e.nrm[1], e.nrm[2], v[8], v[9],
                              e.uv[0], e.uv[1], v[12], v[13], v[14], v[15], e.col[0], e.col[1], e.col[2], e.col[3]);
            }
        }
        ++stats_.fetchTests;
        stats_.fetchDiffering += bad;
        f.expect.clear();
    }
}

void MeshPartTable::report() {
    const double now = std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
    if (now - lastReport_ < 5.0) return;
    lastReport_ = now;
    const auto& ps = parts_.stats();
    const auto& ls = layouts_.stats();
    const std::uint64_t bytes = ps.bytes + ls.bytes, slots = ps.slotsUploaded + ls.slotsUploaded;
    const std::uint64_t frames = std::max<std::uint64_t>(frame_ - lastFrame_, 1);
    std::array<std::uint64_t, kReasons> byReason{};
    std::array<std::array<std::uint64_t, kReasons>, 8> byClass{};
    for (std::uint32_t s = 0; s < used_; ++s) {
        if (!cpu_[s].live) continue;
        ++byReason[cpu_[s].reason];
        ++byClass[cpu_[s].cls][cpu_[s].reason];
    }
    std::string outside;
    for (std::uint32_t c = 0; c < 8; ++c) {
        for (std::uint32_t r = 1; r < kReasons; ++r) {
            if (byClass[c][r] != 0) outside += std::format(" {} {}: {};", kClassNames[c], kReasonNames[r], byClass[c][r]);
        }
    }
    spdlog::info(
        "native: mesh parts (5 s): {} parts (peak {}, slots {}; in arena {}, outside {} = ring-copied {}, no arena address {}, no buffer {}{}), {} layouts; draws {} checked {} "
        "(against the device bindings {}, replayed {}) DIFFERING {}, late address {}; created {}, dropped by buffer {}, idle {}; skipped: no sub-mesh {}, unindexed {}, no buffer {}; "
        "{:.2f} records / {:.2f} KiB per frame; verify: parts {} read-backs ({} slots) DIFFERING {}, layouts {} DIFFERING {}; fetch test: {} runs, {} cases, {} skipped, DIFFERING {}; "
        "overflow {} gpu {}",
        liveParts_, peakParts_, used_, byReason[0], liveParts_ - static_cast<std::uint32_t>(byReason[0]), byReason[kRingCopied], byReason[kNoAddress], byReason[kNoBuffer],
        outside.empty() ? std::string() : " [" + outside + " ]", layoutEls_.size(), stats_.draws, stats_.checked, stats_.checkedBound, stats_.replayed, stats_.differing,
        stats_.lateResolved, stats_.created, stats_.droppedByBuffer, stats_.idleFreed, stats_.skippedNoSubMesh, stats_.skippedUnindexed, stats_.skippedNoBuffer,
        static_cast<double>(slots - lastSlots_) / static_cast<double>(frames), static_cast<double>(bytes - lastBytes_) / 1024.0 / static_cast<double>(frames), ps.verifyDone,
        ps.verifySlots, ps.verifyBad, ls.verifyDone, ls.verifyBad, stats_.fetchTests, stats_.fetchCases, stats_.fetchSkipped, stats_.fetchDiffering, stats_.overflow,
        ps.overflow + ls.overflow);
    lastFrame_ = frame_;
    lastBytes_ = bytes;
    lastSlots_ = slots;
}

}  // namespace rvk::gpuscene
