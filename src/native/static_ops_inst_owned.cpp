// F2f owned instance lists (docs/research/frame-ownership.md 14.19, setting native.owned_instances): see static_ops.h.
//
// A render-instance op (render-op node 0xffffff, solid list) draws its mesh resource's instance list: `res +0xb8` instances of 0x58 bytes at `res +0xb0` (matrix +0x00, switch
// +0x40, illumination +0x48, flags +0x50), sorted once per frame by eCRenderContextMesh::Render while `res +0xa4` is set. With the setting on:
//   * the hardware batches (res +0xa0, >= 2 instances, no lightmaps) are composed like the engine's own fallback, one draw per instance (composeSolid), so every such op is
//     an instance list;
//   * the op's stamp holds the list as the composition reads it (instanceStamp: the sorted copy, the instances' bytes and their illuminations' pass-structure inputs), so the
//     record is valid exactly while the list is the same;
//   * each draw's VS window 2 (c32 .w = 1, c33..c35 = the instance's world rows) is an image in the instance row table (gpuscene/row_table.h), written once from the composed
//     registers when the record is built; the composed draws of a valid record are served from the same images (instanceServeComposed), so the check frames compare like for
//     like;
//   * the owned frames run the slot's side effects (BindLightmaps, the touches, the render-instance sort).

#include <windows.h>

#include <algorithm>
#include <chrono>
#include <cstring>
#include <format>
#include <vector>

#include <spdlog/spdlog.h>

#include "engine/memread.h"
#include "engine/symbols.h"
#include "gpuscene/object_compose.h"
#include "gpuscene/row_table.h"
#include "native/static_ops.h"

