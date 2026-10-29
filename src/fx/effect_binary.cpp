#include "fx/effect_binary.h"

// Format knowledge: Wine's d3dx9 (dlls/d3dx9_36/effect.c, preshader.c, shader.c) and vkd3d-shader (fx.c), verified on
// the game's effects (docs/research/fx2.md). Own implementation.

#include <algorithm>
#include <bit>
#include <cctype>
#include <cmath>
#include <cstring>
#include <format>
#include <limits>
#include <utility>

#include "shader/sm_ir.h"

namespace rvk::fx {

// ==================================================================================================================
// names, state table
// ==================================================================================================================

const char* className(ParamClass c) {
    switch (c) {
        case ParamClass::Scalar: return "SCALAR";
        case ParamClass::Vector: return "VECTOR";
        case ParamClass::MatrixRows: return "MATRIX_ROWS";
        case ParamClass::MatrixColumns: return "MATRIX_COLUMNS";
        case ParamClass::Object: return "OBJECT";
        case ParamClass::Struct: return "STRUCT";
    }
    return "?";
}

const char* typeName(ParamType t) {
    static constexpr const char* names[] = {
        "void",      "bool",      "int",       "float",       "string",      "texture",     "texture1D",
        "texture2D", "texture3D", "textureCUBE", "sampler",   "sampler1D",   "sampler2D",   "sampler3D",
        "samplerCUBE", "pixelshader", "vertexshader", "pixelfragment", "vertexfragment", "unsupported",
    };
    const auto i = static_cast<std::size_t>(t);
    return i < std::size(names) ? names[i] : "?";
}

bool isSampler(ParamType t) { return t >= ParamType::Sampler && t <= ParamType::SamplerCube; }
bool isTexture(ParamType t) { return t >= ParamType::Texture && t <= ParamType::TextureCube; }
bool isShader(ParamType t) { return t == ParamType::PixelShader || t == ParamType::VertexShader; }
bool isNumeric(ParamType t) { return t == ParamType::Bool || t == ParamType::Int || t == ParamType::Float; }

const char* registerSetName(RegisterSet s) {
    switch (s) {
        case RegisterSet::Bool: return "bool";
        case RegisterSet::Int4: return "int4";
        case RegisterSet::Float4: return "float4";
        case RegisterSet::Sampler: return "sampler";
    }
    return "?";
}

const char* stateKindName(StateKind k) {
    switch (k) {
        case StateKind::Constant: return "constant";
        case StateKind::ParamRef: return "parameter";
        case StateKind::Expression: return "expression";
        case StateKind::ArraySelector: return "array selector";
    }
    return "?";
}

namespace {

// Effect state ids (index) -> D3D9 state. Order = Wine's d3dx9 effect.c state_table; values = the D3D9 enums.
constexpr StateInfo kStates[] = {
    {StateClass::RenderState, 7, "D3DRS_ZENABLE"},  // 0 = 0x0
    {StateClass::RenderState, 8, "D3DRS_FILLMODE"},  // 1 = 0x1
    {StateClass::RenderState, 9, "D3DRS_SHADEMODE"},  // 2 = 0x2
    {StateClass::RenderState, 14, "D3DRS_ZWRITEENABLE"},  // 3 = 0x3
    {StateClass::RenderState, 15, "D3DRS_ALPHATESTENABLE"},  // 4 = 0x4
    {StateClass::RenderState, 16, "D3DRS_LASTPIXEL"},  // 5 = 0x5
    {StateClass::RenderState, 19, "D3DRS_SRCBLEND"},  // 6 = 0x6
    {StateClass::RenderState, 20, "D3DRS_DESTBLEND"},  // 7 = 0x7
    {StateClass::RenderState, 22, "D3DRS_CULLMODE"},  // 8 = 0x8
    {StateClass::RenderState, 23, "D3DRS_ZFUNC"},  // 9 = 0x9
    {StateClass::RenderState, 24, "D3DRS_ALPHAREF"},  // 10 = 0xa
    {StateClass::RenderState, 25, "D3DRS_ALPHAFUNC"},  // 11 = 0xb
    {StateClass::RenderState, 26, "D3DRS_DITHERENABLE"},  // 12 = 0xc
    {StateClass::RenderState, 27, "D3DRS_ALPHABLENDENABLE"},  // 13 = 0xd
    {StateClass::RenderState, 28, "D3DRS_FOGENABLE"},  // 14 = 0xe
    {StateClass::RenderState, 29, "D3DRS_SPECULARENABLE"},  // 15 = 0xf
    {StateClass::RenderState, 34, "D3DRS_FOGCOLOR"},  // 16 = 0x10
    {StateClass::RenderState, 35, "D3DRS_FOGTABLEMODE"},  // 17 = 0x11
    {StateClass::RenderState, 36, "D3DRS_FOGSTART"},  // 18 = 0x12
    {StateClass::RenderState, 37, "D3DRS_FOGEND"},  // 19 = 0x13
    {StateClass::RenderState, 38, "D3DRS_FOGDENSITY"},  // 20 = 0x14
    {StateClass::RenderState, 48, "D3DRS_RANGEFOGENABLE"},  // 21 = 0x15
    {StateClass::RenderState, 52, "D3DRS_STENCILENABLE"},  // 22 = 0x16
    {StateClass::RenderState, 53, "D3DRS_STENCILFAIL"},  // 23 = 0x17
    {StateClass::RenderState, 54, "D3DRS_STENCILZFAIL"},  // 24 = 0x18
    {StateClass::RenderState, 55, "D3DRS_STENCILPASS"},  // 25 = 0x19
    {StateClass::RenderState, 56, "D3DRS_STENCILFUNC"},  // 26 = 0x1a
    {StateClass::RenderState, 57, "D3DRS_STENCILREF"},  // 27 = 0x1b
    {StateClass::RenderState, 58, "D3DRS_STENCILMASK"},  // 28 = 0x1c
    {StateClass::RenderState, 59, "D3DRS_STENCILWRITEMASK"},  // 29 = 0x1d
    {StateClass::RenderState, 60, "D3DRS_TEXTUREFACTOR"},  // 30 = 0x1e
    {StateClass::RenderState, 128, "D3DRS_WRAP0"},  // 31 = 0x1f
    {StateClass::RenderState, 129, "D3DRS_WRAP1"},  // 32 = 0x20
    {StateClass::RenderState, 130, "D3DRS_WRAP2"},  // 33 = 0x21
    {StateClass::RenderState, 131, "D3DRS_WRAP3"},  // 34 = 0x22
    {StateClass::RenderState, 132, "D3DRS_WRAP4"},  // 35 = 0x23
    {StateClass::RenderState, 133, "D3DRS_WRAP5"},  // 36 = 0x24
    {StateClass::RenderState, 134, "D3DRS_WRAP6"},  // 37 = 0x25
    {StateClass::RenderState, 135, "D3DRS_WRAP7"},  // 38 = 0x26
    {StateClass::RenderState, 198, "D3DRS_WRAP8"},  // 39 = 0x27
    {StateClass::RenderState, 199, "D3DRS_WRAP9"},  // 40 = 0x28
    {StateClass::RenderState, 200, "D3DRS_WRAP10"},  // 41 = 0x29
    {StateClass::RenderState, 201, "D3DRS_WRAP11"},  // 42 = 0x2a
    {StateClass::RenderState, 202, "D3DRS_WRAP12"},  // 43 = 0x2b
    {StateClass::RenderState, 203, "D3DRS_WRAP13"},  // 44 = 0x2c
    {StateClass::RenderState, 204, "D3DRS_WRAP14"},  // 45 = 0x2d
    {StateClass::RenderState, 205, "D3DRS_WRAP15"},  // 46 = 0x2e
    {StateClass::RenderState, 136, "D3DRS_CLIPPING"},  // 47 = 0x2f
    {StateClass::RenderState, 137, "D3DRS_LIGHTING"},  // 48 = 0x30
    {StateClass::RenderState, 139, "D3DRS_AMBIENT"},  // 49 = 0x31
    {StateClass::RenderState, 140, "D3DRS_FOGVERTEXMODE"},  // 50 = 0x32
    {StateClass::RenderState, 141, "D3DRS_COLORVERTEX"},  // 51 = 0x33
    {StateClass::RenderState, 142, "D3DRS_LOCALVIEWER"},  // 52 = 0x34
    {StateClass::RenderState, 143, "D3DRS_NORMALIZENORMALS"},  // 53 = 0x35
    {StateClass::RenderState, 145, "D3DRS_DIFFUSEMATERIALSOURCE"},  // 54 = 0x36
    {StateClass::RenderState, 146, "D3DRS_SPECULARMATERIALSOURCE"},  // 55 = 0x37
    {StateClass::RenderState, 147, "D3DRS_AMBIENTMATERIALSOURCE"},  // 56 = 0x38
    {StateClass::RenderState, 148, "D3DRS_EMISSIVEMATERIALSOURCE"},  // 57 = 0x39
    {StateClass::RenderState, 151, "D3DRS_VERTEXBLEND"},  // 58 = 0x3a
    {StateClass::RenderState, 152, "D3DRS_CLIPPLANEENABLE"},  // 59 = 0x3b
    {StateClass::RenderState, 154, "D3DRS_POINTSIZE"},  // 60 = 0x3c
    {StateClass::RenderState, 155, "D3DRS_POINTSIZE_MIN"},  // 61 = 0x3d
    {StateClass::RenderState, 166, "D3DRS_POINTSIZE_MAX"},  // 62 = 0x3e
    {StateClass::RenderState, 156, "D3DRS_POINTSPRITEENABLE"},  // 63 = 0x3f
    {StateClass::RenderState, 157, "D3DRS_POINTSCALEENABLE"},  // 64 = 0x40
    {StateClass::RenderState, 158, "D3DRS_POINTSCALE_A"},  // 65 = 0x41
    {StateClass::RenderState, 159, "D3DRS_POINTSCALE_B"},  // 66 = 0x42
    {StateClass::RenderState, 160, "D3DRS_POINTSCALE_C"},  // 67 = 0x43
    {StateClass::RenderState, 161, "D3DRS_MULTISAMPLEANTIALIAS"},  // 68 = 0x44
    {StateClass::RenderState, 162, "D3DRS_MULTISAMPLEMASK"},  // 69 = 0x45
    {StateClass::RenderState, 163, "D3DRS_PATCHEDGESTYLE"},  // 70 = 0x46
    {StateClass::RenderState, 165, "D3DRS_DEBUGMONITORTOKEN"},  // 71 = 0x47
    {StateClass::RenderState, 167, "D3DRS_INDEXEDVERTEXBLENDENABLE"},  // 72 = 0x48
    {StateClass::RenderState, 168, "D3DRS_COLORWRITEENABLE"},  // 73 = 0x49
    {StateClass::RenderState, 170, "D3DRS_TWEENFACTOR"},  // 74 = 0x4a
    {StateClass::RenderState, 171, "D3DRS_BLENDOP"},  // 75 = 0x4b
    {StateClass::RenderState, 172, "D3DRS_POSITIONDEGREE"},  // 76 = 0x4c
    {StateClass::RenderState, 173, "D3DRS_NORMALDEGREE"},  // 77 = 0x4d
    {StateClass::RenderState, 174, "D3DRS_SCISSORTESTENABLE"},  // 78 = 0x4e
    {StateClass::RenderState, 175, "D3DRS_SLOPESCALEDEPTHBIAS"},  // 79 = 0x4f
    {StateClass::RenderState, 176, "D3DRS_ANTIALIASEDLINEENABLE"},  // 80 = 0x50
    {StateClass::RenderState, 178, "D3DRS_MINTESSELLATIONLEVEL"},  // 81 = 0x51
    {StateClass::RenderState, 179, "D3DRS_MAXTESSELLATIONLEVEL"},  // 82 = 0x52
    {StateClass::RenderState, 180, "D3DRS_ADAPTIVETESS_X"},  // 83 = 0x53
    {StateClass::RenderState, 181, "D3DRS_ADAPTIVETESS_Y"},  // 84 = 0x54
    {StateClass::RenderState, 182, "D3DRS_ADAPTIVETESS_Z"},  // 85 = 0x55
    {StateClass::RenderState, 183, "D3DRS_ADAPTIVETESS_W"},  // 86 = 0x56
    {StateClass::RenderState, 184, "D3DRS_ENABLEADAPTIVETESSELLATION"},  // 87 = 0x57
    {StateClass::RenderState, 185, "D3DRS_TWOSIDEDSTENCILMODE"},  // 88 = 0x58
    {StateClass::RenderState, 186, "D3DRS_CCW_STENCILFAIL"},  // 89 = 0x59
    {StateClass::RenderState, 187, "D3DRS_CCW_STENCILZFAIL"},  // 90 = 0x5a
    {StateClass::RenderState, 188, "D3DRS_CCW_STENCILPASS"},  // 91 = 0x5b
    {StateClass::RenderState, 189, "D3DRS_CCW_STENCILFUNC"},  // 92 = 0x5c
    {StateClass::RenderState, 190, "D3DRS_COLORWRITEENABLE1"},  // 93 = 0x5d
    {StateClass::RenderState, 191, "D3DRS_COLORWRITEENABLE2"},  // 94 = 0x5e
    {StateClass::RenderState, 192, "D3DRS_COLORWRITEENABLE3"},  // 95 = 0x5f
    {StateClass::RenderState, 193, "D3DRS_BLENDFACTOR"},  // 96 = 0x60
    {StateClass::RenderState, 194, "D3DRS_SRGBWRITEENABLE"},  // 97 = 0x61
    {StateClass::RenderState, 195, "D3DRS_DEPTHBIAS"},  // 98 = 0x62
    {StateClass::RenderState, 206, "D3DRS_SEPARATEALPHABLENDENABLE"},  // 99 = 0x63
    {StateClass::RenderState, 207, "D3DRS_SRCBLENDALPHA"},  // 100 = 0x64
    {StateClass::RenderState, 208, "D3DRS_DESTBLENDALPHA"},  // 101 = 0x65
    {StateClass::RenderState, 209, "D3DRS_BLENDOPALPHA"},  // 102 = 0x66
    {StateClass::TextureStage, 1, "D3DTSS_COLOROP"},  // 103 = 0x67
    {StateClass::TextureStage, 26, "D3DTSS_COLORARG0"},  // 104 = 0x68
    {StateClass::TextureStage, 2, "D3DTSS_COLORARG1"},  // 105 = 0x69
    {StateClass::TextureStage, 3, "D3DTSS_COLORARG2"},  // 106 = 0x6a
    {StateClass::TextureStage, 4, "D3DTSS_ALPHAOP"},  // 107 = 0x6b
    {StateClass::TextureStage, 27, "D3DTSS_ALPHAARG0"},  // 108 = 0x6c
    {StateClass::TextureStage, 5, "D3DTSS_ALPHAARG1"},  // 109 = 0x6d
    {StateClass::TextureStage, 6, "D3DTSS_ALPHAARG2"},  // 110 = 0x6e
    {StateClass::TextureStage, 28, "D3DTSS_RESULTARG"},  // 111 = 0x6f
    {StateClass::TextureStage, 7, "D3DTSS_BUMPENVMAT00"},  // 112 = 0x70
    {StateClass::TextureStage, 8, "D3DTSS_BUMPENVMAT01"},  // 113 = 0x71
    {StateClass::TextureStage, 9, "D3DTSS_BUMPENVMAT10"},  // 114 = 0x72
    {StateClass::TextureStage, 10, "D3DTSS_BUMPENVMAT11"},  // 115 = 0x73
    {StateClass::TextureStage, 11, "D3DTSS_TEXCOORDINDEX"},  // 116 = 0x74
    {StateClass::TextureStage, 22, "D3DTSS_BUMPENVLSCALE"},  // 117 = 0x75
    {StateClass::TextureStage, 23, "D3DTSS_BUMPENVLOFFSET"},  // 118 = 0x76
    {StateClass::TextureStage, 24, "D3DTSS_TEXTURETRANSFORMFLAGS"},  // 119 = 0x77
    {StateClass::TextureStage, 32, "D3DTSS_CONSTANT"},  // 120 = 0x78
    {StateClass::NPatchMode, 0, "NPatchMode"},  // 121 = 0x79
    {StateClass::Fvf, 0, "FVF"},  // 122 = 0x7a
    {StateClass::Transform, 3, "D3DTS_PROJECTION"},  // 123 = 0x7b
    {StateClass::Transform, 2, "D3DTS_VIEW"},  // 124 = 0x7c
    {StateClass::Transform, 256, "D3DTS_WORLD"},  // 125 = 0x7d
    {StateClass::Transform, 16, "D3DTS_TEXTURE0"},  // 126 = 0x7e
    {StateClass::Material, std::to_underlying(MaterialOp::Diffuse), "MaterialDiffuse"},  // 127 = 0x7f
    {StateClass::Material, std::to_underlying(MaterialOp::Ambient), "MaterialAmbient"},  // 128 = 0x80
    {StateClass::Material, std::to_underlying(MaterialOp::Specular), "MaterialSpecular"},  // 129 = 0x81
    {StateClass::Material, std::to_underlying(MaterialOp::Emissive), "MaterialEmissive"},  // 130 = 0x82
    {StateClass::Material, std::to_underlying(MaterialOp::Power), "MaterialPower"},  // 131 = 0x83
    {StateClass::Light, std::to_underlying(LightOp::Type), "LightType"},  // 132 = 0x84
    {StateClass::Light, std::to_underlying(LightOp::Diffuse), "LightDiffuse"},  // 133 = 0x85
    {StateClass::Light, std::to_underlying(LightOp::Specular), "LightSpecular"},  // 134 = 0x86
    {StateClass::Light, std::to_underlying(LightOp::Ambient), "LightAmbient"},  // 135 = 0x87
    {StateClass::Light, std::to_underlying(LightOp::Position), "LightPosition"},  // 136 = 0x88
    {StateClass::Light, std::to_underlying(LightOp::Direction), "LightDirection"},  // 137 = 0x89
    {StateClass::Light, std::to_underlying(LightOp::Range), "LightRange"},  // 138 = 0x8a
    {StateClass::Light, std::to_underlying(LightOp::FallOff), "LightFallOff"},  // 139 = 0x8b
    {StateClass::Light, std::to_underlying(LightOp::Attenuation0), "LightAttenuation0"},  // 140 = 0x8c
    {StateClass::Light, std::to_underlying(LightOp::Attenuation1), "LightAttenuation1"},  // 141 = 0x8d
    {StateClass::Light, std::to_underlying(LightOp::Attenuation2), "LightAttenuation2"},  // 142 = 0x8e
    {StateClass::Light, std::to_underlying(LightOp::Theta), "LightTheta"},  // 143 = 0x8f
    {StateClass::Light, std::to_underlying(LightOp::Phi), "LightPhi"},  // 144 = 0x90
    {StateClass::LightEnable, 0, "LightEnable"},  // 145 = 0x91
    {StateClass::VertexShader, 0, "Vertexshader"},  // 146 = 0x92
    {StateClass::PixelShader, 0, "Pixelshader"},  // 147 = 0x93
    {StateClass::ShaderConst, std::to_underlying(ShaderConstOp::VsFloat), "VertexShaderConstantF"},  // 148 = 0x94
    {StateClass::ShaderConst, std::to_underlying(ShaderConstOp::VsBool), "VertexShaderConstantB"},  // 149 = 0x95
    {StateClass::ShaderConst, std::to_underlying(ShaderConstOp::VsInt), "VertexShaderConstantI"},  // 150 = 0x96
    {StateClass::ShaderConst, std::to_underlying(ShaderConstOp::VsFloat), "VertexShaderConstant"},  // 151 = 0x97
    {StateClass::ShaderConst, std::to_underlying(ShaderConstOp::VsFloat), "VertexShaderConstant1"},  // 152 = 0x98
    {StateClass::ShaderConst, std::to_underlying(ShaderConstOp::VsFloat), "VertexShaderConstant2"},  // 153 = 0x99
    {StateClass::ShaderConst, std::to_underlying(ShaderConstOp::VsFloat), "VertexShaderConstant3"},  // 154 = 0x9a
    {StateClass::ShaderConst, std::to_underlying(ShaderConstOp::VsFloat), "VertexShaderConstant4"},  // 155 = 0x9b
    {StateClass::ShaderConst, std::to_underlying(ShaderConstOp::PsFloat), "PixelShaderConstantF"},  // 156 = 0x9c
    {StateClass::ShaderConst, std::to_underlying(ShaderConstOp::PsBool), "PixelShaderConstantB"},  // 157 = 0x9d
    {StateClass::ShaderConst, std::to_underlying(ShaderConstOp::PsInt), "PixelShaderConstantI"},  // 158 = 0x9e
    {StateClass::ShaderConst, std::to_underlying(ShaderConstOp::PsFloat), "PixelShaderConstant"},  // 159 = 0x9f
    {StateClass::ShaderConst, std::to_underlying(ShaderConstOp::PsFloat), "PixelShaderConstant1"},  // 160 = 0xa0
    {StateClass::ShaderConst, std::to_underlying(ShaderConstOp::PsFloat), "PixelShaderConstant2"},  // 161 = 0xa1
    {StateClass::ShaderConst, std::to_underlying(ShaderConstOp::PsFloat), "PixelShaderConstant3"},  // 162 = 0xa2
    {StateClass::ShaderConst, std::to_underlying(ShaderConstOp::PsFloat), "PixelShaderConstant4"},  // 163 = 0xa3
    {StateClass::Texture, 0, "Texture"},  // 164 = 0xa4
    {StateClass::SamplerState, 1, "AddressU"},  // 165 = 0xa5
    {StateClass::SamplerState, 2, "AddressV"},  // 166 = 0xa6
    {StateClass::SamplerState, 3, "AddressW"},  // 167 = 0xa7
    {StateClass::SamplerState, 4, "BorderColor"},  // 168 = 0xa8
    {StateClass::SamplerState, 5, "MagFilter"},  // 169 = 0xa9
    {StateClass::SamplerState, 6, "MinFilter"},  // 170 = 0xaa
    {StateClass::SamplerState, 7, "MipFilter"},  // 171 = 0xab
    {StateClass::SamplerState, 8, "MipMapLodBias"},  // 172 = 0xac
    {StateClass::SamplerState, 9, "MaxMipLevel"},  // 173 = 0xad
    {StateClass::SamplerState, 10, "MaxAnisotropy"},  // 174 = 0xae
    {StateClass::SamplerState, 11, "SRGBTexture"},  // 175 = 0xaf
    {StateClass::SamplerState, 12, "ElementIndex"},  // 176 = 0xb0
    {StateClass::SamplerState, 13, "DMAPOffset"},  // 177 = 0xb1
    {StateClass::SetSampler, 0, "Sampler"},  // 178 = 0xb2
};

}  // namespace

std::span<const StateInfo> stateTable() { return kStates; }

const StateInfo* stateInfo(std::uint32_t stateId) {
    return stateId < std::size(kStates) ? &kStates[stateId] : nullptr;
}

std::string stateValueName(std::uint32_t stateId, std::uint32_t value) {
    const StateInfo* info = stateInfo(stateId);
    if (info == nullptr) {
        return {};
    }
    const std::string_view n = info->name;
    auto pick = [value](std::initializer_list<std::pair<std::uint32_t, const char*>> list) -> std::string {
        for (const auto& [v, s] : list) {
            if (v == value) {
                return s;
            }
        }
        return {};
    };
    if (info->cls == StateClass::RenderState) {
        if (n == "D3DRS_ZENABLE") {
            return pick({{0, "D3DZB_FALSE"}, {1, "D3DZB_TRUE"}, {2, "D3DZB_USEW"}});
        }
        if (n == "D3DRS_FILLMODE") {
            return pick({{1, "D3DFILL_POINT"}, {2, "D3DFILL_WIREFRAME"}, {3, "D3DFILL_SOLID"}});
        }
        if (n == "D3DRS_SHADEMODE") {
            return pick({{1, "D3DSHADE_FLAT"}, {2, "D3DSHADE_GOURAUD"}, {3, "D3DSHADE_PHONG"}});
        }
        if (n == "D3DRS_SRCBLEND" || n == "D3DRS_DESTBLEND" || n == "D3DRS_SRCBLENDALPHA" || n == "D3DRS_DESTBLENDALPHA") {
            return pick({{1, "D3DBLEND_ZERO"}, {2, "D3DBLEND_ONE"}, {3, "D3DBLEND_SRCCOLOR"}, {4, "D3DBLEND_INVSRCCOLOR"},
                         {5, "D3DBLEND_SRCALPHA"}, {6, "D3DBLEND_INVSRCALPHA"}, {7, "D3DBLEND_DESTALPHA"},
                         {8, "D3DBLEND_INVDESTALPHA"}, {9, "D3DBLEND_DESTCOLOR"}, {10, "D3DBLEND_INVDESTCOLOR"},
                         {11, "D3DBLEND_SRCALPHASAT"}, {12, "D3DBLEND_BOTHSRCALPHA"}, {13, "D3DBLEND_BOTHINVSRCALPHA"},
                         {14, "D3DBLEND_BLENDFACTOR"}, {15, "D3DBLEND_INVBLENDFACTOR"}});
        }
        if (n == "D3DRS_CULLMODE") {
            return pick({{1, "D3DCULL_NONE"}, {2, "D3DCULL_CW"}, {3, "D3DCULL_CCW"}});
        }
        if (n == "D3DRS_ZFUNC" || n == "D3DRS_ALPHAFUNC" || n == "D3DRS_STENCILFUNC" || n == "D3DRS_CCW_STENCILFUNC") {
            return pick({{1, "D3DCMP_NEVER"}, {2, "D3DCMP_LESS"}, {3, "D3DCMP_EQUAL"}, {4, "D3DCMP_LESSEQUAL"},
                         {5, "D3DCMP_GREATER"}, {6, "D3DCMP_NOTEQUAL"}, {7, "D3DCMP_GREATEREQUAL"}, {8, "D3DCMP_ALWAYS"}});
        }
        if (n.find("STENCILFAIL") != n.npos || n.find("STENCILZFAIL") != n.npos || n.find("STENCILPASS") != n.npos) {
            return pick({{1, "D3DSTENCILOP_KEEP"}, {2, "D3DSTENCILOP_ZERO"}, {3, "D3DSTENCILOP_REPLACE"},
                         {4, "D3DSTENCILOP_INCRSAT"}, {5, "D3DSTENCILOP_DECRSAT"}, {6, "D3DSTENCILOP_INVERT"},
                         {7, "D3DSTENCILOP_INCR"}, {8, "D3DSTENCILOP_DECR"}});
        }
        if (n == "D3DRS_BLENDOP" || n == "D3DRS_BLENDOPALPHA") {
            return pick({{1, "D3DBLENDOP_ADD"}, {2, "D3DBLENDOP_SUBTRACT"}, {3, "D3DBLENDOP_REVSUBTRACT"},
                         {4, "D3DBLENDOP_MIN"}, {5, "D3DBLENDOP_MAX"}});
        }
        if (n == "D3DRS_FOGTABLEMODE" || n == "D3DRS_FOGVERTEXMODE") {
            return pick({{0, "D3DFOG_NONE"}, {1, "D3DFOG_EXP"}, {2, "D3DFOG_EXP2"}, {3, "D3DFOG_LINEAR"}});
        }
        if (n.starts_with("D3DRS_COLORWRITEENABLE")) {
            std::string s;
            for (int i = 0; i < 4; ++i) {
                if ((value & (1u << i)) != 0) {
                    s += "RGBA"[i];
                }
            }
            return value <= 15 ? (s.empty() ? std::string("0") : s) : std::string{};
        }
        if (n.ends_with("ENABLE") || n == "D3DRS_LASTPIXEL" || n == "D3DRS_CLIPPING" || n == "D3DRS_LIGHTING" ||
            n == "D3DRS_COLORVERTEX" || n == "D3DRS_LOCALVIEWER" || n == "D3DRS_NORMALIZENORMALS" ||
            n == "D3DRS_TWOSIDEDSTENCILMODE" || n == "D3DRS_SEPARATEALPHABLENDENABLE") {
            return pick({{0, "FALSE"}, {1, "TRUE"}});
        }
        return {};
    }
    if (info->cls == StateClass::SamplerState) {
        if (n.starts_with("Address")) {
            return pick({{1, "D3DTADDRESS_WRAP"}, {2, "D3DTADDRESS_MIRROR"}, {3, "D3DTADDRESS_CLAMP"},
                         {4, "D3DTADDRESS_BORDER"}, {5, "D3DTADDRESS_MIRRORONCE"}});
        }
        if (n.ends_with("Filter")) {
            return pick({{0, "D3DTEXF_NONE"}, {1, "D3DTEXF_POINT"}, {2, "D3DTEXF_LINEAR"}, {3, "D3DTEXF_ANISOTROPIC"},
                         {6, "D3DTEXF_PYRAMIDALQUAD"}, {7, "D3DTEXF_GAUSSIANQUAD"}});
        }
        if (n == "SRGBTexture") {
            return pick({{0, "FALSE"}, {1, "TRUE"}});
        }
    }
    return {};
}

const char* presOpName(PresOp op) {
    switch (op) {
        case PresOp::Nop: return "nop";
        case PresOp::Mov: return "mov";
        case PresOp::Neg: return "neg";
        case PresOp::Rcp: return "rcp";
        case PresOp::Frc: return "frc";
        case PresOp::Exp: return "exp";
        case PresOp::Log: return "log";
        case PresOp::Rsq: return "rsq";
        case PresOp::Sin: return "sin";
        case PresOp::Cos: return "cos";
        case PresOp::Asin: return "asin";
        case PresOp::Acos: return "acos";
        case PresOp::Atan: return "atan";
        case PresOp::Sqrt: return "sqrt";
        case PresOp::Ineg: return "ineg";
        case PresOp::Not: return "not";
        case PresOp::Itof: return "itof";
        case PresOp::Utof: return "utof";
        case PresOp::Ftou: return "ftou";
        case PresOp::Ftob: return "ftob";
        case PresOp::Floor: return "floor";
        case PresOp::Ceil: return "ceil";
        case PresOp::Min: return "min";
        case PresOp::Max: return "max";
        case PresOp::Lt: return "lt";
        case PresOp::Ge: return "ge";
        case PresOp::Add: return "add";
        case PresOp::Mul: return "mul";
        case PresOp::Atan2: return "atan2";
        case PresOp::Div: return "div";
        case PresOp::Cmp: return "cmp";
        case PresOp::Movc: return "movc";
        case PresOp::Dot: return "dot";
        case PresOp::Noise: return "noise";
        case PresOp::DotSwiz: return "d3ds_dotswiz";
    }
    return "?";
}

const char* presTableName(PresTable t) {
    switch (t) {
        case PresTable::None: return "?";
        case PresTable::Literal: return "imm";
        case PresTable::Input: return "c";
        case PresTable::VInput: return "v";
        case PresTable::Output: return "oc";
        case PresTable::OutBool: return "ob";
        case PresTable::OutInt: return "oi";
        case PresTable::Temp: return "r";
    }
    return "?";
}

// ==================================================================================================================
// low-level reading
// ==================================================================================================================

namespace {

struct ParseError {
    std::string message;
};

[[noreturn]] void fail(std::string message) { throw ParseError{std::move(message)}; }

std::uint32_t loadU32(const std::byte* p) {
    std::uint32_t v;
    std::memcpy(&v, p, 4);
    return v;
}

std::uint16_t loadU16(const std::byte* p) {
    std::uint16_t v;
    std::memcpy(&v, p, 2);
    return v;
}

// Bounds-checked view of a byte range; offsets are relative to its start.
class Bytes {
public:
    explicit Bytes(std::span<const std::byte> d, const char* what) : d_(d), what_(what) {}

