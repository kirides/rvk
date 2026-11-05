// The engine's light model and the Full-owned light table (docs/research/gpu-scene.md 9.7, rtgi.md 1 / 19), shared by the cluster build pass
// (shaders/lighting/light_cluster.comp.hlsl) and the material shader variants (rvk_material_variant.hlsli, RVK_MV_CLUSTER).
//
// THE ENGINE'S MODEL (ge_default_ps.fx / ge_default_phong.fx, matched against corpus bytecode; rtgi.md 1):
//     acc  = ambient (lerp(c14, SH9(N), AO)) + lightmap + sun + sum over point lights i
//     point light i (c25 + 4 i = pos + 1/R^2, c26 diffuse x intensity, c27 specular, c28 shadow channel selector):
//         acc += col_i * sat(N.L_i) * sat(4 Ngeo.L_i) * (1 - sat(d^2 / R^2)) * (1 - dot(sel_i, shadowMask))
//     rgb  = diffuse * pow(acc, 0.45) * shadowMask.r + specular + selfIllum        (the scene target holds gamma-encoded values)
//     fog  : rgb = lerp(rgb, c3.rgb, sat(min(dist * c4.y + c4.x, c4.z)))
// The `original` style keeps every term of that: the material pixel shader's own light loop evaluates it, our variant only decides WHICH lights
// fill the loop's registers per pixel (the table's, picked from the pixel's cluster) and with which colour. The `physical` style changes only the
// distance law, as a per-pixel colour ratio on the same registers: col' = col * phys(d) / orig(d), so the shader's own (1 - d^2/R^2) factor
// times the ratio is the physical law, without touching the sun, the ambient SH / hemisphere, the exposure, the tone map or the encode.
//
// THE TABLE (src/enh/light_table.*): one device buffer, addresses in FrameBlock params[kFrameParamLights] (xy = reference, zw = unused),
// params[kFrameParamLights + 1] = (enabled, style, debug view, 0).
//   header   64 B   RvkLightHeader
//   lights   N x 64 B  RvkLight (posRcp, colRange, specFlags, misc)
//   clusters G x (4 + 4 * RVK_LIGHT_MAX_PER_CLUSTER) bytes: count, then light indices
#pragma once

#define RVK_LIGHT_MAX           1024u
#define RVK_LIGHT_MAX_PER_CLUSTER 63u                      // indices per cluster (+ the count word = 64 words = 256 B)
#define RVK_LIGHT_CLUSTER_BYTES 256u
#define RVK_LIGHT_BYTES         64u
#define RVK_LIGHT_HEADER_BYTES  128u
#define RVK_LIGHT_SHADOW_MAX    31u                         // ids of the ray-traced light shadows (a pixel keeps 4 of them: RVK_LS_ENTRIES)
#define RVK_LS_ENTRIES          4u
#define RVK_LIGHT_FLAG_DYNAMIC  1u                          // an eCPointLight_PS (flicker, spells), else a static light

// engine constants of the model
#define RVK_LM_ENCODE_POWER 0.45                            // pow(acc, 0.45)
#define RVK_LM_SELF_SHADOW  4.0                             // sat(4 Ngeo.L)
#define RVK_LM_MATCH_DIST2  64.0                            // an engine register light within 8 units of a table light is that light

// header words (uint4 x 4): the build pass writes them every frame
struct RvkLightHeader {
    uint4 a;  // x frame stamp (frameInfo.x + 1), y light count, z light shadow image slot (0 = none this frame), w its size (width | height << 16)
    uint4 grid;  // x tiles x, y tiles y, z slices, w max indices per cluster
    float4 zs;   // x near, y far, z slices / ln(far / near), w unused
    uint4 offs;  // x lights offset (bytes), y clusters offset (bytes), z style, w flags
    uint4 grid1; // the far level (beyond zs.y): x tiles x, y tiles y, z slices (0 = none), w clusters offset (bytes)
    float4 zs1;  // x near (= zs.y), y far, z slices / ln(far / near), w unused
};

