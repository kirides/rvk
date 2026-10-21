#pragma once

namespace rvk::proxy {

// The system d3d9.dll (System32), loaded on first use; nullptr if it cannot be loaded. Never call under the loader lock.
void* systemD3D9();

}  // namespace rvk::proxy
