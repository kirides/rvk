#pragma once

// The material table's records and the pure composition of a draw's material registers / texture bindings from them
// (frame-ownership.md F0 material table; the pattern of illum_compose.h). One record per engine material instance as N5 draws it, keyed by the
// shader / constant-pool object (the eCShaderDefault the composition's constantsInto / materialSamplers read: pools at +0xb0 / +0xe0, sampler slots at
// +0xf8 + 0x28 k), holding exactly what the composition takes from the material:
//   * the VS pool (c232.. g_vShaderData: up to 32 registers from `vsFirst`) and the PS pool (c6..c13 g_ShaderData: up to 16 from `psFirst`), raw floats;
//   * the sampler pool: per slot k the bindless texture index of the slot's image (as materialSamplers resolves it: image data, the per-stage placeholder,
//     the error image) and the sampler-state words (raw: 0xABABABAB = the gfx admin's default filter, which is a global parameter of the draw);
//   * mask reference (shader +0x58), blend mode (+0x50), the flags that steer passes / states, the graph ids of the material's PS requests.
// Registers / slots a SetSwitch element writes (history dependent: the engine writes the pool in place per pass) are noted by the composition
// (`sw` masks, a union over the run) and not compared: the F1 table needs the switch tables for them.
// shaders/include/rvk_material_table.hlsli reads the same layout on the GPU.

#include <algorithm>
#include <bitset>
#include <cstdint>
#include <cstring>

#include "native/static_compose.h"

