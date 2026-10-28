#pragma once

// N5.5c "effect draws" (docs/research/engine-frame/README.md "13. N5.5c result"): the pure rules by which the engine's image
// filters (eCImageFilterEffect::ExecuteEffect 0x75af70: the post_SSM shadow mask chain, the terrain map's post_blur Blur7H / V)
// turn their parameter values into a draw, re-written from the decompiled functions and our fx_2_0 runtime (src/fx):
//  * every parameter object (eSImageEffectParam, filter +0xe0) is pushed by OnSetParams 0x75c740 as raw bytes through
//    SetParameter(handle, +0x58, N) (a column-major matrix transposed first: eTEffectParamTransposedMatrix), every TEXTUREn input
//    through AttachImmImageData (the input's output image +0x60 = its API texture);
//  * the technique's pass is applied as the HAL's effect does it (hal/vulkan/effect.cpp HalEffect::applyPass: the pass's states in
//    order, then the shaders' preshaders + CTAB constants and their samplers' sampler_state assignments);
//  * then eCGfxVisual::Execute 0x5069a0: SetRenderMode / SetCullingMode / SetDepthTesting / SetDepthWrite from the visual (+0x18 ..
//    +0x24; no-ops while the HAL's master flag of that state is set) and the primitive.
// The composed draw is described completely (shaders, declaration, streams, ranges, all 210 render states, the sampled stages'
// textures and all 13 sampler states, the float / bool / int registers, the render target, depth surface and viewport) and
// compared with the engine's draw field class by field class. No engine access, no Vulkan (unit tests: tests/native_tests.cpp).

#include <array>
#include <bitset>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include "fx/effect_binary.h"

