#pragma once

#include <cstdint>

// Risen's engine DLLs export their C++ classes with MSVC-decorated names: bind with
// GetProcAddress("?InitializeAdmin@eCGfxAdmin@@IEAAXW4eEAPIType@eCGfxShared@@@Z") instead of hard-coded addresses.
namespace rvk::exports {

// Address of `symbol` in an already loaded (or at least mapped) module, or nullptr. Never loads libraries,
// so it is safe under the loader lock.
void* resolve(const wchar_t* module, const char* symbol);

template <class T>
T resolveAs(const wchar_t* module, const char* symbol) {
    return reinterpret_cast<T>(resolve(module, symbol));
}

std::uintptr_t moduleBase(const wchar_t* module);

}  // namespace rvk::exports
