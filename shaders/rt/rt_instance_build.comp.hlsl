// The RT scene's TLAS instances + geometry records built on the GPU (src/enh/rt_gpu_build.h, docs/research/frame-ownership.md 14.43; native.rt_gpu_build).
// One thread per TLAS instance of the frame: entry k (input slot | literal, rung, cut-out, glass, segments) + the persistent tables (instance input, BLAS row,
// material row) -> VkAccelerationStructureInstanceKHR (= D3D12_RAYTRACING_INSTANCE_DESC) k and RtGeometryRecord k. No arithmetic on the data: words are
// selected and copied (the CPU twin enh::rtbuild::compose must match byte for byte). Out-of-range slots give an inactive instance (address 0, mask 0) and a
// zero record (the GPU never follows an unchecked index).
//
// header (rtbuild::Header, 7 x uint4): h0 = (count, literals, inputCap, blasCap), h1 = (matCap, -, -, -), h2 = (entries.xy, inputs.xy),
// h3 = (blas.xy, materials.xy), h4 = (literal.xy, outInst.xy rw), h5 = (outRec.xy rw, boxAs.xy), h6 = (boxVertices.xy, boxIndices.xy)

#include "rvk_common.hlsli"

struct RtInstanceBuildPush {
    RVK_PASS_PUSH_HEADER
    uint2 table;
    uint2 pad;
};
RVK_PUSH(RtInstanceBuildPush, pc);

static const uint IB_LITERAL = 0x80000000u;
static const uint IB_ALPHA = 4u, IB_GLASS = 8u;
// shader::abi (abi.h)
static const uint IB_REC_ALPHA_TESTED = 2u;
static const uint IB_REC_SEGMENTED = 64u;
static const uint IB_REC_BOX_CLEAR = 1u | 2u | 4u | 8u;  // kRtRecordIndex32 | AlphaTested | SpeedTree | Instanced
static const uint IB_KIND_SHIFT = 8u;
static const uint IB_MASK_OPAQUE = 1u, IB_MASK_ALPHA = 2u, IB_MASK_GLASS = 8u;
static const uint IB_ABSENT = 0xFFFFFFFFu;
static const uint IB_UV_BOX = 0x8u | 0x400u;  // kRtUvNoTexture | kRtUvOpacityOpaque
static const uint IB_BOX_COLOUR = 0xFF5C6166u;
static const uint IB_SPEEDTREE_ALPHA = 5u | (80u << 8);
// VkGeometryInstanceFlagBitsKHR (= D3D12_RAYTRACING_INSTANCE_FLAGS): cull disable 1, force opaque 4, force no opaque 8
static const uint IB_INST_CULL_DISABLE = 1u, IB_INST_OPAQUE = 4u, IB_INST_NO_OPAQUE = 8u;

uint4 ibLoad(RvkAddr a, uint byteOffset) { return rvkRawLoad<uint4, 16>(rvkAddrAdd(a, byteOffset)); }
void ibStore(RvkAddr a, uint byteOffset, uint4 v) { rvkRwStore<uint4, 16>(rvkAddrAdd(a, byteOffset), v); }

void ibWrite(RvkAddr outInst, RvkAddr outRec, uint k, uint4 i0, uint4 i1, uint4 i2, uint4 i3, uint4 r0, uint4 r1, uint4 r2, uint4 r3, uint4 r4, uint4 r5, uint4 r6) {
    ibStore(outInst, k * 64u, i0);
    ibStore(outInst, k * 64u + 16u, i1);
    ibStore(outInst, k * 64u + 32u, i2);
    ibStore(outInst, k * 64u + 48u, i3);
    ibStore(outRec, k * 112u, r0);
    ibStore(outRec, k * 112u + 16u, r1);
    ibStore(outRec, k * 112u + 32u, r2);
    ibStore(outRec, k * 112u + 48u, r3);
    ibStore(outRec, k * 112u + 64u, r4);
    ibStore(outRec, k * 112u + 80u, r5);
    ibStore(outRec, k * 112u + 96u, r6);
}

uint ibRecordAlpha(uint flags, uint data, uint matAlpha) {
    if ((flags & 4u) != 0u && (flags & IB_REC_ALPHA_TESTED) != 0u) return IB_SPEEDTREE_ALPHA;
    if ((data & 0xFFFFu) != 0u) return data & 0xFFFFu;
    return matAlpha != 0u ? matAlpha : 8u;
}

