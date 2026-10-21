#pragma once

namespace rvk::bootstrap {

// DllMain (loader lock held, Engine.dll mapped but not initialised): config, log, symbols, hooks. Never loads libraries.
void onProcessAttach(void* module);
// processTerminating: the process is exiting (do not touch other DLLs or threads).
void onProcessDetach(bool processTerminating);

}  // namespace rvk::bootstrap
