#pragma once

// Crash / device-lost diagnostics (docs/research/dx12-game-port.md "Agility SDK 620: debugging"): a minidump + a text report in
// <bin>\rvk\dumps\ (never into the game's data). Used by both backends when the GPU device is lost (Vulkan: VK_EXT_device_fault report;
// D3D12: DRED breadcrumbs + page fault data) and, opt in, by an unhandled-exception filter.

#include <filesystem>
#include <string>
#include <string_view>

namespace rvk::crashdump {

// Where dumps go (created on first use). Unset: <directory of this module>\dumps.
void setDirectory(std::filesystem::path dir);
std::filesystem::path directory();

// "<dir>\<prefix>_<yyyymmdd-hhmmss>_<tag>.dmp" (a MiniDumpWriteDump of this process: threads, modules, handle data; no full memory) and, when `report`
// is not empty, the same name with ".txt". Returns the dump's path (empty when it could not be written). `exception` may be null.
// Never throws; safe to call from any thread, at most `kMaxDumps` files per run are written (a looping failure must not fill the disk).
std::filesystem::path write(std::string_view tag, std::string_view report, void* exceptionPointers = nullptr);

// SetUnhandledExceptionFilter: writes a dump named "crash" and chains to the previous filter (the engine's own handler may replace ours later: then no
// dump of ours, nothing else changes). Idempotent.
void installUnhandledExceptionFilter();

constexpr int kMaxDumps = 4;

}  // namespace rvk::crashdump
