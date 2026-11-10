// Risen's material shader (ge_default.fxh / ge_common*.fxh / ge_default_phong.fx) as HLSL 6.6 for DXC -> SPIR-V: the port of
// ge_default.glsl (which ported the shipped HLSL to GLSL; this brings it back to HLSL, with the draw ABI of rvk_draw.hlsli).
//
// The game compiles ONE HLSL source per material x lighting situation, steered by XFX_* defines (docs/research/shader-permutations.md).
// This is that source with the same defines: compile ge_default.frag.hlsl / ge_default.vert.hlsl with `-D XFX_DIRLIGHT=1 -D XFX_PNTLIGHTCOUNT=2 ...`
// (rvk_shadertool hlsl material/ge_default.frag -D ...). Booleans are 0 / 1 (HLSL: true / false). The material graph enters through
// XFX_PS_DIFFUSE / OPACITY / NORMAL / SPECULAR / SPECULARPOWER / SELFILLUMINATION / EMISSION / DISTORTION, each an expression exactly as the
// engine generates it (e.g. `-D "XFX_PS_DIFFUSE=tex2D( g_SamplerData[0], XFX_PS_TEX0 )"`); the compatibility layer below makes tex2D,
// g_SamplerData[], g_ShaderData[] resolve to the renderer's ABI (rvk_draw.hlsli) like the translated bytecode does. lerp / saturate / frac /
// ddx / rsqrt are HLSL's own here (the GLSL text had to define them).
//
// Inputs / outputs follow the translator's ABI so a variant is a drop-in for the translated D3D9 shader: varyings at location =
// TEXCOORD index (COLOR0 = 16), constants c# through the register windows (RVK_VSC / RVK_PSC), D3D9 sampler register s# = rvk_draw.tex[#] / smp[#].
#pragma once

#include "rvk_draw.hlsli"

// ---- XFX_* defaults (ge_default.fxh) ------------------------------------------------------------------------------------------
#define XFX_QUALITY_LOW 0
#define XFX_QUALITY_MEDIUM 1
#define XFX_QUALITY_HIGH 2
#ifndef XFX_QUALITY
#define XFX_QUALITY 2
#endif

#define EAmbientLightType_None 0
#define EAmbientLightType_Simple 1
#define EAmbientLightType_Complex 2

#define BRDF_Phong_Simple 0
#define BRDF_Phong_Complex 1
#define BRDF_Phong_WrapAround 2

// pass selection (the engine picks the entry point: PS_Default / PS_DefaultZPass; VS_Default / VS_DefaultZPass)
#define XFX_PASS_DEFAULT 0
#define XFX_PASS_ZPASS 1
#ifndef XFX_PASS
#define XFX_PASS XFX_PASS_DEFAULT
#endif

