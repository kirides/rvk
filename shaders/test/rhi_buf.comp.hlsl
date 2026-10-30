// RHI backend test (tools/rhitest): buffers a shader writes (rhi::Buffer::shaderRefRw) and atomics, one HLSL text for both backends.
// Thread i (of `count`): data[i] = 2i + 1, reads its own word back and writes data[count + i] = that + 1; counters[0] counts the threads, counters[1]
// sums the values atomicAdd returned (0 + 1 + ... + count - 1: every thread saw a different one), counters[2 + (i & 7)] sums i per bucket.
#include "rvk_common.hlsli"

struct BufPush {
    uint2 data;      // Buffer::shaderRefRw of the data buffer (2 * count words)
    uint2 counters;  // ... of the counter buffer (10 words)
    uint count;
    uint pad0;
    uint pad1;
    uint pad2;
};
RVK_PUSH(BufPush, pc);

[numthreads(64, 1, 1)]
void CSMain(uint3 id : SV_DispatchThreadID) {
    const uint i = id.x;
    if (i >= pc.count) return;
    RVK_BUF_STORE(pc.data, i * 4u, i * 2u + 1u);
    const uint v = RVK_BUF_LOAD(pc.data, i * 4u);
    RVK_BUF_STORE(pc.data, (pc.count + i) * 4u, v + 1u);
    const uint orig = RVK_BUF_ADD(pc.counters, 0u, 1u);
    RVK_BUF_ADD(pc.counters, 4u, orig);
    RVK_BUF_ADD(pc.counters, 8u + (i & 7u) * 4u, i);
}
