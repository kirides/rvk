// RHI backend test (tools/rhitest): inline ray queries on both backends from one text; the port of test/rt_query.comp.hlsl (src/vk/raytracing.cpp
// selfTest) with the TLAS reached by its heap slot (rhi::AccelerationStructure::tlasSlot) and the result written through a read-write buffer reference.
// Orthographic rays along +z; non-opaque candidates pass a stand-in alpha test (barycentric u < 0.5).
// Output per pixel: 0 = miss, else bit 31 | primitive << 16 | round(t * 4) << 8 | (custom index + 1).
#include "rvk_common.hlsli"

RVK_DECLARE_TLAS

struct RtPush {
    uint2 outBuf;  // Buffer::shaderRefRw of the output (width x height uints)
    uint tlas;     // AccelerationStructure::tlasSlot
    uint width;
    uint height;
    uint pad0;
    uint pad1;
    uint pad2;
};
RVK_PUSH(RtPush, pc);

[numthreads(8, 8, 1)]
void CSMain(uint3 id : SV_DispatchThreadID) {
    if (id.x >= pc.width || id.y >= pc.height) return;
    const float2 p = (float2(id.xy) + 0.5) / float2(pc.width, pc.height) * 2.0 - 1.0;

    RayDesc ray;
    ray.Origin = float3(p, -10.0);
    ray.TMin = 0.0;
    ray.Direction = float3(0.0, 0.0, 1.0);
    ray.TMax = 100.0;
    RayQuery<RAY_FLAG_NONE> rq;
    rq.TraceRayInline(RVK_TLAS(pc.tlas), RAY_FLAG_NONE, 0xFF, ray);
    while (rq.Proceed()) {
        if (rq.CandidateType() == CANDIDATE_NON_OPAQUE_TRIANGLE) {
            const float2 bary = rq.CandidateTriangleBarycentrics();
            if (bary.x < 0.5) rq.CommitNonOpaqueTriangleHit();
        }
    }
    uint r = 0u;
    if (rq.CommittedStatus() == COMMITTED_TRIANGLE_HIT) {
        const uint idx = rq.CommittedInstanceID();
        const uint t4 = uint(round(rq.CommittedRayT() * 4.0));
        const uint prim = rq.CommittedPrimitiveIndex();
        r = (1u << 31) | (prim << 16) | (t4 << 8) | (idx + 1u);
    }
    RVK_BUF_STORE(pc.outBuf, (id.y * pc.width + id.x) * 4u, r);
}
