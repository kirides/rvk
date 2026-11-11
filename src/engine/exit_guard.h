#pragma once

// The game's own exit crash (docs/research/shutdown.md; any render mode, also Original = the system D3D9): Game.dll's static
// gCTutorialAdmin is destroyed in Game.dll's DLL_PROCESS_DETACH (inside ExitProcess), long after the engine's GUI resources were
// destroyed in the normal shutdown. Its gCTutorialHintCollection's eCGuiBitmapProxy2 members still point at those dead
// eCDeferredGuiBitmap2 objects; ~eCGuiBitmapProxy2 calls their Release -> eCGuiResource2::Release calls vtable slot 0x118 of a
// destroyed object: SharedBase.dll+0x7caa9 (the slot is bCObjectBase::Read: the base vtable of a destroyed object) or
// Engine.dll+0x8f1bd (a garbage slot). Measured: 2 of 3 / 3 of 5 Original exits, most Vulkan / D3D12 exits.
//
// The guard: while the loader shuts the process down (ntdll!RtlDllShutdownInProgress), ~eCGuiBitmapProxy2 drops its pointer without
// the Release and eCGuiResource2::Release does nothing. Harmless: a GUI resource's last release only runs OnFinalRelease (a no-op for
// eCGuiResource2, COMDAT-folded with an empty function) and frees its memory, which the exiting process returns anyway; no file / save /
// config write is involved (those finished before ExitProcess). Outside the process shutdown both functions run unchanged.
//
// Also: the rich presence worker thread reads the destroyed eCApplication (Game.dll+0x970c6e); after ~eCApplication it is parked.

namespace rvk::engine::exitguard {

// Hooks ~eCGuiBitmapProxy2 and eCGuiResource2::Release (DllMain: Engine.dll is mapped, nothing is called). Any mode. false when
// the exports are missing (an unknown build: the game's behaviour stays).
bool install();

// how many releases the guard skipped (rvk.log is gone by then: for tests / the exit report)
unsigned skipped();

}  // namespace rvk::engine::exitguard
