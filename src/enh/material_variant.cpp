#include "enh/material_variant.h"

#include <format>
#include <string>

#include "d3d9/draws.h"
#include "shader/embedded.h"

namespace rvk::enh {

using d3d9::Semantic;

bool materialVariantOptions(const d3d9::Shader& s, MaterialVariant kind, shader::GlslOptions& o) {
    if (s.stage != shader::Stage::Pixel || (s.reflection.colorOutputs & 1u) == 0) return false;
    const auto& amb = s.semantics[Semantic::AmbientLight];
    const bool ambient = amb.present() && amb.count >= 7;
    const bool gbuffer = kind != MaterialVariant::Gi;
    if (!ambient && !gbuffer) return false;
    // G-buffer variants: material shaders only (the engine's material graph samplers, or lit)
    if (gbuffer && !ambient && !s.semantics.has(d3d9::ShaderSemantics::Material)) return false;
    const auto& ind = s.semantics[Semantic::IndoorAmbient];

    std::string defines;
    if (ambient) {
        defines += std::format("#define RVK_MV_AMBIENT 1\n#define RVK_MV_AMB_REG {}\n", amb.reg);
        if (ind.present()) defines += std::format("#define RVK_MV_HAS_IND 1\n#define RVK_MV_IND_REG {}\n", ind.reg);
    }
    if (gbuffer) defines += "#define RVK_MV_GBUFFER 1\n";
    if (kind == MaterialVariant::GbufferMotion) defines += "#define RVK_MV_MOTION 1\n";
    defines += std::format("#define RVK_MV_KIND_SKINNED {}\n", static_cast<unsigned>(d3d9::draws::Kind::Skinned));
    // the interpolated world normal (TEXCOORD4 of the material vertex shader: vNormalAndAO)
    std::string vertexNormal = "vec4(0.0)";
    for (const auto& in : s.reflection.pixelInputs) {
        if (in.location == shader::abi::varyingLocation(shader::Usage::TexCoord, 4)) vertexNormal = in.name;
    }
    if (vertexNormal != "vec4(0.0)") defines += "#define RVK_MV_VNORMAL 1\n";

    o.prelude = "// enhancement variant (enh/material_variant)\n" + defines + std::string(shader::embedded::giGlsl()) + "\n" +
                std::string(shader::embedded::materialVariantGlsl());
    o.prologue = std::format("    rvkMvPrologue({});", vertexNormal);
    o.epilogue = "    rvkMvEpilogue(oC0);";
    if (ambient) {
        for (int i = 0; i < 7; ++i) o.constOverrides[static_cast<std::uint32_t>(amb.reg + i)] = std::format("rvk_gi[{}]", 1 + i);
        if (ind.present()) o.constOverrides[static_cast<std::uint32_t>(ind.reg)] = "rvk_gi[0]";
        // the normal the shader evaluates its ambient with (normal-mapped, world space)
        if (gbuffer) o.dotCaptures.push_back({static_cast<std::uint32_t>(amb.reg), "rvk_gbN"});
    }
    return true;
}

}  // namespace rvk::enh
