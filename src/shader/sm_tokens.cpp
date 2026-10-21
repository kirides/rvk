#include "shader/sm_tokens.h"

namespace rvk::shader {

std::optional<Version> parseVersion(std::uint32_t token) {
    const std::uint32_t kind = token & 0xFFFF0000u;
    Version v;
    if (kind == 0xFFFE0000u) {
        v.stage = Stage::Vertex;
    } else if (kind == 0xFFFF0000u) {
        v.stage = Stage::Pixel;
    } else {
        return std::nullopt;
    }
    v.major = static_cast<int>((token >> 8) & 0xFF);
    v.minor = static_cast<int>(token & 0xFF);
    if (v.major < 1 || v.major > 3) {
        return std::nullopt;
    }
    return v;
}

std::size_t byteLength(const std::uint32_t* tokens, std::size_t maxTokens) {
    if (tokens == nullptr || maxTokens == 0) {
        return 0;
    }
    const auto version = parseVersion(tokens[0]);
    if (!version) {
        return 0;
    }
    std::size_t i = 1;
    while (i < maxTokens) {
        const std::uint32_t tok = tokens[i];
        if (tok == kEndToken) {
            return (i + 1) * sizeof(std::uint32_t);
        }
        const std::uint32_t opcode = tok & 0xFFFF;
        if (opcode == 0xFFFE) {  // comment: length in bits 16..30
            i += 1 + ((tok >> 16) & 0x7FFF);
            continue;
        }
        if (version->major >= 2) {
            i += 1 + ((tok >> 24) & 0x0F);
            continue;
        }
        // SM 1.x has no length field: parameter tokens have bit 31 set, except def's four float immediates
        ++i;
        if (opcode == 0x51) {  // def dst, f, f, f, f
            i += 5;
            continue;
        }
        while (i < maxTokens && (tokens[i] & 0x80000000u) != 0) {
            ++i;
        }
    }
    return 0;
}

}  // namespace rvk::shader
