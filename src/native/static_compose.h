#pragma once

// N5.1 "static ops" (docs/research/engine-frame/README.md 4 and "7. N5.1 result"): the pure rules by which the engine turns a
// static mesh op of the main view's z pre-pass / solid list into draws, re-written from the decompiled functions
// (C-materials.md 1-4; RVAs Engine.dll), and the description of one composed draw with its comparison against the draw the
// engine actually made. No engine access, no Vulkan (unit tests: tests/native_tests.cpp); the engine side that feeds these
// rules with engine data is native/static_ops.cpp.

#include <array>
#include <bitset>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace rvk::native::sc {

// ---- shader passes ------------------------------------------------------------------------------------------------------

// eSShaderMaterialPass +8 (C-materials.md 5.4): bit 0 sun, bits 2-3 point lights, bits 4-5 lightmap combi, bits 6-8 kind
// (0 base, 1 additive light pass, 2 z-fill), bit 9 not instanced, bits 10-11 SpeedTree billboard VS variant
struct Pass {
    std::uint32_t flags = 0;
    const void* illum = nullptr;  // eSShaderMaterialIllumination* (null: no lighting written)
    std::uint8_t firstLight = 0;
    std::int32_t switchValue = 0;
};
inline std::uint32_t passKind(std::uint32_t flags) { return flags >> 6 & 7u; }
inline std::uint32_t passLights(std::uint32_t flags) { return flags >> 2 & 3u; }
inline std::uint32_t passCombi(std::uint32_t flags) { return flags >> 4 & 3u; }
inline bool passSun(std::uint32_t flags) { return (flags & 1u) != 0; }
inline bool passNonInstanced(std::uint32_t flags) { return (flags >> 9 & 1u) != 0; }
// eCShaderVersionDefault_3_0's Execute (0x6cc610): VS at version +0x1a8 + 16 vsIndex, PS at +0x208 + 16 psIndex
inline std::uint32_t vsIndex(std::uint32_t flags) { return (flags >> 9 & 1u) * 3u + (flags >> 10 & 3u); }
inline std::uint32_t psIndex(std::uint32_t flags) {
    return (flags >> 2 & 3u) + (((flags >> 4 & 3u) + (flags & 3u) * 2u) * 3u + (flags >> 6 & 7u)) * 4u;
}

// the inputs of eCShaderVersionDefault_3_0::GetPasses (0x6cc970) for one call
struct PassInputs {
    bool zFill = false;           // shader +0x2d8 EnableAlphaZWrite and IsBlended: a z-fill pass (kind 2) first
    const void* illum = nullptr;  // the illumination (null, or its light list +0 null: one unlit-list pass)
    bool hasList = false;         // illum +0 != null
    std::uint8_t listSun = 0;     // light list +0
    std::uint8_t illumNoSun = 0;  // illum +0xc
    std::uint8_t staticLights = 0;   // illum +0xd
    std::uint8_t dynamicLights = 0;  // light list +0x34
    std::uint32_t combi = 0;      // eMeshResource2::BindLightmaps' result (0 / 1)
    bool instanced = false;
    std::int32_t switchValue = 0;
};
// appends the passes (in the engine's order); returns how many were added
std::size_t buildPasses(const PassInputs& in, std::vector<Pass>& out);

// ---- render states ------------------------------------------------------------------------------------------------------

// eSShaderRenderStates (0x24; C-materials.md 2.2)
struct RenderStates {
    std::uint32_t alphaRef = 0, alphaFunc = 8, alphaTest = 1;  // test: 1 off, 2 on (only feeds PS c5.w)
    bool blend = false, colorWrite = false, alphaWrite = false;
    std::uint32_t src = 2, dst = 1;
    std::uint32_t depthWriteMaster = 0;  // 0 none (the visual's), 1 off, 2 on
    std::uint32_t renderMode = 0;        // 0 = the admin's (+0x340)
    std::uint32_t depthFunc = 4;
    bool valid = true;                   // false: a kind the engine leaves uninitialised (not composable)
};
// eCShaderBase::FillBlendingRenderStates 0x6d0430 (fields the mode does not set keep their value)
struct BlendInputs {
    std::int32_t mode = 0;       // shader +0x50 eEShaderMaterialBlendMode
    std::uint8_t maskRef = 0;    // +0x58
    bool hasOpacity = false;     // eCShaderDefault::HasOpacity (+0x330)
    bool refracted = false;      // eCShaderDefault::IsRefracted (+0x420)
    bool tradeAlphaAsSolid = false;  // +0x79
};
void fillBlending(RenderStates& rs, const BlendInputs& b, bool additive, float fade);
// eCShaderBase::IsBlended 0x6d2ef0
bool isBlended(const BlendInputs& b);
// the states FUN_1806cc610 hands to SetCurrentRenderStates for a pass; fade = the global alpha of instance 0 for
// non-instanced default-VS passes (bit 9 set, bits 10-11 clear), else 1
RenderStates passStates(std::uint32_t flags, const BlendInputs& b, float fade);
// BeginZPass 0x6b9720 (no per-draw states in the z pass)
RenderStates zPassStates(bool zWrite, bool colorWrite);

