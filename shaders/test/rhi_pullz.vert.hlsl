// RHI backend test (tools/rhitest testPullDepthOnly, frame-ownership.md 14.38): vertex-pulled depth-only draws in an ExecuteIndirect (count buffer) against the
// input assembler. The renderer's pulled scene vertex shaders (rvk_pull.hlsli) in miniature: the instance-rate ref stream gives the draw's slot, the slot's
// 128-byte block in a table holds the two vertex streams' references, strides, the base vertex and the mesh's placement; SV_VertexID (the index value, vertexOffset 0) + the
// base vertex addresses the vertex. VSIa* read the same vertices through the input assembler (the record's vertexOffset = the base vertex). Every form computes
// the position with the same expression from the same float bits: the depth images must be equal bit for bit.
#include "rvk_common.hlsli"

struct PullzPush {
    uint2 table;  // Buffer::shaderRef of the slot blocks (128 bytes each)
    uint pad0;
    uint pad1;
};
RVK_PUSH(PullzPush, pc);

struct Block {
    uint2 vb0, vb1;   // stream 0 (float3 position), stream 1 (float3 normal + float2 uv)
    uint stride0, stride1;
    int baseVertex;
    float4 xf0;  // cos, sin, scale, depth base
    float4 xf1;  // tx, ty, depth scale, w scale
};

Block loadBlock(uint slot) {
    const RvkAddr a = rvkAddrAdd(rvkAddr(pc.table), slot * 128u);
    Block b;
    const uint4 h0 = rvkRawLoad<uint4, 16>(a);
    const uint4 h1 = rvkRawLoad<uint4, 16>(rvkAddrAdd(a, 16u));
    b.vb0 = h0.xy;
    b.vb1 = h0.zw;
    b.stride0 = h1.x;
    b.stride1 = h1.y;
    b.baseVertex = asint(h1.z);
    b.xf0 = asfloat(rvkRawLoad<uint4, 16>(rvkAddrAdd(a, 32u)));
    b.xf1 = asfloat(rvkRawLoad<uint4, 16>(rvkAddrAdd(a, 48u)));
    return b;
}

float4 place(Block b, float3 p) {
    precise float2 r = float2(p.x * b.xf0.x - p.y * b.xf0.y, p.x * b.xf0.y + p.y * b.xf0.x) * b.xf0.z;
    precise float w = 1.0 + p.z * b.xf1.w;
    precise float z = b.xf0.w + p.z * b.xf1.z;
    return float4((r + b.xf1.xy) * w, z * w, w);
}

struct OutPos {
    float4 position : SV_Position;
};
struct OutFull {
    float4 position : SV_Position;
    float4 v0 : VARY0;
    float4 v1 : VARY1;
    float4 v2 : VARY2;
    float4 v3 : VARY3;
};

OutFull full(Block b, float3 p, float3 n, float2 uv, uint slot) {
    OutFull o;
    o.position = place(b, p);
    o.v0 = float4(n, 1.0);
    o.v1 = float4(uv, float(slot), 0.0);
    o.v2 = float4(p * 0.5 + 0.5, 1.0);
    o.v3 = b.xf0;
    return o;
}

struct PullIn {
    [[vk::location(0)]] uint2 ref : ATTR0;
    uint vid : SV_VertexID;
};

void pulled(PullIn i, out Block b, out float3 p, out float3 n, out float2 uv) {
    b = loadBlock(i.ref.x);
    const uint vtx = uint(int(i.vid) + b.baseVertex);
    p = asfloat(rvkRawLoad<uint3, 4>(rvkAddrAdd(rvkAddr(b.vb0), vtx * b.stride0)));
    const RvkAddr a = rvkAddrAdd(rvkAddr(b.vb1), vtx * b.stride1);
    n = asfloat(rvkRawLoad<uint3, 4>(a));
    uv = asfloat(rvkRawLoad<uint2, 4>(rvkAddrAdd(a, 12u)));
}

OutPos VSPulledPos(PullIn i) {
    Block b;
    float3 p, n;
    float2 uv;
    pulled(i, b, p, n, uv);
    OutPos o;
    o.position = place(b, p);
    return o;
}

OutFull VSPulledFull(PullIn i) {
    Block b;
    float3 p, n;
    float2 uv;
    pulled(i, b, p, n, uv);
    return full(b, p, n, uv, i.ref.x);
}

struct IaIn {
    [[vk::location(0)]] uint2 ref : ATTR0;
    [[vk::location(1)]] float3 p : ATTR1;
    [[vk::location(2)]] float3 n : ATTR2;
    [[vk::location(3)]] float2 uv : ATTR3;
};

OutFull VSIaFull(IaIn i) { return full(loadBlock(i.ref.x), i.p, i.n, i.uv, i.ref.x); }

// a z-pass pixel shader with a texkill and no colour output (the renderer's texkill z draws keep theirs); never kills here (v1.z = slot >= 0)
void PSKill(OutFull i) { clip(i.v1.z + 0.5); }
// the colour pass form: one R32F target
float PSColor(OutFull i) : SV_Target0 { return i.v1.z + i.v0.x * 0.001; }
