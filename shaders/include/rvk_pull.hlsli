// S-4 vertex pulling (docs/research/frame-ownership.md 14.12; abi.h VertexFetch / PullFormat, TranslateOptions::pullVertices): a pulled scene vertex
// shader's D3D9 inputs v0..v15 read from the draw's static buffers by reference instead of the input assembler. Included by the translator's pulled form
// after rvk_draw.hlsli (RVK_SCENE_RECORD: `pc` is the RvkScenePush, spare1 = the VertexFetch table). The conversion of every format reproduces the input
// assembler's result BIT FOR BIT (rvk_rhitest "vertex pulling = the input assembler (S-4)" compares every code of every format on both backends): the z
// pre-pass and the colour pass test the depth for equality, and an owned (pulled) z draw meets a composed (input-assembler) colour draw of the same geometry.
#ifndef RVK_PULL_HLSLI
#define RVK_PULL_HLSLI

static const uint RVK_FETCH_BYTES = 128u;  // abi.h kVertexFetchSize
static const uint RVK_FETCH_STRIDE = 32u, RVK_FETCH_BASE = 48u, RVK_FETCH_ATTR = 64u;

// the layout (abi.h VertexFetch; rvk_tests / rvk_rhitest compare it with the C++ one). The shaders read it word by word through the offsets above
struct RvkVertexFetch {
    uint4 ref01;        // the reference pairs of streams 0 (xy) and 1 (zw)
    uint4 ref23;
    uint4 stride;
    int baseVertex;
    uint valid;
    uint2 pad;
    uint4 attr[4];      // v0..v15: offset | stream << 12 | format << 16
};

// unsigned / signed normalised conversions: the input assembler rounds c / (2^n - 1) correctly (measured on the RX 7900 XTX, both backends); a shader division is not
// (DXC / the driver make it a multiply by the rounded reciprocal: 126 of the 256 UNORM8 codes one ulp off). c / N = c / 2^n + c / (2^n N): the first product is exact
// (a power of two), the second one rounding, the sum one more: correctly rounded for every code of N = 255, 65535 and 32767 (checked exhaustively in IEEE float32,
// fused or not; the rhitest case checks the GPU)
float rvkPullUnorm8(uint c) {
    const float f = float(c);
    precise const float q = f * (1.0 / 256.0) + f * (1.0 / 65280.0);
    return q;
}
float rvkPullUnorm16(uint c) {
    const float f = float(c);
    precise const float q = f * (1.0 / 65536.0) + f * (1.0 / 4294901760.0);
    return q;
}
float rvkPullSnorm16(uint c) {
    const float f = float(int(c << 16) >> 16);
    precise const float q = f * (1.0 / 32768.0) + f * (1.0 / 1073709056.0);
    return max(q, -1.0);
}

// the fetch block's header + attribute words of one vertex, loaded once (7 x 16-byte loads: the block is 128-byte aligned in its table) instead of per input
struct RvkPullCtx {
    uint4 ref01, ref23, stride;
    uint4 attr[4];
    uint vtx;  // SV_VertexID + the base vertex
};
// vertex `vid` (SV_VertexID: the index value; the command's vertexOffset is 0) of the draw whose fetch block is `blk`
RvkPullCtx rvkPullBegin(RvkAddr blk, uint vid) {
    RvkPullCtx c;
    c.ref01 = rvkRawLoad<uint4, 16>(blk);
    c.ref23 = rvkRawLoad<uint4, 16>(rvkAddrAdd(blk, 16u));
    c.stride = rvkRawLoad<uint4, 16>(rvkAddrAdd(blk, RVK_FETCH_STRIDE));
    const uint4 meta = rvkRawLoad<uint4, 16>(rvkAddrAdd(blk, RVK_FETCH_BASE));
    [unroll] for (uint i = 0u; i < 4u; ++i) c.attr[i] = rvkRawLoad<uint4, 16>(rvkAddrAdd(blk, RVK_FETCH_ATTR + i * 16u));
    c.vtx = uint(int(vid) + asint(meta.x));
    return c;
}