    std::size_t size() const { return d_.size(); }
    void need(std::size_t off, std::size_t n) const {
        if (off > d_.size() || d_.size() - off < n) {
            fail(std::format("{}: {} bytes at 0x{:x} exceed the size 0x{:x}", what_, n, off, d_.size()));
        }
    }
    std::uint32_t u32(std::size_t off) const {
        need(off, 4);
        return loadU32(d_.data() + off);
    }
    std::uint16_t u16(std::size_t off) const {
        need(off, 2);
        return loadU16(d_.data() + off);
    }
    std::uint32_t read(std::size_t& pos) const {
        const std::uint32_t v = u32(pos);
        pos += 4;
        return v;
    }
    std::span<const std::byte> span(std::size_t off, std::size_t n) const {
        need(off, n);
        return d_.subspan(off, n);
    }
    // NUL-terminated string starting at `off` (bounded by the range)
    std::string cstr(std::size_t off) const {
        if (off >= d_.size()) {
            fail(std::format("{}: string offset 0x{:x} out of range", what_, off));
        }
        std::string s;
        for (std::size_t i = off; i < d_.size() && d_[i] != std::byte{0}; ++i) {
            s.push_back(static_cast<char>(d_[i]));
        }
        return s;
    }

private:
    std::span<const std::byte> d_;
    const char* what_;
};

std::string stripNul(std::span<const std::byte> s) {
    std::string r(reinterpret_cast<const char*>(s.data()), s.size());
    while (!r.empty() && r.back() == '\0') {
        r.pop_back();
    }
    return r;
}

std::vector<std::uint32_t> toTokens(std::span<const std::byte> b) {
    std::vector<std::uint32_t> t(b.size() / 4);
    if (!t.empty()) {
        std::memcpy(t.data(), b.data(), t.size() * 4);
    }
    return t;
}

}  // namespace

std::span<const std::uint32_t> findComment(std::span<const std::uint32_t> tokens, std::uint32_t tag) {
    std::size_t i = 1;
    while (i < tokens.size()) {
        const std::uint32_t tok = tokens[i];
        if ((tok & 0xFFFFu) != 0xFFFEu) {
            break;
        }
        const std::size_t len = (tok >> 16) & 0x7FFFu;
        if (len == 0 || len > tokens.size() - i - 1) {
            break;
        }
        if (tokens[i + 1] == tag) {
            return tokens.subspan(i + 2, len - 1);
        }
        i += 1 + len;
    }
    return {};
}

// ==================================================================================================================
// constant tables
// ==================================================================================================================

const Constant* ConstantTable::find(std::string_view n) const {
    for (const Constant& c : constants) {
        if (c.name == n) {
            return &c;
        }
    }
    return nullptr;
}

namespace {

// D3DXSHADER_CONSTANTTABLE (28 bytes), D3DXSHADER_CONSTANTINFO (20), D3DXSHADER_TYPEINFO (16),
// D3DXSHADER_STRUCTMEMBERINFO (8). Register ranges of elements/members are derived like D3DX does: each element/member
// starts after the *untruncated* size of its predecessors, its count is clipped to the parent's range.
// Returns the untruncated register size.
std::uint32_t parseCtabType(const Bytes& ctab, std::uint32_t typeOffset, Constant& c, bool isElement, std::uint32_t index,
                   std::uint32_t maxIndex, std::uint32_t* defaultOffset, const std::string& name, RegisterSet set,
                   int depth) {
    if (depth > 32) {
        fail("CTAB: type nesting too deep");
    }
    ctab.need(typeOffset, 16);
    const auto cls = static_cast<ParamClass>(ctab.u16(typeOffset));
    const auto type = static_cast<ParamType>(ctab.u16(typeOffset + 2));
    const std::uint16_t rows = ctab.u16(typeOffset + 4);
    const std::uint16_t columns = ctab.u16(typeOffset + 6);
    const std::uint16_t elements = ctab.u16(typeOffset + 8);
    const std::uint16_t structMembers = ctab.u16(typeOffset + 10);
    const std::uint32_t memberInfo = ctab.u32(typeOffset + 12);

    c.name = name;
    c.registerSet = set;
    c.registerIndex = index;
    c.cls = cls;
    c.type = type;
    c.rows = rows;
    c.columns = columns;
    c.elements = isElement ? 1u : elements;
    c.structMembers = structMembers;
    const std::uint32_t defaultStart = defaultOffset != nullptr ? *defaultOffset : 0;

    std::uint32_t count = 0;
    bool viaMemberInfo = false;
    if (elements > 1 && !isElement) {
        count = elements;
    } else if (cls == ParamClass::Struct && structMembers != 0) {
        count = structMembers;
        viaMemberInfo = true;
    }

    std::uint32_t size = 0;
    if (count != 0) {
        c.members.resize(count);
        for (std::uint32_t i = 0; i < count; ++i) {
            std::uint32_t memberType = typeOffset;
            std::string memberName = name;
            if (viaMemberInfo) {
                memberName = ctab.cstr(ctab.u32(memberInfo + i * 8));
                memberType = ctab.u32(memberInfo + i * 8 + 4);
            }
            size += parseCtabType(ctab, memberType, c.members[i], !viaMemberInfo, index + size, maxIndex,
                                  defaultOffset, memberName, set, depth + 1);
        }
    } else {
        std::uint32_t offsetDiff = std::uint32_t(rows) * columns;
        size = std::uint32_t(rows) * columns;
        switch (set) {
            case RegisterSet::Bool: break;
            case RegisterSet::Float4:
            case RegisterSet::Int4:
                switch (cls) {
                    case ParamClass::Vector:
                        size = 1;
                        offsetDiff = rows * 4u;
                        break;
                    case ParamClass::Scalar: offsetDiff = rows * 4u; break;
                    case ParamClass::MatrixRows:
                        offsetDiff = rows * 4u;
                        size = rows;
                        break;
                    case ParamClass::MatrixColumns:
                        offsetDiff = columns * 4u;
                        size = columns;
                        break;
                    default: break;
                }
                break;
            case RegisterSet::Sampler: size = 1; break;
        }
        if (defaultOffset != nullptr) {
            *defaultOffset += offsetDiff * 4;
        }
    }
    c.registerCount = maxIndex > index ? std::min(maxIndex - index, size) : 0u;
    c.bytes = 4u * c.elements * rows * columns;
    if (defaultOffset != nullptr && *defaultOffset > defaultStart) {
        const std::uint32_t n = (*defaultOffset - defaultStart) / 4;
        ctab.need(defaultStart, std::size_t(n) * 4);
        c.defaultValue.resize(n);
        for (std::uint32_t i = 0; i < n; ++i) {
            c.defaultValue[i] = ctab.u32(defaultStart + i * 4);
        }
    }
    return size;
}

ConstantTable parseCtab(std::span<const std::byte> data) {
    const Bytes ctab(data, "CTAB");
    ConstantTable t;
    t.present = true;
    if (ctab.u32(0) != 28) {
        fail(std::format("CTAB: header size {} (expected 28)", ctab.u32(0)));
    }
    const std::uint32_t creator = ctab.u32(4);
    t.version = ctab.u32(8);
    const std::uint32_t count = ctab.u32(12);
    const std::uint32_t info = ctab.u32(16);
    t.flags = ctab.u32(20);
    const std::uint32_t target = ctab.u32(24);
    if (creator != 0) {
        t.creator = ctab.cstr(creator);
    }
    if (target != 0) {
        t.target = ctab.cstr(target);
    }
    if (count > 65536) {
        fail("CTAB: implausible constant count");
    }
    ctab.need(info, std::size_t(count) * 20);
    t.constants.resize(count);
    for (std::uint32_t i = 0; i < count; ++i) {
        const std::uint32_t o = info + i * 20;
        const std::string name = ctab.cstr(ctab.u32(o));
        const auto set = static_cast<RegisterSet>(ctab.u16(o + 4));
        if (ctab.u16(o + 4) > 3) {
            fail(std::format("CTAB: constant '{}' has register set {}", name, ctab.u16(o + 4)));
        }
        const std::uint16_t regIndex = ctab.u16(o + 6);
        const std::uint16_t regCount = ctab.u16(o + 8);
        const std::uint16_t reserved = ctab.u16(o + 10);
        const std::uint32_t typeInfo = ctab.u32(o + 12);
        std::uint32_t defaultValue = ctab.u32(o + 16);
        Constant& c = t.constants[i];
        parseCtabType(ctab, typeInfo, c, false, regIndex, std::uint32_t(regIndex) + regCount,
                      defaultValue != 0 ? &defaultValue : nullptr, name, set, 0);
        if (set == RegisterSet::Int4) {
            c.registerCount = regCount;  // D3DX reports the raw count for top-level int4 constants
        }
        c.reserved = reserved;
    }
    return t;
}

}  // namespace

ConstantTableResult parseConstantTableData(std::span<const std::byte> ctab) {
    ConstantTableResult r;
    try {
        r.table = parseCtab(ctab);
    } catch (const ParseError& e) {
        r.error = e.message;
        r.table = {};
    }
    return r;
}

ConstantTableResult parseConstantTable(std::span<const std::uint32_t> shaderTokens) {
    const auto body = findComment(shaderTokens, fourcc('C', 'T', 'A', 'B'));
    if (body.empty()) {
        return {};
    }
    return parseConstantTableData(std::as_bytes(body));
}

// ==================================================================================================================
// preshaders
// ==================================================================================================================

namespace {

struct OpInfo {
    PresOp op;
    std::uint32_t inputs;     // operand count (0 = 6 or 8: dotswiz)
    bool allComponents;       // one result from all components (dot)
};

constexpr OpInfo kOps[] = {
    {PresOp::Nop, 0, false},   {PresOp::Mov, 1, false},   {PresOp::Neg, 1, false},   {PresOp::Rcp, 1, false},
    {PresOp::Frc, 1, false},   {PresOp::Exp, 1, false},   {PresOp::Log, 1, false},   {PresOp::Rsq, 1, false},
    {PresOp::Sin, 1, false},   {PresOp::Cos, 1, false},   {PresOp::Asin, 1, false},  {PresOp::Acos, 1, false},
    {PresOp::Atan, 1, false},  {PresOp::Sqrt, 1, false},  {PresOp::Floor, 1, false}, {PresOp::Ceil, 1, false},
    {PresOp::Min, 2, false},   {PresOp::Max, 2, false},   {PresOp::Lt, 2, false},    {PresOp::Ge, 2, false},
    {PresOp::Add, 2, false},   {PresOp::Mul, 2, false},   {PresOp::Atan2, 2, false}, {PresOp::Div, 2, false},
    {PresOp::Cmp, 3, false},   {PresOp::Movc, 3, false},  {PresOp::Dot, 2, true},    {PresOp::DotSwiz, 0, false},
};

const OpInfo* findOp(std::uint32_t code) {
    for (const OpInfo& o : kOps) {
        if (static_cast<std::uint32_t>(o.op) == code) {
            return &o;
        }
    }
    return nullptr;
}

std::uint32_t componentsPerRegister(PresTable t) { return t == PresTable::OutBool ? 1u : 4u; }

class TokenReader {
public:
    explicit TokenReader(std::span<const std::uint32_t> t) : t_(t) {}
    std::uint32_t next() {
        if (pos_ >= t_.size()) {
            fail("FXLC: instruction stream truncated");
        }
        return t_[pos_++];
    }

private:
    std::span<const std::uint32_t> t_;
    std::size_t pos_ = 0;
};

PresRegister readPresRegister(TokenReader& r) {
    const std::uint32_t table = r.next();
    if (table < 1 || table > 7) {
        fail(std::format("FXLC: unknown register table {}", table));
    }
    PresRegister reg;
    reg.table = static_cast<PresTable>(table);
    reg.offset = r.next();
    return reg;
}

PresOperand readPresOperand(TokenReader& r) {
    PresOperand o;
    const std::uint32_t indexCount = r.next();
    if (indexCount > 1) {
        fail(std::format("FXLC: {} index registers", indexCount));
    }
    if (indexCount == 1) {
        o.relative = true;
        o.index = readPresRegister(r);
    }
    o.reg = readPresRegister(r);
    if (o.reg.table == PresTable::OutBool) {
        o.reg.offset /= 4;
    }
    return o;
}

Preshader parsePres(std::span<const std::uint32_t> t) {
    if (t.empty()) {
        fail("preshader: empty");
    }
    Preshader p;
    p.tokens.assign(t.begin(), t.end());
    p.version = t[0];
    if (const auto clit = findComment(t, fourcc('C', 'L', 'I', 'T')); !clit.empty()) {
        const std::uint32_t n = clit[0];
        if (n > (clit.size() - 1) / 2) {
            fail(std::format("CLIT: {} literals do not fit {} dwords", n, clit.size()));
        }
        if (n % 4 != 0) {
            fail(std::format("CLIT: literal count {} is not a multiple of 4", n));
        }
        p.literals.resize(n);
        std::memcpy(p.literals.data(), clit.data() + 1, std::size_t(n) * 8);
    }
    if (const auto ctab = findComment(t, fourcc('C', 'T', 'A', 'B')); !ctab.empty()) {
        p.inputs = parseCtab(std::as_bytes(ctab));
    }
    if (const auto prsi = findComment(t, fourcc('P', 'R', 'S', 'I')); !prsi.empty()) {
        p.prsi.assign(prsi.begin(), prsi.end());
    }
    const auto fxlc = findComment(t, fourcc('F', 'X', 'L', 'C'));
    if (fxlc.empty()) {
        return p;
    }
    TokenReader r(fxlc.subspan(1));
    const std::uint32_t count = fxlc[0];
    if (count > fxlc.size()) {
        fail("FXLC: implausible instruction count");
    }
    p.code.resize(count);
    for (std::uint32_t i = 0; i < count; ++i) {
        PresInstruction& ins = p.code[i];
        ins.raw = r.next();
        const std::uint32_t inputs = r.next();
        const std::uint32_t opcode = (ins.raw >> 20) & 0x7FFu;
        ins.components = ins.raw & 0xFFFFu;
        ins.scalar = (ins.raw & 0x80000000u) != 0;
        const OpInfo* info = findOp(opcode);
        if (info == nullptr) {
            fail(std::format("FXLC: unsupported opcode 0x{:x} (instruction {}, raw 0x{:08x})", opcode, i, ins.raw));
        }
        ins.op = info->op;
        const bool countOk = info->op == PresOp::DotSwiz ? (inputs == 6 || inputs == 8) : inputs == info->inputs;
        if (!countOk) {
            fail(std::format("FXLC: {} with {} inputs", presOpName(info->op), inputs));
        }
        if (ins.components < 1 || ins.components > 4) {
            fail(std::format("FXLC: {} components", ins.components));
        }
        ins.inputs.resize(inputs);
        for (auto& in : ins.inputs) {
            in = readPresOperand(r);
        }
        ins.output = readPresOperand(r);
        if (ins.output.relative) {
            fail("FXLC: relative addressing in an output");
        }
        const PresTable ot = ins.output.reg.table;
        if (ot != PresTable::Output && ot != PresTable::Temp && ot != PresTable::OutBool && ot != PresTable::OutInt) {
            fail(std::format("FXLC: output to table {}", presTableName(ot)));
        }
    }

    // register file sizes
    auto grow = [&p](const PresRegister& reg, std::uint32_t lastComponent) {
        const std::uint32_t n = (reg.offset + lastComponent) / componentsPerRegister(reg.table) + 1;
        switch (reg.table) {
            case PresTable::Input: p.inputRegisters = std::max(p.inputRegisters, n); break;
            case PresTable::Output: p.outputRegisters = std::max(p.outputRegisters, n); break;
            case PresTable::OutBool: p.outBoolRegisters = std::max(p.outBoolRegisters, n); break;
            case PresTable::OutInt: p.outIntRegisters = std::max(p.outIntRegisters, n); break;
            case PresTable::Temp: p.tempRegisters = std::max(p.tempRegisters, n); break;
            default: break;
        }
    };
    for (const Constant& c : p.inputs.constants) {
        if (c.registerSet == RegisterSet::Float4 && c.registerCount != 0) {
            p.inputRegisters = std::max(p.inputRegisters, c.registerIndex + c.registerCount);
        }
    }
    for (const PresInstruction& ins : p.code) {
        const bool all = findOp(static_cast<std::uint32_t>(ins.op))->allComponents;
        for (std::size_t k = 0; k < ins.inputs.size(); ++k) {
            const PresOperand& in = ins.inputs[k];
            const std::uint32_t last = (ins.scalar && k == 0) ? 0 : ins.components - 1;
            if (in.relative) {
                grow(in.index, 0);
            } else {
                if (in.reg.table == PresTable::Literal && in.reg.offset + last >= p.literals.size()) {
                    fail(std::format("FXLC: literal {} out of range ({} literals)", in.reg.offset + last,
                                     p.literals.size()));
                }
                grow(in.reg, last);
            }
        }
        grow(ins.output.reg, all ? 0 : ins.components - 1);
    }
    return p;
}

}  // namespace

PreshaderResult parsePreshader(std::span<const std::uint32_t> tokens) {
    PreshaderResult r;
    try {
        r.preshader = parsePres(tokens);
    } catch (const ParseError& e) {
        r.error = e.message;
        r.preshader = {};
    }
    return r;
}

namespace {

class PresVm {
public:
    PresVm(const Preshader& p, PresRegisters& r) : p_(p), r_(r) {
        auto atLeast = [](auto& v, std::size_t n) {
            if (v.size() < n) {
                v.resize(n);
            }
        };
        atLeast(r_.input, std::size_t(p.inputRegisters) * 4);
        atLeast(r_.output, std::size_t(p.outputRegisters) * 4);
        atLeast(r_.outInt, std::size_t(p.outIntRegisters) * 4);
        atLeast(r_.outBool, std::size_t(p.outBoolRegisters));
        temp_.assign(std::size_t(p.tempRegisters) * 4, 0.0f);
    }

