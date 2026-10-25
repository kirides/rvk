#version 460
// Risen material vertex shader: ge_default_vs.fx (+ ge_common_vs.fxh constants) in GLSL.
// Entry points of the HLSL: VS_Default (XFX_PASS = XFX_PASS_DEFAULT) and VS_DefaultZPass (XFX_PASS_ZPASS).
// HLSL matrices: float4x4 / float3x3 constants are column major, i.e. register k holds column k, so mul(v, M) is the row vector
// times the GLSL mat built from the registers as columns; M[i] (a row in HLSL) is written out as row(M, i).
// Vertex inputs are declared only when the active XFX_* macros read them, locations assigned in the order below
// (renderer side: vertex declaration usage -> location from this order; docs/research/shader-permutations.md).
// See ge_default.glsl for the defines.

#define RVK_VERTEX_SHADER
#include "ge_default.glsl"

// `input.vTex0` etc. as in the material's XFX_VS_TEXn expressions (`input` is reserved in GLSL)
#define input rvk_input

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

// ---- vertex inputs (sVSInput_Default); unread ones stay zero -------------------------------------------------------------------
layout(location = 0) in vec4 aPos;       // POSITION0 (w: billboard corner index for SpeedTree billboards)
layout(location = 1) in vec3 aNorm;      // NORMAL0
layout(location = 2) in vec3 aTangent;   // TANGENT0
layout(location = 3) in vec4 aCol0;      // COLOR0 (r: binormal sign)
layout(location = 4) in vec4 aCol1;      // COLOR1 (a: vertex ambient occlusion)
layout(location = 5) in vec2 aTex0;      // TEXCOORD0
layout(location = 6) in vec2 aLightmap;  // TEXCOORD6
#define RVK_N7 7
#if XFX_VS_INPUT_TEXSETS > 1
layout(location = RVK_N7) in vec2 aTex1;  // TEXCOORD1
layout(location = RVK_N7 + 1) in vec2 aTex2;
layout(location = RVK_N7 + 2) in vec2 aTex3;
#define RVK_N8 (RVK_N7 + 3)
#else
#define RVK_N8 RVK_N7
#endif
#if RVK_NEED_INSTANCE
layout(location = RVK_N8) in float aInstance;  // TEXCOORD4: instance index
#define RVK_N9 (RVK_N8 + 1)
#else
#define RVK_N9 RVK_N8
#endif
#if RVK_NEED_BILLBOARD
layout(location = RVK_N9) in vec2 aBillboard;  // TEXCOORD5: billboard u / v axis scale
#define RVK_N10 (RVK_N9 + 1)
#else
#define RVK_N10 RVK_N9
#endif
#if RVK_NEED_INDICES0
layout(location = RVK_N10) in vec4 aIndices0;  // BLENDINDICES0
#define RVK_N11 (RVK_N10 + 1)
#else
#define RVK_N11 RVK_N10
#endif
#if RVK_NEED_WEIGHTS0
layout(location = RVK_N11) in vec4 aWeights0;  // BLENDWEIGHT0
#define RVK_N12 (RVK_N11 + 1)
#else
#define RVK_N12 RVK_N11
#endif
#if RVK_NEED_WEIGHTS1
layout(location = RVK_N12) in vec4 aWeights1;  // BLENDWEIGHT1
#define RVK_N13 (RVK_N12 + 1)
#else
#define RVK_N13 RVK_N12
#endif
#if RVK_NEED_WEIGHTS2
layout(location = RVK_N13) in vec4 aWeights2;  // BLENDWEIGHT2
#define RVK_N14 (RVK_N13 + 1)
#else
#define RVK_N14 RVK_N13
#endif
#if RVK_NEED_NORMDELTA12
layout(location = RVK_N14) in vec3 aNormDelta1;  // NORMAL1
layout(location = RVK_N14 + 1) in vec3 aNormDelta2;  // NORMAL2
#define RVK_N15 (RVK_N14 + 2)
#else
#define RVK_N15 RVK_N14
#endif
#if XFX_MORPHED
layout(location = RVK_N15) in vec4 aPosDelta1;  // POSITION1
layout(location = RVK_N15 + 1) in vec4 aPosDelta2;  // POSITION2
layout(location = RVK_N15 + 2) in vec4 aPosDelta3;  // POSITION3
layout(location = RVK_N15 + 3) in vec3 aNormDelta3;  // NORMAL3
#endif

