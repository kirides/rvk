#pragma once

// The RT scene's TLAS instance list built on the GPU (docs/research/frame-ownership.md 14.43; setting `native.rt_gpu_build`, Debug, Full, default on).
//
// The CPU keeps deciding what goes into the TLAS (membership, order, level-of-detail rung, proxy / box, cut-out, glass, segments: state the GPU cannot see)
// but no longer composes every VkAccelerationStructureInstanceKHR (= D3D12_RAYTRACING_INSTANCE_DESC) + RtGeometryRecord field of every instance every frame.
// Three persistent GPU tables (gpuscene::GpuTable: slot, CPU twin, dirty uploads, read-back verify) hold what changes rarely:
//   * InstanceInput (one per registry instance, slot in Registry::Instance::gpuSlot): rows, the box rows of rung 2, the BLAS row, the material row, the
//     instance's flags / data word / id; rewritten only when one of them changed;
//   * BlasRow (one per BLAS): the AS address, the vertex / index references, the proxy's AS / indices at the active bias, stride + attribute words;
//     re-derived once per frame per BLAS in use, uploaded when they changed (compaction, rebuild, proxy built);
//   * MaterialRow (one per RT material id, slot 0 = no material): the record's material fields (bindless slots, the textures' log2 areas, uv set / map,
//     colour, opacity slot, pack word / pack LOD, the material's alpha test, masked / cut-out bits): materialFields() runs again only when the material's
//     stamp (its textures' slot / readiness / size, the pack's generation and map readiness) moved.
// Per frame the CPU writes one 16-byte Entry per TLAS instance (input slot, rung, cut-out, glass, segment table reference) and the characters / animated trees
// as literal 176-byte pairs; one compute dispatch (shaders/rt/rt_instance_build.comp.hlsl) writes the instances + records into per-frame-slot device
// buffers, the TLAS is built from there. `compose()` below is the shader's C++ twin (rvk_rhitest compares them byte for byte; the CPU fallback of a frame
// whose table upload failed uses it). Check frames compose the old CPU path as the reference and compare it with the GPU's output read back: byte for byte,
// 0 differing (rvk.log `rt: gpu build (5 s)`).

#include <cstdint>
#include <cstring>

#include "vk/vk.h"

#include "shader/abi.h"