    void run() {
        double args[8];
        for (const PresInstruction& ins : p_.code) {
            const auto n = static_cast<std::uint32_t>(ins.inputs.size());
            if (ins.op == PresOp::Dot) {
                // one result from all components
                for (std::uint32_t k = 0; k < 2; ++k) {
                    for (std::uint32_t j = 0; j < ins.components; ++j) {
                        args[k * ins.components + j] = get(ins.inputs[k], ins.scalar && k == 0 ? 0 : j);
                    }
                }
                double sum = 0.0;
                for (std::uint32_t j = 0; j < ins.components; ++j) {
                    sum += args[j] * args[ins.components + j];
                }
                set(ins.output.reg, 0, sum);
                continue;
            }
            for (std::uint32_t j = 0; j < ins.components; ++j) {
                for (std::uint32_t k = 0; k < n; ++k) {
                    args[k] = get(ins.inputs[k], ins.scalar && k == 0 ? 0 : j);
                }
                set(ins.output.reg, j, compute(ins.op, args, n));
            }
        }
    }

private:
    static double signedNan(double v) {
        return std::isnan(v) ? -std::numeric_limits<double>::quiet_NaN() : v;
    }

    // Semantics of native d3dx9 as established by Wine's tests (preshader.c).
    static double compute(PresOp op, const double* a, std::uint32_t n) {
        switch (op) {
            case PresOp::Nop: return 0.0;
            case PresOp::Mov: return a[0];
            case PresOp::Neg: return -a[0];
            case PresOp::Rcp: return 1.0 / a[0];
            case PresOp::Frc: return a[0] - std::floor(a[0]);
            case PresOp::Exp: return std::exp2(a[0]);
            case PresOp::Log: {
                const double v = std::fabs(a[0]);
                return v == 0.0 ? 0.0 : std::log2(v);
            }
            case PresOp::Rsq: {
                const double v = std::fabs(a[0]);
                return v == 0.0 ? std::numeric_limits<double>::infinity() : 1.0 / std::sqrt(v);
            }
            case PresOp::Sin: return std::sin(a[0]);
            case PresOp::Cos: return std::cos(a[0]);
            case PresOp::Asin: return signedNan(std::asin(a[0]));
            case PresOp::Acos: return signedNan(std::acos(a[0]));
            case PresOp::Atan: return std::atan(a[0]);
            case PresOp::Sqrt: return std::sqrt(a[0]);
            case PresOp::Floor: return std::floor(a[0]);
            case PresOp::Ceil: return std::ceil(a[0]);
            case PresOp::Min: return std::fmin(a[0], a[1]);
            case PresOp::Max: return std::fmax(a[0], a[1]);
            case PresOp::Lt: return a[0] < a[1] ? 1.0 : 0.0;
            case PresOp::Ge: return a[0] >= a[1] ? 1.0 : 0.0;
            case PresOp::Add: return a[0] + a[1];
            case PresOp::Mul: return a[0] * a[1];
            case PresOp::Atan2: return std::atan2(a[0], a[1]);
            case PresOp::Div: return 0.0;  // native d3dx9 returns 0 (the compiler emits rcp + mul instead)
            case PresOp::Cmp: return a[0] >= 0.0 ? a[1] : a[2];
            case PresOp::Movc: return a[0] != 0.0 ? a[1] : a[2];
            case PresOp::DotSwiz: {
                const std::uint32_t h = n / 2;
                double s = 0.0;
                for (std::uint32_t i = 0; i < h; ++i) {
                    s += a[i] * a[i + h];
                }
                return s;
            }
            default: return 0.0;  // integer ops are rejected by the parser
        }
    }

