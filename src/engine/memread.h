#pragma once

// Reads of engine memory that cannot crash the game (objects we do not own: SEH-guarded copies), and the engine's bCString.

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <type_traits>

namespace rvk::engine {

// Header definitions (x64 SEH costs nothing until a fault). NOTE (cpuclean, measured): MSVC does not inline a function that holds a __try, so these
// stay calls and the profile's safeLoad8 self time (cache misses on the engine memory it reads) did not move; the gain has to come from fewer
// reads (one block per object: safeReadN, lights.cpp), not from inlining
// copies `size` bytes; false when the source is not readable
inline bool safeRead(const void* src, void* dst, std::size_t size) {
    __try {
        std::memcpy(dst, src, size);
        return true;
    } __except (1 /* EXCEPTION_EXECUTE_HANDLER */) {
        return false;
    }
}
// copies `size` bytes into engine memory; false when the destination is not writable (O2b: the lean path's pool / field stores)
inline bool safeWrite(void* dst, const void* src, std::size_t size) {
    __try {
        std::memcpy(dst, src, size);
        return true;
    } __except (1) {
        return false;
    }
}
// one guarded load of 1 / 2 / 4 / 8 bytes (no copy routine: the hot field reads of native/*; O1)
#define RVK_SAFE_LOAD(NAME, T)                                        inline bool NAME(const void* src, T& out) {                           __try {                                                               out = *static_cast<const volatile T*>(src);                       return true;                                                  } __except (1) {                                                      return false;                                                 }                                                             }
RVK_SAFE_LOAD(safeLoad1, std::uint8_t)
RVK_SAFE_LOAD(safeLoad2, std::uint16_t)
RVK_SAFE_LOAD(safeLoad4, std::uint32_t)
RVK_SAFE_LOAD(safeLoad8, std::uint64_t)
#undef RVK_SAFE_LOAD

// a guarded copy of a fixed size (one SEH frame, the copy expanded inline; O1: an engine object's fields read at once)
template <std::size_t N>
bool safeReadN(const void* src, void* dst) {
    __try {
        std::memcpy(dst, src, N);
        return true;
    } __except (1 /* EXCEPTION_EXECUTE_HANDLER */) {
        return false;
    }
}

#pragma warning(push)
#pragma warning(disable : 4723)  // (inlined into callers that divide by a field: their fallback-0 path, IEEE as before)
template <class T>
inline bool readAt(const void* base, std::size_t offset, T& out) {
    if (base == nullptr) return false;
    const void* p = static_cast<const std::uint8_t*>(base) + offset;
    if constexpr (std::is_trivially_copyable_v<T> && (sizeof(T) == 1 || sizeof(T) == 2 || sizeof(T) == 4 || sizeof(T) == 8)) {
        using W = std::conditional_t<sizeof(T) == 1, std::uint8_t,
                                     std::conditional_t<sizeof(T) == 2, std::uint16_t, std::conditional_t<sizeof(T) == 4, std::uint32_t, std::uint64_t>>>;
        W v{};
        bool ok = false;
        if constexpr (sizeof(T) == 1) ok = safeLoad1(p, v);
        else if constexpr (sizeof(T) == 2) ok = safeLoad2(p, v);
        else if constexpr (sizeof(T) == 4) ok = safeLoad4(p, v);
        else ok = safeLoad8(p, v);
        if (ok) std::memcpy(&out, &v, sizeof(T));
        return ok;
    } else {
        return safeRead(p, &out, sizeof(T));
    }
}
#pragma warning(pop)

// A bCString field (SharedBase: a char* whose int length sits 8 bytes before the text) -> its text ("" when unreadable)
std::string readCString(const void* field);

}  // namespace rvk::engine
