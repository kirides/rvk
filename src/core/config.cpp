#include "core/config.h"

#include <fstream>
#include <sstream>

#include <toml++/toml.hpp>

namespace rvk {

std::string_view toString(Mode mode) {
    switch (mode) {
    case Mode::Passthrough: return "passthrough";
    case Mode::Trace: return "trace";
    case Mode::Vulkan: return "vulkan";
    }
    return "?";
}

Config parseConfig(std::string_view text, std::string* error) {
    Config cfg;
    toml::table tbl;
    try {
        tbl = toml::parse(text);
    } catch (const toml::parse_error& e) {
        if (error != nullptr) {
            std::ostringstream os;
            os << e.description() << " (line " << e.source().begin.line << ")";
            *error = os.str();
        }
        return cfg;
    }

    if (auto mode = tbl["renderer"]["mode"].value<std::string>()) {
        if (*mode == "passthrough" || *mode == "d3d9") {
            cfg.mode = Mode::Passthrough;
        } else if (*mode == "trace") {
            cfg.mode = Mode::Trace;
        } else if (*mode == "vulkan") {
            cfg.mode = Mode::Vulkan;
        } else if (error != nullptr) {
            *error = "renderer.mode: unknown value '" + *mode + "'";
        }
    }
    cfg.logLevel = tbl["log"]["level"].value_or(cfg.logLevel);
    cfg.validation = tbl["vulkan"]["validation"].value_or(cfg.validation);
    cfg.probe = tbl["vulkan"]["probe"].value_or(cfg.probe);
    cfg.adapter = tbl["vulkan"]["adapter"].value_or(cfg.adapter);
    cfg.maxFps = tbl["vulkan"]["max_fps"].value_or(cfg.maxFps);
    cfg.pipelineLibraries = tbl["vulkan"]["pipeline_libraries"].value_or(cfg.pipelineLibraries);
    if (const auto v = tbl["vulkan"]["vsync"].value<bool>()) {
        cfg.vsync = *v ? 1 : 0;
    } else if (const auto str = tbl["vulkan"]["vsync"].value<std::string>(); str && *str != "engine" && error != nullptr) {
        *error = "vulkan.vsync: true, false or \"engine\"";
    }
    cfg.traceLogFirst = tbl["trace"]["log_first"].value_or(cfg.traceLogFirst);
    cfg.traceCapture = tbl["trace"]["capture"].value_or(cfg.traceCapture);
    cfg.logDraws = static_cast<std::uint32_t>(tbl["debug"]["log_draws"].value_or(0));
    cfg.logFrame = static_cast<std::uint32_t>(tbl["debug"]["log_frame"].value_or(0));
    cfg.dumpSavedTextures = tbl["debug"]["dump_saved_textures"].value_or(cfg.dumpSavedTextures);
    cfg.perf = tbl["debug"]["perf"].value_or(cfg.perf);
    cfg.perfFrame = static_cast<std::uint32_t>(tbl["debug"]["perf_frame"].value_or(0));
    const auto frameList = [&](const char* key, std::vector<std::uint32_t>& out) {
        if (const auto* frames = tbl["debug"][key].as_array()) {
            for (const auto& f : *frames) {
                if (const auto v = f.value<std::int64_t>(); v && *v > 0) {
                    out.push_back(static_cast<std::uint32_t>(*v));
                }
            }
        }
    };
    cfg.overlay = tbl["overlay"]["enabled"].value_or(cfg.overlay);
    cfg.overlayInScreenshots = tbl["debug"]["overlay_in_screenshots"].value_or(cfg.overlayInScreenshots);
    if (const auto* cmds = tbl["debug"]["commands"].as_array()) {
        for (const auto& c : *cmds) {
            if (const auto v = c.value<std::string>()) cfg.commands.push_back(*v);
        }
    }
    frameList("screenshot_frames", cfg.screenshotFrames);
    frameList("snapshot_frames", cfg.snapshotFrames);
    return cfg;
}

Config loadConfig(const std::filesystem::path& file, std::string* error) {
    std::ifstream in(file, std::ios::binary);
    if (!in) {
        return {};
    }
    std::ostringstream ss;
    ss << in.rdbuf();
    return parseConfig(ss.str(), error);
}

}  // namespace rvk
