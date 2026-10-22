#include "d3d9/semantics.h"

#include <algorithm>

#include "fx/effect_binary.h"

namespace rvk::d3d9 {
namespace {

struct NameEntry {
    std::string_view ctab;
    Semantic semantic;
};

constexpr NameEntry kNames[] = {
    {"g_vViewProjection", Semantic::ViewProjection},
    {"g_vInvProjection", Semantic::InvProjection},
    {"g_vInvView", Semantic::InvView},
    {"g_vCamPosAndFar", Semantic::CamPosAndFar},
    {"g_CamPosAndFar", Semantic::CamPosAndFar},
    {"g_vWindMatrix", Semantic::WindMatrix},
    {"g_vVegetationData1", Semantic::VegetationData},
    {"g_vLeafAngleMatrix", Semantic::LeafAngleMatrix},
    {"g_vLeafAngles", Semantic::LeafAngles},
    {"g_vFogSettings", Semantic::FogSettings},
    {"g_FogSettings", Semantic::FogSettings},
    {"g_vInstanceMisc", Semantic::InstanceMisc},
    {"g_vWorldMatrix", Semantic::WorldMatrix},
    {"g_vInstanceData", Semantic::InstanceData},
    {"g_vGlobalAlpha", Semantic::GlobalAlpha},
    {"g_vShaderData", Semantic::ShaderData},
    {"g_ShaderData", Semantic::ShaderData},
    {"g_ViewToScreenPos", Semantic::ViewToScreenPos},
    {"g_FogColor", Semantic::FogColor},
    {"g_CamObstacleAndMaskReference", Semantic::CamObstacle},
    {"g_IndoorAmbientColor", Semantic::IndoorAmbient},
    {"g_AmbientLight", Semantic::AmbientLight},
    {"g_DirLight", Semantic::DirLight},
    {"g_PointLights", Semantic::PointLights},
    {"smpShadowMask", Semantic::ShadowMask},
    {"smpRefract", Semantic::Refract},
    {"smpDepthmap", Semantic::DepthMap},
    {"smpLightMap1", Semantic::LightMap1},
    {"smpLightMap2", Semantic::LightMap2},
    {"g_SamplerData", Semantic::SamplerData},
    {"DepthToShadowMatrix", Semantic::ShadowMatrix},
    {"DepthToVarianceShadowMatrix", Semantic::VarianceMatrix},
    {"FadOut", Semantic::ShadowFade},
    {"MinVariance", Semantic::MinVariance},
    {"MinLightBleeding", Semantic::MinLightBleeding},
    {"DepthSampler", Semantic::MaskDepth},
    {"ShadowSampler", Semantic::ShadowSampler},
    {"VarianceShadowSampler", Semantic::VarianceSampler},
};

constexpr const char* kSemanticNames[] = {
    "ViewProjection", "InvProjection", "InvView",     "CamPosAndFar", "WindMatrix", "VegetationData", "LeafAngleMatrix",
    "LeafAngles",     "FogSettings",   "InstanceMisc", "WorldMatrix",  "InstanceData", "GlobalAlpha",  "ShaderData",
    "ViewToScreenPos", "FogColor",     "CamObstacle",  "IndoorAmbient", "AmbientLight", "DirLight",    "PointLights",
    "ShadowMatrix",   "VarianceMatrix", "ShadowFade",  "MinVariance",  "MinLightBleeding",
    "ShadowMask",     "Refract",       "DepthMap",     "LightMap1",    "LightMap2",    "SamplerData",  "MaskDepth",
    "ShadowSampler",  "VarianceSampler",
};
static_assert(std::size(kSemanticNames) == static_cast<std::size_t>(Semantic::Count));

constexpr const char* kFlagNames[] = {"Material", "Skinned",     "Instanced", "SpeedTree", "Leaves",
                                      "Vegetation", "Refracts", "ReadsDepth", "Lightmapped", "DirLight"};

bool isSampler(Semantic s) { return s >= Semantic::ShadowMask; }

}  // namespace

const char* name(Semantic s) {
    const auto i = static_cast<std::size_t>(s);
    return i < std::size(kSemanticNames) ? kSemanticNames[i] : "?";
}

Semantic semanticOf(std::string_view ctabName) {
    for (const NameEntry& e : kNames) {
        if (e.ctab == ctabName) return e.semantic;
    }
    return Semantic::Count;
}

std::string flagNames(std::uint32_t flags) {
    std::string s;
    for (std::size_t i = 0; i < std::size(kFlagNames); ++i) {
        if ((flags & (1u << i)) != 0) {
            if (!s.empty()) s += ' ';
            s += kFlagNames[i];
        }
    }
    return s;
}

ShaderSemantics shaderSemantics(std::span<const std::uint32_t> tokens, const shader::Reflection& reflection) {
    ShaderSemantics out;
    const auto ct = fx::parseConstantTable(tokens);
    if (ct.ok() && ct.table.present) {
        out.hasCtab = true;
        for (const fx::Constant& c : ct.table.constants) {
            const Semantic s = semanticOf(c.name);
            if (s == Semantic::Count) continue;
            // a sampler name must be in the sampler set, a constant in the float set (the same name never changes sets)
            const bool sampler = c.registerSet == fx::RegisterSet::Sampler;
            if (sampler != isSampler(s) || (!sampler && c.registerSet != fx::RegisterSet::Float4)) continue;
            auto& r = out.ranges[static_cast<std::size_t>(s)];
            r.reg = static_cast<std::int16_t>(c.registerIndex);
            r.count = static_cast<std::uint16_t>(std::max<std::uint32_t>(1, c.registerCount));
        }
    }
    const bool vs = reflection.version.stage == shader::Stage::Vertex;
    const auto has = [&](Semantic s) { return out[s].present(); };
    if (vs) {
        bool blendIndices = false, texcoord4 = false;
        for (const auto& in : reflection.vertexInputs) {
            blendIndices |= in.usage == shader::Usage::BlendIndices;
            texcoord4 |= in.usage == shader::Usage::TexCoord && in.usageIndex == 4;
        }
        if (has(Semantic::ViewProjection)) out.flags |= ShaderSemantics::Material;
        if (has(Semantic::WindMatrix)) out.flags |= ShaderSemantics::SpeedTree;
        if (has(Semantic::LeafAngles) || has(Semantic::LeafAngleMatrix)) out.flags |= ShaderSemantics::Leaves;
        if (has(Semantic::VegetationData)) out.flags |= ShaderSemantics::Vegetation;
        if (blendIndices && has(Semantic::InstanceData) && !has(Semantic::WindMatrix)) out.flags |= ShaderSemantics::Skinned;
        if (texcoord4 && has(Semantic::InstanceData)) out.flags |= ShaderSemantics::Instanced;
    } else {
        if (has(Semantic::SamplerData)) out.flags |= ShaderSemantics::Material;
        if (has(Semantic::Refract)) out.flags |= ShaderSemantics::Refracts;
        if (has(Semantic::DepthMap)) out.flags |= ShaderSemantics::ReadsDepth;
        if (has(Semantic::LightMap1)) out.flags |= ShaderSemantics::Lightmapped;
        if (has(Semantic::DirLight)) out.flags |= ShaderSemantics::DirLight;
    }
    return out;
}

}  // namespace rvk::d3d9
