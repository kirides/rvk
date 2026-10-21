#include "engine/vfs.h"

#include <cstring>

#include <windows.h>

#include "engine/symbols.h"

namespace rvk::engine {
namespace {

// bCString (SharedBase.dll) is one pointer to a shared text block [C: eCDX9Device::SaveBackbufferToFile keeps it in 8 bytes];
// the buffer is larger to be safe.
struct alignas(16) EngineString {
    unsigned char storage[32]{};
};

using StringCtorFn = void* (*)(EngineString*, const char*);
using StringDtorFn = void (*)(EngineString*);
using StringGetTextFn = const char* (*)(const void*);
using VfsGetInstanceFn = void* (*)();
using VfsMakeAbsPhysicalPathFn = bool (*)(void* vfs, EngineString*);
using AppGetInstanceFn = void* (*)();
using AppSnapshotFn = void (*)(void* app);
using AppGetLastSnapshotNameFn = const void* (*)(const void* app);

std::filesystem::path fromAnsi(const char* s) {
    const int n = MultiByteToWideChar(CP_ACP, 0, s, -1, nullptr, 0);
    if (n <= 1) return {};
    std::wstring w(static_cast<std::size_t>(n), L' ');
    MultiByteToWideChar(CP_ACP, 0, s, -1, w.data(), n);
    w.resize(static_cast<std::size_t>(n - 1));
    return std::filesystem::path(w);
}

}  // namespace

std::optional<std::filesystem::path> physicalPath(const char* enginePath, std::string* error) {
    const auto fail = [&](const char* why) -> std::optional<std::filesystem::path> {
        if (error != nullptr) *error = why;
        return std::nullopt;
    };
    if (enginePath == nullptr || *enginePath == '\0') {
        return fail("empty path");
    }
    const auto& s = sym();
    if (s.stringCtor == nullptr || s.stringDtor == nullptr || s.stringGetText == nullptr || s.vfsGetInstance == nullptr ||
        s.vfsMakeAbsPhysicalPath == nullptr) {
        return fail("engine VFS symbols missing");
    }
    EngineString str;
    reinterpret_cast<StringCtorFn>(s.stringCtor)(&str, enginePath);
    void* vfs = reinterpret_cast<VfsGetInstanceFn>(s.vfsGetInstance)();
    const bool ok = vfs != nullptr && reinterpret_cast<VfsMakeAbsPhysicalPathFn>(s.vfsMakeAbsPhysicalPath)(vfs, &str);
    std::filesystem::path result;
    if (ok) {
        if (const char* text = reinterpret_cast<StringGetTextFn>(s.stringGetText)(&str); text != nullptr) {
            result = fromAnsi(text);
        }
    }
    reinterpret_cast<StringDtorFn>(s.stringDtor)(&str);
    if (!ok) {
        return fail("eCVirtualFileSystem::MakeAbsPhysicalPath failed");
    }
    if (result.empty() || !result.is_absolute()) {
        return fail("the engine returned no absolute physical path");
    }
    return result.make_preferred();
}

bool requestEngineSnapshot() {
    const auto& s = sym();
    if (s.appGetInstance == nullptr || s.appSnapshot == nullptr) {
        return false;
    }
    void* app = reinterpret_cast<AppGetInstanceFn>(s.appGetInstance)();
    if (app == nullptr) {
        return false;
    }
    reinterpret_cast<AppSnapshotFn>(s.appSnapshot)(app);
    return true;
}

std::string lastEngineSnapshotName() {
    const auto& s = sym();
    if (s.appGetInstance == nullptr || s.appGetLastSnapshotName == nullptr || s.stringGetText == nullptr) {
        return {};
    }
    const void* app = reinterpret_cast<AppGetInstanceFn>(s.appGetInstance)();
    if (app == nullptr) {
        return {};
    }
    const void* name = reinterpret_cast<AppGetLastSnapshotNameFn>(s.appGetLastSnapshotName)(app);
    const char* text = name != nullptr ? reinterpret_cast<StringGetTextFn>(s.stringGetText)(name) : nullptr;
    return text != nullptr ? std::string(text) : std::string();
}

}  // namespace rvk::engine