[numthreads(64, 1, 1)]
void CSMain(uint3 dtid : SV_DispatchThreadID) {
    RvkAddr table = rvkAddr(pc.table);
    uint4 h0 = ibLoad(table, 0u);
    uint4 h1 = ibLoad(table, 16u);
    uint4 h2 = ibLoad(table, 32u);
    uint4 h3 = ibLoad(table, 48u);
    uint4 h4 = ibLoad(table, 64u);
    uint4 h5 = ibLoad(table, 80u);
    uint4 h6 = ibLoad(table, 96u);
    uint k = dtid.x;
    if (k >= h0.x) return;
    RvkAddr entries = rvkAddr(h2.xy);
    RvkAddr inputs = rvkAddr(h2.zw);
    RvkAddr blas = rvkAddr(h3.xy);
    RvkAddr materials = rvkAddr(h3.zw);
    RvkAddr literals = rvkAddr(h4.xy);
    RvkAddr outInst = rvkAddr(h4.zw);
    RvkAddr outRec = rvkAddr(h5.xy);
    uint4 e = ibLoad(entries, k * 16u);
    uint4 z = uint4(0u, 0u, 0u, 0u);
    if ((e.x & IB_LITERAL) != 0u) {
        uint li = e.x & ~IB_LITERAL;
        if (li >= h0.y) {
            ibWrite(outInst, outRec, k, z, z, z, z, z, z, z, z, z, z, z);
            return;
        }
        uint base = li * 176u;
        ibWrite(outInst, outRec, k, ibLoad(literals, base), ibLoad(literals, base + 16u), ibLoad(literals, base + 32u), ibLoad(literals, base + 48u),
                ibLoad(literals, base + 64u), ibLoad(literals, base + 80u), ibLoad(literals, base + 96u), ibLoad(literals, base + 112u),
                ibLoad(literals, base + 128u), ibLoad(literals, base + 144u), ibLoad(literals, base + 160u));
        return;
    }
    if (e.x >= h0.z) {
        ibWrite(outInst, outRec, k, z, z, z, z, z, z, z, z, z, z, z);
        return;
    }
    // the instance input (128 B): rows (3 x uint4), box rows (3 x uint4), (blasRow, material, flags, data), (instanceId, -, -, -)
    uint ib = e.x * 128u;
    uint4 w6 = ibLoad(inputs, ib + 96u);
    uint4 w7 = ibLoad(inputs, ib + 112u);
    if (w6.x >= h0.w || w6.y >= h1.x) {
        ibWrite(outInst, outRec, k, z, z, z, z, z, z, z, z, z, z, z);
        return;
    }
    // the BLAS row (64 B): (as.xy, vertices.xy), (indices.xy, proxyAs.xy), (proxyIndices.xy, stride, normal), (texcoord0, texcoord1, texcoord6, color0)
    uint4 b0 = ibLoad(blas, w6.x * 64u);
    uint4 b1 = ibLoad(blas, w6.x * 64u + 16u);
    uint4 b2 = ibLoad(blas, w6.x * 64u + 32u);
    uint4 b3 = ibLoad(blas, w6.x * 64u + 48u);
    // the material row (64 B): (albedoTex, albedoSampler, uvSet, packWord), (packLod, alphaWord, bits, opacityTex), uvMap0, uvMap1
    uint4 m0 = ibLoad(materials, w6.y * 64u);
    uint4 m1 = ibLoad(materials, w6.y * 64u + 16u);
    uint4 m2 = ibLoad(materials, w6.y * 64u + 32u);
    uint4 m3 = ibLoad(materials, w6.y * 64u + 48u);

    bool alpha = (e.y & IB_ALPHA) != 0u;
    bool glass = (e.y & IB_GLASS) != 0u;
    uint variant = e.y & 3u;
    uint segCount = e.y >> 16;
    uint flags = w6.z, data = w6.w;

    uint4 i0, i1, i2;
    if (variant == 2u) {
        i0 = ibLoad(inputs, ib + 48u);
        i1 = ibLoad(inputs, ib + 64u);
        i2 = ibLoad(inputs, ib + 80u);
    } else {
        i0 = ibLoad(inputs, ib);
        i1 = ibLoad(inputs, ib + 16u);
        i2 = ibLoad(inputs, ib + 32u);
    }
    uint mask = glass ? IB_MASK_GLASS : alpha ? IB_MASK_ALPHA : IB_MASK_OPAQUE;
    uint iflags = IB_INST_CULL_DISABLE | (alpha ? IB_INST_NO_OPAQUE : IB_INST_OPAQUE);
    uint2 asAddr = variant == 1u ? b1.zw : variant == 2u ? h5.zw : b0.xy;
    uint4 i3 = uint4((k & 0x00FFFFFFu) | (mask << 24), iflags << 24, asAddr.x, asAddr.y);

    uint2 vtx = variant == 2u ? h6.xy : b0.zw;
    uint2 idx = variant == 1u ? b2.xy : variant == 2u ? h6.zw : b1.xy;
    uint stride = variant == 2u ? 12u : b2.z;
    uint rflags = (flags & ~IB_REC_ALPHA_TESTED) | (alpha ? IB_REC_ALPHA_TESTED : 0u) | (((data >> 16) & 0xFFu) << IB_KIND_SHIFT);
    if (segCount != 0u) rflags |= IB_REC_SEGMENTED;
    if (variant == 2u) rflags &= ~IB_REC_BOX_CLEAR;
    uint normal = variant == 2u ? IB_ABSENT : b2.w;
    uint4 attribs = variant == 2u ? uint4(IB_ABSENT, IB_ABSENT, IB_ABSENT, IB_ABSENT) : b3;  // texcoord0, texcoord1, texcoord6, color0
    uint ralpha = variant == 2u ? 8u : (ibRecordAlpha(flags, data, m1.y) | (m1.x << 16));
    uint albedoTex = variant == 2u ? 0u : m0.x;
    uint uvSet = variant == 2u ? IB_UV_BOX : m0.z;
    uint4 uv0 = variant == 2u ? uint4(0u, 0u, 0u, IB_BOX_COLOUR) : m2;
    uint4 uv1 = variant == 2u ? z : m3;
    uint2 seg = segCount != 0u ? e.zw : uint2(0u, 0u);

    uint4 r0 = uint4(vtx, idx);
    uint4 r1 = uint4(stride, rflags, normal, attribs.x);
    uint4 r2 = uint4(attribs.y, attribs.z, attribs.w, w7.x);
    uint4 r3 = uint4(albedoTex, m0.y, ralpha, uvSet);
    uint4 r6 = uint4(seg, segCount, m0.w);
    ibWrite(outInst, outRec, k, i0, i1, i2, i3, r0, r1, r2, r3, uv0, uv1, r6);
}
