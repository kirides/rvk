#pragma once

// What the game shows: the world, a menu page or the loading screen (docs/research/menu-fps.md). Read through the GUI's page
// predicates exported by Script.dll (GUI2::IsLoading / GetMode / IsVisible, forwarders to gCHudSystem2 in Game.dll) and
// eCApplication::GetAppState:
//  * Loading: the loading page (29) is visible, or the app state is 2 (loading / prefetch) or 0x10 (the wait after a load).
//    A savegame load runs synchronously on the engine thread and presents the loading screen after every load step
//    (gCWorldEventSource::SendEvent -> eCGuiModule2::RenderEdit -> SwapScreen, no throttle): any wait at present adds to
//    the load time, so the frame pacer does not wait at all then.
//  * Menu: the game view (page 1) is hidden (GUI2::GetMode() == 1: the ESC / main menu, the title screen, the outro), or one
//    of the full-screen panels Inventory (2), Character (3), Log (4), Map (5) is open (they keep the world running but cover
//    it). Dialogues, trade, loot and cutscenes are Game.
//  * Unknown: the GUI is not up yet (or Script.dll is not loaded): treated as Game.
// Engine thread only (the predicates walk the GUI's page table; FindPage makes a temporary string, so not free: a few
// microseconds per query).

#include <cstdint>

namespace rvk::engine::menu {

enum class State : std::uint8_t { Unknown, Game, Menu, Loading };

struct Status {
    State state = State::Unknown;
    std::uint32_t pages = 0;      // bit n: page n checked and visible (1, 2..5, 29)
    std::uint32_t appState = 0;   // eCApplication::GetAppState (0 normal, 1 paused, 2 loading, 8 time freeze, 0x10 after a load)
    bool faulted = false;         // an engine call raised an exception (the answer is Unknown)
};

// once per present
Status query();

const char* name(State s);

}  // namespace rvk::engine::menu
