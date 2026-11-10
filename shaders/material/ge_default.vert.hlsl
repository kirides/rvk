// Risen material vertex shader: ge_default_vs.fx (+ ge_common_vs.fxh constants) as HLSL 6.6 (DXC -> SPIR-V), the port of ge_default.vert.
// Entry point VSMain: VS_Default (XFX_PASS = XFX_PASS_DEFAULT) and VS_DefaultZPass (XFX_PASS_ZPASS).
// HLSL matrices: float4x4 / float3x3 constants are column major, i.e. register k holds column k, so mul(v, M) is dot(v, c_k) per component:
// written out below as dot products (the ViewProjection, the inverse view, the wind matrices); the three rotation matrices and the leaf mesh
// matrix are built from rows (float3x3 rows) and applied as mul(M, v), as the GLSL port did with its transpose()d mat3.
// Vertex inputs are declared only when the active XFX_* macros read them, locations assigned in the order below
// (renderer side: vertex declaration usage -> location from this order; docs/research/shader-permutations.md).
// See ge_default.hlsli for the defines.

#define RVK_VERTEX_SHADER
#include "ge_default.hlsli"

#ifndef XFX_VS_INPUT_TEXSETS
#define XFX_VS_INPUT_TEXSETS 1  // 1..4: how many of TEXCOORD0..3 the material's XFX_VS_TEXn read
#endif
#define XFX_VS_INPUT_TEX0 input.vTex0
#define XFX_VS_INPUT_TEX1 input.vTex1
#define XFX_VS_INPUT_TEX2 input.vTex2
#define XFX_VS_INPUT_TEX3 input.vTex3
#ifndef XFX_VS_TEX0
#define XFX_VS_TEX0 XFX_VS_INPUT_TEX0
#endif
#ifndef XFX_VS_TEX1
#define XFX_VS_TEX1 XFX_VS_INPUT_TEX0
#endif
#ifndef XFX_VS_TEX2
#define XFX_VS_TEX2 XFX_VS_INPUT_TEX0
#endif
#ifndef XFX_VS_TEX3
#define XFX_VS_TEX3 XFX_VS_INPUT_TEX0
#endif

// ---- which inputs the active macros read ---------------------------------------------------------------------------------------
#define RVK_NEED_INSTANCE (XFX_INSTANCED || XFX_ST_BRANCH || XFX_ST_LEAFMESH || XFX_ST_LEAFCARDS)
#define RVK_NEED_BILLBOARD (XFX_BILLBOARD || XFX_ST_BILLBOARDS)
#define RVK_NEED_INDICES0 (XFX_SKINNED || XFX_SPEEDTREE)
#define RVK_NEED_WEIGHTS0 (XFX_SKINNED || XFX_SPEEDTREE || XFX_VEGETATION || XFX_ST_BILLBOARDS)
#define RVK_NEED_WEIGHTS1 (XFX_ST_BRANCH || XFX_ST_LEAFCARDS || XFX_VEGETATION || XFX_ST_BILLBOARDS)
#define RVK_NEED_WEIGHTS2 (XFX_ST_LEAFMESH || XFX_ST_LEAFCARDS || XFX_VEGETATION || XFX_ST_BILLBOARDS)
#define RVK_NEED_NORMDELTA12 (XFX_ST_LEAFMESH || XFX_MORPHED)

// ---- attribute locations -------------------------------------------------------------------------------------------------------
// Every attribute the shader reads has RVK_LOC_<name> (its location) + RVK_SEM_<name> (its semantic: ATTR<location>, the name the D3D12 input layout uses).
// Run time (RVK_VS_DECL, shader::uberVertexHlsl): the draw's own vertex shader bytecode declares the inputs, the generator defines one pair per declared
// input (location = the dcl register, as the translator's ATTR<n>), so the pipeline's vertex input state (built for the engine's shader) fits this one.
// Offline (no RVK_VS_DECL: the equivalence checks, dxil-check, the corpus tools): the order below, from the macros.
#ifndef RVK_VS_DECL
#define RVK_LOC_aPos 0
#define RVK_SEM_aPos ATTR0
#define RVK_LOC_aNorm 1
#define RVK_SEM_aNorm ATTR1
#define RVK_LOC_aTangent 2
#define RVK_SEM_aTangent ATTR2
#define RVK_LOC_aCol0 3
#define RVK_SEM_aCol0 ATTR3
#define RVK_LOC_aCol1 4
#define RVK_SEM_aCol1 ATTR4
#define RVK_LOC_aTex0 5
#define RVK_SEM_aTex0 ATTR5
#define RVK_LOC_aLightmap 6
#define RVK_SEM_aLightmap ATTR6
#if XFX_VS_INPUT_TEXSETS > 1
#define RVK_LOC_aTex1 7
#define RVK_SEM_aTex1 ATTR7
#define RVK_LOC_aTex2 8
#define RVK_SEM_aTex2 ATTR8
#define RVK_LOC_aTex3 9
#define RVK_SEM_aTex3 ATTR9
#define RVK_N7 10
#else
#define RVK_N7 7
#endif
#if RVK_NEED_INSTANCE
#define RVK_LOC_aInstance RVK_N7
#define RVK_SEM_aInstance ATTR10
#define RVK_N8 (RVK_N7 + 1)
#else
#define RVK_N8 RVK_N7
#endif
#if RVK_NEED_BILLBOARD
#define RVK_LOC_aBillboard RVK_N8
#define RVK_SEM_aBillboard ATTR11
#define RVK_N9 (RVK_N8 + 1)
#else
#define RVK_N9 RVK_N8
#endif
#if RVK_NEED_INDICES0
#define RVK_LOC_aIndices0 RVK_N9
#define RVK_SEM_aIndices0 ATTR12
#define RVK_N10 (RVK_N9 + 1)
#else
#define RVK_N10 RVK_N9
#endif
#if RVK_NEED_WEIGHTS0
#define RVK_LOC_aWeights0 RVK_N10
#define RVK_SEM_aWeights0 ATTR13
#define RVK_N11 (RVK_N10 + 1)
#else
#define RVK_N11 RVK_N10
#endif
#if RVK_NEED_WEIGHTS1
#define RVK_LOC_aWeights1 RVK_N11
#define RVK_SEM_aWeights1 ATTR14
#define RVK_N12 (RVK_N11 + 1)
#else
#define RVK_N12 RVK_N11
#endif
#if RVK_NEED_WEIGHTS2
#define RVK_LOC_aWeights2 RVK_N12
#define RVK_SEM_aWeights2 ATTR15
#define RVK_N13 (RVK_N12 + 1)
#else
#define RVK_N13 RVK_N12
#endif
#if RVK_NEED_NORMDELTA12
#define RVK_LOC_aNormDelta1 RVK_N13
#define RVK_SEM_aNormDelta1 ATTR16
#define RVK_LOC_aNormDelta2 (RVK_N13 + 1)
#define RVK_SEM_aNormDelta2 ATTR17
#define RVK_N14 (RVK_N13 + 2)
#else
#define RVK_N14 RVK_N13
#endif
#if XFX_MORPHED
#define RVK_LOC_aPosDelta1 RVK_N14
#define RVK_SEM_aPosDelta1 ATTR18
#define RVK_LOC_aPosDelta2 (RVK_N14 + 1)
#define RVK_SEM_aPosDelta2 ATTR19
#define RVK_LOC_aPosDelta3 (RVK_N14 + 2)
#define RVK_SEM_aPosDelta3 ATTR20
#define RVK_LOC_aNormDelta3 (RVK_N14 + 3)
#define RVK_SEM_aNormDelta3 ATTR21
#endif
#endif  // !RVK_VS_DECL

