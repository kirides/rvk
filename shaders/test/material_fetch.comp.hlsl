// Test shader of the material table (src/gpuscene/material_table.*): thread i reads case `cases[i]` = (material slot, vs register index, ps register index,
// sampler slot) from the GPU records and writes the VS / PS register, the bindless slot and the sampler words (a GPU twin of what gpuscene::diffMaterial
// compares; keeps shaders/include/rvk_material_table.hlsli compiled: embedded + dxil-check).

#include "rvk_material_table.hlsli"

struct MaterialTestPush {
    uint2 table;  // Buffer::shaderRef of the MaterialRecord table
    uint2 cases;  // read: uint4 x count: x material slot, y vs reg, z ps reg, w sampler slot
    uint2 dst;    // write: per case 4 x uint4: vs reg, ps reg, (tex slot, version, flags, graph), (u, v, mag, min)
    uint count;
    uint defFilter;
};
RVK_PUSH(MaterialTestPush, pc);

[numthreads(64, 1, 1)]
void CSMain(uint3 id : SV_DispatchThreadID) {
    if (id.x >= pc.count) return;
    const uint4 c = rvkRawLoad<uint4, 16>(rvkAddrAdd(rvkAddr(pc.cases), id.x * 16u));
    const RvkAddr rec = rvkMaterialRecord(pc.table, c.x);
    const uint base = id.x * 64u;
    const float4 vs = rvkMaterialVsReg(rec, c.y);
    const float4 ps = rvkMaterialPsReg(rec, c.z);
    const uint4 info = rvkMaterialInfo(rec);
    const uint4 graph = rvkMaterialGraph(rec);
    rvkRwStore<float4, 16>(rvkAddrAdd(rvkAddr(pc.dst), base), vs);
    rvkRwStore<float4, 16>(rvkAddrAdd(rvkAddr(pc.dst), base + 16u), ps);
    rvkRwStore<uint4, 16>(rvkAddrAdd(rvkAddr(pc.dst), base + 32u), uint4(rvkMaterialTexSlot(rec, c.w), info.y, info.w, graph.x));
    rvkRwStore<uint4, 16>(rvkAddrAdd(rvkAddr(pc.dst), base + 48u),
                          uint4(rvkMaterialSamplerWord(rec, c.w, 0u, pc.defFilter), rvkMaterialSamplerWord(rec, c.w, 1u, pc.defFilter),
                                rvkMaterialSamplerWord(rec, c.w, 2u, pc.defFilter), rvkMaterialSamplerWord(rec, c.w, 3u, pc.defFilter)));
}
