// core/crash_dump: a minidump + report file in the chosen directory (docs/research/dx12-game-port.md "Agility SDK 620: debugging")

#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>

#include "core/caps.h"
#include "core/crash_dump.h"
#include "core/log.h"

#include <spdlog/spdlog.h>
#include "test.h"

using namespace rvk;

TEST(crash_dump_writes_minidump_and_report) {
    namespace fs = std::filesystem;
    const fs::path dir = fs::temp_directory_path() / "rvk_crash_dump_test";
    std::error_code ec;
    fs::remove_all(dir, ec);
    crashdump::setDirectory(dir);
    CHECK(crashdump::directory() == dir);
    const fs::path dump = crashdump::write("unit_test", "report line\r\n");
    CHECK(!dump.empty());
    CHECK(fs::exists(dump, ec) && fs::file_size(dump, ec) > 1024);
    fs::path txt = dump;
    txt.replace_extension(".txt");
    std::ifstream in(txt, std::ios::binary);
    const std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    CHECK(text == "report line\r\n");
    fs::remove_all(dir, ec);
}

TEST(caps_vendor_rt_entries_are_named) {
    CHECK(caps::name(caps::Cap::ClusterAccelerationStructure) == "cluster_acceleration_structure");
    CHECK(caps::name(caps::Cap::RayTracingReorder) == "ray_tracing_reorder");
    CHECK(!caps::label(caps::Cap::RayTracingReorder).empty());
}

// WS22 review: after enterExitMode (the exit hooks) the caller writes: lines of the exit path are in the file, flushSync does not wait
TEST(log_exit_mode_writes_on_the_caller) {
    namespace fs = std::filesystem;
    const fs::path file = fs::temp_directory_path() / "rvk_log_exit_test" / "rvk.log";
    std::error_code ec;
    fs::remove_all(file.parent_path(), ec);
    auto previous = spdlog::default_logger();
    log::init(file, "info");
    log::startAsync();
    spdlog::info("exit-test before");
    log::enterExitMode();
    spdlog::info("exit-test after");
    const auto t0 = std::chrono::steady_clock::now();
    log::flushSync();
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
    CHECK(ms < 500);
    spdlog::default_logger()->flush();
    std::ifstream in(file, std::ios::binary);
    const std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    CHECK(text.find("exit-test before") != std::string::npos);
    CHECK(text.find("exit-test after") != std::string::npos);
    spdlog::set_default_logger(previous);
}
