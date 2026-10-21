// Layout checks of src/hal/types.h. Sizes/offsets proven by the decompile are static_asserts in the header; these tests
// check what the header claims on top: the HAL enums and structs are D3D9 pass-through, so they must match the
// Windows SDK's d3d9 definitions (the Vulkan side maps them exactly like a D3D9 driver).

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <d3d9.h>

#include <cstddef>

#include "hal/types.h"
#include "test.h"

using namespace rvk::hal;
using GS = eCGfxShared;

// Compare across enum types without C++20/23 deprecated mixed-enum comparisons.
#define CHECK_EQ_VALUE(a, b) CHECK(static_cast<long long>(a) == static_cast<long long>(b))

TEST(hal_enums_are_d3d9_values) {
    CHECK_EQ_VALUE(GS::eECmpFunc_Never, D3DCMP_NEVER);
    CHECK_EQ_VALUE(GS::eECmpFunc_LessEqual, D3DCMP_LESSEQUAL);
    CHECK_EQ_VALUE(GS::eECmpFunc_Always, D3DCMP_ALWAYS);
    CHECK_EQ_VALUE(GS::eEMasterCmpFunc_GreaterEqual, GS::eECmpFunc_GreaterEqual);

    CHECK_EQ_VALUE(GS::eEBlending_Zero, D3DBLEND_ZERO);
    CHECK_EQ_VALUE(GS::eEBlending_SrcAlpha, D3DBLEND_SRCALPHA);
    CHECK_EQ_VALUE(GS::eEBlending_InvSrcAlpha, D3DBLEND_INVSRCALPHA);
    CHECK_EQ_VALUE(GS::eEBlending_BlendFactor, D3DBLEND_BLENDFACTOR);
    CHECK_EQ_VALUE(GS::eEBlending_InvSrcColor2, 17);  // D3DBLEND_INVSRCCOLOR2 (D3D9Ex)
    CHECK_EQ_VALUE(GS::eEMasterBlending_InvDestColor, GS::eEBlending_InvDestColor);

    CHECK_EQ_VALUE(GS::eERenderMode_Wireframe, D3DFILL_WIREFRAME);
    CHECK_EQ_VALUE(GS::eERenderMode_Solid, D3DFILL_SOLID);
    CHECK_EQ_VALUE(GS::eEShadeMode_Gouraud, D3DSHADE_GOURAUD);
    CHECK_EQ_VALUE(GS::eECullingMode_None, D3DCULL_NONE);
    CHECK_EQ_VALUE(GS::eECullingMode_CW, D3DCULL_CW);
    CHECK_EQ_VALUE(GS::eECullingMode_CCW, D3DCULL_CCW);

    CHECK_EQ_VALUE(GS::eEViewportClear_Target, D3DCLEAR_TARGET);
    CHECK_EQ_VALUE(GS::eEViewportClear_ZBuffer, D3DCLEAR_ZBUFFER);
    CHECK_EQ_VALUE(GS::eEViewportClear_Stencil, D3DCLEAR_STENCIL);

    CHECK_EQ_VALUE(GS::eEPrimitiveType_PointList, D3DPT_POINTLIST);
    CHECK_EQ_VALUE(GS::eEPrimitiveType_TriangleList, D3DPT_TRIANGLELIST);
    CHECK_EQ_VALUE(GS::eEPrimitiveType_TriangleFan, D3DPT_TRIANGLEFAN);

    CHECK_EQ_VALUE(GS::eETexAddress_Wrap, D3DTADDRESS_WRAP);
    CHECK_EQ_VALUE(GS::eETexAddress_Clamp, D3DTADDRESS_CLAMP);
    CHECK_EQ_VALUE(GS::eETexAddress_MirrorOnce, D3DTADDRESS_MIRRORONCE);
    CHECK_EQ_VALUE(GS::eETexFilter_None, D3DTEXF_NONE);
    CHECK_EQ_VALUE(GS::eETexFilter_Anisotropic, D3DTEXF_ANISOTROPIC);
    CHECK_EQ_VALUE(GS::eETexFilter_ConvolutionMono, D3DTEXF_CONVOLUTIONMONO);

    CHECK_EQ_VALUE(GS::eESamplerState_AddressU, D3DSAMP_ADDRESSU);
    CHECK_EQ_VALUE(GS::eESamplerState_MinFilter, D3DSAMP_MINFILTER);
    CHECK_EQ_VALUE(GS::eESamplerState_MipFilter, D3DSAMP_MIPFILTER);
    CHECK_EQ_VALUE(GS::eESamplerState_MaxMipLevel, D3DSAMP_MAXMIPLEVEL);
    CHECK_EQ_VALUE(GS::eESamplerState_SRGBTexture, D3DSAMP_SRGBTEXTURE);
    CHECK_EQ_VALUE(GS::eESamplerState_DMapOffset, D3DSAMP_DMAPOFFSET);

    CHECK_EQ_VALUE(GS::eECubeMapFace_PositiveX, D3DCUBEMAP_FACE_POSITIVE_X);
    CHECK_EQ_VALUE(GS::eECubeMapFace_NegativeZ, D3DCUBEMAP_FACE_NEGATIVE_Z);

    CHECK_EQ_VALUE(GS::eEPoolType_Default, D3DPOOL_DEFAULT);
    CHECK_EQ_VALUE(GS::eEPoolType_Managed, D3DPOOL_MANAGED);
    CHECK_EQ_VALUE(GS::eEPoolType_SystemMem, D3DPOOL_SYSTEMMEM);
    CHECK_EQ_VALUE(GS::eEPoolType_Scratch, D3DPOOL_SCRATCH);

    CHECK_EQ_VALUE(GS::eEMaterialColorSrc_Color2, D3DMCS_COLOR2);
    CHECK_EQ_VALUE(GS::eETexCoordCalc_SphereMap, D3DTSS_TCI_SPHEREMAP);
    CHECK_EQ_VALUE(GS::eETexArgument_Texture, D3DTA_TEXTURE);
    CHECK_EQ_VALUE(GS::eETexArgument_AlphaReplicate, D3DTA_ALPHAREPLICATE);
    CHECK_EQ_VALUE(GS::eETexOperation_Modulate, D3DTOP_MODULATE);
    CHECK_EQ_VALUE(GS::eETexOperation_Lerp, D3DTOP_LERP);
}

