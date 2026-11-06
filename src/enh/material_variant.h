#pragma once

// Variants of the game's material pixel shaders for enhancements (docs/research/rtgi.md 9-10): the GLSL of
// shaders/include/rvk_material_variant.glsl (after rvk_gi.glsl) pasted into the translated shader with a few defines; the
// translated program itself is unchanged apart from its ambient constants (overrides) and one operand capture.
//
//   Gi              every read of g_AmbientLight / g_IndoorAmbientColor = the GI probes' irradiance (enh/gi)
//   Gbuffer         + the reprojected AO / screen probes and the thin G-buffer outputs the session reads (GbufferOutputs; enh/gbuffer)
//   GbufferMotion   + per-object motion from the vertex capture's previous world position (location 15; only with the
//                   capture vertex shader variant: it writes that varying)

#include <cstdint>
#include <string>
#include <string_view>

#include "d3d9/device.h"
#include "shader/translate.h"

namespace rvk::enh {

enum class MaterialVariant : std::uint8_t { Gi, Gbuffer, GbufferMotion };

// The G-buffer targets a Gbuffer / GbufferMotion variant writes: only what a consumer of the session reads (no unread outputs):
// None = AO / screen probes only (no attachments), Motion = FSR / frame generation (SV_Target1 motion), NormalMotion = traced material
// reflections / debug views (SV_Target1 normal, SV_Target2 motion). Bit 1 = motion, bit 2 = normal.
enum class GbufferOutputs : std::uint8_t { None = 0, Motion = 1, NormalMotion = 3 };
inline bool writesNormal(GbufferOutputs o) { return (static_cast<unsigned>(o) & 2u) != 0; }
inline bool writesMotion(GbufferOutputs o) { return (static_cast<unsigned>(o) & 1u) != 0; }

// the variant's options for a pixel shader; false = not applicable (Gi: no ambient constants; all: no colour output 0; GbufferMotion
// without motion output)
bool materialVariantOptions(const d3d9::Shader& s, MaterialVariant kind, shader::TranslateOptions& o,
                            GbufferOutputs outputs = GbufferOutputs::NormalMotion);

// the SPIR-V cache tag of a variant: `base` + a hash of the embedded GLSL it pastes (an edit of rvk_gi.glsl /
// rvk_material_variant.glsl never reuses a stale cached variant)
std::string materialVariantTag(std::string_view base);

// Material pack (enh/material_pack, docs/research/material-pack.md): with this on at startup (setting materials.pack, a restart setting) every
// variant also contains the physically based specular / reflection code of rvk_material_variant.hlsli (RVK_MV_PBR); it acts on draws whose
// DrawBlock carries a pack word (alpha.w != 0), others are untouched. Decided once: variants are cached per shader.
void setPbrVariants(bool on);
bool pbrVariants();

// Full-owned light table (enh/light_table, gpu-scene.md 9.7): with this on at startup (setting lighting.clustered, a restart setting) every variant of a
// shader that reads g_PointLights fills those registers per pixel from the table's cluster (RVK_MV_CLUSTER); the tag changes with it.
void setClusterVariants(bool on);
bool clusterVariants();

// the varying location of the capture vertex shader's previous world position (enh/rt_scene)
inline constexpr std::uint32_t kPrevWorldLocation = 15;  // TEXCOORD15: no game shader writes it (location 30 exceeded maxVertexOutputComponents 128)

}  // namespace rvk::enh
