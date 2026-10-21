#include "core/paths.h"

#include <string>

#include <windows.h>

namespace rvk::paths {
namespace {
HMODULE g_self = nullptr;
std::filesystem::path g_bin;
std::filesystem::path g_data;
}  // namespace

void init(void* self) {
    g_self = static_cast<HMODULE>(self);
    std::wstring buf(MAX_PATH, L'\0');
    for (;;) {
        const DWORD n = GetModuleFileNameW(g_self, buf.data(), static_cast<DWORD>(buf.size()));
        if (n < buf.size()) {
            buf.resize(n);
            break;
        }
        buf.resize(buf.size() * 2);
    }
    g_bin = std::filesystem::path(buf).parent_path();
    g_data = g_bin / L"rvk";
}

void* selfModule() { return g_self; }
const std::filesystem::path& binDir() { return g_bin; }
const std::filesystem::path& dataDir() { return g_data; }

}  // namespace rvk::paths
