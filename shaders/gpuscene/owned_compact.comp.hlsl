// F2c-4c GPU compaction of the persistent owned draws (src/gpuscene/owned_compact.*, docs/research/frame-ownership.md 7.32) + F5-2 cluster cull (7.34).
// One group per bucket of a flush: the bucket's entries (slot, view slot; 8 bytes each, in the engine's order) of the list block are read in chunks of 64, each
// entry's record (DrawRecord at records + slot x 128) must hold a draw address and a non-zero index count (the same guards as the CPU's), and the valid ones are
// compacted IN ORDER (an inclusive scan per chunk) into VkDrawIndexedIndirectCommand records {indexCount, 1, firstIndex, vertexOffset, firstInstance}, with
// firstInstance = the entry's position in the list block (the instance-rate ref stream of the scene vertex input reads the entry at that position, as in the CPU
// path). The bucket's count (the number of records written) goes to the counts block; the packet is a DrawIndexedIndirectCount over this range.
//
// F5-2: a record of a clustered mesh (args[3] = rowFirst | rowCount << 16, reserved0 = the entry id) in a job with the cull flag is CULLED: the group tests the
// entry's clusters (rows of the cluster table, 32 bytes: bounding sphere + first index + index count + entry id) against the six frustum planes of the entry's view
// (the view block's VS c0..c3) in the object's space (the record's VS window 2: world rows c33..c35), widened by a margin (the camera's jitter is applied after the
// matrices), and the visible clusters become RUNS (maximal runs of consecutive visible clusters, at most kRunCap per entry, the rest merged into the last: from its
// start to the last visible cluster): one argument record per run, same firstInstance. Everything is guarded: rows beyond the table / of another entry / a range
// that is not the record's draw = the entry's whole range as one record; an entry with no visible cluster writes none. The reference is src/core/cluster_cull.*.
// Job (2 x uint4 per bucket): x first entry, y entries, z argument index of the first record, w count index; x capacity (argument records of the bucket), y flags
// (bit 0: cull, bit 1: side planes only), z flags (bit 0, S-4: a pulled bucket: every record's vertexOffset is 0, the vertex shader adds the base vertex itself from
// its VertexFetch block: D3D12's SV_VertexID ignores the command's base vertex), w 0.

#include "rvk_common.hlsli"

struct OwnedCompactPush {
    uint2 records;    // read: the persistent DrawRecord table (128 bytes per slot)
    uint2 list;       // read: the flush's ref list block (uint2 per entry: slot, view slot)
    uint2 jobs;       // read: 2 x uint4 per bucket
    uint2 dst;        // write: the arguments + counts buffer
    uint countsBase;  // byte offset of the counts in `out`
    uint argsBase;    // byte offset of the first argument record in `out`
    uint slotCount;   // slots of the record table: an entry beyond it is dropped
    uint viewSlots;   // slots of the view table (a view beyond it = not culled)
    uint2 clusters;   // read: the cluster table (ClusterRow, 32 bytes)
    uint2 views;      // read: the view table (ViewBlock2, 640 bytes per slot)
    uint clusterRows; // rows of the cluster table
    uint pad0;
    uint pad1;
    uint pad2;
};
RVK_PUSH(OwnedCompactPush, pc);

static const uint kThreads = 64u;
static const uint kRunCap = 8u;
static const uint kMaxRows = 4096u;
static const float kMargin = 0.02f;
static const float kDepthMargin = 0.001f;
static const uint kViewBlockSize = 640u;

groupshared uint gScan[kThreads];
groupshared uint gTotal;
groupshared uint gKind[kThreads];              // 0 dropped, 1 one record, 2 culled
groupshared uint4 gA[kThreads];                // culled: rowFirst, rowCount, entry id, view slot
groupshared uint4 gB[kThreads];                // culled: indexCount, firstIndex, window 2 (low, high)
groupshared uint2 gRuns[kThreads * kRunCap];   // culled: (first index, index count) per run
groupshared uint gRunN[kThreads];
groupshared uint gVis[kMaxRows / 32u];
groupshared uint gBad;

