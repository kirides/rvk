// The MeshPart / Layout records' pure logic (src/gpuscene/mesh_layout.h, docs/research/frame-ownership.md F0): the declaration -> per-semantic words, and the
// element decode shaders/include/rvk_mesh.hlsli `rvkMeshDecode` is the twin of (hand-computed expectations, D3D9 vertex shader input rules).

#include <cmath>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include "gpuscene/mesh_layout.h"
#include "test.h"

using namespace rvk;
using namespace rvk::gpuscene;
using rvk::d3d9::VertexElement;

namespace {
VertexElement el(std::uint16_t stream, std::uint16_t offset, std::uint8_t type, shader::Usage usage, std::uint8_t index) {
    return {stream, offset, type, 0, static_cast<std::uint8_t>(usage), index};
}
bool near(float a, float b) { return std::fabs(a - b) < 1e-6f; }
void decode(std::uint32_t type, std::initializer_list<std::uint8_t> bytes, float out[4]) {
    std::vector<std::uint8_t> b(bytes);
    b.resize(16, 0);
    decodeElement(type, b.data(), out);
}
}  // namespace

TEST(mesh_parts_record_sizes_match_the_shader_strides) {
    CHECK(sizeof(PartRecord) == 128);
    CHECK(sizeof(LayoutRecord) == 80);
    CHECK(kSlotCount == 16);
}

TEST(mesh_layout_maps_semantics_to_slot_words) {
    const std::vector<VertexElement> els = {
        el(0, 0, d3d9::DECLTYPE_FLOAT3, shader::Usage::Position, 0),   el(0, 12, d3d9::DECLTYPE_FLOAT3, shader::Usage::Normal, 0),
        el(0, 24, d3d9::DECLTYPE_FLOAT2, shader::Usage::TexCoord, 0),  el(1, 8, d3d9::DECLTYPE_FLOAT2, shader::Usage::TexCoord, 3),
        el(0, 32, d3d9::DECLTYPE_D3DCOLOR, shader::Usage::Color, 1),   el(2, 4, d3d9::DECLTYPE_FLOAT4, shader::Usage::Tangent, 0),
    };
    const LayoutRecord r = buildLayout(els);
    CHECK(r.info[0] == 1 && r.info[2] == 6 && r.info[3] == 0);
    CHECK(r.slot[kSlotPosition] == (0u | (d3d9::DECLTYPE_FLOAT3 + 1u) << 16));
    CHECK(slotOffset(r.slot[kSlotNormal]) == 12 && slotType(r.slot[kSlotNormal]) == d3d9::DECLTYPE_FLOAT3 && slotStream(r.slot[kSlotNormal]) == 0);
    CHECK(slotOffset(r.slot[kSlotTexcoord0]) == 24 && slotType(r.slot[kSlotTexcoord0]) == d3d9::DECLTYPE_FLOAT2);
    CHECK(slotOffset(r.slot[kSlotTexcoord0 + 3]) == 8 && slotStream(r.slot[kSlotTexcoord0 + 3]) == 1);
    CHECK(slotType(r.slot[kSlotColor0 + 1]) == d3d9::DECLTYPE_D3DCOLOR);
    CHECK(slotStream(r.slot[kSlotTangent]) == 2 && slotOffset(r.slot[kSlotTangent]) == 4);
    CHECK(r.slot[kSlotBinormal] == 0 && r.slot[kSlotBlendIndices] == 0 && r.slot[kSlotColor0] == 0);  // absent
}

TEST(mesh_layout_counts_what_it_cannot_express) {
    const std::vector<VertexElement> els = {
        el(0, 0, d3d9::DECLTYPE_FLOAT3, shader::Usage::Position, 0),
        el(0, 12, d3d9::DECLTYPE_FLOAT3, shader::Usage::Position, 0),  // duplicate
        el(0, 24, d3d9::DECLTYPE_FLOAT1, shader::Usage::PSize, 0),     // no slot
        el(0, 28, d3d9::DECLTYPE_FLOAT2, shader::Usage::TexCoord, 8),  // index past 7
        el(0, 36, d3d9::DECLTYPE_UNUSED, shader::Usage::Normal, 0),    // unknown type
        el(0, 40, d3d9::DECLTYPE_FLOAT3, shader::Usage::Position, 1),  // second position
    };
    const LayoutRecord r = buildLayout(els);
    CHECK(r.info[2] == 6);
    CHECK(r.info[3] == 5);
    CHECK(slotOffset(r.slot[kSlotPosition]) == 0);  // the first one wins
    CHECK(r.slot[kSlotNormal] == 0);
}

TEST(mesh_layout_hash_is_content_based) {
    const std::vector<VertexElement> a = {el(0, 0, d3d9::DECLTYPE_FLOAT3, shader::Usage::Position, 0), el(0, 12, d3d9::DECLTYPE_FLOAT2, shader::Usage::TexCoord, 0)};
    std::vector<VertexElement> b = a;
    CHECK(hashElements(a) == hashElements(b));
    const LayoutRecord la = buildLayout(a), lb = buildLayout(b);
    CHECK(std::memcmp(&la, &lb, sizeof(LayoutRecord)) == 0);
    b[1].offset = 16;
    CHECK(hashElements(a) != hashElements(b));
}

