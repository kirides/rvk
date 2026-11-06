// RHI backend test (tools/rhitest): a compute pass writes the inputs of GPU-driven indexed draws (frame-ownership.md 4): 7 VkDrawIndexedIndirectCommand
// records (20 bytes apart, from byte 0 of the args reference), the draw count (6: the 7th record lies beyond it) and the instance-rate stream of
// uint2 refs (tile, colour id), one per instance slot. The tables are mirrored by testIndirectPull (tools/rhitest/main.cpp): change both together.
#include "rvk_common.hlsli"

struct IndPush {
    uint2 args;   // Buffer::shaderRefRw of the record array
    uint2 count;  // ... of the count word
    uint2 refs;   // ... of the ref stream
    uint pad0;
    uint pad1;
};
RVK_PUSH(IndPush, pc);

// record i: instances, first instance slot, template (its indices start at 9 * tpl, values 4 * tpl + 0..3), half = the lower-left triangle only (firstIndex 6, 3 indices)
static const uint kInstances[7] = {1, 2, 0, 1, 3, 1, 1};
static const uint kFirst[7] = {0, 1, 3, 3, 4, 8, 9};
static const uint kTemplate[7] = {0, 1, 2, 1, 0, 2, 1};
static const uint kHalf[7] = {0, 0, 0, 1, 1, 0, 0};
// slot s: the tile it draws into (slot 7 is never named by a record: a junk ref; slot 9 is drawn by record 6, beyond the count)
static const uint kTile[10] = {0, 1, 2, 3, 4, 5, 6, 3, 7, 8};

[numthreads(16, 1, 1)]
void CSMain(uint3 id : SV_DispatchThreadID) {
    const uint i = id.x;
    if (i < 7u) {
        const uint o = i * 20u;
        RVK_BUF_STORE(pc.args, o + 0u, kHalf[i] != 0u ? 3u : 6u);  // indexCount
        RVK_BUF_STORE(pc.args, o + 4u, kInstances[i]);              // instanceCount
        RVK_BUF_STORE(pc.args, o + 8u, kTemplate[i] * 9u + (kHalf[i] != 0u ? 6u : 0u));  // firstIndex (absolute indices: 9 per template)
        RVK_BUF_STORE(pc.args, o + 12u, 0u);                        // vertexOffset: 0 (see rhi_ind.vert.hlsl)
        RVK_BUF_STORE(pc.args, o + 16u, kFirst[i]);                 // firstInstance
    }
    if (i < 10u) {
        RVK_BUF_STORE(pc.refs, i * 8u, kTile[i]);
        RVK_BUF_STORE(pc.refs, i * 8u + 4u, i + 1u);  // colour id
    }
    if (i == 0u) RVK_BUF_STORE(pc.count, 0u, 6u);
}
