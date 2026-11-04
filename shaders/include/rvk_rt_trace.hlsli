// Tracing the RT scene with materials (M9 P2, docs/research/rt-reflections.md 3.2; port of rvk_rt_trace.glsl): the closest hit with the alpha
// test of alpha-tested instances resolved in the ray query's candidate loop (the record's D3D9 alpha test on the diffuse / opacity alpha, mip
// from a ray cone), and the hit's surface (albedo, normals, uv). For every RT consumer (the water's reflections, the debug view, GI, the RT
// sun shadows).
//
//   RvkRtHit h = rvkRtTraceClosest(frame, origin, dir, tMin, tMax, coneWidth0, coneSpread, RVK_RT_TRACE_*);
//   if (h.hit) { RvkRtHitSurface s = rvkRtHitSurface(frame, h, dir, coneWidth0, coneSpread); ... s.albedo ... }
//
// Ray cone: width(t) = coneWidth0 + coneSpread * t (world units); camera rays: coneWidth0 = 0, coneSpread = the pixel's
// angle (2 / (proj[1][1] * height)); a reflection: the width at the reflector plus the spread its roughness / curvature adds.
// The ray-cone LOD follows GD3D11's D3D12 RT scene (Shaders/D3D12/include/RtScene.hlsl, the user's own code).
#pragma once

#include "rvk_rt.hlsli"

static const uint RVK_RT_TRACE_OPAQUE = 1u;       // alpha-tested instances count as solid (cheap tier: no candidate loop work)
static const uint RVK_RT_TRACE_NO_CUTOUTS = 2u;   // alpha-tested instances are left out (cull mask)
static const uint RVK_RT_TRACE_NO_DYNAMIC = 4u;   // characters are left out (cull mask)

// the cull mask of RVK_RT_TRACE_* flags
uint rvkRtTraceMask(uint flags) {
    uint mask = 0xFFu;
    if ((flags & RVK_RT_TRACE_NO_CUTOUTS) != 0u) mask &= ~RVK_RT_MASK_ALPHA_TESTED;
    if ((flags & RVK_RT_TRACE_NO_DYNAMIC) != 0u) mask &= ~RVK_RT_MASK_DYNAMIC;
    return mask;
}

struct RvkRtHit {
    bool hit;
    float t;
    uint record;         // geometry record index (instanceCustomIndex + geometryIndex)
    uint primitive;
    float2 bary;
    bool frontFace;
    float3x4 objectToWorld;
    float3x4 worldToObject;
};

float3x4 rvkIdentity3x4() { return float3x4(1.0, 0.0, 0.0, 0.0, 0.0, 1.0, 0.0, 0.0, 0.0, 0.0, 1.0, 0.0); }

RayDesc rvkRay(float3 origin, float tMin, float3 dir, float tMax) {
    RayDesc ray;
    ray.Origin = origin;
    ray.TMin = tMin;
    ray.Direction = dir;
    ray.TMax = tMax;
    return ray;
}

RvkRtHit rvkRtTraceClosest(RvkFrame f, float3 origin, float3 dir, float tMin, float tMax, float coneWidth0, float coneSpread, uint flags) {
    RvkRtHit h;
    h.hit = false;
    h.t = tMax;
    h.record = 0u;
    h.primitive = 0u;
    h.bary = float2(0.0, 0.0);
    h.frontFace = true;
    h.objectToWorld = rvkIdentity3x4();
    h.worldToObject = rvkIdentity3x4();
    if (!rvkRtValid(f)) return h;
    uint mask = rvkRtTraceMask(flags);
    uint rayFlags = (flags & RVK_RT_TRACE_OPAQUE) != 0u ? RAY_FLAG_FORCE_OPAQUE : RAY_FLAG_NONE;
    RayQuery<RAY_FLAG_NONE> rq;
    rq.TraceRayInline(rvkRtTlas(f), rayFlags, mask, rvkRay(origin, tMin, dir, tMax));
    while (rq.Proceed()) {
        if (rq.CandidateType() != CANDIDATE_NON_OPAQUE_TRIANGLE) continue;
        uint index = rq.CandidateInstanceID() + rq.CandidateGeometryIndex();
        RvkRtRecord r = rvkRtRecord(f, index);
        float t = rq.CandidateTriangleRayT();
        if (rvkRtCandidatePasses(r, rq.CandidatePrimitiveIndex(), rq.CandidateTriangleBarycentrics(), rq.CandidateObjectToWorld3x4(),
                                 coneWidth0 + coneSpread * t)) {
            rq.CommitNonOpaqueTriangleHit();
        }
    }
    if (rq.CommittedStatus() != COMMITTED_TRIANGLE_HIT) return h;
    h.hit = true;
    h.t = rq.CommittedRayT();
    h.record = rq.CommittedInstanceID() + rq.CommittedGeometryIndex();
    h.primitive = rq.CommittedPrimitiveIndex();
    h.bary = rq.CommittedTriangleBarycentrics();
    h.frontFace = rq.CommittedTriangleFrontFace();
    h.objectToWorld = rq.CommittedObjectToWorld3x4();
    h.worldToObject = rq.CommittedWorldToObject3x4();
    return h;
}