struct sVSInput_Default {
    vec4 vPos;
    vec3 vNorm;
    vec3 vTangent;
    vec4 vCol0;
    vec4 vCol1;
    vec2 vTex0;
    vec2 vTex1;
    vec2 vTex2;
    vec2 vTex3;
    float vInstance;
    vec2 vBillboard;
    vec2 vLightmap;
    vec4 vPosDelta1;
    vec3 vNormDelta1;
    vec4 vPosDelta2;
    vec3 vNormDelta2;
    vec4 vPosDelta3;
    vec3 vNormDelta3;
    ivec4 vIndices0;
    vec4 vWeights0;
    vec4 vWeights1;
    vec4 vWeights2;
};
sVSInput_Default rvk_input;

// ---- outputs: sVSOutput_Default (the pixel shader's inputs; vPos = gl_Position) ----------------------------------------------
layout(location = 0) out vec4 rvk_vTex0;
layout(location = 1) out vec4 rvk_vTex1;
layout(location = 2) out vec4 rvk_vViewPos;
layout(location = 3) out vec4 rvk_vPositionAndAlpha;
layout(location = 4) out vec4 rvk_vNormalAndAO;
layout(location = 5) out vec3 rvk_vTangent;
layout(location = 6) out vec3 rvk_vBinormal;
layout(location = 7) out vec2 rvk_vLightmap;
layout(location = 16) out vec4 rvk_vColor0;

struct sVSOutput_Default {
    vec4 vPos;
    vec4 vColor0;
    vec4 vTex0;
    vec4 vTex1;
    vec4 vViewPos;
    vec4 vPositionAndAlpha;
    vec4 vNormalAndAO;
    vec3 vTangent;
    vec3 vBinormal;
    vec2 vLightmap;
};

// ---- constants (ge_common_vs.fxh, ge_default.fxh) --------------------------------------------------------------------------------
vec4 vsC(int i) { return uint(i) < 256u ? pc.vs.c[i] : vec4(0.0); }
#define g_vViewProjection mat4(pc.vs.c[0], pc.vs.c[1], pc.vs.c[2], pc.vs.c[3])
#define g_vInvView mat3(pc.vs.c[8].xyz, pc.vs.c[9].xyz, pc.vs.c[10].xyz)
#define g_vCamPosAndFar pc.vs.c[11]
#define g_vCamPos g_vCamPosAndFar.xyz
#define g_vFar g_vCamPosAndFar.w
#define g_vVegetationData1 pc.vs.c[12]
#define g_vVegetationTimeStamp g_vVegetationData1.x
#define g_vVegetationWindStrength g_vVegetationData1.y
#define g_vVegetationBendWeight g_vVegetationData1.z
#define g_vVegetationWaveSpeed g_vVegetationData1.w
#define g_vInstanceMisc pc.vs.c[31]
#define g_vVegetationDisolve g_vInstanceMisc.xy
#define g_vMorphTargetWeights g_vInstanceMisc.xyz
// g_vInstanceData[i] = c(32 + i); g_vShaderData[i] = c(232 + i) (material texture coordinate transforms)
vec4 instData(int i) { return vsC(32 + i); }
vec4 instData(float i) { return vsC(32 + int(i)); }
#define g_vInstanceData(i) instData(i)
vec4 g_vShaderData[20];  // c232..: the material's texture coordinate transforms (XFX_VS_TEXn expressions)
mat3 windMatrix(int i) { return mat3(vsC(14 + 3 * i).xyz, vsC(15 + 3 * i).xyz, vsC(16 + 3 * i).xyz); }
#define g_vLeafAngleMatrix mat3(pc.vs.c[23].xyz, pc.vs.c[24].xyz, pc.vs.c[25].xyz)
#define g_vLeafAngles_c(i) vsC(26 + (i))

