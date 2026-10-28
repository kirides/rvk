#pragma once

// Inline byte-wise comparisons for the per-draw hot paths (V4): the CRT's memcmp is a call into vcruntime140 for every
// small block (interned vertex input / dynamic state, pipeline descriptions, constant registers), and the profile put
// those calls at ~1.5 - 2 % of the engine thread. SSE2 is part of x64.

#include <emmintrin.h>

#include <cstddef>
#include <cstdint>
#include <cstring>

namespace rvk::d3d9 {

// a == b over n bytes
inline bool memEqual(const void* a, const void* b, std::size_t n) {
    const auto* p = static_cast<const std::uint8_t*>(a);
    const auto* q = static_cast<const std::uint8_t*>(b);
    while (n >= 16) {
        const __m128i x = _mm_loadu_si128(reinterpret_cast<const __m128i*>(p));
        const __m128i y = _mm_loadu_si128(reinterpret_cast<const __m128i*>(q));
        if (_mm_movemask_epi8(_mm_cmpeq_epi8(x, y)) != 0xFFFF) return false;
        p += 16;
        q += 16;
        n -= 16;
    }
    if (n >= 8) {
        std::uint64_t x, y;
        std::memcpy(&x, p, 8);
        std::memcpy(&y, q, 8);
        if (x != y) return false;
        p += 8;
        q += 8;
        n -= 8;
    }
    if (n >= 4) {
        std::uint32_t x, y;
        std::memcpy(&x, p, 4);
        std::memcpy(&y, q, 4);
        if (x != y) return false;
        p += 4;
        q += 4;
        n -= 4;
    }
    for (; n != 0; --n) {
        if (*p++ != *q++) return false;
    }
    return true;
}

// the registers (16 bytes each) of two runs: the first and the last register that differ, false when all are equal
inline bool diffRegisters(const float (*a)[4], const float (*b)[4], std::uint32_t count, std::uint32_t& first, std::uint32_t& last) {
    const auto differs = [&](std::uint32_t i) {
        const __m128i x = _mm_loadu_si128(reinterpret_cast<const __m128i*>(a[i]));
        const __m128i y = _mm_loadu_si128(reinterpret_cast<const __m128i*>(b[i]));
        return _mm_movemask_epi8(_mm_cmpeq_epi8(x, y)) != 0xFFFF;
    };
    std::uint32_t i = 0;
    while (i < count && !differs(i)) ++i;
    if (i == count) return false;
    std::uint32_t j = count - 1;
    while (j > i && !differs(j)) --j;
    first = i;
    last = j;
    return true;
}

}  // namespace rvk::d3d9