    std::uint32_t tableRegisters(PresTable t) const {
        switch (t) {
            case PresTable::Literal: return static_cast<std::uint32_t>(p_.literals.size() / 4);
            case PresTable::Input: return static_cast<std::uint32_t>(r_.input.size() / 4);
            case PresTable::VInput: return static_cast<std::uint32_t>(r_.vinput.size() / 4);
            case PresTable::Output: return static_cast<std::uint32_t>(r_.output.size() / 4);
            case PresTable::OutInt: return static_cast<std::uint32_t>(r_.outInt.size() / 4);
            case PresTable::OutBool: return static_cast<std::uint32_t>(r_.outBool.size());
            case PresTable::Temp: return static_cast<std::uint32_t>(temp_.size() / 4);
            default: return 0;
        }
    }

    double load(PresTable t, std::uint32_t off) const {
        auto at = [off](const auto& v) -> double { return off < v.size() ? static_cast<double>(v[off]) : 0.0; };
        switch (t) {
            case PresTable::Literal: return at(p_.literals);
            case PresTable::Input: return at(r_.input);
            case PresTable::VInput: return at(r_.vinput);
            case PresTable::Output: return at(r_.output);
            case PresTable::OutInt: return at(r_.outInt);
            case PresTable::OutBool: return at(r_.outBool);
            case PresTable::Temp: return at(temp_);
            default: return 0.0;
        }
    }

