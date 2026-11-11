// F2e-1 step A: event-driven validity of the static op records (docs/research/frame-ownership.md 7.30): see static_ops.h.
//
// What an owned op had to prove every frame (recStamp: ~35 guarded engine words, a hash chain, firstDifference) is split by what can change it:
//   events   the resource cache's transitions, OnLightmapChanged, SetShader (engine/dep_events.*):
//            the records that registered the object (the DepIndex) are marked dirty; a marked record is verified with the full stamp at its next look
//   polled   per op (a few words: the LOD resource ctx +0x28, the switch value, the entity's overlay flags, the illumination (6 words), the fade, the scene-table slot, our own
//            epochs) or per shader and frame (the property fields of the covered shaders, memoised)
// The full stamp stays the net: the check frames (recEnter), a rolling scrub of 1 in 16 owned ops. A difference nobody announced is an EVENT MISS.

#include <windows.h>

#include <algorithm>
#include <bit>
#include <cstring>
#include <format>

#include <spdlog/spdlog.h>

#include "engine/dep_events.h"
#include "engine/memread.h"
#include "engine/scene_table.h"
#include "enh/light_table.h"
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

bool envOff() {
    static const bool off = [] {
        char b[8];
        return GetEnvironmentVariableA("RVK_F2E_NO_EVENTS", b, sizeof(b)) != 0;
    }();
    return off;
}

}  // namespace

bool StaticOps::depEvents() const { return engine::depevents::active() && !envOff(); }

// the fields of a shader object the stamp reads (static_ops_records.cpp recStamp): its class, blend mode, flags, sort id, depth bias, admin
std::uint64_t StaticOps::shaderFp(const std::uint8_t* sh) {
    const std::uint64_t frame = dev_.stats().frames;
    ShaderMemo& m = shaderMemo_[(reinterpret_cast<std::uintptr_t>(sh) >> 4) & 255u];
    if (m.p == sh && m.frame == frame) {
        ++depW_.shaderHits;
        return m.fp;
    }
    ++depW_.shaderPolls;
    std::uint64_t h = mix(0, reinterpret_cast<std::uintptr_t>(ptr(sh, 0)));
    h = mix(h, static_cast<std::uint32_t>(get<std::int32_t>(sh, 0x50)));
    h = mix(h, get<std::uint8_t>(sh, 0x58) | (get<std::uint8_t>(sh, 0x79) << 8) | (get<std::uint8_t>(sh, 0x330) << 16) | (static_cast<std::uint64_t>(get<std::uint8_t>(sh, 0x420)) << 24) |
                   (static_cast<std::uint64_t>(get<std::uint8_t>(sh, 0x2d8)) << 32) | (static_cast<std::uint64_t>(get<std::uint8_t>(sh, 0x78)) << 40) |
                   (static_cast<std::uint64_t>(get<std::uint8_t>(sh, 0x2b8)) << 48) | (static_cast<std::uint64_t>(get<std::uint8_t>(sh, 0x2a8)) << 56));
    h = mix(mix(h, static_cast<std::uint32_t>(get<std::int32_t>(sh, 0x68))), reinterpret_cast<std::uintptr_t>(ptr(sh, 0x220)));
    m = {sh, frame, h};
    return h;
}

void StaticOps::depDrain() {
    if (!depEvents()) return;
    engine::depevents::drain(depDrained_);
    if (depDrained_.empty()) return;
    orec::DepIndex& idx = orec::DepIndex::get();
    depW_.objects += depDrained_.size();
    for (const void* o : depDrained_) depW_.marked += idx.markDirty(o);
}

void StaticOps::depRegister(orec::OpRecord& rec, const Op& op) {
    if (!depEvents()) return;
    orec::DepIndex& idx = orec::DepIndex::get();
    const auto add = [&](const void* o) {
        if (o == nullptr || std::find(rec.deps.begin(), rec.deps.end(), o) != rec.deps.end()) return;
        rec.deps.push_back(o);
        idx.add(o, &rec);
    };
    U8* ctx = static_cast<U8*>(op.raw.context);
    U8* res = ptr(ctx, 0x28);
    add(res);
    add(ptr(res, 0x90));
    add(ptr(res, 0x98));
    const std::uint32_t sub = op.raw.desc & 0xffu;
    U8* subs = ptr(res, 0x58);
    const std::int32_t count = get<std::int32_t>(res, 0x60);
    const std::int32_t first = sub == 0xffu ? 0 : static_cast<std::int32_t>(sub);
    const std::int32_t last = sub == 0xffu ? count : std::min<std::int32_t>(static_cast<std::int32_t>(sub) + 1, count);
    for (std::int32_t i = first; i < last && i < 64 && subs != nullptr; ++i) {
        U8* m = ptr(subs + 0x48 * static_cast<std::size_t>(i), 0x38);
        U8* sh = ptr(m, 0x70);
        add(m);
        if (sh != nullptr) {
            const bool known = std::any_of(rec.shaders.begin(), rec.shaders.end(), [&](const auto& s) { return s.first == sh; });
            if (!known) rec.shaders.emplace_back(sh, shaderFp(sh));
        }
    }
    rec.dirty = false;
}

bool StaticOps::depClean(const orec::OpRecord& rec) const { return !rec.dirty; }

void StaticOps::depVerified(const orec::OpRecord& rec) {
    rec.dirty = false;
}

