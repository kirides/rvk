#version 460
// Risen material pixel shader: ge_default_ps.fx + ge_default_phong.fx (+ the material hooks of ge_default.fxh) in GLSL.
// Entry points of the HLSL: PS_Default (XFX_PASS = XFX_PASS_DEFAULT) and PS_DefaultZPass (XFX_PASS_ZPASS: depth / shadow map /
// variance map). PS_DefaultRadiosityPass (the editor's radiosity baker) is not ported: the game never draws it.
// Notes against the HLSL: `half` is `float` (the D3D9 _pp hints do not exist in Vulkan GLSL); vFace is gl_FrontFacing as the
// translator reads it (+1 front / -1 back).
// See ge_default.glsl for the defines.

#include "ge_default.glsl"

// ---- varyings: sPSInput_Default (location = TEXCOORD index, COLOR0 = 16) ------------------------------------------------------
layout(location = 0) in vec4 vTex0;            // TEXCOORD0: texture coordinate sets 0 (xy) and 2 (zw)
layout(location = 1) in vec4 vTex1;            // TEXCOORD1: sets 1 (xy) and 3 (zw)
layout(location = 2) in vec4 vViewPos;         // TEXCOORD2: clip position (x, y, z, w)
layout(location = 3) in vec4 vPositionAndAlpha;  // TEXCOORD3: xyz = world position, w = global alpha
layout(location = 4) in vec4 vNormalAndAO;     // TEXCOORD4: xyz = normal, w = vertex ambient occlusion
layout(location = 5) in vec3 vTangent;         // TEXCOORD5
layout(location = 6) in vec3 vBinormal;        // TEXCOORD6
layout(location = 7) in vec2 vLightmap;        // TEXCOORD7
layout(location = 16) in vec4 vColor0;         // COLOR0
layout(location = 0) out vec4 rvk_oC0;

// ---- structures (ge_default.fxh) ----------------------------------------------------------------------------------------------
#define eLightType_DirLight 0
#define eLightType_PntLight 1

struct sPassLight {
    vec3 m_vecLight;
    vec3 m_colSpecular;
    vec3 m_colDiffuse;
    float m_fFallOff;
    float m_fShadow;
    int m_iLightType;
};

struct sPassData {
    vec3 m_matTangentSpace[3];
    vec3 m_vecWorldPos;
    vec3 m_vecWorldNormal;
    vec3 m_vecTangentNormal;
    vec3 m_vecWorldReflect;
    vec3 m_vecWorldEye;
    vec3 m_vecTangentEye;
    vec4 m_vecViewPos;
    vec2 m_vecScreenPos;
    float m_fVertexAmbientOcclusion;
    float m_fScreenSpaceAmbientOcclusion;
    float m_fCamDistance;
    vec2 m_vecLightMapCoord;
    float m_fSrcDepth;
    float m_fDestDepth;
    float m_fGlobalAlpha;
    float m_fFresnel;
    vec4 m_colVertex;

    vec2 m_vecTex0;
    vec2 m_vecTex1;
    vec2 m_vecTex2;
    vec2 m_vecTex3;

    vec4 m_vecShadowMask;

    vec4 m_colDiffuseAndAlpha;
    vec3 m_colSelfIllumination;
    vec3 m_colSpecularMask;
    vec3 m_colLightEmission;

    vec2 m_vecDistortionOffset;
    float m_fSpecularPower;
    vec3 m_colAccDiffuse;
    vec3 m_colAccSpecular;

    int m_iLightCount;
};

