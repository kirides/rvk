#include "core/exports.h"

#include <windows.h>

namespace rvk::exports {

void* resolve(const wchar_t* module, const char* symbol) {
    const HMODULE mod = GetModuleHandleW(module);
    if (mod == nullptr) {
        return nullptr;
    }
    return reinterpret_cast<void*>(GetProcAddress(mod, symbol));
}

std::uintptr_t moduleBase(const wchar_t* module) {
    return reinterpret_cast<std::uintptr_t>(GetModuleHandleW(module));
}

}  // namespace rvk::exports