TEST(mesh_layout_decode_float_and_fill_rules) {
    float o[4];
    decode(d3d9::DECLTYPE_FLOAT1, {0, 0, 0x80, 0x3f}, o);  // 1.0f
    CHECK(near(o[0], 1.0f) && o[1] == 0 && o[2] == 0 && o[3] == 1.0f);
    float v[3] = {1.5f, -2.0f, 4.0f};
    std::uint8_t raw[16]{};
    std::memcpy(raw, v, 12);
    decodeElement(d3d9::DECLTYPE_FLOAT3, raw, o);
    CHECK(o[0] == 1.5f && o[1] == -2.0f && o[2] == 4.0f && o[3] == 1.0f);
    float v4[4] = {1, 2, 3, 0.25f};
    std::memcpy(raw, v4, 16);
    decodeElement(d3d9::DECLTYPE_FLOAT4, raw, o);
    CHECK(o[3] == 0.25f);
    decodeElement(d3d9::DECLTYPE_FLOAT2, raw, o);
    CHECK(o[0] == 1 && o[1] == 2 && o[2] == 0 && o[3] == 1);
}

TEST(mesh_layout_decode_packed_types) {
    float o[4];
    decode(d3d9::DECLTYPE_D3DCOLOR, {0x10, 0x20, 0x30, 0x40}, o);  // bytes B, G, R, A -> (R, G, B, A)
    CHECK(near(o[0], 0x30 / 255.0f) && near(o[1], 0x20 / 255.0f) && near(o[2], 0x10 / 255.0f) && near(o[3], 0x40 / 255.0f));
    decode(d3d9::DECLTYPE_UBYTE4, {1, 2, 3, 255}, o);
    CHECK(o[0] == 1 && o[1] == 2 && o[2] == 3 && o[3] == 255);
    decode(d3d9::DECLTYPE_UBYTE4N, {0, 51, 255, 255}, o);
    CHECK(near(o[1], 0.2f) && o[2] == 1.0f);
    decode(d3d9::DECLTYPE_SHORT2, {0xfe, 0xff, 5, 0}, o);  // -2, 5
    CHECK(o[0] == -2 && o[1] == 5 && o[2] == 0 && o[3] == 1);
    decode(d3d9::DECLTYPE_SHORT4, {1, 0, 0xff, 0xff, 0, 1, 0, 0x80}, o);
    CHECK(o[0] == 1 && o[1] == -1 && o[2] == 256 && o[3] == -32768);
    decode(d3d9::DECLTYPE_SHORT2N, {0xff, 0x7f, 0x00, 0x80}, o);  // 32767 -> 1, -32768 -> clamped -1
    CHECK(o[0] == 1.0f && o[1] == -1.0f);
    decode(d3d9::DECLTYPE_SHORT4N, {0, 0, 0xff, 0x3f, 0, 0xc0, 0xff, 0x7f}, o);  // 0, 16383, -16384, 32767
    CHECK(o[0] == 0 && near(o[1], 16383.0f / 32767.0f) && near(o[2], -16384.0f / 32767.0f) && o[3] == 1.0f);
    decode(d3d9::DECLTYPE_USHORT2N, {0xff, 0xff, 0, 0x80}, o);
    CHECK(o[0] == 1.0f && near(o[1], 32768.0f / 65535.0f));
    decode(d3d9::DECLTYPE_USHORT4N, {0, 0, 0xff, 0xff, 0, 0x80, 0, 0}, o);
    CHECK(o[0] == 0 && o[1] == 1.0f && near(o[2], 32768.0f / 65535.0f) && o[3] == 0);
}

TEST(mesh_layout_decode_dec3_and_half) {
    float o[4];
    // UDEC3: x = 1, y = 2, z = 1023 (bits 0..9, 10..19, 20..29)
    const std::uint32_t u = 1u | 2u << 10 | 1023u << 20;
    std::uint8_t raw[16]{};
    std::memcpy(raw, &u, 4);
    decodeElement(d3d9::DECLTYPE_UDEC3, raw, o);
    CHECK(o[0] == 1 && o[1] == 2 && o[2] == 1023 && o[3] == 1);
    // DEC3N: x = 511 -> 1, y = -512 -> clamped -1, z = -256 -> -256 / 511
    const std::uint32_t d = 511u | (static_cast<std::uint32_t>(-512) & 0x3ffu) << 10 | (static_cast<std::uint32_t>(-256) & 0x3ffu) << 20;
    std::memcpy(raw, &d, 4);
    decodeElement(d3d9::DECLTYPE_DEC3N, raw, o);
    CHECK(o[0] == 1.0f && o[1] == -1.0f && near(o[2], -256.0f / 511.0f));
    CHECK(halfToFloat(0x3c00) == 1.0f && halfToFloat(0xc000) == -2.0f && halfToFloat(0x0000) == 0.0f && halfToFloat(0x3800) == 0.5f);
    CHECK(near(halfToFloat(0x0001), 5.9604645e-8f));  // the smallest subnormal
    CHECK(std::isinf(halfToFloat(0x7c00)) && halfToFloat(0xfc00) < 0);
    decode(d3d9::DECLTYPE_FLOAT16_2, {0x00, 0x3c, 0x00, 0xc0}, o);
    CHECK(o[0] == 1.0f && o[1] == -2.0f && o[2] == 0 && o[3] == 1);
    decode(d3d9::DECLTYPE_FLOAT16_4, {0x00, 0x3c, 0x00, 0xc0, 0x00, 0x38, 0x00, 0x00}, o);
    CHECK(o[0] == 1.0f && o[1] == -2.0f && o[2] == 0.5f && o[3] == 0.0f);
}

TEST(mesh_layout_type_sizes_cover_every_decl_type) {
    for (std::uint32_t t = 0; t < d3d9::DECLTYPE_UNUSED; ++t) CHECK_MSG(declTypeBytes(t) != 0, std::to_string(t));
    CHECK(declTypeBytes(d3d9::DECLTYPE_UNUSED) == 0);
    CHECK(declTypeBytes(d3d9::DECLTYPE_FLOAT3) == 12 && declTypeBytes(d3d9::DECLTYPE_FLOAT16_4) == 8 && declTypeBytes(d3d9::DECLTYPE_DEC3N) == 4);
}
