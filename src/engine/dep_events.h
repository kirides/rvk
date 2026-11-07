#pragma once

// F2e-1 step A (docs/research/frame-ownership.md 7.30): the engine's change events that invalidate an owned op record, as a queue drained once per frame.
//
//   eCResourceCache2::LoadData 0x684e90 / QueueData 0x6852d0 / FlushResource 0x684340   every resource state / data pointer transition (mesh, material, image)
//   eCMeshResource2::OnLightmapChanged 0x75f700                                          the lightmap pointers of a mesh resource
//   eCMaterialResource2::SetShader 0x68f620                                              a material's shader pointer
// (the illumination is NOT an event input: eCLightSet::UpdateIllumination reports a change about every frame and the light lists are shared by hundreds of records, so a
// set / PS event marks everything; the light stamp is polled, 6 words per op: 7.30)
//
// Post-hooks: the object (the resource / PS pointer) is queued after the original returned (its state is final). The hooks may run on a loader thread:
// the queue has a lock; the consumer drains it on the engine thread at the start of a main view and marks the records that registered the object.

#include <cstdint>
#include <vector>

namespace rvk::engine::depevents {

// creates the detours (bootstrap, with the other engine hooks; only while native.owned_static is on at start); false when a symbol is missing
bool install();
bool active();


// engine thread: the objects queued since the last drain (deduplicated), 
void drain(std::vector<const void*>& out);

struct Counters {
    std::uint64_t loadData = 0, flush = 0, queueData = 0, lightmap = 0, setShader = 0, drained = 0, queueUnchanged = 0;
};
const Counters& counters();

}  // namespace rvk::engine::depevents