// ---- vertex inputs (sVSInput_Default); unread ones stay zero -------------------------------------------------------------------
struct RvkVsIn {
#ifdef RVK_LOC_aPos
    [[vk::location(RVK_LOC_aPos)]] float4 aPos : RVK_SEM_aPos;  // POSITION0 (w: billboard corner index for SpeedTree billboards)
#endif
#ifdef RVK_LOC_aNorm
    [[vk::location(RVK_LOC_aNorm)]] float3 aNorm : RVK_SEM_aNorm;  // NORMAL0
#endif
#ifdef RVK_LOC_aTangent
    [[vk::location(RVK_LOC_aTangent)]] float3 aTangent : RVK_SEM_aTangent;  // TANGENT0
#endif
#ifdef RVK_LOC_aCol0
    [[vk::location(RVK_LOC_aCol0)]] float4 aCol0 : RVK_SEM_aCol0;  // COLOR0 (r: binormal sign)
#endif
#ifdef RVK_LOC_aCol1
    [[vk::location(RVK_LOC_aCol1)]] float4 aCol1 : RVK_SEM_aCol1;  // COLOR1 (a: vertex ambient occlusion)
#endif
#ifdef RVK_LOC_aTex0
    [[vk::location(RVK_LOC_aTex0)]] float2 aTex0 : RVK_SEM_aTex0;  // TEXCOORD0
#endif
#ifdef RVK_LOC_aLightmap
    [[vk::location(RVK_LOC_aLightmap)]] float2 aLightmap : RVK_SEM_aLightmap;  // TEXCOORD6
#endif
#ifdef RVK_LOC_aTex1
    [[vk::location(RVK_LOC_aTex1)]] float2 aTex1 : RVK_SEM_aTex1;  // TEXCOORD1
#endif
#ifdef RVK_LOC_aTex2
    [[vk::location(RVK_LOC_aTex2)]] float2 aTex2 : RVK_SEM_aTex2;  // TEXCOORD2
#endif
#ifdef RVK_LOC_aTex3
    [[vk::location(RVK_LOC_aTex3)]] float2 aTex3 : RVK_SEM_aTex3;  // TEXCOORD3
#endif
#ifdef RVK_LOC_aInstance
    [[vk::location(RVK_LOC_aInstance)]] float aInstance : RVK_SEM_aInstance;  // TEXCOORD4: instance index
#endif
#ifdef RVK_LOC_aBillboard
    [[vk::location(RVK_LOC_aBillboard)]] float2 aBillboard : RVK_SEM_aBillboard;  // TEXCOORD5: billboard u / v axis scale
#endif
#ifdef RVK_LOC_aIndices0
    [[vk::location(RVK_LOC_aIndices0)]] float4 aIndices0 : RVK_SEM_aIndices0;  // BLENDINDICES0
#endif
#ifdef RVK_LOC_aWeights0
    [[vk::location(RVK_LOC_aWeights0)]] float4 aWeights0 : RVK_SEM_aWeights0;  // BLENDWEIGHT0
#endif
#ifdef RVK_LOC_aWeights1
    [[vk::location(RVK_LOC_aWeights1)]] float4 aWeights1 : RVK_SEM_aWeights1;  // BLENDWEIGHT1
#endif
#ifdef RVK_LOC_aWeights2
    [[vk::location(RVK_LOC_aWeights2)]] float4 aWeights2 : RVK_SEM_aWeights2;  // BLENDWEIGHT2
#endif
#ifdef RVK_LOC_aNormDelta1
    [[vk::location(RVK_LOC_aNormDelta1)]] float3 aNormDelta1 : RVK_SEM_aNormDelta1;  // NORMAL1
#endif
#ifdef RVK_LOC_aNormDelta2
    [[vk::location(RVK_LOC_aNormDelta2)]] float3 aNormDelta2 : RVK_SEM_aNormDelta2;  // NORMAL2
#endif
#ifdef RVK_LOC_aPosDelta1
    [[vk::location(RVK_LOC_aPosDelta1)]] float4 aPosDelta1 : RVK_SEM_aPosDelta1;  // POSITION1
#endif
#ifdef RVK_LOC_aPosDelta2
    [[vk::location(RVK_LOC_aPosDelta2)]] float4 aPosDelta2 : RVK_SEM_aPosDelta2;  // POSITION2
#endif
#ifdef RVK_LOC_aPosDelta3
    [[vk::location(RVK_LOC_aPosDelta3)]] float4 aPosDelta3 : RVK_SEM_aPosDelta3;  // POSITION3
#endif
#ifdef RVK_LOC_aNormDelta3
    [[vk::location(RVK_LOC_aNormDelta3)]] float3 aNormDelta3 : RVK_SEM_aNormDelta3;  // NORMAL3
#endif
#ifdef RVK_SCENE_RECORD
    [[vk::location(RVK_SCENE_REF_LOCATION)]] uint2 rvk_sceneRef : ATTR24;  // abi.h kSceneRefLocation: (record index, free), per instance
#endif
};

