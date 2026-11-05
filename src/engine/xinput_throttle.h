#pragma once

// The engine polls XInputGetState / XInputSetState for four pads every frame (eCInputAdmin::UpdateX360). A pad that is not
// connected makes xinput1_4 talk to the driver (NtDeviceIoControlFile, through the Steam overlay's hook): 0.13 - 0.16 ms per
// frame at the savegame, 2.5 - 3 % of the engine thread (docs/research/performance.md 21). Microsoft's own advice is to poll
// a disconnected user index rarely. After a call answered ERROR_DEVICE_NOT_CONNECTED, the next calls for that index within
// `intervalMs` get the same answer without entering the library; a connected pad is polled every frame as before.
namespace rvk::engine::xinput {

// 0 = off (every call goes through), else the re-poll interval of a disconnected index (ms)
void setIntervalMs(int ms);

// Engine thread, once per present: installs the hooks as soon as xinput1_4.dll is loaded (the engine loads it lazily)
void onPresent();

}  // namespace rvk::engine::xinput
