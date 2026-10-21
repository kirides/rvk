#pragma once

// Minimal test registry: TEST(name) { CHECK(cond); }  (run all with rvk_tests.exe)

#include <cstdio>
#include <functional>
#include <string>
#include <vector>

namespace rvk::test {

struct TestCase {
    const char* name;
    std::function<void()> fn;
};
std::vector<TestCase>& registry();
int& failures();

struct Register {
    Register(const char* name, std::function<void()> fn) { registry().push_back({name, std::move(fn)}); }
};

}  // namespace rvk::test

#define TEST(name)                                                  \
    static void test_##name();                                      \
    static ::rvk::test::Register reg_##name(#name, test_##name);    \
    static void test_##name()

#define CHECK(cond)                                                                  \
    do {                                                                             \
        if (!(cond)) {                                                               \
            std::printf("  FAILED %s:%d: %s\n", __FILE__, __LINE__, #cond);          \
            ++::rvk::test::failures();                                               \
        }                                                                            \
    } while (0)

#define CHECK_MSG(cond, msg)                                                                         \
    do {                                                                                             \
        if (!(cond)) {                                                                               \
            std::printf("  FAILED %s:%d: %s\n%s\n", __FILE__, __LINE__, #cond, std::string(msg).c_str()); \
            ++::rvk::test::failures();                                                               \
        }                                                                                            \
    } while (0)