namespace rvk::enh::rtbuild {

inline constexpr std::uint32_t kNone = 0xFFFFFFFFu;
inline constexpr std::uint32_t kEntryLiteral = 0x80000000u;  // Entry::input: a literal pair (characters, animated trees)
// Entry::mode
inline constexpr std::uint32_t kModeVariantMask = 0x3u;   // 0 the original BLAS, 1 the proxy, 2 the box
inline constexpr std::uint32_t kModeAlpha = 0x4u;         // cut-out (mask kRtMaskAlphaTested, non-opaque, kRtRecordAlphaTested)
inline constexpr std::uint32_t kModeGlass = 0x8u;         // glass pane (mask kRtMaskGlass)
inline constexpr std::uint32_t kModeSegShift = 16;        // bits 16-31: the record's segment count (0 = none)
// MaterialRow::bits
inline constexpr std::uint32_t kMatHas = 1u;     // a material (else the "no material" fields)
inline constexpr std::uint32_t kMatMasked = 2u;  // blend mode Masked
inline constexpr std::uint32_t kMatCut = 4u;     // can be cut out: an opacity texture and not kRtUvOpacityOpaque
// the box variant's fixed record fields (enh/rt_scene.cpp record(): rung 2)
inline constexpr std::uint32_t kBoxColour = 0xFF5C6166u;
inline constexpr std::uint32_t kSpeedTreeAlpha = 5u | (80u << 8);

struct InstanceInput {   // 128 B
    float rows[12];      // 0   the instance's rows (immutable while it lives)
    float boxRows[12];   // 48  rung 2: the unit box under the object's bounds (written while the instance is boxed)
    std::uint32_t blasRow;     // 96
    std::uint32_t material;    // 100 MaterialRow slot
    std::uint32_t flags;       // 104 Registry::Instance::flags
    std::uint32_t data;        // 108 Registry::Instance::data (alpha test word | kind << 16)
    std::uint32_t instanceId;  // 112
    std::uint32_t pad[3];      // 116
};
static_assert(sizeof(InstanceInput) == 128);

struct BlasRow {         // 64 B
    std::uint32_t as[2];          // 0  the original's AS address (0 = none: released)
    std::uint32_t vertices[2];    // 8  record reference of vertex 0
    std::uint32_t indices[2];     // 16 record reference of the first index
    std::uint32_t proxyAs[2];     // 24 the proxy at the active bias (0 = none)
    std::uint32_t proxyIndices[2];// 32
    std::uint32_t stride;         // 40
    std::uint32_t normal;         // 44
    std::uint32_t texcoord0;      // 48
    std::uint32_t texcoord1;      // 52
    std::uint32_t texcoord6;      // 56
    std::uint32_t color0;         // 60
};
static_assert(sizeof(BlasRow) == 64);

struct MaterialRow {     // 64 B
    std::uint32_t albedoTex;      // 0
    std::uint32_t albedoSampler;  // 4
    std::uint32_t uvSet;          // 8
    std::uint32_t packWord;       // 12
    std::uint32_t packLod;        // 16
    std::uint32_t alphaWord;      // 20 the material's own alpha test (0 = none)
    std::uint32_t bits;           // 24 kMat*
    std::uint32_t opacityTex;     // 28
    float uvMap[2][4];            // 32
    bool operator==(const MaterialRow& o) const { return std::memcmp(this, &o, sizeof(*this)) == 0; }
};
static_assert(sizeof(MaterialRow) == 64);

struct Entry {           // 16 B: one TLAS instance of the frame (index = instanceCustomIndex)
    std::uint32_t input;   // InstanceInput slot | kEntryLiteral + literal index
    std::uint32_t mode;    // kMode*
    std::uint32_t seg[2];  // the segment table reference of the record (mode's count != 0)
};
static_assert(sizeof(Entry) == 16);

struct Literal {         // 176 B: a pair the CPU composed (characters, animated trees)
    VkAccelerationStructureInstanceKHR vi;
    shader::abi::RtGeometryRecord r;
};
static_assert(sizeof(Literal) == 176);

// the dispatch's header (ring block): table references + capacities + the box's (rung 2) fixed AS / vertex / index references
struct Header {          // 112 B
    std::uint32_t count, literals, inputCap, blasCap;          // 0
    std::uint32_t matCap, pad0, pad1, pad2;                    // 16
    std::uint32_t entries[2], inputs[2];                       // 32
    std::uint32_t blas[2], materials[2];                       // 48
    std::uint32_t literal[2], outInst[2];                      // 64  (outInst: read-write reference)
    std::uint32_t outRec[2], boxAs[2];                         // 80  (outRec: read-write reference)
    std::uint32_t boxVertices[2], boxIndices[2];               // 96
};
static_assert(sizeof(Header) == 112);

// the record alpha of enh/rt_scene.cpp recordAlpha() from the material row's alpha word
inline std::uint32_t recordAlpha(std::uint32_t flags, std::uint32_t data, std::uint32_t matAlpha) {
    namespace abi = shader::abi;
    if ((flags & abi::kRtRecordSpeedTree) != 0 && (flags & abi::kRtRecordAlphaTested) != 0) return kSpeedTreeAlpha;
    if ((data & 0xFFFFu) != 0) return data & 0xFFFFu;
    return matAlpha != 0 ? matAlpha : 8u;
}

// The shader's C++ twin: entry `k` of the frame -> its instance + record. `literals` may be null when `literalCount` is 0. Out-of-range slots give an
// inactive instance (AS address 0, mask 0) and a zero record (the shader's guards).
void compose(const Header& h, std::uint32_t k, const Entry& e, const InstanceInput* inputs, const BlasRow* blas, const MaterialRow* materials,
             const Literal* literals, VkAccelerationStructureInstanceKHR& vi, shader::abi::RtGeometryRecord& r);

}  // namespace rvk::enh::rtbuild