struct sVSInput_Default {
    float4 vPos;
    float3 vNorm;
    float3 vTangent;
    float4 vCol0;
    float4 vCol1;
    float2 vTex0;
    float2 vTex1;
    float2 vTex2;
    float2 vTex3;
    float vInstance;
    float2 vBillboard;
    float2 vLightmap;
    float4 vPosDelta1;
    float3 vNormDelta1;
    float4 vPosDelta2;
    float3 vNormDelta2;
    float4 vPosDelta3;
    float3 vNormDelta3;
    int4 vIndices0;
    float4 vWeights0;
    float4 vWeights1;
    float4 vWeights2;
};
static sVSInput_Default input;

// ---- outputs: sVSOutput_Default (the pixel shader's inputs; vPos = SV_Position) ----------------------------------------------
struct RvkVsOut {
    float4 position : SV_Position;
    // all float4 and in ascending location order: D3D12 links the stages by register (declaration order) and by component mask, and the translated
    // pixel shaders read float4 varyings (the translator's own vertex outputs are float4 too)
    [[vk::location(0)]] float4 rvk_vTex0 : VARY0;
    [[vk::location(1)]] float4 rvk_vTex1 : VARY1;
    [[vk::location(2)]] float4 rvk_vViewPos : VARY2;
    [[vk::location(3)]] float4 rvk_vPositionAndAlpha : VARY3;
    [[vk::location(4)]] float4 rvk_vNormalAndAO : VARY4;
    [[vk::location(5)]] float4 rvk_vTangent : VARY5;
    [[vk::location(6)]] float4 rvk_vBinormal : VARY6;
    [[vk::location(7)]] float4 rvk_vLightmap : VARY7;
#ifndef __spirv__
    [[vk::location(8)]] float4 rvk_vPad8 : VARY8;
    [[vk::location(9)]] float4 rvk_vPad9 : VARY9;
    [[vk::location(10)]] float4 rvk_vPad10 : VARY10;
    [[vk::location(11)]] float4 rvk_vPad11 : VARY11;
    [[vk::location(12)]] float4 rvk_vPad12 : VARY12;
    [[vk::location(13)]] float4 rvk_vPad13 : VARY13;
#ifndef RVK_SCENE_RECORD
    [[vk::location(14)]] float4 rvk_vPad14 : VARY14;
#endif
#endif
#ifdef RVK_SCENE_RECORD
    [[vk::location(14)]] nointerpolation uint rvk_sceneIdx : VARY14;  // abi.h kSceneVaryingLocation: the DrawRecord index (scene variant)
#endif
#ifdef RVK_UBER_BONE_PREV
    [[vk::location(15)]] float4 rvk_prevWorld : VARY15;  // enh::kPrevWorldLocation: the previous frame's world position (bone-history motion, 0 = none)
#elif !defined(__spirv__)
    [[vk::location(15)]] float4 rvk_vPad15 : VARY15;
#endif
    [[vk::location(16)]] float4 rvk_vColor0 : VARY16;
};

struct sVSOutput_Default {
    float4 vPos;
    float4 vColor0;
    float4 vTex0;
    float4 vTex1;
    float4 vViewPos;
    float4 vPositionAndAlpha;
    float4 vNormalAndAO;
    float3 vTangent;
    float3 vBinormal;
    float2 vLightmap;
};

// ---- constants (ge_common_vs.fxh, ge_default.fxh) --------------------------------------------------------------------------------
float4 vsC(int i) {
    if (uint(i) < 256u) return RVK_VSC(i);
    return float4(0.0, 0.0, 0.0, 0.0);
}
// vec4(p, 1) * g_vViewProjection (register k = column k: component k is the dot with register k)
float4 mulViewProj(float3 p) {
    float4 v = float4(p, 1.0);
    return float4(dot(v, RVK_VSC(0)), dot(v, RVK_VSC(1)), dot(v, RVK_VSC(2)), dot(v, RVK_VSC(3)));
}
// g_vInvView = float3x3 with the columns c8, c9, c10: v * M, and row i of M
float3 mulInvView(float3 v) { return float3(dot(v, RVK_VSC(8).xyz), dot(v, RVK_VSC(9).xyz), dot(v, RVK_VSC(10).xyz)); }
float3 invViewRow(int i) { return float3(RVK_VSC(8)[i], RVK_VSC(9)[i], RVK_VSC(10)[i]); }
#define g_vCamPosAndFar RVK_VSC(11)
#define g_vCamPos g_vCamPosAndFar.xyz
#define g_vFar g_vCamPosAndFar.w
#define g_vVegetationData1 RVK_VSC(12)
#define g_vVegetationTimeStamp g_vVegetationData1.x
#define g_vVegetationWindStrength g_vVegetationData1.y
#define g_vVegetationBendWeight g_vVegetationData1.z
#define g_vVegetationWaveSpeed g_vVegetationData1.w
#define g_vInstanceMisc RVK_VSC(31)
#define g_vVegetationDisolve g_vInstanceMisc.xy
#define g_vMorphTargetWeights g_vInstanceMisc.xyz
// g_vInstanceData[i] = c(32 + i); g_vShaderData[i] = c(232 + i) (material texture coordinate transforms)
float4 instData(int i) { return vsC(32 + i); }
float4 instData(float i) { return vsC(32 + int(i)); }
#define g_vInstanceData(i) instData(i)
static float4 g_vShaderData[20];  // c232..: the material's texture coordinate transforms (XFX_VS_TEXn expressions)
// the wind matrix i: float3x3 with the columns c(14 + 3i), c(15 + 3i), c(16 + 3i); v * M
float3 mulWindMatrix(float3 v, int i) {
    return float3(dot(v, vsC(14 + 3 * i).xyz), dot(v, vsC(15 + 3 * i).xyz), dot(v, vsC(16 + 3 * i).xyz));
}
// g_vLeafAngleMatrix = float3x3 with the columns c23, c24, c25: M * v
float3 mulLeafAngleMatrix(float3 v) { return RVK_VSC(23).xyz * v.x + RVK_VSC(24).xyz * v.y + RVK_VSC(25).xyz * v.z; }
#define g_vLeafAngles_c(i) vsC(26 + (i))

