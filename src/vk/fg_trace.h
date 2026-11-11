#pragma once

// Diagnostics of AMD's frame generation swapchain (docs/research/frame-generation.md 13, FG-1): `RVK_FG_TRACE=1` (Win32 environment,
// run.ps1 -Environment) patches the import table of amd_fidelityfx_framegeneration_vk.dll (it calls Vulkan through vulkan-1.dll's
// exports; we use volk's device-level pointers, so only the DLL's own calls go through the patched entries) and records every queue
// submit / present / acquire / semaphore wait it makes, plus ours on the game queue, into a ring. The hang watchdog
// (diag::startHangWatchdog, -Perf) dumps the ring, every timeline semaphore's current value and the waits in progress
// ("fgtrace:" lines): which wait is not satisfied and which submit should have signalled it.
//
// Off (the default): nothing is patched, the record calls return at once.

#include <cstdint>

#include "vk/vk.h"

namespace rvk::vk::fgtrace {

bool enabled();

// patches the module's vulkan-1.dll imports (once per module); `device` for the semaphore reads of the dump
void install(void* module, VkDevice device);
void setDevice(VkDevice device);
// names for the queues of the dump
void nameQueue(VkQueue queue, const char* name);

// our own game queue traffic (Context::queueSubmit, the AMD callback, Swapchain::present)
void recordSubmit2(const char* who, VkQueue queue, const VkSubmitInfo2& submit, VkFence fence, VkResult result);
void recordSubmit(const char* who, VkQueue queue, std::uint32_t count, const VkSubmitInfo* submits, VkFence fence, VkResult result);
void note(const char* what, std::uint64_t a = 0, std::uint64_t b = 0);

// the dump (hang watchdog thread)
void dump(const char* why);

}  // namespace rvk::vk::fgtrace
