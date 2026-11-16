// Vertex shader of rvk_shadereq --gpl (the pipeline library vs monolithic build diff, frame-ownership.md 16.27): like eq_varyings, but the varyings
// vary across the big triangle (so quad derivatives, LODs and interpolation matter): o<loc> = c[loc] * (0.6 + 0.4 * (p.x * c[loc + 32].x + p.y * c[loc + 32].y)).

#include "rvk_draw.hlsli"

struct VSOut {
    float4 position : SV_Position;
    [[vk::location(0)]] float4 o0 : TEXCOORD0;
    [[vk::location(1)]] float4 o1 : TEXCOORD1;
    [[vk::location(2)]] float4 o2 : TEXCOORD2;
    [[vk::location(3)]] float4 o3 : TEXCOORD3;
    [[vk::location(4)]] float4 o4 : TEXCOORD4;
    [[vk::location(5)]] float4 o5 : TEXCOORD5;
    [[vk::location(6)]] float4 o6 : TEXCOORD6;
    [[vk::location(7)]] float4 o7 : TEXCOORD7;
    [[vk::location(16)]] float4 o16 : TEXCOORD16;
    [[vk::location(17)]] float4 o17 : TEXCOORD17;
};

VSOut VSMain(uint vertexId : SV_VertexID) {
    VSOut o;
    RvkStageData vs = rvkPtr<RvkStageData, 16>(pc.vs).Get();
    int vi = int(vertexId);
    float2 p = float2(float((vi << 1) & 2), float(vi & 2));
    float4 g;
    g = vs.c[32];  o.o0 = vs.c[0] * (0.6 + 0.4 * dot(p, g.xy));
    g = vs.c[33];  o.o1 = vs.c[1] * (0.6 + 0.4 * dot(p, g.xy));
    g = vs.c[34];  o.o2 = vs.c[2] * (0.6 + 0.4 * dot(p, g.xy));
    g = vs.c[35];  o.o3 = vs.c[3] * (0.6 + 0.4 * dot(p, g.xy));
    g = vs.c[36];  o.o4 = vs.c[4] * (0.6 + 0.4 * dot(p, g.xy));
    g = vs.c[37];  o.o5 = vs.c[5] * (0.6 + 0.4 * dot(p, g.xy));
    g = vs.c[38];  o.o6 = vs.c[6] * (0.6 + 0.4 * dot(p, g.xy));
    g = vs.c[39];  o.o7 = vs.c[7] * (0.6 + 0.4 * dot(p, g.xy));
    g = vs.c[48];  o.o16 = vs.c[16] * (0.6 + 0.4 * dot(p, g.xy));
    g = vs.c[49];  o.o17 = vs.c[17] * (0.6 + 0.4 * dot(p, g.xy));
    o.position = float4(p * 2.0 - 1.0, 0.0, 1.0);
    return o;
}
