#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace rvk {

// What the proxy does with the game's graphics.
enum class Mode {
    Passthrough,  // the game's own D3D9 renderer, untouched (we only log and probe)
    Trace,        // the D3D9 renderer behind our tracing HAL wrappers (M1)
    Vulkan,       // our Vulkan HAL (M3+)
};

struct Config {
    Mode mode = Mode::Passthrough;
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
    // (vulkan) the overlay is drawn into the back buffer instead of the swapchain image, so screenshots (rvk_screenshot,
    // screenshot_frames, the engine snapshot) include it; default: captures never show the overlay
    bool overlayInScreenshots = false;
    // (vulkan) scripted overlay / settings commands at presents: "<frame>: <command>" (docs/research/overlay.md 5), e.g.
    // "450: overlay open", "600: set vulkan.max_fps 30", "700: ab flip", "800: key f10" (a posted WM_KEYDOWN/UP pair)
    std::vector<std::string> commands;
};

// Parses config.toml text. Unknown keys are ignored, bad values keep the default; `error` gets a parse error message.
Config parseConfig(std::string_view toml, std::string* error = nullptr);
// Reads the file; a missing file gives the defaults.
Config loadConfig(const std::filesystem::path& file, std::string* error = nullptr);

std::string_view toString(Mode mode);

}  // namespace rvk
