#include "core/iat_patch.h"

#include <cstdint>
#include <cstring>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

namespace rvk::iat {

namespace {

// the import address table entry of `function` from `importDll`, or null
ULONG_PTR* findSlot(void* module, const char* importDll, const char* function) {
    if (module == nullptr || importDll == nullptr || function == nullptr) return nullptr;
    auto* base = static_cast<std::uint8_t*>(module);
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return nullptr;
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) return nullptr;
    const IMAGE_DATA_DIRECTORY& dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (dir.VirtualAddress == 0 || dir.Size == 0) return nullptr;
    for (auto* imp = reinterpret_cast<const IMAGE_IMPORT_DESCRIPTOR*>(base + dir.VirtualAddress); imp->Name != 0; ++imp) {
        if (_stricmp(reinterpret_cast<const char*>(base + imp->Name), importDll) != 0) continue;
        const auto* names = reinterpret_cast<const IMAGE_THUNK_DATA*>(base + (imp->OriginalFirstThunk != 0 ? imp->OriginalFirstThunk : imp->FirstThunk));
        auto* slots = reinterpret_cast<IMAGE_THUNK_DATA*>(base + imp->FirstThunk);
        for (; names->u1.AddressOfData != 0; ++names, ++slots) {
            if (IMAGE_SNAP_BY_ORDINAL(names->u1.Ordinal)) continue;
            const auto* ibn = reinterpret_cast<const IMAGE_IMPORT_BY_NAME*>(base + names->u1.AddressOfData);
            if (std::strcmp(reinterpret_cast<const char*>(ibn->Name), function) != 0) continue;
            return &slots->u1.Function;
        }
    }
    return nullptr;
}

}  // namespace

bool patchImport(void* module, const char* importDll, const char* function, void* hook, void** original) {
    if (hook == nullptr || original == nullptr) return false;
    ULONG_PTR* slot = findSlot(module, importDll, function);
    if (slot == nullptr) return false;
    if (reinterpret_cast<void*>(*slot) == hook) return true;  // patched already
    DWORD old = 0;
    if (!VirtualProtect(slot, sizeof(void*), PAGE_READWRITE, &old)) return false;
    *original = reinterpret_cast<void*>(*slot);
    *slot = reinterpret_cast<ULONG_PTR>(hook);
    VirtualProtect(slot, sizeof(void*), old, &old);
    FlushInstructionCache(GetCurrentProcess(), slot, sizeof(void*));
    return true;
}

void* importTarget(void* module, const char* importDll, const char* function) {
    ULONG_PTR* slot = findSlot(module, importDll, function);
    return slot != nullptr ? reinterpret_cast<void*>(*slot) : nullptr;
}

}  // namespace rvk::iat
