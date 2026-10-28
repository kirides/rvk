#pragma once

// Reads of engine memory that cannot crash the game (objects we do not own: SEH-guarded copies), and the engine's bCString.

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <type_traits>

namespace rvk::engine {

// copies `size` bytes; false when the source is not readable
bool safeRead(const void* src, void* dst, std::size_t size);
// one guarded load of 1 / 2 / 4 / 8 bytes (no copy routine: the hot field reads of native/*; O1)
bool safeLoad1(const void* src, std::uint8_t& out);
bool safeLoad2(const void* src, std::uint16_t& out);
bool safeLoad4(const void* src, std::uint32_t& out);
bool safeLoad8(const void* src, std::uint64_t& out);

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
