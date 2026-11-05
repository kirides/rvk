#include "core/crash_dump.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <dbghelp.h>

#include <atomic>
#include <cstdio>
#include <ctime>
#include <mutex>

#pragma comment(lib, "dbghelp.lib")

namespace rvk::crashdump {
namespace {

std::mutex g_mutex;
std::filesystem::path g_dir;
std::atomic<int> g_written{0};
LPTOP_LEVEL_EXCEPTION_FILTER g_previous = nullptr;
std::atomic<bool> g_installed{false};

std::filesystem::path moduleDir() {
    HMODULE self = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, reinterpret_cast<LPCWSTR>(&moduleDir), &self);
    wchar_t path[MAX_PATH]{};
    const DWORD n = GetModuleFileNameW(self, path, MAX_PATH);
    std::filesystem::path p(std::wstring(path, n < MAX_PATH ? n : 0));
    return p.parent_path();
}

LONG WINAPI unhandledFilter(EXCEPTION_POINTERS* info) {
    write("crash", {}, info);
    return g_previous != nullptr ? g_previous(info) : EXCEPTION_CONTINUE_SEARCH;
}

}  // namespace

void setDirectory(std::filesystem::path dir) {
    const std::lock_guard lock(g_mutex);
    g_dir = std::move(dir);
}

std::filesystem::path directory() {
    const std::lock_guard lock(g_mutex);
    return g_dir.empty() ? moduleDir() / L"dumps" : g_dir;
}

std::filesystem::path write(std::string_view tag, std::string_view report, void* exceptionPointers) {
    if (g_written.fetch_add(1) >= kMaxDumps) return {};
    try {
        const std::filesystem::path dir = directory();
        std::error_code ec;
        std::filesystem::create_directories(dir, ec);
        std::time_t now = std::time(nullptr);
        std::tm tm{};
        localtime_s(&tm, &now);
        char stamp[32];
        std::strftime(stamp, sizeof(stamp), "%Y%m%d-%H%M%S", &tm);
        const std::string base = std::string("rvk_") + stamp + "_" + std::string(tag);
        const std::filesystem::path dump = dir / (base + ".dmp");
        if (!report.empty()) {
            if (std::FILE* f = _wfopen((dir / (base + ".txt")).c_str(), L"wb")) {
                std::fwrite(report.data(), 1, report.size(), f);
                std::fclose(f);
            }
        }
        HANDLE file = CreateFileW(dump.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file == INVALID_HANDLE_VALUE) return {};
        MINIDUMP_EXCEPTION_INFORMATION mei{};
        MINIDUMP_EXCEPTION_INFORMATION* meiPtr = nullptr;
        if (exceptionPointers != nullptr) {
            mei.ThreadId = GetCurrentThreadId();
            mei.ExceptionPointers = static_cast<EXCEPTION_POINTERS*>(exceptionPointers);
            mei.ClientPointers = FALSE;
            meiPtr = &mei;
        }
        const auto type = static_cast<MINIDUMP_TYPE>(MiniDumpWithDataSegs | MiniDumpWithHandleData | MiniDumpWithThreadInfo | MiniDumpWithUnloadedModules |
                                                     MiniDumpWithIndirectlyReferencedMemory);
        const BOOL ok = MiniDumpWriteDump(GetCurrentProcess(), GetCurrentProcessId(), file, type, meiPtr, nullptr, nullptr);
        CloseHandle(file);
        if (!ok) {
            std::filesystem::remove(dump, ec);
            return {};
        }
        return dump;
    } catch (...) {
        return {};
    }
}

void installUnhandledExceptionFilter() {
    if (g_installed.exchange(true)) return;
    g_previous = SetUnhandledExceptionFilter(&unhandledFilter);
}

}  // namespace rvk::crashdump