    double get(const PresOperand& o, std::uint32_t comp) const {
        const PresTable t = o.reg.table;
        const std::uint32_t cpr = componentsPerRegister(t);
        std::uint32_t base = 0;
        if (o.relative) {
            base = static_cast<std::uint32_t>(std::lrint(load(o.index.table, o.index.offset)));
        }
        std::uint32_t off = base * cpr + o.reg.offset + comp;
        std::uint32_t reg = off / cpr;
        const std::uint32_t size = tableRegisters(t);
        if (reg >= size) {
            // out of range (relative addressing): native wraps; c# wraps at the next power of two
            const std::uint32_t wrap = t == PresTable::Input ? std::bit_ceil(std::max(size, 1u)) : size;
            if (wrap == 0) {
                return 0.0;
            }
            reg %= wrap;
            if (reg >= size) {
                return 0.0;
            }
            off = reg * cpr + off % cpr;
        }
        return load(t, off);
    }

    void set(const PresRegister& reg, std::uint32_t comp, double v) {
        const std::uint32_t off = reg.offset + comp;
        switch (reg.table) {
            case PresTable::Output:
                if (off < r_.output.size()) {
                    r_.output[off] = static_cast<float>(v);
                }
                break;
            case PresTable::Temp:
                if (off < temp_.size()) {
                    temp_[off] = static_cast<float>(v);
                }
                break;
            case PresTable::OutInt:
                if (off < r_.outInt.size()) {
                    r_.outInt[off] = static_cast<std::int32_t>(std::lrint(v));
                }
                break;
            case PresTable::OutBool:
                if (off < r_.outBool.size()) {
                    r_.outBool[off] = v != 0.0 ? 1u : 0u;
                }
                break;
            default: break;
        }
    }

    const Preshader& p_;
    PresRegisters& r_;
    std::vector<float> temp_;
};

std::string formatDouble(double v) {
    std::string s = std::format("{}", v);
    return s;
}

std::string formatOperand(const Preshader& p, const PresOperand& o, std::uint32_t comps) {
    static constexpr char xyzw[] = "xyzw";
    const PresTable t = o.reg.table;
    if (t == PresTable::Literal && !o.relative) {
        std::string s = "(";
        for (std::uint32_t i = 0; i < comps; ++i) {
            const std::uint32_t idx = o.reg.offset + i;
            s += (i ? ", " : "") + (idx < p.literals.size() ? formatDouble(p.literals[idx]) : std::string("?"));
        }
        return s + ")";
    }
    const std::uint32_t cpr = componentsPerRegister(t);
    const std::uint32_t reg = o.reg.offset / cpr;
    std::string s = std::format("{}{}", presTableName(t), reg);
    if (o.relative) {
        s = std::format("{}[{}{}.{}]", presTableName(t), presTableName(o.index.table), o.index.offset / 4,
                        xyzw[o.index.offset % 4]);
        s = std::format("{}{}", s, reg != 0 ? std::format(" + {}", reg) : std::string{});
    }
    if (cpr == 4) {
        s += '.';
        for (std::uint32_t i = 0; i < comps; ++i) {
            s += xyzw[(o.reg.offset + i) % 4];
        }
    }
    if (t == PresTable::Input) {
        for (const Constant& c : p.inputs.constants) {
            if (c.registerSet == RegisterSet::Float4 && reg >= c.registerIndex &&
                reg < c.registerIndex + c.registerCount) {
                s += std::format(" /*{}{}*/", c.name,
                                 reg != c.registerIndex ? std::format("+{}", reg - c.registerIndex) : std::string{});
                break;
            }
        }
    }
    return s;
}

}  // namespace

void runPreshader(const Preshader& p, PresRegisters& regs) { PresVm(p, regs).run(); }

std::string disassemblePreshader(const Preshader& p) {
    std::string s;
    if (p.version == 0x54580100u) {
        s += "tx_1_0\n";
    } else {
        s += std::format("preshader version 0x{:08x}\n", p.version);
    }
    for (std::size_t i = 0; i < p.literals.size(); i += 4) {
        s += std::format("  imm{} = (", i / 4);
        for (std::size_t j = i; j < std::min(i + 4, p.literals.size()); ++j) {
            s += (j > i ? ", " : "") + formatDouble(p.literals[j]);
        }
        s += ")\n";
    }
    for (const Constant& c : p.inputs.constants) {
        s += std::format("  input {} c{}..c{} ({} {}x{})\n", c.name, c.registerIndex,
                         c.registerIndex + std::max(c.registerCount, 1u) - 1, className(c.cls), c.rows, c.columns);
    }
    for (const PresInstruction& ins : p.code) {
        const bool all = ins.op == PresOp::Dot;
        s += std::format("  {:<6} {}", presOpName(ins.op), formatOperand(p, ins.output, all ? 1 : ins.components));
        for (std::size_t k = 0; k < ins.inputs.size(); ++k) {
            s += ", " + formatOperand(p, ins.inputs[k], ins.scalar && k == 0 ? 1 : ins.components);
        }
        s += '\n';
    }
    if (!p.prsi.empty()) {
        s += "  PRSI:";
        for (std::uint32_t v : p.prsi) {
            s += std::format(" {}", v);
        }
        s += '\n';
    }
    return s;
}

// ==================================================================================================================
// effect
// ==================================================================================================================

std::uint32_t Parameter::leafCount() const { return rows * columns; }

std::uint32_t Parameter::dwordAt(std::size_t i) const {
    if ((i + 1) * 4 > data.size()) {
        return 0;
    }
    return loadU32(data.data() + i * 4);
}

float Parameter::floatAt(std::size_t i) const {
    const std::uint32_t d = dwordAt(i);
    switch (type) {
        case ParamType::Int: return static_cast<float>(static_cast<std::int32_t>(d));
        case ParamType::Bool: return d != 0 ? 1.0f : 0.0f;
        default: return std::bit_cast<float>(d);
    }
}

std::string_view Object::string() const {
    std::string_view s(reinterpret_cast<const char*>(data.data()), data.size());
    while (!s.empty() && s.back() == '\0') {
        s.remove_suffix(1);
    }
    return s;
}

std::uint32_t Effect::parameterIndex(std::string_view n) const {
    for (std::size_t i = 0; i < parameters.size(); ++i) {
        if (parameters[i].name == n) {
            return static_cast<std::uint32_t>(i);
        }
    }
    return kNone;
}

const Parameter* Effect::findParameter(std::string_view fullName) const {
    std::size_t end = fullName.find_first_of(".[@");
    const std::uint32_t top = parameterIndex(fullName.substr(0, end));
    if (top == kNone) {
        return nullptr;
    }
    const Parameter* p = &parameters[top];
    bool isTop = true;
    while (end < fullName.size()) {
        const char sep = fullName[end];
        if (sep == '[') {
            const std::size_t close = fullName.find(']', end);
            if (close == std::string_view::npos || !p->isArray()) {
                return nullptr;
            }
            std::uint32_t idx = 0;
            for (std::size_t i = end + 1; i < close; ++i) {
                if (fullName[i] < '0' || fullName[i] > '9') {
                    return nullptr;
                }
                idx = idx * 10 + static_cast<std::uint32_t>(fullName[i] - '0');
            }
            if (idx >= p->members.size()) {
                return nullptr;
            }
            p = &p->members[idx];
            end = close + 1;
        } else {
            const std::size_t next = fullName.find_first_of(".[@", end + 1);
            const std::string_view part = fullName.substr(end + 1, next - end - 1);
            const std::vector<Parameter>& list = sep == '@' ? p->annotations : p->members;
            if ((sep == '.' && (p->isArray() || p->cls != ParamClass::Struct)) || (sep == '@' && !isTop)) {
                return nullptr;
            }
            const Parameter* found = nullptr;
            for (const Parameter& m : list) {
                if (m.name == part) {
                    found = &m;
                    break;
                }
            }
            if (found == nullptr) {
                return nullptr;
            }
            p = found;
            end = next;
        }
        isTop = false;
    }
    return p;
}

const Technique* Effect::findTechnique(std::string_view n) const {
    for (const Technique& t : techniques) {
        if (t.name == n) {
            return &t;
        }
    }
    return nullptr;
}

bool isEffectBinary(std::span<const std::byte> data) {
    return data.size() >= 8 && loadU32(data.data()) == kEffectMagic;
}

namespace {

class EffectParser {
public:
    EffectParser(std::span<const std::byte> body, Effect& e) : d_(body, "effect"), e_(e) {}

