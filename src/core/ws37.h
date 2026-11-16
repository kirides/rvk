#pragma once

// WS37 (frame-ownership.md 16.28): switches + a prefetch helper of the engine-thread CPU trims.
// RVK_WS37_OFF=<bit mask>: 1 = the BLAS eviction walk runs every frame again, 2 = no prefetch in the RT scene's per-instance loop,
// 4 = no prefetch in the owned pass's op loop. Read once.

#include <intrin.h>

#include <cstdint>
#include <cstdlib>

namespace rvk {

inline std::uint32_t ws37Off() {
    static const std::uint32_t mask = [] {
        const char* v = std::getenv("RVK_WS37_OFF");
        return v == nullptr ? 0u : static_cast<std::uint32_t>(std::strtoul(v, nullptr, 10));
    }();
    return mask;
}

// a hint only: any address (a null / dangling one is harmless, nothing is dereferenced or faulted)
inline void prefetchLine(const void* p) { _mm_prefetch(static_cast<const char*>(p), _MM_HINT_T0); }

}  // namespace rvk
