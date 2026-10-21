#pragma once

// D3D9 shader bytecode (SM 1.x - 3.0) token-stream basics.

#include <cstddef>
#include <cstdint>
#include <optional>

namespace rvk::shader {

enum class Stage { Vertex, Pixel };

struct Version {
    Stage stage = Stage::Vertex;
    int major = 0;
    int minor = 0;
};

// The version token (first DWORD): 0xFFFE0300 = vs_3_0, 0xFFFF0300 = ps_3_0.
std::optional<Version> parseVersion(std::uint32_t token);

// Size in bytes of the shader at `tokens` including the END token, or 0 if no END is found within `maxTokens`
// or the stream is malformed. Skips comments (CTAB etc.) and, for SM2+, whole instructions by their length field, so a
// constant that happens to equal the END token is not mistaken for it.
std::size_t byteLength(const std::uint32_t* tokens, std::size_t maxTokens);

inline constexpr std::uint32_t kEndToken = 0x0000FFFF;

}  // namespace rvk::shader
