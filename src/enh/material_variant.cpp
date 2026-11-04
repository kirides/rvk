#include "enh/material_variant.h"

#include <algorithm>
#include <format>
#include <string>

#include "d3d9/draws.h"
#include "shader/embedded.h"

namespace rvk::enh {

using d3d9::Semantic;

namespace {
bool g_pbr = false;
}
void setPbrVariants(bool on) { g_pbr = on; }
bool pbrVariants() { return g_pbr; }

std::string materialVariantTag(std::string_view base) {
    std::uint64_t h = 1469598103934665603ull;
    for (const std::string_view t : {shader::embedded::hlslSource("include/rvk_gi.hlsli"), shader::embedded::hlslSource("include/rvk_material_variant.hlsli")}) {
        for (const char c : t) {
            h ^= static_cast<std::uint8_t>(c);
            h *= 1099511628211ull;
        }
    }
    return std::format("{}{}{:08x}", base, g_pbr ? "-pbr" : "", static_cast<std::uint32_t>(h ^ (h >> 32)));
}

bool materialVariantOptions(const d3d9::Shader& s, MaterialVariant kind, shader::TranslateOptions& o) {
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
    const auto& dirLight = s.semantics[Semantic::DirLight];
    const auto& pntLights = s.semantics[Semantic::PointLights];
    const bool pbr = g_pbr && (dirLight.present() || pntLights.present());
    if (pbr) {
        defines += std::format("#define RVK_MV_PBR 1\n#define RVK_MV_PBR_DIR_REG {}\n#define RVK_MV_PBR_PNT_REG {}\n#define RVK_MV_PBR_PNT_COUNT {}\n",
                               dirLight.present() ? dirLight.reg : -1, pntLights.present() ? pntLights.reg : 0,
                               pntLights.present() ? std::min(3, pntLights.count / 4) : 0);
        const auto& sm = s.semantics[Semantic::ShadowMask];
        defines += std::format("#define RVK_MV_PBR_SHADOW_SLOT {}\n", sm.present() ? sm.reg : 0);
    }
    if (gbuffer) defines += "#define RVK_MV_GBUFFER 1\n";
    if (kind == MaterialVariant::GbufferMotion) defines += "#define RVK_MV_MOTION 1\n";
    defines += std::format("#define RVK_MV_KIND_SKINNED {}\n", static_cast<unsigned>(d3d9::draws::Kind::Skinned));
    // the interpolated world normal (TEXCOORD4 of the material vertex shader: vNormalAndAO)
    const std::string kNoNormal = "float4(0.0, 0.0, 0.0, 0.0)";
    std::string vertexNormal = kNoNormal;
    for (const auto& in : s.reflection.pixelInputs) {
        if (in.location == shader::abi::varyingLocation(shader::Usage::TexCoord, 4)) vertexNormal = in.name;
    }
    if (vertexNormal != kNoNormal) defines += "#define RVK_MV_VNORMAL 1\n";

    o.uber = true;  // built from the HLSL material shader when the device uses it (d3d9::Device::setUbershader)
    o.uberDefines = defines;
    // HLSL hooks (docs/research/hlsl-rhi.md 1, P1d): the prelude includes the GI lookup and the variant code; the prologue / epilogue pass the pixel
    // position and write the extra G-buffer targets of the entry's output struct; the previous world position of the motion variant is an extra
    // varying input of the entry (location kPrevWorldLocation)
    o.prelude = "// enhancement variant (enh/material_variant)\n" + defines + "#include \"rvk_gi.hlsli\"\n#include \"rvk_material_variant.hlsli\"\n";
    if (kind == MaterialVariant::GbufferMotion) {
        o.extraInputs = std::format("    [[vk::location({})]] float4 rvk_prevWorld : VARY{};", kPrevWorldLocation, kPrevWorldLocation);
        o.prologue = "    rvk_prevWorld = rvk_in.rvk_prevWorld;\n";
    }
    o.prologue += std::format("    rvkMvPrologue(rvk_fragCoord, {});", vertexNormal);
    o.epilogue = "    rvkMvEpilogue(oC0);";
    if (gbuffer) {
        // (no [[vk::location]] on pixel outputs: DXC rejects a partial explicit assignment next to oC0 : SV_Target0)
        o.extraOutputs = "    float4 rvk_gbNormal : SV_Target1;\n    float2 rvk_gbMotion : SV_Target2;";
        o.epilogue += "\n    rvk_out.rvk_gbNormal = rvk_mvNormalOut;\n    rvk_out.rvk_gbMotion = rvk_mvMotionOut;";
    }
    if (ambient) {
        for (int i = 0; i < 7; ++i) o.constOverrides[static_cast<std::uint32_t>(amb.reg + i)] = std::format("rvk_gi[{}]", 1 + i);
        if (ind.present()) o.constOverrides[static_cast<std::uint32_t>(ind.reg)] = "rvk_gi[0]";
        // the normal the shader evaluates its ambient with (normal-mapped, world space)
        if (gbuffer || pbr) o.dotCaptures.push_back({static_cast<std::uint32_t>(amb.reg), "rvk_gbN"});
    }
    if (pbr) o.texCaptures.push_back({9, "rvk_pbrUv", "rvk_pbrAlbedo", "rvk_pbrOk"});  // the diffuse (g_SamplerData[0] = s9)
    return true;
}

}  // namespace rvk::enh
