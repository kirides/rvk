#pragma once

// The GPU illumination table (docs/research/frame-ownership.md F0; the pattern of instance_buffer.*): one 288-byte record per illumination object
// (eSShaderMaterialIllumination = eCIlluminated_PS + 0x78) and one 336-byte record per light set, raw engine bytes (illum_compose.h), in persistent
// device buffers updated in batches once per present on both backends (rhi buffers; shader references in shaders/include/rvk_illum.hlsli).
// Setting `native.illum_table` (Debug, restart, default off; Full only: only StaticOps::writeLighting allocates records):
//   * slots are allocated at the first sight of an object in writeLighting;
//   * a record is re-read (and re-uploaded when its bytes changed) when (a) the engine's UpdateIllumination ran with the dirty byte (PS + 0x188)
//     set, (b) SetLightSet re-pointed the object, (c) the outdoor factor f differs (polled once per frame, for the objects drawn);
//   * a light set's record is re-read after every eCLightSet::UpdateIllumination / eCIlluminationAdmin::UpdateLightSets (the contents change in
//     place every frame);
//   * freed at eCIlluminated_PS::Destroy, or when not drawn / updated for kIllumIdleFrames frames.
// Verification (F0 gate): `checkDraw` composes the registers of each lit draw FROM THE RECORDS and compares them with what writeLighting wrote
// (rvk.log `native: illum table (5 s): ... DIFFERING N`, must stay 0); every 240th frame the buffers are copied back and compared with the CPU copy.

#include <cstdint>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <vector>

#include "gpuscene/illum_compose.h"
#include "rhi/game_device.h"

namespace rvk::d3d9 {
class Device;
}

namespace rvk::gpuscene {

inline constexpr std::uint32_t kNoSlot = 0xffffffffu;

struct TableStats {
    std::uint64_t flushes = 0, slotsUploaded = 0, runs = 0, bytes = 0, overflow = 0;
    std::uint64_t verifies = 0, verifyDone = 0, verifySlots = 0, verifyBad = 0;
};

// one device buffer + its CPU twin + the dirty list (the instance buffer's recipe, for any record type)
template <class T>
class GpuTable {
public:
    ~GpuTable();
    // CPU side only (records can be written before the GPU buffer exists)
    void init(std::uint32_t capacity);
    // `padBytes`: unused bytes in front of record 0 (ref() points past them), so a register window biased below a record (frame-ownership.md 7.8) keeps a byte offset >= 0 on D3D12
    bool create(d3d9::Device& dev, std::uint32_t capacity, const char* name, std::uint64_t padBytes = 0);
    bool ready() const { return buffer_ != nullptr; }
    rhi::GpuRef ref() const { return ref_; }
    std::uint32_t capacity() const { return capacity_; }
    std::vector<T>& shadow() { return shadow_; }
    const std::vector<T>& shadow() const { return shadow_; }
    // marks a slot for the next flush (call with the CPU copy already updated)
    void mark(std::uint32_t i);
    void markAll(std::uint32_t used);
    // uploads the marked slots (one ring block, one copy per contiguous run); `verify`: also copy the whole used range back and compare
    void flush(d3d9::Device& dev, bool verify, std::uint32_t used);
    // collects finished read-backs
    void collect(rhi::GameDevice& game, const char* name);
    // drops the device buffer, the read-back buffers and the dirty list (tests; the CPU shadow stays)
    void release();
    const TableStats& stats() const { return stats_; }
    std::size_t dirtyCount() const { return dirty_.size(); }

private:
    void startVerify(d3d9::Device& dev, std::uint32_t used);

    std::unique_ptr<rhi::Buffer> buffer_;
    rhi::GpuRef ref_;
    std::uint32_t capacity_ = 0;
    std::uint64_t pad_ = 0;  // bytes in front of record 0 in buffer_
    std::vector<T> shadow_;
    std::vector<std::uint32_t> dirty_;
    std::vector<std::uint8_t> isDirty_;
    struct Pending {
        std::unique_ptr<rhi::Buffer> buffer;
        std::uint64_t frameValue = 0;
        std::vector<T> expect;
        bool busy = false;
    };
    Pending pending_[rhi::kFramesInFlight];
    TableStats stats_;
};

struct IllumStats {
    std::uint64_t uploadDirty = 0, uploadSet = 0, uploadF = 0, uploadPoll = 0, uploadNew = 0, uploadSetData = 0;  // records re-uploaded by cause
    std::uint64_t dirtyNoChange = 0, setNoChange = 0;                                              // hook fired, the bytes were the same
    std::uint64_t freed = 0, destroyed = 0, idleFreed = 0, setsFreed = 0;
    std::uint64_t checked = 0, differing = 0, unchecked = 0;
    std::uint64_t illumOverflow = 0, setOverflow = 0, readFailed = 0;
};

class IllumTable {
public:
    static constexpr std::uint32_t kIllumCapacity = 16384;  // slots (4.5 MiB)
    static constexpr std::uint32_t kSetCapacity = 8;        // light sets (measured: 1 per frame)
    static constexpr std::uint64_t kIllumIdleFrames = 1800;
    static constexpr std::uint64_t kSetIdleFrames = 600;

