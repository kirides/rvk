// F2c-4c GPU compaction of the persistent owned draws (src/gpuscene/owned_compact.*, docs/research/frame-ownership.md 7.32).
// One group per bucket of a flush: the bucket's entries (slot, view slot; 8 bytes each, in the engine's order) of the list block are read in chunks of 64, each
// entry's record (DrawRecord at records + slot x 128) must hold a draw address and a non-zero index count (the same guards as the CPU's), and the valid ones are
// compacted IN ORDER (an inclusive scan per chunk) into VkDrawIndexedIndirectCommand records {indexCount, 1, firstIndex, vertexOffset, firstInstance}, with
// firstInstance = the entry's position in the list block (the instance-rate ref stream of the scene vertex input reads the entry at that position, as in the CPU
// path). The bucket's count (the number of records written) goes to the counts block; the packet is a DrawIndexedIndirectCount over this range.
// Job (uint4 per bucket): x first entry, y entries, z argument index of the first record, w count index.

#include "rvk_common.hlsli"

struct OwnedCompactPush {
    uint2 records;    // read: the persistent DrawRecord table (128 bytes per slot)
    uint2 list;       // read: the flush's ref list block (uint2 per entry: slot, view slot)
    uint2 jobs;       // read: uint4 per bucket
    uint2 dst;        // write: the arguments + counts buffer
    uint countsBase;  // byte offset of the counts in `out`
    uint argsBase;    // byte offset of the first argument record in `out`
    uint slotCount;   // slots of the record table: an entry beyond it is dropped
    uint pad0;
};
RVK_PUSH(OwnedCompactPush, pc);

static const uint kThreads = 64u;
groupshared uint gScan[kThreads];
groupshared uint gTotal;

[numthreads(64, 1, 1)]
void CSMain(uint3 gid : SV_GroupID, uint gi : SV_GroupIndex) {
    const uint4 job = rvkRawLoad<uint4, 16>(rvkAddrAdd(rvkAddr(pc.jobs), gid.x * 16u));
    if (gi == 0u) gTotal = 0u;
    GroupMemoryBarrierWithGroupSync();
    for (uint base = 0u; base < job.y; base += kThreads) {
        const uint i = base + gi;
        uint valid = 0u;
        uint3 a = uint3(0u, 0u, 0u);
        uint entry = 0u;
        if (i < job.y) {
            entry = job.x + i;
            const uint2 ent = rvkRawLoad<uint2, 8>(rvkAddrAdd(rvkAddr(pc.list), entry * 8u));
            if (ent.x < pc.slotCount) {
                const RvkAddr rec = rvkAddrAdd(rvkAddr(pc.records), ent.x * 128u);
                const uint4 w = rvkRawLoad<uint4, 16>(rvkAddrAdd(rec, 64u));    // the DrawBlock address (draw: low, high), prevVs
                const uint4 g = rvkRawLoad<uint4, 16>(rvkAddrAdd(rec, 112u));   // args: indexCount, firstIndex, vertexOffset
                if ((w.x | w.y) != 0u && g.x != 0u) {
                    valid = 1u;
                    a = g.xyz;
                }
            }
        }
        gScan[gi] = valid;
        GroupMemoryBarrierWithGroupSync();
        [unroll] for (uint off = 1u; off < kThreads; off <<= 1u) {
            uint v = gScan[gi];
            if (gi >= off) v += gScan[gi - off];
            GroupMemoryBarrierWithGroupSync();
            gScan[gi] = v;
            GroupMemoryBarrierWithGroupSync();
        }
        if (valid != 0u) {
            const uint rank = gTotal + gScan[gi] - 1u;
            const RvkAddr dst = rvkAddrAdd(rvkAddr(pc.dst), pc.argsBase + (job.z + rank) * 20u);
            rvkRwStore<uint, 4>(dst, a.x);
            rvkRwStore<uint, 4>(rvkAddrAdd(dst, 4u), 1u);
            rvkRwStore<uint, 4>(rvkAddrAdd(dst, 8u), a.y);
            rvkRwStore<uint, 4>(rvkAddrAdd(dst, 12u), a.z);
            rvkRwStore<uint, 4>(rvkAddrAdd(dst, 16u), entry);
        }
        GroupMemoryBarrierWithGroupSync();
        if (gi == kThreads - 1u) gTotal += gScan[kThreads - 1u];
        GroupMemoryBarrierWithGroupSync();
    }
    if (gi == 0u) rvkRwStore<uint, 4>(rvkAddrAdd(rvkAddr(pc.dst), pc.countsBase + job.w * 4u), gTotal);
}
