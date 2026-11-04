#pragma once

// The GPU scene's instance table, CPU side (docs/research/gpu-scene.md 3, M1a): one slot per cached-in geometry entity, kept by the
// engine's change signals instead of by sighting draws:
//   eCPrefetchAdmin::OnEntityCachedIn / OnEntityCachedOut   slot allocated / freed
//   eCEntityAdmin::NotifyDeleteEntities                     slot freed (the net for an entity deleted while cached in)
//   eCGeometryEntity / eCDynamicEntity::OnUpdatedWorldMatrix  the slot's world matrix replaced (previous kept), slot marked dirty
// Dirty slots are drained in batches (the upload to the GPU instance buffer, M1b). Everything runs on the engine's thread
// (the signals and the ops' slot calls), no locks. Verification (`native.gpu_scene` >= 1): every op of a covered class
// that the engine's frame walks must resolve to a slot whose matrix equals the entity's own (a missed signal = a bug).

#include <cstdint>
#include <functional>

namespace rvk::engine::scenetable {

struct Slot {
    const void* entity = nullptr;   // null = free
    float world[16]{};               // entity +0x60, row-major 4x4 (row-vector convention as the engine stores it)
    float prev[16]{};                // the matrix before the last change (object motion for FSR / RT)
    std::uint32_t generation = 0;    // bumped on every allocation of the slot
    std::uint32_t version = 0;       // bumped on every matrix change
    std::uint32_t flags = 0;         // entity +0x28 at cache-in (poll: not signal-fed yet)
    std::uint64_t movedFrame = 0;    // frame of the last matrix change
    bool dirty = false;              // not uploaded since the last change
};

struct Counters {
    std::uint64_t cachedIn = 0, cachedOut = 0, doubleIn = 0, outUnknown = 0, deleted = 0, deletedLive = 0;
    std::uint64_t matrixSignals = 0, matrixUnknown = 0, matrixSame = 0;
    std::uint64_t checked = 0, resolved = 0, unresolved = 0, matrixOk = 0, matrixBad = 0;
    std::uint64_t drained = 0, batches = 0;
};

// Creates the detours (enabled with the others by hooks::enableAll); false when a symbol is missing
bool install();

// engine thread, once per present: advances the frame, prints the 5 s report
void onPresent();

std::uint32_t liveSlots();
std::uint32_t capacity();
const Slot* slot(std::uint32_t index);
std::uint32_t find(const void* entity);   // UINT32_MAX when unknown
const Counters& counters();

// Calls fn(index, slot) for every dirty slot and clears the flags: one batch (the M1b upload). Returns the count.
std::uint32_t drainDirty(const std::function<void(std::uint32_t, const Slot&)>& fn);

// Calls fn(index) for every slot freed since the last call (apply before drainDirty: a freed slot may have been reallocated and be dirty again).
// only while a consumer drains them (the GPU table): else the list is not kept
void trackFreed(bool on);
std::uint32_t drainFreed(const std::function<void(std::uint32_t)>& fn);

// Verification of one op's entity against the table (counts; logs the first mismatches). `entity` = op context +0x10.
void checkEntity(const void* entity);

}  // namespace rvk::engine::scenetable