// visibility along a ray (1 = nothing in [tMin, tMax]: shadow rays); the first confirmed hit ends the query. Alpha-tested
// candidates as in rvkRtTraceClosest (RVK_RT_TRACE_OPAQUE: solid, the cheap shadow)
float rvkRtVisibility(RvkFrame f, float3 origin, float3 dir, float tMin, float tMax, float coneWidth0, float coneSpread, uint flags) {
    if (!rvkRtValid(f)) return 1.0;
    uint mask = rvkRtTraceMask(flags);
    uint rayFlags = RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH | ((flags & RVK_RT_TRACE_OPAQUE) != 0u ? RAY_FLAG_FORCE_OPAQUE : RAY_FLAG_NONE);
    RayQuery<RAY_FLAG_NONE> rq;
    rq.TraceRayInline(rvkRtTlas(f), rayFlags, mask, rvkRay(origin, tMin, dir, tMax));
    while (rq.Proceed()) {
        if (rq.CandidateType() != CANDIDATE_NON_OPAQUE_TRIANGLE) continue;
        uint index = rq.CandidateInstanceID() + rq.CandidateGeometryIndex();
        RvkRtRecord r = rvkRtRecord(f, index);
        float t = rq.CandidateTriangleRayT();
        if (rvkRtCandidatePasses(r, rq.CandidatePrimitiveIndex(), rq.CandidateTriangleBarycentrics(), rq.CandidateObjectToWorld3x4(),
                                 coneWidth0 + coneSpread * t)) {
            rq.CommitNonOpaqueTriangleHit();
        }
    }
    return rq.CommittedStatus() == COMMITTED_NOTHING ? 1.0 : 0.0;
}

struct RvkRtHitSurface {
    float4 albedo;           // the diffuse at the ray cone's mip, as sampled (rgb, a = its alpha); the record's colour without a texture
    float3 position;         // world
    float3 normal;           // world, interpolated vertex normal, turned towards the ray's origin (geometric when there is none)
    float3 geometricNormal;  // world, towards the ray's origin
    float2 uv;               // the diffuse coordinate (the material's uv map applied)
    float2 uvLightmap;       // TEXCOORD6 (lightmapped level meshes)
    float lod;               // the mip level the albedo was sampled at
    uint flags;              // record flags (RVK_RT_*: kind << 8, views << 16)
    uint uvSet;              // record uvSet (RVK_RT_UV_*)
    uint instanceId;
    // material pack (enh/material_pack.h, docs/research/material-pack.md): defaults without an entry: rough dielectric, no emission
    float roughness;         // 0 mirror .. 1
    float metalness;
    float occlusion;
    float emissive;          // 0..1 mask: the albedo is emitted (ORM alpha)
    uint packWord;           // the entry (0 = none)
};

// Terrain layers (a level's ground is several coplanar copies of its triangles, one per ground texture, each fading out through its vertex colour alpha; the
// engine draws them in order with alpha blending): the layer hits of the ray within a few units of the closest hit, blended in record order (the engine's
// draw order), each weighted by its interpolated vertex alpha; the lowest layer is the base.
static const float RVK_RT_LAYER_DEPTH = 6.0;
static const int RVK_RT_LAYER_MAX = 4;

