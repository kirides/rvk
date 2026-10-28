#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace rvk {

// What the proxy does with the game's graphics (the backend of the session).
enum class Mode {
    Passthrough,  // the game's own D3D9 renderer, untouched (we only log and probe): render mode Original
    Trace,        // the D3D9 renderer behind our tracing HAL wrappers (M1; a developer mode outside the three render modes)
    Vulkan,       // our Vulkan HAL (M3+): render modes Wrapper and Full
};

// The player's three render modes (setting renderer.mode, docs/PLAN.md decisions; option order = the setting's index):
//   Original: the system d3d9 forwarded, no Vulkan, no enhancements, no overlay (backend Passthrough)
//   Wrapper:  the D3D9 model on Vulkan + the enhancements, the engine draws the frame through the HAL (backend Vulkan)
//   Full:     Vulkan with the native frame taking over every draw (native.static_ops + native.scene_ops, all groups)
// Original <-> Wrapper / Full needs a restart (another backend); Wrapper <-> Full switches at a frame boundary.
enum class RenderMode : std::uint8_t { Original = 0, Wrapper = 1, Full = 2 };
inline constexpr RenderMode kDefaultRenderMode = RenderMode::Wrapper;  // the shipped default

struct Config {
    // the session's backend: parseConfig derives it from renderMode (absent: the default's); bootstrap resolves it again
    // from the player's renderer.mode at start (resolveStartMode)
    Mode mode = Mode::Vulkan;
    // [renderer] mode as written ("original" | "wrapper" | "full"; aliases "passthrough" / "d3d9" -> original, "vulkan" ->
    // wrapper; an unknown value or a broken file -> original, the safe one); nullopt = not set, or "trace"
    std::optional<RenderMode> renderMode;
    std::string logLevel = "info";

    // [vulkan]
    bool validation = false;   // VK_LAYER_KHRONOS_validation (needs the Vulkan SDK)
    bool syncValidation = false;  // + its synchronization validation (implies validation; slow)
    bool probe = true;         // log the Vulkan devices and their features at startup
    int adapter = -1;          // physical device index, -1 = best discrete GPU
    // vsync, max_fps and adapter are also player settings (settings.toml, core/settings.h); a non-default value here
    // overrides the player's for the session (developer runs: run.ps1 -Vsync / -MaxFps)
    double maxFps = 0.0;       // present-rate cap; 0 = the monitor's refresh rate with vsync, none without
    int vsync = -1;            // -1 = the engine's option, 0 = off, 1 = on ("engine" / false / true)
    bool pipelineLibraries = false;  // link pipelines from VK_EXT_graphics_pipeline_library libraries (AMD crash: pipeline_library.cpp)

    // [overlay]
    bool overlay = true;       // enabled: the in-game settings overlay (src/overlay); false = never created

    // [trace] (mode = "trace")
    int traceLogFirst = 3;     // calls per HAL slot logged with their arguments
    bool traceCapture = true;  // write shader bytecode / effect sources to <bin>/rvk/corpus

