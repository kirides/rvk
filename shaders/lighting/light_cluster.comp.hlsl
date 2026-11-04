// Cluster build of the Full-owned light table (src/enh/light_table.*, docs/research/gpu-scene.md 9.7): one thread per cluster of a view-space froxel
// grid (tiles x tiles x exponential depth slices of the main camera) tests every light of the table (a sphere) against the cluster's box and writes
// the cluster's light list (count + indices). Threads below the light count also copy their light from the CPU upload buffer into the device
// buffer the material shaders read, and thread 0 writes the header (frame stamp, grid, style). Runs at AfterDepthPrepass (needs only the camera).
//
// The grid: NDC corner (x, y) -> view-space ray slope (x / z, y / z) through the inverse projection (any depth on the ray; the sign of z cancels in the
// ratio); a cluster is the box around the 8 corners at its slice's near and far z. View space = the FrameBlock's `view` (row vectors; z forward).

#include "rvk_common.hlsli"
#include "rvk_light_model.hlsli"

struct LightClusterPush {
    RVK_PASS_PUSH_HEADER
    uint2 src;       // the CPU upload buffer's read reference: RvkLight[count] (this frame in flight's)
    uint2 dst;       // the device buffer's read-write reference
    uint4 grid;      // x tiles x, y tiles y, z slices, w light count
    float4 zs;       // x near, y far, z slices / ln(far / near), w unused
    uint4 offs;      // x lights offset, y clusters offset (bytes), z style, w flags
};
RVK_PUSH(LightClusterPush, pc);

float2 rvkClusterSlope(RvkFrame f, float2 ndc) {
    float4 v = mul(float4(ndc, 0.5, 1.0), f.invProj);
    return v.xy / (abs(v.z) > 1e-9 ? v.z : 1e-9);
}

[numthreads(64, 1, 1)]
void CSMain(uint3 id : SV_DispatchThreadID) {
    const uint t = id.x;
    const uint count = pc.grid.w;
    const uint clusters = pc.grid.x * pc.grid.y * pc.grid.z;
    RvkFrame f = rvkLoadFrame(pc.frame);

    if (t == 0u) {
        rvkBufStore(pc.dst, 0, f.frameInfo.x + 1u);
        rvkBufStore(pc.dst, 4, count);
        rvkBufStore(pc.dst, 8, 0u);
        rvkBufStore(pc.dst, 12, 0u);
        rvkBufStore(pc.dst, 16, pc.grid.x);
        rvkBufStore(pc.dst, 20, pc.grid.y);
        rvkBufStore(pc.dst, 24, pc.grid.z);
        rvkBufStore(pc.dst, 28, RVK_LIGHT_MAX_PER_CLUSTER);
        rvkBufStore(pc.dst, 32, asuint(pc.zs.x));
        rvkBufStore(pc.dst, 36, asuint(pc.zs.y));
        rvkBufStore(pc.dst, 40, asuint(pc.zs.z));
        rvkBufStore(pc.dst, 44, 0u);
        rvkBufStore(pc.dst, 48, pc.offs.x);
        rvkBufStore(pc.dst, 52, pc.offs.y);
        rvkBufStore(pc.dst, 56, pc.offs.z);
        rvkBufStore(pc.dst, 60, pc.offs.w);
    }
    // the copy: lights the pixel shaders read (the CPU upload buffer is per frame in flight, its address changes)
    if (t < count) {
        [unroll] for (uint k = 0; k < 4; ++k) {
            float4 v = rvkRawLoad<float4, 16>(rvkAddrAdd(rvkAddr(pc.src), t * RVK_LIGHT_BYTES + k * 16u));
            rvkRwStore<float4, 16>(rvkAddrAdd(rvkAddr(pc.dst), pc.offs.x + t * RVK_LIGHT_BYTES + k * 16u), v);
        }
    }
    if (t >= clusters) return;

    // cluster t -> (tile x, tile y, slice)
    const uint tx = t % pc.grid.x;
    const uint ty = (t / pc.grid.x) % pc.grid.y;
    const uint sz = t / (pc.grid.x * pc.grid.y);
    // NDC of the tile's corners (y up in NDC; tile row 0 = top of the screen)
    const float x0 = -1.0 + 2.0 * float(tx) / float(pc.grid.x), x1 = -1.0 + 2.0 * float(tx + 1u) / float(pc.grid.x);
    const float y1 = 1.0 - 2.0 * float(ty) / float(pc.grid.y), y0 = 1.0 - 2.0 * float(ty + 1u) / float(pc.grid.y);
    const float2 s00 = rvkClusterSlope(f, float2(x0, y0)), s10 = rvkClusterSlope(f, float2(x1, y0));
    const float2 s01 = rvkClusterSlope(f, float2(x0, y1)), s11 = rvkClusterSlope(f, float2(x1, y1));
    const float zn = pc.zs.x * exp(float(sz) / pc.zs.z);
    const float zf = pc.zs.x * exp(float(sz + 1u) / pc.zs.z);
    float3 bmin = float3(1e30, 1e30, 1e30), bmax = float3(-1e30, -1e30, -1e30);
    const float2 sl[4] = {s00, s10, s01, s11};
    [unroll] for (uint c = 0; c < 4; ++c) {
        float3 a = float3(sl[c] * zn, zn), b = float3(sl[c] * zf, zf);
        bmin = min(bmin, min(a, b));
        bmax = max(bmax, max(a, b));
    }
    uint n = 0;
    const uint outBase = pc.offs.y + t * RVK_LIGHT_CLUSTER_BYTES;
    for (uint i = 0; i < count && n < RVK_LIGHT_MAX_PER_CLUSTER; ++i) {
        float4 pr = rvkRawLoad<float4, 16>(rvkAddrAdd(rvkAddr(pc.src), i * RVK_LIGHT_BYTES));
        float range = rsqrt(max(pr.w, 1e-12));
        float3 c = mul(float4(pr.xyz, 1.0), f.view).xyz;
        float3 q = clamp(c, bmin, bmax);
        float3 d = c - q;
        if (dot(d, d) <= range * range) {
            rvkBufStore(pc.dst, outBase + 4u + n * 4u, i);
            ++n;
        }
    }
    rvkBufStore(pc.dst, outBase, n);
}
