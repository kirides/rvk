#include <windows.h>

#include "bootstrap.h"

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID reserved) {
    switch (reason) {
    case DLL_PROCESS_ATTACH:
        DisableThreadLibraryCalls(module);
        rvk::bootstrap::onProcessAttach(module);
        break;
    case DLL_PROCESS_DETACH:
        rvk::bootstrap::onProcessDetach(reserved != nullptr);
        break;
    default:
        break;
    }
    return TRUE;
}
