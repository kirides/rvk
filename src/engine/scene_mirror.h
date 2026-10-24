#pragma once

// The engine side of the native scene mirror (docs/research/native-renderer.md N0): detours on the engine's own entity
// lifecycle events feed ::rvk::scene::Mirror, a read-only snapshot of each cached-in entity (world matrix +0x60, flags +0x28, world
// tree sphere +0xb8, render context list +0xf0) is refreshed every few frames, and rvk.log reports the mirror against the
// engine's own counters (eCPrefetchAdmin +0x528 / +0x52c) and against the draws' entity tags. Vulkan mode only. Nothing here
// changes what the engine draws.

#include <vector>

#include "scene/mirror.h"

namespace rvk::engine::mirror {

// creates the detours (enabled with the others by hooks::enableAll); false when a symbol is missing
bool install();

::rvk::scene::Mirror& get();

// engine thread, once per present: refreshes the snapshots (every few frames) and reports every 5 s
void onPresent();

// the entities of the last frame's draws (null ones skipped): counted as resolved / unresolved in the mirror, the first
// unresolved ones are logged
void crossCheck(const std::vector<const void*>& drawEntities);

}  // namespace rvk::engine::mirror
