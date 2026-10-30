// Ray query self-test (src/vk/raytracing.cpp rt::selfTest, rvk_vktest --rt, [debug] rt_selftest): orthographic rays along +z
// through a TLAS (a push descriptor: set 0 binding 0); non-opaque candidates pass a stand-in alpha test (barycentric u < 0.5).
// Output per pixel: 0 = miss, else bit 31 | primitive << 16 | round(t * 4) << 8 | (custom index + 1).

#include "rvk_abi.hlsli"

[[vk::binding(0, 0)]] RaytracingAccelerationStructure tlas : register(t0, space0);

struct Push {
    uint2 outBuf;  // device address of the output (width x height uints)
    uint width;
    uint height;
};
RVK_PUSH(Push, pc);

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
    rq.TraceRayInline(tlas, RAY_FLAG_NONE, 0xFF, ray);
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
    rvkRwStore<uint, 4>(rvkAddrAdd(rvkAddr(pc.outBuf), (id.y * pc.width + id.x) * 4u), r);
}
