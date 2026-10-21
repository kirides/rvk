#pragma once

// D3DFVF codes: vertex size (D3DXGetFVFVertexSize) and the equivalent vertex declaration (D3DXDeclaratorFromFVF).

#include <cstdint>
#include <vector>

#include "d3d9/defs.h"

namespace rvk::d3d9 {

// D3DFVF_*
inline constexpr std::uint32_t FVF_XYZ = 0x002, FVF_XYZRHW = 0x004, FVF_XYZB1 = 0x006, FVF_XYZB5 = 0x00e, FVF_XYZW = 0x4002,
                               FVF_POSITION_MASK = 0x400e, FVF_NORMAL = 0x010, FVF_PSIZE = 0x020, FVF_DIFFUSE = 0x040,
                               FVF_SPECULAR = 0x080, FVF_TEXCOUNT_MASK = 0xf00, FVF_TEXCOUNT_SHIFT = 8,
                               FVF_LASTBETA_UBYTE4 = 0x1000, FVF_LASTBETA_D3DCOLOR = 0x8000, FVF_RESERVED0 = 0x001;

// D3DDECLUSAGE
enum DeclUsage : std::uint8_t {
    DECLUSAGE_POSITION = 0, DECLUSAGE_BLENDWEIGHT, DECLUSAGE_BLENDINDICES, DECLUSAGE_NORMAL, DECLUSAGE_PSIZE,
    DECLUSAGE_TEXCOORD, DECLUSAGE_TANGENT, DECLUSAGE_BINORMAL, DECLUSAGE_TESSFACTOR, DECLUSAGE_POSITIONT, DECLUSAGE_COLOR,
    DECLUSAGE_FOG, DECLUSAGE_DEPTH, DECLUSAGE_SAMPLE,
};

// bytes per vertex of an FVF (D3DXGetFVFVertexSize); 0 for 0
std::uint32_t fvfVertexSize(std::uint32_t fvf);

// the vertex declaration of an FVF on stream 0, in D3DXDeclaratorFromFVF's element order, terminated by D3DDECL_END.
// Empty when the FVF is invalid (D3DX: D3DERR_INVALIDCALL).
std::vector<VertexElement> fvfDeclaration(std::uint32_t fvf);

}  // namespace rvk::d3d9