    // [debug]
    std::vector<std::uint32_t> screenshotFrames;  // (vulkan) back buffer -> <bin>/rvk/screenshots/frame_<n>.png at these frames
    std::uint32_t logDraws = 0;  // (vulkan) log the first N draws in detail
    std::uint32_t logFrame = 0;  // (vulkan) log the HAL calls of this frame
    // (any mode) at these eCApplication::Draw calls: the engine's snapshot (eCApplication::Snapshot -> HAL slot 159) plus a
    // self-test of the rvk_screenshot API on the same frame (<bin>/rvk/screenshots/api_draw<n>.*)
    std::vector<std::uint32_t> snapshotFrames;
    // (vulkan) eCAPITexture::SaveTextureToFile (always refused) also dumps the image to <bin>/rvk/dumps/<name>.png
    bool dumpSavedTextures = false;
    bool perf = false;            // (vulkan) GPU timestamps + CPU timings, a "perf:" line in rvk.log every 5 s
    std::uint32_t perfFrame = 0;  // (vulkan) log the GPU time of every render pass of this frame (needs perf)
    // (vulkan) build a tiny BLAS / TLAS and trace it with ray query at device creation (vk::rt::selfTest; "rt selftest" in
    // rvk.log); needs the ray tracing tier (rt.allow + a GPU with ray query)
    bool rtSelfTest = false;
    bool shaderDebugInfo = false;  // [debug] shader_debug_info: SPIR-V with names, source lines and the GLSL text (RenderDoc), optimizer off
    // (vulkan) the overlay is drawn into the back buffer instead of the swapchain image, so screenshots (rvk_screenshot,
    // screenshot_frames, the engine snapshot) include it; default: captures never show the overlay
    bool overlayInScreenshots = false;
    // (vulkan) scripted overlay / settings commands at presents: "<frame>: <command>" (docs/research/overlay.md 5), e.g.
    // "450: overlay open", "600: set vulkan.max_fps 30", "700: ab flip", "800: key f10" (a posted WM_KEYDOWN/UP pair)
    std::vector<std::string> commands;
    // (any mode) sample the presenting thread's stack while a present lasts longer than this many ms and log where the time
    // goes every 5 s (diag::StallSampler, docs/research/fps-limit.md); 0 = off. `present_stalls = true` = 4 ms
    double presentStalls = 0.0;
    // [debug] cpu_profile: a sampled flat profile of the engine thread between presents every 5 s (diag::frameSampler,
    // docs/research/performance.md 8); Vulkan mode
    bool cpuProfile = false;
    // (any mode) "NAME=VALUE" environment variables set in DllMain, before anything loads Vulkan or D3D (environment
    // variables of run.ps1 do not reach the game), e.g. "VK_LOADER_LAYERS_DISABLE=~implicit~"; "NAME=" removes one
    std::vector<std::string> environment;
};

// Parses config.toml text. Unknown keys are ignored, bad values keep the default; `error` gets a parse error message.
Config parseConfig(std::string_view toml, std::string* error = nullptr);
// Reads the file; a missing file gives the defaults.
Config loadConfig(const std::filesystem::path& file, std::string* error = nullptr);

std::string_view toString(Mode mode);
std::string_view toString(RenderMode mode);
// "original" / "wrapper" / "full" and the aliases ("passthrough", "d3d9", "vulkan"), case-sensitive like the TOML keys
std::optional<RenderMode> parseRenderMode(std::string_view text);
// the backend a render mode runs on
constexpr Mode backendOf(RenderMode m) { return m == RenderMode::Original ? Mode::Passthrough : Mode::Vulkan; }
// Original <-> Wrapper / Full needs a restart: they run on different backends
constexpr bool needsRestart(RenderMode started, RenderMode requested) { return backendOf(started) != backendOf(requested); }

// The start-time decision (bootstrap, after settings.toml is loaded). The player's renderer.mode (settings.toml) is the
// source of truth; config.toml's [renderer] mode is a developer override when it is not the default (wrapper): it locks the
// setting for the session, like [vulkan] vsync. "trace" locks it to Original (a D3D9 session).
// configModeLock: the value config.toml forces onto renderer.mode (nullopt = the player's value applies)
std::optional<RenderMode> configModeLock(const Config& cfg);
// the session's backend for the render mode it starts with (trace stays trace)
Mode sessionBackend(const Config& cfg, RenderMode started);
// the render mode a Vulkan session applies for a requested one: Original (pending a restart) renders as Wrapper
constexpr RenderMode vulkanSessionMode(RenderMode requested) {
    return requested == RenderMode::Full ? RenderMode::Full : RenderMode::Wrapper;
}

// What a Vulkan session takes over natively (HalDevice::applyStaticOps): Full turns the static and the scene ops on; the
// Debug overrides native.static_ops / native.scene_ops turn a stage on in Wrapper too (bisecting), native.scene_ops_groups
// masks the scene ops' groups in either mode, the compare settings work on top of both.
struct NativePlan {
    bool staticTake = false, staticCompare = false;
    bool sceneTake = false, sceneCompare = false;
    std::uint32_t groups = 0;  // native.scene_ops_groups (the scene ops' group bits)
    bool any() const { return staticTake || staticCompare || sceneTake || sceneCompare; }
};
inline constexpr std::uint32_t kAllSceneOpGroups = 131071;
NativePlan nativePlan(RenderMode mode, bool staticOverride, bool staticCompare, bool sceneOverride, bool sceneCompare, int groups);

}  // namespace rvk
