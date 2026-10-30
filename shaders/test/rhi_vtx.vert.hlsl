// RHI backend test (tools/rhitest): vertex input from two bindings, one per vertex (position, colour) and one per instance (a shift). Location N is
// semantic ATTR<N> (the convention of the D3D9 translator, src/shader/hlsl.cpp RvkVsIn).

struct VsIn {
    [[vk::location(0)]] float2 pos : ATTR0;
    [[vk::location(1)]] float4 col : ATTR1;
    [[vk::location(2)]] float2 shift : ATTR2;
};

struct VsOut {
    float4 position : SV_Position;
    float4 col : TEXCOORD0;
};

VsOut VSMain(VsIn i) {
    VsOut o;
    o.position = float4(i.pos + i.shift, 0.0, 1.0);
    o.col = i.col;
    return o;
}
