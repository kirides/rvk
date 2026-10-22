// The ray tracing scene in GLSL (src/enh/rt_scene.*, docs/research/rt-reflections.md 3): the FrameBlock's TLAS and the
// geometry records (mirror of shader::abi::RtGeometryRecord; rvk_tests abi_glsl_layout_matches_cpp compares the offsets),
// vertex / index fetches from the engine's buffers by device address, barycentric interpolation. Include after
// rvk_abi.glsl. Shaders that trace enable GL_EXT_ray_query themselves and use rvkRtTlas(frame).
//
// A hit: record = rvkRtRecord(frame, rayQueryGetIntersectionInstanceCustomIndexEXT(rq, true)
//                                      + rayQueryGetIntersectionGeometryIndexEXT(rq, true));
//        RvkRtSurface s = rvkRtSurface(record, rayQueryGetIntersectionPrimitiveIndexEXT(rq, true),
//                                      rayQueryGetIntersectionBarycentricsEXT(rq, true));
//        world normal = rvkRtWorldNormal(rayQueryGetIntersectionWorldToObjectEXT(rq, true), s.normal)

#ifndef RVK_RT_GLSL
#define RVK_RT_GLSL

#extension GL_EXT_buffer_reference_uvec2 : require

// D3DDECLTYPE of an attribute word (offset | type << 16; RVK_RT_ABSENT = the declaration has no such element)
const uint RVK_RT_ABSENT = 0xFFFFFFFFu;
const uint RVK_DECL_FLOAT1 = 0u, RVK_DECL_FLOAT2 = 1u, RVK_DECL_FLOAT3 = 2u, RVK_DECL_FLOAT4 = 3u, RVK_DECL_D3DCOLOR = 4u,
           RVK_DECL_UBYTE4 = 5u, RVK_DECL_SHORT2 = 6u, RVK_DECL_SHORT4 = 7u, RVK_DECL_UBYTE4N = 8u, RVK_DECL_SHORT2N = 9u,
           RVK_DECL_SHORT4N = 10u, RVK_DECL_USHORT2N = 11u, RVK_DECL_USHORT4N = 12u, RVK_DECL_FLOAT16_2 = 15u,
           RVK_DECL_FLOAT16_4 = 16u;

// RvkRtRecord.flags (abi.h kRtRecord*); bits 8-15 = the draws::Kind of the draw, bits 16-23 the views it was seen in
// (RVK_RT_VIEW_MAIN, RVK_RT_VIEW_SHADOW)
const uint RVK_RT_INDEX32 = 1u;
const uint RVK_RT_ALPHA_TESTED = 2u;
const uint RVK_RT_SPEEDTREE = 4u;
const uint RVK_RT_INSTANCED = 8u;
const uint RVK_RT_NO_DEPTH_WRITE = 16u;  // a main-view solid draw of it had z writes off
const uint RVK_RT_DEPTH_BIAS = 32u;      // a main-view draw of it had a depth bias
const uint RVK_RT_VIEW_MAIN = 0x10000u;
const uint RVK_RT_VIEW_SHADOW = 0x20000u;
// TLAS instance masks (abi.h kRtMask*)
const uint RVK_RT_MASK_OPAQUE = 1u;
const uint RVK_RT_MASK_ALPHA_TESTED = 2u;

// one geometry of one TLAS instance (96 bytes)
struct RvkRtRecord {
    uvec2 vertices;      //  0 device address of vertex 0 (stream offset + base vertex applied)
    uvec2 indices;       //  8 device address of the draw's first index
    uint stride;         // 16 vertex stride in bytes
    uint flags;          // 20 RVK_RT_* | kind << 8 | views << 16
    uint normal;         // 24 attribute words
    uint texcoord0;      // 28
    uint texcoord1;      // 32
    uint texcoord6;      // 36 the lightmap uv
    uint color0;         // 40
    uint instanceId;     // 44 stable id of the instance (debug colours)
    uint albedoTex;      // 48 (M9 P2) bindless t2D slot of the diffuse texture, 0 = none
    uint albedoSampler;  // 52 (P2) bindless sampler
    uint alpha;          // 56 alpha test: D3DCMPFUNC | reference << 8 (func 8 = always)
    uint uvSet;          // 60 (P2) which uv set the albedo uses + flags
    vec4 uvMap0;         // 64 (P2) u' = dot(uvMap0.xy, uv) + uvMap0.z
    vec4 uvMap1;         // 80 (P2) v' = dot(uvMap1.xy, uv) + uvMap1.z
};

