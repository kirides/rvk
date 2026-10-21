#pragma once

// Shader semantics from the CTAB (roadmap do-now item 4, docs/research/draw-tags.md 3): the registers of the engine's named
// shader constants and samplers (g_vViewProjection, g_vInstanceData, g_DirLight, g_AmbientLight, smpRefract, smpDepthmap,
// g_SamplerData, ...), resolved once per shader at creation and cached on d3d9::Shader, plus flags derived from them and from
// the vertex inputs (skinning, instancing, SpeedTree wind, vegetation). Lets later passes read the camera, world matrices,
// bones and lights of a draw from its constants without more engine RE. Pure logic (unit tests: tests/draws_tests.cpp).
//
// Names surveyed over the captured corpus (149 VS, 2334 PS; draw-tags.md 3.1): CTAB names survive in every material shader;
// g_bSkinned / g_bInstanced are compile-time and absent, so skinning shows as a BLENDINDICES input (without SpeedTree wind)
// and instancing as a TEXCOORD4 input next to g_vInstanceData.

#include <array>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>

#include "shader/glsl.h"

namespace rvk::d3d9 {

enum class Semantic : std::uint8_t {
    // float constants (vertex shader names have a g_v prefix; CamPosAndFar / ShaderData / FogSettings exist in both stages)
    ViewProjection,   // VS c0   g_vViewProjection (transposed view-projection, frame-phases.md 5)
    InvProjection,    // VS c4   g_vInvProjection
    InvView,          // VS c8   g_vInvView (3 registers)
    CamPosAndFar,     // VS c11 g_vCamPosAndFar / PS c0 g_CamPosAndFar
    WindMatrix,       // VS c14  g_vWindMatrix (SpeedTree)
    VegetationData,   // VS c12  g_vVegetationData1 (grass)
    LeafAngleMatrix,  // VS c23  g_vLeafAngleMatrix (SpeedTree leaves)
    LeafAngles,       // VS c26  g_vLeafAngles (SpeedTree leaves)
    FogSettings,      // VS c30 g_vFogSettings / PS c4 g_FogSettings
    InstanceMisc,     // VS c31  g_vInstanceMisc
    WorldMatrix,      // VS c31  g_vWorldMatrix (a few non-material shaders)
    InstanceData,     // VS c32  g_vInstanceData[] ([0].a alpha, world 3x4, instances, bones)
    GlobalAlpha,      // VS c34  g_vGlobalAlpha
    ShaderData,       // VS c232 g_vShaderData / PS c6 g_ShaderData
    ViewToScreenPos,  // PS c1   g_ViewToScreenPos
    FogColor,         // PS c3   g_FogColor
    CamObstacle,      // PS c5   g_CamObstacleAndMaskReference
    IndoorAmbient,    // PS c14  g_IndoorAmbientColor
    AmbientLight,     // PS c15  g_AmbientLight[7]
    DirLight,         // PS c22  g_DirLight (3 registers)
    PointLights,      // PS c25  g_PointLights[]
    // samplers
    ShadowMask,       // s0 smpShadowMask
    Refract,          // s1 smpRefract (the refraction copy: water)
    DepthMap,         // s2 smpDepthmap (linear depth)
    LightMap1,        // s4 smpLightMap1
    LightMap2,        // s5 smpLightMap2
    SamplerData,      // s9.. g_SamplerData[] (material samplers)
    Count
};
const char* name(Semantic s);

struct ShaderSemantics {
    struct Range {
        std::int16_t reg = -1;  // first register (c# / s#), -1 = absent
        std::uint16_t count = 0;
        bool present() const { return reg >= 0; }
    };
    bool hasCtab = false;
    std::array<Range, static_cast<std::size_t>(Semantic::Count)> ranges{};

    // derived flags
    enum : std::uint32_t {
        Material = 1u << 0,      // an engine material shader (g_vViewProjection in a VS, g_SamplerData in a PS)
        Skinned = 1u << 1,       // VS: BLENDINDICES input with g_vInstanceData and no SpeedTree wind (bones in g_vInstanceData)
        Instanced = 1u << 2,     // VS: TEXCOORD4 input (instance index) with g_vInstanceData
        SpeedTree = 1u << 3,     // VS: g_vWindMatrix
        Leaves = 1u << 4,        // VS: g_vLeafAngles / g_vLeafAngleMatrix
        Vegetation = 1u << 5,    // VS: g_vVegetationData1
        Refracts = 1u << 6,      // PS: samples smpRefract
        ReadsDepth = 1u << 7,    // PS: samples smpDepthmap
        Lightmapped = 1u << 8,   // PS: smpLightMap1
        DirLight = 1u << 9,      // PS: g_DirLight
    };
    std::uint32_t flags = 0;

    const Range& operator[](Semantic s) const { return ranges[static_cast<std::size_t>(s)]; }
    bool has(std::uint32_t f) const { return (flags & f) == f; }
};

// The semantic of a CTAB name (Semantic::Count when unknown)
Semantic semanticOf(std::string_view ctabName);
// From the shader tokens (version token first) and its translator reflection (vertex inputs)
ShaderSemantics shaderSemantics(std::span<const std::uint32_t> tokens, const shader::Reflection& reflection);
// "Material Skinned SpeedTree" (empty for 0)
std::string flagNames(std::uint32_t flags);

}  // namespace rvk::d3d9
