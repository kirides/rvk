// RHI backend test (tools/rhitest): the vertex shader of the indirect draw check. The per-draw data is a uint2 `ref` of an instance-rate stream
// (the record's firstInstance offsets it on both backends); the positions are pulled by address from a raw buffer by SV_VertexID = the index value.
// Observed: Vulkan's VertexIndex includes the record's vertexOffset (base vertex), D3D12's SV_VertexID does NOT, so the records use vertexOffset 0 and
// absolute indices (the design of frame-ownership.md 4). SV_InstanceID is not used either: Vulkan's InstanceIndex includes firstInstance, D3D12's does not.
#include "rvk_common.hlsli"

struct PullPush {
    uint2 positions;  // Buffer::shaderRef of the float2 template positions
    uint pad0;
    uint pad1;
};
RVK_PUSH(PullPush, pc);

struct VsIn {
    [[vk::location(0)]] uint2 ref : ATTR0;  // (tile, colour id)
    uint vid : SV_VertexID;
};
struct VsOut {
    float4 position : SV_Position;
    float4 col : TEXCOORD0;
};

VsOut VSMain(VsIn i) {
    const float2 local = rvkRawLoad<float2, 4>(rvkAddrAdd(rvkAddr(pc.positions), i.vid * 8u));
    // 4 x 4 tiles of half 0.25 (NDC), row 0 at the bottom
    const float2 centre = (float2(float(i.ref.x & 3u), float(i.ref.x >> 2u)) + 0.5) * 0.5 - 1.0;
    VsOut o;
    o.position = float4(centre + local * 0.25, 0.0, 1.0);
    const uint id = i.ref.y;
    o.col = float4(float((id * 53u) & 255u), float((id * 97u + 50u) & 255u), float((id * 29u + 120u) & 255u), 255.0) / 255.0;
    return o;
}
