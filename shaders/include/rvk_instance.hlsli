// The GPU scene's instance table (src/gpuscene/instance_buffer.h InstanceRecord, 160 bytes): one record per cached-in geometry entity, kept on the
// GPU by batched uploads from engine/scene_table. Reach a record with rvkLoadInstance(tableRef, index) (S1 rule: raw loads from an address pair).
#pragma once

#include "rvk_common.hlsli"

struct RvkInstance {
    float4 world[4];  // the engine's row-major world matrix (row vectors)
    float4 prev[4];   // the matrix before its last change
    uint4 info;       // generation, version, entity flags, live (1 = cached in)
    uint4 moved;      // x = low 32 bits of the frame of the last change
};
static const uint RVK_INSTANCE_STRIDE = 160u;

RvkInstance rvkLoadInstance(uint2 table, uint index) {
    const RvkAddr a = rvkAddrAdd(rvkAddr(table), index * RVK_INSTANCE_STRIDE);
    RvkInstance r;
    [unroll] for (uint k = 0; k < 4; ++k) {
        r.world[k] = rvkRawLoad<float4, 16>(rvkAddrAdd(a, k * 16u));
        r.prev[k] = rvkRawLoad<float4, 16>(rvkAddrAdd(a, 64u + k * 16u));
    }
    r.info = rvkRawLoad<uint4, 16>(rvkAddrAdd(a, 128u));
    r.moved = rvkRawLoad<uint4, 16>(rvkAddrAdd(a, 144u));
    return r;
}