TEST(hal_format_enums_are_d3dformat) {
    CHECK_EQ_VALUE(eEColorFormat_A8R8G8B8, D3DFMT_A8R8G8B8);
    CHECK_EQ_VALUE(eEColorFormat_X8R8G8B8, D3DFMT_X8R8G8B8);
    CHECK_EQ_VALUE(eEColorFormat_A4R4G4B4, D3DFMT_A4R4G4B4);
    CHECK_EQ_VALUE(eEColorFormat_R16F, D3DFMT_R16F);
    CHECK_EQ_VALUE(eEColorFormat_A16B16G16R16F, D3DFMT_A16B16G16R16F);
    CHECK_EQ_VALUE(eEColorFormat_G32R32F, D3DFMT_G32R32F);
    CHECK_EQ_VALUE(eEColorFormat_A32B32G32R32F, D3DFMT_A32B32G32R32F);
    CHECK_EQ_VALUE(eEColorFormat_DXT1, D3DFMT_DXT1);
    CHECK_EQ_VALUE(eEColorFormat_DXT5, D3DFMT_DXT5);

    CHECK_EQ_VALUE(GS::eEDepthFormat_D16, D3DFMT_D16);
    CHECK_EQ_VALUE(GS::eEDepthFormat_D24S8, D3DFMT_D24S8);
    CHECK_EQ_VALUE(GS::eEDepthFormat_D24X8, D3DFMT_D24X8);
    CHECK_EQ_VALUE(GS::eEDepthFormat_D32, D3DFMT_D32);
    CHECK_EQ_VALUE(GS::eEDepthFormat_D24FS8, D3DFMT_D24FS8);
    CHECK_EQ_VALUE(GS::eEDepthFormat_D32FLockable, D3DFMT_D32F_LOCKABLE);

    CHECK_EQ_VALUE(eEMultiSample_4, D3DMULTISAMPLE_4_SAMPLES);
    CHECK_EQ_VALUE(eEMultiSample_16, D3DMULTISAMPLE_16_SAMPLES);
    CHECK_EQ_VALUE(eEContextCreate_HardwareVertexProcessing, D3DCREATE_HARDWARE_VERTEXPROCESSING);
    CHECK_EQ_VALUE(eEContextCreate_FPUPreserve, D3DCREATE_FPU_PRESERVE);
}