// SpeedTree billboard constants
#define c_f2PI g_vInstanceData(0).z
#define c_fClearAlpha g_vInstanceData(0).w
#define c_fOpaqueAlpha g_vInstanceData(1).z
#define c_fAlphaSpread g_vInstanceData(1).w
#define c_fMinAlpha g_vInstanceData(2).z

vec3 row(mat3 m, int i) { return vec3(m[0][i], m[1][i], m[2][i]); }
float fmodf(float x, float y) { return x - y * trunc(x / y); }

// ---- special output (global alpha, SpeedTree billboards) ------------------------------------------------------------------------
void GetSpecialOutputData(inout sVSOutput_Default a_Output) {
    float GlobalAlpha = 1.0;

#if XFX_ST_BILLBOARDS
    // fading / fizzling between the 360 degree images
    float fFadeInStartDist = input.vWeights1.x;
    float fFadeInRange = input.vWeights1.y;
    float fTransitionPercent = input.vWeights1.z;

    vec3 vecCamDelta = g_vCamPos - input.vPos.xyz;
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
        vec3 vecCamToTree = nrmD3D(input.vPos.xyz - g_vCamPos);
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
        vec2 vecTexCoordsOut;
        vec4 vecTexCoords = g_vInstanceData(iTexCoordOffset + iImage360);
        vecTexCoordsOut.x = vecTexCoords.x - vecTexCoords.z * g_vInstanceData(iCorner).x;
        vecTexCoordsOut.y = vecTexCoords.y - vecTexCoords.w * g_vInstanceData(iCorner).y;

        a_Output.vTex0.xy = vecTexCoordsOut;
        a_Output.vTex1.xy = vecTexCoordsOut;
        a_Output.vTex0.zw = vecTexCoordsOut;
        a_Output.vTex1.zw = vecTexCoordsOut;

        // fading between two adjacent images
        float fFadePoint = mix(c_fClearAlpha, c_fOpaqueAlpha, fDistanceFade);

        float fAlpha;
#if XFX_ST_BILLBOARDS_VERT1
        fAlpha = 1.0 - fmodf(fAzimuth, fSliceDiameter) / fSliceDiameter;
#else
        fAlpha = (fAzimuth - (float(iImage360) * fSliceDiameter)) / fSliceDiameter;
#endif

        fAlpha = max(fAlpha, fTransitionPercent);
        fAlpha = mix(fFadePoint, c_fClearAlpha, powD3D((fAlpha - fTransitionPercent) / (1.0 - fTransitionPercent), 1.7));

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
    GlobalAlpha = mix(0.33, 1.0, clamp(fValue, 0.0, 1.0));
#elif XFX_VEGETATION
    // vegetation dissolve
    vec3 CamDelta = g_vCamPos - a_Output.vPositionAndAlpha.xyz;
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
    vec4 r0, r1, r2;
};