float4 rvkRtBlendLayers(RvkFrame f, RvkRtHit h, float3 origin, float3 dir, float lod, float4 closest) {
    uint rec[RVK_RT_LAYER_MAX];
    uint prim[RVK_RT_LAYER_MAX];
    float2 bary[RVK_RT_LAYER_MAX];
    int n = 0;
    RayQuery<RAY_FLAG_NONE> rq;
    rq.TraceRayInline(rvkRtTlas(f), RAY_FLAG_FORCE_NON_OPAQUE, 0xFFu, rvkRay(origin, max(h.t - RVK_RT_LAYER_DEPTH, 0.0), dir, h.t + RVK_RT_LAYER_DEPTH));
    while (rq.Proceed()) {
        if (rq.CandidateType() != CANDIDATE_NON_OPAQUE_TRIANGLE) continue;
        uint index = rq.CandidateInstanceID() + rq.CandidateGeometryIndex();
        if ((rvkRtRecord(f, index).flags & RVK_RT_LAYER) == 0u) continue;
        // an insertion sort by record index (a hit of the same record twice cannot happen: one layer's triangles do not overlap)
        uint p = rq.CandidatePrimitiveIndex();
        float2 b = rq.CandidateTriangleBarycentrics();
        int at = n < RVK_RT_LAYER_MAX ? n : RVK_RT_LAYER_MAX - 1;
        if (n == RVK_RT_LAYER_MAX && rec[at] <= index) continue;
        while (at > 0 && rec[at - 1] > index) {
            rec[at] = rec[at - 1];
            prim[at] = prim[at - 1];
            bary[at] = bary[at - 1];
            --at;
        }
        rec[at] = index;
        prim[at] = p;
        bary[at] = b;
        if (n < RVK_RT_LAYER_MAX) ++n;
    }
    if (n < 2) return closest;
    float3 c = float3(0.0, 0.0, 0.0);
    for (int i = 0; i < n; ++i) {
        RvkRtRecord r = rvkRtRecord(f, rec[i]);
        rvkRtApplySegment(r, prim[i]);
        RvkRtMaterialTri tri = rvkRtMaterialTriangle(r, prim[i]);
        float4 a = rvkRtAlbedo(r, rvkRtMaterialTriUv(tri, bary[i]), lod);
        uint3 t = rvkRtTriangle(r, prim[i]);
        float3 w = rvkRtWeights(bary[i]);
        float weight = r.color0 == RVK_RT_ABSENT ? 1.0
                       : rvkRtAttribute(r, t.x, r.color0).w * w.x + rvkRtAttribute(r, t.y, r.color0).w * w.y + rvkRtAttribute(r, t.z, r.color0).w * w.z;
        c = i == 0 ? a.rgb : lerp(c, a.rgb, clamp(weight, 0.0, 1.0));
    }
    return float4(c, 1.0);
}

RvkRtHitSurface rvkRtHitSurface(RvkFrame f, RvkRtHit h, float3 dir, float coneWidth0, float coneSpread) {
    RvkRtRecord r = rvkRtRecord(f, h.record);
    rvkRtApplySegment(r, h.primitive);
    RvkRtSurface s = rvkRtSurface(r, h.primitive, h.bary);
    RvkRtMaterialTri tri = rvkRtMaterialTriangle(r, h.primitive);
    RvkRtHitSurface o;
    o.position = rvkRtWorldPosition(h.objectToWorld, s.position);
    float3 g = rvkRtWorldNormal(h.worldToObject, s.geometricNormal);
    if (dot(g, dir) > 0.0) g = -g;
    float3 n = dot(s.normal, s.normal) > 1e-12 ? rvkRtWorldNormal(h.worldToObject, s.normal) : g;
    if (dot(n, g) < 0.0) n = -n;
    o.geometricNormal = g;
    o.normal = n;
    o.uv = rvkRtMaterialTriUv(tri, h.bary);
    o.uvLightmap = s.uvLightmap;
    o.flags = r.flags;
    o.uvSet = r.uvSet;
    o.instanceId = r.instanceId;
    o.lod = 0.0;
    if (r.albedoTex != 0u && (r.uvSet & RVK_RT_UV_NO_TEXTURE) == 0u) {
        o.lod = max(rvkRtTextureLod(tri, h.objectToWorld, rvkSize2D(r.albedoTex), coneWidth0 + coneSpread * h.t, abs(dot(g, dir))), 0.0);
    }
    o.albedo = rvkRtAlbedo(r, o.uv, o.lod);
    if ((r.flags & RVK_RT_LAYER) != 0u) o.albedo = rvkRtBlendLayers(f, h, o.position - dir * h.t, dir, o.lod, o.albedo);
    o.roughness = 1.0;
    o.metalness = 0.0;
    o.occlusion = 1.0;
    o.emissive = 0.0;
    o.packWord = r.packWord;
    if ((r.packWord & (1u << 14)) != 0u) {
        uint slot = r.packWord & 0x3FFFu;
        bool orm = (r.packWord & (1u << 15)) != 0u;
        float rScale = float((r.packWord >> 16) & 31u) * (1.0 / 16.0);
        float rBias = float((r.packWord >> 21) & 31u) * (1.0 / 31.0);
        float mScale = float((r.packWord >> 26) & 31u) * (1.0 / 31.0);
        float4 t = float4(1.0, 1.0, 1.0, 0.0);
        if (slot != 0u) {
            float lod = o.lod;
            if (r.albedoTex != 0u && (r.uvSet & RVK_RT_UV_NO_TEXTURE) == 0u) lod += log2(float(max(rvkSize2D(r.albedoTex).x, 1)) / float(max(rvkSize2D(slot).x, 1)));
            t = rvkSample2DLod(slot, r.albedoSampler, o.uv, max(lod, 0.0));
        }
        o.roughness = saturate((slot == 0u ? 1.0 : (orm ? t.g : t.r)) * rScale + rBias);
        o.occlusion = slot != 0u && orm ? t.r : 1.0;
        o.metalness = saturate(slot != 0u && orm ? t.b * mScale : mScale);
        o.emissive = slot != 0u && orm && (r.packWord >> 31) != 0u ? t.a : 0.0;
    }
    return o;
}
