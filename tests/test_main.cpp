// Unit tests for pure logic (no game, no GPU). Run: build\RelWithDebInfo\rvk_tests.exe (or ctest -C RelWithDebInfo --test-dir build).

#include <cstdio>
#include <functional>
#include <string>
#include <vector>

#include "core/config.h"

namespace {

struct TestCase {
    const char* name;
    std::function<void()> fn;
};
std::vector<TestCase>& registry() {
    static std::vector<TestCase> r;
    return r;
}
struct Register {
    Register(const char* name, std::function<void()> fn) { registry().push_back({name, std::move(fn)}); }
};
int g_failures = 0;

#define TEST(name)                                  \
    static void test_##name();                      \
    static Register reg_##name(#name, test_##name); \
    static void test_##name()

#define CHECK(cond)                                                              \
    do {                                                                         \
        if (!(cond)) {                                                           \
            std::printf("  FAILED %s:%d: %s\n", __FILE__, __LINE__, #cond);      \
            ++g_failures;                                                        \
        }                                                                        \
    } while (0)

}  // namespace

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

int main() {
    for (const auto& t : registry()) {
        const int before = g_failures;
        t.fn();
        std::printf("%s %s\n", g_failures == before ? "ok  " : "FAIL", t.name);
    }
    std::printf("%zu tests, %d failure(s)\n", registry().size(), g_failures);
    return g_failures == 0 ? 0 : 1;
}
