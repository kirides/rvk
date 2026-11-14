#pragma once
// frame-ownership.md 7.27: the owned chain's per-op / per-draw stage timers (compaction, stamps, build, derive, side effects, ...: the microsecond fields of the
// `native: owned ...` 5 s lines) cost two clock reads each (~25 ns): measured +0.05 .. 0.1 ms per frame. Off by default, `RVK_OWNED_TIMERS=1` (or
// RVK_F2C4_MEASURE=1) turns them on; with them off those fields read 0 and the counters are unchanged.
#include <chrono>

extern "C" __declspec(dllimport) unsigned long __stdcall GetEnvironmentVariableA(const char* name, char* buffer, unsigned long size);

namespace rvk::native {

// (the Win32 environment, read at the first use: `-Environment` variables are set by the proxy's DllMain, after the static initialisers)
inline int g_ownedTimers = -1;
inline bool ownedTimersOn() {
    if (g_ownedTimers < 0) [[unlikely]] {
        char a[4] = {}, b[4] = {};
        const bool on = (GetEnvironmentVariableA("RVK_OWNED_TIMERS", a, sizeof(a)) != 0 && a[0] != '0') || (GetEnvironmentVariableA("RVK_F2C4_MEASURE", b, sizeof(b)) != 0 && b[0] != '0');
        g_ownedTimers = on ? 1 : 0;
    }
    return g_ownedTimers != 0;
}

// steady_clock::now() while the timers are on, else the epoch (both ends of an interval are then equal: ns() = 0)
inline std::chrono::steady_clock::time_point tnow() { return ownedTimersOn() ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{}; }

// WS15: ownedPass' phases (ns, summed over the passes of a 5 s window; printed with the pass profile): 0 view (composeView), 1 view table + windows, 2 register save + state,
// 3 record lookups + scope + reserve + bulk begin, 4 the op loop (incl. the flushes inside), 5 end (bulk flush, restore), 6 passes counted
inline std::uint64_t g_passPhaseNs[8] = {};
inline std::uint64_t g_passPhaseCount[3] = {};  // [0] owned passes, [1] with persistent, [2] ops

}  // namespace rvk::native
