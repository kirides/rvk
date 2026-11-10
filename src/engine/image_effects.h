#pragma once

// Debug commands that switch the engine's post image effect at run time (S-7d, docs/research/frame-ownership.md 14.18), so test runs draw the rare post
// effects the game starts only from effect data (RadialBlur, Wobble, EdgeSmooth) or camera types / hits (OverlayBlackScreen, OverlayWhiteScreen, OverlayHit):
//   post effect <name>                       eCImageFilterAdmin::ActivateEffect(name): the scene effect (admin +0x90) until replaced
//   post default                             ActivateDefaultEffect: the config's /Engine/Render/PostProcessing/DefaultEffect again
//   post overlay <name> [dur [in [out]]]     StartOverlayEffect(name, duration, fade-in, fade-out) (seconds; duration <= 0 = until stopped)
//   post overlay stop [fade]                 StopOverlayEffect(fade)
// Run at presents ([debug] commands): the engine thread, between frames (plain field writes + reference counts, read by the next frame's Execute).

#include <string>
#include <string_view>

namespace rvk::engine::imagefx {

// `rest` = the command after "post"; false + `out` = the failure
bool runCommand(std::string_view rest, std::string* out);

}  // namespace rvk::engine::imagefx
