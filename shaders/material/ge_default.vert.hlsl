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

// location numbers: the order below, an attribute that is not read takes no location (RVK_N7.. count the declared ones)
#define RVK_N7 7
#if XFX_VS_INPUT_TEXSETS > 1
#define RVK_N8 (RVK_N7 + 3)
#else
#define RVK_N8 RVK_N7
#endif
#if RVK_NEED_INSTANCE
#define RVK_N9 (RVK_N8 + 1)
#else
#define RVK_N9 RVK_N8
#endif
#if RVK_NEED_BILLBOARD
#define RVK_N10 (RVK_N9 + 1)
#else
#define RVK_N10 RVK_N9
#endif
#if RVK_NEED_INDICES0
#define RVK_N11 (RVK_N10 + 1)
#else
#define RVK_N11 RVK_N10
#endif
#if RVK_NEED_WEIGHTS0
#define RVK_N12 (RVK_N11 + 1)
#else
#define RVK_N12 RVK_N11
#endif
#if RVK_NEED_WEIGHTS1
#define RVK_N13 (RVK_N12 + 1)
#else
#define RVK_N13 RVK_N12
#endif
#if RVK_NEED_WEIGHTS2
#define RVK_N14 (RVK_N13 + 1)
#else
#define RVK_N14 RVK_N13
#endif
#if RVK_NEED_NORMDELTA12
#define RVK_N15 (RVK_N14 + 2)
#else
#define RVK_N15 RVK_N14
#endif