TEST(hal_shader_type_bits) {
    // GetShaderType returns type & 1: every pixel type has bit 0 set, every vertex type not.
    CHECK_EQ_VALUE(GS::eEGfxHLSLShaderType_PS_3_0 & 1, GS::eEGfxHLSLShaderType_PS);
    CHECK_EQ_VALUE(GS::eEGfxHLSLShaderType_PS_2_0 & 1, GS::eEGfxHLSLShaderType_PS);
    CHECK_EQ_VALUE(GS::eEGfxHLSLShaderType_VS_3_0 & 1, GS::eEGfxHLSLShaderType_VS);
    CHECK_EQ_VALUE(GS::eEGfxHLSLShaderType_VS_1_1 & 1, GS::eEGfxHLSLShaderType_VS);
    CHECK((GS::eEGfxHLSLShaderType_PS_3_0 >> 8) == 8);
    CHECK((GS::eEGfxHLSLShaderType_VS_3_0 >> 8) == 4);
}

TEST(hal_structs_match_d3d9_structs) {
    CHECK(sizeof(GS::eSGfxDclElement) == sizeof(D3DVERTEXELEMENT9));
    CHECK(offsetof(GS::eSGfxDclElement, offset) == offsetof(D3DVERTEXELEMENT9, Offset));
    CHECK(offsetof(GS::eSGfxDclElement, type) == offsetof(D3DVERTEXELEMENT9, Type));
    CHECK(offsetof(GS::eSGfxDclElement, usageIndex) == offsetof(D3DVERTEXELEMENT9, UsageIndex));

    CHECK(sizeof(GS::eSGfxLockedRect) == sizeof(D3DLOCKED_RECT));
    CHECK(offsetof(GS::eSGfxLockedRect, bits) == offsetof(D3DLOCKED_RECT, pBits));

    CHECK(sizeof(GS::eSGfxGammaRamp) == sizeof(D3DGAMMARAMP));
    CHECK(sizeof(bCMatrix) == sizeof(D3DMATRIX));
    CHECK(sizeof(bCRect) == sizeof(RECT));
    CHECK(offsetof(bCRect, bottom) == offsetof(RECT, bottom));

    // eSGfxModeDesc: width/height/refresh at the D3DDISPLAYMODE offsets, format moved to +0x14.
    CHECK(offsetof(GS::eSGfxModeDesc, refreshRate) == offsetof(D3DDISPLAYMODE, RefreshRate));
}

TEST(hal_adapter_caps_match_d3dcaps9) {
    // eSGfxAdapterDesc + 0x418 holds D3DCAPS9 from offset 8 (DeviceType and AdapterOrdinal dropped).
    CHECK(sizeof(D3DCaps9Tail) + 8 == sizeof(D3DCAPS9));
#define RVK_CAPS(ours, theirs) CHECK(offsetof(D3DCaps9Tail, ours) + 8 == offsetof(D3DCAPS9, theirs))
    RVK_CAPS(caps, Caps);
    RVK_CAPS(textureFilterCaps, TextureFilterCaps);
    RVK_CAPS(maxTextureWidth, MaxTextureWidth);
    RVK_CAPS(maxAnisotropy, MaxAnisotropy);
    RVK_CAPS(maxVertexW, MaxVertexW);
    RVK_CAPS(extentsAdjust, ExtentsAdjust);
    RVK_CAPS(stencilCaps, StencilCaps);
    RVK_CAPS(maxSimultaneousTextures, MaxSimultaneousTextures);
    RVK_CAPS(maxPointSize, MaxPointSize);
    RVK_CAPS(maxStreams, MaxStreams);
    RVK_CAPS(vertexShaderVersion, VertexShaderVersion);
    RVK_CAPS(maxVertexShaderConst, MaxVertexShaderConst);
    RVK_CAPS(pixelShaderVersion, PixelShaderVersion);
    RVK_CAPS(pixelShader1xMaxValue, PixelShader1xMaxValue);
    RVK_CAPS(devCaps2, DevCaps2);
    RVK_CAPS(maxNpatchTessellationLevel, MaxNpatchTessellationLevel);
    RVK_CAPS(declTypes, DeclTypes);
    RVK_CAPS(numSimultaneousRTs, NumSimultaneousRTs);
    RVK_CAPS(stretchRectFilterCaps, StretchRectFilterCaps);
    RVK_CAPS(vs20Caps, VS20Caps);
    RVK_CAPS(ps20Caps, PS20Caps);
    RVK_CAPS(vertexTextureFilterCaps, VertexTextureFilterCaps);
    RVK_CAPS(maxPixelShader30InstructionSlots, MaxPixelShader30InstructionSlots);
#undef RVK_CAPS
    CHECK(offsetof(GS::eSGfxAdapterDesc, caps) + offsetof(D3DCaps9Tail, pixelShaderVersion) == 0x4dc);
}