    void parse(std::uint32_t start) {
        std::size_t pos = start;
        const std::uint32_t paramCount = d_.read(pos);
        const std::uint32_t techCount = d_.read(pos);
        e_.shaderCount = d_.read(pos);
        const std::uint32_t objectCount = d_.read(pos);
        if (paramCount > 65536 || techCount > 65536 || objectCount > 1u << 20) {
            fail("implausible header counts");
        }
        e_.objects.resize(objectCount);
        e_.parameters.resize(paramCount);
        for (Parameter& p : e_.parameters) {
            parseParameter(pos, p);
        }
        e_.techniques.resize(techCount);
        for (Technique& t : e_.techniques) {
            parseTechnique(pos, t);
        }
        const std::uint32_t blobCount = d_.read(pos);
        const std::uint32_t resourceCount = d_.read(pos);
        for (std::uint32_t i = 0; i < blobCount; ++i) {
            const std::uint32_t id = d_.read(pos);
            if (id >= e_.objects.size()) {
                fail(std::format("object data for id {} (only {} objects)", id, e_.objects.size()));
            }
            const auto blob = readBlob(pos);
            e_.objects[id].data.assign(blob.begin(), blob.end());
        }
        for (std::uint32_t i = 0; i < resourceCount; ++i) {
            parseResource(pos);
        }
        createShaders();
        linkStates();
    }

private:
    std::span<const std::byte> readBlob(std::size_t& pos) {
        const std::uint32_t size = d_.read(pos);
        const auto blob = d_.span(pos, size);
        pos += (std::size_t(size) + 3) & ~std::size_t(3);
        return blob;
    }

    std::string nameAt(std::uint32_t off) {
        const std::uint32_t len = d_.u32(off);
        if (len == 0) {
            return {};
        }
        return stripNul(d_.span(std::size_t(off) + 4, len));
    }

    void parseTypedef(std::size_t& pos, Parameter& p, const Parameter* parent, std::uint32_t parentMembers, int depth) {
        if (depth > 32) {
            fail("type nesting too deep");
        }
        std::uint32_t memberCount = 0;
        if (parent == nullptr) {
            const std::uint32_t type = d_.read(pos);
            const std::uint32_t cls = d_.read(pos);
            if (type > static_cast<std::uint32_t>(ParamType::Unsupported) ||
                cls > static_cast<std::uint32_t>(ParamClass::Struct)) {
                fail(std::format("invalid type {} / class {} at 0x{:x}", type, cls, pos - 8));
            }
            p.type = static_cast<ParamType>(type);
            p.cls = static_cast<ParamClass>(cls);
            p.name = nameAt(d_.read(pos));
            p.semantic = nameAt(d_.read(pos));
            p.elements = d_.read(pos);
            switch (p.cls) {
                case ParamClass::Vector:
                    p.columns = d_.read(pos);
                    p.rows = d_.read(pos);
                    break;
                case ParamClass::Scalar:
                case ParamClass::MatrixRows:
                case ParamClass::MatrixColumns:
                    p.rows = d_.read(pos);
                    p.columns = d_.read(pos);
                    break;
                case ParamClass::Struct: memberCount = d_.read(pos); break;
                case ParamClass::Object: break;
            }
            if (p.rows > 64 || p.columns > 64 || p.elements > 65536 || memberCount > 65536) {
                fail(std::format("implausible type of '{}'", p.name));
            }
        } else {
            p.type = parent->type;
            p.cls = parent->cls;
            p.name = parent->name;
            p.semantic = parent->semantic;
            p.rows = parent->rows;
            p.columns = parent->columns;
            p.flags = parent->flags;
            memberCount = parentMembers;
        }
        if (p.elements != 0) {
            const std::size_t save = pos;
            p.members.resize(p.elements);
            for (Parameter& m : p.members) {
                pos = save;
                m.flags = p.flags;
                parseTypedef(pos, m, &p, memberCount, depth + 1);
            }
        } else if (memberCount != 0) {
            p.members.resize(memberCount);
            for (Parameter& m : p.members) {
                m.flags = p.flags;
                parseTypedef(pos, m, nullptr, 0, depth + 1);
            }
        }
    }

    void parseValue(std::size_t& pos, Parameter& p, std::vector<std::byte>& root, int depth) {
        p.dataOffset = static_cast<std::uint32_t>(root.size());
        if (p.elements != 0 || p.cls == ParamClass::Struct) {
            for (Parameter& m : p.members) {
                parseValue(pos, m, root, depth + 1);
            }
        } else {
            switch (p.cls) {
                case ParamClass::Scalar:
                case ParamClass::Vector:
                case ParamClass::MatrixRows:
                case ParamClass::MatrixColumns: {
                    const std::size_t n = std::size_t(p.rows) * p.columns * 4;
                    const auto b = d_.span(pos, n);
                    root.insert(root.end(), b.begin(), b.end());
                    pos += n;
                    break;
                }
                case ParamClass::Object:
                    if (isSampler(p.type)) {
                        const std::uint32_t n = d_.read(pos);
                        if (n > 1024) {
                            fail(std::format("sampler '{}' with {} states", p.name, n));
                        }
                        p.samplerStates.resize(n);
                        for (State& s : p.samplerStates) {
                            parseState(pos, s, depth + 1);
                        }
                    } else if (p.type == ParamType::String || isTexture(p.type) || isShader(p.type)) {
                        p.objectId = d_.read(pos);
                        if (p.objectId >= e_.objects.size()) {
                            fail(std::format("'{}': object id {} out of range", p.name, p.objectId));
                        }
                        e_.objects[p.objectId].type = p.type;
                    }
                    break;
                case ParamClass::Struct: break;
            }
        }
        p.dataSize = static_cast<std::uint32_t>(root.size()) - p.dataOffset;
    }

    void parseRoot(std::uint32_t typeOffset, std::uint32_t valueOffset, Parameter& p, std::uint32_t flags, int depth) {
        std::size_t tp = typeOffset;
        p.flags = flags;
        parseTypedef(tp, p, nullptr, 0, depth);
        std::size_t vp = valueOffset;
        parseValue(vp, p, p.data, depth);
    }

    void parseState(std::size_t& pos, State& s, int depth) {
        if (depth > 32) {
            fail("state nesting too deep");
        }
        s.id = d_.read(pos);
        if (stateInfo(s.id) == nullptr) {
            fail(std::format("unknown state id 0x{:x}", s.id));
        }
        s.index = d_.read(pos);
        const std::uint32_t typeOffset = d_.read(pos);
        const std::uint32_t valueOffset = d_.read(pos);
        parseRoot(typeOffset, valueOffset, s.value, 0, depth + 1);
    }

    void parseAnnotations(std::size_t& pos, std::vector<Parameter>& list, std::uint32_t count) {
        if (count > 4096) {
            fail("implausible annotation count");
        }
        list.resize(count);
        for (Parameter& a : list) {
            const std::uint32_t typeOffset = d_.read(pos);
            const std::uint32_t valueOffset = d_.read(pos);
            parseRoot(typeOffset, valueOffset, a, kParamAnnotation, 0);
        }
    }

    void parseParameter(std::size_t& pos, Parameter& p) {
        const std::uint32_t typeOffset = d_.read(pos);
        const std::uint32_t valueOffset = d_.read(pos);
        const std::uint32_t flags = d_.read(pos);
        const std::uint32_t annotations = d_.read(pos);
        parseRoot(typeOffset, valueOffset, p, flags, 0);
        parseAnnotations(pos, p.annotations, annotations);
    }

    void parseTechnique(std::size_t& pos, Technique& t) {
        t.name = nameAt(d_.read(pos));
        const std::uint32_t annotations = d_.read(pos);
        const std::uint32_t passes = d_.read(pos);
        if (passes > 4096) {
            fail("implausible pass count");
        }
        parseAnnotations(pos, t.annotations, annotations);
        t.passes.resize(passes);
        for (Pass& p : t.passes) {
            p.name = nameAt(d_.read(pos));
            const std::uint32_t passAnnotations = d_.read(pos);
            const std::uint32_t states = d_.read(pos);
            if (states > 4096) {
                fail("implausible state count");
            }
            parseAnnotations(pos, p.annotations, passAnnotations);
            p.states.resize(states);
            for (State& s : p.states) {
                parseState(pos, s, 0);
            }
        }
    }

    std::uint32_t addPreshader(std::span<const std::byte> blob, const std::string& what) {
        const auto tokens = toTokens(blob);
        PreshaderResult r = parsePreshader(tokens);
        if (!r.ok()) {
            fail(std::format("{}: {}", what, r.error));
        }
        e_.preshaders.push_back(std::move(r.preshader));
        return static_cast<std::uint32_t>(e_.preshaders.size() - 1);
    }

