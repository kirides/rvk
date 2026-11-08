#include "shader/uber.h"

#include <cstdint>
#include <format>
#include <span>

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

// the request's macros as #defines (true / false -> 1 / 0, a body on one line); false on an invalid name
bool appendMacros(std::string& out, const MacroList& macros) {
    for (const auto& [name, value] : macros) {
        if (!validName(name)) return false;
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
    return true;
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
    if (!appendMacros(out, macros)) return {};
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

bool uberVsSupports(std::string_view effectFile, std::string_view entry) {
    return (effectFile == "ge_default_phong.fx" || effectFile == "ge_default_leafs.fx") && (entry == "VS_Default" || entry == "VS_DefaultZPass");
}

namespace {
// the HLSL attribute (shaders/material/ge_default.vert.hlsl RVK_LOC_<name>) of a D3D9 vertex input; null = the port has no such input
const char* uberAttrName(Usage usage, std::uint32_t index) {
    switch (usage) {
    case Usage::Position: return index == 0 ? "aPos" : index == 1 ? "aPosDelta1" : index == 2 ? "aPosDelta2" : index == 3 ? "aPosDelta3" : nullptr;
    case Usage::Normal: return index == 0 ? "aNorm" : index == 1 ? "aNormDelta1" : index == 2 ? "aNormDelta2" : index == 3 ? "aNormDelta3" : nullptr;
    case Usage::Tangent: return index == 0 ? "aTangent" : nullptr;
    case Usage::Color: return index == 0 ? "aCol0" : index == 1 ? "aCol1" : nullptr;
    case Usage::TexCoord:
        switch (index) {
        case 0: return "aTex0";
        case 1: return "aTex1";
        case 2: return "aTex2";
        case 3: return "aTex3";
        case 4: return "aInstance";
        case 5: return "aBillboard";
        case 6: return "aLightmap";
        default: return nullptr;
        }
    case Usage::BlendIndices: return index == 0 ? "aIndices0" : nullptr;
    case Usage::BlendWeight: return index == 0 ? "aWeights0" : index == 1 ? "aWeights1" : index == 2 ? "aWeights2" : nullptr;
    default: return nullptr;
    }
}
}  // namespace

std::string uberVertexHlsl(std::string_view effectFile, std::string_view entry, const MacroList& macros, std::span<const VertexInput> inputs, bool sceneRecord,
                           bool sceneView, std::string_view extraDefines) {
    (void)effectFile;  // the vertex shaders of both effect files are ge_default_vs.fx
    std::string out = "// HLSL material vertex shader (shaders/material/ge_default.vert.hlsl) built from the engine's macros + the bytecode's input declarations\n";
    if (!appendMacros(out, macros)) return {};
    if (entry.ends_with("ZPass")) out += "#define XFX_PASS 1\n";
    out += "#define RVK_VS_DECL 1\n";
    out += extraDefines;
    if (!extraDefines.empty() && extraDefines.back() != '\n') out += "\n";
    for (const VertexInput& in : inputs) {
        const char* n = uberAttrName(in.usage, in.index);
        if (n == nullptr) continue;
        out += std::format("#define RVK_LOC_{} {}\n#define RVK_SEM_{} ATTR{}\n", n, in.reg, n, in.reg);
    }
    if (sceneView) out += "#define RVK_SCENE_VIEW 1\n";
    if (sceneRecord || sceneView) out += "#define RVK_SCENE_RECORD 1\n";
    out += "#include \"material/ge_default.vert.hlsl\"\n";
    return out;
}

std::uint64_t uberHlslHash() {
    std::uint64_t h = 1469598103934665603ull;
    for (const char* name : {"material/ge_default.frag.hlsl", "material/ge_default.vert.hlsl", "material/ge_default.hlsli", "include/rvk_material_variant.hlsli", "include/rvk_gi.hlsli",
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