// material expression macros (ge_default.fxh): the generated XFX_PS_* code refers to these
#define XFX_PS_REFLECT PassData.m_vecWorldReflect
#define XFX_PS_TANGENTEYE PassData.m_vecTangentEye.xy  // only used as (float2 expr) * XFX_PS_TANGENTEYE: HLSL truncates the float3
#define XFX_PS_WORLDEYE PassData.m_vecWorldEye
#define XFX_PS_WORLDNORMAL PassData.m_vecWorldNormal
#define XFX_PS_TANGENTNORMAL PassData.m_vecTangentNormal
#define XFX_PS_FRESNEL PassData.m_fFresnel
#define XFX_PS_CAMDISTANCE PassData.m_fCamDistance
#define XFX_PS_PROJDISTANCE PassData.m_fSrcDepth
#define XFX_PS_SCENEDEPTH PassData.m_fDestDepth
#define XFX_PS_TEX0 PassData.m_vecTex0
#define XFX_PS_TEX1 PassData.m_vecTex1
#define XFX_PS_TEX2 PassData.m_vecTex2
#define XFX_PS_TEX3 PassData.m_vecTex3
#define XFX_PS_VERTEXCOLOR PassData.m_colVertex
#define XFX_PS_VIEWPOS PassData.m_vecViewPos

// ---- material hooks (ge_default.fxh) ------------------------------------------------------------------------------------------
void GetDiffuseMap(inout sPassData PassData) {
#ifdef XFX_PS_DIFFUSE
    PassData.m_colDiffuseAndAlpha = vec4(XFX_PS_DIFFUSE);  // HLSL's implicit splat / truncation: explicit constructors
#endif
}

void GetOpacityMap(inout sPassData PassData) {
#ifdef XFX_PS_OPACITY
    PassData.m_colDiffuseAndAlpha.a = float(XFX_PS_OPACITY);
#endif
}

void GetNormalMap(inout sPassData PassData) {
#ifdef XFX_PS_NORMAL
    vec4 NormalMapColor = (XFX_PS_NORMAL - 0.5) * 2.0;

#if XFX_COMPRESSEDNORMALMAP
    // DXT5nm: x in alpha, y in green
    PassData.m_vecTangentNormal = vec3(NormalMapColor.w, NormalMapColor.y, sqrtD3D(1.0 - NormalMapColor.w * NormalMapColor.w - NormalMapColor.y * NormalMapColor.y));
#else
    PassData.m_vecTangentNormal = nrmD3D(NormalMapColor.xyz);
#endif

    // world space normal
    PassData.m_vecWorldNormal = PassData.m_vecTangentNormal.xxx * PassData.m_matTangentSpace[0];
    PassData.m_vecWorldNormal += PassData.m_vecTangentNormal.yyy * PassData.m_matTangentSpace[1];
    PassData.m_vecWorldNormal += PassData.m_vecTangentNormal.zzz * PassData.m_matTangentSpace[2];

    PassData.m_vecWorldReflect = reflect(-PassData.m_vecWorldEye, PassData.m_vecWorldNormal);
    PassData.m_fFresnel = dot(PassData.m_vecWorldNormal, PassData.m_vecWorldEye);
#endif
}

void GetSpecularPower(inout sPassData PassData) {
#ifdef XFX_PS_SPECULARPOWER
    PassData.m_fSpecularPower = float(XFX_PS_SPECULARPOWER);
#endif
}

void GetSelfIlluminationMap(inout sPassData PassData) {
#ifdef XFX_PS_SELFILLUMINATION
    PassData.m_colSelfIllumination = vec3(XFX_PS_SELFILLUMINATION);
#endif
}

void GetSpecularMaskMap(inout sPassData PassData) {
#ifdef XFX_PS_SPECULAR
    PassData.m_colSpecularMask = vec3(XFX_PS_SPECULAR);
#endif
}

void GetEmissionMap(inout sPassData PassData) {
#ifdef XFX_PS_EMISSION
    PassData.m_colLightEmission = vec3(XFX_PS_EMISSION);
#endif
}

void GetDistortionMap(inout sPassData PassData) {
#ifdef XFX_PS_DISTORTION
    PassData.m_vecDistortionOffset = vec2(XFX_PS_DISTORTION);
#ifdef XFX_PS_NORMAL
    // use the tangent space normal for the distortion offset
    PassData.m_vecDistortionOffset *= PassData.m_vecTangentNormal.xy;
#endif
#endif
}

