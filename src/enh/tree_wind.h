#pragma once

// F4-5 (docs/research/frame-ownership.md 14.17): the SpeedTree wind / sway of the ray tracing scene without vertex captures. The position part of the engine's
// SpeedTree vertex shader (ge_default_vs.fx Transform with XFX_SPEEDTREE, ported in shaders/material/ge_default.vert.hlsl) is evaluated per tree instance by
// shaders/rt/rt_tree_wind.comp.hlsl; this header holds what the CPU side shares with it: the job word layout, the vertex class of a declaration and a CPU
// reference of the math (rvk_rhitest, rvk_tests). Pure, no engine / GPU dependencies.

#include <cmath>
#include <cstdint>
#include <span>

namespace rvk::enh::treewind {

// the vertex classes (job word 2.y): what the request's XFX_ST_* macros make of the position
enum Class : std::uint32_t {
    kWind = 0,       // branches (XFX_ST_BRANCH) and fronds (XFX_SPEEDTREE only): the wind matrix blend, then the world rows
    kLeafMesh = 1,   // XFX_ST_LEAFMESH: + the rocked offset (BLENDWEIGHT2.xyz) through the mesh matrix (NORMAL1, NORMAL2)
    kLeafCards = 2,  // XFX_ST_LEAFCARDS: + the card corner (BLENDWEIGHT1 / 2) rotated by the rock / rustle angles and the leaf angle matrix (c23..c25)
};

inline constexpr std::uint32_t kAbsent = 0xFFFFFFFFu;
// attribute words of a job (offset | D3DDECLTYPE << 16), in the job's order
enum Attr : std::uint32_t { kPos = 0, kIndices0, kWeights0, kWeights1, kWeights2, kNormal1, kNormal2, kTex0, kAttrCount };
// constants of a job: 20 float4 = c14..c29 (wind matrices, leaf angle matrix, rock / rustle angles), c32..c35 (the instance's row)
inline constexpr std::uint32_t kConsts = 20;
inline constexpr std::uint32_t kJobWords = 20;  // 5 uint4 per job
inline constexpr std::uint32_t kHeadBytes = 16;
inline constexpr std::uint32_t kErrBytes = 32;  // the compare accumulator: maxBits, compared, missing, over 0.1, over 1, max bits per class

struct Element {  // the D3DVERTEXELEMENT9 fields the layout needs
    std::uint16_t stream = 0, offset = 0;
    std::uint8_t type = 0, usage = 0, usageIndex = 0;
};

struct Layout {
    bool ok = false;
    const char* why = nullptr;  // when not ok
    Class cls = kWind;
    std::uint32_t words[kAttrCount] = {kAbsent, kAbsent, kAbsent, kAbsent, kAbsent, kAbsent, kAbsent, kAbsent};
};

// D3DDECLTYPEs the compute pass decodes (rt_tree_wind.comp.hlsl twAttr)
inline bool typeSupported(std::uint8_t t) { return t <= 12 || t == 15 || t == 16; }

// the class and attribute words of a SpeedTree draw's declaration (stream 0 only; the instance stream is the batch's): op 0 / 1 (branches, fronds) = the wind
// only, op > 1 (leaves) = leaf mesh when NORMAL1 is declared (the mesh matrix), else leaf cards (BLENDWEIGHT1 + 2)
inline Layout layoutOf(std::span<const Element> elements, std::uint32_t opDesc) {
    Layout l;
    for (const Element& e : elements) {
        if (e.stream == 0xFF) break;
        if (e.stream != 0) continue;
        int a = -1;
        if (e.usage == 0 && e.usageIndex == 0) a = kPos;
        else if (e.usage == 2 && e.usageIndex == 0) a = kIndices0;
        else if (e.usage == 1 && e.usageIndex <= 2) a = kWeights0 + e.usageIndex;
        else if (e.usage == 3 && (e.usageIndex == 1 || e.usageIndex == 2)) a = e.usageIndex == 1 ? kNormal1 : kNormal2;
        else if (e.usage == 5 && e.usageIndex == 0) a = kTex0;
        if (a < 0) continue;
        if (!typeSupported(e.type) || e.offset > 0xFFFF) {
            l.why = "attribute type";
            return l;
        }
        l.words[a] = static_cast<std::uint32_t>(e.offset) | (static_cast<std::uint32_t>(e.type) << 16);
    }
    if (l.words[kPos] != (2u << 16)) {
        l.why = "no POSITION FLOAT3 at offset 0";
        return l;
    }
    if (l.words[kIndices0] == kAbsent || l.words[kWeights0] == kAbsent) {
        l.why = "no wind attributes (BLENDINDICES0 / BLENDWEIGHT0)";
        return l;
    }
    if (opDesc > 1) {
        if (l.words[kWeights2] == kAbsent) {
            l.why = "leaves without BLENDWEIGHT2";
            return l;
        }
        if (l.words[kNormal1] != kAbsent && l.words[kNormal2] != kAbsent) {
            l.cls = kLeafMesh;
        } else if (l.words[kWeights1] != kAbsent) {
            l.cls = kLeafCards;
        } else {
            l.why = "leaves: neither a leaf mesh (NORMAL1 / 2) nor leaf cards (BLENDWEIGHT1)";
            return l;
        }
    }
    l.ok = true;
    return l;
}

// one vertex's decoded inputs (as the shader's input assembler gives them: missing components 0, w = 1)
struct Vertex {
    float pos[4] = {0, 0, 0, 1};
    float indices0[4] = {0, 0, 0, 1};
    float weights0[4] = {0, 0, 0, 1}, weights1[4] = {0, 0, 0, 1}, weights2[4] = {0, 0, 0, 1};
    float normal1[4] = {0, 0, 0, 1}, normal2[4] = {0, 0, 0, 1};
};

// the CPU reference of the compute pass (= the port's Transform for XFX_SPEEDTREE): the world position of one vertex; consts = kConsts float4
inline void world(const float (*consts)[4], const Vertex& v, Class cls, float out[3]) {
    const auto reg = [&](int r, int c) -> float { return r >= 14 && r <= 29 ? consts[r - 14][c] : 0.0f; };
    const auto dot3 = [](const float* a, float x, float y, float z) { return a[0] * x + a[1] * y + a[2] * z; };
    const int wi = static_cast<int>(v.indices0[0]);
    float p[4] = {v.pos[0], v.pos[1], v.pos[2], v.pos[3]};
    for (int r = 0; r < 3; ++r) {
        const float wp = reg(14 + 3 * wi + r, 0) * v.pos[0] + reg(14 + 3 * wi + r, 1) * v.pos[1] + reg(14 + 3 * wi + r, 2) * v.pos[2];
        p[r] = v.pos[r] + v.weights0[0] * (wp - v.pos[r]);
    }
    float off[3] = {0, 0, 0};
    if (cls != kWind) {
        const int la = 26 + static_cast<int>(v.weights2[3]);
        const float rock = consts[16][0] * reg(la, 0), rustle = consts[16][1] * reg(la, 1);
        if (cls == kLeafMesh) {
            // RotX(rock) * w2.xyz, then the rows (n1, -cross(n1, n2), n2)
            const float s = std::sin(rock), c = std::cos(rock);
            const float x = v.weights2[0], y = c * v.weights2[1] - s * v.weights2[2], z = s * v.weights2[1] + c * v.weights2[2];
            const float* n1 = v.normal1;
            const float* n2 = v.normal2;
            const float m1[3] = {-(n1[1] * n2[2] - n1[2] * n2[1]), -(n1[2] * n2[0] - n1[0] * n2[2]), -(n1[0] * n2[1] - n1[1] * n2[0])};
            off[0] = dot3(n1, x, y, z);
            off[1] = dot3(m1, x, y, z);
            off[2] = dot3(n2, x, y, z);
        } else {
            float o[3] = {v.weights1[1] * v.weights2[1], v.weights1[0] * v.weights2[2], 0.0f};
            {  // RotZ(w1.w + rock)
                const float a = v.weights1[3] + rock, s = std::sin(a), c = std::cos(a);
                const float x = c * o[0] - s * o[1], y = s * o[0] + c * o[1];
                o[0] = x;
                o[1] = y;
            }
            {  // RotY(w1.z + rustle)
                const float a = v.weights1[2] + rustle, s = std::sin(a), c = std::cos(a);
                const float x = c * o[0] + s * o[2], z = -s * o[0] + c * o[2];
                o[0] = x;
                o[2] = z;
            }
            for (int r = 0; r < 3; ++r) off[r] = reg(23, r) * o[0] + reg(24, r) * o[1] + reg(25, r) * o[2];
        }
    }
    for (int r = 0; r < 3; ++r) {
        const float* row = consts[17 + r];
        out[r] = p[0] * row[0] + p[1] * row[1] + p[2] * row[2] + p[3] * row[3] + off[r];
    }
}

}  // namespace rvk::enh::treewind