#ifndef XFX_SPECULAR
#define XFX_SPECULAR 0
#endif
#ifndef XFX_FOG
#define XFX_FOG 1
#endif
#ifndef XFX_AMBIENTLIGHT
#define XFX_AMBIENTLIGHT 0
#endif
#ifndef XFX_DIRLIGHT
#define XFX_DIRLIGHT 0
#endif
#ifndef XFX_LIGHTMAP
#define XFX_LIGHTMAP 0
#endif
#ifndef XFX_PNTLIGHTCOUNT
#define XFX_PNTLIGHTCOUNT 0
#endif
#ifndef XFX_SHADOWMAPPASS
#define XFX_SHADOWMAPPASS 0
#endif
#ifndef XFX_SHADOWMAPPASS_PROJECTED
#define XFX_SHADOWMAPPASS_PROJECTED 0
#endif
#ifndef XFX_SHADOWMAPPASS_POINT
#define XFX_SHADOWMAPPASS_POINT 0
#endif
#ifndef XFX_MASKED
#define XFX_MASKED 0
#endif
#ifndef XFX_VARIANCESHADOWMAP
#define XFX_VARIANCESHADOWMAP 0
#endif
#ifndef XFX_PNTLIGHTSHADOWS
#define XFX_PNTLIGHTSHADOWS 1
#endif
#ifndef XFX_DIRLIGHTSHADOWS
#define XFX_DIRLIGHTSHADOWS 1
#endif
#ifndef XFX_DISABLELIGHTING
#define XFX_DISABLELIGHTING 0
#endif
#ifndef XFX_AMBIENTOCCLUSION
#define XFX_AMBIENTOCCLUSION 1
#endif
#ifndef XFX_AMBIENTLIGHTTYPE
#define XFX_AMBIENTLIGHTTYPE EAmbientLightType_None
#endif
#ifndef XFX_INSTANCED
#define XFX_INSTANCED 0
#endif
#ifndef XFX_SKINNED
#define XFX_SKINNED 0
#endif
#ifndef XFX_MORPHED
#define XFX_MORPHED 0
#endif
#ifndef XFX_BILLBOARD
#define XFX_BILLBOARD 0
#endif
#ifndef XFX_ST_BRANCH
#define XFX_ST_BRANCH 0
#endif
// RVK_LEAFS = 1: the request is from ge_default_leafs.fx (SpeedTree leaves: its own ambient / light / back lighting hooks; the VS is the same file)
#ifndef RVK_LEAFS
#define RVK_LEAFS 0
#endif
#ifndef XFX_ST_LEAFMESH
#define XFX_ST_LEAFMESH 0
#endif
#ifndef XFX_ST_LEAFCARDS
#define XFX_ST_LEAFCARDS 0
#endif
#ifndef XFX_VEGETATION
#define XFX_VEGETATION 0
#endif
#ifndef XFX_ST_BILLBOARDS_VERT1
#define XFX_ST_BILLBOARDS_VERT1 0
#endif
#ifndef XFX_ST_BILLBOARDS
#define XFX_ST_BILLBOARDS 0
#endif
#ifndef XFX_COMPRESSEDNORMALMAP
#define XFX_COMPRESSEDNORMALMAP 0
#endif
#ifndef XFX_ALPHABLEND
#define XFX_ALPHABLEND 0
#endif
#ifndef XFX_DOUBLESIDED
#define XFX_DOUBLESIDED 0
#endif
#ifndef XFX_LIGHTINGTYPE
#define XFX_LIGHTINGTYPE BRDF_Phong_Complex
#endif
#ifndef XFX_VS_INSTANCEDATASIZE
#define XFX_VS_INSTANCEDATASIZE 200
#endif
// SM 3: three point lights (the VS-side XFX_SHADERMODEL_3 define of the HLSL)
#define XFX_MAX_PNTLIGHTS 3

#define XFX_SPEEDTREE (XFX_ST_BRANCH || XFX_ST_LEAFMESH || XFX_ST_LEAFCARDS)

#ifdef XFX_PS_DISTORTION
#define XFX_DISTORTION 1
#else
#define XFX_DISTORTION 0
#endif

// ---- D3D9 shader arithmetic (what the translator does for the bytecode): pow = exp2(y * log2(|x|)), sqrt = rcp(rsq(|x|)),
// normalize(0) = 0 (0 * inf = 0): never NaN where D3D9 gives a finite colour (e.g. a negative SH ambient term under the gamma pow,
// a DXT block whose decoded normal has x^2 + y^2 > 1 under the sqrt)
float powD3D(float x, float y) { return pow(abs(x), y); }
float2 powD3D(float2 x, float y) { return pow(abs(x), (float2)y); }
float3 powD3D(float3 x, float y) { return pow(abs(x), (float3)y); }
float4 powD3D(float4 x, float y) { return pow(abs(x), (float4)y); }
float2 powD3D(float2 x, float2 y) { return pow(abs(x), y); }
float3 powD3D(float3 x, float3 y) { return pow(abs(x), y); }
float4 powD3D(float4 x, float4 y) { return pow(abs(x), y); }
// the engine's material snippets (XFX_PS_*) call the HLSL pow: from here on the D3D9 one (the bytecode's pow is |x|^y)
#define pow(x, y) powD3D(x, y)
float sqrtD3D(float x) { return sqrt(abs(x)); }
// D3DX compiles atan2 to this polynomial (max error ~1e-4; constants as in the shipped shaders): same curve, same colours
float atan2D3D(float y, float x) {
    float ax = abs(x), ay = abs(y);
    float mx = max(ax, ay), mn = min(ax, ay);
    float t = mx > 0.0 ? mn / mx : 0.0;
    float s = t * t;
    float r = t * (((((0.0208350997 * s - 0.0851330012) * s + 0.180141002) * s - 0.330299497) * s) + 0.999866009);
    if (ay > ax) r = 1.57079637 - r;
    if (x < 0.0) r = 3.14159274 - r;
    return y < 0.0 ? -r : r;
}
float3 nrmD3D(float3 v) {
    float l = dot(v, v);
    return l > 0.0 ? v * rsqrt(l) : float3(0.0, 0.0, 0.0);
}

