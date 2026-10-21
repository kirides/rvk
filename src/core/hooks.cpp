#include "core/hooks.h"

#include <MinHook.h>
#include <spdlog/spdlog.h>

namespace rvk::hooks {
namespace {
bool g_initialized = false;
}

bool initialize() {
    if (g_initialized) {
        return true;
    }
    const MH_STATUS st = MH_Initialize();
    if (st != MH_OK) {
        spdlog::error("MinHook: MH_Initialize failed: {}", MH_StatusToString(st));
        return false;
    }
    g_initialized = true;
    return true;
}

void shutdown() {
    if (!g_initialized) {
        return;
    }
    MH_DisableHook(MH_ALL_HOOKS);
    MH_Uninitialize();
    g_initialized = false;
}

bool create(std::string_view name, void* target, void* detour, void** original) {
    const MH_STATUS st = MH_CreateHook(target, detour, original);
    if (st != MH_OK) {
        spdlog::error("hook '{}' @ {} failed: {}", name, target, MH_StatusToString(st));
        return false;
    }
    spdlog::debug("hook '{}' created @ {}", name, target);
    return true;
}

bool enableAll() {
    const MH_STATUS st = MH_EnableHook(MH_ALL_HOOKS);
    if (st != MH_OK) {
        spdlog::error("MinHook: enable failed: {}", MH_StatusToString(st));
        return false;
    }
    return true;
}

}  // namespace rvk::hooks
