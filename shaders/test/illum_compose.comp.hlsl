// Test shader of the illumination table (src/gpuscene/illumination_table.*): thread i composes the lighting registers c14 .. c37 of the case
// `cases[i]` = (illum slot, params) from the GPU records and writes 24 float4 to `out` (a GPU twin of gpuscene::composeLighting; a self-test
// reads it back and compares with the CPU function). Also keeps shaders/include/rvk_illum.hlsli compiled (embedded + dxil-check).

#include "rvk_illum.hlsli"

struct IllumTestPush {
    uint2 illums;  // Buffer::shaderRef of the IllumRecord table
    uint2 sets;    // ... of the LightSetRecord table
    uint2 cases;   // read: uint4 x count: x illum slot, y flags (bit 0 tableLit, bit 1 holdsAll), z n, w firstLight
    uint2 dst;     // write: per case 24 x float4, then one uint4 (x = composed ok)
    uint count;
    uint pad0;
    uint pad1;
    uint pad2;
};
RVK_PUSH(IllumTestPush, pc);

[numthreads(64, 1, 1)]
void CSMain(uint3 id : SV_DispatchThreadID) {
    if (id.x >= pc.count) return;
    const uint4 c = rvkRawLoad<uint4, 16>(rvkAddrAdd(rvkAddr(pc.cases), id.x * 16u));
    RvkIllumParams p;
    p.tableLit = c.y & 1u;
    p.holdsAll = (c.y >> 1u) & 1u;
    p.n = c.z;
    p.firstLight = c.w;
    float4 L[24];
    [unroll] for (uint k = 0u; k < 24u; ++k) L[k] = float4(0, 0, 0, 0);
    const bool ok = rvkIllumCompose(pc.illums, c.x, pc.sets, p, L);
    const uint base = id.x * 400u;  // 24 x 16 + 16
    [unroll] for (uint r = 0u; r < 24u; ++r) rvkRwStore<float4, 16>(rvkAddrAdd(rvkAddr(pc.dst), base + r * 16u), L[r]);
    rvkRwStore<uint4, 16>(rvkAddrAdd(rvkAddr(pc.dst), base + 384u), uint4(ok ? 1u : 0u, 0, 0, 0));
}
