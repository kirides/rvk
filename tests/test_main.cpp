// Unit tests for pure logic (no game, no GPU). Run: build\RelWithDebInfo\rvk_tests.exe (or ctest -C RelWithDebInfo --test-dir build).

#include <cstdint>
#include <iterator>

#include "core/config.h"
#include "shader/sm_tokens.h"
#include "test.h"

namespace rvk::test {
std::vector<TestCase>& registry() {
    static std::vector<TestCase> r;
    return r;
}
int& failures() {
    static int n = 0;
    return n;
}
}  // namespace rvk::test

TEST(config_defaults) {
    const rvk::Config c = rvk::parseConfig("");
    // no [renderer] mode: the player's renderer.mode decides (default wrapper, a Vulkan backend)
    CHECK(c.mode == rvk::Mode::Vulkan);
    CHECK(!c.renderMode.has_value());
    CHECK(!rvk::configModeLock(c).has_value());
    CHECK(c.logLevel == "info");
    CHECK(!c.validation);
    CHECK(c.probe);
    CHECK(c.adapter == -1);
}

TEST(config_values) {
    std::string err;
    const rvk::Config c = rvk::parseConfig(R"(
[renderer]
mode = "vulkan"
[log]
level = "debug"
[vulkan]
validation = true
probe = false
adapter = 1
)",
                                           &err);
    CHECK(err.empty());
    CHECK(c.mode == rvk::Mode::Vulkan);
    CHECK(c.renderMode == rvk::RenderMode::Wrapper);  // "vulkan" = the old name of wrapper
    CHECK(c.logLevel == "debug");
    CHECK(c.validation);
    CHECK(!c.probe);
    CHECK(c.adapter == 1);
}

TEST(config_present_stalls_environment) {
    CHECK(rvk::parseConfig("").presentStalls == 0.0);
    CHECK(rvk::parseConfig("[debug]\npresent_stalls = true\n").presentStalls == 4.0);
    CHECK(rvk::parseConfig("[debug]\npresent_stalls = false\n").presentStalls == 0.0);
    CHECK(rvk::parseConfig("[debug]\npresent_stalls = 8.5\n").presentStalls == 8.5);
    CHECK(rvk::parseConfig("[debug]\npresent_stalls = 6\n").presentStalls == 6.0);
    const rvk::Config c = rvk::parseConfig("[debug]\nenvironment = [\"A=1\", \"no equals\", \"B=\"]\n");
    CHECK(c.environment.size() == 2);
    CHECK(c.environment.size() == 2 && c.environment[0] == "A=1" && c.environment[1] == "B=");
}

TEST(config_errors) {
    std::string err;
    rvk::Config c = rvk::parseConfig("[renderer]\nmode = \"dx12\"\n", &err);
    CHECK(!err.empty());
    CHECK(c.mode == rvk::Mode::Passthrough);  // unknown: the safe mode, locked
    CHECK(rvk::configModeLock(c) == rvk::RenderMode::Original);
    err.clear();
    c = rvk::parseConfig("this is not toml", &err);
    CHECK(!err.empty());
    CHECK(c.mode == rvk::Mode::Passthrough);
    CHECK(rvk::configModeLock(c) == rvk::RenderMode::Original);
}

TEST(config_render_modes) {
    using rvk::Mode;
    using rvk::RenderMode;
    struct Case {
        const char* text;
        RenderMode mode;
        Mode backend;
        bool locks;  // a non-default value locks renderer.mode for the session
    };
    const Case cases[] = {
        {"original", RenderMode::Original, Mode::Passthrough, true}, {"passthrough", RenderMode::Original, Mode::Passthrough, true},
        {"d3d9", RenderMode::Original, Mode::Passthrough, true},     {"wrapper", RenderMode::Wrapper, Mode::Vulkan, false},
        {"vulkan", RenderMode::Wrapper, Mode::Vulkan, false},        {"full", RenderMode::Full, Mode::Vulkan, true},
    };
    for (const Case& k : cases) {
        std::string err;
        const rvk::Config c = rvk::parseConfig(std::string("[renderer]\nmode = \"") + k.text + "\"\n", &err);
        CHECK_MSG(err.empty() && c.renderMode == k.mode && c.mode == k.backend, k.text);
        CHECK_MSG(rvk::configModeLock(c).has_value() == k.locks, k.text);
        if (k.locks) CHECK_MSG(*rvk::configModeLock(c) == k.mode, k.text);
        CHECK(rvk::parseRenderMode(k.text) == k.mode);
    }
    CHECK(!rvk::parseRenderMode("Full").has_value());
    CHECK(!rvk::parseRenderMode("trace").has_value());
    CHECK(rvk::toString(RenderMode::Original) == "original" && rvk::toString(RenderMode::Wrapper) == "wrapper" &&
          rvk::toString(RenderMode::Full) == "full");
    // the backend of a session: the started render mode, trace stays trace
    const rvk::Config plain = rvk::parseConfig("");
    CHECK(rvk::sessionBackend(plain, RenderMode::Original) == Mode::Passthrough);
    CHECK(rvk::sessionBackend(plain, RenderMode::Wrapper) == Mode::Vulkan);
    CHECK(rvk::sessionBackend(plain, RenderMode::Full) == Mode::Vulkan);
    const rvk::Config trace = rvk::parseConfig("[renderer]\nmode = \"trace\"\n");
    CHECK(rvk::configModeLock(trace) == RenderMode::Original);
    CHECK(rvk::sessionBackend(trace, RenderMode::Original) == Mode::Trace);
    // restarts: only between backends
    CHECK(!rvk::needsRestart(RenderMode::Wrapper, RenderMode::Full) && !rvk::needsRestart(RenderMode::Full, RenderMode::Wrapper));
    CHECK(rvk::needsRestart(RenderMode::Wrapper, RenderMode::Original) && rvk::needsRestart(RenderMode::Original, RenderMode::Full));
    CHECK(rvk::vulkanSessionMode(RenderMode::Original) == RenderMode::Wrapper);
    CHECK(rvk::vulkanSessionMode(RenderMode::Full) == RenderMode::Full);
}