namespace rvk::gpuscene {

inline constexpr std::uint32_t kMatVsRegs = 32, kMatPsRegs = 16, kMatSlots = 8;
inline constexpr std::uint32_t kMatNoTex = 0xffffffffu;       // the slot's image could not be resolved
inline constexpr std::uint32_t kEngineNoState = 0xABABABABu;  // a sampler slot value meaning "the gfx admin's default filter"
inline constexpr std::uint32_t kMatRawBytes = 0x430;          // the engine shader object's bytes the record is built from

// MaterialRecord::info[3] bits
enum MatFlag : std::uint32_t {
    MatHasOpacity = 1u << 0,      // +0x330
    MatRefracted = 1u << 1,       // +0x420
    MatTradeAlpha = 1u << 2,      // +0x79
    MatZFill = 1u << 3,           // +0x2d8
    MatDepthBias = 1u << 4,       // +0x78
    MatDoubleSided = 1u << 5,     // XFX_DOUBLESIDED in the PS request's macros (a static macro: it splits pipelines)
    MatHasSwitch = 1u << 6,       // switch elements (+0x250 > 0)
    MatSlotsOverflow = 1u << 7,   // more sampler slots than the record holds (its textures are not checked)
};

// GPU: 1104 bytes at slot * 1104 (RvkMaterial in shaders/include/rvk_material_table.hlsli)
struct alignas(16) MaterialRecord {
    std::uint32_t info[4];    // x live (1), y version (bumped when the content changed), z material pack word (reserved: 0, the pack keys by tag at draw time), w MatFlag bits
    std::uint32_t counts[4];  // x vsFirst, y vsCount, z psFirst, w psCount
    std::uint32_t state[4];   // x blend mode (+0x50), y mask reference (+0x58), z first sampler stage (+0x219) | slots (+0x21b) << 8, w 0
    std::uint32_t graph[4];   // x PS_Default graph id, y PS_DefaultZPass graph id (0 = not seen), z / w 0 (a material has several permutations: pipeline ids are counted per run, not stored)
    std::uint32_t sw[4];      // x VS pool registers a switch element wrote, y PS pool registers | sampler slots << 16, z / w 0
    float vs[kMatVsRegs][4];
    float ps[kMatPsRegs][4];
    struct Slot {
        std::uint32_t tex;     // bindless slot of the slot's image (kMatNoTex = unresolved)
        std::uint32_t raw[6];  // sampler slot +0 / +4 / +0xc / +8 / +0x10 / +0x18: ADDRESSU, ADDRESSV, MAGFILTER, MINFILTER, MIPFILTER, MIPMAPLODBIAS
        std::uint32_t srgb;    // the int8 at +0x14, sign-extended
    } slots[kMatSlots];
};
static_assert(sizeof(MaterialRecord) == 1104);
static_assert(sizeof(MaterialRecord::Slot) == 32);

inline std::uint32_t matSlotCount(const MaterialRecord& r) { return (r.state[2] >> 8) & 0xffu; }
inline std::uint32_t matBaseStage(const MaterialRecord& r) { return r.state[2] & 0xffu; }

// What a draw's composition holds of its material (the native composition's DrawDesc, in plain arrays)
struct MaterialDraw {
    const float (*vsF)[4] = nullptr;  // kVsRegs registers
    const float (*psF)[4] = nullptr;  // kPsRegs registers
    const std::bitset<native::sc::kVsRegs>* vsLive = nullptr;
    const std::bitset<native::sc::kVsRegs>* psLive = nullptr;
    std::uint32_t samplerMask = 0;                           // stages the pixel shader samples
    std::uint32_t texSlot[native::sc::kStages]{};            // bindless slot of the composed texture, kMatNoTex = none composed (inherited)
    std::uint32_t ss[native::sc::kStages][7]{};              // the composed sampler states
    std::uint32_t blendMode = 0;                             // the blend mode the composition used (blendModeOf)
    bool defaultClass = true;                                // eCShaderDefault: the pool is c232.. (VS) / c6..c13 (PS)
    bool checkBlend = true;                                  // false: an override (blend / mask reference) is active for this material
    std::int32_t maskRef = -1;                               // the mask reference the composition used, -1 = none
    std::uint32_t defFilter = 0;                             // the gfx admin's default filter (substituted for kEngineNoState)
};

struct MaterialDiff {
    std::uint32_t regsCompared = 0, regsDiffer = 0, regsSwitch = 0;  // registers; regsSwitch = skipped as switch-written
    std::uint32_t texCompared = 0, texDiffer = 0, stateCompared = 0, stateDiffer = 0, texUnresolved = 0;
    std::uint32_t blendCompared = 0, blendDiffer = 0;
    int firstReg = -1;       // first differing register (PS: +1000)
    int firstStage = -1;     // first differing stage
    int firstSsWord = -1;
    bool differs() const { return regsDiffer + texDiffer + stateDiffer + blendDiffer != 0; }
};

// the material's expectation compared with the draw: registers, texture bindings, sampler states, blend mode / mask reference
inline MaterialDiff diffMaterial(const MaterialRecord& r, const MaterialDraw& d) {
    namespace sc = native::sc;
    MaterialDiff out;
    const std::uint32_t swVs = r.sw[0], swPs = r.sw[1] & 0xffffu, swSlots = r.sw[1] >> 16;
    // VS pool registers (default class: only those from c232, the lower ones belong to the admin / global pools)
    for (std::uint32_t i = 0; i < r.counts[1] && i < kMatVsRegs; ++i) {
        const std::uint32_t reg = r.counts[0] + i;
        if (reg >= sc::kVsRegs || !(*d.vsLive)[reg]) continue;
        if (d.defaultClass && reg < 232) continue;
        if (swVs >> i & 1u) {
            ++out.regsSwitch;
            continue;
        }
        ++out.regsCompared;
        if (std::memcmp(r.vs[i], d.vsF[reg], 16) != 0) {
            ++out.regsDiffer;
            if (out.firstReg < 0) out.firstReg = static_cast<int>(reg);
        }
    }
    // PS pool registers (default class: c6..c13; the admin owns c0..c5)
    for (std::uint32_t i = 0; i < r.counts[3] && i < kMatPsRegs; ++i) {
        const std::uint32_t reg = r.counts[2] + i;
        if (reg >= sc::kPsRegs || !(*d.psLive)[reg] || reg < 6) continue;
        if (d.defaultClass && reg >= 14) continue;
        if (swPs >> i & 1u) {
            ++out.regsSwitch;
            continue;
        }
        ++out.regsCompared;
        if (std::memcmp(r.ps[i], d.psF[reg], 16) != 0) {
            ++out.regsDiffer;
            if (out.firstReg < 0) out.firstReg = 1000 + static_cast<int>(reg);
        }
    }
    // sampler pool slot k -> stage base + k when the pixel shader samples it
    const std::uint32_t base = matBaseStage(r), n = std::min(matSlotCount(r), kMatSlots);
    if ((r.info[3] & MatSlotsOverflow) == 0) {
        for (std::uint32_t k = 0; k < n; ++k) {
            const std::uint32_t st = base + k;
            if (st >= sc::kStages || (d.samplerMask >> st & 1u) == 0) continue;
            if (swSlots >> k & 1u) continue;
            const auto& s = r.slots[k];
            if (d.texSlot[st] != kMatNoTex) {
                if (s.tex == kMatNoTex) {
                    ++out.texUnresolved;
                } else {
                    ++out.texCompared;
                    if (s.tex != d.texSlot[st]) {
                        ++out.texDiffer;
                        if (out.firstStage < 0) out.firstStage = static_cast<int>(st);
                    }
                }
            }
            std::uint32_t want[7];
            for (std::uint32_t i = 0; i < 6; ++i) want[i] = s.raw[i] == kEngineNoState ? d.defFilter : s.raw[i];
            want[6] = s.srgb;
            ++out.stateCompared;
            for (std::uint32_t i = 0; i < 7; ++i) {
                if (want[i] != d.ss[st][i]) {
                    ++out.stateDiffer;
                    if (out.firstStage < 0) out.firstStage = static_cast<int>(st);
                    if (out.firstSsWord < 0) out.firstSsWord = static_cast<int>(i);
                    break;
                }
            }
        }
    }
    if (d.checkBlend) {
        ++out.blendCompared;
        bool bad = static_cast<std::uint32_t>(d.blendMode) != r.state[0];
        if (d.maskRef >= 0 && static_cast<std::uint32_t>(d.maskRef) != r.state[1]) bad = true;
        if (bad) ++out.blendDiffer;
    }
    return out;
}

// The record's engine-derived part from raw bytes: `img` = the shader object's bytes [0, kMatRawBytes), `vsPool` / `psPool` = its pool registers (counts
// from the object, at most 32 / 16 registers), `texOf(stage, imageResource)` = the composition's texture resolution (bindless slot or kMatNoTex).
// Keeps info.x / .y / .z, the graph ids, the switch masks and the double-sided flag (set from the requests).
template <class TexFn>
inline void fillMaterialRecord(MaterialRecord& r, const std::uint8_t* img, const float (*vsPool)[4], const float (*psPool)[4], TexFn&& texOf) {
    const auto u32 = [&](std::uint32_t off) {
        std::uint32_t v;
        std::memcpy(&v, img + off, 4);
        return v;
    };
    const auto ptrAt = [&](std::uint32_t off) {
        const void* v;
        std::memcpy(&v, img + off, 8);
        return v;
    };
    const std::uint32_t keep = r.info[3] & MatDoubleSided;
    r.counts[0] = u32(0x9c);
    r.counts[1] = std::min<std::uint32_t>(img[0xc1], kMatVsRegs);
    r.counts[2] = u32(0xcc);
    r.counts[3] = std::min<std::uint32_t>(img[0xf1], kMatPsRegs);
    r.state[0] = u32(0x50);
    r.state[1] = img[0x58];
    const std::uint32_t slots = img[0x21b];
    r.state[2] = img[0x219] | std::min<std::uint32_t>(slots, 16u) << 8;
    r.state[3] = 0;
    std::uint32_t flags = keep;
    if (img[0x330] != 0) flags |= MatHasOpacity;
    if (img[0x420] != 0) flags |= MatRefracted;
    if (img[0x79] != 0) flags |= MatTradeAlpha;
    if (img[0x2d8] != 0) flags |= MatZFill;
    if (img[0x78] != 0) flags |= MatDepthBias;
    if (u32(0x250) != 0 && static_cast<std::int32_t>(u32(0x250)) > 0) flags |= MatHasSwitch;
    if (slots > kMatSlots) flags |= MatSlotsOverflow;
    r.info[3] = flags;
    std::memset(r.vs, 0, sizeof(r.vs));
    std::memset(r.ps, 0, sizeof(r.ps));
    if (r.counts[1] != 0) std::memcpy(r.vs, vsPool, r.counts[1] * 16u);
    if (r.counts[3] != 0) std::memcpy(r.ps, psPool, r.counts[3] * 16u);
    for (std::uint32_t k = 0; k < kMatSlots; ++k) {
        MaterialRecord::Slot& s = r.slots[k];
        std::memset(&s, 0, sizeof(s));
        s.tex = kMatNoTex;
        if (k >= slots) continue;
        const std::uint32_t o = 0xf8 + 0x28 * k;
        s.raw[0] = u32(o);
        s.raw[1] = u32(o + 4);
        s.raw[2] = u32(o + 0xc);
        s.raw[3] = u32(o + 8);
        s.raw[4] = u32(o + 0x10);
        s.raw[5] = u32(o + 0x18);
        s.srgb = static_cast<std::uint32_t>(static_cast<std::int32_t>(static_cast<std::int8_t>(img[o + 0x14])));
        s.tex = texOf(static_cast<std::uint32_t>(img[0x219]) + k, ptrAt(o + 0x20));
    }
}

}  // namespace rvk::gpuscene
