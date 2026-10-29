#pragma once

// Test helper: the member offsets of the structs in a SPIR-V module (the ABI layout tests compare them with the C++ structs)

#include <cstdint>
#include <map>
#include <span>
#include <string>

namespace rvk::test {

// struct name -> member name -> Offset decoration, from a SPIR-V module (OpName 5, OpMemberName 6, OpMemberDecorate 72)
inline std::map<std::string, std::map<std::string, std::uint32_t>> memberOffsets(std::span<const std::uint32_t> words) {
    std::map<std::uint32_t, std::string> names;
    std::map<std::uint32_t, std::map<std::uint32_t, std::string>> memberNames;
    std::map<std::uint32_t, std::map<std::uint32_t, std::uint32_t>> offsets;
    const auto str = [](std::span<const std::uint32_t> w) {
        std::string s;
        for (std::uint32_t x : w) {
            for (int b = 0; b < 4; ++b) {
                const char c = static_cast<char>((x >> (8 * b)) & 0xff);
                if (c == '\0') return s;
                s += c;
            }
        }
        return s;
    };
    for (std::size_t i = 5; i < words.size();) {
        const std::uint32_t op = words[i] & 0xffff, n = words[i] >> 16;
        if (n == 0 || i + n > words.size()) break;
        const auto ins = words.subspan(i, n);
        if (op == 5 && n >= 3) names[ins[1]] = str(ins.subspan(2));
        if (op == 6 && n >= 4) memberNames[ins[1]][ins[2]] = str(ins.subspan(3));
        if (op == 72 && n >= 5 && ins[3] == 35) offsets[ins[1]][ins[2]] = ins[4];  // Offset
        i += n;
    }
    std::map<std::string, std::map<std::string, std::uint32_t>> out;
    for (const auto& [id, members] : offsets) {
        auto& o = out[names[id]];
        for (const auto& [m, off] : members) o[memberNames[id][m]] = off;
    }
    return out;
}

}  // namespace rvk::test
