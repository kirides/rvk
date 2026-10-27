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

// ---- sky dome (eCSkydome_PS::Render 0x6e4730) -------------------------------------------------------------------------------

// eCSkyObject::eSSkyObjectBillboard (0x30 bytes): position f3 @0, half size s f2 @0xc, second axis r f2 @0x14, colour @0x1c,
// uv rect (u0, v0, u1, v1) @0x20
inline constexpr std::uint32_t kSkyBillboardSize = 0x30, kSkyVertexStride = 0x20;
// eCSkyObject::FillVertexBuffer 0x6e1a50: 4 vertices per billboard (stride 0x20): position @0, colour @0xc, uv @0x10, the
// corner offset @0x18: (-s + r, u0 v0), (s + r, u1 v0), (s - r, u1 v1), (-s - r, u0 v1)
void skyBillboardVertices(std::span<const std::uint8_t> billboards, std::uint32_t count, std::vector<std::uint8_t>& out);
// eCSkyObject::FillIndexBuffers 0x6e1940: billboard k -> 4k + (0, 1, 2, 0, 2, 3), 16-bit
void skyQuadIndices(std::uint32_t count, std::vector<std::uint8_t>& out);
// eCShaderVersionSkyDome_2_0's states (0x6d9740): FillBlendingRenderStates (not additive, fade = GetGlobalAlpha(0) = 1), colour
// write on, alpha write off, depth func LESSEQUAL, depth write off for IsBlended && !IsRefracted (else the visual's), render mode 0
RenderStates skyDomeStates(const BlendInputs& b, float fade);

// ---- the opaque billboards' queue (eCBillboardAdmin::RenderBillboards 0x4f0530) -------------------------------------------------

// a queue entry after the engine's sort (SortRenderQueue 0x4f0ff0): {eCBillboard*, eCLightSet*, blend mode, switch}; the record
// keys are the billboard's material (+0) and image (+8), the light set, the blend mode and the switch value
struct BillboardEntry {
    const void* material = nullptr;
    const void* image = nullptr;
    const void* lightSet = nullptr;
    std::int32_t blend = 0;
    std::int32_t switchValue = 0;
    std::uint32_t quads = 0;  // eCBillboard +0x20
};
struct BillboardRecord {
    std::uint32_t cycle = 0;                       // lock cycle (VB / IB locked DISCARD at offset 0)
    std::uint32_t firstQuad = 0, quads = 0;        // in the cycle's buffers
    std::uint32_t firstEntry = 0, entryCount = 0;  // the entries it holds (consecutive, zero-quad entries included)
    BillboardEntry keys;
};
// per entry: the cycle and quad position its vertices are copied to (-1 cycle: not drawn, 0 quads)
struct BillboardPlace {
    std::int32_t cycle = -1;
    std::uint32_t quad = 0;
};
// a cycle holds at most maxQuads (1000) quads: the records are drawn (and the buffers unlocked) when the next entry would
// overflow it, and at the end; a new record starts when a key differs from the previous record's
void planBillboards(std::span<const BillboardEntry> entries, std::uint32_t maxQuads, std::vector<BillboardRecord>& records,
                    std::vector<BillboardPlace>& places);
// the indices of quad q (cycle-relative): 4q + (0, 1, 3, 1, 2, 3)
void billboardQuadIndices(std::uint32_t firstQuad, std::uint32_t quads, std::vector<std::uint8_t>& out);

// ---- skinned actors (eCAnimation_HardRenderActor2, README.md "10. N5.3 result") ----------------------------------------------

