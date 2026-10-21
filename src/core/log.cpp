#include "core/log.h"

#include <memory>
#include <string>
#include <vector>

#include <spdlog/sinks/basic_file_sink.h>
#include <spdlog/sinks/msvc_sink.h>
#include <spdlog/spdlog.h>

namespace rvk::log {

void init(const std::filesystem::path& file, std::string_view level) {
    std::error_code ec;
    std::filesystem::create_directories(file.parent_path(), ec);

    std::vector<spdlog::sink_ptr> sinks;
    try {
        sinks.push_back(std::make_shared<spdlog::sinks::basic_file_sink_mt>(file.string(), /*truncate=*/true));
    } catch (const std::exception&) {
        // no log file (read-only directory?): the debugger sink still works
    }
    sinks.push_back(std::make_shared<spdlog::sinks::msvc_sink_mt>(/*check_debugger_present=*/true));

    auto logger = std::make_shared<spdlog::logger>("rvk", sinks.begin(), sinks.end());
    logger->set_pattern("[%H:%M:%S.%e] [%t] [%l] %v");
    logger->set_level(spdlog::level::from_str(std::string(level)));
    // the game may die without a clean shutdown (crash, TerminateProcess): flush every line from info up
    logger->flush_on(spdlog::level::info);
    spdlog::set_default_logger(std::move(logger));
}

void shutdown() {
    if (auto l = spdlog::default_logger()) {
        l->flush();
    }
}

}  // namespace rvk::log
