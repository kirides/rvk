#pragma once

// The engine's scene exposure (docs/research/upscaling.md 18, engine-frame/F-post-water-gui.md 2.3 / 2.4): the eye adaptation
// chain keeps the adapted average luminance in a persistent 1x1 R32F image (the "AvgLuminance" copy filter of the AvgLuminance
// effect; job 9 of the default frame), and the ToneMap filter scales the scene by ToneMapFactor / avgLum. FSR gets the same
// adapted luminance as its exposure input (in its own convention, kFsrExposureKey). The image holds the value of the previous
// frame's post chain when the scene job (where FSR runs) starts.
// Reads of engine memory are SEH-guarded; any thread, but the engine's graph is only stable on the render thread.

namespace rvk::engine::exposure {

// FSR's exposure convention: scene colour x exposure is 1 at the luminance FSR treats as saturated. Its own automatic exposure is
// 1 / Lmax with Lmax = (78 / (0.65 x 100)) x 2^log2(Lavg x 100 / 12.5) = 9.6 x Lavg (Lavg = the scene's geometric mean luminance,
// the same quantity the engine's LogLuminance chain adapts). The engine's ToneMap key (ToneMapFactor 0.6 / avgLum) is a middle-grey
// key, not that: fed to FSR it was 5.7x too large (measured: softer picture, upscaling.md 18).
inline constexpr float kFsrExposureKey = 1.0f / 9.6f;

// The eCAPIImage (the HAL's texture handle: resolve it with d3d9::Device::engineTexture) of the adapted luminance, or nullptr
// while the scene effect has not run its luminance jobs yet (the image is allocated by the first one), the effect has no
// AvgLuminance (the Bloom-only default) or the engine layout is not the verified one.
const void* adaptedLuminanceApi();

}  // namespace rvk::engine::exposure