// SpeedTree billboard constants
#define c_f2PI g_vInstanceData(0).z
#define c_fClearAlpha g_vInstanceData(0).w
#define c_fOpaqueAlpha g_vInstanceData(1).z
#define c_fAlphaSpread g_vInstanceData(1).w
#define c_fMinAlpha g_vInstanceData(2).z

float fmodf(float x, float y) { return x - y * trunc(x / y); }

// ---- special output (global alpha, SpeedTree billboards) ------------------------------------------------------------------------
void GetSpecialOutputData(inout sVSOutput_Default a_Output) {
    float GlobalAlpha = 1.0;

#if XFX_ST_BILLBOARDS
    // fading / fizzling between the 360 degree images
    float fFadeInStartDist = input.vWeights1.x;
    float fFadeInRange = input.vWeights1.y;
    float fTransitionPercent = input.vWeights1.z;

    float3 vecCamDelta = g_vCamPos - input.vPos.xyz;
    float fCamDistance = length(vecCamDelta);

    // 0 until the tree is fFadeInStartDist away, 1 at fFadeInRange
    float fDistanceFade = clamp((fCamDistance - fFadeInStartDist) / fFadeInRange, 0.0, 1.0);

    if (fDistanceFade > 0.0) {
        int iCorner = int(input.vPos.w);

        int iTexCoordOffset = int(input.vWeights0.x);
        int iNumImages = int(input.vWeights0.y);
        float fAzimuthTree = input.vWeights0.z;
        float fSliceDiameter = input.vWeights0.w;

        // camera-to-tree azimuth
        float3 vecCamToTree = nrmD3D(input.vPos.xyz - g_vCamPos);
        float fAzimuth = atan2D3D(vecCamToTree.z, -vecCamToTree.x) + fAzimuthTree;

        // into [0, 2 pi]
        if (fAzimuth < 0.0)
            fAzimuth += c_f2PI;
        else if (fAzimuth > c_f2PI)
            fAzimuth -= c_f2PI;

        // the billboard image
        int iImage360;
#if XFX_ST_BILLBOARDS_VERT1
        iImage360 = int(fAzimuth / fSliceDiameter + 1.0);
#else
        iImage360 = int(fAzimuth / fSliceDiameter);
#endif

        if (iImage360 > iNumImages - 1) iImage360 = 0;

        // texcoords from the table: base offset + the 360 degree image index
        float2 vecTexCoordsOut;
        float4 vecTexCoords = g_vInstanceData(iTexCoordOffset + iImage360);
        vecTexCoordsOut.x = vecTexCoords.x - vecTexCoords.z * g_vInstanceData(iCorner).x;
        vecTexCoordsOut.y = vecTexCoords.y - vecTexCoords.w * g_vInstanceData(iCorner).y;

        a_Output.vTex0.xy = vecTexCoordsOut;
        a_Output.vTex1.xy = vecTexCoordsOut;
        a_Output.vTex0.zw = vecTexCoordsOut;
        a_Output.vTex1.zw = vecTexCoordsOut;

        // fading between two adjacent images
        float fFadePoint = lerp(c_fClearAlpha, c_fOpaqueAlpha, fDistanceFade);

        float fAlpha;
#if XFX_ST_BILLBOARDS_VERT1
        fAlpha = 1.0 - fmodf(fAzimuth, fSliceDiameter) / fSliceDiameter;
#else
        fAlpha = (fAzimuth - (float(iImage360) * fSliceDiameter)) / fSliceDiameter;
#endif

        fAlpha = max(fAlpha, fTransitionPercent);
        fAlpha = lerp(fFadePoint, c_fClearAlpha, powD3D((fAlpha - fTransitionPercent) / (1.0 - fTransitionPercent), 1.7));

        GlobalAlpha = 1.0 - (fAlpha - c_fOpaqueAlpha) / c_fAlphaSpread;
    } else {
        GlobalAlpha = 0.0;
    }

    // clip the billboard: coordinates the pipeline clips
    if (GlobalAlpha == 0.0) {
        a_Output.vPos = g_vInstanceData(4);
    }
#elif XFX_ST_BRANCH
    // branch dissolve
    float fCurrentLod = g_vInstanceData(input.vInstance * 4.0).x;
    float fLodRadius = g_vInstanceData(input.vInstance * 4.0).y;

    float fValue = (fCurrentLod - input.vWeights1.x) / fLodRadius;

    // SpeedTree's fizzle range is c_fOpaqueAlpha (84) .. c_fClearAlpha (255): 0.33 = 84 / 255
    GlobalAlpha = lerp(0.33, 1.0, clamp(fValue, 0.0, 1.0));
#elif XFX_VEGETATION
    // vegetation dissolve
    float3 CamDelta = g_vCamPos - a_Output.vPositionAndAlpha.xyz;
    GlobalAlpha = clamp(length(CamDelta) * g_vVegetationDisolve.x + g_vVegetationDisolve.y, 0.0, 1.0);
#elif XFX_INSTANCED
    GlobalAlpha = g_vInstanceData(input.vInstance * 4.0).a;
#else
    GlobalAlpha = g_vInstanceData(0).a;
#endif

    a_Output.vPositionAndAlpha.w = GlobalAlpha;
}

// ---- world matrix (4x3 as three rows: world = (dot(p, r0), dot(p, r1), dot(p, r2))) -----------------------------------------
struct WorldMat {
    float4 r0, r1, r2;
};

WorldMat GetMatrixFromInstanceData(int a_iOffset) {
    WorldMat m;
    m.r0 = g_vInstanceData(a_iOffset);
    m.r1 = g_vInstanceData(a_iOffset + 1);
    m.r2 = g_vInstanceData(a_iOffset + 2);
    return m;
}