struct RvkLight {
    float4 posRcp;    // xyz world position, w 1 / R^2
    float4 colRange;  // rgb diffuse x intensity (the constants the engine's shaders get), w range R
    float4 specFlags; // rgb specular colour, w RVK_LIGHT_FLAG_* (as float bits)
    float4 misc;      // x stable id (float bits), y light shadow id 1..RVK_LIGHT_SHADOW_MAX (uint bits, 0 = none: lighting.light_shadows), zw unused
};

// ---- the engine's terms (shared by the variant's scoring and the physical ratio) ------------------------------------------------------
float rvkLmFalloff(float3 toLight, float rcpSqrRange) { return 1.0 - saturate(dot(toLight, toLight) * rcpSqrRange); }

float rvkLmLum(float3 c) { return dot(c, float3(0.2126, 0.7152, 0.0722)); }

// the physical distance law relative to the engine's: phys(d) / orig(d), with phys = k (1 - u)^2 / (1 + d^2 / r0^2), u = d^2 / R^2, r0 = 0.2 R
// (an inverse-square law inside the smooth window (1 - u)^2, normalised so the light's brightness at 0.35 R equals the engine's: the exposure /
// look of the scene is kept, near the lamp it gets hotter, far away it is gentler); orig = 1 - u, so the ratio is finite up to R
float rvkLmPhysicalRatio(float d2, float rcpSqrRange) {
    float u = saturate(d2 * rcpSqrRange);
    float r0sq = 0.04 / max(rcpSqrRange, 1e-12);
    // k: phys(0.35 R) = orig(0.35 R): (1 - 0.1225) (1 + 0.35^2 / 0.2^2) / (1 - 0.1225)^2 = 4.6297
    return 4.6297 * (1.0 - u) / (1.0 + d2 / r0sq);
}

// cluster addressing ---------------------------------------------------------------------------------------------------------------
uint rvkLightSlice(float viewZ, float nearZ, float slicesPerLn, uint slices) {
    float s = log(max(viewZ, nearZ) / nearZ) * slicesPerLn;
    return min(uint(s), slices - 1u);
}

// the cluster that holds view depth `viewZ` at screen position `uv` (0..1): level 0 (exponential slices zs.x .. zs.y), beyond zs.y the coarse far level 1
// (a finer tile grid over a few slices up to zs1.y: lights of the far forests the material shaders' per-object lists still carry); `base` = the table
RvkAddr rvkLightClusterAt(RvkAddr base, float viewZ, float2 uv) {
    uint4 grid = rvkRawLoad<uint4, 16>(rvkAddrAdd(base, 16u));
    float4 zs = rvkRawLoad<float4, 16>(rvkAddrAdd(base, 32u));
    uint4 offs = rvkRawLoad<uint4, 16>(rvkAddrAdd(base, 48u));
    uint4 grid1 = rvkRawLoad<uint4, 16>(rvkAddrAdd(base, 64u));
    if (grid1.z != 0u && viewZ >= zs.y) {
        float4 zs1 = rvkRawLoad<float4, 16>(rvkAddrAdd(base, 80u));
        uint tx1 = min(uint(uv.x * float(grid1.x)), grid1.x - 1u), ty1 = min(uint(uv.y * float(grid1.y)), grid1.y - 1u);
        uint sl1 = rvkLightSlice(viewZ, zs1.x, zs1.z, grid1.z);
        return rvkAddrAdd(base, grid1.w + ((sl1 * grid1.y + ty1) * grid1.x + tx1) * RVK_LIGHT_CLUSTER_BYTES);
    }
    uint tx = min(uint(uv.x * float(grid.x)), grid.x - 1u), ty = min(uint(uv.y * float(grid.y)), grid.y - 1u);
    uint sl = rvkLightSlice(viewZ, zs.x, zs.z, grid.z);
    return rvkAddrAdd(base, offs.y + ((sl * grid.y + ty) * grid.x + tx) * RVK_LIGHT_CLUSTER_BYTES);
}
