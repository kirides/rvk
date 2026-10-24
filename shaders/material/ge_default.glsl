// Risen's material shader (ge_default.fxh / ge_common*.fxh / ge_default_phong.fx), ported from the shipped HLSL to GLSL 4.60.
//
// The game compiles ONE HLSL source per material x lighting situation, steered by XFX_* defines (docs/research/shader-permutations.md).
// This is that source with the same defines: compile ge_default.frag / ge_default.vert with `-DXFX_DIRLIGHT=1 -DXFX_PNTLIGHTCOUNT=2 ...`.
// Booleans are 0 / 1 (HLSL: true / false). The material graph enters through XFX_PS_DIFFUSE / OPACITY / NORMAL / SPECULAR /
// SPECULARPOWER / SELFILLUMINATION / EMISSION / DISTORTION, each a GLSL expression exactly as the engine generates it for HLSL
// (e.g. `-D"XFX_PS_DIFFUSE=tex2D( g_SamplerData[0], XFX_PS_TEX0 )"`); the compatibility layer below makes tex2D, g_SamplerData[],
// g_ShaderData[], lerp, saturate, ... resolve to the renderer's ABI (shader/abi.h) like the translated bytecode does.
//
// Inputs / outputs follow the translator's ABI so a variant is a drop-in for the translated D3D9 shader: varyings at location =
// TEXCOORD index (COLOR0 = 16), constants c# = pc.<stage>.c[#], D3D9 sampler register s# = pc.draw.tex[#] / smp[#].

#extension GL_GOOGLE_include_directive : require
#include "rvk_abi.glsl"

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

// ---- HLSL vocabulary ----------------------------------------------------------------------------------------------------------
#define lerp mix
#define frac fract
#define ddx dFdx
#define ddy dFdy
#define rsqrt inversesqrt
float saturate(float x) { return clamp(x, 0.0, 1.0); }
vec2 saturate(vec2 x) { return clamp(x, vec2(0.0), vec2(1.0)); }
vec3 saturate(vec3 x) { return clamp(x, vec3(0.0), vec3(1.0)); }
vec4 saturate(vec4 x) { return clamp(x, vec4(0.0), vec4(1.0)); }
float rcp(float x) { return 1.0 / x; }

// D3D9 shader arithmetic (what the translator does for the bytecode): pow = exp2(y * log2(|x|)), sqrt = rcp(rsq(|x|)),
// normalize(0) = 0 (0 * inf = 0): never NaN where D3D9 gives a finite colour (e.g. a negative SH ambient term under the gamma pow,
// a DXT block whose decoded normal has x^2 + y^2 > 1 under the sqrt)
float powD3D(float x, float y) { return pow(abs(x), y); }
vec2 powD3D(vec2 x, float y) { return pow(abs(x), vec2(y)); }
vec3 powD3D(vec3 x, float y) { return pow(abs(x), vec3(y)); }
vec4 powD3D(vec4 x, float y) { return pow(abs(x), vec4(y)); }
vec2 powD3D(vec2 x, vec2 y) { return pow(abs(x), y); }
vec3 powD3D(vec3 x, vec3 y) { return pow(abs(x), y); }
vec4 powD3D(vec4 x, vec4 y) { return pow(abs(x), y); }
// HLSL broadcasts a scalar first operand (GLSL's max / min only take the scalar second)
vec2 max(float a, vec2 b) { return max(vec2(a), b); }
vec3 max(float a, vec3 b) { return max(vec3(a), b); }
vec4 max(float a, vec4 b) { return max(vec4(a), b); }
vec2 min(float a, vec2 b) { return min(vec2(a), b); }
vec3 min(float a, vec3 b) { return min(vec3(a), b); }
vec4 min(float a, vec4 b) { return min(vec4(a), b); }
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
vec3 nrmD3D(vec3 v) {
    float l = dot(v, v);
    return l > 0.0 ? v * inversesqrt(l) : vec3(0.0);
}

#ifdef RVK_VERTEX_SHADER
#define RVK_STAGE pc.vs
#else
#define RVK_STAGE pc.ps
#endif

// ---- pixel shader constants and samplers (ge_common_ps.fxh, ge_default.fxh) --------------------------------------------------
#ifndef RVK_VERTEX_SHADER

// D3D9 sampler registers
const int smpShadowMask = 0;  // shadow mask / screen space AO
const int smpRefract = 1;
const int smpDepthmap = 2;
const int smpLightMap1 = 4;
const int smpLightMap2 = 5;
// material samplers: g_SamplerData[k] = register s(9 + k)
const int g_SamplerData[7] = int[7](9, 10, 11, 12, 13, 14, 15);

