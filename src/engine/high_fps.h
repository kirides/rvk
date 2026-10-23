#pragma once

// High fps compatibility: the three per-frame gameplay bugs that VaanaCZ's Risen-FPS-Patch fixes for the 2009 build
// (https://github.com/VaanaCZ/Risen-FPS-Patch), ported to the x64 build (docs/research/fps-limit.md 12). Each is one call
// instruction redirected (FF 15 / call through the import table) to our replacement, through a slot or stub near the module:
//  * airwalk: gCCharacterMovement_PS::CheckChangedMovementStates' |dy| < 5 "stalled" test of a fall (g_fabsf at Game
//    0x8e5c7f) sees |dy| times frameScale, so a slow per-frame step at high fps is not mistaken for standing still
//  * thunder: eCWeatherAdmin::Process' per-frame thunder / lightning roll (bCMersenneTwister::Rand at Engine 0x710eaf) is
//    multiplied by frameScale: the chance per second stays that of 30 fps
//  * climbing: ProcessClimbHigh / ProcessClimbStandard turn the Y correction into a velocity with the frame time clamped to
//    >= 0.0084 s (bCVector::GetInvScaled at Game 0x8e8c31 / 0x8e90da); above ~119 fps the hero's Y is set directly instead
//    (as the patch does), X / Z keep the game's velocity
// The replacements read the setting each call (compat.high_fps): off = the game's behaviour. A site whose bytes do not match
// this build is left alone. Any mode (gameplay, not rendering).

namespace rvk::engine::highfps {

// Redirects the call sites (DllMain: Game.dll / Engine.dll are mapped, nothing is called); false when none could be patched
bool install();

// (1/30 s) / frameTime, clamped to [1, 100]; 1 for a non-positive frame time (time freeze). Pure (unit-tested).
float frameScale(float frameTimeSeconds);

}  // namespace rvk::engine::highfps