// eCAnimation_HardRenderActor2::SetupMorphTargets 0x4dd220 (disassembled): three slots (target, weight) start at (0, 0); for each
// morph target k of the actor instance's setup (weight at +4 of 0xc-byte entries) the slot with the smallest weight (slot 1 when
// w0 > w1, else 0; that one when its weight <= w2, else 2) takes (k, w) when w is strictly greater than its weight
struct MorphPick {
    std::array<std::uint32_t, 3> target{};
    std::array<float, 3> weight{};
};
MorphPick pickMorphTargets(std::span<const float> weights);
// ... and each slot i sets the stream offset of stream i + 1: target x the LOD's vertex count (HRA +0x48 + 0x14 lod) x 0x10
inline std::uint32_t morphStreamOffset(std::uint32_t target, std::uint32_t lodVertexCount) { return target * lodVertexCount << 4; }
// eCAnimation_HardRenderActor2::Render 0x4dd650: the actor LOD (ActorInstance +0x250, unsigned) clamped to 4; the primitive
// index u16(first of the LOD (HRA +0x38 + 0x14 lod) + the op's primitive)
inline std::uint32_t actorLod(std::uint32_t lod) { return lod < 4 ? lod & 0xffffu : 4u; }
inline std::uint32_t actorPrimitive(std::uint16_t first, std::uint16_t primIdx) { return static_cast<std::uint16_t>(first + primIdx); }
// the bones a primitive sets (eSHardPrimitive +0x18 start, +0x1a count): `start < u16(start + count)` or none
inline std::uint32_t primitiveBones(std::uint16_t start, std::uint16_t count) {
    return start < static_cast<std::uint16_t>(start + count) ? count : 0u;
}
// eCShaderDefault::SetBoneMatrix 0x6c9d80: bone b -> the global pool's c(3 (b + 11)) .. +2 (3x4 transpose)
inline std::uint32_t boneRegister(std::uint32_t b) { return (b + 11) * 3; }

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

// what a draw is, for the per-class counts: a z pre-pass draw (special pass), a base pass (kind 0), an additive light pass
// (kind 1, lights 4..), a z-fill pass (kind 2, EnableAlphaZWrite), an entity overlay pass (burned / frozen / highlighted:
// another material's passes over the same geometry), a non-op path (decals, billboards) or the sky dome's parts
enum class DrawClass : std::uint8_t { ZPass, Base, Additive, ZFill, Overlay, NonOp, Sky, Count };
inline constexpr std::size_t kDrawClassCount = static_cast<std::size_t>(DrawClass::Count);
const char* name(DrawClass c);
// the class of a material pass (eSShaderMaterialPass flags) drawn as an overlay or not
DrawClass passClass(std::uint32_t flags, bool overlay);

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
    // bool / int registers: the material path never writes them (HAL slots 104 / 105 / 114 / 115), so the composition is the
    // device's register file at the op's entry; every register is compared (the CTAB-live ones are counted)
    std::array<std::uint32_t, 16> vsB{}, psB{};
    std::int32_t vsI[16][4]{};
    std::int32_t psI[16][4]{};
    std::uint32_t biLive = 0;  // captured draws: bool / int registers in the shaders' CTABs
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
    // ... and its water classification (the Refracted / Alpha passes: the enhanced water's DrawReplacer acts on it)
    std::uint8_t tagWater = 0;            // draws::Water
    const char* tagMaterialName = nullptr;
    DrawClass drawClass = DrawClass::Base;
    // CPU-generated geometry (non-op paths: decals): the vertex bytes of [minIndex, minIndex + numVertices) and the index bytes
    // of [startIndex, startIndex + 3 primCount) as the engine writes them into its dynamic buffers; empty: static geometry
    std::uint32_t vertexStride = 0;
    std::vector<std::uint8_t> vertexBytes, indexBytes;
};

// what the comparison counts (one class per field group; constants by register group)
enum class Field : std::uint8_t {
    DrawCount, Tag, Vs, Ps, Decl, Streams, Indices, Range, GeometryData, ZFunc, ZWrite, Blend, ColorWrite, AlphaTest, Cull, Fill, Fog, Texture, SamplerState,
    VsBool, VsInt, PsBool, PsInt,
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
    std::uint32_t biRegs = 0;            // bool / int registers compared (whole register files)
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
    std::uint64_t biRegs = 0, biLive = 0;  // bool / int registers compared / live in the captured shaders' CTABs
    // per draw class (DrawClass) and per op list (native::List index): draws compared, equal
    std::array<std::uint64_t, kDrawClassCount> classDraws{}, classClean{};
    std::array<std::uint64_t, 8> listDraws{}, listClean{};
    void add(const CompareCounts& o);
};
// listNames: the names of the list indices (null: no per-list part)
std::string summary(const CompareCounts& c, const char* const* listNames = nullptr, std::size_t listCount = 0);

}  // namespace rvk::native::sc