float PhongSpecular(sPassData PassData, vec3 vecLight) {
    float Specular = saturate(dot(PassData.m_vecWorldReflect, vecLight));
    return powD3D(Specular, PassData.m_fSpecularPower * 128.0);
}

void Refract(inout vec3 OutputColor, sPassData PassData, float fAlpha) {
    vec2 Offset = PassData.m_vecDistortionOffset;

#if XFX_QUALITY > XFX_QUALITY_LOW
    // the distorted pixel must be behind the shaded one
    float DistortedDepth = tex2D(smpDepthmap, PassData.m_vecScreenPos + PassData.m_vecDistortionOffset).r * g_FarDist;
    Offset = (DistortedDepth >= PassData.m_fSrcDepth) ? PassData.m_vecDistortionOffset : vec2(0.0);
#endif

    vec3 RefractColor = tex2D(smpRefract, PassData.m_vecScreenPos + Offset).rgb;
    OutputColor = mix(RefractColor, OutputColor, fAlpha);
}

// ---- ambient (ge_common.fxh CalcAmbientLight*; ge_default_phong.fx CustumFragmentAmbient) -------------------------------------
vec3 CalcAmbientLightSimple(vec3 n) {
    return vec3(dot(g_AmbientLight_c(0), vec4(n, 1.0)), dot(g_AmbientLight_c(1), vec4(n, 1.0)), dot(g_AmbientLight_c(2), vec4(n, 1.0)));
}

vec3 CalcAmbientLightComplex(vec3 n) {
    vec3 AmbientColor = CalcAmbientLightSimple(n);

    // 4 of the quadratic polynomials
    vec4 vB = n.xyzz * n.yzzx;
    AmbientColor.r += dot(g_AmbientLight_c(3), vB);
    AmbientColor.g += dot(g_AmbientLight_c(4), vB);
    AmbientColor.b += dot(g_AmbientLight_c(5), vB);

    // final quadratic polynomial
    float vC = n.x * n.x - n.y * n.y;
    AmbientColor += g_AmbientLight_c(6).rgb * vC;
    return AmbientColor;
}

void CustumFragmentAmbient(inout sPassData PassData, inout vec3 colAmbient) {
#ifdef RVK_MV_GBUFFER
    rvk_gbN = vec4(PassData.m_vecWorldNormal, 0.0);  // the normal-mapped world normal the ambient is evaluated with: the G-buffer's
#endif
#if XFX_AMBIENTLIGHTTYPE == EAmbientLightType_None
    colAmbient = vec3(0.0);
#elif XFX_AMBIENTLIGHTTYPE == EAmbientLightType_Simple
    colAmbient = CalcAmbientLightSimple(PassData.m_vecWorldNormal);
#else
#if XFX_QUALITY == XFX_QUALITY_HIGH
    colAmbient = CalcAmbientLightComplex(PassData.m_vecWorldNormal);
#else
    colAmbient = CalcAmbientLightSimple(PassData.m_vecWorldNormal);
#endif
#endif

    // indoor stuff
    colAmbient = mix(g_IndoorAmbientColor.rgb, colAmbient, PassData.m_fVertexAmbientOcclusion);
}

// ---- Phong lighting (ge_default_phong.fx) -------------------------------------------------------------------------------------
void SimpleBrdfPhong(inout sPassData PassData, sPassLight PassLight) {
    float Lamb = saturate(dot(PassLight.m_vecLight, PassData.m_vecWorldNormal));
    // shadows apply to lamb (wrap around lighting)
    Lamb *= PassLight.m_fShadow;

#if XFX_SPECULAR && XFX_QUALITY > XFX_QUALITY_LOW
    float Specular = PhongSpecular(PassData, PassLight.m_vecLight);
    PassData.m_colAccSpecular += PassLight.m_colSpecular * Specular * PassLight.m_fShadow * PassLight.m_fFallOff;
#endif

    PassData.m_colAccDiffuse += PassLight.m_colDiffuse * Lamb * PassLight.m_fFallOff;
}