namespace rvk::native {

namespace {

using engine::readAt;
using U8 = const std::uint8_t;

template <class T>
inline T get(const void* p, std::size_t off, T fallback = T{}) {
    T v = fallback;
    return readAt(p, off, v) ? v : fallback;
}
inline U8* ptr(const void* p, std::size_t off) { return get<U8*>(p, off, nullptr); }

inline std::uint64_t mix(std::uint64_t h, std::uint64_t v) {
    h ^= v + 0x9E3779B97F4A7C15ull + (h << 6) + (h >> 2);
    return h * 0xff51afd7ed558ccdull;
}

constexpr std::int32_t kMaxInstances = 512;  // (an op with more instances stays composed: counted as `instance list`)
constexpr std::size_t kInstanceBytes = 0x58;

using ThisFn = void (*)(const void*);

// the resource's instance list (n records) into `out`; false = unreadable
bool readInstances(const std::uint8_t* res, std::int32_t n, std::vector<std::uint8_t>& out) {
    out.resize(static_cast<std::size_t>(n) * kInstanceBytes);
    U8* list = ptr(res, 0xb0);
    return list != nullptr && engine::safeRead(list, out.data(), out.size());
}

}  // namespace

void StaticOps::setOwnedInstances(bool on) {
    if (on == ownedInstances_) return;
    ownedInstances_ = on;
    if (!on && recs_) {
        for (auto it = recs_->map.begin(); it != recs_->map.end();) {
            if (it->second->instances) it = recs_->map.erase(it);
            else ++it;
        }
    }
    spdlog::info("native: owned instances {}", on ? "on" : "off");
}

bool StaticOps::instanceUnbatch() const {
    return ownedInstancesOn() && viewKind_ == ViewKind::Main && g_ == &groups_[static_cast<std::size_t>(Group::Static)];
}

orec::Excl StaticOps::instanceExclusion(const std::uint8_t* res) {
    const std::int32_t n = get<std::int32_t>(res, 0xb8);
    if (n <= 0) return orec::Excl::NotComposed;
    if (n > kMaxInstances) return orec::Excl::InstanceList;
    static thread_local std::vector<std::uint8_t> inst;
    if (!readInstances(res, n, inst)) return orec::Excl::NotComposed;
    for (std::int32_t i = 0; i < n; ++i) {
        U8* at = inst.data() + kInstanceBytes * static_cast<std::size_t>(i);
        if (at[0x50] != 0) return orec::Excl::Overlay;  // (burned / frozen / highlighted instance: overlay passes)
        U8* illum = nullptr;
        std::memcpy(&illum, at + 0x48, 8);
        if (illum == nullptr || ptr(illum, 0) == nullptr) return orec::Excl::NoList;
    }
    return orec::Excl::Count;
}

std::uint64_t StaticOps::instanceStamp(const std::uint8_t* res) {
    const std::int32_t n = get<std::int32_t>(res, 0xb8);
    std::uint64_t h = mix(0x696e7374ull, static_cast<std::uint32_t>(n));
    if (n <= 0 || n > kMaxInstances) return h;
    static thread_local std::vector<std::uint8_t> inst;
    if (!readInstances(res, n, inst)) return mix(h, 0xdeadull);
    // (the order the composition will draw: eCRenderContextMesh::Render sorts the list in place while res +0xa4 is set; the engine's qsort with its comparator is
    // deterministic, so the sorted copy here is the order the in-place sort makes)
    if (get<std::int32_t>(res, 0xa4) != 0 && n > 1 && qsort_ != nullptr && instanceCmp_ != nullptr) qsort_(inst.data(), static_cast<std::size_t>(n), kInstanceBytes, instanceCmp_);
    for (std::int32_t i = 0; i < n; ++i) {
        U8* at = inst.data() + kInstanceBytes * static_cast<std::size_t>(i);
        std::uint64_t w[10];
        std::memcpy(w, at, sizeof(w));  // matrix +0x00, switch +0x40, +0x44, illumination +0x48
        for (const std::uint64_t v : w) h = mix(h, v);
        h = mix(h, at[0x50]);
        U8* illum = nullptr;
        std::memcpy(&illum, at + 0x48, 8);
        U8* list = ptr(illum, 0);
        // (the illumination's pass-structure inputs, as the plain ops' stamp reads them from the entity's: the list, its sun bit, no-sun, static / dynamic light counts)
        h = mix(mix(h, reinterpret_cast<std::uintptr_t>(list)),
                get<std::uint8_t>(list, 0) | (get<std::uint8_t>(illum, 0xc) << 8) | (get<std::uint8_t>(illum, 0xd) << 16) | (static_cast<std::uint32_t>(get<std::uint8_t>(list, 0x34)) << 24));
    }
    ++instW_.ops;
    return h;
}

void StaticOps::instanceServeComposed(const sc::DrawDesc& d) {
    float regs[gpuscene::kObjectRegs][4];
    std::memcpy(regs, d.vsF[gpuscene::kObjectFirstReg], sizeof(regs));
    const std::uint32_t bits = gpuscene::objectComposedBits(d.vsSet.data());
    const orec::Store* R = recs_.get();
    const orec::OpRecord* rec = R != nullptr && R->active && R->cur != nullptr && R->cur->instances && op_ != nullptr && R->key.ctx == op_->raw.context && R->key.desc == op_->raw.desc
                                    ? R->cur
                                    : nullptr;
    if (rec != nullptr && ownedDrawIndex_ < rec->draws.size()) {
        const orec::DrawRec& r = rec->draws[ownedDrawIndex_];
        d3d9::OwnedWindows::Win w;
        if (r.row.has() && r.row.bits == bits && gpuscene::rowtable().same(r.row.slot, regs, bits) && gpuscene::rowtable().windowFor(r.row.slot, bits, w)) {
            ownedWin_.w[0][2] = w;
            ++instW_.served;
            return;
        }
    }
    ++instW_.notServed;  // (the record is being built: the draw keeps its uploaded window 2)
}

bool StaticOps::instanceRows(orec::OpRecord& rec) {
    for (std::size_t k = 0; k < rec.draws.size() && k < drawCount_; ++k) {
        const sc::DrawDesc& d = *draws_[k];
        float regs[gpuscene::kObjectRegs][4];
        std::memcpy(regs, d.vsF[gpuscene::kObjectFirstReg], sizeof(regs));
        const std::uint32_t bits = gpuscene::objectComposedBits(d.vsSet.data());
        const std::uint32_t slot = gpuscene::rowtable().alloc(regs, bits);
        if (slot == gpuscene::kNoSlot) {
            ++instW_.rowsRefused;
            return false;
        }
        rec.draws[k].row.slot = slot;
        rec.draws[k].row.bits = bits;
    }
    ++instW_.recordsBuilt;
    return true;
}

void StaticOps::instanceSideEffects(const orec::OpRecord& rec) {
    // eCRenderContextMesh::Render 0x5c7110: once per frame per resource, the render instances sorted in place, then +0xa4 = 0 (what leanSortInstances does on lean frames)
    U8* res = static_cast<U8*>(rec.lightmapRes);
    if (res == nullptr || get<std::int32_t>(res, 0xa4) == 0 || engine::sym().meshSortRenderInstances == nullptr) return;
    reinterpret_cast<ThisFn>(engine::sym().meshSortRenderInstances)(res);
    const std::int32_t zero = 0;
    engine::safeWrite(const_cast<std::uint8_t*>(res) + 0xa4, &zero, sizeof(zero));
    ++instW_.sorts;
}

void StaticOps::instReport() {
    InstStats& w = instW_;
    if (ownedInstancesOn()) {
        std::uint64_t records = 0, draws = 0;
        if (recs_) {
            for (const auto& kv : recs_->map) {
                if (!kv.second->instances) continue;
                ++records;
                draws += kv.second->draws.size();
            }
        }
        spdlog::info("native: owned instances (5 s): stamps read {}, records built {} ({} live, {} draws), owned ops {} / draws {}, hardware batches composed per instance {} ops / {} instances, "
                     "composed draws served from their row image {} (uploaded while built {}), row images refused {}, render-instance sorts by the owned path {}; stamp CPU {:.3f} ms per 5 s",
                     w.ops, w.recordsBuilt, records, draws, w.ownedOps, w.ownedDraws, w.unbatchedOps, w.unbatchedInstances, w.served, w.notServed, w.rowsRefused, w.sorts,
                     static_cast<double>(w.stampNs) / 1.0e6);
    }
    w = {};
}

}  // namespace rvk::native