WorldMat GetWorldMatrix() {
#if XFX_SKINNED
    // skinning matrix: the weighted sum of four bones
    WorldMat m0 = GetMatrixFromInstanceData(input.vIndices0.x * 3 + 1);
    WorldMat m1 = GetMatrixFromInstanceData(input.vIndices0.y * 3 + 1);
    WorldMat m2 = GetMatrixFromInstanceData(input.vIndices0.z * 3 + 1);
    WorldMat m3 = GetMatrixFromInstanceData(input.vIndices0.w * 3 + 1);
    WorldMat W;
    W.r0 = m0.r0 * input.vWeights0.x + m1.r0 * input.vWeights0.y + m2.r0 * input.vWeights0.z + m3.r0 * input.vWeights0.w;
    W.r1 = m0.r1 * input.vWeights0.x + m1.r1 * input.vWeights0.y + m2.r1 * input.vWeights0.z + m3.r1 * input.vWeights0.w;
    W.r2 = m0.r2 * input.vWeights0.x + m1.r2 * input.vWeights0.y + m2.r2 * input.vWeights0.z + m3.r2 * input.vWeights0.w;
    return W;
#elif XFX_INSTANCED
    return GetMatrixFromInstanceData(int(input.vInstance) * 4 + 1);
#else
    return GetMatrixFromInstanceData(1);
#endif
}

// ---- vegetation helpers ----------------------------------------------------------------------------------------------------------
float3 qrot(float4 q, float3 v) { return v + 2.0 * cross(q.xyz, cross(q.xyz, v) + q.w * v); }
float4 SmoothCurve(float4 x) { return x * x * (3.0 - 2.0 * x); }
float4 TriangleWave(float4 x) { return abs(frac(x + 0.5) * 2.0 - 1.0); }

// ---- Transform --------------------------------------------------------------------------------------------------------------------
void Transform(out float3 a_vecWorldPosition, out float3 a_vecWorldNormal, out float3 a_vecWorldTangent, out float3 a_vecWorldBinormal) {
    a_vecWorldPosition = float3(0.0, 0.0, 0.0);
    a_vecWorldNormal = float3(0.0, 0.0, 0.0);
    a_vecWorldTangent = float3(0.0, 0.0, 0.0);
    a_vecWorldBinormal = float3(0.0, 0.0, 0.0);
    WorldMat WorldMatrix = GetWorldMatrix();

    float3 WorldPosOffset = float3(0.0, 0.0, 0.0);
    float4 Position = input.vPos;
    float3 Normal = input.vNorm;
    float3 Tangent = input.vTangent;

#if XFX_MORPHED
    Position.xyz += input.vPosDelta1.xyz * g_vMorphTargetWeights.x;
    Normal += (input.vNormDelta1.xyz * 2.0 - 1.0) * g_vMorphTargetWeights.x;
    Position.xyz += input.vPosDelta2.xyz * g_vMorphTargetWeights.y;
    Normal += (input.vNormDelta2.xyz * 2.0 - 1.0) * g_vMorphTargetWeights.y;
    Position.xyz += input.vPosDelta3.xyz * g_vMorphTargetWeights.z;
    Normal += (input.vNormDelta3.xyz * 2.0 - 1.0) * g_vMorphTargetWeights.z;

    // morphed tangent by orthonormalization
    Tangent = nrmD3D(input.vTangent - dot(input.vTangent, Normal) * Normal);
#endif

#if XFX_SPEEDTREE
    // wind
    Position.xyz = lerp(input.vPos.xyz, mulWindMatrix(input.vPos.xyz, input.vIndices0.x), input.vWeights0.x);

#if XFX_ST_LEAFMESH
    // placement using speedwind leaf rocking
    float2 RockAndRustle = g_vInstanceData(input.vInstance * 4.0).xy * g_vLeafAngles_c(int(input.vWeights2.w)).xy;
    float3x3 RockRustle = RotationMatrix_xAxis(RockAndRustle.x);  // rock
    float3x3 MeshMatrix = float3x3(input.vNormDelta1, -cross(input.vNormDelta1, input.vNormDelta2), input.vNormDelta2);

    WorldPosOffset = input.vWeights2.xyz;
    WorldPosOffset = mul(RockRustle, WorldPosOffset);
    WorldPosOffset = mul(MeshMatrix, WorldPosOffset);

    Normal = mul(MeshMatrix, Normal);
    Tangent = mul(MeshMatrix, Tangent);
#elif XFX_ST_LEAFCARDS
    float2 RockAndRustle = g_vInstanceData(input.vInstance * 4.0).xy * g_vLeafAngles_c(int(input.vWeights2.w)).xy;
    WorldPosOffset = float3(input.vWeights1.yx * input.vWeights2.yz, 0.0);

#if XFX_SHADOWMAPPASS
    // shadow map pass: offset into the z axis
    WorldPosOffset.z = dot(WorldPosOffset.xy, float2(0.5, 0.5));
#endif

    WorldPosOffset = mul(RotationMatrix_zAxis(input.vWeights1.w + RockAndRustle.x), WorldPosOffset);
    WorldPosOffset = mul(RotationMatrix_yAxis(input.vWeights1.z + RockAndRustle.y), WorldPosOffset);
    WorldPosOffset = mulLeafAngleMatrix(WorldPosOffset);
#endif
#endif

#if XFX_VEGETATION
    float3 vInstanceOffset = input.vWeights0.xyz;
    float4 qInstanceRotation = input.vWeights1;
    float2 vInstanceScaling = input.vWeights2.xy;
    float fNormalBendFactor = input.vWeights2.z;
    float fWindStrength = input.vWeights0.w;

    // instance scaling / rotation / position
    a_vecWorldPosition = Position.xyz * vInstanceScaling.xyx;
    a_vecWorldPosition = qrot(qInstanceRotation, a_vecWorldPosition);

    float fObjPhase = length(vInstanceOffset);
    float2 vWavesIn = (float2)g_vVegetationTimeStamp;
    vWavesIn += fObjPhase * 0.02;

    float4 vWaves = (frac((vWavesIn.x + vWavesIn.y) * float4(0.89, 0.5168, 0.8713, 0.61341) * 0.1) * 2.0 - 1.0) * g_vVegetationWaveSpeed;
    vWaves = SmoothCurve(TriangleWave(vWaves));

    float2 vWavesSum = vWaves.xz + vWaves.yw;
    float2 vBend = vWavesSum.xy * g_vVegetationWindStrength * fWindStrength;

    // bend factor
    float2 fBendFactor = (float2)(a_vecWorldPosition.y * g_vVegetationBendWeight + 1.0);

    // smooth bend factor
    fBendFactor *= fBendFactor;
    fBendFactor = fBendFactor * fBendFactor - fBendFactor;

    // rotate in the wind
    float fLength = length(a_vecWorldPosition);
    float3 vecNewPos = a_vecWorldPosition;
    vecNewPos.xz += vBend.xy * fBendFactor;
    a_vecWorldPosition = nrmD3D(vecNewPos) * fLength;

    a_vecWorldPosition += vInstanceOffset;

    // bent normal
    Normal = lerp(Normal, float3(0.0, 1.0, 0.0), fNormalBendFactor);

    a_vecWorldNormal = nrmD3D(qrot(qInstanceRotation, Normal));
    a_vecWorldTangent = float3(0.0, 0.0, 0.0);
    a_vecWorldBinormal = float3(0.0, 0.0, 0.0);
#elif XFX_BILLBOARD
    // screen aligned billboard
    a_vecWorldPosition = float3(dot(Position, WorldMatrix.r0), dot(Position, WorldMatrix.r1), dot(Position, WorldMatrix.r2));
    a_vecWorldPosition += invViewRow(0) * input.vBillboard.x;
    a_vecWorldPosition += invViewRow(1) * input.vBillboard.y;
    a_vecWorldNormal = nrmD3D(mulInvView(input.vNorm));
    a_vecWorldTangent = nrmD3D(mulInvView(input.vTangent));
    a_vecWorldBinormal = cross(a_vecWorldNormal, a_vecWorldTangent);
#elif XFX_ST_BILLBOARDS
    // SpeedTree special billboard
    a_vecWorldPosition = Position.xyz;

    // the vector from the tree's pivot to the camera is the billboard's normal
    float3 vecTreeToCam = g_vCamPos - Position.xyz;
    vecTreeToCam.y = 0.0;
    float3 vecBBNormal = nrmD3D(vecTreeToCam);
    float3 vecBB_Y = input.vWeights2.xyz;
    float3 vecBB_X = cross(vecBBNormal, vecBB_Y);

    a_vecWorldPosition += vecBB_X * input.vBillboard.x;
    a_vecWorldPosition += vecBB_Y * input.vBillboard.y;

    a_vecWorldNormal = vecBBNormal;
    a_vecWorldTangent = -vecBB_X;  // invert X for the billboard normals
    a_vecWorldBinormal = vecBB_Y;
#else
    // into world space
    a_vecWorldPosition = float3(dot(Position, WorldMatrix.r0), dot(Position, WorldMatrix.r1), dot(Position, WorldMatrix.r2)) + WorldPosOffset;
    a_vecWorldNormal = nrmD3D(float3(dot(Normal, WorldMatrix.r0.xyz), dot(Normal, WorldMatrix.r1.xyz), dot(Normal, WorldMatrix.r2.xyz)));
    a_vecWorldTangent = nrmD3D(float3(dot(Tangent, WorldMatrix.r0.xyz), dot(Tangent, WorldMatrix.r1.xyz), dot(Tangent, WorldMatrix.r2.xyz)));
    a_vecWorldBinormal = cross(a_vecWorldNormal, a_vecWorldTangent);

    // right handed binormals: the sign is stored in the red component of the diffuse colour
    a_vecWorldBinormal *= (input.vCol0.r * -2.0 + 1.0);
#endif
}