void ComplexBrdfPhong(inout sPassData PassData, sPassLight PassLight) {
    float Lamb = saturate(dot(PassLight.m_vecLight, PassData.m_vecWorldNormal));

    // self shadow term
    float SelfShadow = 1.0;
#if XFX_QUALITY > XFX_QUALITY_MEDIUM
#ifdef XFX_PS_NORMAL
    SelfShadow = saturate(4.0 * dot(PassLight.m_vecLight, PassData.m_matTangentSpace[2]));
#endif
#endif

    float Attenuation = PassLight.m_fFallOff * PassLight.m_fShadow * SelfShadow;

#if XFX_SPECULAR && XFX_QUALITY > XFX_QUALITY_LOW
    float Specular = PhongSpecular(PassData, PassLight.m_vecLight);
    float Fresnel = max(dot(PassData.m_vecWorldEye, PassData.m_vecWorldNormal), 0.00001);
    Specular = saturate(Specular * Lamb / Fresnel);
    PassData.m_colAccSpecular += PassLight.m_colSpecular * Specular * Attenuation;
#endif

    PassData.m_colAccDiffuse += PassLight.m_colDiffuse * Lamb * Attenuation;
}

void CustumFragmentLight(inout sPassData PassData, sPassLight PassLight) {
#if XFX_LIGHTINGTYPE == BRDF_Phong_Simple
    SimpleBrdfPhong(PassData, PassLight);
#else
    // BRDF_Phong_Complex and BRDF_Phong_WrapAround (the rim BRDF is commented out in the HLSL)
    ComplexBrdfPhong(PassData, PassLight);
#endif
}

void CustumFragmentInitPass(inout sPassData PassData) {
    GetNormalMap(PassData);
    GetSpecularPower(PassData);
    GetDiffuseMap(PassData);
    GetOpacityMap(PassData);
    GetSelfIlluminationMap(PassData);
    GetSpecularMaskMap(PassData);
    GetDistortionMap(PassData);
    GetEmissionMap(PassData);
}

void CustumFragmentFinish(inout sPassData PassData, inout vec4 colFinal) {
    colFinal = PassData.m_colDiffuseAndAlpha;

    // gamma
#if XFX_QUALITY == XFX_QUALITY_HIGH
    PassData.m_colAccDiffuse = powD3D(PassData.m_colAccDiffuse, 0.45);
#else
    PassData.m_colAccDiffuse = sqrt(abs(PassData.m_colAccDiffuse));
#endif

#if XFX_AMBIENTOCCLUSION
    PassData.m_colAccDiffuse *= PassData.m_fScreenSpaceAmbientOcclusion;
#endif

    colFinal.rgb *= PassData.m_colAccDiffuse;

#if XFX_SPECULAR
    PassData.m_colAccSpecular *= PassData.m_colSpecularMask;
    colFinal.rgb += PassData.m_colAccSpecular;
#endif

    colFinal.rgb += PassData.m_colSelfIllumination;
}

// ---- lights and passes (ge_default_ps.fx) -------------------------------------------------------------------------------------
void AddAmbientLight(inout sPassData PassData) {
    vec3 AmbientColor = vec3(0.0);
    CustumFragmentAmbient(PassData, AmbientColor);
    PassData.m_colAccDiffuse += AmbientColor;
}

void AddDirLight(inout sPassData PassData, vec3 vecLight, vec3 colDiffuse, vec3 colSpecular) {
    sPassLight PassLight;
    PassLight.m_vecLight = vecLight;
    PassLight.m_colSpecular = colSpecular;
    PassLight.m_colDiffuse = colDiffuse;
    PassLight.m_fFallOff = 1.0;
    PassLight.m_fShadow = 1.0;
    PassLight.m_iLightType = eLightType_DirLight;
#if XFX_DIRLIGHTSHADOWS
    PassLight.m_fShadow = PassData.m_vecShadowMask.a;
#endif
    CustumFragmentLight(PassData, PassLight);
}

