#pragma once

// GPU events shared between processes (docs/research/rt-shadows.md 11.2): every renderer process on this machine (one per Risen.exe that loaded
// our d3d9.dll, any copy / instance / worktree build) writes its device creation, GPU stalls and device losses into one small named shared
// memory board (session local). A device loss / stall then names the OTHER renderer processes that share the GPU and what happened to them just
// before: on AMD a page fault (or a hang) in one process makes the driver reset the GPU, which can stall and then lose the device of every other
// process on it, without a fault report of its own (2026-11-11: a water-compare run "hung" at a spot 1 s after another test run's page fault).
// The board is diagnostics only: nothing reads it to decide anything; a process that cannot open it just logs nothing extra.

#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace rvk::gpuevents {

enum class Kind : std::uint32_t {
    None = 0,
    DeviceCreated = 1,  // this process created its GPU device (= shares the GPU from now on)
    Stall = 2,          // a frame wait went past 4 s (Vulkan) / 10 s (D3D12)
    LostWithFault = 3,  // device lost / removed and the driver reported a page fault / DRED data
    Lost = 4,           // device lost / removed without a fault report (a victim of a reset, a TDR, ...)
    DeviceDestroyed = 5,
};

struct Event {
    std::uint32_t pid = 0;
    Kind kind = Kind::None;
    std::int64_t unixMs = 0;     // wall clock (ms since 1970) when it was written
    char detail[112] = {};       // what / where (NUL terminated, cut)
    wchar_t exe[128] = {};       // the process's exe path (cut)
};

constexpr std::uint32_t kSlots = 32;

// Pure (rvk_tests): the events of other processes worth naming next to an own event at `nowMs`: losses / stalls of another pid within
// `windowMs` before (or shortly after) it, newest first; at most `maxCount`.
std::vector<Event> othersNear(std::span<const Event> events, std::uint32_t ownPid, std::int64_t nowMs, std::int64_t windowMs, std::size_t maxCount = 6);
// Pure: the other pids whose latest event is DeviceCreated / Stall (= a device that was alive at their last word), newest first
std::vector<Event> othersAlive(std::span<const Event> events, std::uint32_t ownPid);
// one line per event: "pid 123 (B:\...\Risen.exe) page fault 1.0 s before: <detail>"
std::string describe(const Event& e, std::int64_t nowMs);

// The board (Windows, shared memory "Local\rvk_gpu_events_v1"). Never throws.
void record(Kind kind, const std::string& detail);
// The lines to log for an own loss / stall: other processes' losses within 30 s and the other renderer processes still alive (OpenProcess); empty when
// there are none (or the board is not available).
std::vector<std::string> reportOthers();

}  // namespace rvk::gpuevents