// first bit >= p of the visibility mask equal to `wantSet`; nRows when none
uint nextBit(uint p, uint nRows, bool wantSet) {
    if (p >= nRows) return nRows;
    const uint words = (nRows + 31u) >> 5;
    uint w = p >> 5;
    uint bits = wantSet ? gVis[w] : ~gVis[w];
    bits &= ~0u << (p & 31u);
    for (;;) {
        if (bits != 0u) return min((w << 5) + firstbitlow(bits), nRows);
        ++w;
        if (w >= words) return nRows;
        bits = wantSet ? gVis[w] : ~gVis[w];
    }
    return nRows;
}

RvkAddr rowAddr(uint rowFirst, uint j) { return rvkAddrAdd(rvkAddr(pc.clusters), (rowFirst + j) * 32u); }

// all 64 threads: entry k of the chunk (gA / gB filled). Ends with gRunN[k] / gRuns[k * kRunCap ...] written and visible to the group.
void cullEntry(uint k, uint gi, bool sidesOnly) {
    const uint4 A = gA[k];
    const uint4 B = gB[k];
    const uint rows = A.y;
    const uint words = (rows + 31u) >> 5;
    for (uint w = gi; w < words; w += kThreads) gVis[w] = 0u;
    if (gi == 0u) gBad = 0u;
    GroupMemoryBarrierWithGroupSync();
    // the planes (every thread makes the same ones): object-to-clip rows r_i = c_i.x * w0 + c_i.y * w1 + c_i.z * w2 + (0, 0, 0, c_i.w)
    const RvkAddr vb = rvkAddrAdd(rvkAddr(pc.views), A.w * kViewBlockSize);
    const RvkAddr wb = rvkAddrAdd(rvkAddr(uint2(B.z, B.w)), 33u * 16u);
    const float4 w0 = rvkRawLoad<float4, 16>(wb);
    const float4 w1 = rvkRawLoad<float4, 16>(rvkAddrAdd(wb, 16u));
    const float4 w2 = rvkRawLoad<float4, 16>(rvkAddrAdd(wb, 32u));
    float4 r[4];
    [unroll] for (uint i = 0u; i < 4u; ++i) {
        const float4 c = rvkRawLoad<float4, 16>(rvkAddrAdd(vb, i * 16u));
        r[i] = c.x * w0 + c.y * w1 + c.z * w2 + float4(0.0f, 0.0f, 0.0f, c.w);
    }
    float4 pl[6];
    pl[0] = r[3] + r[0];
    pl[1] = r[3] - r[0];
    pl[2] = r[3] + r[1];
    pl[3] = r[3] - r[1];
    pl[4] = r[2];
    pl[5] = float4(0.0f, 0.0f, 0.0f, 1.0f);  // the far plane stays open (see src/core/cluster_cull.cpp)
    float nl[6];
    bool ok = true;
    [unroll] for (uint p = 0u; p < 6u; ++p) {
        nl[p] = p == 5u ? 0.0f : sqrt(dot(pl[p].xyz, pl[p].xyz));
        ok = ok && isfinite(nl[p]) && all(isfinite(pl[p]));
    }
    const float len3 = sqrt(dot(r[3].xyz, r[3].xyz));
    ok = ok && isfinite(len3);
    if (sidesOnly) {
        pl[4] = float4(0.0f, 0.0f, 0.0f, 1.0f);
        nl[4] = 0.0f;
    }
    for (uint j = gi; j < rows; j += kThreads) {
        const RvkAddr ra = rowAddr(A.x, j);
        const float4 s = rvkRawLoad<float4, 16>(ra);
        const uint4 u = rvkRawLoad<uint4, 16>(rvkAddrAdd(ra, 16u));
        if (u.z != A.z) {
            uint old;
            InterlockedOr(gBad, 1u, old);
            continue;
        }
        bool outside = false;
        const float wSpan = abs(dot(r[3].xyz, s.xyz) + r[3].w) + s.w * len3;  // |w| over the sphere: the margin is a fraction of it, on either side of the camera plane
        [unroll] for (uint p = 0u; p < 6u; ++p) {
            const float d = dot(pl[p].xyz, s.xyz) + pl[p].w;
            outside = outside || (d < -(s.w * nl[p] + (p < 4u ? kMargin : (p == 4u ? kDepthMargin : 0.0f)) * wSpan));
        }
        if (!outside) {
            uint old;
            InterlockedOr(gVis[j >> 5], 1u << (j & 31u), old);
        }
    }
    GroupMemoryBarrierWithGroupSync();
    if (gi == 0u) {
        uint n = 0u;
        const uint lo = B.y;
        const uint hi = B.y + B.x;
        if (gBad != 0u || !ok) {
            gRuns[k * kRunCap] = uint2(B.y, B.x);  // (the whole range: a row of another entry / a plane that is not finite)
            n = 1u;
        } else {
            uint last = 0xffffffffu;
            for (uint w = words; w > 0u && last == 0xffffffffu; --w) {
                if (gVis[w - 1u] != 0u) last = ((w - 1u) << 5) + firstbithigh(gVis[w - 1u]);
            }
            if (last != 0xffffffffu) {
                uint p = 0u;
                for (uint step = 0u; step < kRunCap && p <= last; ++step) {
                    const uint s = nextBit(p, rows, true);
                    const uint e = (step == kRunCap - 1u) ? last + 1u : nextBit(s + 1u, rows, false);
                    const uint4 ra = rvkRawLoad<uint4, 16>(rvkAddrAdd(rowAddr(A.x, s), 16u));
                    const uint4 rb = rvkRawLoad<uint4, 16>(rvkAddrAdd(rowAddr(A.x, e - 1u), 16u));
                    const uint f = max(ra.x, lo);
                    const uint t = min(rb.x + rb.y, hi);
                    if (t > f) {
                        gRuns[k * kRunCap + n] = uint2(f, t - f);
                        ++n;
                    }
                    p = e;
                }
            }
        }
        gRunN[k] = n;
    }
    GroupMemoryBarrierWithGroupSync();
}

