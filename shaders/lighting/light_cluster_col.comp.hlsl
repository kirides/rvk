// Column-wise cluster build of the Full-owned light table (WS21, docs/research/gpu-scene.md 9.16). The same output as light_cluster.comp.hlsl (header, light
// copy, per-cluster lists, occupancy bits) with GD3D11's scheme (D3D12/LightCull.hlsl): ONE GROUP PER SCREEN-TILE COLUMN of the froxel grid (both levels). The
// group tests each light once against the column's bounding box (the union of its slices' boxes), maps the light's view-space z range to the slices it can
// touch, runs the legacy per-cluster box test (the very same arithmetic) only for those slices and collects the hits as one bit per light and slice in shared
// memory; the column's lists are then written in ascending light order (the legacy kernel's order, capped at RVK_LIGHT_MAX_PER_CLUSTER) and the occupancy bits
// set. The contents are the legacy kernel's bit for bit (the coarse test is a superset of the exact one, the slice span has a slice of margin on both sides):
// `lighting.cluster_build = compare` runs the legacy kernel on the same frame and counts differences.
//
// The occupancy words are set with atomic adds of single bits (every cluster is written by exactly one group: add = or) into words the host cleared first.

#include "rvk_common.hlsli"
#include "rvk_light_model.hlsli"

struct LightClusterPush {
    RVK_PASS_PUSH_HEADER
    uint2 src;       // the CPU upload buffer's read reference: RvkLight[count] (this frame in flight's)
    uint2 dst;       // the device buffer's read-write reference
    uint4 grid;      // x tiles x, y tiles y, z slices, w light count
    float4 zs;       // x near, y far, z slices / ln(far / near), w the occupancy bit set's byte offset (uint bits; 0 = not written)
    uint4 offs;      // x lights offset, y clusters offset (bytes), z style, w flags
    uint4 shadow;    // x light shadow image slot (bindless, 0 = none this frame), y width, z height, w light count with a shadow id
    uint4 grid1;     // the far level: x tiles x, y tiles y, z slices (0 = none), w its clusters' offset (bytes)
    float4 zs1;      // x near (= zs.y), y far, z slices / ln(far / near), w unused
};
RVK_PUSH(LightClusterPush, pc);

#define RVK_COL_MAX_SLICES 24u
#define RVK_COL_MASK_WORDS 32u  // 1,024 lights (RVK_LIGHT_MAX) per slice

float2 rvkClusterSlope(RvkFrame f, float2 ndc) {
    float4 v = mul(float4(ndc, 0.5, 1.0), f.invProj);
    return v.xy / (abs(v.z) > 1e-9 ? v.z : 1e-9);
}

groupshared uint gsMask[RVK_COL_MAX_SLICES * RVK_COL_MASK_WORDS];
groupshared uint gsPre[RVK_COL_MAX_SLICES * RVK_COL_MASK_WORDS];
groupshared float3 gsBmin[RVK_COL_MAX_SLICES];
groupshared float3 gsBmax[RVK_COL_MAX_SLICES];

