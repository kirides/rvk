// F13 owned records (docs/research/frame-ownership.md 14.34, native.own_gui = owned): the gui effect's pixel shader #1 (gui_main.frag.hlsl) for the pulled
// sprite records of gui_main_pull.vert.hlsl: the texture is the record's bindless slot (each record's batch image: one draw covers every batch of a run whatever
// its texture), the sampler the draw's stage 0 (the sprite effect's pass sets it once per flush). The rest is gui_main.frag's program.
#include "post_port.hlsli"

struct RvkPsIn {
    float4 rvk_fragCoord : SV_Position;
    [[vk::location(0)]] float4 t0 : VARY0;
    [[vk::location(1)]] float4 v0 : VARY1;
    [[vk::location(2)]] nointerpolation uint4 slot : VARY2;
};
struct RvkPsOut {
    float4 oC0 : SV_Target0;
};

RvkPsOut PSMain(RvkPsIn rvk_in) {
    const RvkDrawData rvk_draw = rvkLoadDraw();
    float4 rvk_fragCoord = (float4)0;
    float4 v0 = (float4)0;
    float4 t0 = (float4)0;
    float4 oC0 = float4(0.0, 0.0, 0.0, 0.0);
    float4 r0 = float4(0.0, 0.0, 0.0, 0.0);
    RvkPsOut rvk_out = (RvkPsOut)0;
    rvk_fragCoord = rvk_in.rvk_fragCoord;
    v0 = rvk_in.v0;
    t0 = rvk_in.t0;
    // ---- the program
    r0 = RVK_TEX2D(rvk_in.slot.x).Sample(RVK_SAMP_UNI(rvk_draw.smp[0][0]), t0.xy);
    r0 = r0 * v0;
    oC0 = r0;
    // ---- outputs
    if (!rvkPortAlphaTest(rvk_draw, rvk_fragCoord, oC0.a)) discard;
    rvk_out.oC0 = oC0;
    return rvk_out;
}