// the draw's DrawBlock (loaded by the entry point)
static RvkDrawData rvk_draw;

#ifdef RVK_VERTEX_SHADER
#define RVK_STAGEC(n) RVK_VSC(n)
#else
#define RVK_STAGEC(n) RVK_PSC(n)
#endif

// ---- pixel shader constants and samplers (ge_common_ps.fxh, ge_default.fxh) --------------------------------------------------
#ifndef RVK_VERTEX_SHADER

// D3D9 sampler registers
static const int smpShadowMask = 0;  // shadow mask / screen space AO
static const int smpRefract = 1;
static const int smpDepthmap = 2;
static const int smpLightMap1 = 4;
static const int smpLightMap2 = 5;
// material samplers: g_SamplerData[k] = register s(9 + k)
static const int g_SamplerData[7] = {9, 10, 11, 12, 13, 14, 15};

float4 tex2D(int slot, float2 uv) {
    return RVK_TEX2D(rvkDrawTex(rvk_draw, uint(slot))).Sample(RVK_SAMP(rvkDrawSmp(rvk_draw, uint(slot))), uv);
}
float4 texCUBE(int slot, float3 dir) {
    return RVK_TEXCUBE(rvkDrawTex(rvk_draw, uint(slot))).Sample(RVK_SAMP(rvkDrawSmp(rvk_draw, uint(slot))), dir);
}

#define g_CamPosAndFar RVK_PSC(0)
#define g_ViewToScreenPos RVK_PSC(1)
#define g_FogColor RVK_PSC(3)
#define g_FogSettings RVK_PSC(4)
#define g_CamObstacleAndMaskReference RVK_PSC(5)
#define g_CamObstacleDistances g_CamObstacleAndMaskReference.xy
#define g_MaskReference g_CamObstacleAndMaskReference.w
#define g_CamPos g_CamPosAndFar.xyz
#define g_FarDist g_CamPosAndFar.w
#define g_FogScaleAndOffset g_FogSettings.xy
#define g_FogDensity g_FogSettings.z
#define g_ViewToScreenScale g_ViewToScreenPos.xy
#define g_ViewToScreenOffset g_ViewToScreenPos.zw
#ifdef RVK_MV_AMBIENT
#ifdef RVK_GI_PACKED
#define g_IndoorAmbientColor rvkGiGet(0)  // (packed halves, rvk_material_variant.hlsli)
#else
#define g_IndoorAmbientColor rvk_gi[0]  // enhancement variants: the GI probes' ambient (rvk_material_variant.hlsli)
#endif
#else
#define g_IndoorAmbientColor RVK_PSC(14)
#endif

