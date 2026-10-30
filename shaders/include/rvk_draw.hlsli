// The draw ABI in HLSL: the push block and the blocks of draws that go through the D3D9 model (translated D3D9 shaders, the material
// shaders, the shaders of draw replacers: water, shadow mask). Mirror of abi.h PushConstants / DrawBlock / StageConsts and of
// rvk_abi.glsl's RvkPush; pass shaders (compute, injected passes) do NOT include this: they declare their own push block.
// Every address is a (low, high) uint pair so that the push block needs no 64-bit types of its own; reach the data with rvkPtr.
#pragma once

#include "rvk_common.hlsli"

// D3D9 constant registers of one stage (StageConsts, 4416 bytes): c at 0, i at 4096, b at 4352
struct RvkStageData {
    float4 c[256];
    int4 i[16];
    uint4 b[4];  // 16 bool registers as uints: b[n >> 2][n & 3]
};

// Per-draw state (DrawBlock, 208 bytes): bindless texture / sampler indices of D3D9 samplers s0..s15 (pixel) and 16..19 (vertex s0..s3),
// the half-pixel fix, the alpha test; alpha.zw = pad0 (debug tint, RGBA8, 0 = none) and pad1
struct RvkDrawData {
    uint4 tex[5];    // tex[i >> 2][i & 3]
    uint4 smp[5];    // smp[i >> 2][i & 3]
    float4 posFix;
    uint4 alpha;     // x alphaFunc, y alphaRef (float bits), z pad0, w pad1
    float4 psC5;     // pixel shader c5 of the draw (split constants: the pixel window of register 5)
};

// Push constants of draws (112 bytes, vertex + fragment). drawId: the draw's index in the frame; flags: RVK_DRAW_* below. prevVs: a vertex
// capture address (draw variants; 0 = none). vs / ps: the draw's one full StageConsts (legacy draws; 0 for split draws). vsw / psw: the float
// constants by register window (abi.h kVsWindowFirst / kPsWindowFirst)
// (pointers are uint2 pairs: a struct member of a DX12 root-constant block would start on a 16-byte boundary, and so would the elements of an array
// of uint2: the windows are four members each; rvkPtr<T, A>(pair) / rvkLoadFrame(pc.frame) make the references)
struct RvkDrawPush {
    uint2 vs;
    uint2 ps;
    uint2 draw;   // a replacer's own block (water, shadow mask) can live here instead of a DrawBlock
    uint2 frame;  // the FrameBlock (RvkFrameData)
    uint2 prevVs;
    uint drawId;
    uint flags;
    uint2 vsw0;
    uint2 vsw1;
    uint2 vsw2;
    uint2 vsw3;
    uint2 psw0;
    uint2 psw1;
    uint2 psw2;
    uint2 psw3;
};
RVK_PUSH(RvkDrawPush, pc);

// the window block of window index w (a constant after folding in every shader that reads a constant register)
uint2 rvkVsWin(uint w) { return w == 0u ? pc.vsw0 : w == 1u ? pc.vsw1 : w == 2u ? pc.vsw2 : pc.vsw3; }
uint2 rvkPsWin(uint w) { return w == 0u ? pc.psw0 : w == 1u ? pc.psw1 : w == 2u ? pc.psw2 : pc.psw3; }

// RvkDrawPush.flags: bits 0-7 the draw kind (d3d9/draws.h Kind), 8-11 the pass (scene.h Pass), 12-15 the view (scene.h View), 16.. feature bits
static const uint RVK_DRAW_KIND_MASK = 0xffu;
static const uint RVK_DRAW_PASS_SHIFT = 8u;
static const uint RVK_DRAW_VIEW_SHIFT = 12u;
static const uint RVK_DRAW_TINTED = 0x10000u;

// float constant register n of the vertex / pixel stage (n: an int / uint expression; a constant folds to one window)
#define RVK_VS_WIN(n) ((uint(n)) < 12u ? 0 : (uint(n)) < 31u ? 1 : (uint(n)) < 232u ? 2 : 3)
#define RVK_PS_WIN(n) ((uint(n)) < 5u ? 0 : (uint(n)) < 6u ? 1 : (uint(n)) < 14u ? 2 : 3)
// (offset of c[n] in StageConsts = n * 16: a float4 load from the window's block)
#define RVK_VSC(n) rvkRawLoad<float4, 16>(rvkAddrAdd(rvkAddr(rvkVsWin(RVK_VS_WIN(n))), uint(n) * 16u))
#define RVK_PSC(n) rvkRawLoad<float4, 16>(rvkAddrAdd(rvkAddr(rvkPsWin(RVK_PS_WIN(n))), uint(n) * 16u))

// the draw's DrawBlock (not for replacers that put their own block in pc.draw)
RvkDrawData rvkLoadDraw() { return rvkPtr<RvkDrawData, 16>(pc.draw).Get(); }
uint rvkDrawTex(RvkDrawData d, uint i) { return d.tex[i >> 2][i & 3u]; }
uint rvkDrawSmp(RvkDrawData d, uint i) { return d.smp[i >> 2][i & 3u]; }