WorldMat GetMatrixFromInstanceData(int a_iOffset) {
    return WorldMat(g_vInstanceData(a_iOffset), g_vInstanceData(a_iOffset + 1), g_vInstanceData(a_iOffset + 2));
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
vec3 qrot(vec4 q, vec3 v) { return v + 2.0 * cross(q.xyz, cross(q.xyz, v) + q.w * v); }
vec4 SmoothCurve(vec4 x) { return x * x * (3.0 - 2.0 * x); }
vec4 TriangleWave(vec4 x) { return abs(fract(x + 0.5) * 2.0 - 1.0); }

// ---- Transform --------------------------------------------------------------------------------------------------------------------
void Transform(out vec3 a_vecWorldPosition, out vec3 a_vecWorldNormal, out vec3 a_vecWorldTangent, out vec3 a_vecWorldBinormal) {
    WorldMat WorldMatrix = GetWorldMatrix();

    vec3 WorldPosOffset = vec3(0.0);
    vec4 Position = input.vPos;
    vec3 Normal = input.vNorm;
    vec3 Tangent = input.vTangent;

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
    Position.xyz = mix(input.vPos.xyz, input.vPos.xyz * windMatrix(input.vIndices0.x), input.vWeights0.x);

#if XFX_ST_LEAFMESH
    // placement using speedwind leaf rocking
    vec2 RockAndRustle = g_vInstanceData(input.vInstance * 4.0).xy * g_vLeafAngles_c(int(input.vWeights2.w)).xy;
    mat3 RockRustle = RotationMatrix_xAxis(RockAndRustle.x);  // rock
    mat3 MeshMatrix = transpose(mat3(input.vNormDelta1, -cross(input.vNormDelta1, input.vNormDelta2), input.vNormDelta2));

    WorldPosOffset = input.vWeights2.xyz;
    WorldPosOffset = RockRustle * WorldPosOffset;
    WorldPosOffset = MeshMatrix * WorldPosOffset;

    Normal = MeshMatrix * Normal;
    Tangent = MeshMatrix * Tangent;
#elif XFX_ST_LEAFCARDS
    vec2 RockAndRustle = g_vInstanceData(input.vInstance * 4.0).xy * g_vLeafAngles_c(int(input.vWeights2.w)).xy;
    WorldPosOffset = vec3(input.vWeights1.yx * input.vWeights2.yz, 0.0);

#if XFX_SHADOWMAPPASS
    // shadow map pass: offset into the z axis
    WorldPosOffset.z = dot(WorldPosOffset.xy, vec2(0.5));
#endif

    WorldPosOffset = RotationMatrix_zAxis(input.vWeights1.w + RockAndRustle.x) * WorldPosOffset;
    WorldPosOffset = RotationMatrix_yAxis(input.vWeights1.z + RockAndRustle.y) * WorldPosOffset;
    WorldPosOffset = g_vLeafAngleMatrix * WorldPosOffset;
#endif
#endif

#if XFX_VEGETATION
    vec3 vInstanceOffset = input.vWeights0.xyz;
    vec4 qInstanceRotation = input.vWeights1;
    vec2 vInstanceScaling = input.vWeights2.xy;
    float fNormalBendFactor = input.vWeights2.z;
    float fWindStrength = input.vWeights0.w;

    // instance scaling / rotation / position
    a_vecWorldPosition = Position.xyz * vInstanceScaling.xyx;
    a_vecWorldPosition = qrot(qInstanceRotation, a_vecWorldPosition);

    float fObjPhase = length(vInstanceOffset);
    vec2 vWavesIn = vec2(g_vVegetationTimeStamp);
    vWavesIn += fObjPhase * 0.02;

    vec4 vWaves = (fract((vWavesIn.x + vWavesIn.y) * vec4(0.89, 0.5168, 0.8713, 0.61341) * 0.1) * 2.0 - 1.0) * g_vVegetationWaveSpeed;
    vWaves = SmoothCurve(TriangleWave(vWaves));

    vec2 vWavesSum = vWaves.xz + vWaves.yw;
    vec2 vBend = vWavesSum.xy * g_vVegetationWindStrength * fWindStrength;

    // bend factor
    vec2 fBendFactor = vec2(a_vecWorldPosition.y * g_vVegetationBendWeight + 1.0);

    // smooth bend factor
    fBendFactor *= fBendFactor;
    fBendFactor = fBendFactor * fBendFactor - fBendFactor;

    // rotate in the wind
    float fLength = length(a_vecWorldPosition);
    vec3 vecNewPos = a_vecWorldPosition;
    vecNewPos.xz += vBend.xy * fBendFactor;
    a_vecWorldPosition = nrmD3D(vecNewPos) * fLength;

    a_vecWorldPosition += vInstanceOffset;

    // bent normal
    Normal = mix(Normal, vec3(0.0, 1.0, 0.0), fNormalBendFactor);

    a_vecWorldNormal = nrmD3D(qrot(qInstanceRotation, Normal));
    a_vecWorldTangent = vec3(0.0);
    a_vecWorldBinormal = vec3(0.0);
#elif XFX_BILLBOARD
    // screen aligned billboard
    a_vecWorldPosition = vec3(dot(Position, WorldMatrix.r0), dot(Position, WorldMatrix.r1), dot(Position, WorldMatrix.r2));
    a_vecWorldPosition += row(g_vInvView, 0) * input.vBillboard.x;
    a_vecWorldPosition += row(g_vInvView, 1) * input.vBillboard.y;
    a_vecWorldNormal = nrmD3D(input.vNorm * g_vInvView);
    a_vecWorldTangent = nrmD3D(input.vTangent * g_vInvView);
    a_vecWorldBinormal = cross(a_vecWorldNormal, a_vecWorldTangent);
#elif XFX_ST_BILLBOARDS
    // SpeedTree special billboard
    a_vecWorldPosition = Position.xyz;

    // the vector from the tree's pivot to the camera is the billboard's normal
    vec3 vecTreeToCam = g_vCamPos - Position.xyz;
    vecTreeToCam.y = 0.0;
    vec3 vecBBNormal = nrmD3D(vecTreeToCam);
    vec3 vecBB_Y = input.vWeights2.xyz;
    vec3 vecBB_X = cross(vecBBNormal, vecBB_Y);

    a_vecWorldPosition += vecBB_X * input.vBillboard.x;
    a_vecWorldPosition += vecBB_Y * input.vBillboard.y;

    a_vecWorldNormal = vecBBNormal;
    a_vecWorldTangent = -vecBB_X;  // invert X for the billboard normals
    a_vecWorldBinormal = vecBB_Y;
#else
    // into world space
    a_vecWorldPosition = vec3(dot(Position, WorldMatrix.r0), dot(Position, WorldMatrix.r1), dot(Position, WorldMatrix.r2)) + WorldPosOffset;
    a_vecWorldNormal = nrmD3D(vec3(dot(Normal, WorldMatrix.r0.xyz), dot(Normal, WorldMatrix.r1.xyz), dot(Normal, WorldMatrix.r2.xyz)));
    a_vecWorldTangent = nrmD3D(vec3(dot(Tangent, WorldMatrix.r0.xyz), dot(Tangent, WorldMatrix.r1.xyz), dot(Tangent, WorldMatrix.r2.xyz)));
    a_vecWorldBinormal = cross(a_vecWorldNormal, a_vecWorldTangent);

    // right handed binormals: the sign is stored in the red component of the diffuse colour
    a_vecWorldBinormal *= (input.vCol0.r * -2.0 + 1.0);
#endif
}

void loadInputs() {
    for (int rvk_i = 0; rvk_i < 20; ++rvk_i) g_vShaderData[rvk_i] = pc.vs.c[232 + rvk_i];
    rvk_input.vPos = aPos;
    rvk_input.vNorm = aNorm;
    rvk_input.vTangent = aTangent;
    rvk_input.vCol0 = aCol0;
    rvk_input.vCol1 = aCol1;
    rvk_input.vTex0 = aTex0;
    rvk_input.vLightmap = aLightmap;
#if XFX_VS_INPUT_TEXSETS > 1
    rvk_input.vTex1 = aTex1;
    rvk_input.vTex2 = aTex2;
    rvk_input.vTex3 = aTex3;
#else
    rvk_input.vTex1 = vec2(0.0);
    rvk_input.vTex2 = vec2(0.0);
    rvk_input.vTex3 = vec2(0.0);
#endif
#if RVK_NEED_INSTANCE
    rvk_input.vInstance = aInstance;
#else
    rvk_input.vInstance = 0.0;
#endif
#if RVK_NEED_BILLBOARD
    rvk_input.vBillboard = aBillboard;
#else
    rvk_input.vBillboard = vec2(0.0);
#endif
#if RVK_NEED_INDICES0
    rvk_input.vIndices0 = ivec4(aIndices0);
#else
    rvk_input.vIndices0 = ivec4(0);
#endif
#if RVK_NEED_WEIGHTS0
    rvk_input.vWeights0 = aWeights0;
#else
    rvk_input.vWeights0 = vec4(0.0);
#endif
#if RVK_NEED_WEIGHTS1
    rvk_input.vWeights1 = aWeights1;
#else
    rvk_input.vWeights1 = vec4(0.0);
#endif
#if RVK_NEED_WEIGHTS2
    rvk_input.vWeights2 = aWeights2;
#else
    rvk_input.vWeights2 = vec4(0.0);
#endif
#if RVK_NEED_NORMDELTA12
    rvk_input.vNormDelta1 = aNormDelta1;
    rvk_input.vNormDelta2 = aNormDelta2;
#else
    rvk_input.vNormDelta1 = vec3(0.0);
    rvk_input.vNormDelta2 = vec3(0.0);
#endif
#if XFX_MORPHED
    rvk_input.vPosDelta1 = aPosDelta1;
    rvk_input.vPosDelta2 = aPosDelta2;
    rvk_input.vPosDelta3 = aPosDelta3;
    rvk_input.vNormDelta3 = aNormDelta3;
#else
    rvk_input.vPosDelta1 = vec4(0.0);
    rvk_input.vPosDelta2 = vec4(0.0);
    rvk_input.vPosDelta3 = vec4(0.0);
    rvk_input.vNormDelta3 = vec3(0.0);
#endif
}

#if XFX_PASS == XFX_PASS_DEFAULT

// VS_Default
sVSOutput_Default VS_Default() {
    sVSOutput_Default output_;

    vec3 vecWorldPosition, vecWorldNormal, vecWorldTangent, vecWorldBinormal;
    Transform(vecWorldPosition, vecWorldNormal, vecWorldTangent, vecWorldBinormal);

    // texture coordinates
    output_.vTex0.xy = XFX_VS_TEX0;
    output_.vTex1.xy = XFX_VS_TEX1;
    output_.vTex0.zw = XFX_VS_TEX2;
    output_.vTex1.zw = XFX_VS_TEX3;

    output_.vLightmap = input.vLightmap;

    // project
    output_.vPos = vec4(vecWorldPosition, 1.0) * g_vViewProjection;
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
    sVSOutput_Default output_;
    output_.vPos = vec4(0.0);
    output_.vColor0 = vec4(0.0);
    output_.vTex0 = vec4(0.0);
    output_.vTex1 = vec4(0.0);
    output_.vViewPos = vec4(0.0);
    output_.vPositionAndAlpha = vec4(0.0);
    output_.vNormalAndAO = vec4(0.0);
    output_.vTangent = vec3(0.0);
    output_.vBinormal = vec3(0.0);
    output_.vLightmap = vec2(0.0);

    vec3 vecWorldPosition, vecWorldNormal, vecWorldTangent, vecWorldBinormal;
    Transform(vecWorldPosition, vecWorldNormal, vecWorldTangent, vecWorldBinormal);

    // texture coordinates only for the masked ones
#if XFX_MASKED
    output_.vTex0.xy = XFX_VS_TEX0;
    output_.vTex1.xy = XFX_VS_TEX1;
    output_.vTex0.zw = XFX_VS_TEX2;
    output_.vTex1.zw = XFX_VS_TEX3;
#endif

    output_.vPos = vec4(vecWorldPosition, 1.0) * g_vViewProjection;
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

void main() {
    loadInputs();
#if XFX_PASS == XFX_PASS_DEFAULT
    sVSOutput_Default o = VS_Default();
#else
    sVSOutput_Default o = VS_DefaultZPass();
#endif
    rvk_vTex0 = o.vTex0;
    rvk_vTex1 = o.vTex1;
    rvk_vViewPos = o.vViewPos;
    rvk_vPositionAndAlpha = o.vPositionAndAlpha;
    rvk_vNormalAndAO = o.vNormalAndAO;
    rvk_vTangent = o.vTangent;
    rvk_vBinormal = o.vBinormal;
    rvk_vLightmap = o.vLightmap;
    rvk_vColor0 = o.vColor0;
    // the translator's half-pixel fix
    vec4 rvk_pos = o.vPos;
    rvk_pos.xy += pc.draw.posFix.xy * rvk_pos.w;
    rvk_pos.z += pc.draw.posFix.z * rvk_pos.z + pc.draw.posFix.w * rvk_pos.w;  // reversed depth (posFix.zw, abi.h)
    gl_Position = rvk_pos;
}
