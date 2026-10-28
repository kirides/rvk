#include "engine/memread.h"

#include <windows.h>

#include <algorithm>
#include <cstring>

namespace rvk::engine {

bool safeLoad1(const void* src, std::uint8_t& out) {
    __try {
        out = *static_cast<const volatile std::uint8_t*>(src);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool safeLoad2(const void* src, std::uint16_t& out) {
    __try {
        out = *static_cast<const volatile std::uint16_t*>(src);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool safeLoad4(const void* src, std::uint32_t& out) {
    __try {
        out = *static_cast<const volatile std::uint32_t*>(src);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool safeLoad8(const void* src, std::uint64_t& out) {
    __try {
        out = *static_cast<const volatile std::uint64_t*>(src);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool safeRead(const void* src, void* dst, std::size_t size) {
    __try {
        std::memcpy(dst, src, size);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool safeWrite(void* dst, const void* src, std::size_t size) {
    __try {
        std::memcpy(dst, src, size);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

std::string readCString(const void* field) {
    const char* text = nullptr;
    if (!readAt(field, 0, text) || text == nullptr) return {};
    std::int32_t length = 0;
    if (!safeRead(text - 8, &length, sizeof(length))) length = 0;  // unknown: up to the terminator
    char buf[256]{};
    const std::size_t n = length > 0 ? std::min<std::size_t>(static_cast<std::size_t>(length), sizeof(buf) - 1) : sizeof(buf) - 1;
    if (!safeRead(text, buf, n)) {
        // the buffer may end before 255 bytes: read byte by byte up to the terminator
        std::size_t i = 0;
        for (; i < sizeof(buf) - 1; ++i) {
            if (!safeRead(text + i, &buf[i], 1) || buf[i] == 0) break;
        }
        buf[i] = 0;
    }
    buf[n] = 0;
    return std::string(buf, strnlen(buf, n));
}

}  // namespace rvk::engine