vec4 tex2D(int slot, vec2 uv) { return texture(sampler2D(t2D[pc.draw.tex[slot]], samp[pc.draw.smp[slot]]), uv); }
vec4 texCUBE(int slot, vec3 dir) { return texture(samplerCube(tCube[pc.draw.tex[slot]], samp[pc.draw.smp[slot]]), dir); }

#define g_CamPosAndFar RVK_STAGE.c[0]
#define g_ViewToScreenPos RVK_STAGE.c[1]
#define g_FogColor RVK_STAGE.c[3]
#define g_FogSettings RVK_STAGE.c[4]
#define g_CamObstacleAndMaskReference RVK_STAGE.c[5]
#define g_CamObstacleDistances g_CamObstacleAndMaskReference.xy
#define g_MaskReference g_CamObstacleAndMaskReference.w
#define g_CamPos g_CamPosAndFar.xyz
#define g_FarDist g_CamPosAndFar.w
#define g_FogScaleAndOffset g_FogSettings.xy
#define g_FogDensity g_FogSettings.z
#define g_ViewToScreenScale g_ViewToScreenPos.xy
#define g_ViewToScreenOffset g_ViewToScreenPos.zw
#define g_IndoorAmbientColor RVK_STAGE.c[14]

// g_ShaderData[8] c6, g_AmbientLight[7] c15, g_DirLight c22 (diffuse, specular, direction), g_PointLights[3] c25 (4 registers each)
#define g_ShaderData_c(i) RVK_STAGE.c[6 + (i)]
vec4 g_ShaderData[8];
#define g_AmbientLight_c(i) RVK_STAGE.c[15 + (i)]
#define g_DirLight_Diffuse RVK_STAGE.c[22]
#define g_DirLight_Specular RVK_STAGE.c[23]
#define g_DirLight_Direction RVK_STAGE.c[24]
#define g_PointLight_PosAndRcpRange(i) RVK_STAGE.c[25 + 4 * (i)]
#define g_PointLight_Diffuse(i) RVK_STAGE.c[26 + 4 * (i)]
#define g_PointLight_Specular(i) RVK_STAGE.c[27 + 4 * (i)]
#define g_PointLight_ShadowMask(i) RVK_STAGE.c[28 + 4 * (i)]

#define RVK_LOAD_SHADER_DATA() \
    for (int rvk_i = 0; rvk_i < 8; ++rvk_i) g_ShaderData[rvk_i] = g_ShaderData_c(rvk_i)

#endif  // !RVK_VERTEX_SHADER

// ---- shared helpers (ge_common.fxh) -------------------------------------------------------------------------------------------
float RadialFogFactor(float fCamDistance, vec2 vecFogSettings, float fFogDensity) {
    return saturate(min(fCamDistance * vecFogSettings.y + vecFogSettings.x, fFogDensity));
}

void RadialFog(inout vec3 OutputColor, float fCameraDistance, vec3 colFog, vec2 vecFogSettings, float fFogDensity) {
    float fFog = RadialFogFactor(fCameraDistance, vecFogSettings, fFogDensity);
    OutputColor = mix(OutputColor, colFog, fFog);
}

float Luminance(vec3 colFinal) { return dot(colFinal, vec3(0.3, 0.59, 0.11)); }

mat3 RotationMatrix_xAxis(float fAngle) {
    vec2 vSinCos = vec2(sin(fAngle), cos(fAngle));
    // HLSL rows: (1,0,0) (0,c,-s) (0,s,c)
    return transpose(mat3(vec3(1.0, 0.0, 0.0), vec3(0.0, vSinCos.y, -vSinCos.x), vec3(0.0, vSinCos.x, vSinCos.y)));
}
mat3 RotationMatrix_yAxis(float fAngle) {
    vec2 vSinCos = vec2(sin(fAngle), cos(fAngle));
    return transpose(mat3(vec3(vSinCos.y, 0.0, vSinCos.x), vec3(0.0, 1.0, 0.0), vec3(-vSinCos.x, 0.0, vSinCos.y)));
}
mat3 RotationMatrix_zAxis(float fAngle) {
    vec2 vSinCos = vec2(sin(fAngle), cos(fAngle));
    return transpose(mat3(vec3(vSinCos.y, -vSinCos.x, 0.0), vec3(vSinCos.x, vSinCos.y, 0.0), vec3(0.0, 0.0, 1.0)));
}
