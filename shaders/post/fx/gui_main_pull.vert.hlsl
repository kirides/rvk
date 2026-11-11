// F13 owned records (docs/research/frame-ownership.md 14.34, native.own_gui = owned): the gui effect's vertex shader #0 (gui_main.vert.hlsl, the port of the
// sprite effect) with its vertex fetch replaced by the sprite record the input assembler pulls at instance rate. Instance i = record i of a run of the flush (the
// engine's eCSprite records in their queue order), vertex 0..3 = the quad's corner (the engine's index order 0 2 3 0 1 2: eCSpriteMesh::CreateIndexBuffer). The
// corner is assembled exactly as RenderBatches writes the quad's vertices (native/effect_compose.cpp fxc::spriteQuad: x / y of corner +0x00 / +0x08 / +0x18 /
// +0x10, z +0x30, colour +0x40, u +0x20 / +0x28, v +0x24 / +0x2c) and the program below is gui_main.vert's, unchanged (the translator's shape: registers as
// static globals, the program in its own function, so DXC keeps the clip position bit-equal: 14.24). The record's texture is its bindless slot (the batch's
// image as the device would bind it), passed to gui_main_pull.frag without interpolation.
// Record (d3d9 instance stream, EffectOps::kGuiRecordStride = 0x40; native/effect_compose.h GuiRecord):
//   TEXCOORD8  = corners 0 (x, y) and 1 (x, y)        (the engine record's +0x00 .. +0x0f)
//   TEXCOORD9  = the corners at +0x10 and +0x18          (+0x10 .. +0x1f: vertex 3, vertex 2)
//   TEXCOORD10 = u0 v0 u1 v1                             (+0x20 .. +0x2f)
//   TEXCOORD11 = z, the texture's bindless slot (float)  (+0x30, -)
//   TEXCOORD12 = the colour (D3DCOLOR, as the engine's vertex: the same input-assembler conversion)
#include "post_port.hlsli"

struct RvkVsIn {
    [[vk::location(8)]] float4 r0 : ATTR8;
    [[vk::location(9)]] float4 r1 : ATTR9;
    [[vk::location(10)]] float4 r2 : ATTR10;
    [[vk::location(11)]] float4 r3 : ATTR11;
    [[vk::location(12)]] float4 r4 : ATTR12;
    uint rvk_vertexId : SV_VertexID;
    uint rvk_instanceId : SV_InstanceID;
};
struct RvkVsOut {
    float4 position : SV_Position;
    [[vk::location(0)]] float4 rvk_out0 : VARY0;
    [[vk::location(1)]] float4 rvk_out16 : VARY1;
    [[vk::location(2)]] nointerpolation uint4 rvk_slot : VARY2;
};

static RvkDrawData rvk_draw;
static float4 v0;
static float4 v1;
static float4 v2;
static float4 oPos = float4(0.0, 0.0, 0.0, 0.0);
static float4 oD0 = float4(0.0, 0.0, 0.0, 0.0);
static float4 oT0 = float4(0.0, 0.0, 0.0, 0.0);

void rvk_program() {
    oPos.x = ((float4)(dot(v0, RVK_VSC(0)))).x;
    oPos.y = ((float4)(dot(v0, RVK_VSC(1)))).y;
    oPos.z = ((float4)(dot(v0, RVK_VSC(2)))).z;
    oPos.w = ((float4)(dot(v0, RVK_VSC(3)))).w;
    oD0 = v1;
    oT0.xy = v2.xy;
}

RvkVsOut VSMain(RvkVsIn rvk_in) {
    rvk_draw = rvkLoadDraw();
    RvkVsOut rvk_out = (RvkVsOut)0;
    // the corner (the input assembler's FLOAT3 position: w = 1; FLOAT2 uv: (u, v, 0, 1))
    const uint c = rvk_in.rvk_vertexId & 3u;
    const float2 xy = c == 0u ? rvk_in.r0.xy : c == 1u ? rvk_in.r0.zw : c == 2u ? rvk_in.r1.zw : rvk_in.r1.xy;
    const float u = (c == 1u || c == 2u) ? rvk_in.r2.z : rvk_in.r2.x;
    const float v = c >= 2u ? rvk_in.r2.w : rvk_in.r2.y;
    v0 = float4(xy, rvk_in.r3.x, 1.0);
    v1 = rvk_in.r4;
    v2 = float4(u, v, 0.0, 1.0);
    rvk_program();
    rvk_out.rvk_out16 = oD0;
    rvk_out.rvk_out0 = oT0;
    rvk_out.rvk_slot = uint4((uint)rvk_in.r3.y, 0u, 0u, 0u);
    precise float4 rvk_pos = oPos;
    rvk_pos.xy += rvk_draw.posFix.xy * rvk_pos.w;
    rvk_pos.z += rvk_draw.posFix.z * rvk_pos.z + rvk_draw.posFix.w * rvk_pos.w;
    rvk_out.position = rvk_pos;
    return rvk_out;
}
