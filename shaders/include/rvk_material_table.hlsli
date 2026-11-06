// The material table (src/gpuscene/material_table.*, material_compose.h; frame-ownership.md F0 / 1.2 row Material): one 1104-byte record per engine
// material as the native composition draws it. The GPU side of what StaticOps::constantsInto / materialSamplers take from the material: the VS pool
// registers (c232.., g_vShaderData), the PS pool registers (c6.., g_ShaderData), the sampler slots (bindless texture + the engine's sampler-state words),
// mask reference / blend mode / flags / graph ids. Raw word reads from address pairs (S1 rules): float4 / uint4 only, no typed block arrays.
#pragma once

#include "rvk_common.hlsli"

static const uint RVK_MAT_STRIDE = 1104u;
static const uint RVK_MAT_COUNTS = 16u;
static const uint RVK_MAT_STATE = 32u;
static const uint RVK_MAT_GRAPH = 48u;
static const uint RVK_MAT_SWITCH = 64u;
static const uint RVK_MAT_VS = 80u;     // 32 x float4
static const uint RVK_MAT_PS = 592u;    // 16 x float4
static const uint RVK_MAT_SLOTS = 848u; // 8 x 32 bytes: tex, 6 raw sampler words, sRGB
static const uint RVK_MAT_SLOT_STRIDE = 32u;
static const uint RVK_MAT_NO_TEX = 0xffffffffu;
static const uint RVK_ENGINE_NO_STATE = 0xABABABABu;  // a sampler slot word meaning "the gfx admin's default filter"

static const uint RVK_MAT_FLAG_HAS_OPACITY = 1u;
static const uint RVK_MAT_FLAG_REFRACTED = 2u;
static const uint RVK_MAT_FLAG_TRADE_ALPHA = 4u;
static const uint RVK_MAT_FLAG_ZFILL = 8u;
static const uint RVK_MAT_FLAG_DEPTH_BIAS = 16u;
static const uint RVK_MAT_FLAG_DOUBLE_SIDED = 32u;
static const uint RVK_MAT_FLAG_HAS_SWITCH = 64u;
static const uint RVK_MAT_FLAG_SLOTS_OVERFLOW = 128u;

RvkAddr rvkMaterialRecord(uint2 table, uint slot) { return rvkAddrAdd(rvkAddr(table), slot * RVK_MAT_STRIDE); }

uint4 rvkMaterialInfo(RvkAddr rec) { return rvkRawLoad<uint4, 16>(rec); }  // x live, y version, z pack word, w flags
uint4 rvkMaterialCounts(RvkAddr rec) { return rvkRawLoad<uint4, 16>(rvkAddrAdd(rec, RVK_MAT_COUNTS)); }  // vsFirst, vsCount, psFirst, psCount
uint4 rvkMaterialState(RvkAddr rec) { return rvkRawLoad<uint4, 16>(rvkAddrAdd(rec, RVK_MAT_STATE)); }   // blend, maskRef, base stage | slots << 8, 0
uint4 rvkMaterialGraph(RvkAddr rec) { return rvkRawLoad<uint4, 16>(rvkAddrAdd(rec, RVK_MAT_GRAPH)); }   // PS_Default id, PS_DefaultZPass id (0 = not seen), 0, 0

// register c(vsFirst + i) / c(psFirst + i) of the material's pools
float4 rvkMaterialVsReg(RvkAddr rec, uint i) { return rvkRawLoad<float4, 16>(rvkAddrAdd(rec, RVK_MAT_VS + i * 16u)); }
float4 rvkMaterialPsReg(RvkAddr rec, uint i) { return rvkRawLoad<float4, 16>(rvkAddrAdd(rec, RVK_MAT_PS + i * 16u)); }

// sampler pool slot k: the bindless texture slot and the composed sampler states (raw words with the default filter substituted)
uint rvkMaterialTexSlot(RvkAddr rec, uint k) { return rvkRawLoad<uint, 4>(rvkAddrAdd(rec, RVK_MAT_SLOTS + k * RVK_MAT_SLOT_STRIDE)); }
uint rvkMaterialSamplerWord(RvkAddr rec, uint k, uint i, uint defFilter) {  // i 0..5 = ADDRESSU, ADDRESSV, MAGFILTER, MINFILTER, MIPFILTER, MIPMAPLODBIAS; 6 = sRGB
    const uint w = rvkRawLoad<uint, 4>(rvkAddrAdd(rec, RVK_MAT_SLOTS + k * RVK_MAT_SLOT_STRIDE + 4u + i * 4u));
    return (i < 6u && w == RVK_ENGINE_NO_STATE) ? defFilter : w;
}
