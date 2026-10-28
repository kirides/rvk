#include "core/config.h"

#include <algorithm>
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

std::string_view toString(RenderMode mode) {
    switch (mode) {
    case RenderMode::Original: return "original";
    case RenderMode::Wrapper: return "wrapper";
    case RenderMode::Full: return "full";
    }
    return "?";
}

std::optional<RenderMode> parseRenderMode(std::string_view text) {
    if (text == "original" || text == "passthrough" || text == "d3d9") return RenderMode::Original;
    if (text == "wrapper" || text == "vulkan") return RenderMode::Wrapper;
    if (text == "full") return RenderMode::Full;
    return std::nullopt;
}

std::optional<RenderMode> configModeLock(const Config& cfg) {
    if (cfg.mode == Mode::Trace) return RenderMode::Original;
    if (cfg.renderMode && *cfg.renderMode != kDefaultRenderMode) return cfg.renderMode;
    return std::nullopt;
}

Mode sessionBackend(const Config& cfg, RenderMode started) {
    return cfg.mode == Mode::Trace ? Mode::Trace : backendOf(started);
}

NativePlan nativePlan(RenderMode mode, bool staticOverride, bool staticCompare, bool sceneOverride, bool sceneCompare, int groups) {
    const bool full = mode == RenderMode::Full;
    NativePlan p;
    p.staticTake = full || staticOverride;
    p.staticCompare = staticCompare;
    p.sceneTake = full || sceneOverride;
    p.sceneCompare = sceneCompare;
    p.groups = static_cast<std::uint32_t>(groups) & kAllSceneOpGroups;
    return p;
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
        // a broken file: the safe mode (the game's own renderer), locked like an explicit "original"
        cfg.renderMode = RenderMode::Original;
        cfg.mode = Mode::Passthrough;
        return cfg;
    }

    if (auto mode = tbl["renderer"]["mode"].value<std::string>()) {
        if (*mode == "trace") {
            cfg.mode = Mode::Trace;
        } else if (const auto m = parseRenderMode(*mode)) {
            cfg.renderMode = *m;
        } else {
            cfg.renderMode = RenderMode::Original;
            if (error != nullptr) *error = "renderer.mode: unknown value '" + *mode + "' (original, wrapper, full): original";
        }
    }
    if (cfg.mode != Mode::Trace) cfg.mode = backendOf(cfg.renderMode.value_or(kDefaultRenderMode));
    cfg.logLevel = tbl["log"]["level"].value_or(cfg.logLevel);
    cfg.validation = tbl["vulkan"]["validation"].value_or(cfg.validation);
    cfg.syncValidation = tbl["vulkan"]["sync_validation"].value_or(cfg.syncValidation);
    cfg.validation = cfg.validation || cfg.syncValidation;
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
    cfg.rtSelfTest = tbl["debug"]["rt_selftest"].value_or(cfg.rtSelfTest);
    cfg.shaderDebugInfo = tbl["debug"]["shader_debug_info"].value_or(cfg.shaderDebugInfo);
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
    if (const auto stalls = tbl["debug"]["present_stalls"]; stalls.is_boolean()) {
        cfg.presentStalls = stalls.value_or(false) ? 4.0 : 0.0;
    } else {
        cfg.presentStalls = std::max(0.0, stalls.value_or(0.0));
    }
    cfg.cpuProfile = tbl["debug"]["cpu_profile"].value_or(cfg.cpuProfile);
    if (const auto* env = tbl["debug"]["environment"].as_array()) {
        for (const auto& e : *env) {
            if (const auto v = e.value<std::string>(); v && v->find('=') != std::string::npos) cfg.environment.push_back(*v);
        }
    }
    frameList("screenshot_frames", cfg.screenshotFrames);
    frameList("snapshot_frames", cfg.snapshotFrames);
    return cfg;
}

Config loadConfig(const std::filesystem::path& file, std::string* error) {
    std::ifstream in(file, std::ios::binary);
    if (!in) {
        return parseConfig("", nullptr);
    }
    std::ostringstream ss;
    ss << in.rdbuf();
    return parseConfig(ss.str(), error);
}

}  // namespace rvk