// input register `reg` (0..15, a constant at every call) of the vertex of `c`: ONE load of the attribute's bytes (dword / x2 / x3 / x4 by format, 4-byte aligned: the
// arena's strides and offsets are; RDNA's global loads take that alignment for the wide forms)
float4 rvkPullAttr(RvkPullCtx c, uint reg) {
    const uint4 aw = c.attr[reg >> 2];
    const uint w = (reg & 3u) == 0u ? aw.x : (reg & 3u) == 1u ? aw.y : (reg & 3u) == 2u ? aw.z : aw.w;
    const uint fmt = (w >> 16) & 0xffu;
    if (fmt == 0u) return float4(0.0, 0.0, 0.0, 1.0);
    const uint s = (w >> 12) & 3u;
    const uint2 ref = s == 0u ? c.ref01.xy : s == 1u ? c.ref01.zw : s == 2u ? c.ref23.xy : c.ref23.zw;
    const uint stride = s == 0u ? c.stride.x : s == 1u ? c.stride.y : s == 2u ? c.stride.z : c.stride.w;
    const RvkAddr a = rvkAddrAdd(rvkAddr(ref), c.vtx * stride + (w & 0xfffu));
    float4 v = float4(0.0, 0.0, 0.0, 1.0);
    switch (fmt) {
    case 1u: v.x = asfloat(rvkRawLoad<uint, 4>(a)); break;
    case 2u: v.xy = asfloat(rvkRawLoad<uint2, 4>(a)); break;
    case 3u: v.xyz = asfloat(rvkRawLoad<uint3, 4>(a)); break;
    case 4u: v = asfloat(rvkRawLoad<uint4, 4>(a)); break;
    case 5u: {  // D3DCOLOR: bytes B, G, R, A
        const uint c = rvkRawLoad<uint, 4>(a);
        v = float4(rvkPullUnorm8((c >> 16) & 0xffu), rvkPullUnorm8((c >> 8) & 0xffu), rvkPullUnorm8(c & 0xffu), rvkPullUnorm8(c >> 24));
        break;
    }
    case 6u: {
        const uint c = rvkRawLoad<uint, 4>(a);
        v = float4(float(c & 0xffu), float((c >> 8) & 0xffu), float((c >> 16) & 0xffu), float(c >> 24));
        break;
    }
    case 7u: {
        const uint c = rvkRawLoad<uint, 4>(a);
        v = float4(rvkPullUnorm8(c & 0xffu), rvkPullUnorm8((c >> 8) & 0xffu), rvkPullUnorm8((c >> 16) & 0xffu), rvkPullUnorm8(c >> 24));
        break;
    }
    case 8u: {
        const uint c = rvkRawLoad<uint, 4>(a);
        v.xy = float2(rvkPullSnorm16(c & 0xffffu), rvkPullSnorm16(c >> 16));
        break;
    }
    case 9u: {
        const uint2 c = rvkRawLoad<uint2, 4>(a);
        v = float4(rvkPullSnorm16(c.x & 0xffffu), rvkPullSnorm16(c.x >> 16), rvkPullSnorm16(c.y & 0xffffu), rvkPullSnorm16(c.y >> 16));
        break;
    }
    case 10u: {
        const uint c = rvkRawLoad<uint, 4>(a);
        v.xy = float2(rvkPullUnorm16(c & 0xffffu), rvkPullUnorm16(c >> 16));
        break;
    }
    case 11u: {
        const uint2 c = rvkRawLoad<uint2, 4>(a);
        v = float4(rvkPullUnorm16(c.x & 0xffffu), rvkPullUnorm16(c.x >> 16), rvkPullUnorm16(c.y & 0xffffu), rvkPullUnorm16(c.y >> 16));
        break;
    }
    case 12u: {
        const uint c = rvkRawLoad<uint, 4>(a);
        v.xy = float2(f16tof32(c & 0xffffu), f16tof32(c >> 16));
        break;
    }
    case 13u: {
        const uint2 c = rvkRawLoad<uint2, 4>(a);
        v = float4(f16tof32(c.x & 0xffffu), f16tof32(c.x >> 16), f16tof32(c.y & 0xffffu), f16tof32(c.y >> 16));
        break;
    }
    default: break;
    }
    return v;
}

#ifdef RVK_SCENE_RECORD
// the fetch block of the draw whose ref stream value is `ref` (the persistent record slot = recordBase + ref.x)
RvkAddr rvkPullBlock(uint2 ref) { return rvkAddrAdd(rvkAddr(pc.spare1), (pc.recordBase + ref.x) * RVK_FETCH_BYTES); }
#endif

#endif