void AddPointLight(inout sPassData PassData, vec4 vecLightPosAndRcpSqrRange, vec3 colDiffuse, vec3 colSpecular, vec4 vecShadowMask) {
    sPassLight PassLight;
    PassLight.m_vecLight = vecLightPosAndRcpSqrRange.xyz - PassData.m_vecWorldPos;
    PassLight.m_fFallOff = 1.0 - saturate(dot(PassLight.m_vecLight, PassLight.m_vecLight) * vecLightPosAndRcpSqrRange.w);
    PassLight.m_vecLight = nrmD3D(PassLight.m_vecLight);
    PassLight.m_iLightType = eLightType_PntLight;
#if XFX_QUALITY > XFX_QUALITY_MEDIUM
    PassLight.m_colSpecular = colSpecular;
#else
    PassLight.m_colSpecular = vec3(0.0);
#endif
    PassLight.m_colDiffuse = colDiffuse;
    PassLight.m_fShadow = 1.0;
#if XFX_PNTLIGHTSHADOWS
    PassLight.m_fShadow = 1.0 - dot(vecShadowMask, PassData.m_vecShadowMask);
#endif
    CustumFragmentLight(PassData, PassLight);
}

void AddLightMap(inout sPassData PassData) {
    // ambient light term
    vec4 AmbientLightmap1 = tex2D(smpLightMap1, PassData.m_vecLightMapCoord);
    vec4 AmbientLightmap2 = tex2D(smpLightMap2, PassData.m_vecLightMapCoord);

    AmbientLightmap1.a = AmbientLightmap1.a * 2.0 - 1.0;
    AmbientLightmap2.a = AmbientLightmap2.a * 2.0 - 1.0;

    vec3 AmbientLightDir = vec3(AmbientLightmap1.a, AmbientLightmap2.a, sqrtD3D(1.0 - AmbientLightmap1.a * AmbientLightmap1.a - AmbientLightmap2.a * AmbientLightmap2.a));
    float AmbientFactor = saturate(dot(PassData.m_vecTangentNormal, AmbientLightDir));
    vec3 AmbientLight = mix(AmbientLightmap2, AmbientLightmap1, AmbientFactor).rgb * 4.0;
    PassData.m_colAccDiffuse += AmbientLight;

#if XFX_QUALITY > XFX_QUALITY_LOW
    // specular colour
    vec3 LightDir = AmbientLightDir.xxx * PassData.m_matTangentSpace[0];
    LightDir += AmbientLightDir.yyy * PassData.m_matTangentSpace[1];
    LightDir += AmbientLightDir.zzz * PassData.m_matTangentSpace[2];

    float Specular = PhongSpecular(PassData, LightDir);
    PassData.m_colAccSpecular += AmbientLight * Specular;
#endif
}

