#pragma once

// The contract between translated D3D9 shaders (shader/glsl.cpp), hand-written shaders (shaders/**) and the Vulkan renderer.
// Its GLSL side is shaders/include/rvk_abi.glsl (embedded into every translated shader, #included by hand-written ones);
// change both together. rvk_tests (abi_glsl_layout_matches_cpp) compares the offsets of every block below with that file's
// SPIR-V. Design: docs/research/injection.md.
//
// Push constants of draws (48 bytes of the 128 every device has, vertex + fragment):
//   uint64 vsConsts      device address of the vertex shader's StageConsts
//   uint64 psConsts      device address of the pixel shader's StageConsts
//   uint64 draw          device address of the DrawBlock
//   uint64 frame         device address of the FrameBlock (one per frame and submission; 0 never: always uploaded)
//   uint64 prevVsConsts  last frame's StageConsts of the same geometry (motion vectors, VS capture); 0 = not tracked (yet)
//   uint32 drawId        the draw's index in the frame (= its draws::Record index)
//   uint32 flags         bits 0-7 draws::Kind, 8-11 scene::Pass, 12-15 scene::View, 16.. feature bits (kDrawFlag*)
//
// StageConsts (std430): vec4 c[256]; ivec4 i[16]; uint b[16]          -> c at 0, i at 4096, b at 4352, 4416 bytes
// DrawBlock (std430):   uint tex[20]; uint smp[20]; vec4 posFix; uint alphaFunc; float alphaRef; uint pad[2]
//                       tex/smp: bindless texture / sampler indices of D3D9 samplers s0..s15 (pixel) and 16..19 (vertex s0..s3)
//                       posFix.xy: added to gl_Position.xy * w. D3D9 puts pixel centres on integer coordinates, Vulkan on
//                       +0.5; moving the geometry half a pixel right/down makes Vulkan sample where D3D9 did:
//                       (+1/width, -1/height) with the y-flipped viewport (rvk_vktest checks this)
//                       alphaFunc: D3DCMPFUNC (1 never .. 8 always), alphaRef: 0..1
//                       pad[0]: debug tint, RGBA8 (unpackUnorm4x8; 0 = none), read only by the tint variant of a pixel
//                       shader (GlslOptions::tintEpilogue, the "tint draws" debug view); pad[1] unused
// FrameBlock (std430, 928 bytes): the main camera (current + previous), depth conventions, target size, jitter, time, frame
//                       index + flags, bindless slots of the tagged engine images, 16 vec4 feature parameters (below), the
//                       ray tracing scene (TLAS + geometry record addresses, RtGeometryRecord below)
//
// Pass shaders (injected passes, d3d9/inject.h) use their own pipeline layout: set 0 = this bindless set, set 1 = a push
// descriptor set of the pass, 128 bytes of push constants starting with PassPushHeader (GLSL RVK_PASS_PUSH_HEADER).
//
// Descriptor set 0 (bindless, update-after-bind, partially bound; vertex, fragment and compute stages):
//   binding 0: texture2D t2D[]   binding 1: textureCube tCube[]   binding 2: texture3D t3D[]   binding 3: sampler samp[]
//   index 0 of every texture array is a 1x1 (0,0,0,1) texture (D3D9's value of an unbound sampler).
//
// Varyings: location = varyingLocation(usage, usageIndex); vertex inputs: location = v# register index.

#include <cstddef>
#include <cstdint>

#include "shader/sm_ir.h"

