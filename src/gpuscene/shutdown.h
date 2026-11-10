#pragma once

// The GPU scene's process-wide tables (gpuscene::*() singletons: static or leaked on purpose, so they outlive any device) hold backend buffers and pipelines.
// Their owner must release those before the device goes: a buffer still alive at vkDestroyDevice is a leak (VUID-vkDestroyDevice-device-05137), and a static
// table's destructor at process exit would retire it through the freed backend (an access violation). Call once, right before d3d9::Device::destroy
// (HalDevice::~HalDevice, the tools' teardown). The tables' CPU state stays; nothing may flush them until a new device exists.

namespace rvk::d3d9 {
class Device;
}

namespace rvk::gpuscene {

void releaseGpuObjects(d3d9::Device& dev);

}  // namespace rvk::gpuscene