void loadInputs(RvkVsIn rvk_in) {
    [unroll] for (int rvk_i = 0; rvk_i < 20; ++rvk_i) g_vShaderData[rvk_i] = RVK_VSC(232 + rvk_i);
    input = (sVSInput_Default)0;
#ifdef RVK_LOC_aPos
    input.vPos = rvk_in.aPos;
#endif
#ifdef RVK_LOC_aNorm
    input.vNorm = rvk_in.aNorm;
#endif
#ifdef RVK_LOC_aTangent
    input.vTangent = rvk_in.aTangent;
#endif
#ifdef RVK_LOC_aCol0
    input.vCol0 = rvk_in.aCol0;
#endif
#ifdef RVK_LOC_aCol1
    input.vCol1 = rvk_in.aCol1;
#endif
#ifdef RVK_LOC_aTex0
    input.vTex0 = rvk_in.aTex0;
#endif
#ifdef RVK_LOC_aLightmap
    input.vLightmap = rvk_in.aLightmap;
#endif
#ifdef RVK_LOC_aTex1
    input.vTex1 = rvk_in.aTex1;
#endif
#ifdef RVK_LOC_aTex2
    input.vTex2 = rvk_in.aTex2;
#endif
#ifdef RVK_LOC_aTex3
    input.vTex3 = rvk_in.aTex3;
#endif
#ifdef RVK_LOC_aInstance
    input.vInstance = rvk_in.aInstance;
#endif
#ifdef RVK_LOC_aBillboard
    input.vBillboard = rvk_in.aBillboard;
#endif
#ifdef RVK_LOC_aIndices0
    // D3D12 reads a D3D9 UBYTE4 attribute as R8G8B8A8_UNORM (no scaled formats in DXGI); the draw's push flags bit 17 + register mark it (abi.h, the
    // translator does the same for every input): without the scale every bone index became 0 or 1 (a black hero with a detached boot)
    {
        float4 rvk_bi = rvk_in.aIndices0;
        if (RVK_LOC_aIndices0 < 15 && ((rvkDrawFlags() >> (17u + uint(RVK_LOC_aIndices0))) & 1u) != 0u) rvk_bi = round(rvk_bi * 255.0);
        input.vIndices0 = int4(rvk_bi);
    }
#endif
#ifdef RVK_LOC_aWeights0
    input.vWeights0 = rvk_in.aWeights0;
#endif
#ifdef RVK_LOC_aWeights1
    input.vWeights1 = rvk_in.aWeights1;
#ifdef RVK_LOD_FADE
    // the billboard fade variant (Device::lodModule): the fade distances scaled by the frame parameter RVK_LOD_FADE (lod.trees)
    input.vWeights1.xy *= rvkPtr<RvkFrameData, 16>(pc.frame).Get().params[RVK_LOD_FADE].x;
#endif
#endif
#ifdef RVK_LOC_aWeights2
    input.vWeights2 = rvk_in.aWeights2;
#endif
#ifdef RVK_LOC_aNormDelta1
    input.vNormDelta1 = rvk_in.aNormDelta1;
#endif
#ifdef RVK_LOC_aNormDelta2
    input.vNormDelta2 = rvk_in.aNormDelta2;
#endif
#ifdef RVK_LOC_aPosDelta1
    input.vPosDelta1 = rvk_in.aPosDelta1;
#endif
#ifdef RVK_LOC_aPosDelta2
    input.vPosDelta2 = rvk_in.aPosDelta2;
#endif
#ifdef RVK_LOC_aPosDelta3
    input.vPosDelta3 = rvk_in.aPosDelta3;
#endif
#ifdef RVK_LOC_aNormDelta3
    input.vNormDelta3 = rvk_in.aNormDelta3;
#endif
}

