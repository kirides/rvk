#pragma once

#include <cstddef>
#include <cstdint>

namespace rvk::pheap {

// WS35 (frame-ownership.md 16.26): the renderer's own allocations (every global operator new of the d3d9.dll module, see proxy/new_override.cpp) live in private
// Win32 heaps instead of the process heap the engine and its ~40 threads share: our churn neither waits on nor blocks the engine's heap lock.
//
// kShards heaps; a thread allocates from its own shard (the first thread to allocate, the loader thread = the engine's main thread, gets shard 0 alone; later threads
// share shards 1..kShards-1 round-robin). Every block carries a 16-byte self-checking header (tag = magic ^ block address, shard, offset to the heap block), so
// `release` recognises a block of ours and sends anything else (memory the CRT's own operator new in msvcp140 / vcruntime140 allocated, e.g. a std::string a CRT function returned) to
// the CRT's free. `RVK_PRIVATE_HEAP=0` = every shard is the process heap (the headers stay): the A/B switch.
inline constexpr std::uint32_t kShards = 8;

// align must be a power of two (<= 4096); size 0 allocates a minimal block. nullptr when the heap is exhausted.
void* alloc(std::size_t size, std::size_t align = 16) noexcept;
// Frees a block from alloc(); a pointer that did not come from alloc() goes to the CRT (free, or _aligned_free when alignedForm). nullptr is a no-op.
void release(void* p, bool alignedForm = false) noexcept;
// True when p carries our header (tests).
bool owns(const void* p) noexcept;
// true = private heaps, false = process heap (RVK_PRIVATE_HEAP=0).
bool privateActive() noexcept;
// The usable size of one of our blocks (0 for a foreign pointer).
std::size_t usableSize(const void* p) noexcept;

}  // namespace rvk::pheap
