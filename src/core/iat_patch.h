#pragma once

// Import-table patching of ONE module (not a global detour): the entry `function` that `module` imports from `importDll` (by name,
// case-insensitive DLL name) is pointed at `hook`; `*original` receives the previous entry. Used for AMD's frame generation DLL
// (vk/fg_swapchain.cpp: its named events; vk/fg_trace.cpp: its Vulkan calls). Returns false when the module does not import it.
// Idempotent: an entry that already points at `hook` is left alone (and `*original` is not touched).

namespace rvk::iat {

bool patchImport(void* module, const char* importDll, const char* function, void* hook, void** original);
// the current entry of that import (null when the module does not import it)
void* importTarget(void* module, const char* importDll, const char* function);

}  // namespace rvk::iat