TEST(render_mode_native_plan) {
    using rvk::RenderMode;
    const int all = static_cast<int>(rvk::kAllSceneOpGroups);
    // Wrapper: no take-over
    auto p = rvk::nativePlan(RenderMode::Wrapper, false, false, false, false, all);
    CHECK(!p.staticTake && !p.sceneTake && !p.any());
    // Full: static + scene ops, every group, no compare
    p = rvk::nativePlan(RenderMode::Full, false, false, false, false, all);
    CHECK(p.staticTake && p.sceneTake && !p.staticCompare && !p.sceneCompare && p.groups == rvk::kAllSceneOpGroups);
    // Full with the groups mask (bisecting) and compare on top
    p = rvk::nativePlan(RenderMode::Full, false, true, false, true, 64 | 128);
    CHECK(p.staticTake && p.sceneTake && p.staticCompare && p.sceneCompare && p.groups == (64u | 128u));
    // Wrapper with the Debug overrides: one stage at a time
    p = rvk::nativePlan(RenderMode::Wrapper, true, false, false, false, all);
    CHECK(p.staticTake && !p.sceneTake);
    p = rvk::nativePlan(RenderMode::Wrapper, false, false, true, false, 1);
    CHECK(!p.staticTake && p.sceneTake && p.groups == 1);
    // compare alone in Wrapper (observe, nothing dropped)
    p = rvk::nativePlan(RenderMode::Wrapper, false, true, false, false, 0);
    CHECK(!p.staticTake && p.staticCompare && p.any());
    // Original never reaches a Vulkan session: it renders as Wrapper there
    p = rvk::nativePlan(rvk::vulkanSessionMode(RenderMode::Original), false, false, false, false, all);
    CHECK(!p.any());
}

TEST(config_trace) {
    const rvk::Config c = rvk::parseConfig(R"(
[renderer]
mode = "trace"
[trace]
log_first = 10
capture = false
)");
    CHECK(c.mode == rvk::Mode::Trace);
    CHECK(c.traceLogFirst == 10);
    CHECK(!c.traceCapture);
}

TEST(sm_version) {
    auto v = rvk::shader::parseVersion(0xFFFE0300u);
    CHECK(v && v->stage == rvk::shader::Stage::Vertex && v->major == 3 && v->minor == 0);
    v = rvk::shader::parseVersion(0xFFFF0101u);
    CHECK(v && v->stage == rvk::shader::Stage::Pixel && v->major == 1 && v->minor == 1);
    CHECK(!rvk::shader::parseVersion(0x00000300u));
    CHECK(!rvk::shader::parseVersion(0xFFFF0400u));
}

TEST(sm_length) {
    using rvk::shader::byteLength;
    // ps_3_0, comment of 2 dwords that contains the END token, def c0 with an immediate equal to END (length field 5),
    // mov oC0, c0 (length 2), END
    const std::uint32_t ps3[] = {0xFFFF0300u, 0x0002FFFEu, 0x0000FFFFu, 0x12345678u, 0x05000051u, 0xA00F0000u,
                                 0x0000FFFFu, 0x3F800000u, 0x0u,        0x3F800000u, 0x02000001u, 0x800F0800u,
                                 0xA0E40000u, 0x0000FFFFu};
    CHECK(byteLength(ps3, std::size(ps3)) == sizeof(ps3));
    // the same without the END token: not found
    CHECK(byteLength(ps3, std::size(ps3) - 1) == 0);
    // ps_1_1: tex t0 (1 param), mov r0, t0 (2 params), END
    const std::uint32_t ps1[] = {0xFFFF0101u, 0x00000042u, 0x800F0000u, 0x00000001u,
                                 0x800F0000u, 0xB0E40000u, 0x0000FFFFu};
    CHECK(byteLength(ps1, std::size(ps1)) == sizeof(ps1));
    CHECK(byteLength(ps1, 0) == 0);
    const std::uint32_t garbage[] = {0x12345678u, 0x0000FFFFu};
    CHECK(byteLength(garbage, std::size(garbage)) == 0);
}

int main() {
    for (const auto& t : rvk::test::registry()) {
        const int before = rvk::test::failures();
        t.fn();
        std::printf("%s %s\n", rvk::test::failures() == before ? "ok  " : "FAIL", t.name);
    }
    std::printf("%zu tests, %d failure(s)\n", rvk::test::registry().size(), rvk::test::failures());
    return rvk::test::failures() == 0 ? 0 : 1;
}
