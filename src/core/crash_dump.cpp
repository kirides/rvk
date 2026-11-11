#include "core/crash_dump.h"

#include "core/hooks.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <dbghelp.h>

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <ctime>
#include <mutex>
#include <string>

#pragma comment(lib, "dbghelp.lib")

namespace rvk::crashdump {
namespace {

std::mutex g_mutex;
std::filesystem::path g_dir;
std::atomic<int> g_written{0};
LPTOP_LEVEL_EXCEPTION_FILTER g_previous = nullptr;
std::atomic<bool> g_installed{false};
std::atomic<bool> g_exiting{false};
std::atomic<unsigned> g_exitCode{0};
std::atomic<int> g_exitReports{0};
DWORD g_mainThread = 0;
bool g_exitTrace = false;
constexpr int kMaxExitReports = 8;

std::filesystem::path moduleDir() {
    HMODULE self = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, reinterpret_cast<LPCWSTR>(&moduleDir), &self);
    wchar_t path[MAX_PATH]{};
    const DWORD n = GetModuleFileNameW(self, path, MAX_PATH);
    std::filesystem::path p(std::wstring(path, n < MAX_PATH ? n : 0));
    return p.parent_path();
}

// "Module.dll+0x1234" (or the raw address when no module holds it: an unloaded DLL, heap, garbage).
std::string where(std::uint64_t address) {
    char buf[320];
    HMODULE mod = nullptr;
    if (address != 0 && GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, reinterpret_cast<LPCWSTR>(address), &mod) &&
        mod != nullptr) {
        char path[MAX_PATH]{};
        const DWORD n = GetModuleFileNameA(mod, path, MAX_PATH);
        const char* name = path;
        for (DWORD i = 0; i < n; ++i)
            if (path[i] == '\\' || path[i] == '/') name = path + i + 1;
        std::snprintf(buf, sizeof(buf), "%s+0x%llx", name, static_cast<unsigned long long>(address - reinterpret_cast<std::uint64_t>(mod)));
    } else {
        std::snprintf(buf, sizeof(buf), "0x%016llx", static_cast<unsigned long long>(address));
    }
    return buf;
}

// The qword at `address` when it is committed readable memory (a register's pointee: an object's vtable, a vtable's slot).
bool readQword(std::uint64_t address, std::uint64_t& out) {
    if (address < 0x10000 || (address & 7) != 0) return false;
    MEMORY_BASIC_INFORMATION mbi{};
    if (VirtualQuery(reinterpret_cast<const void*>(address), &mbi, sizeof(mbi)) != sizeof(mbi)) return false;
    if (mbi.State != MEM_COMMIT || (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)) != 0) return false;
    if ((mbi.Protect & (PAGE_READONLY | PAGE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY)) == 0) return false;
    out = *reinterpret_cast<const std::uint64_t*>(address);
    return true;
}