[numthreads(64, 1, 1)]
void CSMain(uint3 gid : SV_GroupID, uint gi : SV_GroupIndex) {
    const uint4 job = rvkRawLoad<uint4, 16>(rvkAddrAdd(rvkAddr(pc.jobs), gid.x * 32u));
    const uint4 job1 = rvkRawLoad<uint4, 16>(rvkAddrAdd(rvkAddr(pc.jobs), gid.x * 32u + 16u));
    const uint cap = job1.x;
    const bool cullOn = (job1.y & 1u) != 0u && (pc.clusters.x | pc.clusters.y) != 0u && (pc.views.x | pc.views.y) != 0u;
    const bool sidesOnly = (job1.y & 2u) != 0u;
    if (gi == 0u) gTotal = 0u;
    GroupMemoryBarrierWithGroupSync();
    for (uint base = 0u; base < job.y; base += kThreads) {
        const uint i = base + gi;
        uint kind = 0u;
        uint3 a = uint3(0u, 0u, 0u);
        uint entry = 0u;
        if (i < job.y) {
            entry = job.x + i;
            const uint2 ent = rvkRawLoad<uint2, 8>(rvkAddrAdd(rvkAddr(pc.list), entry * 8u));
            if (ent.x < pc.slotCount) {
                const RvkAddr rec = rvkAddrAdd(rvkAddr(pc.records), ent.x * 128u);
                const uint4 w = rvkRawLoad<uint4, 16>(rvkAddrAdd(rec, 64u));    // the DrawBlock address (draw: low, high), prevVs
                const uint4 g = rvkRawLoad<uint4, 16>(rvkAddrAdd(rec, 112u));   // args: indexCount, firstIndex, vertexOffset, rows
                if ((w.x | w.y) != 0u && g.x != 0u) {
                    kind = 1u;
                    a = g.xyz;
                    if ((job1.z & 1u) != 0u) a.z = 0u;
                    const uint rowCount = g.w >> 16;
                    const uint rowFirst = g.w & 0xffffu;
                    if (cullOn && rowCount != 0u && rowCount <= kMaxRows && rowFirst + rowCount <= pc.clusterRows && ent.y < pc.viewSlots) {
                        const uint2 win2 = rvkRawLoad<uint2, 8>(rvkAddrAdd(rec, 16u));  // VS window 2 (c33..c35 = the world rows at + 33 x 16)
                        const uint id = rvkRawLoad<uint, 4>(rvkAddrAdd(rec, 108u));
                        if ((win2.x | win2.y) != 0u) {
                            kind = 2u;
                            gA[gi] = uint4(rowFirst, rowCount, id, ent.y);
                            gB[gi] = uint4(g.x, g.y, win2.x, win2.y);
                        }
                    }
                }
            }
        }
        gKind[gi] = kind;
        GroupMemoryBarrierWithGroupSync();
        for (uint k = 0u; k < kThreads; ++k) {
            if (gKind[k] == 2u) cullEntry(k, gi, sidesOnly);
        }
        const uint n = kind == 1u ? 1u : (kind == 2u ? gRunN[gi] : 0u);
        gScan[gi] = n;
        GroupMemoryBarrierWithGroupSync();
        [unroll] for (uint off = 1u; off < kThreads; off <<= 1u) {
            uint v = gScan[gi];
            if (gi >= off) v += gScan[gi - off];
            GroupMemoryBarrierWithGroupSync();
            gScan[gi] = v;
            GroupMemoryBarrierWithGroupSync();
        }
        const uint rank0 = gTotal + gScan[gi] - n;
        if (kind == 1u) {
            if (rank0 < cap) {
                const RvkAddr dst = rvkAddrAdd(rvkAddr(pc.dst), pc.argsBase + (job.z + rank0) * 20u);
                rvkRwStore<uint, 4>(dst, a.x);
                rvkRwStore<uint, 4>(rvkAddrAdd(dst, 4u), 1u);
                rvkRwStore<uint, 4>(rvkAddrAdd(dst, 8u), a.y);
                rvkRwStore<uint, 4>(rvkAddrAdd(dst, 12u), a.z);
                rvkRwStore<uint, 4>(rvkAddrAdd(dst, 16u), entry);
            }
        } else if (kind == 2u) {
            for (uint q = 0u; q < n; ++q) {
                if (rank0 + q < cap) {
                    const uint2 run = gRuns[gi * kRunCap + q];
                    const RvkAddr dst = rvkAddrAdd(rvkAddr(pc.dst), pc.argsBase + (job.z + rank0 + q) * 20u);
                    rvkRwStore<uint, 4>(dst, run.y);
                    rvkRwStore<uint, 4>(rvkAddrAdd(dst, 4u), 1u);
                    rvkRwStore<uint, 4>(rvkAddrAdd(dst, 8u), run.x);
                    rvkRwStore<uint, 4>(rvkAddrAdd(dst, 12u), a.z);
                    rvkRwStore<uint, 4>(rvkAddrAdd(dst, 16u), entry);
                }
            }
        }
        GroupMemoryBarrierWithGroupSync();
        if (gi == kThreads - 1u) gTotal += gScan[kThreads - 1u];
        GroupMemoryBarrierWithGroupSync();
    }
    if (gi == 0u) rvkRwStore<uint, 4>(rvkAddrAdd(rvkAddr(pc.dst), pc.countsBase + job.w * 4u), min(gTotal, cap));
}