sPassData InitPassData() {
    sPassData PassData;

    PassData.m_vecWorldPos = vPositionAndAlpha.xyz;

    PassData.m_vecTex0 = vTex0.xy;
    PassData.m_vecTex1 = vTex1.xy;
    PassData.m_vecTex2 = vTex0.zw;
    PassData.m_vecTex3 = vTex1.zw;

    PassData.m_colVertex = vColor0;

    // tangent space
    PassData.m_matTangentSpace[0] = vTangent.xyz;
    PassData.m_matTangentSpace[1] = vBinormal.xyz;
    PassData.m_matTangentSpace[2] = vNormalAndAO.xyz;

#if XFX_DOUBLESIDED
    float vFace = gl_FrontFacing ? 1.0 : -1.0;
    PassData.m_matTangentSpace[0] *= -vFace;
    PassData.m_matTangentSpace[1] *= -vFace;
    PassData.m_matTangentSpace[2] *= -vFace;
#endif

    // eye vector
    vec3 vecWorldEye = g_CamPos - vPositionAndAlpha.xyz;
    PassData.m_fCamDistance = length(vecWorldEye);
    PassData.m_vecWorldEye = nrmD3D(vecWorldEye);

    PassData.m_vecTangentEye.x = dot(PassData.m_vecWorldEye, PassData.m_matTangentSpace[0]);
    PassData.m_vecTangentEye.y = dot(PassData.m_vecWorldEye, PassData.m_matTangentSpace[1]);
    PassData.m_vecTangentEye.z = dot(PassData.m_vecWorldEye, PassData.m_matTangentSpace[2]);

    PassData.m_vecTangentNormal = vec3(0.0, 0.0, 1.0);
    PassData.m_vecWorldNormal = PassData.m_matTangentSpace[2];

    // screen position
    PassData.m_vecViewPos = vViewPos;
    PassData.m_vecScreenPos = (PassData.m_vecViewPos.xy / PassData.m_vecViewPos.w) * g_ViewToScreenScale + g_ViewToScreenOffset;

    PassData.m_vecLightMapCoord = vLightmap;

    // depth
    PassData.m_fSrcDepth = vViewPos.w;
    PassData.m_fDestDepth = tex2D(smpDepthmap, PassData.m_vecScreenPos).r * g_FarDist;

    PassData.m_fGlobalAlpha = vPositionAndAlpha.w;

    PassData.m_vecWorldReflect = reflect(-PassData.m_vecWorldEye, PassData.m_vecWorldNormal);
    PassData.m_fFresnel = dot(PassData.m_vecWorldNormal, PassData.m_vecWorldEye);

    PassData.m_vecShadowMask = tex2D(smpShadowMask, PassData.m_vecScreenPos);

    PassData.m_fVertexAmbientOcclusion = vNormalAndAO.w;
    PassData.m_fScreenSpaceAmbientOcclusion = PassData.m_vecShadowMask.r;

    PassData.m_colDiffuseAndAlpha = vec4(1.0);
    PassData.m_colSelfIllumination = vec3(0.0);
    PassData.m_colSpecularMask = vec3(1.0);
    PassData.m_fSpecularPower = 1.0;
    PassData.m_vecDistortionOffset = vec2(0.0);
    PassData.m_colLightEmission = vec3(0.0);

    PassData.m_colAccDiffuse = vec3(0.0);
    PassData.m_colAccSpecular = vec3(0.0);

    PassData.m_iLightCount = XFX_DIRLIGHT + XFX_PNTLIGHTCOUNT;

    CustumFragmentInitPass(PassData);
    return PassData;
}

// D3D9 alpha test (state emulation, as in the translated shaders)
bool rvk_alphaTest(float a) {
    float q = round(clamp(a, 0.0, 1.0) * 255.0);
    float r = pc.draw.alphaRef;
    uint f = pc.draw.alphaFunc & 0xffu;
    float w = max(fwidth(a), 1.0 / 255.0);
    if ((f == 5u || f == 7u) && (pc.draw.alphaFunc & 0x500u) != 0u) {  // abi.h kAlphaHashed / kAlphaThicken
        float x = a - (f == 5u ? r + 0.5 : r - 0.5) / 255.0;
        if ((pc.draw.alphaFunc & 0x400u) != 0u) x += float(pc.draw.alphaFunc >> 24u) * (1.0 / 255.0) * min(w, 1.0);
        if ((pc.draw.alphaFunc & 0x100u) == 0u) return x >= 0.0;
        vec2 p = gl_FragCoord.xy + 5.588238 * float((pc.draw.alphaFunc >> 16u) & 63u);
        return clamp(x / w + 0.5, 0.0, 1.0) > fract(52.9829189 * fract(dot(p, vec2(0.06711056, 0.00583715))));
    }
    switch (f) {
    case 1u: return false;
    case 2u: return q < r;
    case 3u: return q == r;
    case 4u: return q <= r;
    case 5u: return q > r;
    case 6u: return q != r;
    case 7u: return q >= r;
    default: return true;
    }
}

#if XFX_PASS == XFX_PASS_DEFAULT