// the D3D9 render states of a draw after SetCurrentRenderStates (+ the HAL's master semantics) and eCGfxVisual::Execute
enum State : std::uint8_t { StZFunc, StZWrite, StBlend, StSrc, StDst, StColorWrite, StAlphaTest, StCull, StFill, StFog, StOwnedCount };
struct Visual {
    std::uint32_t fill = 3, cull = 2, depthFunc = 4, depthWrite = 2;  // eCGfxVisual +0x18 / +0x1c / +0x20 / +0x24
};
std::array<std::uint32_t, StOwnedCount> deviceStates(const RenderStates& rs, const Visual& v, std::uint32_t adminRenderMode);
const char* stateName(std::uint32_t s);

// ---- geometry -----------------------------------------------------------------------------------------------------------

// a sub-mesh as eCMeshResource2::RenderZPass's whole-mesh loop (0x7603b0, desc 0xff) sees it
struct ZSub {
    bool ready = false;  // material set, cached (state 3) and with a shader
    bool blended = false, masked = false;
    std::uint32_t firstVertex = 0, vertexCount = 0, startIndex = 0, indexCount = 0;  // eCSubMesh +0x30 / +0x34 / +0x28 / +0x2c
};
struct ZDraw {
    std::uint32_t minIndex = 0, numVertices = 0, startIndex = 0, primCount = 0;
    std::uint32_t shaderOf = 0;  // index of the sub-mesh whose shader's special pass draws it
};
// consecutive non-masked sub-meshes merge into one draw (drawn with the shader of the last one added), a masked one draws
// alone, blended / refracted ones are skipped and end a run
void planWholeMeshZ(std::span<const ZSub> subs, std::vector<ZDraw>& out);

// a render instance as eCMeshResource2::ExecuteRenderInstances (0x7607b0) batches it (instances sorted first)
struct Instance {
    const void* list = nullptr;     // its illumination's light list (illum +0)
    std::int32_t switchValue = 0;   // +0x40
    std::uint8_t flags = 0;         // +0x50 overlay flags
    float f = 1.0f;                 // illum +8
    bool alone = false;             // illum +0xd static lights or +0x90 static SH: ends its batch
};
struct Batch {
    std::uint32_t first = 0, count = 0;  // instances [first, first + count); the last one's illumination / switch / flags draw it
};
void planBatches(std::span<const Instance> instances, std::uint32_t maxPerBatch, std::vector<Batch>& out);

// eCColorSrcConstantSwitch::SetSwitch 0x6c0c60: the colour index of switch value s (count +0x58, repeat +0x68)
std::int32_t constantSwitchIndex(std::int32_t s, std::int32_t count, std::int32_t repeat);
// 0x6c38f0 (eCColorSrcSampler::SetSwitch): the image index (begin +0x88, repeat +0x80, count +0xe8); -1 = none (not a switch
// sampler or no image)
std::int32_t samplerSwitchIndex(std::int32_t s, std::int32_t begin, std::int32_t count, std::int32_t repeat, std::int32_t type);

// ---- constants ----------------------------------------------------------------------------------------------------------

// eCShaderMaterialAdmin::SetFog 0x6b9fc0: PS c3 = colour, PS c4 = VS c30 = (-start k, k, density, 0), k = 1 / ((end - start) / density)
void fogConstants(bool enabled, std::uint32_t colorArgb, float start, float end, float density, float c3[4], float c4[4]);
// SetCamera 0x6ba1d0: PS c1 = (0.5, -0.5, 0.5 / w + 0.5, 0.5 / h + 0.5), w / h = the viewport's right - left / bottom - top
void viewToScreen(std::int32_t width, std::int32_t height, float c1[4]);
// SetCamObstacleDistances 0x6b7f90: PS c5.x = 1 / 150, .y = (d - 50) * (-1 / 150)
void camObstacle(float d, float& x, float& y);
// FUN_1806b5bb0 0x6b5bb0: c15..c21 = hemisphere SH (list +0xb8) x f, .w of c15..c17 += (1 - f) indoor, + static SH
void ambientSh(const float list[7][4], const float indoor[3], float f, const float* staticSh /* 7x4 or null */, float out[7][4]);

