#pragma once

// Layouts of the engine structs and enums that cross the HAL (filled in from the decompiled eCDX9* layer).
//
// Source: headless-Ghidra decompile of Engine.dll's DX9 layer (B:\DevDrive\risen-gfx-re\dx9_layer.txt) and of the callers
// (eCGfxContextAdmin, eCGfxAdmin, eCGfxImageData). Every RVA below is an Engine.dll RVA. The engine never translates these
// enums: the eCDX9* layer passes them straight to D3D9 / D3DX, so every enumerator *is* the D3D9 value named in its comment
// (the Vulkan side maps exactly like a D3D9 driver would). Full per-slot semantics: docs/research/hal-structs.md.
//
// Enumerator names: the engine only reveals names for eEColorFormat (reflection strings "eEColorFormat_*"); all other names
// are descriptive (they follow the D3D9 constant, as the unofficial Gothic 3 SDK by georgeto names the same Genome enums).
// "[I]" = inferred (not proven by code that reads/writes the member); everything else is confirmed by the cited function.

#include <cstddef>
#include <cstdint>

#include "hal/api_generated.h"

namespace rvk::hal {

// ---------------------------------------------------------------------------------------------------------------------
// Result codes besides bEResult_False/bEResult_Ok (types_base.h). Returned by the file-reading slots
// (PreprocessShader 0x73e780, eCAPIHLSLShader::Compile 0x749810, CompileEffectFromFile 0x743cf0). Names [I].
inline constexpr bEResult bEResult_FileOpenFailed = static_cast<bEResult>(4);   // eCArchiveFile::Open failed
inline constexpr bEResult bEResult_FileNotFound = static_cast<bEResult>(10);    // archive manager has no such file

// ---------------------------------------------------------------------------------------------------------------------
// Engine string members (bCString / bCUnicodeString): 8 bytes, a pointer to the engine's ref-counted text buffer (the int
// length sits at text - 8, PreprocessShader 0x73e780). Read the text through the pointer (may be null = empty); never
// write it directly: assign through SharedBase's bCString::operator=(char const*) / bCUnicodeString::SetText so the
// engine's ref counting stays intact. Copying the struct copies the pointer only (no AddRef): copy is deleted.
struct EngineString {
    char* text;
    EngineString() = default;
    EngineString(const EngineString&) = delete;
    EngineString& operator=(const EngineString&) = delete;
};
struct EngineUnicodeString {
    wchar_t* text;
    EngineUnicodeString() = default;
    EngineUnicodeString(const EngineUnicodeString&) = delete;
    EngineUnicodeString& operator=(const EngineUnicodeString&) = delete;
};
static_assert(sizeof(EngineString) == 8 && sizeof(EngineUnicodeString) == 8);

// ---------------------------------------------------------------------------------------------------------------------
// SharedBase math/geometry classes that cross the HAL (forward-declared in api_generated.h).

// bCRect: copied member by member into a RECT (SetScissor 0x740310, SwapScreen 0x7407b0, CopyCurrentRTToImage 0x74d450).
class bCRect {
public:
    std::int32_t left, top, right, bottom;
};
static_assert(sizeof(bCRect) == 0x10);

// bCMatrix: handed to IDirect3DDevice9::SetTransform / ID3DXConstantTable::SetMatrix unchanged (SetWorldMatrix 0x741d10,
// SetConstantMatrix 0x74a1d0), so it is a D3DMATRIX: 16 floats, m[row][col], row vectors (v' = v * M, translation in
// m[3][0..2]). HLSL with default column_major packing receives the transpose (D3DX handles that in SetMatrix).
class bCMatrix {
public:
    float m[4][4];
};
static_assert(sizeof(bCMatrix) == 64);

// bCVector4: D3DXVECTOR4 (ID3DXConstantTable::SetVectorArray, 0x74a6b0).
class bCVector4 {
public:
    float x, y, z, w;
};
static_assert(sizeof(bCVector4) == 16);

// bCVector: D3DXVECTOR3 (ID3DXSprite::Draw center/position, AddSpriteToSpriteList 0x746eb0) [I].
class bCVector {
public:
    float x, y, z;
};
static_assert(sizeof(bCVector) == 12);

// bCVector2 [I].
class bCVector2 {
public:
    float x, y;
};
static_assert(sizeof(bCVector2) == 8);

// Color as embedded in eSAPIMaterial / eSGfxLightData (bCFloatAlphaColor [I]): the DX9 layer reads r,g,b at +8,+0xc,+0x10
// and alpha at +0x18 of every 0x20-byte color (SetMaterial 0x742730, SetLights 0x7422a0). The first 8 bytes are presumably
// a vtable pointer of bCFloatColor [I].
struct EngineFloatAlphaColor {
    void* vtable;  // [I]
    float r, g, b;
    std::uint32_t pad0;  // [I]
    float a;
    std::uint32_t pad1;  // [I]
};
static_assert(sizeof(EngineFloatAlphaColor) == 0x20);
static_assert(offsetof(EngineFloatAlphaColor, r) == 8 && offsetof(EngineFloatAlphaColor, a) == 0x18);

// =====================================================================================================================
// Enums (all completed out of class against the opaque declarations in eCGfxShared)
// =====================================================================================================================

// eEAPIType: eCGfxAdmin::InitializeAdmin 0x74e510 creates an eCDX9Instance only for 3; eCDX9Instance::Init 0x74b140
// fails for anything else.
enum eCGfxShared::eEAPIType : int {
    eEAPIType_Invalid = 0,  // [I]
    eEAPIType_Dummy = 1,    // [I] (Gothic 3 SDK name)
    eEAPIType_DX9 = 3,
};

// Comparison functions = D3DCMPFUNC (SetDepthTesting 0x7417b0 -> D3DRS_ZFUNC unchanged; eSGfxStencilData::func,
// eSGfxAlphaTestData::func -> D3DRS_STENCILFUNC / D3DRS_ALPHAFUNC unchanged).
enum eCGfxShared::eECmpFunc : int {
    eECmpFunc_Never = 1,         // D3DCMP_NEVER        -> VK_COMPARE_OP_NEVER
    eECmpFunc_Less = 2,          // D3DCMP_LESS         -> VK_COMPARE_OP_LESS
    eECmpFunc_Equal = 3,         // D3DCMP_EQUAL        -> VK_COMPARE_OP_EQUAL
    eECmpFunc_LessEqual = 4,     // D3DCMP_LESSEQUAL    -> VK_COMPARE_OP_LESS_OR_EQUAL
    eECmpFunc_Greater = 5,       // D3DCMP_GREATER      -> VK_COMPARE_OP_GREATER
    eECmpFunc_NotEqual = 6,      // D3DCMP_NOTEQUAL     -> VK_COMPARE_OP_NOT_EQUAL
    eECmpFunc_GreaterEqual = 7,  // D3DCMP_GREATEREQUAL -> VK_COMPARE_OP_GREATER_OR_EQUAL
    eECmpFunc_Always = 8,        // D3DCMP_ALWAYS       -> VK_COMPARE_OP_ALWAYS
};
// Master variant: 0 = no master (release the override), any other value = the eECmpFunc to force (0x740ca0).
enum eCGfxShared::eEMasterCmpFunc : int {
    eEMasterCmpFunc_NoMaster = 0,
    eEMasterCmpFunc_Never = 1,
    eEMasterCmpFunc_Less = 2,
    eEMasterCmpFunc_Equal = 3,
    eEMasterCmpFunc_LessEqual = 4,
    eEMasterCmpFunc_Greater = 5,
    eEMasterCmpFunc_NotEqual = 6,
    eEMasterCmpFunc_GreaterEqual = 7,
    eEMasterCmpFunc_Always = 8,
};

// Blend factors = D3DBLEND (SetBlending 0x742960 / ActivateBlending 0x286260 -> D3DRS_SRCBLEND / D3DRS_DESTBLEND unchanged).
enum eCGfxShared::eEBlending : int {
    eEBlending_Zero = 1,             // D3DBLEND_ZERO            -> VK_BLEND_FACTOR_ZERO
    eEBlending_One = 2,              // D3DBLEND_ONE             -> VK_BLEND_FACTOR_ONE
    eEBlending_SrcColor = 3,         // D3DBLEND_SRCCOLOR        -> VK_BLEND_FACTOR_SRC_COLOR
    eEBlending_InvSrcColor = 4,      // D3DBLEND_INVSRCCOLOR     -> VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR
    eEBlending_SrcAlpha = 5,         // D3DBLEND_SRCALPHA        -> VK_BLEND_FACTOR_SRC_ALPHA
    eEBlending_InvSrcAlpha = 6,      // D3DBLEND_INVSRCALPHA     -> VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA
    eEBlending_DestAlpha = 7,        // D3DBLEND_DESTALPHA       -> VK_BLEND_FACTOR_DST_ALPHA
    eEBlending_InvDestAlpha = 8,     // D3DBLEND_INVDESTALPHA    -> VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA
    eEBlending_DestColor = 9,        // D3DBLEND_DESTCOLOR       -> VK_BLEND_FACTOR_DST_COLOR
    eEBlending_InvDestColor = 10,    // D3DBLEND_INVDESTCOLOR    -> VK_BLEND_FACTOR_ONE_MINUS_DST_COLOR
    eEBlending_SrcAlphaSat = 11,     // D3DBLEND_SRCALPHASAT     -> VK_BLEND_FACTOR_SRC_ALPHA_SATURATE
    eEBlending_BothSrcAlpha = 12,    // D3DBLEND_BOTHSRCALPHA    -> src SRC_ALPHA, dst ONE_MINUS_SRC_ALPHA (as source only)
    eEBlending_BothInvSrcAlpha = 13, // D3DBLEND_BOTHINVSRCALPHA -> src ONE_MINUS_SRC_ALPHA, dst SRC_ALPHA
    eEBlending_BlendFactor = 14,     // D3DBLEND_BLENDFACTOR     -> VK_BLEND_FACTOR_CONSTANT_COLOR (D3DRS_BLENDFACTOR)
    eEBlending_InvBlendFactor = 15,  // D3DBLEND_INVBLENDFACTOR  -> VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_COLOR
    eEBlending_SrcColor2 = 16,       // D3DBLEND_SRCCOLOR2 (D3D9Ex) -> VK_BLEND_FACTOR_SRC1_COLOR
    eEBlending_InvSrcColor2 = 17,    // D3DBLEND_INVSRCCOLOR2    -> VK_BLEND_FACTOR_ONE_MINUS_SRC1_COLOR
};
// Master variant (SetMasterBlending 0x740f20): both factors non-zero = force, either 0 = release.
enum eCGfxShared::eEMasterBlending : int {
    eEMasterBlending_NoMaster = 0,
    eEMasterBlending_Zero = 1,
    eEMasterBlending_One = 2,
    eEMasterBlending_SrcColor = 3,
    eEMasterBlending_InvSrcColor = 4,
    eEMasterBlending_SrcAlpha = 5,
    eEMasterBlending_InvSrcAlpha = 6,
    eEMasterBlending_DestAlpha = 7,
    eEMasterBlending_InvDestAlpha = 8,
    eEMasterBlending_DestColor = 9,
    eEMasterBlending_InvDestColor = 10,
    eEMasterBlending_SrcAlphaSat = 11,
    eEMasterBlending_BothSrcAlpha = 12,
    eEMasterBlending_BothInvSrcAlpha = 13,
    eEMasterBlending_BlendFactor = 14,
    eEMasterBlending_InvBlendFactor = 15,
    eEMasterBlending_SrcColor2 = 16,
    eEMasterBlending_InvSrcColor2 = 17,
};

// Fill mode = D3DFILLMODE (SetRenderMode 0x7415d0 -> D3DRS_FILLMODE unchanged).
enum eCGfxShared::eERenderMode : int {
    eERenderMode_Point = 1,      // D3DFILL_POINT     -> VK_POLYGON_MODE_POINT
    eERenderMode_Wireframe = 2,  // D3DFILL_WIREFRAME -> VK_POLYGON_MODE_LINE
    eERenderMode_Solid = 3,      // D3DFILL_SOLID     -> VK_POLYGON_MODE_FILL
};
enum eCGfxShared::eEMasterRenderMode : int {
    eEMasterRenderMode_NoMaster = 0,
    eEMasterRenderMode_Point = 1,
    eEMasterRenderMode_Wireframe = 2,
    eEMasterRenderMode_Solid = 3,
};

// Shade mode = D3DSHADEMODE (SetShadeMode 0x741670 -> D3DRS_SHADEMODE). Only matters for fixed function / flat interpolation.
enum eCGfxShared::eEShadeMode : int {
    eEShadeMode_Flat = 1,     // D3DSHADE_FLAT
    eEShadeMode_Gouraud = 2,  // D3DSHADE_GOURAUD
    eEShadeMode_Phong = 3,    // D3DSHADE_PHONG (unsupported by D3D9 drivers, behaves as Gouraud)
};
enum eCGfxShared::eEMasterShadeMode : int {
    eEMasterShadeMode_NoMaster = 0,
    eEMasterShadeMode_Flat = 1,
    eEMasterShadeMode_Gouraud = 2,
    eEMasterShadeMode_Phong = 3,
};

// Culling = D3DCULL (SetCullingMode 0x741710 -> D3DRS_CULLMODE unchanged). D3D9 front faces are clockwise in window space.
enum eCGfxShared::eECullingMode : int {
    eECullingMode_None = 1,  // D3DCULL_NONE -> VK_CULL_MODE_NONE
    eECullingMode_CW = 2,    // D3DCULL_CW   -> cull clockwise triangles
    eECullingMode_CCW = 3,   // D3DCULL_CCW  -> cull counter-clockwise triangles (D3D9 default)
};
enum eCGfxShared::eEMasterCullingMode : int {
    eEMasterCullingMode_NoMaster = 0,
    eEMasterCullingMode_None = 1,
    eEMasterCullingMode_CW = 2,
    eEMasterCullingMode_CCW = 3,
};

// Depth write (SetDepthWrite 0x741850: D3DRS_ZWRITEENABLE = (value == 2)).
enum eCGfxShared::eEWrite : int {
    eEWrite_Disabled = 1,
    eEWrite_Enabled = 2,
};
enum eCGfxShared::eEMasterWrite : int {
    eEMasterWrite_NoMaster = 0,
    eEMasterWrite_Disabled = 1,
    eEMasterWrite_Enabled = 2,
};

// Stencil test (SetStencilTesting 0x7419f0: D3DRS_STENCILENABLE = (value == 2); the data is only applied when enabled).
enum eCGfxShared::eEStencil : int {
    eEStencil_Disabled = 1,
    eEStencil_Enabled = 2,
};
enum eCGfxShared::eEMasterStencil : int {
    eEMasterStencil_NoMaster = 0,
    eEMasterStencil_Disabled = 1,
    eEMasterStencil_Enabled = 2,
};

// Alpha test (SetAlphaTesting 0x742a50: D3DRS_ALPHATESTENABLE = (value == 2); ref/func only applied when enabled).
enum eCGfxShared::eEAlphaTest : int {
    eEAlphaTest_Disabled = 1,
    eEAlphaTest_Enabled = 2,
};
enum eCGfxShared::eEMasterAlphaTest : int {
    eEMasterAlphaTest_NoMaster = 0,
    eEMasterAlphaTest_Disabled = 1,
    eEMasterAlphaTest_Enabled = 2,
};

// Master fog (SetMasterFogging 0x741040): only 1 has an effect (D3DRS_FOGENABLE = 0 and SetFog is ignored from then on).
enum eCGfxShared::eEMasterFogging : int {
    eEMasterFogging_NoMaster = 0,
    eEMasterFogging_Disable = 1,
};

// Clear flags = D3DCLEAR_* (ClearViewport 0x740710 -> IDirect3DDevice9::Clear(0, NULL, flags, color, z, stencil)).
enum eCGfxShared::eEViewportClear : int {
    eEViewportClear_Target = 1,   // D3DCLEAR_TARGET  (all bound color targets)
    eEViewportClear_ZBuffer = 2,  // D3DCLEAR_ZBUFFER
    eEViewportClear_Stencil = 4,  // D3DCLEAR_STENCIL
};

// Primitive type = D3DPRIMITIVETYPE (Draw 0x741f40, DrawUP 0x742080, DrawIndexedUP 0x742160, eSGfxDrawCallDesc::type).
enum eCGfxShared::eEPrimitiveType : int {
    eEPrimitiveType_PointList = 1,      // D3DPT_POINTLIST     -> VK_PRIMITIVE_TOPOLOGY_POINT_LIST
    eEPrimitiveType_LineList = 2,       // D3DPT_LINELIST      -> VK_PRIMITIVE_TOPOLOGY_LINE_LIST
    eEPrimitiveType_LineStrip = 3,      // D3DPT_LINESTRIP     -> VK_PRIMITIVE_TOPOLOGY_LINE_STRIP
    eEPrimitiveType_TriangleList = 4,   // D3DPT_TRIANGLELIST  -> VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST
    eEPrimitiveType_TriangleStrip = 5,  // D3DPT_TRIANGLESTRIP -> VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP
    eEPrimitiveType_TriangleFan = 6,    // D3DPT_TRIANGLEFAN   -> VK_PRIMITIVE_TOPOLOGY_TRIANGLE_FAN
};

// Texture addressing = D3DTEXTUREADDRESS (SetTextureAdressing 0x744170 -> D3DSAMP_ADDRESSU/V/W unchanged).
enum eCGfxShared::eETexAddress : int {
    eETexAddress_Wrap = 1,        // D3DTADDRESS_WRAP       -> VK_SAMPLER_ADDRESS_MODE_REPEAT
    eETexAddress_Mirror = 2,      // D3DTADDRESS_MIRROR     -> VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT
    eETexAddress_Clamp = 3,       // D3DTADDRESS_CLAMP      -> VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE
    eETexAddress_Border = 4,      // D3DTADDRESS_BORDER     -> VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER (D3DSAMP_BORDERCOLOR)
    eETexAddress_MirrorOnce = 5,  // D3DTADDRESS_MIRRORONCE -> VK_SAMPLER_ADDRESS_MODE_MIRROR_CLAMP_TO_EDGE
};

// Texture filter = D3DTEXTUREFILTERTYPE. Used for D3DSAMP_MIN/MAG/MIPFILTER (SetTextureFiltering 0x744300) and as the
// StretchRect filter (CopyCurrentRTToImage 0x74d450 / CopyImageToCurrentRT 0x74d610).
enum eCGfxShared::eETexFilter : int {
    eETexFilter_None = 0,             // D3DTEXF_NONE (as MIPFILTER: no mipmapping, sample level 0 / max LOD 0)
    eETexFilter_Point = 1,            // D3DTEXF_POINT         -> VK_FILTER_NEAREST / VK_SAMPLER_MIPMAP_MODE_NEAREST
    eETexFilter_Linear = 2,           // D3DTEXF_LINEAR        -> VK_FILTER_LINEAR / VK_SAMPLER_MIPMAP_MODE_LINEAR
    eETexFilter_Anisotropic = 3,      // D3DTEXF_ANISOTROPIC   -> linear + anisotropy (D3DSAMP_MAXANISOTROPY)
    eETexFilter_PyramidalQuad = 6,    // D3DTEXF_PYRAMIDALQUAD -> treat as linear
    eETexFilter_GaussianQuad = 7,     // D3DTEXF_GAUSSIANQUAD  -> treat as linear
    eETexFilter_ConvolutionMono = 8,  // D3DTEXF_CONVOLUTIONMONO
};

// Sampler state = D3DSAMPLERSTATETYPE (SetSamplerState 0x744540 -> IDirect3DDevice9::SetSamplerState(stage, state, value)
// unchanged; the device caches 14 dwords per stage for stages 0..15, cache index = stage * 14 + state).
enum eCGfxShared::eESamplerState : int {
    eESamplerState_AddressU = 1,       // D3DSAMP_ADDRESSU (eETexAddress)
    eESamplerState_AddressV = 2,       // D3DSAMP_ADDRESSV
    eESamplerState_AddressW = 3,       // D3DSAMP_ADDRESSW
    eESamplerState_BorderColor = 4,    // D3DSAMP_BORDERCOLOR (D3DCOLOR ARGB)
    eESamplerState_MagFilter = 5,      // D3DSAMP_MAGFILTER (eETexFilter)
    eESamplerState_MinFilter = 6,      // D3DSAMP_MINFILTER
    eESamplerState_MipFilter = 7,      // D3DSAMP_MIPFILTER
    eESamplerState_MipMapLodBias = 8,  // D3DSAMP_MIPMAPLODBIAS (float bits)
    eESamplerState_MaxMipLevel = 9,    // D3DSAMP_MAXMIPLEVEL (most detailed level allowed = Vulkan minLod)
    eESamplerState_MaxAnisotropy = 10, // D3DSAMP_MAXANISOTROPY
    eESamplerState_SRGBTexture = 11,   // D3DSAMP_SRGBTEXTURE (sample through an sRGB view)
    eESamplerState_ElementIndex = 12,  // D3DSAMP_ELEMENTINDEX
    eESamplerState_DMapOffset = 13,    // D3DSAMP_DMAPOFFSET
};

// Cube faces = D3DCUBEMAP_FACES (eCDX9CubeMap::GetRenderTargetHandle 0x73c790 -> GetCubeMapSurface(face, 0)).
// Vulkan cube array layers use the same order (+X, -X, +Y, -Y, +Z, -Z).
enum eCGfxShared::eECubeMapFace : int {
    eECubeMapFace_PositiveX = 0,
    eECubeMapFace_NegativeX = 1,
    eECubeMapFace_PositiveY = 2,
    eECubeMapFace_NegativeY = 3,
    eECubeMapFace_PositiveZ = 4,
    eECubeMapFace_NegativeZ = 5,
};

// D3DX filter flags = D3DX_FILTER_* (GenerateTextureMipMaps 0x74cef0 -> D3DXFilterTexture, ConvertFrom 0x74cd70 ->
// D3DXLoadSurfaceFromSurface). A flag set: a filter (1..5) OR'ed with mirror/dither/sRGB bits.
enum eCGfxShared::eECopyFilter : int {
    eECopyFilter_None = 1,                   // D3DX_FILTER_NONE
    eECopyFilter_Point = 2,                  // D3DX_FILTER_POINT
    eECopyFilter_Linear = 3,                 // D3DX_FILTER_LINEAR
    eECopyFilter_Triangle = 4,               // D3DX_FILTER_TRIANGLE
    eECopyFilter_Box = 5,                    // D3DX_FILTER_BOX
    eECopyFilter_MirrorU = 1 << 16,          // D3DX_FILTER_MIRROR_U
    eECopyFilter_MirrorV = 2 << 16,          // D3DX_FILTER_MIRROR_V
    eECopyFilter_MirrorW = 4 << 16,          // D3DX_FILTER_MIRROR_W
    eECopyFilter_Mirror = 7 << 16,           // D3DX_FILTER_MIRROR
    eECopyFilter_Dither = 1 << 19,           // D3DX_FILTER_DITHER
    eECopyFilter_DitherDiffusion = 2 << 19,  // D3DX_FILTER_DITHER_DIFFUSION
    eECopyFilter_SRGBIn = 1 << 21,           // D3DX_FILTER_SRGB_IN
    eECopyFilter_SRGBOut = 2 << 21,          // D3DX_FILTER_SRGB_OUT
    eECopyFilter_SRGB = 3 << 21,             // D3DX_FILTER_SRGB
};

// Normal map generation flags = D3DX_NORMALMAP_* (ComputeNormalMap 0x74ccd0 -> D3DXComputeNormalMap) [I].
enum eCGfxShared::eENormalMapGen : int {
    eENormalMapGen_MirrorU = 0x10000,           // D3DX_NORMALMAP_MIRROR_U
    eENormalMapGen_MirrorV = 0x20000,           // D3DX_NORMALMAP_MIRROR_V
    eENormalMapGen_Mirror = 0x30000,            // D3DX_NORMALMAP_MIRROR
    eENormalMapGen_InvertSign = 0x80000,        // D3DX_NORMALMAP_INVERTSIGN
    eENormalMapGen_ComputeOcclusion = 0x100000, // D3DX_NORMALMAP_COMPUTE_OCCLUSION
};
// Channel flags = D3DX_CHANNEL_* (D3DXComputeNormalMap source channel) [I].
enum eCGfxShared::eEChannel : int {
    eEChannel_Red = 1,         // D3DX_CHANNEL_RED
    eEChannel_Blue = 2,        // D3DX_CHANNEL_BLUE
    eEChannel_Green = 4,       // D3DX_CHANNEL_GREEN
    eEChannel_Alpha = 8,       // D3DX_CHANNEL_ALPHA
    eEChannel_Luminance = 16,  // D3DX_CHANNEL_LUMINANCE
};

// Depth formats = D3DFORMAT (CreateDepthBuffer 0x744ad0 -> CreateDepthStencilSurface(Format = value); start/mode
// parameters -> D3DPRESENT_PARAMETERS::AutoDepthStencilFormat). The engine uses D24S8 (eCApplication::
// CreateGraphicsSubsystem 0x4c4330 passes 0x4b) and D24S8 with D24X8 fallback for shadow-map depth buffers.
enum eCGfxShared::eEDepthFormat : int {
    eEDepthFormat_D16Lockable = 70,   // D3DFMT_D16_LOCKABLE -> VK_FORMAT_D16_UNORM
    eEDepthFormat_D32 = 71,           // D3DFMT_D32          -> VK_FORMAT_D32_SFLOAT (no 32-bit unorm in Vulkan)
    eEDepthFormat_D15S1 = 73,         // D3DFMT_D15S1        -> VK_FORMAT_D16_UNORM_S8_UINT / D24S8
    eEDepthFormat_D24S8 = 75,         // D3DFMT_D24S8        -> VK_FORMAT_D24_UNORM_S8_UINT (fallback D32_SFLOAT_S8_UINT)
    eEDepthFormat_D24X8 = 77,         // D3DFMT_D24X8        -> VK_FORMAT_X8_D24_UNORM_PACK32 (fallback D32_SFLOAT)
    eEDepthFormat_D24X4S4 = 79,       // D3DFMT_D24X4S4      -> as D24S8
    eEDepthFormat_D16 = 80,           // D3DFMT_D16          -> VK_FORMAT_D16_UNORM
    eEDepthFormat_D32FLockable = 82,  // D3DFMT_D32F_LOCKABLE -> VK_FORMAT_D32_SFLOAT
    eEDepthFormat_D24FS8 = 83,        // D3DFMT_D24FS8       -> VK_FORMAT_D32_SFLOAT_S8_UINT
};

// HLSL shader type (eCDX9HLSLShader +0x28; Compile 0x749810 maps eSGfxHLSLShaderSrc::profile to it; stored in the shader
// pool record). Bit 0: 0 = vertex, 1 = pixel; bits 8..: version. GetShaderType 0x749ef0 returns only (type & 1), i.e.
// eEGfxHLSLShaderType_VS or eEGfxHLSLShaderType_PS.
enum eCGfxShared::eEGfxHLSLShaderType : int {
    eEGfxHLSLShaderType_VS = 0,
    eEGfxHLSLShaderType_PS = 1,
    eEGfxHLSLShaderType_PS_1_1 = 0x101,
    eEGfxHLSLShaderType_PS_1_2 = 0x201,
    eEGfxHLSLShaderType_PS_1_3 = 0x301,
    eEGfxHLSLShaderType_PS_1_4 = 0x401,
    eEGfxHLSLShaderType_PS_2_0 = 0x501,
    eEGfxHLSLShaderType_PS_2_a = 0x601,
    eEGfxHLSLShaderType_PS_2_b = 0x701,
    eEGfxHLSLShaderType_PS_3_0 = 0x801,
    eEGfxHLSLShaderType_VS_1_1 = 0x100,
    eEGfxHLSLShaderType_VS_2_0 = 0x200,
    eEGfxHLSLShaderType_VS_2_a = 0x300,
    eEGfxHLSLShaderType_VS_3_0 = 0x400,
};

// Memory type of the profiling callbacks (SetMemoryAllocCallback / SetMemoryFreeCallback). The DX9 layer only stores the
// callbacks (ms_pRegDxMemoryAlloc/Free are never called), so a replacement may ignore them. Values from
// eCGfxGaugeAdmin::GetNameFromFormat 0x508670 / GetTotalSizeWithMipmaps 0x5082e0; names [I].
enum eCGfxShared::eEGraphicMemoryType : int {
    eEGraphicMemoryType_Texture = 0,       // [I] (format = D3DFORMAT, size with mipmaps)
    eEGraphicMemoryType_CubeMap = 1,       // [I]
    eEGraphicMemoryType_VertexBuffer = 2,  // [I] (2..5: no mip-size computation)
    eEGraphicMemoryType_IndexBuffer = 3,   // [I]
    eEGraphicMemoryType_VertexShader = 4,  // format = eEGfxHLSLShaderType
    eEGraphicMemoryType_PixelShader = 5,   // format = eEGfxHLSLShaderType
};

// Material color source = D3DMATERIALCOLORSOURCE (Set*MaterialSource 0x742ba0.. -> D3DRS_AMBIENT/DIFFUSE/SPECULAR/
// EMISSIVEMATERIALSOURCE). Fixed function only.
enum eCGfxShared::eEMaterialColorSrc : int {
    eEMaterialColorSrc_Material = 0,  // D3DMCS_MATERIAL
    eEMaterialColorSrc_Color1 = 1,    // D3DMCS_COLOR1 (vertex diffuse)
    eEMaterialColorSrc_Color2 = 2,    // D3DMCS_COLOR2 (vertex specular)
};

// Sprite list options = D3DXSPRITE_* flags (BeginSpriteList 0x746bd0 -> ID3DXSprite::Begin(flags)) [I].
enum eCGfxShared::eESpriteRenderOpt : int {
    eESpriteRenderOpt_DoNotSaveState = 0x1,          // D3DXSPRITE_DONOTSAVESTATE
    eESpriteRenderOpt_DoNotModifyRenderState = 0x2,  // D3DXSPRITE_DONOTMODIFY_RENDERSTATE
    eESpriteRenderOpt_ObjectSpace = 0x4,             // D3DXSPRITE_OBJECTSPACE
    eESpriteRenderOpt_Billboard = 0x8,               // D3DXSPRITE_BILLBOARD
    eESpriteRenderOpt_AlphaBlend = 0x10,             // D3DXSPRITE_ALPHABLEND
    eESpriteRenderOpt_SortTexture = 0x20,            // D3DXSPRITE_SORT_TEXTURE
    eESpriteRenderOpt_SortDepthFrontToBack = 0x40,   // D3DXSPRITE_SORT_DEPTH_FRONTTOBACK
    eESpriteRenderOpt_SortDepthBackToFront = 0x80,   // D3DXSPRITE_SORT_DEPTH_BACKTOFRONT
    eESpriteRenderOpt_DoNotAddRefTexture = 0x100,    // D3DXSPRITE_DO_NOT_ADDREF_TEXTURE
};

// Pool = D3DPOOL (CreateImageFromMemory 0x73f030 stores it into eSGfxLayersDesc::pool, which goes to D3D/D3DX unchanged).
enum eCGfxShared::eEPoolType : int {
    eEPoolType_Default = 0,    // D3DPOOL_DEFAULT: GPU memory, lost on reset (render targets, dynamic buffers)
    eEPoolType_Managed = 1,    // D3DPOOL_MANAGED: GPU copy + CPU backing, survives reset
    eEPoolType_SystemMem = 2,  // D3DPOOL_SYSTEMMEM: CPU memory (GetRenderTargetData destinations)
    eEPoolType_Scratch = 3,    // D3DPOOL_SCRATCH: CPU only, not bindable
};

// Fixed-function texture stage enums (lower priority: the shader paths never use them).
// Texture coordinate generation = D3DTSS_TCI_* (SetTextureCoordinateIndexAndCalc 0x744490: TEXCOORDINDEX = index | calc).
enum eCGfxShared::eETexCoordCalc : int {
    eETexCoordCalc_PassThru = 0x00000,                     // D3DTSS_TCI_PASSTHRU
    eETexCoordCalc_CameraSpaceNormal = 0x10000,            // D3DTSS_TCI_CAMERASPACENORMAL
    eETexCoordCalc_CameraSpacePosition = 0x20000,          // D3DTSS_TCI_CAMERASPACEPOSITION
    eETexCoordCalc_CameraSpaceReflectionVector = 0x30000,  // D3DTSS_TCI_CAMERASPACEREFLECTIONVECTOR
    eETexCoordCalc_SphereMap = 0x40000,                    // D3DTSS_TCI_SPHEREMAP
};
// Texture argument = D3DTA_* (SetColorCalc/SetAlphaCalc 0x744870/0x7449a0 -> COLORARG1/2, ALPHAARG1/2; SetResultArgument
// 0x7447c0 -> D3DTSS_RESULTARG).
enum eCGfxShared::eETexArgument : int {
    eETexArgument_Diffuse = 0x0,          // D3DTA_DIFFUSE
    eETexArgument_Current = 0x1,          // D3DTA_CURRENT
    eETexArgument_Texture = 0x2,          // D3DTA_TEXTURE
    eETexArgument_TFactor = 0x3,          // D3DTA_TFACTOR (D3DRS_TEXTUREFACTOR = SetColorFactor 0x7428c0)
    eETexArgument_Specular = 0x4,         // D3DTA_SPECULAR
    eETexArgument_Temp = 0x5,             // D3DTA_TEMP
    eETexArgument_Constant = 0x6,         // D3DTA_CONSTANT
    eETexArgument_SelectMask = 0xf,       // D3DTA_SELECTMASK
    eETexArgument_Complement = 0x10,      // D3DTA_COMPLEMENT (modifier bit)
    eETexArgument_AlphaReplicate = 0x20,  // D3DTA_ALPHAREPLICATE (modifier bit)
};
// Texture operation = D3DTEXTUREOP (SetColorCalc -> D3DTSS_COLOROP, SetAlphaCalc -> D3DTSS_ALPHAOP).
enum eCGfxShared::eETexOperation : int {
    eETexOperation_Disable = 1,                    // D3DTOP_DISABLE
    eETexOperation_SelectArg1 = 2,                 // D3DTOP_SELECTARG1
    eETexOperation_SelectArg2 = 3,                 // D3DTOP_SELECTARG2
    eETexOperation_Modulate = 4,                   // D3DTOP_MODULATE
    eETexOperation_Modulate2x = 5,                 // D3DTOP_MODULATE2X
    eETexOperation_Modulate4x = 6,                 // D3DTOP_MODULATE4X
    eETexOperation_Add = 7,                        // D3DTOP_ADD
    eETexOperation_AddSigned = 8,                  // D3DTOP_ADDSIGNED
    eETexOperation_AddSigned2x = 9,                // D3DTOP_ADDSIGNED2X
    eETexOperation_Subtract = 10,                  // D3DTOP_SUBTRACT
    eETexOperation_AddSmooth = 11,                 // D3DTOP_ADDSMOOTH
    eETexOperation_BlendDiffuseAlpha = 12,         // D3DTOP_BLENDDIFFUSEALPHA
    eETexOperation_BlendTextureAlpha = 13,         // D3DTOP_BLENDTEXTUREALPHA
    eETexOperation_BlendFactorAlpha = 14,          // D3DTOP_BLENDFACTORALPHA
    eETexOperation_BlendTextureAlphaPM = 15,       // D3DTOP_BLENDTEXTUREALPHAPM
    eETexOperation_BlendCurrentAlpha = 16,         // D3DTOP_BLENDCURRENTALPHA
    eETexOperation_PreModulate = 17,               // D3DTOP_PREMODULATE
    eETexOperation_ModulateAlphaAddColor = 18,     // D3DTOP_MODULATEALPHA_ADDCOLOR
    eETexOperation_ModulateColorAddAlpha = 19,     // D3DTOP_MODULATECOLOR_ADDALPHA
    eETexOperation_ModulateInvAlphaAddColor = 20,  // D3DTOP_MODULATEINVALPHA_ADDCOLOR
    eETexOperation_ModulateInvColorAddAlpha = 21,  // D3DTOP_MODULATEINVCOLOR_ADDALPHA
    eETexOperation_BumpEnvMap = 22,                // D3DTOP_BUMPENVMAP
    eETexOperation_BumpEnvMapLuminance = 23,       // D3DTOP_BUMPENVMAPLUMINANCE
    eETexOperation_DotProduct3 = 24,               // D3DTOP_DOTPRODUCT3
    eETexOperation_MultiplyAdd = 25,               // D3DTOP_MULTIPLYADD
    eETexOperation_Lerp = 26,                      // D3DTOP_LERP
};

// ---------------------------------------------------------------------------------------------------------------------
// Engine enums that do not appear in any HAL signature (so api_generated.h does not declare them in eCGfxShared); they
// appear inside the structs below. Declared at namespace scope with the engine names.

// eCGfxShared::eEColorFormat = D3DFORMAT. Names and values from the reflection registration
// (eCImageFilterRTBase::StaticConstructor 0x666d00, eCImageResource2::StaticConstructor 0x68ca40). The layers desc
// carries the raw D3DFORMAT (any D3DFMT_*, also FOURCC NULL/DF16/DF24), not only these values.
enum eEColorFormat : int {
    eEColorFormat_Unknown = 0,                // D3DFMT_UNKNOWN
    eEColorFormat_A8R8G8B8 = 21,              // D3DFMT_A8R8G8B8      -> VK_FORMAT_B8G8R8A8_UNORM
    eEColorFormat_X8R8G8B8 = 22,              // D3DFMT_X8R8G8B8      -> VK_FORMAT_B8G8R8A8_UNORM (alpha forced 1)
    eEColorFormat_X1R5G5B5 = 24,              // D3DFMT_X1R5G5B5      -> VK_FORMAT_A1R5G5B5_UNORM_PACK16 (alpha 1)
    eEColorFormat_A1R5G5B5 = 25,              // D3DFMT_A1R5G5B5      -> VK_FORMAT_A1R5G5B5_UNORM_PACK16
    eEColorFormat_A4R4G4B4 = 26,              // D3DFMT_A4R4G4B4      -> VK_FORMAT_B4G4R4A4_UNORM_PACK16 + swizzle
    eEColorFormat_R16F = 111,                 // D3DFMT_R16F          -> VK_FORMAT_R16_SFLOAT (missing G/B read 1, A 1)
    eEColorFormat_G16R16F = 112,              // D3DFMT_G16R16F       -> VK_FORMAT_R16G16_SFLOAT
    eEColorFormat_A16B16G16R16F = 113,        // D3DFMT_A16B16G16R16F -> VK_FORMAT_R16G16B16A16_SFLOAT
    eEColorFormat_R32F = 114,                 // D3DFMT_R32F          -> VK_FORMAT_R32_SFLOAT
    eEColorFormat_G32R32F = 115,              // D3DFMT_G32R32F       -> VK_FORMAT_R32G32_SFLOAT
    eEColorFormat_A32B32G32R32F = 116,        // D3DFMT_A32B32G32R32F -> VK_FORMAT_R32G32B32A32_SFLOAT
    eEColorFormat_DXT1 = 0x31545844,          // D3DFMT_DXT1 ('DXT1') -> VK_FORMAT_BC1_RGBA_UNORM_BLOCK
    eEColorFormat_DXT2 = 0x32545844,          // D3DFMT_DXT2          -> VK_FORMAT_BC2_UNORM_BLOCK (premultiplied)
    eEColorFormat_DXT3 = 0x33545844,          // D3DFMT_DXT3          -> VK_FORMAT_BC2_UNORM_BLOCK
    eEColorFormat_DXT4 = 0x34545844,          // D3DFMT_DXT4          -> VK_FORMAT_BC3_UNORM_BLOCK (premultiplied)
    eEColorFormat_DXT5 = 0x35545844,          // D3DFMT_DXT5          -> VK_FORMAT_BC3_UNORM_BLOCK
};

// eCGfxShared::eEMultiSample = D3DMULTISAMPLE_TYPE. sCreateDevice 0x73d420 maps 2..16 to D3DMULTISAMPLE_2..16_SAMPLES
// (fullscreen only; windowed forces none). The engine passes 1 << config (eCApplication::CreateGraphicsSubsystem 0x4c4330).
enum eEMultiSample : int {
    eEMultiSample_None = 0,  // D3DMULTISAMPLE_NONE
    eEMultiSample_2 = 2,     // D3DMULTISAMPLE_2_SAMPLES
    eEMultiSample_4 = 4,     // D3DMULTISAMPLE_4_SAMPLES
    eEMultiSample_8 = 8,     // D3DMULTISAMPLE_8_SAMPLES
    eEMultiSample_16 = 16,   // D3DMULTISAMPLE_16_SAMPLES
};

// eCGfxShared::eEContextCreate = D3DCREATE_* behavior flags for IDirect3D9::CreateDevice. The engine passes
// 0x40 (+2 when the bool argument of CreateGraphicsSubsystem is set) (0x4c4330).
enum eEContextCreate : int {
    eEContextCreate_FPUPreserve = 0x2,                 // D3DCREATE_FPU_PRESERVE (keep the x87/SSE control word)
    eEContextCreate_MultiThreaded = 0x4,               // D3DCREATE_MULTITHREADED [I] (not seen)
    eEContextCreate_HardwareVertexProcessing = 0x40,   // D3DCREATE_HARDWARE_VERTEXPROCESSING
};

// =====================================================================================================================
// Structs
// =====================================================================================================================

// D3DCAPS9 without its first two dwords (DeviceType, AdapterOrdinal): what sGetAdapterDesc 0x73d7e0 copies (0x128 bytes
// from D3DCAPS9 + 8) into eSGfxAdapterDesc + 0x418. The engine reads caps from there, e.g.
// eCShaderMaterialAdmin::PostInitialize 0x6ba9d0: pixelShaderVersion (max material shader model) and ps20Caps.caps & 4
// (D3DPS20CAPS_PREDICATION). A replacement must fill this like a real SM3 D3D9 driver.
struct D3DCaps9Tail {
    std::uint32_t caps, caps2, caps3, presentationIntervals, cursorCaps, devCaps, primitiveMiscCaps, rasterCaps, zCmpCaps,
        srcBlendCaps, destBlendCaps, alphaCmpCaps, shadeCaps, textureCaps, textureFilterCaps, cubeTextureFilterCaps,
        volumeTextureFilterCaps, textureAddressCaps, volumeTextureAddressCaps, lineCaps, maxTextureWidth, maxTextureHeight,
        maxVolumeExtent, maxTextureRepeat, maxTextureAspectRatio, maxAnisotropy;
    float maxVertexW, guardBandLeft, guardBandTop, guardBandRight, guardBandBottom, extentsAdjust;
    std::uint32_t stencilCaps, fvfCaps, textureOpCaps, maxTextureBlendStages, maxSimultaneousTextures, vertexProcessingCaps,
        maxActiveLights, maxUserClipPlanes, maxVertexBlendMatrices, maxVertexBlendMatrixIndex;
    float maxPointSize;
    std::uint32_t maxPrimitiveCount, maxVertexIndex, maxStreams, maxStreamStride, vertexShaderVersion, maxVertexShaderConst,
        pixelShaderVersion;
    float pixelShader1xMaxValue;
    std::uint32_t devCaps2;
    float maxNpatchTessellationLevel;
    std::uint32_t reserved5, masterAdapterOrdinal, adapterOrdinalInGroup, numberOfAdaptersInGroup, declTypes,
        numSimultaneousRTs, stretchRectFilterCaps;
    struct {
        std::uint32_t caps;
        std::int32_t dynamicFlowControlDepth, numTemps, staticFlowControlDepth;
    } vs20Caps;
    struct {
        std::uint32_t caps;
        std::int32_t dynamicFlowControlDepth, numTemps, staticFlowControlDepth, numInstructionSlots;
    } ps20Caps;
    std::uint32_t vertexTextureFilterCaps, maxVShaderInstructionsExecuted, maxPShaderInstructionsExecuted,
        maxVertexShader30InstructionSlots, maxPixelShader30InstructionSlots;
};
static_assert(sizeof(D3DCaps9Tail) == 0x128);
// offsets inside D3DCAPS9 = offsetof(D3DCaps9Tail, x) + 8
static_assert(offsetof(D3DCaps9Tail, pixelShaderVersion) + 8 == 0xcc);
static_assert(offsetof(D3DCaps9Tail, ps20Caps) + 8 == 0x108);

// eSGfxAdapterDesc (0x548; eCGfxAdmin::InitializeAdmin 0x74e510 clears 0x548 bytes; filled by sGetAdapterDesc 0x73d7e0
// from IDirect3D9::GetAdapterIdentifier + GetDeviceCaps(HAL) + DxDiag).
struct eCGfxShared::eSGfxAdapterDesc {
    char driver[512];            // D3DADAPTER_IDENTIFIER9::Driver
    char description[512];       // D3DADAPTER_IDENTIFIER9::Description (DeviceName is not copied)
    std::uint64_t driverVersion; // D3DADAPTER_IDENTIFIER9::DriverVersion (LARGE_INTEGER)
    std::uint32_t vendorId;
    std::uint32_t deviceId;
    std::uint32_t subSysId;
    std::uint32_t revision;
    D3DCaps9Tail caps;              // +0x418: D3DCAPS9 from offset 8 (see above)
    std::int32_t dedicatedVideoMemoryMB;  // +0x540: DxDiag "szDisplayMemoryLocalized" of this adapter parsed as int (MB), 0
                                          // if DxDiag fails
};
static_assert(sizeof(eCGfxShared::eSGfxAdapterDesc) == 0x548);
static_assert(offsetof(eCGfxShared::eSGfxAdapterDesc, description) == 0x200);
static_assert(offsetof(eCGfxShared::eSGfxAdapterDesc, driverVersion) == 0x400);
static_assert(offsetof(eCGfxShared::eSGfxAdapterDesc, vendorId) == 0x408);
static_assert(offsetof(eCGfxShared::eSGfxAdapterDesc, revision) == 0x414);
static_assert(offsetof(eCGfxShared::eSGfxAdapterDesc, caps) == 0x418);
static_assert(offsetof(eCGfxShared::eSGfxAdapterDesc, dedicatedVideoMemoryMB) == 0x540);

// eSGfxModeDesc (0x18; InitializeAdmin copies 0x18 bytes into eSGfxContextDesc + 0x554). sGetModeDesc 0x73d6f0 fills it
// from IDirect3D9::EnumAdapterModes(adapter, D3DFMT_X8R8G8B8, mode) and writes only width/height/refresh/format; the
// engine later fills +0xc/+0x10 from its setup (eCApplication::CreateGraphicsSubsystem 0x4c4330) and forwards them to the
// start parameters (eCGfxContextAdmin::Create 0x507db0).
struct eCGfxShared::eSGfxModeDesc {
    std::uint32_t width;
    std::uint32_t height;
    std::uint32_t refreshRate;   // Hz
    bool vsync;                  // [I] name: becomes eSAPIStartParameters::vsync (PresentationInterval ONE)
    std::uint32_t unknown10;     // [I] becomes eSAPIStartParameters::unknown2C (DX9 ignores it)
    std::uint32_t format;        // D3DFORMAT, always D3DFMT_X8R8G8B8 (22); CreateGraphicsSubsystem only accepts 22
};
static_assert(sizeof(eCGfxShared::eSGfxModeDesc) == 0x18);
static_assert(offsetof(eCGfxShared::eSGfxModeDesc, vsync) == 0xc);
static_assert(offsetof(eCGfxShared::eSGfxModeDesc, format) == 0x14);

// eSAPIModeParameters (0x24; built on the stack by eCGfxContextAdmin::ToggleNewMode 0x507f30, read by
// eCDX9Device::ToggleNewMode 0x73f690 into D3DPRESENT_PARAMETERS before IDirect3DDevice9::Reset).
struct eCAPIDevice::eSAPIModeParameters {
    bool windowed;                     // true: Windowed, SwapEffect COPY, refresh 0, no MSAA
    std::int32_t multiSample;          // eEMultiSample (fullscreen only)
    std::uint32_t backBufferWidth;
    std::uint32_t backBufferHeight;
    std::uint32_t refreshRate;         // fullscreen only
    std::uint32_t backBufferFormat;    // D3DFORMAT (X8R8G8B8)
    eCGfxShared::eEDepthFormat depthFormat;  // AutoDepthStencilFormat (EnableAutoDepthStencil is always TRUE)
    bool vsync;                        // PresentationInterval = ONE if true, else IMMEDIATE
    std::uint32_t unknown20;           // [I] copied from eSGfxModeDesc::unknown10, unused by DX9
};
static_assert(sizeof(eCAPIDevice::eSAPIModeParameters) == 0x24);
static_assert(offsetof(eCAPIDevice::eSAPIModeParameters, multiSample) == 0x4);
static_assert(offsetof(eCAPIDevice::eSAPIModeParameters, backBufferWidth) == 0x8);
static_assert(offsetof(eCAPIDevice::eSAPIModeParameters, refreshRate) == 0x10);
static_assert(offsetof(eCAPIDevice::eSAPIModeParameters, backBufferFormat) == 0x14);
static_assert(offsetof(eCAPIDevice::eSAPIModeParameters, depthFormat) == 0x18);
static_assert(offsetof(eCAPIDevice::eSAPIModeParameters, vsync) == 0x1c);

// eSAPIStartParameters (0x30; built on the stack by eCGfxContextAdmin::Create 0x507db0, read by sCreateDevice 0x73d420):
// the mode parameters prefixed with the window and the behavior flags.
struct eCAPIDevice::eSAPIStartParameters {
    HWND__* window;                    // hFocusWindow and D3DPRESENT_PARAMETERS::hDeviceWindow (the game window)
    std::uint32_t behaviorFlags;       // eEContextCreate = D3DCREATE_* (0x40 / 0x42)
    bool windowed;                     // +0xc: as eSAPIModeParameters from here on (+0xc)
    std::int32_t multiSample;          // eEMultiSample
    std::uint32_t backBufferWidth;
    std::uint32_t backBufferHeight;
    std::uint32_t refreshRate;
    std::uint32_t backBufferFormat;    // D3DFORMAT
    eCGfxShared::eEDepthFormat depthFormat;
    bool vsync;
    std::uint32_t unknown2C;           // [I] eSGfxModeDesc::unknown10, unused by DX9
};
static_assert(sizeof(eCAPIDevice::eSAPIStartParameters) == 0x30);
static_assert(offsetof(eCAPIDevice::eSAPIStartParameters, behaviorFlags) == 0x8);
static_assert(offsetof(eCAPIDevice::eSAPIStartParameters, windowed) == 0xc);
static_assert(offsetof(eCAPIDevice::eSAPIStartParameters, multiSample) == 0x10);
static_assert(offsetof(eCAPIDevice::eSAPIStartParameters, backBufferWidth) == 0x14);
static_assert(offsetof(eCAPIDevice::eSAPIStartParameters, backBufferHeight) == 0x18);
static_assert(offsetof(eCAPIDevice::eSAPIStartParameters, refreshRate) == 0x1c);
static_assert(offsetof(eCAPIDevice::eSAPIStartParameters, backBufferFormat) == 0x20);
static_assert(offsetof(eCAPIDevice::eSAPIStartParameters, depthFormat) == 0x24);
static_assert(offsetof(eCAPIDevice::eSAPIStartParameters, vsync) == 0x28);

// eSGfxLayersDesc (0x30): description of a texture / cube map / volume. eCGfxImageData::CreateLayers 0x751fc0 copies
// 0x30 bytes and dispatches: cubeMap ? CreateCubeMap : depth == 0 ? CreateTexture : CreateVolume. The DX9 layer reads
// +0x00..+0x1c only.
struct eCGfxShared::eSGfxLayersDesc {
    std::uint32_t width;     // CreateTexture(Width); cube: edge length. 0 with *FromMemory/*FromFile = size of the file
    std::uint32_t height;
    std::uint32_t depth;     // volume depth (CreateVolumeTexture); 0 for 2D/cube. CheckDeviceFormat: > 1 = volume
    bool cubeMap;            // +0xc: cube map (CreateImageFromMemory 0x73f030 sets it for cube DDS files; 3 bytes padding
                             // after it are uninitialised, hence the "garbage" seen in traces)
    std::uint32_t levels;    // mip levels (0 = full chain). Written back by the create functions (GetLevelCount)
    std::uint32_t usage;     // D3DUSAGE_*: 1 RENDERTARGET, 2 DEPTHSTENCIL, 0x200 DYNAMIC (0x400 AUTOGENMIPMAP [I])
    std::uint32_t format;    // D3DFORMAT (incl. FOURCC NULL / DF16 / DF24 / DXTn); written back by the from-file paths
    std::uint32_t pool;      // D3DPOOL (eEPoolType)
    std::uint32_t byteSize;  // +0x20: total size incl. mips (x6 for cubes), computed by the engine
                             // (eCGfxImageData::FillResourceSizeAndLevelCount 0x752140); never read by the HAL
    std::uint32_t unknown24; // [I] padding / unused
    const char* debugName;   // +0x28: creator tag set by the engine (e.g. "eCGfxImageData::CreateImageFromMemoryExt",
                             // 0x7527f0); never read by the HAL
};
static_assert(sizeof(eCGfxShared::eSGfxLayersDesc) == 0x30);
static_assert(offsetof(eCGfxShared::eSGfxLayersDesc, depth) == 0x8);
static_assert(offsetof(eCGfxShared::eSGfxLayersDesc, cubeMap) == 0xc);
static_assert(offsetof(eCGfxShared::eSGfxLayersDesc, levels) == 0x10);
static_assert(offsetof(eCGfxShared::eSGfxLayersDesc, usage) == 0x14);
static_assert(offsetof(eCGfxShared::eSGfxLayersDesc, format) == 0x18);
static_assert(offsetof(eCGfxShared::eSGfxLayersDesc, pool) == 0x1c);
static_assert(offsetof(eCGfxShared::eSGfxLayersDesc, byteSize) == 0x20);
static_assert(offsetof(eCGfxShared::eSGfxLayersDesc, debugName) == 0x28);

// eSGfxImageInfo = D3DXIMAGE_INFO (GetImageInfoFromMemory 0x73f530 copies the 7 dwords of D3DXGetImageInfoFromFileInMemory).
struct eCGfxShared::eSGfxImageInfo {
    std::uint32_t width;
    std::uint32_t height;
    std::uint32_t depth;
    std::uint32_t mipLevels;
    std::uint32_t format;           // D3DFORMAT
    std::uint32_t resourceType;     // D3DRESOURCETYPE: 3 texture, 4 volume texture, 5 cube texture
    std::uint32_t imageFileFormat;  // D3DXIMAGE_FILEFORMAT: 0 BMP, 1 JPG, 2 TGA, 3 PNG, 4 DDS, ...
};
static_assert(sizeof(eCGfxShared::eSGfxImageInfo) == 0x1c);

// eCAPIDevice::eSImageInfo (GetImageInfoFromFile 0x73f470): D3DXIMAGE_INFO condensed.
struct eCAPIDevice::eSImageInfo {
    std::uint32_t width;
    std::uint32_t height;
    std::uint32_t depth;
    std::uint32_t mipLevels;
    std::uint32_t format;  // D3DFORMAT
    bool cubeMap;          // +0x14: resourceType == D3DRTYPE_CUBETEXTURE
    bool volume;           // +0x15: resourceType == D3DRTYPE_VOLUMETEXTURE
};
static_assert(sizeof(eCAPIDevice::eSImageInfo) == 0x18);  // [I] size (fields up to +0x15 confirmed)
static_assert(offsetof(eCAPIDevice::eSImageInfo, cubeMap) == 0x14);

// eSGfxLockedRect = D3DLOCKED_RECT (eCDX9Texture::LockRect 0x74c910 passes the pointer to IDirect3DTexture9::LockRect).
struct eCGfxShared::eSGfxLockedRect {
    std::int32_t pitch;  // bytes per row (per block row for DXTn)
    void* bits;
};
static_assert(sizeof(eCGfxShared::eSGfxLockedRect) == 0x10);
static_assert(offsetof(eCGfxShared::eSGfxLockedRect, bits) == 0x8);

// eSGfxVertexBufDesc (CreateVertexData -> eCDX9VertexData ctor 0x74dcf0 -> CreateVertexBuffer(length, usage, fvf, pool)).
struct eCGfxShared::eSGfxVertexBufDesc {
    std::uint32_t length;  // bytes
    std::uint32_t usage;   // D3DUSAGE_* (0x8 WRITEONLY, 0x200 DYNAMIC)
    std::uint32_t fvf;     // D3DFVF_* or 0; stride = D3DXGetFVFVertexSize(fvf) unless fvf == 0
    std::uint32_t pool;    // D3DPOOL
    std::uint32_t stride;  // vertex size used when fvf == 0 (not written by GetDesc)
};
static_assert(sizeof(eCGfxShared::eSGfxVertexBufDesc) == 0x14);

// eSGfxIndexBufDesc (CreateIndexData -> eCDX9IndexData ctor 0x74ad80 -> CreateIndexBuffer(length, usage, format, pool)).
struct eCGfxShared::eSGfxIndexBufDesc {
    std::uint32_t length;  // bytes
    std::uint32_t usage;   // D3DUSAGE_*
    std::uint32_t format;  // D3DFMT_INDEX16 (101) or D3DFMT_INDEX32 (102)
    std::uint32_t pool;    // D3DPOOL
};
static_assert(sizeof(eCGfxShared::eSGfxIndexBufDesc) == 0x10);

// eSGfxDrawCallDesc (0x20; eCDX9Primitive::SetDrawCallDescription 0x74b530 copies 4 qwords; eCDX9Device::Draw 0x741f40 and
// eCDX9Primitive::Execute 0x74b5b0 issue the draw).
struct eCGfxShared::eSGfxDrawCallDesc {
    bool indexed;                     // true: DrawIndexedPrimitive, false: DrawPrimitive
    eCGfxShared::eEPrimitiveType type;
    std::uint32_t primitiveCount;
    std::uint32_t startVertex;        // DrawPrimitive only (vkCmdDraw firstVertex)
    std::uint32_t minVertexIndex;     // DrawIndexedPrimitive MinIndex (a hint; Vulkan ignores it)
    std::uint32_t numVertices;        // DrawIndexedPrimitive NumVertices (a hint)
    std::uint32_t startIndex;         // DrawIndexedPrimitive StartIndex (vkCmdDrawIndexed firstIndex)
    std::int32_t baseVertexIndex;     // DrawIndexedPrimitive BaseVertexIndex (vkCmdDrawIndexed vertexOffset)
};
static_assert(sizeof(eCGfxShared::eSGfxDrawCallDesc) == 0x20);
static_assert(offsetof(eCGfxShared::eSGfxDrawCallDesc, type) == 0x4);
static_assert(offsetof(eCGfxShared::eSGfxDrawCallDesc, primitiveCount) == 0x8);
static_assert(offsetof(eCGfxShared::eSGfxDrawCallDesc, startVertex) == 0xc);
static_assert(offsetof(eCGfxShared::eSGfxDrawCallDesc, minVertexIndex) == 0x10);
static_assert(offsetof(eCGfxShared::eSGfxDrawCallDesc, numVertices) == 0x14);
static_assert(offsetof(eCGfxShared::eSGfxDrawCallDesc, startIndex) == 0x18);
static_assert(offsetof(eCGfxShared::eSGfxDrawCallDesc, baseVertexIndex) == 0x1c);

// eSGfxDclElement = D3DVERTEXELEMENT9 (CreateVertexDeclaration 0x742e20 passes the array to
// IDirect3DDevice9::CreateVertexDeclaration; terminated by D3DDECL_END = {0xFF, 0, D3DDECLTYPE_UNUSED (17), 0, 0, 0}).
struct eCGfxShared::eSGfxDclElement {
    std::uint16_t stream;      // 0xFF = end marker
    std::uint16_t offset;
    std::uint8_t type;         // D3DDECLTYPE
    std::uint8_t method;       // D3DDECLMETHOD (always DEFAULT)
    std::uint8_t usage;        // D3DDECLUSAGE
    std::uint8_t usageIndex;
};
static_assert(sizeof(eCGfxShared::eSGfxDclElement) == 8);

// eSGfxViewport (SetViewport 0x740530 converts to D3DVIEWPORT9: X = left, Y = top, Width = right - left, Height =
// bottom - top, a zero width/height becomes 32; GetViewport 0x740630 converts back).
struct eCGfxShared::eSGfxViewport {
    std::int32_t left;
    std::int32_t top;
    std::int32_t right;   // exclusive
    std::int32_t bottom;  // exclusive
    float minZ;
    float maxZ;
};
static_assert(sizeof(eCGfxShared::eSGfxViewport) == 0x18);

// eSGfxGammaRamp = D3DGAMMARAMP (SetGammaRamp 0x741550 passes it to IDirect3DDevice9::SetGammaRamp).
struct eCGfxShared::eSGfxGammaRamp {
    std::uint16_t red[256];
    std::uint16_t green[256];
    std::uint16_t blue[256];
};
static_assert(sizeof(eCGfxShared::eSGfxGammaRamp) == 0x600);

// eSGfxStencilData (SetStencilTesting 0x7419f0 / ActivateStencilTesting 0x2860e0 write the 7 dwords to
// D3DRS_STENCILFAIL..D3DRS_STENCILWRITEMASK in order).
struct eCGfxShared::eSGfxStencilData {
    std::uint32_t failOp;       // D3DRS_STENCILFAIL  (D3DSTENCILOP: 1 KEEP, 2 ZERO, 3 REPLACE, 4 INCRSAT, 5 DECRSAT,
                                //                     6 INVERT, 7 INCR, 8 DECR)
    std::uint32_t depthFailOp;  // D3DRS_STENCILZFAIL
    std::uint32_t passOp;       // D3DRS_STENCILPASS
    eCGfxShared::eECmpFunc func;  // D3DRS_STENCILFUNC
    std::uint32_t ref;          // D3DRS_STENCILREF
    std::uint32_t mask;         // D3DRS_STENCILMASK (compare mask)
    std::uint32_t writeMask;    // D3DRS_STENCILWRITEMASK
};
static_assert(sizeof(eCGfxShared::eSGfxStencilData) == 0x1c);

// eSGfxAlphaTestData (SetAlphaTesting 0x742a50: D3DRS_ALPHAREF, D3DRS_ALPHAFUNC). Vulkan: emulate in the pixel shader
// (discard if !(alpha*255 func ref), ref is 0..255).
struct eCGfxShared::eSGfxAlphaTestData {
    std::uint32_t ref;
    eCGfxShared::eECmpFunc func;
};
static_assert(sizeof(eCGfxShared::eSGfxAlphaTestData) == 8);

// eSGfxFogData (SetFog 0x7424e0). Fixed-function fog; D3D9 applies it to vs/ps <= 2.x only (SM3 shaders do their own fog).
struct eCGfxShared::eSGfxFogData {
    bool enable;               // D3DRS_FOGENABLE (nothing else is written when false)
    bool rangeFog;             // +1: D3DRS_RANGEFOGENABLE
    std::uint32_t color;       // +4: D3DRS_FOGCOLOR (D3DCOLOR ARGB)
    std::int32_t vertexFog;    // +8: 1 = vertex fog (D3DRS_FOGTABLEMODE = NONE, D3DRS_FOGVERTEXMODE = mode),
                               //     else table/pixel fog (FOGVERTEXMODE = NONE, FOGTABLEMODE = mode)
    std::uint32_t mode;        // +0xc: D3DFOGMODE (0 NONE, 1 EXP, 2 EXP2, 3 LINEAR)
    float start;               // +0x10: D3DRS_FOGSTART (LINEAR only)
    float end;                 // +0x14: D3DRS_FOGEND (LINEAR only)
    float density;             // +0x18: D3DRS_FOGDENSITY (EXP/EXP2 only)
};
static_assert(sizeof(eCGfxShared::eSGfxFogData) == 0x1c);
static_assert(offsetof(eCGfxShared::eSGfxFogData, color) == 0x4);
static_assert(offsetof(eCGfxShared::eSGfxFogData, density) == 0x18);

// eSGfxPlaneData: float4 plane (a, b, c, d) passed to IDirect3DDevice9::SetClipPlane (SetClipPlaneData 0x745030).
// With shaders bound, D3D9 clip planes are in clip space.
struct eCGfxShared::eSGfxPlaneData {
    float a, b, c, d;  // [I] (only the pointer is passed)
};
static_assert(sizeof(eCGfxShared::eSGfxPlaneData) == 0x10);

// eSGfxMacro = D3DXMACRO (eSGfxHLSLShaderSrc / effect sources; array terminated by {nullptr, nullptr}).
struct eCGfxShared::eSGfxMacro {
    const char* name;
    const char* definition;
};
static_assert(sizeof(eCGfxShared::eSGfxMacro) == 0x10);

// eSGfxHLSLShaderSrc (eCAPIHLSLShader::Compile 0x749810, eCAPIDevice::PreprocessShader 0x73e780).
struct eCGfxShared::eSGfxHLSLShaderSrc {
    EngineString source;            // fromFile ? virtual file path (opened through eCArchiveManager) : the HLSL text
    const eSGfxMacro* macros;       // +8: D3DXMACRO array or null
    EngineString function;          // +0x10: entry point name
    std::int32_t profile;           // +0x18: index into the profile table 0x9eb060: 0 ps_1_1, 1 ps_1_2, 2 ps_1_3, 3 ps_1_4,
                                    //        4 ps_2_0, 5 ps_2_a, 6 ps_2_b, 7 ps_3_0, 8 vs_1_1, 9 vs_2_0, 10 vs_2_a, 11 vs_3_0
    std::uint32_t flags;            // +0x1c: D3DXSHADER_* compile flags
    bool fromFile;                  // +0x20
};
static_assert(sizeof(eCGfxShared::eSGfxHLSLShaderSrc) == 0x28);
static_assert(offsetof(eCGfxShared::eSGfxHLSLShaderSrc, macros) == 0x8);
static_assert(offsetof(eCGfxShared::eSGfxHLSLShaderSrc, function) == 0x10);
static_assert(offsetof(eCGfxShared::eSGfxHLSLShaderSrc, profile) == 0x18);
static_assert(offsetof(eCGfxShared::eSGfxHLSLShaderSrc, flags) == 0x1c);
static_assert(offsetof(eCGfxShared::eSGfxHLSLShaderSrc, fromFile) == 0x20);

// eSGfxHLSLConstantDesc (GetConstantDescription 0x74a000: D3DXCONSTANT_DESC reordered, plus the constant's handle).
struct eCGfxShared::eSGfxHLSLConstantDesc {
    const char* name;              // points into the constant table (valid while the table lives)
    void* handle;                  // +8: D3DXHANDLE of the constant (GetConstant / GetConstantElement)
    std::uint32_t registerSet;     // +0x10: D3DXREGISTER_SET: 0 BOOL, 1 INT4, 2 FLOAT4, 3 SAMPLER
    std::uint32_t parameterClass;  // +0x14: D3DXPARAMETER_CLASS: 0 SCALAR, 1 VECTOR, 2 MATRIX_ROWS, 3 MATRIX_COLUMNS,
                                   //        4 OBJECT, 5 STRUCT
    std::uint32_t parameterType;   // +0x18: D3DXPARAMETER_TYPE: 0 VOID, 1 BOOL, 2 INT, 3 FLOAT, 4 STRING, 5 TEXTURE,
                                   //        ..., 10 SAMPLER, 11 SAMPLER1D, 12 SAMPLER2D, 13 SAMPLER3D, 14 SAMPLERCUBE
    std::uint32_t rows;
    std::uint32_t columns;
    std::uint32_t elements;
    std::uint32_t structMembers;
    std::uint32_t bytes;
    std::uint32_t registerIndex;   // +0x30
    std::uint32_t registerCount;   // +0x34
};
static_assert(sizeof(eCGfxShared::eSGfxHLSLConstantDesc) == 0x38);
static_assert(offsetof(eCGfxShared::eSGfxHLSLConstantDesc, registerSet) == 0x10);
static_assert(offsetof(eCGfxShared::eSGfxHLSLConstantDesc, rows) == 0x1c);
static_assert(offsetof(eCGfxShared::eSGfxHLSLConstantDesc, bytes) == 0x2c);
static_assert(offsetof(eCGfxShared::eSGfxHLSLConstantDesc, registerIndex) == 0x30);
static_assert(offsetof(eCGfxShared::eSGfxHLSLConstantDesc, registerCount) == 0x34);

// eSGfxEffectSource (CompileEffect 0x743b60 -> D3DXCreateEffectCompiler(text, length, macros, NULL, flags)).
struct eCGfxShared::eSGfxEffectSource {
    EngineString source;         // effect text
    std::uint32_t length;        // +8: bytes
    std::uint32_t flags;         // +0xc: D3DXSHADER_* / D3DXFX_* compile flags
    const eSGfxMacro* macros;    // +0x10
};
static_assert(sizeof(eCGfxShared::eSGfxEffectSource) == 0x18);
static_assert(offsetof(eCGfxShared::eSGfxEffectSource, macros) == 0x10);

// eSGfxEffectFileSource (CompileEffectFromFile 0x743cf0: reads the file through eCArchiveManager, then as above).
struct eCGfxShared::eSGfxEffectFileSource {
    EngineString path;           // virtual file path (e.g. an .fx in common\effects.pak)
    std::uint32_t flags;         // +8: compile flags
    const eSGfxMacro* macros;    // +0x10
};
static_assert(sizeof(eCGfxShared::eSGfxEffectFileSource) == 0x18);
static_assert(offsetof(eCGfxShared::eSGfxEffectFileSource, flags) == 0x8);
static_assert(offsetof(eCGfxShared::eSGfxEffectFileSource, macros) == 0x10);

// eSGfxEffectDesc (GetEffectDescription 0x7486e0: from D3DXEFFECT_DESC).
struct eCGfxShared::eSGfxEffectDesc {
    std::uint32_t parameters;  // top-level parameter count
    std::uint32_t techniques;
};
static_assert(sizeof(eCGfxShared::eSGfxEffectDesc) == 8);  // [I] size (only these two dwords are written)

// eSGfxParameterDesc (GetParameterDescription 0x748730: handle = GetParameter(parent, index), then D3DXPARAMETER_DESC).
// The two strings are engine bCStrings assigned with bCString::operator=.
struct eCGfxShared::eSGfxParameterDesc {
    EngineString name;
    void* handle;                  // +8: D3DXHANDLE
    EngineString semantic;         // +0x10: "" when none
    std::uint32_t parameterClass;  // +0x18: D3DXPARAMETER_CLASS
    std::uint32_t parameterType;   // +0x1c: D3DXPARAMETER_TYPE
    std::uint32_t rows;            // +0x20
    std::uint32_t columns;
    std::uint32_t elements;
    std::uint32_t annotations;
    std::uint32_t structMembers;   // +0x30
    std::uint32_t flags;           // +0x34: D3DX_PARAMETER_SHARED 1, LITERAL 2, ANNOTATION 4
    std::uint32_t bytes;           // +0x38
};
static_assert(sizeof(eCGfxShared::eSGfxParameterDesc) == 0x40);
static_assert(offsetof(eCGfxShared::eSGfxParameterDesc, semantic) == 0x10);
static_assert(offsetof(eCGfxShared::eSGfxParameterDesc, parameterClass) == 0x18);
static_assert(offsetof(eCGfxShared::eSGfxParameterDesc, rows) == 0x20);
static_assert(offsetof(eCGfxShared::eSGfxParameterDesc, structMembers) == 0x30);
static_assert(offsetof(eCGfxShared::eSGfxParameterDesc, bytes) == 0x38);

// eSGfxTechniqueDesc (GetTechniqueDescription 0x748820: handle = GetTechnique(index), then D3DXTECHNIQUE_DESC).
struct eCGfxShared::eSGfxTechniqueDesc {
    EngineString name;
    void* handle;                // +8: D3DXHANDLE (what SetTechnique takes)
    std::uint32_t passes;        // +0x10
    std::uint32_t annotations;   // +0x14
};
static_assert(sizeof(eCGfxShared::eSGfxTechniqueDesc) == 0x18);

// eSFontDesc (eCDX9Font ctor 0x748d80 -> D3DXCreateFontW; written back from ID3DXFont::GetDescW).
struct eCGfxShared::eSFontDesc {
    std::int32_t height;
    std::uint32_t width;
    std::uint32_t weight;
    std::uint32_t mipLevels;
    bool italic;                    // +0x10
    std::uint32_t charSet;          // +0x14
    std::uint32_t outputPrecision;  // +0x18
    std::uint32_t quality;          // +0x1c
    std::uint32_t pitchAndFamily;   // +0x20
    EngineUnicodeString faceName;   // +0x28 (bCUnicodeString)
};
static_assert(sizeof(eCGfxShared::eSFontDesc) == 0x30);
static_assert(offsetof(eCGfxShared::eSFontDesc, faceName) == 0x28);

// eSGfxAssistantMethods: GetMethodSupport 0x7452f0 sets 20 bytes to 1 (all D3DX helper methods supported) [I] meaning.
struct eCGfxShared::eSGfxAssistantMethods {
    bool supported[20];  // [I]
};
static_assert(sizeof(eCGfxShared::eSGfxAssistantMethods) == 0x14);

// eSAPIMaterial (SetMaterial 0x742730 -> D3DMATERIAL9). Fixed function only.
struct eCAPIDevice::eSAPIMaterial {
    EngineFloatAlphaColor diffuse;   // +0x00
    EngineFloatAlphaColor ambient;   // +0x20
    EngineFloatAlphaColor specular;  // +0x40
    EngineFloatAlphaColor emissive;  // +0x60
    float power;                     // +0x80
};
static_assert(sizeof(eCAPIDevice::eSAPIMaterial) == 0x88);  // [I] size (power at +0x80 confirmed)
static_assert(offsetof(eCAPIDevice::eSAPIMaterial, power) == 0x80);

// eSGfxLightData (SetLights 0x7422a0: array stride 0xa0 -> D3DLIGHT9). Fixed function only.
struct eCGfxShared::eSGfxLightData {
    std::uint32_t type;              // D3DLIGHTTYPE: 1 POINT, 2 SPOT, 3 DIRECTIONAL
    EngineFloatAlphaColor diffuse;   // +0x08
    EngineFloatAlphaColor specular;  // +0x28
    EngineFloatAlphaColor ambient;   // +0x48
    float position[3];               // +0x68
    float direction[3];              // +0x74
    float range;                     // +0x80
    float falloff;
    float attenuation0;
    float attenuation1;
    float attenuation2;              // +0x90
    float theta;
    float phi;                       // +0x98
};
static_assert(sizeof(eCGfxShared::eSGfxLightData) == 0xa0);
static_assert(offsetof(eCGfxShared::eSGfxLightData, position) == 0x68);
static_assert(offsetof(eCGfxShared::eSGfxLightData, phi) == 0x98);

// Not defined (editor / tool only, never seen at runtime): eSGfxAttributeWeigths (GenerateLODMesh), eSGfxBoxData,
// eSGfxCylinderData, eSGfxPolygonData, eSGfxSphereData, eSGfxTextData, eSGfxTorusData (D3DX shapes), eSSpriteDesc
// (unused by eCDX9Sprite).

}  // namespace rvk::hal
