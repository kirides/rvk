# Menu frame rate limit and unpaced loading screens

Research and build of 2026-10-23. Question (user): cap the frame rate while a menu is open (ESC menu, character screen) so
the GPU does not run flat out on a still picture, without a "savegame load penalty" (in Piranha Bytes' earlier games the
loading screen's render rate also set the load speed).

Legend: [C] read in the binaries or measured, [I] inferred. RVAs relative to each DLL's image base (0x180000000).
Dumps (outside the repo): `B:\DevDrive\risen-ghidra\menufps\` (`script_gui.txt`, `game_hud.txt`, `game_load.txt`,
`game_world.txt`, `game_prefetch.txt`, `findpage.txt`, `scriptgame_ctrl.txt`, `engine_app.txt`, `engine_gui.txt`).

## 1. Result

* **Built**: `engine/menu_state.*` reads the GUI's page state once per present; `HalDevice::updateMenuPacing` caps the
  pacer at `vulkan.menu_fps` (player setting, Display tab, default 60, 0 = unlimited) while a menu is open and **does not
  pace the loading screen at all** (`d3d9::Device::setUnpaced`). The loading-screen exemption covers every cap: vsync,
  `vulkan.max_fps`, the borderless refresh cap and the menu cap.
* **The load penalty is real and large** [C, section 4]: a quick load presents ~650-990 loading-screen frames, one per load
  step, from the engine thread that loads. Paced at 30 fps the same quick load took **20.2 s instead of 2.2 s**. Before this change
  every cap (vsync, `vulkan.max_fps`) paced loading screens; the hidden `debug.pace_loading = true` restores that for A/B.
* **Vsync blocks too**: skipping the pacer alone left vsync-on quick loads at 5.6 s (the FIFO swapchain holds presents to the
  120 Hz refresh: ~106 loading presents/s instead of ~220). So while loading, a frame is **shown at most every 1/30 s** (or
  the cap's interval when longer) and the others are rendered and submitted **without acquire / present**
  (`Device::present`, rvk.log `hal: ... loading frames not shown N`): nothing in the chain can block the loader. Vsync-on
  quick loads: 2.06-2.11 s (section 4).
* Vulkan mode only (the pacer is ours); D3D9 passthrough is unchanged.

## 2. Menu state: the GUI's page predicates [C]

Pages (`gEHudPage`, risen-spass RE.md 42): 1 Game (HUD / game view), 2 Inventory, 3 Character, 4 Log, 5 Map, 6 CraftSelect,
7 ItemSelect, 8 Loot, 9 Pickpocket, 10 Trade, 11 Dialog, 12 Talk, 13 Menu_Main, 14..25 menu sub pages, 26 Menu_Start, 27
Menu_Back, 28 Outro, 29 Loading.

Script.dll exports thin forwarders (`if (init) { s = gCHudModule2::GetSystem(); if (s) return gCHudSystem2::X(s); }`; they
ignore `this`; not COMDAT-folded, called only, never detoured). Implementations in Game.dll on `gCHudSystem2`:

| Script.dll export | RVA | Game.dll | returns |
|---|---|---|---|
| `?IsLoading@GUI2@@QEBA_NXZ` | 0x2105a0 | 0x83de50 | page 29 visible |
| `?GetMode@GUI2@@QEBA?AW4gEHudMode@@XZ` | 0x210520 | 0x83ddc0 | 0 = page 1 visible, 1 = hidden (or missing); -1 = no GUI module yet |
| `?IsVisible@GUI2@@QEBA_NW4gEHudPage@@@Z` | 0x210740 | 0x83f5e0 | the page is visible |
| `?IsBrowsing@GUI2@@QEBA_NXZ` | 0x2106a0 | 0x83e160 | false with page 29; else any visible page with ID > 1 (**also dialogues, trade, loot**) |
| `?IsBrowsingGame@GUI2@@QEBA_NXZ` | 0x210620 | 0x83df80 | page 1 visible and a page 2..12 visible |
| `?IsBrowsingMenu@GUI2@@QEBA_NXZ` | 0x2105e0 | 0x83de80 | page 1 hidden and a page 13..26 visible |

* `IsBrowsing` (what risen-spass uses for "menu") is **not** a menu test: it is true in dialogues.
* The ESC menu hides page 1: `OnControl_Game` (Script_Game.dll 0x165f90) answers "page opens" (0x8037) with hide only for
  page 13, and on "hidden" (0x803a) calls `World::Pause` (`gCSession::Pause` 0x971970: session flag +0x108 bit 0,
  `eCApplication::SetAppState(1)`). `GetMode() == 1` is the ESC / main menu, the title screen and the outro.
* **Inventory, Character, Log and Map keep page 1 visible and do not pause the world** [C in code; measured: the character
  screen's state line reads pages 0xa = 1 and 3, app state 0]. They are full-screen panels, so they count as menus here.
* Our test (`engine::menu::query`, engine thread, SEH-guarded): Loading = `IsLoading()` or app state 2 / 0x10; else Menu =
  `GetMode() == 1` or `IsVisible(2..5)`; else Game (dialogues, trade, loot, pickpocket, crafting, cutscenes). `GetMode() ==
  -1` = Unknown (no GUI yet) = no menu cap. Script.dll is resolved at the first present that finds it.
* Cost: `FindPage` (0x83f470) builds a temporary `bCString` per matching page (`bCString::Right`) and logs "could not find
  page" for a missing one; a query is a handful of `FindPage`s, microseconds, once per present [I].
* Other state: `World::IsPaused` (Script.dll 0x235fb0) -> `gCSession::IsPaused` (Game.dll 0x974590, `this[0x108] & 1`, a
  1-instruction getter) is true only with page 1 hidden. `eCApplication::GetAppState` (Engine 0x4c1ad0, `this+0x134`): 0
  normal, 1 paused, 2 loading / prefetch, 8 debug time freeze, 0x10 the wait after a load.

## 3. The load and its loading screen share the engine thread [C]

* `gCHUDFileManager::OnProcessEvent` (Game.dll 0x87c160, from the event queue): case 0 `NewGame` (0x87afe0), cases 2 / 4
  `LoadGame` (0x87aef0), 5 the teleport prefetch. `LoadGame` = `BeginProgress` (0x87ad10: `gCSession::Stop`, registers the
  file manager as a world-event client, opens page 29, closes page 1, mutes audio), `gCSaveGameManager::Load` (0x99d080),
  `gCSession::Start(1)`, `GotoLoadedPosition`, `RequestAppState(2)`.
* `gCWorld::Reload` (0x994100) / `Activate` (0x994760) call `gCWorldEventSource::SendEvent(&src, 1, 0, &progress)` after
  every sector / step; `SendEvent` (0x98deb0) -> `gCHUDFileManager::OnWorldEvent` (0x87d700, progress per cent) ->
  `eCGuiModule2::RenderEdit(hwnd, 0, 0x7f0000)` (Engine 0x519c90): BeginScene, clear, `eCGuiModule2::Draw`, EndScene,
  **SwapScreen**. No `eCApplication::Draw` / `DoRender` (hence presents > Draw calls, missing-slots.md, and no `ui.frame`
  during a load), **no time throttle** (no QPC / GetTickCount / Sleep in the chain).
* So the loader waits for every present: **any wait at present adds to the load time**, one interval per load step.
* After the synchronous part the main loop runs with app state 2 / 0x10 (page 29 re-shown during 0x10, `gCHudModule2::Process`
  0x83cd90 drops 0x10 after 2000 ms); `EndProgress` (0x87ab00) closes page 29 and opens page 1 (and plays the intro video
  for a new game). Teleports (`PostPrefetch` 0x87bf30, event 5): page 29 + app state 2 without the `SendEvent` loop. Both
  count as Loading. Whether the prefetch phase does per-frame work was not determined [open]: unpaced either way.
* Bink videos (`eCApplication::PlayVideo` 0x4c1b00) have their own loop and Present; the page state stays as it was.

## 4. Measurements [C]

Scenario `mods/rvk_menufps` (new game; 6 s each in world / ESC menu / world / character screen / world, counted with
`ui.frame`; a quick save; 3 quick loads timed from the request to `session.start` / `world.ready`). `RisenTest.exe` (no
Radeon Chill, fps-limit.md), vsync off, 1600x900 window, RX 7900 XTX.

| run | ESC menu | character screen | world | quick load (s) | loading presents / load |
|---|---|---|---|---|---|
| before (no menu cap) | 2568 fps (16 draws) | 158 | 174-230 | 2.09-2.16 | ~400-1000 at ~220 fps |
| A: `vulkan.menu_fps` 60 (default) | **60.0** | **60.0** | 177-241 | 2.06-2.14 | 961-990 |
| B: + `vulkan.max_fps` 30 | 30.0 | 30.0 | 30.0 | **2.24-2.46** | 936-976 |
| C: B + `debug.pace_loading = true` (old behaviour) | 30.0 | 30.0 | 30.0 | **20.14-20.23** | 649 |
| vsync on (120 Hz), pacer skipped only | 60.0 | 60.0 | 120 | **5.64-5.67** | 830 at ~106/s (FIFO) |
| D: vsync on, loading frames thinned to 30 fps (final) | 60.0 | 60.0 | 120 | **2.06-2.11** | ~60 shown, ~730 not shown |
| E: vsync off, final | 60.0 | 60.0 | 178-241 | 2.05-2.15 | as D |

rvk.log `engine: menu state <game|menu|loading|unknown> (pages <bits>, app state <n>) at present <n>: ...` on every change
(ESC menu: pages 0x0 app state 0x1; character screen: pages 0xa app state 0; loads: pages 0x20000000, "unpaced"), plus
`vulkan: frame pacing at 60.000 fps (menu)` when the cap engages.

Not covered: the title screen of a normal start (the test runs autostart a new game: the first presents read `menu` with
pages 0, i.e. `GetMode() == 1`, as expected [I] for page 13); the map / log pages (same test as the character screen).
