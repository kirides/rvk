#pragma once

// The engine side of the native scene mirror (docs/research/native-renderer.md N0 / N1): detours on the engine's own entity
// lifecycle events feed ::rvk::scene::Mirror, a read-only snapshot of each cached-in entity (world matrix +0x60, flags +0x28, world
// tree sphere +0xb8, render context list +0xf0, and for mesh contexts the mesh resource's buffers and sub-mesh table) is refreshed
// every few frames, and rvk.log reports the mirror against the engine's own counters (eCPrefetchAdmin +0x528 / +0x52c) and
// against the draws' tags. Vulkan mode only. Nothing here changes what the engine draws.

#include <cstdint>
#include <vector>

#include "scene/mirror.h"

namespace rvk::engine::mirror {

// creates the detours (enabled with the others by hooks::enableAll); false when a symbol is missing
bool install();

::rvk::scene::Mirror& get();

// engine thread, once per present: refreshes the snapshots (every few frames) and reports every 5 s
void onPresent();

// what a draw of the last frame says about its geometry (d3d9::draws::Record)
struct DrawProbe {
    const void* entity = nullptr;
    const void* subMesh = nullptr;
    const void* meshResource = nullptr;
    std::uint64_t vertexBufferId = 0, indexBufferId = 0;  // d3d9 resource ids of stream 0 / the indices (0 = none)
    std::uint32_t first = 0, primitives = 0;               // start index, triangles
    std::int32_t baseVertex = 0;
    bool plainIndexedMesh = false;                         // a non-instanced indexed triangle-list draw of a static mesh
};

// the entities of the last frame's draws (null ones skipped): counted as resolved / unresolved in the mirror, the first unresolved
// ones are logged; the plain mesh draws are checked against the mirror's mesh parts (the resolved buffers and ranges: N1)
void crossCheck(const std::vector<DrawProbe>& draws);

}  // namespace rvk::engine::mirror
