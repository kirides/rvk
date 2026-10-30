// DXIL parity test (tools/rhitest/parity.cpp, docs/research/hlsl-rhi.md "DXIL parity (S1)"): the production header code that reaches memory through
// references, run on both backends from this one text and compared.
//   - the GI probe lookup (rvk_gi.hlsli rvkGiLookup): frame block (push constant reference) -> volume header (a reference stored in the frame block) ->
//     the probes and the depth moments (addresses stored in the header), with a dynamically indexed cascade array and 7-register probes
//   - the RT geometry record fetch (rvk_rt.hlsli): a record by index from the frame block's record table, 16- and 32-bit indices at 2-byte alignment,
//     vertex attributes of several declaration types by (address + stride), the material segment table (binary search through references)
// Results are written to a read-write buffer (rvkBufStore): 64 words per invocation.
#include "rvk_rt.hlsli"
#include "rvk_gi.hlsli"

struct ParityPush {
    RVK_PASS_PUSH_HEADER
    uint2 outBuf;  // Buffer::shaderRefRw of the results
    uint count;    // invocations
    uint pad0;
};
RVK_PUSH(ParityPush, pc);

static uint gOut;

void putU(uint v) {
    RVK_BUF_STORE(pc.outBuf, gOut, v);
    gOut += 4u;
}
void putF(float v) { putU(asuint(v)); }
void putF2(float2 v) { putF(v.x); putF(v.y); }
void putF3(float3 v) { putF(v.x); putF(v.y); putF(v.z); }
void putF4(float4 v) { putF(v.x); putF(v.y); putF(v.z); putF(v.w); }

[numthreads(32, 1, 1)]
void CSMain(uint3 id : SV_DispatchThreadID) {
    const uint i = id.x;
    if (i >= pc.count) return;
    gOut = i * 64u * 4u;
    RvkFrame f = rvkLoadFrame(pc.frame);

    // ---- the GI lookup (a point inside the grid, a normal and a view direction that change per invocation)
    const float j = float(i & 15u);  // (the grid covers 0 .. 300: the points stay inside it)
    float3 P = float3(20.0 + 13.0 * j, 30.0 + 9.0 * j, 40.0 + 11.0 * j);
    float3 N = normalize(float3(0.3 + 0.1 * float(i), 1.0, -0.2 * float(i)));
    float3 V = normalize(float3(0.5, 0.4 + 0.05 * float(i), 0.7));
    float4 regs[7];
    uint cascade;
    bool hit = rvkGiLookup(f, P, N, V, regs, cascade);
    putU(hit ? 1u : 0u);
    putU(cascade);
    for (int k = 0; k < 7; ++k) putF4(regs[k]);  // 2 + 28 words

    // ---- the RT record fetch
    RvkRtRecord r = rvkRtRecord(f, i & 1u);
    const uint prim = i % 3u;
    const float2 bary = float2(0.2 + 0.05 * float(i & 3u), 0.3);
    RvkRtSurface s = rvkRtSurface(r, prim, bary);
    putF3(s.position);
    putF3(s.normal);
    putF2(s.uv0);
    putF2(s.uv1);
    putF2(s.uvLightmap);
    putF3(s.geometricNormal);  // 3 + 3 + 2 + 2 + 2 + 3 = 15 words
    rvkRtApplySegment(r, prim);
    putU(r.albedoTex);
    putU(r.albedoSampler);
    putU(r.alpha);
    putU(r.uvSet);
    putF4(r.uvMap0);
    putF4(r.uvMap1);  // 12 words
    uint3 t = rvkRtTriangle(r, prim);
    putU(t.x);
    putU(t.y);
    putU(t.z);
    putU(r.flags);
    putF2(rvkRtMaterialUv(r, t.y));  // 3 + 1 + 2 = 6 words
}
