#include "shader/uber.h"

#include <cstdint>
#include <format>

#include "shader/embedded.h"

namespace rvk::shader {
namespace {

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

bool uberSupportsFile(std::string_view effectFile, std::string_view entry) {
    return uberSupports(effectFile, entry) ||
           (effectFile == "ge_default_leafs.fx" && (entry == "PS_Default" || entry == "PS_DefaultZPass"));
}

std::string uberPixelHlsl(std::string_view entry, const MacroList& macros, std::string_view hookDefines, bool sceneRecord, bool sceneView) {
    return uberPixelHlsl("ge_default_phong.fx", entry, macros, hookDefines, sceneRecord, sceneView);
}

std::string uberPixelHlsl(std::string_view effectFile, std::string_view entry, const MacroList& macros, std::string_view hookDefines,
                          bool sceneRecord, bool sceneView) {
    std::string out = "// HLSL material shader (shaders/material) built from the engine's macros\n";
    if (effectFile == "ge_default_leafs.fx") out += "#define RVK_LEAFS 1\n";  // the SpeedTree leaf hooks (ge_default.frag.hlsl)
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
    if (sceneView) out += "#define RVK_SCENE_VIEW 1\n";  // scene ABI v2 (abi.h ViewBlock2): the view's windows / posFix / c5.xyz from the view table
    if (sceneRecord || sceneView) out += "#define RVK_SCENE_RECORD 1\n";  // scene variant (abi.h DrawRecord): the draw's data from a record, not the push block
    if (!hookDefines.empty()) {
        out += "#define RVK_UBER_HOOKS 1\n";
        out += hookDefines;
        out += "\n";
    }
    out += "#include \"material/ge_default.frag.hlsl\"\n";
    return out;
}

std::uint64_t uberHlslHash() {
    std::uint64_t h = 1469598103934665603ull;
    for (const char* name : {"material/ge_default.frag.hlsl", "material/ge_default.hlsli", "include/rvk_material_variant.hlsli", "include/rvk_gi.hlsli",
                             "include/rvk_draw.hlsli", "include/rvk_abi.hlsli", "include/rvk_common.hlsli", "include/rvk_oct.hlsli"}) {
        for (const char c : embedded::hlslSource(name)) {
            h ^= static_cast<std::uint8_t>(c);
            h *= 1099511628211ull;
        }
    }
    return h;
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

}  // namespace rvk::shader