// PS_Default
vec4 PS_Default() {
    sPassData PassData = InitPassData();

#if XFX_DISABLELIGHTING
    PassData.m_colAccDiffuse = vec3(1.0);
#else
#if XFX_AMBIENTLIGHT
    AddAmbientLight(PassData);
#endif

#if XFX_LIGHTMAP
    AddLightMap(PassData);
#endif

#if XFX_DIRLIGHT
    AddDirLight(PassData, g_DirLight_Direction.xyz, g_DirLight_Diffuse.rgb, g_DirLight_Specular.rgb);
#endif

    for (int i = 0; i < XFX_PNTLIGHTCOUNT; ++i) {
        AddPointLight(PassData, g_PointLight_PosAndRcpRange(i), g_PointLight_Diffuse(i).rgb, g_PointLight_Specular(i).rgb, g_PointLight_ShadowMask(i));
    }
#endif

    vec4 FinalOutput = PassData.m_colDiffuseAndAlpha;
    CustumFragmentFinish(PassData, FinalOutput);

#if XFX_FOG
    RadialFog(FinalOutput.rgb, PassData.m_fCamDistance, g_FogColor.rgb, g_FogScaleAndOffset, g_FogDensity);
#endif

    // alpha blended: multiply with the global alpha, else use it directly
#if XFX_ALPHABLEND || XFX_DISTORTION
    FinalOutput.a *= PassData.m_fGlobalAlpha;
#else
    FinalOutput.a = PassData.m_fGlobalAlpha;
#endif

#if XFX_MASKED && XFX_ST_BILLBOARDS
    if (FinalOutput.a - g_MaskReference < 0.0) discard;
#endif

#if XFX_DISTORTION
    Refract(FinalOutput.rgb, PassData, FinalOutput.a);
#endif

    return FinalOutput;
}

#else  // XFX_PASS_ZPASS

vec2 ComputeVarianceShadowMap(float a_fDepth) {
    // first moment is depth itself, second its square (+ the filter width from the derivatives)
    vec2 Moments = vec2(a_fDepth, a_fDepth * a_fDepth);
    float dx = ddx(a_fDepth);
    float dy = ddy(a_fDepth);
    Moments.y += 0.25 * (dx * dx + dy * dy);
    return Moments;
}

// PS_DefaultZPass
vec4 PS_DefaultZPass() {
    sPassData PassData = InitPassData();
    vec4 Result;

#if XFX_SHADOWMAPPASS
#if XFX_SHADOWMAPPASS_PROJECTED
    Result = vec4(PassData.m_vecViewPos.z / PassData.m_vecViewPos.w);
#elif XFX_SHADOWMAPPASS_POINT
    vec3 Delta = g_CamPos - PassData.m_vecWorldPos;
    Result = vec4(length(Delta) / g_FarDist);
#else
    Result = vec4(PassData.m_vecViewPos.z);
#endif
#else
    // only linear z (the VS divided it by the far clip)
    Result = vec4(PassData.m_vecViewPos.z);
#endif

#if XFX_MASKED
    Result.a = PassData.m_colDiffuseAndAlpha.a * PassData.m_fGlobalAlpha;
    if (Result.a - g_MaskReference < 0.0) discard;
#endif

#if XFX_VARIANCESHADOWMAP
    Result.xy = ComputeVarianceShadowMap(Result.x);
#endif

    return Result;
}

#endif

void main() {
    RVK_LOAD_SHADER_DATA();
#ifdef RVK_UBER_HOOKS
#ifdef RVK_MV_VNORMAL
    rvkMvPrologue(vNormalAndAO);
#else
    rvkMvPrologue(vec4(0.0));
#endif
#endif
#if XFX_PASS == XFX_PASS_DEFAULT
    vec4 oC0 = PS_Default();
#else
    vec4 oC0 = PS_DefaultZPass();
#endif
    if (!rvk_alphaTest(oC0.a)) discard;
#ifdef RVK_UBER_HOOKS
    rvkMvEpilogue(oC0);
#endif
    rvk_oC0 = oC0;
}
