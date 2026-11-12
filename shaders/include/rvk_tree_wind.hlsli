// The SpeedTree wind / sway position math (ge_default_vs.fx Transform with XFX_SPEEDTREE, ported in shaders/material/ge_default.vert.hlsl), shared by
// shaders/rt/rt_tree_wind.comp.hlsl (F4-5: the RT scene's animated trees, frame-ownership.md 14.17) and the compare mode of the trees' wind-motion vertex form
// (native.tree_motion=compare, 14.42: the motion form's previous position against this evaluation with the previous frame's constants). CPU reference:
// src/enh/tree_wind.h treewind::world.
//
//   Position.xyz = lerp(pos, pos * WindMatrix[indices0.x], weights0.x)
//   leaf mesh:  offset = MeshMatrix(normal1, normal2) * RotX(rock) * weights2.xyz
//   leaf cards: offset = LeafAngleMatrix * RotY(weights1.z + rustle) * RotZ(weights1.w + rock) * (weights1.yx * weights2.yz, 0)
//   world = (dot(Position, r0), dot(Position, r1), dot(Position, r2)) + offset
#pragma once

// the constants: k[0..15] = c14..c29 (wind matrices, leaf angle matrix, rock / rustle angles), k[16..19] = c32..c35 of the instance (c32.xy rock / rustle scales,
// c33..c35 the world rows); any other register the shader would read is outside them (0)
struct RvkTreeWindConsts {
    float4 k[20];
};
float4 rvkTwC(RvkTreeWindConsts c, int i) {
    if (i < 0 || i >= 20) return float4(0.0, 0.0, 0.0, 0.0);
    return c.k[i];
}
// c(reg) of the vertex shader for reg in 14..29
float4 rvkTwReg(RvkTreeWindConsts c, int reg) { return reg >= 14 && reg <= 29 ? rvkTwC(c, reg - 14) : float4(0.0, 0.0, 0.0, 0.0); }

float3x3 rvkTwRotX(float a) {
    float2 sc = float2(sin(a), cos(a));
    return float3x3(float3(1.0, 0.0, 0.0), float3(0.0, sc.y, -sc.x), float3(0.0, sc.x, sc.y));
}
float3x3 rvkTwRotY(float a) {
    float2 sc = float2(sin(a), cos(a));
    return float3x3(float3(sc.y, 0.0, sc.x), float3(0.0, 1.0, 0.0), float3(-sc.x, 0.0, sc.y));
}
float3x3 rvkTwRotZ(float a) {
    float2 sc = float2(sin(a), cos(a));
    return float3x3(float3(sc.y, -sc.x, 0.0), float3(sc.x, sc.y, 0.0), float3(0.0, 0.0, 1.0));
}

// the world position of one vertex; cls 0 wind only (branches, fronds), 1 leaf mesh, 2 leaf cards
float3 rvkTreeWindWorld(RvkTreeWindConsts k, float4 inPos, int windIndex, float4 w0, float4 w1, float4 w2, float3 n1, float3 n2, uint cls) {
    // wind (mulWindMatrix: the 3x3 with the columns c(14 + 3 i), c(15 + 3 i), c(16 + 3 i), v * M)
    float3 windPos = float3(dot(inPos.xyz, rvkTwReg(k, 14 + 3 * windIndex).xyz), dot(inPos.xyz, rvkTwReg(k, 15 + 3 * windIndex).xyz), dot(inPos.xyz, rvkTwReg(k, 16 + 3 * windIndex).xyz));
    float4 position = inPos;
    position.xyz = lerp(inPos.xyz, windPos, w0.x);
    float3 offset = float3(0.0, 0.0, 0.0);
    if (cls != 0u) {
        float4 inst = rvkTwC(k, 16);  // c32 of the instance (g_vInstanceData(vInstance * 4))
        float2 rockAndRustle = inst.xy * rvkTwReg(k, 26 + int(w2.w)).xy;
        if (cls == 1u) {
            // leaf mesh: placement using SpeedWind leaf rocking
            float3x3 meshMatrix = float3x3(n1, -cross(n1, n2), n2);
            offset = mul(rvkTwRotX(rockAndRustle.x), w2.xyz);
            offset = mul(meshMatrix, offset);
        } else {
            // leaf cards
            offset = float3(w1.yx * w2.yz, 0.0);
            offset = mul(rvkTwRotZ(w1.w + rockAndRustle.x), offset);
            offset = mul(rvkTwRotY(w1.z + rockAndRustle.y), offset);
            // g_vLeafAngleMatrix = the 3x3 with the columns c23, c24, c25: M * v
            offset = rvkTwReg(k, 23).xyz * offset.x + rvkTwReg(k, 24).xyz * offset.y + rvkTwReg(k, 25).xyz * offset.z;
        }
    }
    float4 r0 = rvkTwC(k, 17), r1 = rvkTwC(k, 18), r2 = rvkTwC(k, 19);
    return float3(dot(position, r0), dot(position, r1), dot(position, r2)) + offset;
}
