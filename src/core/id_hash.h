#pragma once

// A cheap hash for integer / pointer keys (std::hash is FNV-1a over the bytes on MSVC: ~8 dependent multiplies per lookup).

#include <cstddef>
#include <cstdint>

namespace rvk {

struct IdHash {
    std::size_t operator()(std::uint64_t x) const noexcept {
        x *= 0x9E3779B97F4A7C15ull;
        return static_cast<std::size_t>(x ^ (x >> 32));
    }
};

}  // namespace rvk