    // "Complex" state values: shaders of passes, expressions, parameter references, array selectors.
    void parseResource(std::size_t& pos) {
        const std::uint32_t tech = d_.read(pos);
        const std::uint32_t index = d_.read(pos);
        const std::uint32_t element = d_.read(pos);
        const std::uint32_t stateIndex = d_.read(pos);
        const std::uint32_t usage = d_.read(pos);
        State* state = nullptr;
        std::string where;
        if (tech == kNone) {
            if (index >= e_.parameters.size()) {
                fail(std::format("resource for parameter {} out of range", index));
            }
            Parameter* p = &e_.parameters[index];
            where = p->name;
            if (element != kNone && p->isArray()) {
                if (element >= p->members.size()) {
                    fail(std::format("resource for {}[{}] out of range", p->name, element));
                }
                p = &p->members[element];
                where += std::format("[{}]", element);
            }
            if (stateIndex >= p->samplerStates.size()) {
                fail(std::format("resource for {} state {} out of range", where, stateIndex));
            }
            state = &p->samplerStates[stateIndex];
        } else {
            if (tech >= e_.techniques.size() || index >= e_.techniques[tech].passes.size() ||
                stateIndex >= e_.techniques[tech].passes[index].states.size()) {
                fail(std::format("resource for technique {} pass {} state {} out of range", tech, index, stateIndex));
            }
            state = &e_.techniques[tech].passes[index].states[stateIndex];
            where = std::format("{}/{}", e_.techniques[tech].name, e_.techniques[tech].passes[index].name);
        }
        where += std::format(" {}", state->info()->name);
        const auto blob = readBlob(pos);
        switch (usage) {
            case 0:
                if (isShader(state->value.type)) {
                    if (state->value.objectId == kNone) {
                        fail(where + ": shader resource without an object");
                    }
                    Object& o = e_.objects[state->value.objectId];
                    o.data.assign(blob.begin(), blob.end());
                    o.type = state->value.type;
                } else {
                    state->kind = StateKind::Expression;
                    state->expression = addPreshader(blob, where);
                }
                break;
            case 1:
                state->kind = StateKind::ParamRef;
                state->reference = stripNul(blob);
                break;
            case 2: {
                if (blob.size() < 4) {
                    fail(where + ": array selector too short");
                }
                const std::uint32_t nameSize = loadU32(blob.data());
                if (nameSize > blob.size() - 4) {
                    fail(where + ": array selector name exceeds the blob");
                }
                state->kind = StateKind::ArraySelector;
                state->reference = stripNul(blob.subspan(4, nameSize));
                state->expression = addPreshader(blob.subspan(4 + nameSize), where);
                break;
            }
            default: fail(std::format("{}: unknown resource usage {}", where, usage));
        }
    }

    void createShaders() {
        for (std::size_t id = 0; id < e_.objects.size(); ++id) {
            Object& o = e_.objects[id];
            if (!isShader(o.type) || o.data.empty()) {
                continue;
            }
            Shader s;
            s.objectId = static_cast<std::uint32_t>(id);
            s.tokens = toTokens(o.data);
            const std::string what = std::format("shader object {}", id);
            const auto version = s.tokens.empty() ? std::nullopt : shader::parseVersion(s.tokens[0]);
            if (!version) {
                fail(what + ": no SM1-3 version token");
            }
            s.stage = version->stage;
            s.major = version->major;
            s.minor = version->minor;
            if ((s.stage == shader::Stage::Vertex) != (o.type == ParamType::VertexShader)) {
                fail(what + ": stage does not match the parameter type");
            }
            const std::size_t len = shader::byteLength(s.tokens.data(), s.tokens.size());
            if (len == 0) {
                fail(what + ": no END token");
            }
            s.tokens.resize(len / 4);
            ConstantTableResult ct = parseConstantTable(s.tokens);
            if (!ct.ok()) {
                fail(what + ": " + ct.error);
            }
            s.constants = std::move(ct.table);
            if (const auto pres = findComment(s.tokens, fourcc('P', 'R', 'E', 'S')); !pres.empty()) {
                s.preshader = addPreshader(std::as_bytes(pres), what + " preshader");
            }
            o.shader = static_cast<std::uint32_t>(e_.shaders.size());
            e_.shaders.push_back(std::move(s));
        }
    }

    void linkState(State& s) {
        if (s.kind == StateKind::Constant && isShader(s.value.type) && s.value.objectId != kNone) {
            s.shader = e_.objects[s.value.objectId].shader;
        }
        if (s.kind == StateKind::ParamRef || s.kind == StateKind::ArraySelector) {
            if (e_.findParameter(s.reference) == nullptr) {
                fail(std::format("state {} references unknown parameter '{}'", s.info()->name, s.reference));
            }
            s.referenceParam = e_.parameterIndex(s.reference.substr(0, s.reference.find_first_of(".[@")));
        }
    }

    void linkParameter(Parameter& p) {
        for (State& s : p.samplerStates) {
            linkState(s);
        }
        for (Parameter& m : p.members) {
            linkParameter(m);
        }
    }

    void linkStates() {
        for (Parameter& p : e_.parameters) {
            linkParameter(p);
        }
        for (Technique& t : e_.techniques) {
            for (Pass& p : t.passes) {
                for (State& s : p.states) {
                    linkState(s);
                }
            }
        }
    }

    Bytes d_;
    Effect& e_;
};

}  // namespace

EffectResult parseEffect(std::span<const std::byte> data) {
    EffectResult r;
    if (!isEffectBinary(data)) {
        r.error = data.size() >= 4 && std::isprint(static_cast<unsigned char>(data[0]))
                      ? "not an fx_2_0 binary (looks like HLSL source text: compile it first)"
                      : "not an fx_2_0 binary (magic 0xFEFF0901 missing)";
        return r;
    }
    try {
        const std::uint32_t start = loadU32(data.data() + 4);
        EffectParser(data.subspan(8), r.effect).parse(start);
    } catch (const ParseError& e) {
        r.error = e.message;
        r.effect = {};
    }
    return r;
}

// ==================================================================================================================
// runtime helpers
// ==================================================================================================================

namespace {

std::uint32_t dwordIn(std::span<const std::byte> data, std::size_t byteOffset) {
    if (byteOffset + 4 > data.size()) {
        return 0;
    }
    return loadU32(data.data() + byteOffset);
}

float toFloat(ParamType from, std::uint32_t d) {
    switch (from) {
        case ParamType::Int: return static_cast<float>(static_cast<std::int32_t>(d));
        case ParamType::Bool: return d != 0 ? 1.0f : 0.0f;
        default: return std::bit_cast<float>(d);
    }
}

std::int32_t toInt(ParamType from, std::uint32_t d) {
    switch (from) {
        case ParamType::Float: return static_cast<std::int32_t>(std::bit_cast<float>(d));
        case ParamType::Bool: return d != 0 ? 1 : 0;
        default: return static_cast<std::int32_t>(d);
    }
}

}  // namespace

ParameterValues ParameterValues::defaults(const Effect& e) {
    ParameterValues v;
    v.data.reserve(e.parameters.size());
    for (const Parameter& p : e.parameters) {
        v.data.push_back(p.data);
    }
    return v;
}

void ParameterValues::setFloats(const Effect& e, std::string_view name, std::span<const float> v) {
    const std::uint32_t idx = e.parameterIndex(name);
    if (idx == kNone || idx >= data.size()) {
        return;
    }
    const Parameter& p = e.parameters[idx];
    std::vector<std::byte>& d = data[idx];
    for (std::size_t i = 0; i < v.size() && (i + 1) * 4 <= d.size(); ++i) {
        std::uint32_t out = 0;
        switch (p.type) {
            case ParamType::Int: out = static_cast<std::uint32_t>(static_cast<std::int32_t>(v[i])); break;
            case ParamType::Bool: out = v[i] != 0.0f ? 1u : 0u; break;
            default: out = std::bit_cast<std::uint32_t>(v[i]); break;
        }
        std::memcpy(d.data() + i * 4, &out, 4);
    }
}

bool setConstant(const Constant& c, const Parameter& param, std::span<const std::byte> value, ConstantRegisters& regs) {
    if (c.cls == ParamClass::Object || c.registerSet == RegisterSet::Sampler) {
        return true;  // samplers are bound by the runtime, not through registers
    }
    if (!c.members.empty()) {
        if (c.elements > 1) {
            if (!param.isArray()) {
                return false;
            }
        } else if (param.isArray() || param.cls != ParamClass::Struct) {
            return false;
        }
        bool ok = param.members.size() == c.members.size();
        const std::size_t n = std::min(param.members.size(), c.members.size());
        for (std::size_t i = 0; i < n; ++i) {
            ok = setConstant(c.members[i], param.members[i], value, regs) && ok;
        }
        return ok;
    }
    if (param.isArray()) {
        return param.members.size() == 1 && setConstant(c, param.members[0], value, regs);
    }
    if (!isNumeric(param.type) || param.cls == ParamClass::Struct) {
        return false;
    }
    const std::uint32_t n = param.rows * param.columns;
    auto src = [&](std::uint32_t k) { return dwordIn(value, std::size_t(param.dataOffset) + std::size_t(k) * 4); };

    if (c.registerSet == RegisterSet::Bool) {
        const std::uint32_t count = std::min(c.registerCount, n);
        if (regs.b.size() < c.registerIndex + count) {
            regs.b.resize(c.registerIndex + count);
        }
        for (std::uint32_t k = 0; k < count; ++k) {
            regs.b[c.registerIndex + k] = src(k) != 0 ? 1u : 0u;
        }
        return true;
    }

    const bool isFloat = c.registerSet == RegisterSet::Float4;
    auto store = [&](std::uint32_t reg, std::uint32_t comp, std::uint32_t d) {
        const std::size_t at = std::size_t(reg) * 4 + comp;
        if (isFloat) {
            if (regs.f.size() <= at) {
                regs.f.resize((std::size_t(reg) + 1) * 4);
            }
            regs.f[at] = toFloat(param.type, d);
        } else {
            if (regs.i.size() <= at) {
                regs.i.resize((std::size_t(reg) + 1) * 4);
            }
            regs.i[at] = toInt(param.type, d);
        }
    };
    if (c.registerCount == 0) {
        return true;
    }
    if (c.cls == ParamClass::Scalar || c.cls == ParamClass::Vector) {
        const std::uint32_t count = std::min(std::max(param.rows, param.columns), 4u);
        for (std::uint32_t k = 0; k < count; ++k) {
            store(c.registerIndex, k, src(k));
        }
        return true;
    }
    if (c.cls != ParamClass::MatrixRows && c.cls != ParamClass::MatrixColumns) {
        return false;
    }
    const bool transpose = (c.cls == ParamClass::MatrixColumns && param.cls == ParamClass::MatrixRows) ||
                           (c.cls == ParamClass::MatrixRows && param.cls == ParamClass::MatrixColumns);
    const std::uint32_t major = c.cls == ParamClass::MatrixColumns ? param.columns : param.rows;
    const std::uint32_t minor = c.cls == ParamClass::MatrixColumns ? param.rows : param.columns;
    const std::uint32_t registers = std::min(c.registerCount, major);
    for (std::uint32_t i = 0; i < registers; ++i) {
        for (std::uint32_t j = 0; j < std::min(minor, 4u); ++j) {
            store(c.registerIndex + i, j, src(transpose ? i + j * major : i * minor + j));
        }
    }
    return n != 0;
}

void fillConstants(const ConstantTable& table, const Effect& e, const ParameterValues& values, ConstantRegisters& regs) {
    for (const Constant& c : table.constants) {
        std::uint32_t idx = c.paramIndexCache;
        if (idx == Constant::kUnresolved) {
            idx = e.parameterIndex(c.name);
            c.paramIndexCache = idx;
        }
        if (idx == kNone || idx >= values.data.size()) {
            continue;
        }
        setConstant(c, e.parameters[idx], values.data[idx], regs);
    }
}

namespace {

void runWithInputs(const Effect& e, const Preshader& p, const ParameterValues& values, PresRegisters& pr) {
    static thread_local ConstantRegisters in;
    in.f.clear();
    in.i.clear();
    in.b.clear();
    fillConstants(p.inputs, e, values, in);
    pr.input.swap(in.f);
    runPreshader(p, pr);
}

}  // namespace

ConstantRegisters evaluateShaderConstants(const Effect& e, const Shader& s, const ParameterValues& values) {
    ConstantRegisters regs;
    evaluateShaderConstants(e, s, values, regs);
    return regs;
}

void evaluateShaderConstants(const Effect& e, const Shader& s, const ParameterValues& values, ConstantRegisters& regs) {
    regs.f.clear();
    regs.i.clear();
    regs.b.clear();
    fillConstants(s.constants, e, values, regs);
    if (s.preshader == kNone || s.preshader >= e.preshaders.size()) {
        return;
    }
    const Preshader& p = e.preshaders[s.preshader];
    static thread_local PresRegisters pr;
    pr.output.clear();
    pr.outInt.clear();
    pr.outBool.clear();
    pr.vinput.clear();
    runWithInputs(e, p, values, pr);
    // copy the registers the preshader writes (whole registers, like D3DX's upload)
    for (const PresInstruction& ins : p.code) {
        const PresRegister& o = ins.output.reg;
        const std::uint32_t last = o.offset + (ins.op == PresOp::Dot ? 0 : ins.components - 1);
        for (std::uint32_t off = o.offset; off <= last; ++off) {
            if (o.table == PresTable::Output) {
                const std::size_t r = off / 4;
                if (regs.f.size() < (r + 1) * 4) {
                    regs.f.resize((r + 1) * 4);
                }
                std::copy_n(pr.output.begin() + static_cast<std::ptrdiff_t>(r * 4), 4,
                            regs.f.begin() + static_cast<std::ptrdiff_t>(r * 4));
            } else if (o.table == PresTable::OutInt) {
                const std::size_t r = off / 4;
                if (regs.i.size() < (r + 1) * 4) {
                    regs.i.resize((r + 1) * 4);
                }
                std::copy_n(pr.outInt.begin() + static_cast<std::ptrdiff_t>(r * 4), 4,
                            regs.i.begin() + static_cast<std::ptrdiff_t>(r * 4));
            } else if (o.table == PresTable::OutBool) {
                if (regs.b.size() <= off) {
                    regs.b.resize(off + 1);
                }
                regs.b[off] = pr.outBool[off];
            }
        }
    }
}

std::vector<std::uint32_t> evaluateExpression(const Effect& e, const Preshader& p, const ParameterValues& values,
                                              ParamType resultType, std::uint32_t resultComponents) {
    PresRegisters pr;
    runWithInputs(e, p, values, pr);
    std::vector<std::uint32_t> out(resultComponents);
    for (std::uint32_t i = 0; i < resultComponents && i < pr.output.size(); ++i) {
        const float f = pr.output[i];
        switch (resultType) {
            case ParamType::Int: out[i] = static_cast<std::uint32_t>(static_cast<std::int32_t>(f)); break;
            case ParamType::Bool: out[i] = std::bit_cast<std::uint32_t>(f) != 0 ? 1u : 0u; break;
            default: out[i] = std::bit_cast<std::uint32_t>(f); break;
        }
    }
    return out;
}

// ==================================================================================================================
// dump
// ==================================================================================================================

namespace {

std::string typeString(const Parameter& p) {
    std::string s;
    if (p.cls == ParamClass::Struct) {
        s = "struct";
    } else if (p.cls == ParamClass::Object) {
        s = typeName(p.type);
    } else {
        s = typeName(p.type);
        if (p.cls == ParamClass::Vector) {
            s += std::to_string(p.columns);
        } else if (p.cls == ParamClass::MatrixRows || p.cls == ParamClass::MatrixColumns) {
            s += std::format("{}x{}", p.rows, p.columns);
            if (p.cls == ParamClass::MatrixColumns) {
                s = "column_major " + s;
            }
        }
    }
    return s;
}

std::string numberString(ParamType t, std::uint32_t d) {
    switch (t) {
        case ParamType::Int: return std::to_string(static_cast<std::int32_t>(d));
        case ParamType::Bool: return d != 0 ? "true" : "false";
        default: return std::format("{}", std::bit_cast<float>(d));
    }
}

class Dumper {
public:
    Dumper(const Effect& e, const DumpOptions& o) : e_(e), o_(o) {}

