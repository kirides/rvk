#pragma once

// The illumination table's records and the pure composition of a lit draw's lighting registers from them (frame-ownership.md F0).
// `composeLighting` reproduces what StaticOps::writeLighting (native/static_ops.cpp, FUN_1806cd1d0) leaves in the lighting pool c14 .. c37
// from exactly two things: the draw object's illumination record and its light set's record (+ the pass's light count / first light).
// shaders/include/rvk_illum.hlsli `rvkIllumCompose` is the same function for the GPU; keep them in step (tests/illum_table_tests.cpp checks
// the CPU side against the engine-shaped rules, the shader is compiled by `rvk_shadertool dxil-check embedded`).
//
// Records hold RAW engine bytes (what writeLighting reads, whatever the object's counts say: the engine reads unused entries too):
//   eSShaderMaterialIllumination bytes [0, 0x110): +0 list pointer (replaced by the set slot in `info.y`), +8 float f, +0xc no-sun byte,
//     +0xd static light count, +0x10 two weighted static lights x 0x40 B (a third / fourth entry would read +0x90 / +0xd0: the SH flag and the
//     SH, raw), +0x90 SH flag, +0x94 static SH 7 x 16 B
//   the light list (= eCLightSet + 0x10) bytes [0, 0x140): +4 the sun 48 B (c22 .. c24), +0x34 dynamic light count, +0x38 dynamic lights x 0x40 B
//     (the engine fills at most 2: entries 2 / 3 read the SH raw), +0xb8 hemisphere SH 7 x 16 B, +0x128 indoor colour 12 B

#include <cstdint>
#include <cstring>

#include "native/static_compose.h"

namespace rvk::gpuscene {

inline constexpr std::uint32_t kIllumRawBytes = 0x110;
inline constexpr std::uint32_t kLightSetRawBytes = 0x140;
inline constexpr std::uint32_t kNoSet = 0xffffffffu;

// GPU: 288 bytes at slot * 288 (RvkIllum in shaders/include/rvk_illum.hlsli)
struct alignas(16) IllumRecord {
    std::uint32_t info[4];  // x = live (1), y = light set slot (kNoSet = none), z = generation (bumped when the record's content changed), w = 0
    std::uint8_t raw[kIllumRawBytes];
};
static_assert(sizeof(IllumRecord) == 288);

// GPU: 336 bytes at slot * 336
struct alignas(16) LightSetRecord {
    std::uint32_t info[4];  // x = live (1), y = generation, z / w = the list's address (diagnostics)
    std::uint8_t raw[kLightSetRawBytes];
};
static_assert(sizeof(LightSetRecord) == 336);

// the pass-dependent inputs of writeLighting
struct ComposeParams {
    bool tableLit = false;   // Full-owned lighting: 3-light permutation, the table's lights per pixel
    bool holdsAll = false;   // the table holds every light of this object: the three light registers stay zero, nothing read
    std::uint32_t n = 0;     // lights the pass takes (3 when tableLit, else passLights(flags))
    std::uint32_t firstLight = 0;
};

namespace detail {
template <class T>
inline T load(const std::uint8_t* raw, std::uint32_t off) {
    T v;
    std::memcpy(&v, raw + off, sizeof(T));
    return v;
}
}  // namespace detail

// Writes the registers writeLighting writes into `L` (c14 .. c37 as L[r - 14]; bytes it does not write keep their value, e.g. c14.w). Returns false
// when the pass reads beyond the records (a light index past the raw bytes): nothing is guaranteed then.
inline bool composeLighting(const IllumRecord& ir, const LightSetRecord& sr, const ComposeParams& p, float L[24][4]) {
    using detail::load;
    const std::uint8_t* il = ir.raw;
    const std::uint8_t* ls = sr.raw;
    float indoor[3];
    std::memcpy(indoor, ls + 0x128, 12);
    std::memcpy(L[0], indoor, 12);
    float sh[7][4], staticSh[7][4]{};
    std::memcpy(sh, ls + 0xb8, sizeof(sh));
    const bool hasStatic = il[0x90] != 0;
    if (hasStatic) std::memcpy(staticSh, il + 0x94, sizeof(staticSh));
    native::sc::ambientSh(sh, indoor, load<float>(il, 8), hasStatic ? &staticSh[0][0] : nullptr, &L[1]);
    std::memcpy(L[8], ls + 4, 48);
    if (p.holdsAll) {
        std::memset(L[11], 0, 3 * 4 * 16);
        return true;
    }
    const std::uint32_t nStatic = il[0x0d];
    const std::uint32_t nDynamic = ls[0x34];
    const std::uint32_t have = p.tableLit ? std::min<std::uint32_t>(3u, nStatic + nDynamic) : p.n;
    if (p.tableLit) std::memset(L[11], 0, 3 * 4 * 16);
    bool ok = true;
    for (std::uint32_t j = 0; j < std::min(p.n, have); ++j) {
        const std::uint32_t idx = p.firstLight + j;
        const std::uint8_t* src;
        if (idx < nStatic) {
            if (0x10 + 0x40 * idx + 64 > kIllumRawBytes) {
                ok = false;
                continue;
            }
            src = il + 0x10 + 0x40 * idx;
        } else {
            if (0x38 + 0x40 * (idx - nStatic) + 64 > kLightSetRawBytes) {
                ok = false;
                continue;
            }
            src = ls + 0x38 + 0x40 * (idx - nStatic);
        }
        if (11 + 4 * j + 4 <= 24) std::memcpy(L[11 + 4 * j], src, 64);
    }
    return ok;
}

}  // namespace rvk::gpuscene
