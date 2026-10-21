#include "d3d9/fvf.h"

namespace rvk::d3d9 {

namespace {

// D3DFVF_TEXCOORDSIZE*: 0 FLOAT2, 1 FLOAT3, 2 FLOAT4, 3 FLOAT1
std::uint32_t texCoordFloats(std::uint32_t fvf, std::uint32_t i) {
    switch ((fvf >> (16 + 2 * i)) & 3) {
        case 0: return 2;
        case 1: return 3;
        case 2: return 4;
        default: return 1;
    }
}

std::uint32_t betaCount(std::uint32_t position) { return position >= FVF_XYZB1 && position <= FVF_XYZB5 ? (position - 4) / 2 : 0; }

}  // namespace

std::uint32_t fvfVertexSize(std::uint32_t fvf) {
    std::uint32_t size = 0;
    const std::uint32_t pos = fvf & FVF_POSITION_MASK;
    if (pos == FVF_XYZ) {
        size += 12;
    } else if (pos == FVF_XYZRHW || pos == FVF_XYZW) {
        size += 16;
    } else if (const std::uint32_t betas = betaCount(pos); betas != 0) {
        size += 12 + 4 * betas;
    }
    if (fvf & FVF_NORMAL) size += 12;
    if (fvf & FVF_PSIZE) size += 4;
    if (fvf & FVF_DIFFUSE) size += 4;
    if (fvf & FVF_SPECULAR) size += 4;
    const std::uint32_t tex = (fvf & FVF_TEXCOUNT_MASK) >> FVF_TEXCOUNT_SHIFT;
    for (std::uint32_t i = 0; i < tex && i < 8; ++i) {
        size += 4 * texCoordFloats(fvf, i);
    }
    return size;
}

std::vector<VertexElement> fvfDeclaration(std::uint32_t fvf) {
    std::vector<VertexElement> e;
    if ((fvf & FVF_RESERVED0) != 0) {
        return e;
    }
    std::uint16_t offset = 0;
    auto add = [&](std::uint8_t type, std::uint8_t usage, std::uint8_t index, std::uint16_t bytes) {
        e.push_back({0, offset, type, 0, usage, index});
        offset = static_cast<std::uint16_t>(offset + bytes);
    };
    const std::uint32_t pos = fvf & FVF_POSITION_MASK;
    if (pos == FVF_XYZ) {
        add(DECLTYPE_FLOAT3, DECLUSAGE_POSITION, 0, 12);
    } else if (pos == FVF_XYZRHW) {
        add(DECLTYPE_FLOAT4, DECLUSAGE_POSITIONT, 0, 16);
    } else if (pos == FVF_XYZW) {
        add(DECLTYPE_FLOAT4, DECLUSAGE_POSITION, 0, 16);
    } else if (const std::uint32_t betas = betaCount(pos); betas != 0) {
        add(DECLTYPE_FLOAT3, DECLUSAGE_POSITION, 0, 12);
        const bool lastBetaIndices = (fvf & (FVF_LASTBETA_UBYTE4 | FVF_LASTBETA_D3DCOLOR)) != 0;
        const std::uint32_t weights = lastBetaIndices ? betas - 1 : betas;
        if (weights > 4) {
            return {};
        }
        if (weights != 0) {
            add(static_cast<std::uint8_t>(DECLTYPE_FLOAT1 + weights - 1), DECLUSAGE_BLENDWEIGHT, 0,
                static_cast<std::uint16_t>(4 * weights));
        }
        if (lastBetaIndices) {
            add((fvf & FVF_LASTBETA_D3DCOLOR) != 0 ? DECLTYPE_D3DCOLOR : DECLTYPE_UBYTE4, DECLUSAGE_BLENDINDICES, 0, 4);
        }
    } else if (pos != 0) {
        return {};
    }
    if (fvf & FVF_NORMAL) add(DECLTYPE_FLOAT3, DECLUSAGE_NORMAL, 0, 12);
    if (fvf & FVF_PSIZE) add(DECLTYPE_FLOAT1, DECLUSAGE_PSIZE, 0, 4);
    if (fvf & FVF_DIFFUSE) add(DECLTYPE_D3DCOLOR, DECLUSAGE_COLOR, 0, 4);
    if (fvf & FVF_SPECULAR) add(DECLTYPE_D3DCOLOR, DECLUSAGE_COLOR, 1, 4);
    const std::uint32_t tex = (fvf & FVF_TEXCOUNT_MASK) >> FVF_TEXCOUNT_SHIFT;
    if (tex > 8) {
        return {};
    }
    for (std::uint32_t i = 0; i < tex; ++i) {
        const std::uint32_t n = texCoordFloats(fvf, i);
        add(static_cast<std::uint8_t>(DECLTYPE_FLOAT1 + n - 1), DECLUSAGE_TEXCOORD, static_cast<std::uint8_t>(i),
            static_cast<std::uint16_t>(4 * n));
    }
    e.push_back({kDeclEndStream, 0, DECLTYPE_UNUSED, 0, 0, 0});
    return e;
}

}  // namespace rvk::d3d9