#if XFX_PASS == XFX_PASS_DEFAULT

// VS_Default
sVSOutput_Default VS_Default() {
    sVSOutput_Default output_ = (sVSOutput_Default)0;

    float3 vecWorldPosition, vecWorldNormal, vecWorldTangent, vecWorldBinormal;
    Transform(vecWorldPosition, vecWorldNormal, vecWorldTangent, vecWorldBinormal);

    // texture coordinates
    output_.vTex0.xy = XFX_VS_TEX0;
    output_.vTex1.xy = XFX_VS_TEX1;
    output_.vTex0.zw = XFX_VS_TEX2;
    output_.vTex1.zw = XFX_VS_TEX3;

    output_.vLightmap = input.vLightmap;

    // project
    output_.vPos = mulViewProj(vecWorldPosition);
    output_.vViewPos = output_.vPos;

    output_.vColor0 = input.vCol0;
    output_.vTangent = vecWorldTangent;
    output_.vBinormal = vecWorldBinormal;
    output_.vPositionAndAlpha.xyz = vecWorldPosition;
    output_.vPositionAndAlpha.w = 1.0;
    output_.vNormalAndAO.xyz = vecWorldNormal;
    output_.vNormalAndAO.w = input.vCol1.a;

    // vertex ambient occlusion
#if XFX_VEGETATION || XFX_SPEEDTREE || XFX_ST_BILLBOARDS
    output_.vNormalAndAO.w = 1.0;
#endif

    GetSpecialOutputData(output_);
    return output_;
}

#else

// VS_DefaultZPass
sVSOutput_Default VS_DefaultZPass() {
    sVSOutput_Default output_ = (sVSOutput_Default)0;

    float3 vecWorldPosition, vecWorldNormal, vecWorldTangent, vecWorldBinormal;
    Transform(vecWorldPosition, vecWorldNormal, vecWorldTangent, vecWorldBinormal);

    // texture coordinates only for the masked ones
#if XFX_MASKED
    output_.vTex0.xy = XFX_VS_TEX0;
    output_.vTex1.xy = XFX_VS_TEX1;
    output_.vTex0.zw = XFX_VS_TEX2;
    output_.vTex1.zw = XFX_VS_TEX3;
#endif

    output_.vPos = mulViewProj(vecWorldPosition);
    output_.vViewPos = output_.vPos;

    // divided by the far clip
#if !XFX_SHADOWMAPPASS
    output_.vViewPos.z = output_.vViewPos.w / g_vFar;
#endif

    output_.vPositionAndAlpha.xyz = vecWorldPosition;
    output_.vPositionAndAlpha.w = 1.0;
    output_.vColor0 = input.vCol0;

    GetSpecialOutputData(output_);
    return output_;
}

#endif

RvkVsOut rvkPortMain(RvkVsIn rvk_in) {
#ifdef RVK_SCENE_RECORD
    rvkSceneBeginVs(rvk_in.rvk_sceneRef);
#endif
    rvk_draw = rvkLoadDraw();
    loadInputs(rvk_in);
#if XFX_PASS == XFX_PASS_DEFAULT
    sVSOutput_Default o = VS_Default();
#else
    sVSOutput_Default o = VS_DefaultZPass();
#endif
    RvkVsOut rvk_out = (RvkVsOut)0;
#ifdef RVK_SCENE_RECORD
    rvk_out.rvk_sceneIdx = rvk_sceneIndex;
#endif
    rvk_out.rvk_vTex0 = o.vTex0;
    rvk_out.rvk_vTex1 = o.vTex1;
    rvk_out.rvk_vViewPos = o.vViewPos;
    rvk_out.rvk_vPositionAndAlpha = o.vPositionAndAlpha;
    rvk_out.rvk_vNormalAndAO = o.vNormalAndAO;
    rvk_out.rvk_vTangent = float4(o.vTangent, 0.0);
    rvk_out.rvk_vBinormal = float4(o.vBinormal, 0.0);
    rvk_out.rvk_vLightmap = float4(o.vLightmap, 0.0, 0.0);
    rvk_out.rvk_vColor0 = o.vColor0;
    // the translator's half-pixel fix
    float4 rvk_pos = o.vPos;
    rvk_pos.xy += rvk_draw.posFix.xy * rvk_pos.w;
    rvk_pos.z += rvk_draw.posFix.z * rvk_pos.z + rvk_draw.posFix.w * rvk_pos.w;  // reversed depth (posFix.zw, abi.h)
    rvk_out.position = rvk_pos;
    return rvk_out;
}

