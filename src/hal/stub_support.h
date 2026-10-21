#pragma once

// Runtime of the generated stubs (hal/stub_generated.h).

namespace rvk::hal {

// logs "<interface>::<slot> is not implemented" the first time a slot is called (thread-safe)
void noteUnimplemented(int iface, int slot);
// zeroed storage for stubs that must return a reference
void* stubStorage();
// how many distinct slots were hit unimplemented so far
int unimplementedCount();

}  // namespace rvk::hal
