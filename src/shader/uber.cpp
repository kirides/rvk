#include "shader/uber.h"

#include <cstdint>
#include <format>

#include "shader/embedded.h"

namespace rvk::shader {
namespace {

// removes every line of `text` that starts (after blanks) with one of the given prefixes
std::string withoutLines(std::string_view text, std::initializer_list<std::string_view> prefixes) {
    std::string out;
    out.reserve(text.size());
    std::size_t pos = 0;
    while (pos < text.size()) {
        std::size_t end = text.find('\n', pos);
        if (end == std::string_view::npos) end = text.size();
        const std::string_view line = text.substr(pos, end - pos);
        std::size_t first = line.find_first_not_of(" \t");
        bool drop = false;
        if (first != std::string_view::npos) {
            for (const std::string_view p : prefixes) {
                if (line.substr(first).starts_with(p)) drop = true;
            }
        }
        if (!drop) {
            out.append(line);
            out.push_back('\n');
        }
        pos = end + 1;
    }
    return out;
}

bool validName(std::string_view n) {
    if (n.empty()) return false;
    for (const char c : n) {
        if (!(c == '_' || (c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z'))) return false;
    }
    return !(n[0] >= '0' && n[0] <= '9');
}

}  // namespace

bool uberSupports(std::string_view effectFile, std::string_view entry) {
    return effectFile == "ge_default_phong.fx" && (entry == "PS_Default" || entry == "PS_DefaultZPass");
}

std::string uberPixelGlsl(std::string_view entry, const MacroList& macros, std::string_view hookDefines) {
    std::string out = "#version 460\n// GLSL material shader (shaders/material) built from the engine's macros\n";
    for (const auto& [name, value] : macros) {
        if (!validName(name)) return {};
        std::string v;
        if (value == "true") v = "1";
        else if (value == "false") v = "0";
        else {
            // a macro body on one line, blanks collapsed
            bool blank = false;
            for (const char c : value) {
                if (c == ' ' || c == '\t' || c == '\r' || c == '\n') {
                    blank = true;
                    continue;
                }
                if (blank && !v.empty()) v.push_back(' ');
                blank = false;
                v.push_back(c);
            }
        }
        out += std::format("#define {} {}\n", name, v);
    }
    if (entry.ends_with("ZPass")) out += "#define XFX_PASS 1\n";
    if (!hookDefines.empty()) {
        out += "#define RVK_UBER_HOOKS 1\n";
        out += hookDefines;
        out += "\n";
    }
    // the ABI (the translated shaders' header), then the enhancement code the hooks call, then the shader itself
    out += embedded::abiGlsl();
    out += "\n";
    if (!hookDefines.empty()) {
        out += embedded::giGlsl();
        out += "\n";
        out += embedded::materialVariantGlsl();
        out += "\n";
    }
    out += withoutLines(embedded::uberGlsl(), {"#extension GL_GOOGLE_include_directive", "#include"});
    out += withoutLines(embedded::uberFrag(), {"#version", "#include"});
    return out;
}

std::uint32_t uberClassMask(std::string_view entry, const MacroList& macros) {
    std::uint32_t m = entry.ends_with("ZPass") ? 1u : 0u;
    const auto on = [&](std::string_view name) {
        for (const auto& [n, v] : macros) {
            if (n == name) return v == "true" || v == "1";
        }
        return false;
    };
    const auto has = [&](std::string_view name) {
        for (const auto& [n, v] : macros) {
            if (n == name) return true;
        }
        return false;
    };
    if (on("XFX_MASKED")) m |= 2u;
    if (on("XFX_ALPHABLEND")) m |= 4u;
    if (on("XFX_LIGHTMAP")) m |= 8u;
    if (has("XFX_PS_NORMAL")) m |= 16u;
    if (on("XFX_SPECULAR")) m |= 32u;
    if (has("XFX_PS_DISTORTION")) m |= 64u;
    if (m == 0) m = 128u;
    return m;
}

std::uint64_t uberSourceHash() {
    std::uint64_t h = 1469598103934665603ull;
    for (const std::string_view t : {embedded::uberGlsl(), embedded::uberFrag()}) {
        for (const char c : t) {
            h ^= static_cast<std::uint8_t>(c);
            h *= 1099511628211ull;
        }
    }
    return h;
}

}  // namespace rvk::shader
