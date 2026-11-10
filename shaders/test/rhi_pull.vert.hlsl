// RHI backend test (tools/rhitest "vertex pulling = the input assembler (S-4)"): every input format the pulled scene vertex shaders read (shaders/include/rvk_pull.hlsli,
// frame-ownership.md 14.12) fetched twice per vertex: by the input assembler (attribute k at location k) and by rvkPullAttr from the same buffer through a VertexFetch
// block. Both values' bits go to a written buffer (per vertex 13 x 2 uint4); the CPU compares them bit for bit. Nothing is rasterised (the position is outside the clip
// volume).
#include "rvk_common.hlsli"
#include "rvk_pull.hlsli"

struct PullTestPush {
    uint2 fetch;  // the VertexFetch block (raw)
    uint2 dst;    // written: per vertex 26 uint4 (13 input-assembler values, then 13 pulled ones)
    uint count;   // vertices
    uint pad0;
    uint pad1;
    uint pad2;
};
RVK_PUSH(PullTestPush, pc);

struct VsIn {
    [[vk::location(0)]] float4 a0 : ATTR0;
    [[vk::location(1)]] float4 a1 : ATTR1;
    [[vk::location(2)]] float4 a2 : ATTR2;
    [[vk::location(3)]] float4 a3 : ATTR3;
    [[vk::location(4)]] float4 a4 : ATTR4;
    [[vk::location(5)]] float4 a5 : ATTR5;
    [[vk::location(6)]] float4 a6 : ATTR6;
    [[vk::location(7)]] float4 a7 : ATTR7;
    [[vk::location(8)]] float4 a8 : ATTR8;
    [[vk::location(9)]] float4 a9 : ATTR9;
    [[vk::location(10)]] float4 a10 : ATTR10;
    [[vk::location(11)]] float4 a11 : ATTR11;
    [[vk::location(12)]] float4 a12 : ATTR12;
    uint vid : SV_VertexID;
};
struct VsOut {
    float4 position : SV_Position;
    float4 col : TEXCOORD0;
};

void put(uint vid, uint k, float4 ia) {
    const float4 pulled = rvkPullAttr(rvkPullBegin(rvkAddr(pc.fetch), vid), k);
    const RvkAddr o = rvkAddr(pc.dst);
    rvkRwStore<uint4, 16>(rvkAddrAdd(o, (vid * 26u + k) * 16u), asuint(ia));
    rvkRwStore<uint4, 16>(rvkAddrAdd(o, (vid * 26u + 13u + k) * 16u), asuint(pulled));
}

VsOut VSMain(VsIn i) {
    if (i.vid < pc.count) {
        put(i.vid, 0u, i.a0);
        put(i.vid, 1u, i.a1);
        put(i.vid, 2u, i.a2);
        put(i.vid, 3u, i.a3);
        put(i.vid, 4u, i.a4);
        put(i.vid, 5u, i.a5);
        put(i.vid, 6u, i.a6);
        put(i.vid, 7u, i.a7);
        put(i.vid, 8u, i.a8);
        put(i.vid, 9u, i.a9);
        put(i.vid, 10u, i.a10);
        put(i.vid, 11u, i.a11);
        put(i.vid, 12u, i.a12);
    }
    VsOut o;
    o.position = float4(4.0, 4.0, 0.5, 1.0);  // (outside the clip volume: nothing is rasterised)
    o.col = float4(1.0, 1.0, 1.0, 1.0);
    return o;
}
