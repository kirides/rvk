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
    CHECK(c.mode == rvk::Mode::Passthrough);
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
    CHECK(c.logLevel == "debug");
    CHECK(c.validation);
    CHECK(!c.probe);
    CHECK(c.adapter == 1);
}

TEST(config_errors) {
    std::string err;
    rvk::Config c = rvk::parseConfig("[renderer]\nmode = \"dx12\"\n", &err);
    CHECK(!err.empty());
    CHECK(c.mode == rvk::Mode::Passthrough);
    err.clear();
    c = rvk::parseConfig("this is not toml", &err);
    CHECK(!err.empty());
    CHECK(c.mode == rvk::Mode::Passthrough);
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
