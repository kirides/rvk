// Tracing the RT scene with materials (M9 P2, docs/research/rt-reflections.md 3.2): the closest hit with the alpha test of
// alpha-tested instances resolved in the ray query's candidate loop (the record's D3D9 alpha test on the diffuse / opacity
// alpha, mip from a ray cone), and the hit's surface (albedo, normals, uv). For every RT consumer (the water's reflections,
// the debug view). Include after rvk_abi.glsl; enables GL_EXT_ray_query.
//
//   RvkRtHit h = rvkRtTraceClosest(frame, origin, dir, tMin, tMax, coneWidth0, coneSpread, RVK_RT_TRACE_*);
//   if (h.hit) { RvkRtHitSurface s = rvkRtHitSurface(frame, h, dir, coneWidth0, coneSpread); ... s.albedo ... }
//
// Ray cone: width(t) = coneWidth0 + coneSpread * t (world units); camera rays: coneWidth0 = 0, coneSpread = the pixel's
// angle (2 / (proj[1][1] * height)); a reflection: the width at the reflector plus the spread its roughness / curvature adds.
// The ray-cone LOD follows GD3D11's D3D12 RT scene (Shaders/D3D12/include/RtScene.hlsl, the user's own code).

#ifndef RVK_RT_TRACE_GLSL
#define RVK_RT_TRACE_GLSL

#extension GL_EXT_ray_query : require

#include "rvk_common.glsl"
#include "rvk_rt.glsl"

const uint RVK_RT_TRACE_OPAQUE = 1u;       // alpha-tested instances count as solid (cheap tier: no candidate loop work)
const uint RVK_RT_TRACE_NO_CUTOUTS = 2u;   // alpha-tested instances are left out (cull mask)

struct RvkRtHit {
    bool hit;
    float t;
    uint record;         // geometry record index (instanceCustomIndex + geometryIndex)
    uint primitive;
    vec2 bary;
    bool frontFace;
    mat4x3 objectToWorld;
    mat4x3 worldToObject;
};

RvkRtHit rvkRtTraceClosest(RvkFrame f, vec3 origin, vec3 dir, float tMin, float tMax, float coneWidth0, float coneSpread, uint flags) {
    RvkRtHit h;
    h.hit = false;
    h.t = tMax;
    h.record = 0u;
    h.primitive = 0u;
    h.bary = vec2(0.0);
    h.frontFace = true;
    h.objectToWorld = mat4x3(1.0);
    h.worldToObject = mat4x3(1.0);
    if (!rvkRtValid(f)) return h;
    uint mask = (flags & RVK_RT_TRACE_NO_CUTOUTS) != 0u ? RVK_RT_MASK_OPAQUE : 0xFFu;
    uint rayFlags = (flags & RVK_RT_TRACE_OPAQUE) != 0u ? gl_RayFlagsOpaqueEXT : gl_RayFlagsNoneEXT;
    rayQueryEXT rq;
    rayQueryInitializeEXT(rq, accelerationStructureEXT(rvkRtTlas(f)), rayFlags, mask, origin, tMin, dir, tMax);
    while (rayQueryProceedEXT(rq)) {
        if (rayQueryGetIntersectionTypeEXT(rq, false) != gl_RayQueryCandidateIntersectionTriangleEXT) continue;
        uint index = uint(rayQueryGetIntersectionInstanceCustomIndexEXT(rq, false)) + uint(rayQueryGetIntersectionGeometryIndexEXT(rq, false));
        RvkRtRecord r = rvkRtRecord(f, index);
        float t = rayQueryGetIntersectionTEXT(rq, false);
        if (rvkRtCandidatePasses(r, uint(rayQueryGetIntersectionPrimitiveIndexEXT(rq, false)), rayQueryGetIntersectionBarycentricsEXT(rq, false),
                                 rayQueryGetIntersectionObjectToWorldEXT(rq, false), coneWidth0 + coneSpread * t)) {
            rayQueryConfirmIntersectionEXT(rq);
        }
    }
    if (rayQueryGetIntersectionTypeEXT(rq, true) != gl_RayQueryCommittedIntersectionTriangleEXT) return h;
    h.hit = true;
    h.t = rayQueryGetIntersectionTEXT(rq, true);
    h.record = uint(rayQueryGetIntersectionInstanceCustomIndexEXT(rq, true)) + uint(rayQueryGetIntersectionGeometryIndexEXT(rq, true));
    h.primitive = uint(rayQueryGetIntersectionPrimitiveIndexEXT(rq, true));
    h.bary = rayQueryGetIntersectionBarycentricsEXT(rq, true);
    h.frontFace = rayQueryGetIntersectionFrontFaceEXT(rq, true);
    h.objectToWorld = rayQueryGetIntersectionObjectToWorldEXT(rq, true);
    h.worldToObject = rayQueryGetIntersectionWorldToObjectEXT(rq, true);
    return h;
}

