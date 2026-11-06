// Test shader of the MeshPart table (src/gpuscene/mesh_parts.*): thread i takes the case `cases[i]` = (part slot, vertex index, k, 0) and fetches through
// shaders/include/rvk_mesh.hlsli: the k-th index of the part's range, and the vertex's position / normal / uv0 / colour 0. Writes 5 uint4 per case
// (index, position, normal, uv0, colour as raw float bits); the CPU compares with its own decode of the same bytes.

#include "rvk_mesh.hlsli"

struct MeshFetchPush {
    uint2 parts;    // MeshPart table
    uint2 layouts;  // Layout table
    uint2 cases;    // read: uint4 x count
    uint2 dst;      // write: 80 B per case
    uint count;
    uint pad0;
    uint pad1;
    uint pad2;
};
RVK_PUSH(MeshFetchPush, pc);

[numthreads(64, 1, 1)]
void CSMain(uint3 id : SV_DispatchThreadID) {
    if (id.x >= pc.count) return;
    const uint4 c = rvkRawLoad<uint4, 16>(rvkAddrAdd(rvkAddr(pc.cases), id.x * 16u));
    const RvkMeshPart part = rvkMeshLoadPart(pc.parts, c.x);
    const RvkMeshLayout layout = rvkMeshLoadLayout(pc.layouts, part.info.y);
    const uint index = rvkMeshFetchIndex(part, c.z);
    const float4 pos = float4(rvkMeshFetchPosition(part, layout, c.y), 1.0);
    const float4 nrm = float4(rvkMeshFetchNormal(part, layout, c.y), 1.0);
    const float4 uv = float4(rvkMeshFetchUv(part, layout, 0u, c.y), 0.0, 1.0);
    const float4 col = rvkMeshFetch(part, layout, RVK_MESH_SLOT_COLOR0, c.y);
    const RvkAddr o = rvkAddrAdd(rvkAddr(pc.dst), id.x * 80u);
    rvkRwStore<uint4, 16>(o, uint4(index, 1u, 0u, 0u));
    rvkRwStore<float4, 16>(rvkAddrAdd(o, 16u), pos);
    rvkRwStore<float4, 16>(rvkAddrAdd(o, 32u), nrm);
    rvkRwStore<float4, 16>(rvkAddrAdd(o, 48u), uv);
    rvkRwStore<float4, 16>(rvkAddrAdd(o, 64u), col);
}