    std::string run() {
        out_ += std::format("fx_2_0 effect: {} parameters, {} techniques, {} objects, {} shaders, {} preshaders\n",
                            e_.parameters.size(), e_.techniques.size(), e_.objects.size(), e_.shaders.size(),
                            e_.preshaders.size());
        out_ += "\nparameters:\n";
        for (std::size_t i = 0; i < e_.parameters.size(); ++i) {
            const Parameter& p = e_.parameters[i];
            parameter(p, p.data, 1, true, std::format("[{}] ", i));
        }
        out_ += "\ntechniques:\n";
        for (const Technique& t : e_.techniques) {
            line(1, std::format("technique {}", t.name));
            annotations(t.annotations, 2);
            for (const Pass& p : t.passes) {
                line(2, std::format("pass {}", p.name));
                annotations(p.annotations, 3);
                for (const State& s : p.states) {
                    state(s, 3);
                }
            }
        }
        out_ += "\nshaders:\n";
        for (std::size_t i = 0; i < e_.shaders.size(); ++i) {
            const Shader& s = e_.shaders[i];
            line(1, std::format("shader #{}: {}s_{}_{} (object {}, {} tokens){}", i,
                                s.stage == shader::Stage::Vertex ? 'v' : 'p', s.major, s.minor, s.objectId,
                                s.tokens.size(),
                                s.preshader != kNone ? std::format(", preshader #{}", s.preshader) : std::string{}));
            if (s.constants.present) {
                indent(dumpConstantTable(s.constants), 2);
            }
            if (o_.shaderListings) {
                const auto d = shader::decode(s.tokens);
                indent(d.ok() ? shader::disassemble(d.program) : "decode error: " + d.error, 2);
            }
        }
        if (!e_.preshaders.empty()) {
            out_ += "\npreshaders:\n";
            for (std::size_t i = 0; i < e_.preshaders.size(); ++i) {
                const Preshader& p = e_.preshaders[i];
                line(1, std::format("preshader #{}: {} instructions, {} literals, inputs c0..c{}, outputs {} float4 "
                                    "{} int4 {} bool, {} temps",
                                    i, p.code.size(), p.literals.size(), int(p.inputRegisters) - 1, p.outputRegisters,
                                    p.outIntRegisters, p.outBoolRegisters, p.tempRegisters));
                if (o_.preshaderCode) {
                    indent(disassemblePreshader(p), 2);
                }
            }
        }
        return std::move(out_);
    }

private:
    void line(int depth, const std::string& s) {
        out_.append(std::size_t(depth) * 2, ' ');
        out_ += s;
        out_ += '\n';
    }

    void indent(const std::string& text, int depth) {
        std::size_t pos = 0;
        while (pos < text.size()) {
            std::size_t nl = text.find('\n', pos);
            if (nl == std::string::npos) {
                nl = text.size();
            }
            line(depth, text.substr(pos, nl - pos));
            pos = nl + 1;
        }
    }

    std::string valueString(const Parameter& p, std::span<const std::byte> root) {
        if (p.cls == ParamClass::Object) {
            if (p.objectId == kNone) {
                return {};
            }
            const Object& o = e_.objects[p.objectId];
            if (p.type == ParamType::String) {
                return std::format("\"{}\"", o.string());
            }
            if (isShader(p.type)) {
                return o.shader != kNone ? std::format("shader #{}", o.shader)
                                         : std::format("object {} (no data)", p.objectId);
            }
            return std::format("object {}", p.objectId);
        }
        if (p.dataSize == 0) {
            return {};
        }
        std::string s = "{";
        for (std::uint32_t k = 0; k < p.dataSize / 4; ++k) {
            s += (k ? ", " : "") + numberString(p.type, dwordIn(root, std::size_t(p.dataOffset) + std::size_t(k) * 4));
        }
        return s + "}";
    }

    void parameter(const Parameter& p, std::span<const std::byte> root, int depth, bool top,
                   const std::string& prefix = {}) {
        std::string head = prefix + typeString(p) + " " + p.name;
        if (p.isArray()) {
            head += std::format("[{}]", p.elements);
        }
        if (!p.semantic.empty()) {
            head += " : " + p.semantic;
        }
        if (top && (p.flags & ~kParamAnnotation) != 0) {
            head += std::format(" (flags {})", p.flags);
        }
        const bool compound = p.isArray() || p.cls == ParamClass::Struct;
        const bool sampler = !p.isArray() && isSampler(p.type);
        if (compound && (p.cls == ParamClass::Object || p.cls == ParamClass::Struct)) {
            line(depth, head);
            for (std::size_t i = 0; i < p.members.size(); ++i) {
                parameter(p.members[i], root, depth + 1, false);
            }
        } else if (sampler) {
            line(depth, head + " = sampler_state");
            for (const State& s : p.samplerStates) {
                state(s, depth + 1);
            }
        } else {
            const std::string v = valueString(p, root);
            line(depth, v.empty() ? head : head + " = " + v);
        }
        if (top) {
            annotations(p.annotations, depth + 1);
        }
    }

    void annotations(const std::vector<Parameter>& list, int depth) {
        for (const Parameter& a : list) {
            const std::string v = valueString(a, a.data);
            line(depth, std::format("@{} {}{}", typeString(a), a.name, v.empty() ? "" : " = " + v));
        }
    }

    void state(const State& s, int depth) {
        const StateInfo* info = s.info();
        std::string head = std::format("[0x{:02x}] {}", s.id, info->name);
        const bool indexed = info->cls == StateClass::SamplerState || info->cls == StateClass::TextureStage ||
                             info->cls == StateClass::Light || info->cls == StateClass::LightEnable ||
                             info->cls == StateClass::Transform || info->cls == StateClass::ShaderConst ||
                             info->cls == StateClass::SetSampler || info->cls == StateClass::Texture ||
                             s.index != 0;
        // assignments inside sampler_state { } carry index 256 (unused: the stage is the shader's sampler register)
        if (indexed && s.index != kSamplerStateIndex) {
            head += std::format("[{}]", s.index);
        }
        std::string v;
        switch (s.kind) {
            case StateKind::Constant: {
                v = valueString(s.value, s.value.data);
                if (s.value.cls != ParamClass::Object && s.value.dataSize == 4) {
                    const std::string sym = stateValueName(s.id, s.value.dwordAt(0));
                    v = numberString(s.value.type, s.value.dwordAt(0));
                    if (s.value.type != ParamType::Float && s.value.dwordAt(0) > 9) {
                        v += std::format(" (0x{:x})", s.value.dwordAt(0));
                    }
                    if (!sym.empty()) {
                        v += " " + sym;
                    }
                }
                break;
            }
            case StateKind::ParamRef: v = std::format("<{}>", s.reference); break;
            case StateKind::Expression: v = std::format("expression (preshader #{}) -> {}", s.expression, typeString(s.value)); break;
            case StateKind::ArraySelector:
                v = std::format("{}[preshader #{}]", s.reference, s.expression);
                break;
        }
        line(depth, head + " = " + v);
    }

    const Effect& e_;
    const DumpOptions& o_;
    std::string out_;
};

void dumpConstant(std::string& out, const Constant& c, int depth) {
    out.append(std::size_t(depth) * 2, ' ');
    const char prefix = c.registerSet == RegisterSet::Bool    ? 'b'
                        : c.registerSet == RegisterSet::Int4  ? 'i'
                        : c.registerSet == RegisterSet::Float4 ? 'c'
                                                               : 's';
    out += std::format("{}{} ({}) {} {} {} {}x{}", prefix, c.registerIndex, c.registerCount, c.name,
                       registerSetName(c.registerSet), typeName(c.type), c.rows, c.columns);
    if (c.cls != ParamClass::Scalar && c.cls != ParamClass::Vector) {
        out += std::format(" {}", className(c.cls));
    }
    if (c.elements > 1) {
        out += std::format(" [{}]", c.elements);
    }
    if (depth == 1 && !c.defaultValue.empty()) {
        out += " default {";
        for (std::size_t i = 0; i < c.defaultValue.size(); ++i) {
            const ParamType t = c.registerSet == RegisterSet::Float4 ? ParamType::Float
                                : c.registerSet == RegisterSet::Int4 ? ParamType::Int
                                                                     : ParamType::Bool;
            out += (i ? ", " : "") + numberString(t, c.defaultValue[i]);
        }
        out += "}";
    }
    out += '\n';
    for (const Constant& m : c.members) {
        dumpConstant(out, m, depth + 1);
    }
}

}  // namespace

std::string dumpConstantTable(const ConstantTable& t) {
    if (!t.present) {
        return "no constant table\n";
    }
    std::string out = std::format("CTAB {} \"{}\", {} constants\n", t.target, t.creator, t.constants.size());
    for (const Constant& c : t.constants) {
        dumpConstant(out, c, 1);
    }
    return out;
}

std::string dumpEffect(const Effect& e, const DumpOptions& options) { return Dumper(e, options).run(); }

}  // namespace rvk::fx