    // bootstrap: the hooks are installed and the setting is on
    void setActive(bool on) { active_ = on; }
    bool active() const { return active_; }

    // ---- engine hooks (engine thread; engine/illum_census.cpp) ----
    void onPsUpdate(const void* ps, bool wasDirty);  // post eCIlluminated_PS::UpdateIllumination
    void onSetLightSet(const void* ps);              // post SetLightSet
    void onSetUpdated(const void* set);              // post eCLightSet::UpdateIllumination
    void onSetsUpdated();                            // post eCIlluminationAdmin::UpdateLightSets
    void onPsDestroy(const void* ps);                // pre eCIlluminated_PS::Destroy

    // ---- StaticOps::writeLighting ----
    // `prior` = the lighting registers (24 x float4) before writeLighting wrote them, `now` = after. Composes from the records and compares.
    // Returns the illumination's slot (kNoSlot: not active / table full).
    // `verify` false (7.27, off the check frames): lookup, the once-per-frame poll and the keep-alive only; prior / now are not read
    std::uint32_t checkDraw(const void* illum, const ComposeParams& p, const float prior[24][4], const float now[24][4], bool verify = true);
    // The CPU composition of slot `slot` from the CURRENT shadow records (the lighting images' check); L must be initialised by the caller. False: no live
    // record / no light set.
    // `why` (optional): 1 = no live record for the slot, 2 = the record has no (live) light set
    bool compose(std::uint32_t slot, const ComposeParams& p, float L[24][4], int* why = nullptr);
    // F4-1 (an owned op composed nothing): checkDraw's bookkeeping without the verification: true when `illum` still has slot `expectSlot` (not freed, not reused); marks it drawn
    // and polls the object's bytes against the record once per frame (a change refreshes the record)
    bool pollDraw(const void* illum, std::uint32_t expectSlot);
    // frame-ownership.md 7.21 addendum: the owned static records' per-op check, no lock and no byte poll (engine thread only, where the slots are freed): true when `illum` still
    // has slot `expectSlot` (not freed by the idle sweep / Destroy, not reused); marks the slot drawn so that an op drawn without being composed is never idle-freed
    bool slotLive(const void* illum, std::uint32_t expectSlot) {
        if (!active_ || illum == nullptr || expectSlot >= illumKey_.size() || illumKey_[expectSlot] != illum) return false;
        illumSeen_[expectSlot] = frame_;
        return true;
    }
    // F4-3 (owned trees: an instance's own illumination, which the engine's batch never lit): the illumination's slot (created on first use), marked drawn and polled like
    // pollDraw (kNoSlot: not active / table full)
    std::uint32_t ensureSlot(const void* illum);

    // once per present (engine thread, before the frame's draws are submitted): uploads, verification, idle sweep, 5 s log. `verify` = read-back every 240th frame
    void flush(d3d9::Device& dev, bool verify);

    rhi::GpuRef illumRef() const { return illums_.ref(); }
    rhi::GpuRef setRef() const { return sets_.ref(); }
    const IllumStats& stats() const { return stats_; }
    std::uint32_t liveIllums() const { return liveIllums_; }

private:
    // all under mutex_
    std::uint32_t allocIllum();
    std::uint32_t setSlotFor(const void* list, bool* isNew);
    bool readSetInto(const void* list, LightSetRecord& rec);
    // reads the object into `slot`'s record; returns whether the content changed
    bool refresh(std::uint32_t slot, const void* illum);
    std::uint32_t lookupOrCreate(const void* illum);
    void report();

    bool active_ = false;
    std::mutex mutex_;
    GpuTable<IllumRecord> illums_;
    GpuTable<LightSetRecord> sets_;
    std::unordered_map<const void*, std::uint32_t> illumSlot_;  // eSShaderMaterialIllumination* -> slot
    std::vector<const void*> illumKey_;                          // slot -> key (nullptr = free)
    std::vector<std::uint64_t> illumSeen_;                       // slot -> frame last drawn / updated
    std::vector<std::uint64_t> illumFChecked_;                   // slot -> frame f was last polled
    std::vector<std::uint32_t> freeIllums_;
    std::uint32_t illumUsed_ = 0, liveIllums_ = 0, peakIllums_ = 0;
    const void* setKey_[kSetCapacity]{};                         // light list pointer (set + 0x10) per slot
    std::uint64_t setSeen_[kSetCapacity]{};
    std::uint32_t setUsed_ = 0;
    std::uint64_t frame_ = 0;
    IllumStats stats_;
    IllumStats lastStats_;
    std::uint64_t lastFrame_ = 0, lastBytes_ = 0, lastSlots_ = 0;
    double lastReport_ = 0.0;
    int differingLogged_ = 0;
    bool gpuFailed_ = false;
};

IllumTable& illumtable();

}  // namespace rvk::gpuscene
