// core/gpu_events: the cross-process GPU event board's selection rules (docs/research/rt-shadows.md 11.2)

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "core/gpu_events.h"
#include "test.h"

using namespace rvk;
using gpuevents::Event;
using gpuevents::Kind;

namespace {
Event ev(std::uint32_t pid, Kind k, std::int64_t ms, const char* detail = "") {
    Event e;
    e.pid = pid;
    e.kind = k;
    e.unixMs = ms;
    std::memcpy(e.detail, detail, std::min(std::strlen(detail), sizeof(e.detail) - 1));
    const wchar_t* exe = L"B:\\copy\\bin\\Risen.exe";
    for (std::size_t i = 0; exe[i] != 0; ++i) e.exe[i] = exe[i];
    return e;
}
}  // namespace

TEST(gpu_events_names_another_process_fault_before_an_own_loss) {
    // the 2026-11-11 incident: pid 26296 page-faulted, 13 s later pid 45396 lost its device without a fault of its own
    const std::int64_t t = 1'000'000;
    std::vector<Event> board = {ev(45396, Kind::DeviceCreated, t - 200'000), ev(26296, Kind::DeviceCreated, t - 80'000),
                                ev(26296, Kind::LostWithFault, t - 13'000, "vulkan, at vkQueueSubmit2"), ev(45396, Kind::Stall, t - 12'000)};
    const auto near = gpuevents::othersNear(board, 45396, t, 30'000);
    CHECK(near.size() == 1);
    CHECK(near[0].pid == 26296 && near[0].kind == Kind::LostWithFault);
    const std::string line = gpuevents::describe(near[0], t);
    CHECK(line.find("pid 26296") != std::string::npos);
    CHECK(line.find("page fault") != std::string::npos);
    CHECK(line.find("13.0 s before") != std::string::npos);
    CHECK(line.find("vkQueueSubmit2") != std::string::npos);
    CHECK(line.find("Risen.exe") != std::string::npos);
}

TEST(gpu_events_ignores_own_old_and_harmless_events) {
    const std::int64_t t = 5'000'000;
    std::vector<Event> board = {ev(1, Kind::LostWithFault, t - 1000),      // own
                                ev(2, Kind::LostWithFault, t - 60'000),    // too old
                                ev(3, Kind::DeviceCreated, t - 1000),      // not a loss / stall
                                ev(4, Kind::Lost, t + 3000),               // logged just after ours: kept
                                ev(5, Kind::Stall, t + 20'000),            // far after: dropped
                                ev(0, Kind::LostWithFault, t)};            // empty slot
    const auto near = gpuevents::othersNear(board, 1, t, 30'000);
    CHECK(near.size() == 1);
    CHECK(near[0].pid == 4);
    CHECK(gpuevents::describe(near[0], t).find("3.0 s after") != std::string::npos);
}

TEST(gpu_events_alive_uses_each_pid_latest_event) {
    const std::int64_t t = 10'000;
    std::vector<Event> board = {ev(7, Kind::DeviceCreated, t - 500), ev(7, Kind::Lost, t - 100),      // 7 is gone
                                ev(8, Kind::DeviceCreated, t - 900), ev(8, Kind::Stall, t - 50),      // 8 alive (newest first)
                                ev(9, Kind::DeviceCreated, t - 700), ev(1, Kind::DeviceCreated, t)};  // 9 alive, 1 = own
    const auto alive = gpuevents::othersAlive(board, 1);
    CHECK(alive.size() == 2);
    CHECK(alive[0].pid == 8 && alive[1].pid == 9);
}

TEST(gpu_events_board_records_without_listing_itself) {
    // the machine's real board: an own event is never reported as another process's (harmless kinds only: a game losing its device in the
    // next 30 s must not name this test)
    gpuevents::record(Kind::DeviceCreated, "rvk_tests (a test event)");
    for (const std::string& line : gpuevents::reportOthers()) {
        std::printf("  gpu events board: %s\n", line.c_str());  // (other renderer processes alive now: informational)
        CHECK(line.find("rvk_tests (a test event)") == std::string::npos);
    }
    gpuevents::record(Kind::DeviceDestroyed, "rvk_tests (a test event)");
}