layout(buffer_reference, std430, buffer_reference_align = 16) readonly buffer RvkRtRecords {
    RvkRtRecord r[];
};
layout(buffer_reference, std430, buffer_reference_align = 4) readonly buffer RvkRtWord {
    uint v;
};

bool rvkRtValid(RvkFrame f) { return (f.rtInfo.z & RVK_FRAME_RT_VALID) != 0u && f.rtScene.xy != uvec2(0u); }
// accelerationStructureEXT(rvkRtTlas(f)) in shaders with GL_EXT_ray_query
uvec2 rvkRtTlas(RvkFrame f) { return f.rtScene.xy; }
RvkRtRecord rvkRtRecord(RvkFrame f, uint index) { return RvkRtRecords(f.rtScene.zw).r[index]; }

// 64-bit address arithmetic on uvec2 (no shaderInt64)
uvec2 rvkAddrAdd(uvec2 a, uint bytes) {
    uint carry;
    uint lo = uaddCarry(a.x, bytes, carry);
    return uvec2(lo, a.y + carry);
}
uint rvkLoadWord(uvec2 a) { return RvkRtWord(a).v; }  // a 4-byte aligned
// a 16-bit value at a 2-byte aligned address
uint rvkLoadHalfWord(uvec2 a) {
    uint w = rvkLoadWord(uvec2(a.x & ~3u, a.y));
    return (a.x & 2u) != 0u ? w >> 16 : w & 0xFFFFu;
}

// the three vertex indices of a triangle of the record's draw (triangle list)
uvec3 rvkRtTriangle(RvkRtRecord r, uint primitive) {
    if ((r.flags & RVK_RT_INDEX32) != 0u) {
        uvec2 a = rvkAddrAdd(r.indices, primitive * 12u);
        return uvec3(rvkLoadWord(a), rvkLoadWord(rvkAddrAdd(a, 4u)), rvkLoadWord(rvkAddrAdd(a, 8u)));
    }
    uvec2 a = rvkAddrAdd(r.indices, primitive * 6u);
    return uvec3(rvkLoadHalfWord(a), rvkLoadHalfWord(rvkAddrAdd(a, 2u)), rvkLoadHalfWord(rvkAddrAdd(a, 4u)));
}

float rvkSignExtend16(uint v) { return float(int(v << 16) >> 16); }

// a vertex attribute as the D3D9 vertex shader reads it (missing components = 0, w = 1; absent = (0, 0, 0, 1))
vec4 rvkRtAttribute(RvkRtRecord r, uint vertex, uint attribWord) {
    if (attribWord == RVK_RT_ABSENT) return vec4(0.0, 0.0, 0.0, 1.0);
    uint type = attribWord >> 16;
    uvec2 a = rvkAddrAdd(r.vertices, vertex * r.stride + (attribWord & 0xFFFFu));
    uint w0 = rvkLoadWord(a);
    switch (type) {
    case RVK_DECL_FLOAT1: return vec4(uintBitsToFloat(w0), 0.0, 0.0, 1.0);
    case RVK_DECL_FLOAT2: return vec4(uintBitsToFloat(w0), uintBitsToFloat(rvkLoadWord(rvkAddrAdd(a, 4u))), 0.0, 1.0);
    case RVK_DECL_FLOAT3:
        return vec4(uintBitsToFloat(w0), uintBitsToFloat(rvkLoadWord(rvkAddrAdd(a, 4u))), uintBitsToFloat(rvkLoadWord(rvkAddrAdd(a, 8u))), 1.0);
    case RVK_DECL_FLOAT4:
        return vec4(uintBitsToFloat(w0), uintBitsToFloat(rvkLoadWord(rvkAddrAdd(a, 4u))), uintBitsToFloat(rvkLoadWord(rvkAddrAdd(a, 8u))),
                    uintBitsToFloat(rvkLoadWord(rvkAddrAdd(a, 12u))));
    case RVK_DECL_D3DCOLOR: return unpackUnorm4x8(w0).zyxw;  // BGRA bytes -> RGBA
    case RVK_DECL_UBYTE4: return vec4(uvec4(w0, w0 >> 8, w0 >> 16, w0 >> 24) & 0xFFu);
    case RVK_DECL_SHORT2: return vec4(rvkSignExtend16(w0), rvkSignExtend16(w0 >> 16), 0.0, 1.0);
    case RVK_DECL_SHORT4: {
        uint w1 = rvkLoadWord(rvkAddrAdd(a, 4u));
        return vec4(rvkSignExtend16(w0), rvkSignExtend16(w0 >> 16), rvkSignExtend16(w1), rvkSignExtend16(w1 >> 16));
    }
    case RVK_DECL_UBYTE4N: return unpackUnorm4x8(w0);
    case RVK_DECL_SHORT2N: return vec4(unpackSnorm2x16(w0), 0.0, 1.0);
    case RVK_DECL_SHORT4N: return vec4(unpackSnorm2x16(w0), unpackSnorm2x16(rvkLoadWord(rvkAddrAdd(a, 4u))));
    case RVK_DECL_USHORT2N: return vec4(unpackUnorm2x16(w0), 0.0, 1.0);
    case RVK_DECL_USHORT4N: return vec4(unpackUnorm2x16(w0), unpackUnorm2x16(rvkLoadWord(rvkAddrAdd(a, 4u))));
    case RVK_DECL_FLOAT16_2: return vec4(unpackHalf2x16(w0), 0.0, 1.0);
    case RVK_DECL_FLOAT16_4: return vec4(unpackHalf2x16(w0), unpackHalf2x16(rvkLoadWord(rvkAddrAdd(a, 4u))));
    default: return vec4(0.0, 0.0, 0.0, 1.0);
    }
}

