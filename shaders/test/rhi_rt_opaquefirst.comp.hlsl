// RHI backend test (tools/rhitest/parity_opaquefirst.inl): the opaque-first traces of rvk_rt_trace.hlsli (rvkRtTraceClosestOpaqueFirst /
// rvkRtVisibilityOpaqueFirst, used by water.rt) against the one-query forms (rvkRtTraceClosest / rvkRtVisibility) on the same rays of a synthetic
// TLAS mixing opaque and alpha-tested instances (the production record fetch + alpha test: rvkRtCandidatePasses on a checker texture).
// Per ray, 40 words: for the flags 0 and RVK_RT_TRACE_NO_DYNAMIC, the one-query closest hit (hit, t bits, record, primitive, bary) and the
// opaque-first one (6 + 6 words), then the four visibilities (one-query / opaque-first per flags), then the ray-t semantics (28..31: forced opaque
// t, forced non-opaque candidate / committed t through rvkRay, the committed t of a plain RayDesc), then the size checks of a lane-varying bindless
// index (32..36: wrong inside the candidate loop, checked, wrong outside it, the width returned, the slot).
#include "rvk_rt_trace.hlsli"

struct RtRayIn {
    float4 originMin;  // origin, tMin
    float4 dirMax;     // direction, tMax
};

struct OpaqueFirstPush {
    RVK_PASS_PUSH_HEADER
    uint2 rays;    // RtRayIn[count]
    uint2 outBuf;  // Buffer::shaderRefRw of the results
    uint count;
    uint pad0;
    uint pad1;
    uint pad2;
};
RVK_PUSH(OpaqueFirstPush, pc);

static uint gOut;
void putU(uint v) {
    RVK_BUF_STORE(pc.outBuf, gOut, v);
    gOut += 4u;
}
void putHit(RvkRtHit h) {
    putU(h.hit ? 1u : 0u);
    putU(h.hit ? asuint(h.t) : 0u);
    putU(h.hit ? h.record : 0xFFFFFFFFu);
    putU(h.hit ? h.primitive : 0xFFFFFFFFu);
    putU(h.hit ? asuint(h.bary.x) : 0u);
    putU(h.hit ? asuint(h.bary.y) : 0u);
}

[numthreads(64, 1, 1)]
void CSMain(uint3 id : SV_DispatchThreadID) {
    const uint i = id.x;
    if (i >= pc.count) return;
    gOut = i * 40u * 4u;
    RvkFrame f = rvkLoadFrame(pc.frame);
    RtRayIn r = rvkPtr<RtRayIn, 16>(rvkPairAdd(pc.rays, i * 32u)).Get();
    const float3 o = r.originMin.xyz, d = r.dirMax.xyz;
    const float tMin = r.originMin.w, tMax = r.dirMax.w;
    const float spread = 0.002;
    float vis[4];
    for (uint k = 0u; k < 2u; ++k) {
        const uint flags = k == 0u ? 0u : RVK_RT_TRACE_NO_DYNAMIC;
        putHit(rvkRtTraceClosest(f, o, d, tMin, tMax, 0.0, spread, flags));
        putHit(rvkRtTraceClosestOpaqueFirst(f, o, d, tMin, tMax, 0.0, spread, flags));
        vis[k * 2u] = rvkRtVisibility(f, o, d, tMin, tMax, 0.0, spread, flags);
        vis[k * 2u + 1u] = rvkRtVisibilityOpaqueFirst(f, o, d, tMin, tMax, 0.0, spread, flags);
    }
    for (uint k = 0u; k < 4u; ++k) putU(asuint(vis[k]));
    // raw ray-t semantics (words 28..31): every instance forced opaque (closest t) and every instance forced non-opaque with every candidate
    // committed (the first candidate's CandidateTriangleRayT, the committed RayT): the three must agree, whatever tMin is
    float tOpaque = -1.0, tCandidate = -1.0, tCommitted = -1.0;
    uint sizeBad = 0u, sizeChecked = 0u;  // texture sizes (GetDimensions of a non-uniform bindless index) inside the candidate loop: the record's
                                          // instanceId holds its texture's width
    {
        RayQuery<RAY_FLAG_NONE> q;
        q.TraceRayInline(rvkRtTlas(f), RAY_FLAG_FORCE_OPAQUE, 0xFFu, rvkRay(o, tMin, d, tMax));
        while (q.Proceed()) {
        }
        if (q.CommittedStatus() == COMMITTED_TRIANGLE_HIT) tOpaque = q.CommittedRayT() + rvkRayT0(tMin);
    }
    {
        RayQuery<RAY_FLAG_NONE> q;
        q.TraceRayInline(rvkRtTlas(f), RAY_FLAG_FORCE_NON_OPAQUE, 0xFFu, rvkRay(o, tMin, d, tMax));
        while (q.Proceed()) {
            if (q.CandidateType() != CANDIDATE_NON_OPAQUE_TRIANGLE) continue;
            float t = q.CandidateTriangleRayT() + rvkRayT0(tMin);
            if (tCandidate < 0.0 || t < tCandidate) tCandidate = t;
            RvkRtRecord rc = rvkRtRecord(f, q.CandidateInstanceID() + q.CandidateGeometryIndex());
            ++sizeChecked;
            if (uint(rvkSize2D(rc.albedoTex).x) != rc.instanceId) ++sizeBad;
            q.CommitNonOpaqueTriangleHit();
        }
        if (q.CommittedStatus() == COMMITTED_TRIANGLE_HIT) tCommitted = q.CommittedRayT() + rvkRayT0(tMin);
    }
    // the same non-opaque query with the plain RayDesc (TMin as given: the platform behaviour rvkRay works around, reported as a note)
    float tPlain = -1.0;
    {
        RayDesc ray;
        ray.Origin = o;
        ray.TMin = tMin;
        ray.Direction = d;
        ray.TMax = tMax;
        RayQuery<RAY_FLAG_NONE> q;
        q.TraceRayInline(rvkRtTlas(f), RAY_FLAG_FORCE_NON_OPAQUE, 0xFFu, ray);
        while (q.Proceed()) {
            if (q.CandidateType() == CANDIDATE_NON_OPAQUE_TRIANGLE && (pc.passFlags == 0u || q.CandidateTriangleRayT() >= 0.0)) q.CommitNonOpaqueTriangleHit();
        }
        if (q.CommittedStatus() == COMMITTED_TRIANGLE_HIT) tPlain = q.CommittedRayT();
    }
    putU(asuint(tOpaque));
    putU(asuint(tCandidate));
    putU(asuint(tCommitted));
    putU(asuint(tPlain));
    // words 32..34: the size checks inside the loop (mismatches, candidates) and outside it (the record of this lane: i % 9)
    RvkRtRecord rl = rvkRtRecord(f, i % 9u);
    putU(sizeBad);
    putU(sizeChecked);
    putU(uint(rvkSize2D(rl.albedoTex).x) != rl.instanceId ? 1u : 0u);
    putU(uint(rvkSize2D(rl.albedoTex).x));
    putU(rl.albedoTex);
}
