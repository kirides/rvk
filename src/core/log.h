#pragma once

#include <filesystem>
#include <string_view>

namespace rvk::log {

// File logger (+ OutputDebugString) as spdlog's default logger. Safe in DllMain: creates no threads.
void init(const std::filesystem::path& file, std::string_view level);
void shutdown();

}  // namespace rvk::log