// The text report of an unhandled exception: code, faulting address, registers (each with its module and, as a pointer, two levels of pointees: an
// object -> its vtable -> slot 0), the unwound stack (.pdata unwinding: works through the engine's frames without symbols) and the raw stack's
// words that point into modules (return-address candidates past a frame the unwinder could not walk). Plain Win32: a crash at process exit has no
// logger left (rvk.log's sink may be gone), and the minidump can fail there (0-byte files at the game's exit, 2026-11-11).
std::string crashReport(const EXCEPTION_RECORD* er, const CONTEXT& start) {
    std::string r;
    char line[512];
    CONTEXT ctx = start;
    if (er != nullptr) {
        std::snprintf(line, sizeof(line), "exception 0x%08lx at %s, thread %lu%s\r\n", er->ExceptionCode, where(reinterpret_cast<std::uint64_t>(er->ExceptionAddress)).c_str(),
                      GetCurrentThreadId(), GetCurrentThreadId() == g_mainThread ? " (the main thread)" : "");
    } else {
        std::snprintf(line, sizeof(line), "thread %lu at %s\r\n", GetCurrentThreadId(), where(ctx.Rip).c_str());
    }
    r += line;
    if (er != nullptr && er->ExceptionCode == EXCEPTION_ACCESS_VIOLATION && er->NumberParameters >= 2) {
        std::snprintf(line, sizeof(line), "access violation: %s of 0x%016llx\r\n",
                      er->ExceptionInformation[0] == 0 ? "read" : er->ExceptionInformation[0] == 1 ? "write" : "execute",
                      static_cast<unsigned long long>(er->ExceptionInformation[1]));
        r += line;
    }
    const struct {
        const char* name;
        std::uint64_t v;
    } regs[] = {{"rax", ctx.Rax}, {"rbx", ctx.Rbx}, {"rcx", ctx.Rcx}, {"rdx", ctx.Rdx}, {"rsi", ctx.Rsi}, {"rdi", ctx.Rdi}, {"rbp", ctx.Rbp}, {"rsp", ctx.Rsp},
                {"r8", ctx.R8},   {"r9", ctx.R9},   {"r10", ctx.R10}, {"r11", ctx.R11}, {"r12", ctx.R12}, {"r13", ctx.R13}, {"r14", ctx.R14}, {"r15", ctx.R15}};
    for (const auto& reg : regs) {
        std::string s = std::string(reg.name) + " = " + where(reg.v);
        std::uint64_t p1 = 0, p2 = 0;
        if (readQword(reg.v, p1)) {
            s += "  -> " + where(p1);
            if (readQword(p1, p2)) s += "  -> " + where(p2);
        }
        r += s + "\r\n";
    }
    r += "unwound stack:\r\n";
    for (int frame = 0; frame < 64 && ctx.Rip != 0; ++frame) {
        r += "  " + where(ctx.Rip) + "\r\n";
        DWORD64 imageBase = 0;
        PRUNTIME_FUNCTION fn = RtlLookupFunctionEntry(ctx.Rip, &imageBase, nullptr);
        if (fn == nullptr) {  // a leaf function: the return address is at [rsp]
            std::uint64_t ret = 0;
            if (!readQword(ctx.Rsp, ret)) break;
            ctx.Rip = ret;
            ctx.Rsp += 8;
            continue;
        }
        void* handlerData = nullptr;
        DWORD64 establisher = 0;
        RtlVirtualUnwind(UNW_FLAG_NHANDLER, imageBase, ctx.Rip, fn, &ctx, &handlerData, &establisher, nullptr);
    }
    r += "stack words pointing into modules (from the faulting rsp):\r\n";
    const std::uint64_t rsp = start.Rsp;
    int shown = 0;
    for (int i = 0; i < 1024 && shown < 48; ++i) {
        std::uint64_t v = 0;
        if (!readQword(rsp + 8ull * i, v)) break;
        HMODULE mod = nullptr;
        if (v < 0x10000 || !GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, reinterpret_cast<LPCWSTR>(v), &mod))
            continue;
        std::snprintf(line, sizeof(line), "  [rsp+0x%x] %s\r\n", i * 8, where(v).c_str());
        r += line;
        ++shown;
    }
    return r;
}