namespace rvk::shader::abi {

inline constexpr std::uint32_t kBindingTex2D = 0;
inline constexpr std::uint32_t kBindingTexCube = 1;
inline constexpr std::uint32_t kBindingTex3D = 2;
inline constexpr std::uint32_t kBindingSamplers = 3;

inline constexpr std::uint32_t kFloatConsts = 256;
inline constexpr std::uint32_t kIntConsts = 16;
inline constexpr std::uint32_t kBoolConsts = 16;
inline constexpr std::size_t kStageConstsIntOffset = kFloatConsts * 16;
inline constexpr std::size_t kStageConstsBoolOffset = kStageConstsIntOffset + kIntConsts * 16;
inline constexpr std::size_t kStageConstsSize = kStageConstsBoolOffset + kBoolConsts * 4;

inline constexpr std::uint32_t kPixelSamplers = 16;
inline constexpr std::uint32_t kVertexSamplers = 4;
inline constexpr std::uint32_t kVertexSamplerBase = 16;  // vertex shader s0..s3 -> DrawBlock slots 16..19
inline constexpr std::uint32_t kSamplerSlots = kPixelSamplers + kVertexSamplers;

struct DrawBlock {
    std::uint32_t tex[kSamplerSlots];
    std::uint32_t smp[kSamplerSlots];
    float posFix[4];
    std::uint32_t alphaFunc;
    float alphaRef;
    std::uint32_t pad[2];
};
static_assert(sizeof(DrawBlock) == 192);

struct PushConstants {
    std::uint64_t vsConsts;
    std::uint64_t psConsts;
    std::uint64_t draw;
    std::uint64_t frame;
    std::uint64_t prevVsConsts;
    std::uint32_t drawId;
    std::uint32_t flags;
};
static_assert(sizeof(PushConstants) == 48);
// every Vulkan device has at least 128 bytes of push constants: the rest is reserved for later fields
inline constexpr std::uint32_t kPushConstantBudget = 128;
static_assert(sizeof(PushConstants) <= kPushConstantBudget);

// PushConstants::flags
inline constexpr std::uint32_t kDrawKindMask = 0xffu;
inline constexpr std::uint32_t kDrawPassShift = 8;
inline constexpr std::uint32_t kDrawViewShift = 12;
inline constexpr std::uint32_t kDrawFlagTinted = 0x10000u;  // the tint debug view's variant draws it
constexpr std::uint32_t drawFlags(std::uint32_t kind, std::uint32_t pass, std::uint32_t view) {
    return (kind & kDrawKindMask) | ((pass & 0xfu) << kDrawPassShift) | ((view & 0xfu) << kDrawViewShift);
}

// The per-frame block. Matrices: the engine's row-major, row-vector (v' = v * M) matrices stored as they are; GLSL's
// column-major mat4 reads them as the transpose, so `clip = viewProj * vec4(world, 1)` there.
struct FrameBlock {
    float viewProj[16];
    float invViewProj[16];
    float view[16];
    float proj[16];
    float invView[16];
    float invProj[16];
    float prevViewProj[16];     // = viewProj when kFramePrevValid is clear
    float prevInvViewProj[16];
    float cameraPos[4];         // xyz, w = engine far (c11.w: the depth map stores view depth / w)
    float prevCameraPos[4];
    float depthParams[4];       // projection near, projection far, engine far, 1 / engine far
    float viewport[4];          // scene target width, height, 1 / width, 1 / height (0 before it is known)
    float jitter[4];            // xy this frame's sub-pixel jitter in pixels, zw the previous frame's (0: no TAA yet)
    float time[4];              // seconds since the device was created, seconds since the previous frame, 0, 0
    std::uint32_t frameInfo[4]; // frame index (low 32 bits), kFrame* flags, main views so far this frame, 0
    std::uint32_t images[4];    // bindless t2D slots: scene HDR, refraction copy, depth map; tCube slot of the sky cube
    float params[16][4];        // feature parameters: kFrameParam* (an enhancement owns its indices)
    // the ray tracing scene (enh/rt_scene, docs/research/rt-reflections.md 3; zero while there is none this submission):
    // TLAS device address (low, high: GLSL accelerationStructureEXT(uvec2) needs no shaderInt64), geometry records address
    std::uint32_t rtScene[4];
    std::uint32_t rtInfo[4];    // instances in the TLAS, records, kFrameRt* flags, 0
};
static_assert(sizeof(FrameBlock) == 928);
inline constexpr std::uint32_t kFrameRtValid = 1u;  // rtScene holds this frame's TLAS (built at AfterOpaque)
inline constexpr std::uint32_t kFrameCameraValid = 1u;      // this frame's main camera
inline constexpr std::uint32_t kFrameCameraPredicted = 2u;  // the previous frame's (no main view yet this frame)
inline constexpr std::uint32_t kFramePrevValid = 4u;        // the prev* fields hold the frame before
inline constexpr std::uint32_t kFrameParams = 16;
inline constexpr std::uint32_t kFrameParamDebugView = 0;   // debug.inject_view: x mode, y strength, z grid spacing
inline constexpr std::uint32_t kFrameParamLod = 1;         // lod.trees: x the billboard fade distance scale (Device::lodModule)

// One geometry record per TLAS instance x geometry of the ray tracing scene (enh/rt_scene; GLSL RvkRtRecord in
// shaders/include/rvk_rt.glsl, reached as records[instanceCustomIndex + geometryIndex]). Vertex attributes are words
// `offset | D3DDECLTYPE << 16` (kRtAttribAbsent = not in the declaration). Material fields are filled from M9 P2 on.
struct RtGeometryRecord {
    std::uint32_t vertices[2];   // device address of vertex 0 (the draw's stream offset + base vertex applied)
    std::uint32_t indices[2];    // device address of the draw's first index
    std::uint32_t stride;        // vertex stride (bytes)
    std::uint32_t flags;         // kRtRecord* | draws::Kind << 8 | view bits << 16
    std::uint32_t normal;        // attribute words
    std::uint32_t texcoord0;
    std::uint32_t texcoord1;
    std::uint32_t texcoord6;     // the lightmap uv
    std::uint32_t color0;
    std::uint32_t instanceId;    // stable id of the instance in the registry (debug colours, temporal reuse)
    std::uint32_t albedoTex;     // bindless t2D slot of the diffuse texture (0 = none: kRtUvNoTexture's colour)
    std::uint32_t albedoSampler; // bindless sampler (the draw's sampler states of that slot)
    std::uint32_t alpha;         // alpha test: D3DCMPFUNC | reference (0..255) << 8 (func 8 = always)
    std::uint32_t uvSet;         // kRtUv*: which vertex uv set the albedo's coordinate follows from + flags
    // affine map of that uv set to the pixel shader's diffuse coordinate: u' = dot(uvMap[0].xy, uv) + uvMap[0].z, v' with
    // [1]; uvMap[0].w = the constant colour (RGBA8 bits) of kRtUvNoTexture, uvMap[1].w = the opacity texture's bindless slot
    // (kRtUvOpacityTexture)
    float uvMap[2][4];
    // material segments (kRtRecordSegmented): the materials of sub-ranges of this draw range, as the main view drew them
    // (the sun cascades draw a level mesh in one merged range, the main view per material): device address of
    // segmentCount RtMaterialSegment sorted by firstPrimitive; triangles outside every segment keep the record's material
    std::uint32_t segments[2];
    std::uint32_t segmentCount;
    std::uint32_t pad;
};
static_assert(sizeof(RtGeometryRecord) == 112);
// the material of the record's triangles [firstPrimitive, firstPrimitive + primitiveCount) (the record's fields of that name)
struct RtMaterialSegment {
    std::uint32_t firstPrimitive;
    std::uint32_t primitiveCount;
    std::uint32_t albedoTex;
    std::uint32_t albedoSampler;
    std::uint32_t alpha;
    std::uint32_t uvSet;
    std::uint32_t pad[2];
    float uvMap[2][4];
};
static_assert(sizeof(RtMaterialSegment) == 64);
inline constexpr std::uint32_t kRtAttribAbsent = 0xFFFFFFFFu;
inline constexpr std::uint32_t kRtRecordIndex32 = 1u;      // 32-bit indices (else 16-bit)
inline constexpr std::uint32_t kRtRecordAlphaTested = 2u;  // the draw had an alpha test (non-opaque instance)
inline constexpr std::uint32_t kRtRecordSpeedTree = 4u;    // SpeedTree branches / fronds in their rest pose (no wind)
inline constexpr std::uint32_t kRtRecordInstanced = 8u;    // from an instanced batch
inline constexpr std::uint32_t kRtRecordNoDepthWrite = 16u; // a main-view solid draw of it had z writes off (not in the depth buffer)
inline constexpr std::uint32_t kRtRecordDepthBias = 32u;    // a main-view draw of it had a depth bias (the depth buffer is offset)
inline constexpr std::uint32_t kRtRecordSegmented = 64u;    // per-triangle materials: RtGeometryRecord::segments
inline constexpr std::uint32_t kRtRecordKindShift = 8;   // bits 8-15: draws::Kind of the draw
inline constexpr std::uint32_t kRtRecordViewShift = 16;  // bits 16-23: views it was seen in (1 main, 2 sun cascades)
// RtGeometryRecord::uvSet (M9 P2, docs/research/rt-reflections.md 3.2)
inline constexpr std::uint32_t kRtUvSetMask = 0x3u;          // 0 = TEXCOORD0, 1 = TEXCOORD1 of the vertex
inline constexpr std::uint32_t kRtUvApprox = 0x4u;           // the pixel shader's coordinate is not affine in that set (parallax, ...)
inline constexpr std::uint32_t kRtUvNoTexture = 0x8u;        // no diffuse texture: uvMap[0].w holds a colour
inline constexpr std::uint32_t kRtUvHeuristic = 0x10u;       // slot from the bytecode fallback (s9), not the material graph
inline constexpr std::uint32_t kRtUvOpacityShift = 8;        // bits 8-9: the opacity's channel (0 r .. 3 a) of its texture
inline constexpr std::uint32_t kRtUvOpacityOpaque = 0x400u;  // the opacity is no texture: never cut out
inline constexpr std::uint32_t kRtUvOpacityTexture = 0x800u; // the opacity has its own texture (bindless slot in uvMap[1].w)
// TLAS instance masks
inline constexpr std::uint32_t kRtMaskOpaque = 0x01u;
inline constexpr std::uint32_t kRtMaskAlphaTested = 0x02u;

// Specialisation-constant ids reserved for variant hooks (GlslOptions::extraGlobals: one SPIR-V module, the variant chosen
// per pipeline). The default translation declares none; the first enhancement that adds an extra output takes these.
inline constexpr std::uint32_t kSpecMotionOutput = 0;  // bool: write screen-space motion to an extra colour output
inline constexpr std::uint32_t kSpecNormalOutput = 1;  // bool: write the interpolated world normal to an extra output

// The first 16 bytes of every pass shader's push block (GLSL RVK_PASS_PUSH_HEADER)
struct PassPushHeader {
    std::uint64_t frame;  // FrameBlock address
    std::uint32_t passId;
    std::uint32_t passFlags;
};
static_assert(sizeof(PassPushHeader) == 16);

inline constexpr std::uint32_t kMaxVaryings = 32;
inline constexpr std::uint32_t kInvalidLocation = 0xFFFFFFFFu;

// Location of an interpolated value between the vertex and pixel shader (both sides use this mapping, so they link
// by semantic, as D3D9 does). kInvalidLocation for semantics that are not varyings (POSITION0, PSIZE).
constexpr std::uint32_t varyingLocation(Usage usage, std::uint32_t index) {
    switch (usage) {
    case Usage::TexCoord: return index < 16 ? index : kInvalidLocation;
    case Usage::Color: return index < 2 ? 16 + index : kInvalidLocation;
    case Usage::Fog: return 18;
    case Usage::Normal: return index < 2 ? 19 + index : kInvalidLocation;
    case Usage::Tangent: return index < 2 ? 21 + index : kInvalidLocation;
    case Usage::Binormal: return index < 2 ? 23 + index : kInvalidLocation;
    case Usage::Position: return index >= 1 && index < 3 ? 24 + index : kInvalidLocation;  // POSITION1/2: 25, 26
    case Usage::BlendWeight: return index == 0 ? 27 : kInvalidLocation;
    case Usage::BlendIndices: return index == 0 ? 28 : kInvalidLocation;
    case Usage::Depth: return index == 0 ? 29 : kInvalidLocation;
    case Usage::PSize: return kInvalidLocation;
    default: return kInvalidLocation;
    }
}

}  // namespace rvk::shader::abi
