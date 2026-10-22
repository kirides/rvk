#pragma once

// Reads of engine memory that cannot crash the game (objects we do not own: SEH-guarded copies), and the engine's bCString.

#include <cstddef>
#include <cstdint>
#include <string>

namespace rvk::engine {

// copies `size` bytes; false when the source is not readable
bool safeRead(const void* src, void* dst, std::size_t size);

template <class T>
bool readAt(const void* base, std::size_t offset, T& out) {
    return base != nullptr && safeRead(static_cast<const std::uint8_t*>(base) + offset, &out, sizeof(T));
}

// A bCString field (SharedBase: a char* whose int length sits 8 bytes before the text) -> its text ("" when unreadable)
std::string readCString(const void* field);

}  // namespace rvk::engine
