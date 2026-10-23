#pragma once

// Variants of the game's material pixel shaders for enhancements (docs/research/rtgi.md 9-10): the GLSL of
// shaders/include/rvk_material_variant.glsl (after rvk_gi.glsl) pasted into the translated shader with a few defines; the
// translated program itself is unchanged apart from its ambient constants (overrides) and one operand capture.
//
//   Gi              every read of g_AmbientLight / g_IndoorAmbientColor = the GI probes' irradiance (enh/gi)
//   Gbuffer         + the thin G-buffer outputs (location 1 normal + id, 2 motion) and the reprojected AO (enh/gbuffer)
//   GbufferMotion   + per-object motion from the vertex capture's previous world position (location 15; only with the
//                   capture vertex shader variant: it writes that varying)

#include <cstdint>

#include "d3d9/device.h"
#include "shader/glsl.h"

namespace rvk::enh {

enum class MaterialVariant : std::uint8_t { Gi, Gbuffer, GbufferMotion };

// the variant's options for a pixel shader; false = not applicable (Gi: no ambient constants; all: no colour output 0)
bool materialVariantOptions(const d3d9::Shader& s, MaterialVariant kind, shader::GlslOptions& o);

// the varying location of the capture vertex shader's previous world position (enh/rt_scene)
inline constexpr std::uint32_t kPrevWorldLocation = 15;  // TEXCOORD15: no game shader writes it (location 30 exceeded maxVertexOutputComponents 128)

}  // namespace rvk::enh