#ifdef RVK_UBER_TRPOS
// The clip position of the TRANSLATED vertex shader of the same bytecode (shader::uberVertexHlsl puts the translator's text in namespace rvkTr before this
// file; shader-permutations.md 12). The z pre-pass and the colour pass test the depth for equality, and a geometry's two passes do not always take the
// same form: an owned z op (this port) meets a composed colour op (the translated base / capture variant) and vice versa. The port computes the same
// position in another order of operations (a few ulps apart): every such pair failed the equality test and the colour pass left black holes (owned
// SpeedTrees with the light table, jungle). The translated position is exact by construction; the port's own position still feeds vViewPos / the
// world position. The entry takes the translator's input struct (float4 per declared register, as the pipeline fetches them); the port gets its copies.
#define RVK_CAT_(a, b) a##b
#define RVK_CAT(a, b) RVK_CAT_(a, b)
#define RVK_TRIN(attr) t.RVK_CAT(v, RVK_LOC_##attr)
#ifdef RVK_UBER_PULL
#define RVK_TR_BODY rvkTr::rvkVsMain  // (S-4: the translator's pulled form names its program rvkVsMain; its entry reads the inputs from the fetch block)
#else
#define RVK_TR_BODY rvkTr::VSMain
#endif
#ifdef RVK_UBER_BONE_PREV
// Bone-history motion in the port (native.bone_motion + debug.ubershader, frame-ownership.md 14.15 item a). The port's own skinning (Transform: GetWorldMatrix reads
// the bones through VS window 2, which RVK_BONE_PREV lets rvk_vsw2Ov redirect) runs on THIS frame's bone image and on the previous frame's (the record's prevVs = the
// bone table's history image, valid when its c231.x = 1), both in one loop body so a part that did not move gives a displacement of exactly 0. The previous world
// position = the translated clip position's world point (`trWorld`: the translated part reconstructs it through invViewProj as the translated bone form does) plus
// that displacement: a still part's motion is exactly the camera's, as with the translated form; the clip position stays the translated one (RVK_UBER_TRPOS).
float4 rvkUberBonePrev(float4 trWorld) {
    float4 r = float4(0.0, 0.0, 0.0, 0.0);
    const uint2 ref = rvkDrawPrevVs();
    const bool has = any(ref != uint2(0u, 0u));
    const uint2 prev = has ? ref : rvkVsWin(2u);  // (never the zero pair: a load hoisted above the test stays in a live buffer)
#ifdef RVK_UBER_BONE_OBJ
    // (a rigid draw's object record: the history record's info.w (c36.w) = 1 when valid, 14.15 b)
    const bool valid = rvkRawLoad<uint4, 16>(rvkAddrAdd(rvkAddr(prev), 36u * 16u)).w == 1u;
#else
    const bool valid = rvkRawLoad<float4, 16>(rvkAddrAdd(rvkAddr(prev), 231u * 16u)).x == 1.0;
#endif
    if (has && valid && trWorld.w == 1.0) {
        float3 w0 = float3(0.0, 0.0, 0.0), w1 = float3(0.0, 0.0, 0.0);
        [loop] for (uint k = 0u; k < 2u; ++k) {
            rvk_vsw2Ov = k == 0u ? uint2(0u, 0u) : prev;
            float3 wp, wn, wt, wb;
            Transform(wp, wn, wt, wb);
            if (k == 0u) w0 = wp;
            else w1 = wp;
        }
        rvk_vsw2Ov = uint2(0u, 0u);
        r = float4(trWorld.xyz + (w1 - w0), 1.0);
    }
    return r;
}
#endif
RvkVsOut rvkUberFromTr(rvkTr::RvkVsIn t) {
    RvkVsIn p = (RvkVsIn)0;
#ifdef RVK_LOC_aPos
    p.aPos = RVK_TRIN(aPos);
#endif
#ifdef RVK_LOC_aNorm
    p.aNorm = RVK_TRIN(aNorm).xyz;
#endif
#ifdef RVK_LOC_aTangent
    p.aTangent = RVK_TRIN(aTangent).xyz;
#endif
#ifdef RVK_LOC_aCol0
    p.aCol0 = RVK_TRIN(aCol0);
#endif
#ifdef RVK_LOC_aCol1
    p.aCol1 = RVK_TRIN(aCol1);
#endif
#ifdef RVK_LOC_aTex0
    p.aTex0 = RVK_TRIN(aTex0).xy;
#endif
#ifdef RVK_LOC_aLightmap
    p.aLightmap = RVK_TRIN(aLightmap).xy;
#endif
#ifdef RVK_LOC_aTex1
    p.aTex1 = RVK_TRIN(aTex1).xy;
#endif
#ifdef RVK_LOC_aTex2
    p.aTex2 = RVK_TRIN(aTex2).xy;
#endif
#ifdef RVK_LOC_aTex3
    p.aTex3 = RVK_TRIN(aTex3).xy;
#endif
#ifdef RVK_LOC_aInstance
    p.aInstance = RVK_TRIN(aInstance).x;
#endif
#ifdef RVK_LOC_aBillboard
    p.aBillboard = RVK_TRIN(aBillboard).xy;
#endif
#ifdef RVK_LOC_aIndices0
    p.aIndices0 = RVK_TRIN(aIndices0);
#endif
#ifdef RVK_LOC_aWeights0
    p.aWeights0 = RVK_TRIN(aWeights0);
#endif
#ifdef RVK_LOC_aWeights1
    p.aWeights1 = RVK_TRIN(aWeights1);
#endif
#ifdef RVK_LOC_aWeights2
    p.aWeights2 = RVK_TRIN(aWeights2);
#endif
#ifdef RVK_LOC_aNormDelta1
    p.aNormDelta1 = RVK_TRIN(aNormDelta1).xyz;
#endif
#ifdef RVK_LOC_aNormDelta2
    p.aNormDelta2 = RVK_TRIN(aNormDelta2).xyz;
#endif
#ifdef RVK_LOC_aPosDelta1
    p.aPosDelta1 = RVK_TRIN(aPosDelta1);
#endif
#ifdef RVK_LOC_aPosDelta2
    p.aPosDelta2 = RVK_TRIN(aPosDelta2);
#endif
#ifdef RVK_LOC_aPosDelta3
    p.aPosDelta3 = RVK_TRIN(aPosDelta3);
#endif
#ifdef RVK_LOC_aNormDelta3
    p.aNormDelta3 = RVK_TRIN(aNormDelta3).xyz;
#endif
#ifdef RVK_SCENE_RECORD
    p.rvk_sceneRef = t.rvk_sceneRef;
#endif
    RvkVsOut o = rvkPortMain(p);
    const rvkTr::RvkVsOut tr = RVK_TR_BODY(t);
    o.position = tr.position;
#ifdef RVK_UBER_BONE_PREV
    o.rvk_prevWorld = rvkUberBonePrev(tr.rvk_prevWorld);
#endif
    return o;
}
#ifdef RVK_UBER_PULL
// S-4 vertex pulling (frame-ownership.md 14.12): the translator's pulled entry builds its input struct from the draw's VertexFetch block (rvk_pull.hlsli);
// the port and the translated position take it as from the input assembler
RvkVsOut VSMain(rvkTr::RvkVsPullIn pin) { return rvkUberFromTr(rvkTr::rvkPullInputs(pin)); }
#else
RvkVsOut VSMain(rvkTr::RvkVsIn t) { return rvkUberFromTr(t); }
#endif
#else
RvkVsOut VSMain(RvkVsIn rvk_in) { return rvkPortMain(rvk_in); }
#endif