// A text file only (no minidump: at process exit MiniDumpWriteDump does not finish), "<dir>\rvk_<stamp>_<tag>_<n>.txt"; at most kMaxExitReports.
void writeText(const char* tag, const std::string& text) {
    const int n = g_exitReports.fetch_add(1);
    if (n >= kMaxExitReports) return;
    try {
        const std::filesystem::path dir = directory();
        std::error_code ec;
        std::filesystem::create_directories(dir, ec);
        std::time_t now = std::time(nullptr);
        std::tm tm{};
        localtime_s(&tm, &now);
        char name[96];
        std::strftime(name, sizeof(name), "rvk_%Y%m%d-%H%M%S_", &tm);
        const std::string base = std::string(name) + tag + "_" + std::to_string(n) + ".txt";
        HANDLE f = CreateFileW((dir / base).c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (f == INVALID_HANDLE_VALUE) return;
        DWORD written = 0;
        const std::string head = "tick " + std::to_string(GetTickCount64()) + "\r\n";
        WriteFile(f, head.data(), static_cast<DWORD>(head.size()), &written, nullptr);
        WriteFile(f, text.data(), static_cast<DWORD>(text.size()), &written, nullptr);
        CloseHandle(f);
    } catch (...) {
    }
}

// Exit diagnostics: after ExitProcess began, every access violation / illegal instruction / stack-buffer failure, first chance and on every thread
// (the main thread's DLL-detach faults never reach the unhandled-exception filter), is written as a report. Our own SEH-guarded reads
// (engine/memread) do not run after ExitProcess.
LONG WINAPI exitVectoredHandler(EXCEPTION_POINTERS* info) {
    if (!g_exiting.load(std::memory_order_relaxed) && !g_exitTrace) return EXCEPTION_CONTINUE_SEARCH;  // (RVK_EXIT_TRACE=1: every fault of the run)
    const DWORD code = info->ExceptionRecord->ExceptionCode;
    if (code != EXCEPTION_ACCESS_VIOLATION && code != EXCEPTION_ILLEGAL_INSTRUCTION && code != STATUS_STACK_BUFFER_OVERRUN && code != EXCEPTION_PRIV_INSTRUCTION)
        return EXCEPTION_CONTINUE_SEARCH;
    try {
        writeText("exitfault", (g_exiting.load() ? "first-chance exception after ExitProcess(" + std::to_string(g_exitCode.load()) + ")\r\n" : std::string("first-chance exception (before ExitProcess)\r\n")) +
                                   crashReport(info->ExceptionRecord, *info->ContextRecord));
    } catch (...) {
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

using ExitProcessFn = void(NTAPI*)(LONG);  // ntdll!RtlExitUserProcess (kernel32!ExitProcess and the CRT end there; hooking ExitProcess saw no call)
Hook<ExitProcessFn> g_exitProcess;

void NTAPI detourExitProcess(LONG code) {
    if (!g_exiting.exchange(true)) {
        g_exitCode = static_cast<unsigned>(code);
        g_mainThread = GetCurrentThreadId();
        if (g_exitTrace) {
            CONTEXT ctx{};
            RtlCaptureContext(&ctx);
            try {
                writeText("exit", "ExitProcess(" + std::to_string(code) + ")\r\n" + crashReport(nullptr, ctx));
            } catch (...) {
            }
        }
    }
    g_exitProcess.original(code);
}

LONG WINAPI unhandledFilter(EXCEPTION_POINTERS* info) {
    std::string report;
    try {
        report = crashReport(info->ExceptionRecord, *info->ContextRecord);
    } catch (...) {
    }
    write("crash", report, info);
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

bool installExitDiagnostics(bool traceExitCall) {
    g_exitTrace = traceExitCall;
    g_mainThread = GetCurrentThreadId();  // (DllMain runs on the game's main thread)
    HMODULE nt = GetModuleHandleW(L"ntdll.dll");
    auto* target = nt != nullptr ? reinterpret_cast<ExitProcessFn>(GetProcAddress(nt, "RtlExitUserProcess")) : nullptr;
    if (!g_exitProcess.install("ntdll!RtlExitUserProcess", target, &detourExitProcess)) return false;
    return AddVectoredExceptionHandler(1, &exitVectoredHandler) != nullptr;
}

bool exiting() { return g_exiting.load(std::memory_order_relaxed); }

void installUnhandledExceptionFilter() {
    if (g_installed.exchange(true)) return;
    g_previous = SetUnhandledExceptionFilter(&unhandledFilter);
}

}  // namespace rvk::crashdump
