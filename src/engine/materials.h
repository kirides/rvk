#pragma once

// The engine's material graph (docs/research/rt-albedo-facts.md 1): which g_SamplerData slot (pixel sampler s(9 + k)) holds
// a material's diffuse (albedo), its opacity and its normal map, its blend mode and mask reference. Read from the shader the
// engine is executing (eCShaderBase::ms_pActiveShader, set by ExecuteShaderConstants before each pass's draws) with
// SEH-guarded reads and vtable checks; cached per shader object (the walk) and re-validated on every lookup. Render thread.

#include <cstdint>
#include <string>

namespace rvk::engine::materials {

enum class ShaderClass : std::uint8_t { Unknown, Default, Leafs, Other };
enum class Source : std::uint8_t {
    None,         // the role has no node
    Sampler,      // a texture (slot = its g_SamplerData index)
    Cube,         // only a cube map (reflection): no albedo
    Constant,     // a constant colour (eCColorSrcConstant)
    VertexColor,  // the vertex colour
    Other,        // something else without a sampler (Fresnel, camera fades, ...)
};

struct Role {
    Source source = Source::None;
    std::int8_t slot = -1;          // g_SamplerData index of the first sampler of the role's tree (depth first, the engine's order)
    std::uint8_t uvSet = 0;         // that sampler's XFX_VS_INPUT_TEX<n> (+0xc8), when it has no tex-coord source
    bool uvSource = false;          // it has a tex-coord source (scale / scroller / rotator ...: the VS or PS computes the uv)
    std::uint8_t swizzle = 0;       // the role proxy's channel swizzle (+0x28): 0 none, 1 .rgb, 2 .r, 3 .g, 4 .b, 5 .a
    const void* image = nullptr;    // the pool's eCImageResource2 of the slot (names in logs)
};

struct ShaderInfo {
    const void* shader = nullptr;   // eCShaderBase*
    ShaderClass cls = ShaderClass::Unknown;
    std::uint8_t blendMode = 0;     // +0x50: 0 Normal, 1 Masked, 2 AlphaBlend, ...
    std::uint8_t maskReference = 0; // +0x58
    std::uint8_t samplerBase = 9;   // +0x219 (first register)
    std::uint8_t samplerCount = 0;  // +0x21b (slots handed out)
    Role diffuse, opacity, normal;
    Role selfIllum;                 // the SelfIllumination role (+0x340): added unlit after the lighting (lava, lamps, fire meshes)
};

// whether the symbols (ms_pActiveShader, the vtables) were found
bool available();
// the shader of the draw: its eCMaterialResource2's (+0x70) when `material` is given (the draw tag's; the z pre-pass and
// the shadow passes do not execute the material's constants, ms_pActiveShader is stale there), else the one the engine
// executes now (eCShaderBase::ms_pActiveShader); false when there is none / it is not a material shader we can read
bool active(ShaderInfo& out, const void* material = nullptr);
// the engine's API texture (eCAPITexture*) of an eCImageResource2: its eCGfxImageData (+0x70), whose +0x60 is the API object
// (eCGfxImageData::Execute binds it); null when not loaded
const void* apiTextureOf(const void* imageResource);
// a shader's eCMaterialResource2 +0x70 (to cross-check ms_pActiveShader against the draw's material); null when unreadable
const void* shaderOfMaterial(const void* material);
// names for logs: the file title of the pool image of slot k ("" when none), of a resource (+0x40)
std::string poolImageName(const ShaderInfo& info, int slot);
std::string resourceName(const void* resource);
const char* name(Source s);
const char* name(ShaderClass c);

}  // namespace rvk::engine::materials
