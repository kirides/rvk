#pragma once

#include <string_view>

namespace rvk::hooks {

// Thin MinHook wrapper. Hooks are created disabled and enabled together (enableAll), so the game never sees a
// partially installed set.
bool initialize();
void shutdown();
bool create(std::string_view name, void* target, void* detour, void** original);
bool enableAll();

}  // namespace rvk::hooks

namespace rvk {

// Typed hook keeping the trampoline:
//   static Hook<void(*)(void*, int)> g_hook;
//   g_hook.install("name", target, &detour);   // the detour calls g_hook.original(...)
template <class Fn>
class Hook {
public:
    Fn original = nullptr;

    bool install(std::string_view name, Fn target, Fn detour) {
        if (target == nullptr) {
            return false;
        }
        return hooks::create(name, reinterpret_cast<void*>(target), reinterpret_cast<void*>(detour),
                             reinterpret_cast<void**>(&original));
    }
    bool installed() const { return original != nullptr; }
};

}  // namespace rvk
