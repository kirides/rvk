#pragma once

#include <filesystem>
#include <string>
#include <string_view>

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
    bool probe = true;         // log the Vulkan devices and their features at startup
    int adapter = -1;          // physical device index, -1 = best discrete GPU

    // [trace] (mode = "trace")
    int traceLogFirst = 3;     // calls per HAL slot logged with their arguments
    bool traceCapture = true;  // write shader bytecode / effect sources to <bin>/rvk/corpus
};

// Parses config.toml text. Unknown keys are ignored, bad values keep the default; `error` gets a parse error message.
Config parseConfig(std::string_view toml, std::string* error = nullptr);
// Reads the file; a missing file gives the defaults.
Config loadConfig(const std::filesystem::path& file, std::string* error = nullptr);

std::string_view toString(Mode mode);

}  // namespace rvk
