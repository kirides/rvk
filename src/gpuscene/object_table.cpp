#include "gpuscene/object_table.h"

#include <algorithm>
#include <chrono>
#include <cstring>

#include <spdlog/spdlog.h>

#include "d3d9/device.h"
#include "engine/scene_table.h"
#include "gpuscene/instance_buffer.h"
#include "gpuscene/window_tables.h"

namespace rvk::gpuscene {

namespace {
constexpr std::uint64_t kVerifyEvery = 240;
}  // namespace

ObjectTable& objecttable() {
    static ObjectTable t;
    return t;
}

ObjectWhy ObjectTable::serve(const void* entity, const float composed[kObjectRegs][4], std::uint32_t composedBits, d3d9::OwnedWindows::Win& out) {
    ++stats_.requests;
    const auto fail = [&](ObjectWhy why) {
        ++stats_.why[static_cast<std::size_t>(why)];
        return why;
    };
    if (!active_ || !table_.ready() || table_.ref().null()) return fail(ObjectWhy::NotReady);
    if ((composedBits & 1u) != 0) return fail(ObjectWhy::C31Live);
    const std::uint32_t slot = engine::scenetable::find(entity);
    if (slot == UINT32_MAX) return fail(ObjectWhy::NoSlot);
    if (slot >= kCapacity) return fail(ObjectWhy::SlotRange);
    const engine::scenetable::Slot* s = engine::scenetable::slot(slot);
    if (s == nullptr) return fail(ObjectWhy::NoSlot);
    // the matrix check: the table's (signal-fed) matrix against the one the draw was composed with (entity +0x60 read at the composition)
    float rows[3][4];
    objectWorldRows(s->world, rows);
    ++stats_.matrixChecks;
    for (std::uint32_t k = 0; k < 3; ++k) {
        if ((composedBits >> (2 + k) & 1u) != 0 && std::memcmp(composed[2 + k], rows[k], 16) != 0) {
            ++stats_.matrixBad;
            return fail(ObjectWhy::MatrixDiffers);
        }
    }
    auto& shadow = table_.shadow();
    ObjectRecord& rec = shadow[slot];
    const bool wasLive = rec.info[3] != 0;
    const float fade = (composedBits & 2u) != 0 ? composed[1][3] : (wasLive ? rec.regs[1][3] : 1.0f);
    const ObjectRecord want = makeObjectRecord(s->world, fade, s->generation, s->version, s->flags);
    const bool regsDiffer = std::memcmp(rec.regs, want.regs, sizeof(rec.regs)) != 0;
    const bool infoDiffer = std::memcmp(rec.info, want.info, sizeof(rec.info)) != 0;
    const bool servedNow = servedFrame_[slot] == frame_ + 1;
    if (regsDiffer && servedNow) return fail(ObjectWhy::FadeConflict);  // (the GPU sees one value per frame)
    if (regsDiffer || infoDiffer) {
        if (!wasLive) {
            ++stats_.newRecords;
        } else if (regsDiffer) {
            const bool fadeOnly = std::memcmp(rec.regs[2], want.regs[2], 3 * 16) == 0;
            ++(fadeOnly ? stats_.fadeRewrites : stats_.matrixRewrites);
        }
        rec = want;
        table_.mark(slot);
        used_ = std::max(used_, slot + 1);
    }
    servedFrame_[slot] = frame_ + 1;
    servedVersion_[slot] = s->version;
    // the window: base = record - 31 x 16 (the pad keeps it >= 0 on D3D12)
    out = d3d9::OwnedWindows::Win{};
    out.kind = d3d9::OwnedWindows::Kind::Object;
    out.base = table_.ref().plus(static_cast<std::int64_t>(slot) * static_cast<std::int64_t>(sizeof(ObjectRecord)) - static_cast<std::int64_t>(kObjectFirstReg) * 16);
    out.covered = kObjectRegs;
    out.verifyMask = composedBits & ~1u;
    out.wOnlyMask = 2u;  // c32: only .w (the fade); .xyz is carry-over in the engine's register file
    out.ignoreMask = ((~composedBits) & ((1u << kObjectRegs) - 1u)) | 1u;
    out.image = rec.regs[0];
    ++stats_.served;
    ++stats_.why[static_cast<std::size_t>(ObjectWhy::Served)];
    return ObjectWhy::Served;
}

bool ObjectTable::serveFast(const void* entity, std::uint32_t composedBits, float fade, d3d9::OwnedWindows::Win& out) {
    if (!active_ || !table_.ready() || table_.ref().null() || (composedBits & 1u) != 0) return false;
    const std::uint32_t slot = engine::scenetable::find(entity);
    if (slot == UINT32_MAX || slot >= kCapacity) return false;
    const engine::scenetable::Slot* s = engine::scenetable::slot(slot);
    if (s == nullptr) return false;
    const ObjectRecord& rec = table_.shadow()[slot];
    if (rec.info[3] == 0 || rec.info[0] != s->generation || rec.info[1] != s->version || rec.info[2] != s->flags) return false;
    if ((composedBits & 2u) != 0 && std::memcmp(&rec.regs[1][3], &fade, sizeof(float)) != 0) return false;
    ++stats_.requests;
    ++stats_.fast;
    servedFrame_[slot] = frame_ + 1;
    servedVersion_[slot] = s->version;
    out = d3d9::OwnedWindows::Win{};
    out.kind = d3d9::OwnedWindows::Kind::Object;
    out.base = table_.ref().plus(static_cast<std::int64_t>(slot) * static_cast<std::int64_t>(sizeof(ObjectRecord)) - static_cast<std::int64_t>(kObjectFirstReg) * 16);
    out.covered = kObjectRegs;
    out.verifyMask = composedBits & ~1u;
    out.wOnlyMask = 2u;
    out.ignoreMask = ((~composedBits) & ((1u << kObjectRegs) - 1u)) | 1u;
    out.image = const_cast<float*>(rec.regs[0]);
    ++stats_.served;
    ++stats_.why[static_cast<std::size_t>(ObjectWhy::Served)];
    return true;
}

bool ObjectTable::windowFor(std::uint32_t slot, d3d9::OwnedWindows::Win& out) const {
    if (!active_ || !table_.ready() || table_.ref().null() || slot >= kCapacity) return false;
    out = d3d9::OwnedWindows::Win{};
    out.kind = d3d9::OwnedWindows::Kind::Object;
    out.base = table_.ref().plus(static_cast<std::int64_t>(slot) * static_cast<std::int64_t>(sizeof(ObjectRecord)) - static_cast<std::int64_t>(kObjectFirstReg) * 16);
    out.covered = kObjectRegs;
    return true;
}

void ObjectTable::flush(d3d9::Device& dev) {
    if (!active_) {
        ++frame_;
        return;
    }
    if (!created_ && !gpuFailed_) {
        created_ = true;
        if (!table_.create(dev, kCapacity, "object table", kTablePad)) {
            gpuFailed_ = true;
            spdlog::warn("native: owned objects: the object table buffer could not be created (the object windows stay uploaded)");
        } else {
            servedFrame_.assign(kCapacity, 0);
            servedVersion_.assign(kCapacity, 0);
        }
    }
    if (table_.ready()) {
        // the scene table's drain (the matrix signals): records that exist are re-composed from the slot's matrix. Only when nothing else drains the table.
        if (!instances().ready()) {
            auto& shadow = table_.shadow();
            engine::scenetable::drainDirty([&](std::uint32_t i, const engine::scenetable::Slot& s) {
                if (i >= kCapacity) return;
                ++stats_.drained;
                if (servedFrame_[i] == frame_ + 1 && servedVersion_[i] != s.version) {
                    // a draw of this frame read the matrix before a later signal changed it: the record uploaded now is not what that draw was composed with
                    ++stats_.changedAfterServe;
                    if (changedLogged_ < 8) {
                        ++changedLogged_;
                        spdlog::warn("native: owned objects: slot {} (entity {}) changed its matrix after a draw of frame {} was served from it (version {} -> {})", i, s.entity,
                                     frame_, servedVersion_[i], s.version);
                    }
                }
                ObjectRecord& rec = shadow[i];
                if (rec.info[3] == 0) return;  // (never served: no record to keep fresh)
                const ObjectRecord want = makeObjectRecord(s.world, rec.regs[1][3], s.generation, s.version, s.flags);
                if (std::memcmp(&rec, &want, sizeof(rec)) != 0) {
                    rec = want;
                    table_.mark(i);
                }
            });
        }
        table_.flush(dev, frame_ % kVerifyEvery == 0, used_);
        table_.collect(dev.game(), "object table");
    }
    report(dev);
    ++frame_;
}

void ObjectTable::report(d3d9::Device& dev) {
    const double now = std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
    if (now - lastReport_ < 5.0) return;
    lastReport_ = now;
    const ObjectStats& s = stats_;
    const ObjectStats& p = last_;
    const d3d9::OwnedStats& o = dev.ownedStats();
    const d3d9::OwnedStats& po = lastDev_;
    const auto d = [&](std::uint64_t a, std::uint64_t b) { return a - b; };
    const auto why = [&](ObjectWhy w) { return d(s.why[static_cast<std::size_t>(w)], p.why[static_cast<std::size_t>(w)]); };
    const auto& ts = table_.stats();
    const std::uint64_t frames = std::max<std::uint64_t>(frame_ - lastFrame_, 1);
    spdlog::info(
        "native: owned objects (5 s): device served VS1 {} VS2 {} (short: VS1 {} VS2 {}); candidates {} (instanced draws {}, other draws {}), served VS2 {} (VS1 offered {}, composed differs {}), fallback: not ready {}, no slot {}, "
        "c31 live {}, matrix differs {}, fade / matrix conflict {}; records new {}, fade rewrites {}, matrix rewrites {}; matrix check {} draws, bad {}, changed after serve {}; "
        "drained {}; {:.2f} records / {:.2f} KiB uploaded per frame; read-backs {} DIFFERING {}",
        o.view1 - po.view1, o.object - po.object, o.shortVs1 - po.shortVs1, o.shortVs2 - po.shortVs2, d(s.requests, p.requests), d(s.instanced, p.instanced), d(s.notCandidate, p.notCandidate), why(ObjectWhy::Served), d(s.vs1Offered, p.vs1Offered), d(s.vs1Differs, p.vs1Differs),
        why(ObjectWhy::NotReady), why(ObjectWhy::NoSlot) + why(ObjectWhy::SlotRange), why(ObjectWhy::C31Live), why(ObjectWhy::MatrixDiffers), why(ObjectWhy::FadeConflict),
        d(s.newRecords, p.newRecords), d(s.fadeRewrites, p.fadeRewrites), d(s.matrixRewrites, p.matrixRewrites), d(s.matrixChecks, p.matrixChecks), s.matrixBad, s.changedAfterServe,
        d(s.drained, p.drained), static_cast<double>(ts.slotsUploaded - lastSlots_) / static_cast<double>(frames), static_cast<double>(ts.bytes - lastBytes_) / 1024.0 / static_cast<double>(frames),
        ts.verifyDone, ts.verifyBad);
    last_ = s;
    lastDev_ = o;
    lastFrame_ = frame_;
    lastBytes_ = ts.bytes;
    lastSlots_ = ts.slotsUploaded;
}

}  // namespace rvk::gpuscene