// ---- the composed draw and its comparison ---------------------------------------------------------------------------------

inline constexpr std::uint32_t kVsRegs = 256, kPsRegs = 224, kStages = 16;

// where a constant register's value came from (diagnostics)
enum class Src : std::uint8_t { None, Native, Pool, Inherit };

struct DrawDesc {
    const void* vs = nullptr;  // d3d9::Shader*
    const void* ps = nullptr;
    const void* decl = nullptr;  // d3d9::VertexDecl*
    struct Stream {
        const void* buffer = nullptr;  // d3d9::Buffer* (null: not bound)
        std::uint32_t offset = 0, stride = 0, freq = 1;
        bool operator==(const Stream&) const = default;
    };
    std::array<Stream, 4> streams{};
    const void* indices = nullptr;
    std::uint32_t primType = 4;
    std::int32_t baseVertex = 0;
    std::uint32_t minIndex = 0, numVertices = 0, startIndex = 0, primCount = 0;
    std::array<std::uint32_t, StOwnedCount> states{};
    struct Sampler {
        const void* texture = nullptr;  // d3d9::Texture*
        std::array<std::uint32_t, 7> ss{};  // ADDRESSU, ADDRESSV, MAGFILTER, MINFILTER, MIPFILTER, MIPMAPLODBIAS, SRGBTEXTURE
        bool operator==(const Sampler&) const = default;
    };
    std::array<Sampler, kStages> samplers{};
    std::uint16_t samplerMask = 0;  // stages the pixel shader samples
    std::uint16_t inheritMask = 0;  // of those, stages left as the device had them (not composed)
    float vsF[kVsRegs][4]{};  // float constants
    float psF[kPsRegs][4]{};
    std::bitset<kVsRegs> vsLive;
    std::bitset<kPsRegs> psLive;
    std::array<Src, kVsRegs> vsSrc{};
    std::array<Src, kPsRegs> psSrc{};
    // the op it belongs to
    const void* subMesh = nullptr;
    const void* material = nullptr;
    std::uint32_t passFlags = 0;  // the shader pass (0 for z draws)
    bool zPass = false;
    // the draw tag's sub-mesh fields as the engine's hooks set them (only eCSubMesh::Render sets them: solid non-instanced
    // draws; z draws and instanced batches keep the slot's null)
    const void* tagSubMesh = nullptr;
    const void* tagMaterial = nullptr;
    const void* tagMeshResource = nullptr;
};

// what the comparison counts (one class per field group; constants by register group)
enum class Field : std::uint8_t {
    DrawCount, Tag, Vs, Ps, Decl, Streams, Indices, Range, ZFunc, ZWrite, Blend, ColorWrite, AlphaTest, Cull, Fill, Fog, Texture, SamplerState,
    VsCamera, VsInvProj, VsInvView, VsCamPos, VsAdminOther, VsFog, VsInstance, VsGlobalOther, VsMaterial,
    PsCamPos, PsScreen, PsReserved, PsFogColor, PsFog, PsObstacle, PsMaskRef, PsMaterial, PsIndoor, PsSh, PsSun, PsLights,
    UnownedConstant, Count
};
inline constexpr std::size_t kFieldCount = static_cast<std::size_t>(Field::Count);
const char* name(Field f);
Field vsField(std::uint32_t reg, std::uint32_t component);
Field psField(std::uint32_t reg, std::uint32_t component);

struct Diff {
    std::bitset<kFieldCount> fields;  // mismatching field classes
    std::uint32_t closeFloats = 0;    // floats equal within 1e-5 relative (not bit-equal)
    std::uint32_t regs = 0, stages = 0;  // constant registers / sampler stages compared (the comparison is not vacuous)
    std::string first;                // the first difference, as text
};
// composed vs captured (the engine's draw): only registers live in the captured draw's shaders and only the captured sampled
// stages count
Diff compare(const DrawDesc& composed, const DrawDesc& captured);

// counters over many draws
struct CompareCounts {
    std::uint64_t ops = 0, draws = 0, cleanDraws = 0;
    std::uint64_t opsComposed = 0, opsNotComposed = 0;
    std::array<std::uint64_t, kFieldCount> mismatches{};
    std::uint64_t closeFloats = 0;
    std::uint64_t regs = 0, stages = 0;
    void add(const CompareCounts& o);
};
std::string summary(const CompareCounts& c);

}  // namespace rvk::native::sc
