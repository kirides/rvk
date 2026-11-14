#pragma once

#include <filesystem>
#include <string_view>

namespace rvk::log {

// File logger (+ OutputDebugString) as spdlog's default logger. Safe in DllMain: creates no threads.
void init(const std::filesystem::path& file, std::string_view level);
void shutdown(bool processTerminating = false);
// WS22 (frame-ownership.md 16.8): the file / debugger writes and flushes of every line on one logger thread (spdlog's async logger on the same sinks; the caller still formats). Not in DllMain
// (it creates a thread): called once the device exists. `RVK_LOG_SYNC=1` keeps the synchronous logger. Idempotent.
void startAsync();
bool asyncActive();
// Waits (at most 2 s) until every line logged so far is written and flushed: the shutdown, the terminate hook, the overlay's quit, the crash filters. No-op for the synchronous logger.
void flushSync();

}  // namespace rvk::log