namespace rvk::native::fxc {

inline constexpr std::uint32_t kRenderStates = 210;  // D3DRS_* < 210
inline constexpr std::uint32_t kSamplerStates = 14;  // D3DSAMP_* 1..13 at their own index
inline constexpr std::uint32_t kStages = 16;
inline constexpr std::uint32_t kVsRegs = 256, kPsRegs = 224;

struct Stream {
    const void* buffer = nullptr;
    std::uint32_t offset = 0, stride = 0, freq = 1;
    bool operator==(const Stream&) const = default;
};

struct Viewport {
    std::uint32_t x = 0, y = 0, width = 0, height = 0;
    float minZ = 0.0f, maxZ = 1.0f;
    bool operator==(const Viewport&) const = default;
};

// one effect draw as the device sees it at its draw call
struct EffectDraw {
    const void* vs = nullptr;  // d3d9::Shader*
    const void* ps = nullptr;
    const void* decl = nullptr;
    std::array<Stream, 4> streams{};
    const void* indices = nullptr;
    bool indexed = true;
    std::uint32_t primType = 5;
    std::int32_t baseVertex = 0;
    std::uint32_t minIndex = 0, numVertices = 0, startIndex = 0, primCount = 0, startVertex = 0;
    std::array<std::uint32_t, kRenderStates> rs{};
    std::array<const void*, kStages> textures{};  // d3d9::Texture*
    std::array<std::array<std::uint32_t, kSamplerStates>, kStages> ss{};
    float vsF[kVsRegs][4]{};
    float psF[kPsRegs][4]{};
    std::array<std::uint32_t, 16> vsB{}, psB{};
    std::int32_t vsI[16][4]{};
    std::int32_t psI[16][4]{};
    const void* rt0 = nullptr;    // d3d9::Surface*
    const void* depth = nullptr;  // d3d9::Surface*
    Viewport viewport;
    // diagnostics: what the composition wrote (the pass, the visual), the rest is the device's state at the unit's entry
    std::bitset<kRenderStates> rsWritten;
    std::uint32_t vsRegsWritten = 0, psRegsWritten = 0;
    bool unsupported = false;  // a pass state the composition does not model (vertex textures, fixed-function stages)
};

// HAL master flags that turn eCGfxVisual::Execute's setters into no-ops (hal/vulkan/device.cpp SetRenderMode ...)
struct Masters {
    bool fill = false, cull = false, depthFunc = false, depthWrite = false;
};

// HalEffect::applyPass for `pass` with the parameter `values`; `paramTextures[i]` = the texture top-level parameter i names (null:
// none), `shaders[k]` = our device shader of the effect's shader k
void applyPass(const fx::Effect& effect, const fx::Pass& pass, const fx::ParameterValues& values, std::span<const void* const> paramTextures,
               std::span<const void* const> shaders, EffectDraw& d);
// eCGfxVisual::Execute's four states (visual +0x18 fill, +0x1c cull, +0x20 depth func, +0x24 depth write 2 = on)
void applyVisual(EffectDraw& d, std::uint32_t fill, std::uint32_t cull, std::uint32_t depthFunc, std::uint32_t depthWrite, const Masters& m);

// eTEffectParamTransposedMatrix<bCMatrix>::Apply 0x28a700: the transpose of the stored matrix
void transpose(const float in[16], float out[16]);

// ---- parameter values the shadow mask's OnUpdateOutput (0x668560) writes ---------------------------------------------------

// the engine's float constants it uses (Engine.dll data, read once): DAT_1809f1718 (dither kernel, 2.5), DAT_1809f184c (-0.5),
// DAT_1809f1630 (0.5), DAT_1809f15d4 (the fade fraction, 0.2), DAT_1809f16f8 (2: the PSSM kernel)
struct MaskConstants {
    float kernel = 2.5f, negHalf = -0.5f, half = 0.5f, fadeFraction = 0.2f, packedKernel = 2.0f;
};
// DizzerKernelSize = kernel / size; PCFFilterOffset = (negHalf, negHalf, half, half) / size; PCFFilterScale = (size, size)
void cascadeFilter(const MaskConstants& k, std::int32_t mapSize, float& kernel, float offset[4], float scale[2]);
// FadOut: f = max x fadeFraction: (camFar / f, -((max - f) / f))
void fadeOut(const MaskConstants& k, float maxDistance, float cameraFar, float out[2]);
// ZDistance of a cascade: (f P33 + P43) / f with f = the next split's start (+0x160), the last: the max distance
float zDistance(float f, float p33, float p43);

// ---- the shadow views' setup (eCShadowMapBase::RenderShadowMap 0x5a1070, eCDepthMap, UpdateCamera 0x5a13e0) ---------------------

// eEDepthMapFormat 3..6 (D16, D24S8, DF16, DF24): a depth map (bound as the depth surface with the working colour buffer)
inline bool isDepthFormat(std::int32_t format) { return format >= 3 && format <= 6; }
// eCDepthMap::ClearViewPort 0x59fe50: depth formats clear z + stencil (6), the others colour too (7)
inline std::uint32_t depthMapClearFlags(std::int32_t format) { return isDepthFormat(format) ? 6u : 7u; }
// eCShadowMapDirectionalLight2::UpdateCamera: the orthographic projection's scale (2 / dx, 2 / dy, 1 / dz) and its translation
// before the texel snap ((min + max) x -0.5 x scale for x / y, -(scale z x min z)); box = min xyz, max xyz (+0xe0 .. +0xf4)
void shadowProjection(const float box[6], float scale[3], float translation[3]);
// ... the texture matrix's bias (identity, [0] 0.5, [5] -0.5, [12] = [13] = 0.5 + 0.5 / size, [14] = -bias), multiplied by P and V
void shadowTextureBias(std::int32_t size, float depthBias, float out[16]);

// ---- N5.7 post chain: the semantic parameter classes' value rules (the classes' Set, vtable +0x28) ----------------------------

// eSImageEffectParamColorMatrix 0x758980 (COLORMATRIX, float3[3]): row i = E t_i (s e_i + (1 - s) w) with E = exposure (weather
// +0x158), s = saturation (+0x170), t = tint (+0x180..+0x188), w = (wR, wG, wB) the luminance weights (Engine.dll data 0.3,
// 0.59, 0.11); the engine's operation order
void colorMatrix(float exposure, float saturation, const float tint[3], float one, const float weights[3], float out[9]);
// eSImageEffectParamCamera 0x289b10 kind 10 (FOCALRANGENEAR): n = focal near (camera +0xe4); n <= epsilon: (0, 0); else n /= far,
// (minusOne / n, -((minusOne / n) n))
void focalRangeNear(float nearRange, float cameraFar, float epsilon, float minusOne, float out[2]);
// kind 11 (FOCALRANGEFAR): s = far start (+0xe8), e = far end (+0xec, the far plane when e <= s), r = (e - s) / far; r >= 1: (0, 0);
// else (1 / r, -((s / far) (1 / r)))
void focalRangeFar(float farStart, float farEnd, float cameraFar, float one, float out[2]);
// eSImageEffectParamSkyDome 0x289fc0 kind 4 (SUNLIGHTSCREENVECTOR): the clamp of x / y of the projected sun direction to [-1, 1]
// (the engine's compare order: a NaN becomes 1)
void clampScreenVector(float v[3], float one, float minusOne);
// kind 5 (SUNFLAREINTENSITY): max(-z_view, 0)^2
float sunFlare(float viewZ);

// ---- N5.7 sprite batcher (eCSprite::Flush 0x757d10 -> RenderBatches 0x757180 -> DrawBatch 0x7570b0 -> eCSpriteMesh::Draw) ------

inline constexpr std::uint32_t kSpriteRecord = 0x48;   // eCSprite +0xa8 records: corners 4 x (x, y), uv (u0, v0, u1, v1) +0x20, z +0x30, image +0x38, colour +0x40, blend +0x44
inline constexpr std::uint32_t kSpriteVertex = 0x1c;   // FLOAT3 position (+ 4 unread bytes: the engine's w lane), D3DCOLOR +0x10, FLOAT2 uv +0x14
inline constexpr std::uint32_t kSpriteQuad = 4 * kSpriteVertex;

struct SpriteKey {
    const void* image = nullptr;
    std::uint32_t blend = 0;
};
// one DrawBatch call: the state it changes (SetBlend, the texture + eCGfxEffect::Execute) and the quads it draws (count 0: none)
struct SpriteBatch {
    bool setBlend = false;
    std::uint32_t blend = 0;
    bool setImage = false;
    const void* image = nullptr;
    std::uint32_t start = 0, count = 0;   // mesh quads [start, start + count)
    std::uint32_t firstRecord = 0;        // the record of the batch's first quad
    enum class Why : std::uint8_t { Blend, Texture, Full, End } why = Why::End;
};
struct SpritePlan {
    std::vector<SpriteBatch> batches;
    std::vector<std::uint32_t> quadOffset;  // per record: its quad's byte offset in the vertex buffer
    std::vector<bool> discard;              // per record: locked with DISCARD (offset 0)
    std::uint32_t endStart = 0, endCount = 0;
};
// RenderBatches: +0x88 = 0, +0x90 = record 0's image, +0x98 = 0, +0x9c = record 0's blend; per record a DrawBatch before it on a
// blend change, a texture change or a full buffer (start + count == capacity: then start = count = 0); a final DrawBatch.
// DrawBatch: SetBlend when +0x98 != +0x9c, the texture when +0x88 != +0x90, the mesh's draw when count != 0 (start += count).
SpritePlan planSprites(std::span<const SpriteKey> records, std::uint32_t capacity, std::uint32_t start, std::uint32_t count);
// one record's quad (RenderBatches' writes; the w lanes get `w`): (p0, u0 v0), (p1, u1 v0), (p3, u1 v1), (p2, u0 v1)
void spriteQuad(const std::uint8_t* record, float w, std::uint8_t out[kSpriteQuad]);
// eCSpriteMesh::CreateIndexBuffer 0x528a80: quad q -> 4q + (0, 2, 3, 0, 1, 2)
void spriteQuadIndices(std::uint32_t q, std::uint16_t out[6]);
// eCSprite::SetBlend 0x7568f0 (flags & 0x7000): alpha blending on / off + SetMasterBlending(src, dst); false: no change (unknown)
bool spriteBlend(std::uint32_t flags, std::uint32_t& enable, std::uint32_t& src, std::uint32_t& dst);
// the vertex bytes compared (not the w lane +0xc..+0xf of a vertex: the engine leaves stack data there, the FLOAT3 decl never reads it)
bool spriteBytesEqual(std::span<const std::uint8_t> a, std::span<const std::uint8_t> b, std::size_t& firstDiff, std::uint64_t& wLanesDiffer);

// ---- comparison -------------------------------------------------------------------------------------------------------------

enum class Field : std::uint8_t {
    DrawCount, Vs, Ps, Decl, Streams, Indices, Range, Depth, Stencil, Blend, ColorWrite, Raster, OtherState, Texture, SamplerState,
    VsConst, PsConst, BoolInt, Target, Viewport, Geometry, Count
};
inline constexpr std::size_t kFieldCount = static_cast<std::size_t>(Field::Count);
const char* name(Field f);
// the field class of a render state
Field stateField(std::uint32_t rs);

// what of a draw is live: the captured shaders' CTAB float registers and the pixel shader's sampled stages
struct Live {
    std::bitset<kVsRegs> vs;
    std::bitset<kPsRegs> ps;
    std::uint16_t stages = 0;
};

struct Diff {
    std::bitset<kFieldCount> fields;
    std::uint32_t regs = 0, stages = 0, states = 0;  // compared (the comparison is not vacuous)
    std::string first;
};
Diff compare(const EffectDraw& composed, const EffectDraw& captured, const Live& live);

struct Counts {
    std::uint64_t units = 0, unitsComposed = 0, draws = 0, cleanDraws = 0;
    std::array<std::uint64_t, kFieldCount> mismatches{};
    std::uint64_t regs = 0, stages = 0, states = 0;
    void add(const Counts& o);
    void add(const Diff& d);
};
std::string summary(const Counts& c);

}  // namespace rvk::native::fxc