[numthreads(64, 1, 1)]
void CSMain(uint3 gid : SV_GroupID, uint gi : SV_GroupIndex) {
    const uint count = pc.grid.w;
    const uint cols0 = pc.grid.x * pc.grid.y;
    const uint clusters0 = cols0 * pc.grid.z;
    RvkFrame f = rvkLoadFrame(pc.frame);

    // header + light copy by the first groups (the copy: lights the pixel shaders read; the CPU upload buffer is per frame in flight, its address changes)
    const uint t = gid.x * 64u + gi;
    if (t == 0u) {
        rvkBufStore(pc.dst, 0, f.frameInfo.x + 1u);
        rvkBufStore(pc.dst, 4, count);
        rvkBufStore(pc.dst, 8, pc.shadow.x);
        rvkBufStore(pc.dst, 12, pc.shadow.y | (pc.shadow.z << 16));
        rvkBufStore(pc.dst, 16, pc.grid.x);
        rvkBufStore(pc.dst, 20, pc.grid.y);
        rvkBufStore(pc.dst, 24, pc.grid.z);
        rvkBufStore(pc.dst, 28, RVK_LIGHT_MAX_PER_CLUSTER);
        rvkBufStore(pc.dst, 32, asuint(pc.zs.x));
        rvkBufStore(pc.dst, 36, asuint(pc.zs.y));
        rvkBufStore(pc.dst, 40, asuint(pc.zs.z));
        rvkBufStore(pc.dst, 44, asuint(pc.zs.w));  // the occupancy bit set's byte offset (0 = none)
        rvkBufStore(pc.dst, 48, pc.offs.x);
        rvkBufStore(pc.dst, 52, pc.offs.y);
        rvkBufStore(pc.dst, 56, pc.offs.z);
        rvkBufStore(pc.dst, 60, pc.offs.w);
        rvkBufStore(pc.dst, 64, pc.grid1.x);
        rvkBufStore(pc.dst, 68, pc.grid1.y);
        rvkBufStore(pc.dst, 72, pc.grid1.z);
        rvkBufStore(pc.dst, 76, pc.grid1.w);
        rvkBufStore(pc.dst, 80, asuint(pc.zs1.x));
        rvkBufStore(pc.dst, 84, asuint(pc.zs1.y));
        rvkBufStore(pc.dst, 88, asuint(pc.zs1.z));
        rvkBufStore(pc.dst, 92, 0u);
    }
    if (t < count) {
        [unroll] for (uint k = 0; k < 4; ++k) {
            float4 v = rvkRawLoad<float4, 16>(rvkAddrAdd(rvkAddr(pc.src), t * RVK_LIGHT_BYTES + k * 16u));
            rvkRwStore<float4, 16>(rvkAddrAdd(rvkAddr(pc.dst), pc.offs.x + t * RVK_LIGHT_BYTES + k * 16u), v);
        }
    }

    // this group's column: (level, tile x, tile y)
    const bool far = gid.x >= cols0;
    const uint u = far ? gid.x - cols0 : gid.x;
    const uint3 gd = far ? pc.grid1.xyz : pc.grid.xyz;
    const uint slices = min(gd.z, RVK_COL_MAX_SLICES);
    // a level with no slices (grid1.z = 0 = no far level): nothing to build; without this s1 = slices - 1 wrapped to 0xFFFFFFFF (gsMask out of bounds) and gsBmin[0] was read uninitialised.
    // (uniform per group: every thread of the group leaves together, no barrier is skipped; the header + light copy above are done)
    if (slices == 0u) return;
    const uint tx = u % gd.x;
    const uint ty = u / gd.x;
    const uint colStride = gd.x * gd.y;  // the cluster index step between two slices of the column
    const float zNear = far ? pc.zs1.x : pc.zs.x, zFar = far ? pc.zs1.y : pc.zs.y, zScale = far ? pc.zs1.z : pc.zs.z;
    const uint levelBase = far ? pc.grid1.w : pc.offs.y;
    const uint cidBase = (far ? clusters0 : 0u) + u;  // + slice * colStride = the cluster's index in the combined space
    const uint occOff = asuint(pc.zs.w);

    // phase 0: the slice boxes (the legacy kernel's arithmetic, one thread per slice) and the cleared masks
    [unroll] for (uint c = 0; c < (RVK_COL_MAX_SLICES * RVK_COL_MASK_WORDS) / 64u; ++c) gsMask[c * 64u + gi] = 0u;
    if (gi < slices) {
        const uint sz = gi;
        const float x0 = -1.0 + 2.0 * float(tx) / float(gd.x), x1 = -1.0 + 2.0 * float(tx + 1u) / float(gd.x);
        const float y1 = 1.0 - 2.0 * float(ty) / float(gd.y), y0 = 1.0 - 2.0 * float(ty + 1u) / float(gd.y);
        const float2 s00 = rvkClusterSlope(f, float2(x0, y0)), s10 = rvkClusterSlope(f, float2(x1, y0));
        const float2 s01 = rvkClusterSlope(f, float2(x0, y1)), s11 = rvkClusterSlope(f, float2(x1, y1));
        const float zn = zNear * exp(float(sz) / zScale);
        const float zf = zNear * exp(float(sz + 1u) / zScale);
        float3 bmin = float3(1e30, 1e30, 1e30), bmax = float3(-1e30, -1e30, -1e30);
        const float2 sl[4] = {s00, s10, s01, s11};
        [unroll] for (uint c = 0; c < 4; ++c) {
            float3 a = float3(sl[c] * zn, zn), b = float3(sl[c] * zf, zf);
            bmin = min(bmin, min(a, b));
            bmax = max(bmax, max(a, b));
        }
        gsBmin[sz] = bmin;
        gsBmax[sz] = bmax;
    }
    GroupMemoryBarrierWithGroupSync();
    // the column's box: the union of its slices' boxes (every thread, from shared memory)
    float3 cmin = gsBmin[0], cmax = gsBmax[0];
    for (uint s = 1; s < slices; ++s) {
        cmin = min(cmin, gsBmin[s]);
        cmax = max(cmax, gsBmax[s]);
    }

    // phase 1: every light once against the column box, then the exact slice box test for the slices its z range can touch
    for (uint base = 0; base < count; base += 64u) {
        const uint li = base + gi;
        if (li < count) {
            float4 pr = rvkRawLoad<float4, 16>(rvkAddrAdd(rvkAddr(pc.src), li * RVK_LIGHT_BYTES));
            const float4 g = float4(mul(float4(pr.xyz, 1.0), f.view).xyz, rsqrt(max(pr.w, 1e-12)));
            const float r2 = g.w * g.w;
            const float3 dc = g.xyz - clamp(g.xyz, cmin, cmax);
            if (dot(dc, dc) <= r2) {
                // the slices whose z range [near e^(s / scale), near e^((s + 1) / scale)] meets [z - r, z + r], one slice of margin on both sides
                const float lo = g.z - g.w, hi = g.z + g.w;
                const float fl = lo <= zNear ? 0.0 : min(log(lo / zNear) * zScale, 1e4);
                const float fh = hi <= zNear ? 0.0 : min(log(hi / zNear) * zScale, 1e4);
                const uint s0 = uint(max(int(floor(fl)) - 1, 0));
                const uint s1 = uint(min(int(floor(fh)) + 1, int(slices) - 1));
                for (uint s = s0; s <= s1; ++s) {
                    const float3 d = g.xyz - clamp(g.xyz, gsBmin[s], gsBmax[s]);
                    if (dot(d, d) <= r2) InterlockedOr(gsMask[s * RVK_COL_MASK_WORDS + (li >> 5)], 1u << (li & 31u));
                }
            }
        }
    }
    GroupMemoryBarrierWithGroupSync();

    // phase 2: per slice the exclusive prefix of the words' populations (thread per slice), the count word and the occupancy bit
    if (gi < slices) {
        uint n = 0;
        for (uint w = 0; w < RVK_COL_MASK_WORDS; ++w) {
            gsPre[gi * RVK_COL_MASK_WORDS + w] = n;
            n += countbits(gsMask[gi * RVK_COL_MASK_WORDS + w]);
        }
        const uint cid = cidBase + gi * colStride;
        rvkBufStore(pc.dst, levelBase + (u + gi * colStride) * RVK_LIGHT_CLUSTER_BYTES, min(n, RVK_LIGHT_MAX_PER_CLUSTER));
        if (n > 0u && occOff != 0u) rvkBufAdd(pc.dst, occOff + (cid >> 5) * 4u, 1u << (cid & 31u));
    }
    GroupMemoryBarrierWithGroupSync();

    // phase 3: the lists, ascending light index (words in order, bits in order), the first RVK_LIGHT_MAX_PER_CLUSTER
    for (uint i = gi; i < slices * RVK_COL_MASK_WORDS; i += 64u) {
        uint m = gsMask[i];
        uint n = gsPre[i];
        if (m == 0u || n >= RVK_LIGHT_MAX_PER_CLUSTER) continue;
        const uint s = i / RVK_COL_MASK_WORDS, w = i % RVK_COL_MASK_WORDS;
        const uint outBase = levelBase + (u + s * colStride) * RVK_LIGHT_CLUSTER_BYTES;
        while (m != 0u && n < RVK_LIGHT_MAX_PER_CLUSTER) {
            const uint b = firstbitlow(m);
            rvkBufStore(pc.dst, outBase + 4u + n * 4u, w * 32u + b);
            ++n;
            m &= m - 1u;
        }
    }
}
