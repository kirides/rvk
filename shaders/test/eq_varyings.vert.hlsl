// Pass-through vertex shader of rvk_shadereq: the VS constants c[loc] are the varyings at location loc (0..7, 16..17), one
// full-screen triangle. Constant across the triangle, so every pixel of both compared fragment shaders sees the same inputs.
// With RVK_SCENE_RECORD (rvk_shadereq --scene / --scene2, S-1a) the constants come through the scene draw's register windows (record 0, view slot 0) and the
// record index leaves as the nointerpolation varying VARY14 that the scene form of the pixel shaders reads.

#include "rvk_draw.hlsli"

// Locations 0..7 and 16..17: explicit [[vk::location]] (the numbering by declaration order would leave no gap 8..15)
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
#ifdef RVK_SCENE_RECORD
    [[vk::location(14)]] nointerpolation uint o14 : TEXCOORD14;
#endif
    [[vk::location(16)]] float4 o16 : TEXCOORD16;
    [[vk::location(17)]] float4 o17 : TEXCOORD17;
};

VSOut VSMain(uint vertexId : SV_VertexID) {
    VSOut o;
#ifdef RVK_SCENE_RECORD
    rvkSceneBeginVs(uint2(0, 0));
    o.o0 = RVK_VSC(0);
    o.o1 = RVK_VSC(1);
    o.o2 = RVK_VSC(2);
    o.o3 = RVK_VSC(3);
    o.o4 = RVK_VSC(4);
    o.o5 = RVK_VSC(5);
    o.o6 = RVK_VSC(6);
    o.o7 = RVK_VSC(7);
    o.o14 = rvk_sceneIndex;
    o.o16 = RVK_VSC(16);
    o.o17 = RVK_VSC(17);
#else
    RvkStageData vs = rvkPtr<RvkStageData, 16>(pc.vs).Get();
    o.o0 = vs.c[0];
    o.o1 = vs.c[1];
    o.o2 = vs.c[2];
    o.o3 = vs.c[3];
    o.o4 = vs.c[4];
    o.o5 = vs.c[5];
    o.o6 = vs.c[6];
    o.o7 = vs.c[7];
    o.o16 = vs.c[16];
    o.o17 = vs.c[17];
#endif
    int vi = int(vertexId);
    float2 p = float2(float((vi << 1) & 2), float(vi & 2));
    o.position = float4(p * 2.0 - 1.0, 0.0, 1.0);
    return o;
}