// g_ShaderData[8] c6, g_AmbientLight[7] c15, g_DirLight c22 (diffuse, specular, direction), g_PointLights[3] c25 (4 registers each)
#define g_ShaderData_c(i) RVK_PSC(6 + (i))
static float4 g_ShaderData[8];
#ifdef RVK_MV_AMBIENT
#ifdef RVK_GI_PACKED
#define g_AmbientLight_c(i) rvkGiGet(1 + (i))
#else
#define g_AmbientLight_c(i) rvk_gi[1 + (i)]
#endif
#else
#define g_AmbientLight_c(i) RVK_PSC(15 + (i))
#endif
#define g_DirLight_Diffuse RVK_PSC(22)
#define g_DirLight_Specular RVK_PSC(23)
#define g_DirLight_Direction RVK_PSC(24)
#ifdef RVK_MV_CLUSTER
// the Full-owned light table (rvk_material_variant.hlsli rvkMvCluster, run right before the point lights): the per-pixel registers.
// RVK_MV_CL_COUNT (the slots the table fills) comes from the translated shader's CTAB, min(3, registers / 4): the compiler trims a light's unread
// registers (3 lights without XFX_PNTLIGHTSHADOWS: c25..c34 / c35 = 10 / 11 registers = 2 slots), so the loop below can run past it. Such a slot
// keeps the engine's own registers, as the translated shader does (rvk_clSrc / rvk_clK have RVK_MV_CL_COUNT entries: an index past them read
// garbage = black leaves of owned SpeedTrees with the light table on, shader-permutations.md 12)
float4 rvkUberPointReg(int i, uint comp) {
    if (uint(i) < uint(RVK_MV_CL_COUNT)) return rvkClReg(min(uint(i), uint(RVK_MV_CL_COUNT) - 1u), comp);
    return RVK_PSC(25 + 4 * i + int(comp));
}
#define g_PointLight_PosAndRcpRange(i) rvkUberPointReg(i, 0u)
#define g_PointLight_Diffuse(i) rvkUberPointReg(i, 1u)
#define g_PointLight_Specular(i) rvkUberPointReg(i, 2u)
#define g_PointLight_ShadowMask(i) rvkUberPointReg(i, 3u)
#else
#define g_PointLight_PosAndRcpRange(i) RVK_PSC(25 + 4 * (i))
#define g_PointLight_Diffuse(i) RVK_PSC(26 + 4 * (i))
#define g_PointLight_Specular(i) RVK_PSC(27 + 4 * (i))
#define g_PointLight_ShadowMask(i) RVK_PSC(28 + 4 * (i))
#endif

#define RVK_LOAD_SHADER_DATA() \
    [unroll] for (int rvk_i = 0; rvk_i < 8; ++rvk_i) g_ShaderData[rvk_i] = g_ShaderData_c(rvk_i)

#endif  // !RVK_VERTEX_SHADER

// ---- shared helpers (ge_common.fxh) -------------------------------------------------------------------------------------------
float RadialFogFactor(float fCamDistance, float2 vecFogSettings, float fFogDensity) {
    return saturate(min(fCamDistance * vecFogSettings.y + vecFogSettings.x, fFogDensity));
}

void RadialFog(inout float3 OutputColor, float fCameraDistance, float3 colFog, float2 vecFogSettings, float fFogDensity) {
    float fFog = RadialFogFactor(fCameraDistance, vecFogSettings, fFogDensity);
    OutputColor = lerp(OutputColor, colFog, fFog);
}

float Luminance(float3 colFinal) { return dot(colFinal, float3(0.3, 0.59, 0.11)); }

// The three rotations as the GLSL port had them (a mat3 built with transpose(): the rows are the vectors below, applied as M * v).
// HLSL: the same float3x3 (rows as written) used as mul(M, v)
float3x3 RotationMatrix_xAxis(float fAngle) {
    float2 vSinCos = float2(sin(fAngle), cos(fAngle));
    return float3x3(float3(1.0, 0.0, 0.0), float3(0.0, vSinCos.y, -vSinCos.x), float3(0.0, vSinCos.x, vSinCos.y));
}
float3x3 RotationMatrix_yAxis(float fAngle) {
    float2 vSinCos = float2(sin(fAngle), cos(fAngle));
    return float3x3(float3(vSinCos.y, 0.0, vSinCos.x), float3(0.0, 1.0, 0.0), float3(-vSinCos.x, 0.0, vSinCos.y));
}
float3x3 RotationMatrix_zAxis(float fAngle) {
    float2 vSinCos = float2(sin(fAngle), cos(fAngle));
    return float3x3(float3(vSinCos.y, -vSinCos.x, 0.0), float3(vSinCos.x, vSinCos.y, 0.0), float3(0.0, 0.0, 1.0));
}
