#pragma once

// The material graph id (frame-ownership.md 1.4 / 7.3): the engine's request for a material shader carries the material graph as XFX_PS_* / XFX_VS_*
// expression macros; the slot / register indices in them (g_ShaderData[i] / g_vShaderData[i] / g_SamplerData[i]) are an assignment, not structure, and
// become [#]. A graph id is the hash of (file, entry, normalised graph text); a pipeline id adds the static macros (everything that is neither a graph
// macro nor one of the dynamic light macros the owned shader turns into uniform branches). This is `tools/material_graph_census.py` (normalise /
// split_request) in C++: tests/material_table_tests.cpp and tools/tests/test_material_graph_census.py share the vectors.

#include <algorithm>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace rvk::gpuscene {

using MacroList = std::vector<std::pair<std::string, std::string>>;

inline bool isGraphMacro(std::string_view n) { return n.starts_with("XFX_PS_") || n.starts_with("XFX_VS_"); }

inline bool isDynamicMacro(std::string_view n) {
    static constexpr std::string_view kDynamic[] = {"XFX_DIRLIGHT",    "XFX_DIRLIGHTSHADOWS", "XFX_PNTLIGHTCOUNT",   "XFX_PNTLIGHTSHADOWS",
                                                    "XFX_LIGHTMAP",    "XFX_AMBIENTLIGHT",    "XFX_AMBIENTLIGHTTYPE", "XFX_AMBIENTOCCLUSION"};
    return std::find(std::begin(kDynamic), std::end(kDynamic), n) != std::end(kDynamic);
}

// slot / register indices -> [#]: the regex \b(g_ShaderData|g_vShaderData|g_SamplerData)\s*\[\s*(\d+)\s*\] -> name[#]
inline std::string normaliseGraph(std::string_view t) {
    static constexpr std::string_view kNames[] = {"g_ShaderData", "g_vShaderData", "g_SamplerData"};
    const auto isWord = [](char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_'; };
    const auto isSpace = [](char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v'; };
    std::string out;
    out.reserve(t.size());
    std::size_t i = 0;
    while (i < t.size()) {
        bool done = false;
        if (i == 0 || !isWord(t[i - 1])) {
            for (const std::string_view name : kNames) {
                if (t.substr(i).starts_with(name)) {
                    std::size_t j = i + name.size();
                    while (j < t.size() && isSpace(t[j])) ++j;
                    if (j >= t.size() || t[j] != '[') continue;
                    ++j;
                    while (j < t.size() && isSpace(t[j])) ++j;
                    const std::size_t d0 = j;
                    while (j < t.size() && t[j] >= '0' && t[j] <= '9') ++j;
                    if (j == d0) continue;
                    while (j < t.size() && isSpace(t[j])) ++j;
                    if (j >= t.size() || t[j] != ']') continue;
                    out.append(name);
                    out.append("[#]");
                    i = j + 1;
                    done = true;
                    break;
                }
            }
        }
        if (!done) out.push_back(t[i++]);
    }
    return out;
}

// "k=normalised v ;; ..." over the graph macros sorted by (name, value); a vertex-stage request only counts its XFX_VS_ macros
inline std::string graphText(const MacroList& macros, bool vertexStage) {
    std::vector<std::pair<std::string, std::string>> items;
    for (const auto& m : macros) {
        if (!isGraphMacro(m.first) || (vertexStage && !m.first.starts_with("XFX_VS_"))) continue;
        items.push_back(m);
    }
    std::sort(items.begin(), items.end());
    std::string out;
    for (std::size_t i = 0; i < items.size(); ++i) {
        if (i != 0) out += " ;; ";
        out += items[i].first + "=" + normaliseGraph(items[i].second);
    }
    return out;
}

// the static macros (neither graph nor dynamic), sorted: "NAME=value NAME=value"
inline std::string staticText(const MacroList& macros) {
    std::vector<std::pair<std::string, std::string>> items;
    for (const auto& m : macros) {
        if (!isGraphMacro(m.first) && !isDynamicMacro(m.first)) items.push_back(m);
    }
    std::sort(items.begin(), items.end());
    std::string out;
    for (const auto& m : items) {
        if (!out.empty()) out += ' ';
        out += m.first + "=" + m.second;
    }
    return out;
}

inline std::uint64_t fnv1a(std::string_view s, std::uint64_t h = 0xcbf29ce484222325ull) {
    for (const unsigned char c : s) {
        h ^= c;
        h *= 0x100000001b3ull;
    }
    return h;
}

inline std::uint32_t fold32(std::uint64_t h) { return static_cast<std::uint32_t>(h ^ (h >> 32)) | 1u; }  // never 0 (0 = not seen)

// graph id = (file, entry, graph text), pipeline id = + the static macros
inline std::uint32_t graphId(std::string_view file, std::string_view entry, std::string_view graph) {
    std::uint64_t h = fnv1a(file);
    h = fnv1a("\n", h);
    h = fnv1a(entry, h);
    h = fnv1a("\n", h);
    return fold32(fnv1a(graph, h));
}
inline std::uint32_t pipelineId(std::uint32_t graph, std::string_view staticMacros) { return fold32(fnv1a(staticMacros, 0x9e3779b97f4a7c15ull ^ graph)); }

}  // namespace rvk::gpuscene