// ---- vertex inputs (sVSInput_Default); unread ones stay zero -------------------------------------------------------------------
struct RvkVsIn {
    [[vk::location(0)]] float4 aPos : ATTR0;       // POSITION0 (w: billboard corner index for SpeedTree billboards)
    [[vk::location(1)]] float3 aNorm : ATTR1;      // NORMAL0
    [[vk::location(2)]] float3 aTangent : ATTR2;   // TANGENT0
    [[vk::location(3)]] float4 aCol0 : ATTR3;      // COLOR0 (r: binormal sign)
    [[vk::location(4)]] float4 aCol1 : ATTR4;      // COLOR1 (a: vertex ambient occlusion)
    [[vk::location(5)]] float2 aTex0 : ATTR5;      // TEXCOORD0
    [[vk::location(6)]] float2 aLightmap : ATTR6;  // TEXCOORD6
#if XFX_VS_INPUT_TEXSETS > 1
    [[vk::location(RVK_N7)]] float2 aTex1 : ATTR7;      // TEXCOORD1
    [[vk::location(RVK_N7 + 1)]] float2 aTex2 : ATTR8;  // TEXCOORD2
    [[vk::location(RVK_N7 + 2)]] float2 aTex3 : ATTR9;  // TEXCOORD3
#endif
#if RVK_NEED_INSTANCE
    [[vk::location(RVK_N8)]] float aInstance : ATTR10;  // TEXCOORD4: instance index
#endif
#if RVK_NEED_BILLBOARD
    [[vk::location(RVK_N9)]] float2 aBillboard : ATTR11;  // TEXCOORD5: billboard u / v axis scale
#endif
#if RVK_NEED_INDICES0
    [[vk::location(RVK_N10)]] float4 aIndices0 : ATTR12;  // BLENDINDICES0
#endif
#if RVK_NEED_WEIGHTS0
    [[vk::location(RVK_N11)]] float4 aWeights0 : ATTR13;  // BLENDWEIGHT0
#endif
#if RVK_NEED_WEIGHTS1
    [[vk::location(RVK_N12)]] float4 aWeights1 : ATTR14;  // BLENDWEIGHT1
#endif
#if RVK_NEED_WEIGHTS2
    [[vk::location(RVK_N13)]] float4 aWeights2 : ATTR15;  // BLENDWEIGHT2
#endif
#if RVK_NEED_NORMDELTA12
    [[vk::location(RVK_N14)]] float3 aNormDelta1 : ATTR16;      // NORMAL1
    [[vk::location(RVK_N14 + 1)]] float3 aNormDelta2 : ATTR17;  // NORMAL2
#endif
#if XFX_MORPHED
    [[vk::location(RVK_N15)]] float4 aPosDelta1 : ATTR18;      // POSITION1
    [[vk::location(RVK_N15 + 1)]] float4 aPosDelta2 : ATTR19;  // POSITION2
    [[vk::location(RVK_N15 + 2)]] float4 aPosDelta3 : ATTR20;  // POSITION3
    [[vk::location(RVK_N15 + 3)]] float3 aNormDelta3 : ATTR21;  // NORMAL3
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
    [[vk::location(0)]] float4 rvk_vTex0 : VARY0;
    [[vk::location(1)]] float4 rvk_vTex1 : VARY1;
    [[vk::location(2)]] float4 rvk_vViewPos : VARY2;
    [[vk::location(3)]] float4 rvk_vPositionAndAlpha : VARY3;
    [[vk::location(4)]] float4 rvk_vNormalAndAO : VARY4;
    [[vk::location(5)]] float3 rvk_vTangent : VARY5;
    [[vk::location(6)]] float3 rvk_vBinormal : VARY6;
    [[vk::location(7)]] float2 rvk_vLightmap : VARY7;
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
    [[vk::location(15)]] float4 rvk_vPad15 : VARY15;
#endif
#ifdef RVK_SCENE_RECORD
    [[vk::location(14)]] nointerpolation uint rvk_sceneIdx : VARY14;  // abi.h kSceneVaryingLocation: the DrawRecord index (scene variant)
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
    input.vPos = rvk_in.aPos;
    input.vNorm = rvk_in.aNorm;
    input.vTangent = rvk_in.aTangent;
    input.vCol0 = rvk_in.aCol0;
    input.vCol1 = rvk_in.aCol1;
    input.vTex0 = rvk_in.aTex0;
    input.vLightmap = rvk_in.aLightmap;
#if XFX_VS_INPUT_TEXSETS > 1
    input.vTex1 = rvk_in.aTex1;
    input.vTex2 = rvk_in.aTex2;
    input.vTex3 = rvk_in.aTex3;
#else
    input.vTex1 = float2(0.0, 0.0);
    input.vTex2 = float2(0.0, 0.0);
    input.vTex3 = float2(0.0, 0.0);
#endif
#if RVK_NEED_INSTANCE
    input.vInstance = rvk_in.aInstance;
#else
    input.vInstance = 0.0;
#endif
#if RVK_NEED_BILLBOARD
    input.vBillboard = rvk_in.aBillboard;
#else
    input.vBillboard = float2(0.0, 0.0);
#endif
#if RVK_NEED_INDICES0
    input.vIndices0 = int4(rvk_in.aIndices0);
#else
    input.vIndices0 = int4(0, 0, 0, 0);
#endif
#if RVK_NEED_WEIGHTS0
    input.vWeights0 = rvk_in.aWeights0;
#else
    input.vWeights0 = float4(0.0, 0.0, 0.0, 0.0);
#endif
#if RVK_NEED_WEIGHTS1
    input.vWeights1 = rvk_in.aWeights1;
#else
    input.vWeights1 = float4(0.0, 0.0, 0.0, 0.0);
#endif
#if RVK_NEED_WEIGHTS2
    input.vWeights2 = rvk_in.aWeights2;
#else
    input.vWeights2 = float4(0.0, 0.0, 0.0, 0.0);
#endif
#if RVK_NEED_NORMDELTA12
    input.vNormDelta1 = rvk_in.aNormDelta1;
    input.vNormDelta2 = rvk_in.aNormDelta2;
#else
    input.vNormDelta1 = float3(0.0, 0.0, 0.0);
    input.vNormDelta2 = float3(0.0, 0.0, 0.0);
#endif
#if XFX_MORPHED
    input.vPosDelta1 = rvk_in.aPosDelta1;
    input.vPosDelta2 = rvk_in.aPosDelta2;
    input.vPosDelta3 = rvk_in.aPosDelta3;
    input.vNormDelta3 = rvk_in.aNormDelta3;
#else
    input.vPosDelta1 = float4(0.0, 0.0, 0.0, 0.0);
    input.vPosDelta2 = float4(0.0, 0.0, 0.0, 0.0);
    input.vPosDelta3 = float4(0.0, 0.0, 0.0, 0.0);
    input.vNormDelta3 = float3(0.0, 0.0, 0.0);
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

RvkVsOut VSMain(RvkVsIn rvk_in) {
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
    rvk_out.rvk_vTangent = o.vTangent;
    rvk_out.rvk_vBinormal = o.vBinormal;
    rvk_out.rvk_vLightmap = o.vLightmap;
    rvk_out.rvk_vColor0 = o.vColor0;
    // the translator's half-pixel fix
    float4 rvk_pos = o.vPos;
    rvk_pos.xy += rvk_draw.posFix.xy * rvk_pos.w;
    rvk_pos.z += rvk_draw.posFix.z * rvk_pos.z + rvk_draw.posFix.w * rvk_pos.w;  // reversed depth (posFix.zw, abi.h)
    rvk_out.position = rvk_pos;
    return rvk_out;
}
