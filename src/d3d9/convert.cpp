#include "d3d9/convert.h"

namespace rvk::d3d9 {

std::uint32_t defaultRenderState(std::uint32_t rs) {
    const std::uint32_t one = asDword(1.0f);
    switch (rs) {
    case RS_ZENABLE: return ZB_TRUE;  // a device with an auto depth stencil starts with z on
    case RS_FILLMODE: return FILL_SOLID;
    case RS_SHADEMODE: return 2;  // gouraud
    case RS_ZWRITEENABLE: return 1;
    case RS_LASTPIXEL: return 1;
    case RS_SRCBLEND: return BLEND_ONE;
    case RS_DESTBLEND: return BLEND_ZERO;
    case RS_CULLMODE: return CULL_CCW;
    case RS_ZFUNC: return CMP_LESSEQUAL;
    case RS_ALPHAFUNC: return CMP_ALWAYS;
    case RS_FOGEND: return one;
    case RS_FOGDENSITY: return one;
    case RS_STENCILFAIL:
    case RS_STENCILZFAIL:
    case RS_STENCILPASS:
    case RS_CCW_STENCILFAIL:
    case RS_CCW_STENCILZFAIL:
    case RS_CCW_STENCILPASS: return STENCILOP_KEEP;
    case RS_STENCILFUNC:
    case RS_CCW_STENCILFUNC: return CMP_ALWAYS;
    case RS_STENCILMASK:
    case RS_STENCILWRITEMASK:
    case RS_TEXTUREFACTOR:
    case RS_MULTISAMPLEMASK:
    case RS_BLENDFACTOR: return 0xFFFFFFFFu;
    case RS_CLIPPING:
    case RS_LIGHTING:
    case RS_COLORVERTEX:
    case RS_LOCALVIEWER:
    case RS_MULTISAMPLEANTIALIAS: return 1;
    case RS_DIFFUSEMATERIALSOURCE: return 1;
    case RS_SPECULARMATERIALSOURCE: return 2;
    case RS_POINTSIZE:
    case RS_POINTSIZE_MIN:
    case RS_MINTESSELLATIONLEVEL:
    case RS_MAXTESSELLATIONLEVEL:
    case RS_ADAPTIVETESS_W: return one;
    case RS_POINTSIZE_MAX: return asDword(256.0f);
    case RS_COLORWRITEENABLE:
    case RS_COLORWRITEENABLE1:
    case RS_COLORWRITEENABLE2:
    case RS_COLORWRITEENABLE3: return 0xF;
    case RS_BLENDOP:
    case RS_BLENDOPALPHA: return BLENDOP_ADD;
    case RS_POSITIONDEGREE: return 3;
    case RS_NORMALDEGREE: return 1;
    case RS_SRCBLENDALPHA: return BLEND_ONE;
    case RS_DESTBLENDALPHA: return BLEND_ZERO;
    default: return 0;
    }
}

std::uint32_t defaultSamplerState(std::uint32_t ss) {
    switch (ss) {
    case SAMP_ADDRESSU:
    case SAMP_ADDRESSV:
    case SAMP_ADDRESSW: return TADDRESS_WRAP;
    case SAMP_MAGFILTER:
    case SAMP_MINFILTER: return TEXF_POINT;
    case SAMP_MIPFILTER: return TEXF_NONE;
    case SAMP_MAXANISOTROPY: return 1;
    default: return 0;
    }
}

VkBlendFactor blendFactor(std::uint32_t b, bool alpha) {
    switch (b) {
    case BLEND_ZERO: return VK_BLEND_FACTOR_ZERO;
    case BLEND_ONE: return VK_BLEND_FACTOR_ONE;
    case BLEND_SRCCOLOR: return alpha ? VK_BLEND_FACTOR_SRC_ALPHA : VK_BLEND_FACTOR_SRC_COLOR;
    case BLEND_INVSRCCOLOR: return alpha ? VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA : VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR;
    case BLEND_SRCALPHA:
    case BLEND_BOTHSRCALPHA: return VK_BLEND_FACTOR_SRC_ALPHA;
    case BLEND_INVSRCALPHA:
    case BLEND_BOTHINVSRCALPHA: return VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    case BLEND_DESTALPHA: return VK_BLEND_FACTOR_DST_ALPHA;
    case BLEND_INVDESTALPHA: return VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA;
    case BLEND_DESTCOLOR: return alpha ? VK_BLEND_FACTOR_DST_ALPHA : VK_BLEND_FACTOR_DST_COLOR;
    case BLEND_INVDESTCOLOR: return alpha ? VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA : VK_BLEND_FACTOR_ONE_MINUS_DST_COLOR;
    case BLEND_SRCALPHASAT: return alpha ? VK_BLEND_FACTOR_ONE : VK_BLEND_FACTOR_SRC_ALPHA_SATURATE;
    case BLEND_BLENDFACTOR: return alpha ? VK_BLEND_FACTOR_CONSTANT_ALPHA : VK_BLEND_FACTOR_CONSTANT_COLOR;
    case BLEND_INVBLENDFACTOR: return alpha ? VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_ALPHA : VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_COLOR;
    default: return VK_BLEND_FACTOR_ONE;
    }
}

VkBlendOp blendOp(std::uint32_t op) {
    switch (op) {
    case BLENDOP_SUBTRACT: return VK_BLEND_OP_SUBTRACT;
    case BLENDOP_REVSUBTRACT: return VK_BLEND_OP_REVERSE_SUBTRACT;
    case BLENDOP_MIN: return VK_BLEND_OP_MIN;
    case BLENDOP_MAX: return VK_BLEND_OP_MAX;
    default: return VK_BLEND_OP_ADD;
    }
}

VkCompareOp compareOp(std::uint32_t c) {
    switch (c) {
    case CMP_NEVER: return VK_COMPARE_OP_NEVER;
    case CMP_LESS: return VK_COMPARE_OP_LESS;
    case CMP_EQUAL: return VK_COMPARE_OP_EQUAL;
    case CMP_LESSEQUAL: return VK_COMPARE_OP_LESS_OR_EQUAL;
    case CMP_GREATER: return VK_COMPARE_OP_GREATER;
    case CMP_NOTEQUAL: return VK_COMPARE_OP_NOT_EQUAL;
    case CMP_GREATEREQUAL: return VK_COMPARE_OP_GREATER_OR_EQUAL;
    default: return VK_COMPARE_OP_ALWAYS;
    }
}

VkStencilOp stencilOp(std::uint32_t s) {
    switch (s) {
    case STENCILOP_ZERO: return VK_STENCIL_OP_ZERO;
    case STENCILOP_REPLACE: return VK_STENCIL_OP_REPLACE;
    case STENCILOP_INCRSAT: return VK_STENCIL_OP_INCREMENT_AND_CLAMP;
    case STENCILOP_DECRSAT: return VK_STENCIL_OP_DECREMENT_AND_CLAMP;
    case STENCILOP_INVERT: return VK_STENCIL_OP_INVERT;
    case STENCILOP_INCR: return VK_STENCIL_OP_INCREMENT_AND_WRAP;
    case STENCILOP_DECR: return VK_STENCIL_OP_DECREMENT_AND_WRAP;
    default: return VK_STENCIL_OP_KEEP;
    }
}

VkCullModeFlags cullMode(std::uint32_t c) {
    switch (c) {
    case CULL_CW: return VK_CULL_MODE_FRONT_BIT;
    case CULL_CCW: return VK_CULL_MODE_BACK_BIT;
    default: return VK_CULL_MODE_NONE;
    }
}

VkPolygonMode polygonMode(std::uint32_t f) {
    switch (f) {
    case FILL_POINT: return VK_POLYGON_MODE_POINT;
    case FILL_WIREFRAME: return VK_POLYGON_MODE_LINE;
    default: return VK_POLYGON_MODE_FILL;
    }
}

VkSamplerAddressMode addressMode(std::uint32_t a) {
    switch (a) {
    case TADDRESS_MIRROR: return VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT;
    case TADDRESS_CLAMP: return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    case TADDRESS_BORDER: return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
    case TADDRESS_MIRRORONCE: return VK_SAMPLER_ADDRESS_MODE_MIRROR_CLAMP_TO_EDGE;
    default: return VK_SAMPLER_ADDRESS_MODE_REPEAT;
    }
}

VkFilter filter(std::uint32_t f) {
    return f == TEXF_POINT || f == TEXF_NONE ? VK_FILTER_NEAREST : VK_FILTER_LINEAR;
}

VkSamplerMipmapMode mipmapMode(std::uint32_t f) {
    return f == TEXF_LINEAR || f == TEXF_ANISOTROPIC ? VK_SAMPLER_MIPMAP_MODE_LINEAR : VK_SAMPLER_MIPMAP_MODE_NEAREST;
}

DeclTypeInfo declType(std::uint32_t t) {
    switch (t) {
    case DECLTYPE_FLOAT1: return {VK_FORMAT_R32_SFLOAT, 4};
    case DECLTYPE_FLOAT2: return {VK_FORMAT_R32G32_SFLOAT, 8};
    case DECLTYPE_FLOAT3: return {VK_FORMAT_R32G32B32_SFLOAT, 12};
    case DECLTYPE_FLOAT4: return {VK_FORMAT_R32G32B32A32_SFLOAT, 16};
    case DECLTYPE_D3DCOLOR: return {VK_FORMAT_B8G8R8A8_UNORM, 4};  // bytes B,G,R,A; the shader sees (R,G,B,A)
    case DECLTYPE_UBYTE4: return {VK_FORMAT_R8G8B8A8_USCALED, 4};
    case DECLTYPE_SHORT2: return {VK_FORMAT_R16G16_SSCALED, 4};
    case DECLTYPE_SHORT4: return {VK_FORMAT_R16G16B16A16_SSCALED, 8};
    case DECLTYPE_UBYTE4N: return {VK_FORMAT_R8G8B8A8_UNORM, 4};
    case DECLTYPE_SHORT2N: return {VK_FORMAT_R16G16_SNORM, 4};
    case DECLTYPE_SHORT4N: return {VK_FORMAT_R16G16B16A16_SNORM, 8};
    case DECLTYPE_USHORT2N: return {VK_FORMAT_R16G16_UNORM, 4};
    case DECLTYPE_USHORT4N: return {VK_FORMAT_R16G16B16A16_UNORM, 8};
    case DECLTYPE_UDEC3: return {VK_FORMAT_A2B10G10R10_USCALED_PACK32, 4};
    case DECLTYPE_DEC3N: return {VK_FORMAT_A2B10G10R10_SNORM_PACK32, 4};
    case DECLTYPE_FLOAT16_2: return {VK_FORMAT_R16G16_SFLOAT, 4};
    case DECLTYPE_FLOAT16_4: return {VK_FORMAT_R16G16B16A16_SFLOAT, 8};
    default: return {};
    }
}

VkPrimitiveTopology topology(std::uint32_t pt) {
    switch (pt) {
    case PT_POINTLIST: return VK_PRIMITIVE_TOPOLOGY_POINT_LIST;
    case PT_LINELIST: return VK_PRIMITIVE_TOPOLOGY_LINE_LIST;
    case PT_LINESTRIP: return VK_PRIMITIVE_TOPOLOGY_LINE_STRIP;
    case PT_TRIANGLESTRIP: return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP;
    case PT_TRIANGLEFAN: return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_FAN;
    default: return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    }
}

std::uint32_t vertexCount(std::uint32_t pt, std::uint32_t n) {
    switch (pt) {
    case PT_POINTLIST: return n;
    case PT_LINELIST: return n * 2;
    case PT_LINESTRIP: return n + 1;
    case PT_TRIANGLESTRIP:
    case PT_TRIANGLEFAN: return n + 2;
    default: return n * 3;
    }
}

void colorToFloat4(std::uint32_t c, float out[4]) {
    out[0] = static_cast<float>((c >> 16) & 0xFF) / 255.0f;
    out[1] = static_cast<float>((c >> 8) & 0xFF) / 255.0f;
    out[2] = static_cast<float>(c & 0xFF) / 255.0f;
    out[3] = static_cast<float>((c >> 24) & 0xFF) / 255.0f;
}

}  // namespace rvk::d3d9