// the object-space position of a vertex (POSITION FLOAT3 at offset 0: what the BLAS was built from)
vec3 rvkRtPosition(RvkRtRecord r, uint vertex) { return rvkRtAttribute(r, vertex, RVK_DECL_FLOAT3 << 16).xyz; }

// barycentric weights of a ray query hit (u, v of vertices 1 and 2)
vec3 rvkRtWeights(vec2 bary) { return vec3(1.0 - bary.x - bary.y, bary.x, bary.y); }

// the surface at a hit, object space
struct RvkRtSurface {
    vec3 position;
    vec3 normal;     // interpolated vertex normal (not normalised; (0, 0, 0) when the declaration has none)
    vec2 uv0;
    vec2 uv1;
    vec2 uvLightmap;
    vec3 geometricNormal;  // from the triangle's edges
};

RvkRtSurface rvkRtSurface(RvkRtRecord r, uint primitive, vec2 bary) {
    uvec3 t = rvkRtTriangle(r, primitive);
    vec3 w = rvkRtWeights(bary);
    RvkRtSurface s;
    vec3 p0 = rvkRtPosition(r, t.x), p1 = rvkRtPosition(r, t.y), p2 = rvkRtPosition(r, t.z);
    s.position = p0 * w.x + p1 * w.y + p2 * w.z;
    s.geometricNormal = cross(p1 - p0, p2 - p0);
    s.normal = r.normal == RVK_RT_ABSENT ? vec3(0.0)
                                         : rvkRtAttribute(r, t.x, r.normal).xyz * w.x + rvkRtAttribute(r, t.y, r.normal).xyz * w.y +
                                               rvkRtAttribute(r, t.z, r.normal).xyz * w.z;
    s.uv0 = rvkRtAttribute(r, t.x, r.texcoord0).xy * w.x + rvkRtAttribute(r, t.y, r.texcoord0).xy * w.y +
            rvkRtAttribute(r, t.z, r.texcoord0).xy * w.z;
    s.uv1 = rvkRtAttribute(r, t.x, r.texcoord1).xy * w.x + rvkRtAttribute(r, t.y, r.texcoord1).xy * w.y +
            rvkRtAttribute(r, t.z, r.texcoord1).xy * w.z;
    s.uvLightmap = rvkRtAttribute(r, t.x, r.texcoord6).xy * w.x + rvkRtAttribute(r, t.y, r.texcoord6).xy * w.y +
                   rvkRtAttribute(r, t.z, r.texcoord6).xy * w.z;
    return s;
}

// object -> world for normals: n * worldToObject's 3x3 (= the inverse transpose of objectToWorld applied to n)
vec3 rvkRtWorldNormal(mat4x3 worldToObject, vec3 n) { return normalize(n * mat3(worldToObject)); }
vec3 rvkRtWorldPosition(mat4x3 objectToWorld, vec3 p) { return objectToWorld * vec4(p, 1.0); }

#endif