// visibility along a ray (1 = nothing in [tMin, tMax]: shadow rays); the first confirmed hit ends the query. Alpha-tested
// candidates as in rvkRtTraceClosest (RVK_RT_TRACE_OPAQUE: solid, the cheap shadow)
float rvkRtVisibility(RvkFrame f, vec3 origin, vec3 dir, float tMin, float tMax, float coneWidth0, float coneSpread, uint flags) {
    if (!rvkRtValid(f)) return 1.0;
    uint mask = (flags & RVK_RT_TRACE_NO_CUTOUTS) != 0u ? RVK_RT_MASK_OPAQUE : 0xFFu;
    uint rayFlags = gl_RayFlagsTerminateOnFirstHitEXT | ((flags & RVK_RT_TRACE_OPAQUE) != 0u ? gl_RayFlagsOpaqueEXT : gl_RayFlagsNoneEXT);
    rayQueryEXT rq;
    rayQueryInitializeEXT(rq, accelerationStructureEXT(rvkRtTlas(f)), rayFlags, mask, origin, tMin, dir, tMax);
    while (rayQueryProceedEXT(rq)) {
        if (rayQueryGetIntersectionTypeEXT(rq, false) != gl_RayQueryCandidateIntersectionTriangleEXT) continue;
        uint index = uint(rayQueryGetIntersectionInstanceCustomIndexEXT(rq, false)) + uint(rayQueryGetIntersectionGeometryIndexEXT(rq, false));
        RvkRtRecord r = rvkRtRecord(f, index);
        float t = rayQueryGetIntersectionTEXT(rq, false);
        if (rvkRtCandidatePasses(r, uint(rayQueryGetIntersectionPrimitiveIndexEXT(rq, false)), rayQueryGetIntersectionBarycentricsEXT(rq, false),
                                 rayQueryGetIntersectionObjectToWorldEXT(rq, false), coneWidth0 + coneSpread * t)) {
            rayQueryConfirmIntersectionEXT(rq);
        }
    }
    return rayQueryGetIntersectionTypeEXT(rq, true) == gl_RayQueryCommittedIntersectionNoneEXT ? 1.0 : 0.0;
}

struct RvkRtHitSurface {
    vec4 albedo;           // the diffuse at the ray cone's mip, as sampled (rgb, a = its alpha); the record's colour without a texture
    vec3 position;         // world
    vec3 normal;           // world, interpolated vertex normal, turned towards the ray's origin (geometric when there is none)
    vec3 geometricNormal;  // world, towards the ray's origin
    vec2 uv;               // the diffuse coordinate (the material's uv map applied)
    vec2 uvLightmap;       // TEXCOORD6 (lightmapped level meshes)
    float lod;             // the mip level the albedo was sampled at
    uint flags;            // record flags (RVK_RT_*: kind << 8, views << 16)
    uint uvSet;            // record uvSet (RVK_RT_UV_*)
    uint instanceId;
};

RvkRtHitSurface rvkRtHitSurface(RvkFrame f, RvkRtHit h, vec3 dir, float coneWidth0, float coneSpread) {
    RvkRtRecord r = rvkRtRecord(f, h.record);
    rvkRtApplySegment(r, h.primitive);
    RvkRtSurface s = rvkRtSurface(r, h.primitive, h.bary);
    RvkRtMaterialTri tri = rvkRtMaterialTriangle(r, h.primitive);
    RvkRtHitSurface o;
    o.position = rvkRtWorldPosition(h.objectToWorld, s.position);
    vec3 g = rvkRtWorldNormal(h.worldToObject, s.geometricNormal);
    if (dot(g, dir) > 0.0) g = -g;
    vec3 n = dot(s.normal, s.normal) > 1e-12 ? rvkRtWorldNormal(h.worldToObject, s.normal) : g;
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
    return o;
}

#endif