bool StaticOps::depShadersChanged(const orec::OpRecord& rec) {
    for (const auto& s : rec.shaders) {
        if (shaderFp(static_cast<U8*>(const_cast<void*>(s.first))) != s.second) return true;
    }
    return false;
}

// -1 = valid without reading the engine beyond the polled words; -2 = verify the full stamp; else the stamp field (kStampNames) that differs
int StaticOps::depFast(const Op& op, const orec::OpRecord& rec) {
    if (!depClean(rec)) return -2;
    const orec::Stamp& s = rec.stamp;
    U8* ctx = static_cast<U8*>(op.raw.context);
    if (ptr(ctx, 0x28) != s.res) return 0;
    U8* entity = ptr(ctx, 0x10);
    if (entity != s.entity) return 1;
    if ((get<std::uint32_t>(entity, 0x28) & 0xe00000u) != s.entityFlags) return 5;
    U8* illumPs = ptr(entity, 0xf8);
    if (illumPs != s.illumPs) return 6;
    {
        // the illumination's pass-structure inputs (recStamp's `light`): polled, 6 words (the light lists are shared by hundreds of records and rewritten by set updates about
        // every frame: an event per set / PS would mark them all)
        U8* illum = illumPs != nullptr ? illumPs + 0x78 : nullptr;
        U8* list = ptr(illum, 0);
        const std::uint64_t light = mix(mix(mix(mix(0, reinterpret_cast<std::uintptr_t>(illum)), reinterpret_cast<std::uintptr_t>(list)),
                                            get<std::uint8_t>(list, 0) | (get<std::uint8_t>(illum, 0xc) << 8) | (get<std::uint8_t>(illum, 0xd) << 16)),
                                        get<std::uint8_t>(list, 0x34));
        if (light != s.light) return 3;
    }
    if (std::bit_cast<std::uint32_t>(op.raw.fade) != s.fadeBits) return 7;
    if (get<std::int32_t>(ptr(ctx, 0x18), 0x20) != s.switchValue) return 8;
    // the scene-table slot the record was made for: still the same entity at the same generation (a direct index: no hash)
    if (s.objSlot == 0xffffffffu) {
        if (engine::scenetable::find(entity) != 0xffffffffu) return 9;
    } else {
        const engine::scenetable::Slot* sl = engine::scenetable::slot(s.objSlot);
        if (sl == nullptr || sl->entity != entity || sl->generation != s.objGen) return 9;
    }
    if (s.variantEpoch != dev_.variantEpoch()) return 10;
    if (s.registryGen != (enh::fullLitSkipsReads() ? enh::LightTable::registryGeneration() : 0)) return 11;
    for (const auto& sh : rec.shaders) {
        if (shaderFp(static_cast<U8*>(const_cast<void*>(sh.first))) != sh.second) {
            rec.dirty = true;  // (polled: the full stamp decides at the next look)
            ++depW_.polledLeft;
            return 2;
        }
    }
    return -1;
}

void StaticOps::depEventMiss(const Op& op, const orec::OpRecord& rec, int field, const char* where) {
    ++depW_.eventMiss;
    rec.dirty = true;
    if (depEventMissLogged_ < 16) {
        ++depEventMissLogged_;
        spdlog::error("native: owned records: EVENT MISS ({}): {} differs in the stamp ({}) while no engine event announced it ({})", where, describe(op),
                      field >= 0 && field < static_cast<int>(orec::kStampFields) ? orec::kStampNames[field] : "?", rec.name);
    }
}

// recEnter's full compare found `field` differing: an input an event or a poll should have announced = an event miss (the fields of the polled class are expected differences)
bool StaticOps::depNoteCheckFrame(const Op& op, const orec::OpRecord& rec, int field) {
    if (!depEvents()) return false;
    ++depW_.checkSeen;
    const bool eventClass = field == 2 || field == 4;  // (geometry / material, lightmaps; the illumination is polled)
    if (!eventClass || rec.dirty) return false;
    if (field == 2 && depShadersChanged(rec)) return false;  // (a shader property: polled, not an event)
    depEventMiss(op, rec, field, "full stamp");
    return true;
}

void StaticOps::depReport() {
    depT_.add(depW_);
    if (depEvents()) {
        const auto& c = engine::depevents::counters();
        spdlog::info("native: owned events (5 s): select fast {} / slow {} (slow = marked by an event: {} then held, {} rebuilt by the stamp), scrub {} full compares, polled-shader changes {}, shader polls {} / memo hits {}, "
                     "QueueData calls that changed nothing {}, objects drained {}, records marked {}, EVENT MISS {} (run {}), check-frame compares of event-class fields {}; engine events (run): LoadData {} QueueData {} Flush {} lightmap {} SetShader {}; "
                     "dependency index {} objects",
                     depW_.fast, depW_.slow, depW_.slowClean, depW_.slow - depW_.slowClean, depW_.scrubbed, depW_.polledLeft, depW_.shaderPolls, depW_.shaderHits, engine::depevents::counters().queueUnchanged, depW_.objects, depW_.marked, depW_.eventMiss, depT_.eventMiss,
                     depW_.checkSeen, c.loadData, c.queueData, c.flush, c.lightmap, c.setShader, orec::DepIndex::get().objects());
    }
    depW_ = {};
}

}  // namespace rvk::native
