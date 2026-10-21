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
    cfg.traceLogFirst = tbl["trace"]["log_first"].value_or(cfg.traceLogFirst);
    cfg.traceCapture = tbl["trace"]["capture"].value_or(cfg.traceCapture);
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
