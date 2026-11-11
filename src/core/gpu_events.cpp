#include "core/gpu_events.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <format>
#include <mutex>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

namespace rvk::gpuevents {

namespace {

bool isLossOrStall(Kind k) { return k == Kind::Stall || k == Kind::LostWithFault || k == Kind::Lost; }

const char* kindText(Kind k) {
    switch (k) {
    case Kind::DeviceCreated: return "created its device";
    case Kind::Stall: return "GPU stall";
    case Kind::LostWithFault: return "DEVICE LOST with a page fault / fault report";
    case Kind::Lost: return "device lost (no fault report)";
    case Kind::DeviceDestroyed: return "destroyed its device";
    default: return "?";
    }
}

std::string narrow(const wchar_t* w) {
    std::string s;
    for (std::size_t i = 0; i < 128 && w[i] != 0; ++i) s.push_back(w[i] < 128 ? static_cast<char>(w[i]) : '?');
    return s;
}

std::int64_t nowUnixMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
}

constexpr std::uint32_t kMagic = 0x31455652u;  // "RVE1"

struct Board {
    std::uint32_t magic;
    std::uint32_t size;
    volatile LONG next;
    std::uint32_t pad;
    Event events[kSlots];
};

Board* board() {
    static std::once_flag once;
    static Board* b = nullptr;
    std::call_once(once, [] {
        HANDLE h = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, sizeof(Board), L"Local\\rvk_gpu_events_v1");
        if (h == nullptr) return;
        const bool existed = GetLastError() == ERROR_ALREADY_EXISTS;
        void* p = MapViewOfFile(h, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(Board));
        if (p == nullptr) {
            CloseHandle(h);
            return;
        }
        // (the handle stays open for the process's life: the mapping lives while any renderer process holds it)
        auto* bb = static_cast<Board*>(p);
        if (!existed || bb->magic != kMagic || bb->size != sizeof(Board)) {
            // a fresh board (zeroed by the system) or one of another layout: (re)initialise; a racing creator writes the same values
            if (existed && bb->magic != 0 && (bb->magic != kMagic || bb->size != sizeof(Board))) {
                UnmapViewOfFile(p);
                return;  // another version's board under our name: leave it alone
            }
            bb->size = sizeof(Board);
            bb->magic = kMagic;
        }
        b = bb;
    });
    return b;
}

}  // namespace

std::vector<Event> othersNear(std::span<const Event> events, std::uint32_t ownPid, std::int64_t nowMs, std::int64_t windowMs, std::size_t maxCount) {
    std::vector<Event> out;
    for (const Event& e : events) {
        if (e.pid == 0 || e.pid == ownPid || !isLossOrStall(e.kind)) continue;
        // before the own event by at most the window; a few seconds after it too (the other process may log its loss later than ours)
        if (e.unixMs < nowMs - windowMs || e.unixMs > nowMs + 5000) continue;
        out.push_back(e);
    }
    std::sort(out.begin(), out.end(), [](const Event& a, const Event& b) { return a.unixMs > b.unixMs; });
    if (out.size() > maxCount) out.resize(maxCount);
    return out;
}

std::vector<Event> othersAlive(std::span<const Event> events, std::uint32_t ownPid) {
    std::vector<Event> latest;  // per pid its newest event
    for (const Event& e : events) {
        if (e.pid == 0 || e.pid == ownPid || e.kind == Kind::None) continue;
        auto it = std::find_if(latest.begin(), latest.end(), [&](const Event& l) { return l.pid == e.pid; });
        if (it == latest.end()) latest.push_back(e);
        else if (e.unixMs >= it->unixMs) *it = e;
    }
    std::vector<Event> out;
    for (const Event& e : latest) {
        if (e.kind == Kind::DeviceCreated || e.kind == Kind::Stall) out.push_back(e);
    }
    std::sort(out.begin(), out.end(), [](const Event& a, const Event& b) { return a.unixMs > b.unixMs; });
    return out;
}

std::string describe(const Event& e, std::int64_t nowMs) {
    const double dt = static_cast<double>(nowMs - e.unixMs) / 1000.0;
    std::string detail(e.detail, strnlen(e.detail, sizeof(e.detail)));
    return std::format("pid {} ({}): {} {:.1f} s {}{}{}", e.pid, narrow(e.exe), kindText(e.kind), dt >= 0.0 ? dt : -dt, dt >= 0.0 ? "before" : "after",
                       detail.empty() ? "" : ": ", detail);
}

void record(Kind kind, const std::string& detail) {
    Board* b = board();
    if (b == nullptr) return;
    const LONG n = InterlockedIncrement(&b->next) - 1;
    Event& e = b->events[static_cast<std::uint32_t>(n) % kSlots];
    // the pid goes in last: a reader that sees a zero pid skips the slot (diagnostics: a torn read of a slot being rewritten is harmless)
    InterlockedExchange(reinterpret_cast<volatile LONG*>(&e.pid), 0);
    e.kind = kind;
    e.unixMs = nowUnixMs();
    std::memset(e.detail, 0, sizeof(e.detail));
    std::memcpy(e.detail, detail.data(), std::min(detail.size(), sizeof(e.detail) - 1));
    std::memset(e.exe, 0, sizeof(e.exe));
    GetModuleFileNameW(nullptr, e.exe, static_cast<DWORD>(std::size(e.exe) - 1));
    InterlockedExchange(reinterpret_cast<volatile LONG*>(&e.pid), static_cast<LONG>(GetCurrentProcessId()));
}

std::vector<std::string> reportOthers() {
    std::vector<std::string> lines;
    Board* b = board();
    if (b == nullptr) return lines;
    std::vector<Event> snap(b->events, b->events + kSlots);
    const std::uint32_t own = GetCurrentProcessId();
    const std::int64_t now = nowUnixMs();
    for (const Event& e : othersNear(snap, own, now, 30000)) {
        lines.push_back("another renderer process on this GPU: " + describe(e, now));
    }
    for (const Event& e : othersAlive(snap, own)) {
        HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, e.pid);
        if (h == nullptr) continue;
        DWORD code = 0;
        bool alive = GetExitCodeProcess(h, &code) && code == STILL_ACTIVE;
        if (alive) {  // a reused pid: the exe must be the one that wrote the event
            wchar_t path[MAX_PATH] = {};
            DWORD n = MAX_PATH;
            if (QueryFullProcessImageNameW(h, 0, path, &n)) alive = _wcsnicmp(path, e.exe, std::size(e.exe) - 1) == 0;
        }
        CloseHandle(h);
        if (alive) lines.push_back("another renderer process shares this GPU: " + describe(e, now));
    }
    return lines;
}

}  // namespace rvk::gpuevents
