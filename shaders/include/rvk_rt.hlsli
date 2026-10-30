// The ray tracing scene in HLSL (src/enh/rt_scene.*, docs/research/rt-reflections.md 3; port of rvk_rt.glsl): the FrameBlock's TLAS and the
// geometry records (mirror of shader::abi::RtGeometryRecord), vertex / index fetches from the engine's buffers by device address, barycentric
// interpolation. The TLAS is an element of the bindless acceleration structure array (set 0 binding 4, FrameBlock rtInfo.w = the slot): HLSL
// cannot make an acceleration structure from an address.
//
// Materials (M9 P2): rvkRtMaterialTriangle / rvkRtTextureLod / rvkRtAlbedo / rvkRtCandidatePasses below; tracing with the alpha test and the hit's
// surface: rvk_rt_trace.hlsli (rvkRtTraceClosest, rvkRtHitSurface).
//
// Matrices of a hit: GLSL's mat4x3 (4 columns of 3 rows) is HLSL's float3x4 (3 rows of 4 columns): `M * vec4(p, 1)` = mul(M, float4(p, 1)),
// `n * mat3(M)` = mul(n, (float3x3)M).
#pragma once

#include "rvk_common.hlsli"

RVK_DECLARE_TLAS

// D3DDECLTYPE of an attribute word (offset | type << 16; RVK_RT_ABSENT = the declaration has no such element)
static const uint RVK_RT_ABSENT = 0xFFFFFFFFu;
static const uint RVK_DECL_FLOAT1 = 0u, RVK_DECL_FLOAT2 = 1u, RVK_DECL_FLOAT3 = 2u, RVK_DECL_FLOAT4 = 3u, RVK_DECL_D3DCOLOR = 4u,
                  RVK_DECL_UBYTE4 = 5u, RVK_DECL_SHORT2 = 6u, RVK_DECL_SHORT4 = 7u, RVK_DECL_UBYTE4N = 8u, RVK_DECL_SHORT2N = 9u,
                  RVK_DECL_SHORT4N = 10u, RVK_DECL_USHORT2N = 11u, RVK_DECL_USHORT4N = 12u, RVK_DECL_FLOAT16_2 = 15u,
                  RVK_DECL_FLOAT16_4 = 16u;

// RvkRtRecord.flags (abi.h kRtRecord*); bits 8-15 = the draws::Kind of the draw, bits 16-23 the views it was seen in
// (RVK_RT_VIEW_MAIN, RVK_RT_VIEW_SHADOW)
static const uint RVK_RT_INDEX32 = 1u;
static const uint RVK_RT_ALPHA_TESTED = 2u;
static const uint RVK_RT_SPEEDTREE = 4u;
static const uint RVK_RT_INSTANCED = 8u;
static const uint RVK_RT_NO_DEPTH_WRITE = 16u;  // a main-view solid draw of it had z writes off
static const uint RVK_RT_DEPTH_BIAS = 32u;      // a main-view draw of it had a depth bias
static const uint RVK_RT_SEGMENTED = 64u;       // per-triangle materials (segments: rvkRtApplySegment)
static const uint RVK_RT_DYNAMIC = 128u;        // a skinned draw's vertices captured this frame (world space, no normals)
static const uint RVK_RT_LAYER = 0x1000000u;    // a terrain layer: blended by its vertex colour alpha over the coplanar layers below
static const uint RVK_RT_VIEW_MAIN = 0x10000u;
static const uint RVK_RT_VIEW_SHADOW = 0x20000u;
// TLAS instance masks (abi.h kRtMask*)
static const uint RVK_RT_MASK_OPAQUE = 1u;
static const uint RVK_RT_MASK_ALPHA_TESTED = 2u;
static const uint RVK_RT_MASK_DYNAMIC = 4u;  // characters
// RvkRtRecord.uvSet (abi.h kRtUv*)
static const uint RVK_RT_UV_SET_MASK = 3u;             // 0 = the vertex's TEXCOORD0, 1 = TEXCOORD1
static const uint RVK_RT_UV_APPROX = 4u;               // the pixel shader's coordinate is not affine in that set
static const uint RVK_RT_UV_NO_TEXTURE = 8u;           // no diffuse texture: uvMap0.w = a colour (RGBA8 bits)
static const uint RVK_RT_UV_HEURISTIC = 0x10u;         // slot from the bytecode fallback
static const uint RVK_RT_UV_OPACITY_SHIFT = 8u;        // bits 8-9: the opacity's channel
static const uint RVK_RT_UV_OPACITY_OPAQUE = 0x400u;   // the opacity is no texture
static const uint RVK_RT_UV_OPACITY_TEXTURE = 0x800u;  // the opacity's own texture: bindless slot in uvMap1.w
static const uint RVK_RT_UV_EMISSIVE = 0x1000u;        // a textured self-illumination role: the diffuse colour is emitted (GI)

// one geometry of one TLAS instance in memory (112 bytes = 7 x uint4; mirror of abi.h RtGeometryRecord)
struct RvkRtRecordData {
    uint4 w0;  //  0 vertices.xy (device address of vertex 0, stream offset + base vertex applied), indices.xy (the draw's first index)
    uint4 w1;  // 16 stride, flags, normal, texcoord0   (attribute words: offset | D3DDECLTYPE << 16)
    uint4 w2;  // 32 texcoord1, texcoord6 (the lightmap uv), color0, instanceId
    uint4 w3;  // 48 albedoTex (bindless t2D slot, 0 = none), albedoSampler, alpha (D3DCMPFUNC | reference << 8), uvSet (RVK_RT_UV_*)
    uint4 uvMap0;  // 64 float bits: u' = dot(uvMap0.xy, uv) + uvMap0.z; .w = colour without a texture (RGBA8 bits)
    uint4 uvMap1;  // 80 float bits: v' = dot(uvMap1.xy, uv) + uvMap1.z; .w = the opacity texture's slot (bits)
    uint4 w6;  // 96 segments.xy (address of segmentCount RvkRtSegment, sorted by firstPrimitive), segmentCount, pad
};

// the record as the shaders use it
struct RvkRtRecord {
    uint2 vertices;
    uint2 indices;
    uint stride;
    uint flags;
    uint normal;
    uint texcoord0;
    uint texcoord1;
    uint texcoord6;
    uint color0;
    uint instanceId;
    uint albedoTex;
    uint albedoSampler;
    uint alpha;
    uint uvSet;
    float4 uvMap0;
    float4 uvMap1;
    uint2 segments;
    uint segmentCount;
};

// the material of a sub-range of a record's triangles (abi.h RtMaterialSegment, 64 bytes)
struct RvkRtSegmentData {
    uint4 w0;  // firstPrimitive, primitiveCount, albedoTex, albedoSampler
    uint4 w1;  // alpha, uvSet, pad, pad
    uint4 uvMap0;
    uint4 uvMap1;
};

bool rvkRtValid(RvkFrame f) { return (f.rtInfo.z & RVK_FRAME_RT_VALID) != 0u && any(f.rtScene.xy != uint2(0u, 0u)); }
// (a macro: DXC cannot return an acceleration structure from a function)
#define rvkRtTlas(f) RVK_TLAS((f).rtInfo.w)

RvkRtRecord rvkRtRecord(RvkFrame f, uint index) {
    RvkRtRecordData d = rvkPtr<RvkRtRecordData, 16>(rvkPairAdd(f.rtScene.zw, index * 112u)).Get();
    RvkRtRecord r;
    r.vertices = d.w0.xy;
    r.indices = d.w0.zw;
    r.stride = d.w1.x;
    r.flags = d.w1.y;
    r.normal = d.w1.z;
    r.texcoord0 = d.w1.w;
    r.texcoord1 = d.w2.x;
    r.texcoord6 = d.w2.y;
    r.color0 = d.w2.z;
    r.instanceId = d.w2.w;
    r.albedoTex = d.w3.x;
    r.albedoSampler = d.w3.y;
    r.alpha = d.w3.z;
    r.uvSet = d.w3.w;
    r.uvMap0 = asfloat(d.uvMap0);
    r.uvMap1 = asfloat(d.uvMap1);
    r.segments = d.w6.xy;
    r.segmentCount = d.w6.z;
    return r;
}

// 4-byte aligned word at a device address
uint rvkLoadWord(RvkAddr a) { return rvkRawLoad<uint, 4>(a); }
// a 16-bit value at a 2-byte aligned address
uint rvkLoadHalfWord(RvkAddr a) {
    uint w = rvkLoadWord(rvkAddrAlign4(a));
    return (rvkAddrLow(a) & 2u) != 0u ? w >> 16 : w & 0xFFFFu;
}

// the three vertex indices of a triangle of the record's draw (triangle list)
uint3 rvkRtTriangle(RvkRtRecord r, uint primitive) {
    RvkAddr base = rvkAddr(r.indices);
    if ((r.flags & RVK_RT_INDEX32) != 0u) {
        RvkAddr a = rvkAddrAdd(base, primitive * 12u);
        return uint3(rvkLoadWord(a), rvkLoadWord(rvkAddrAdd(a, 4u)), rvkLoadWord(rvkAddrAdd(a, 8u)));
    }
    RvkAddr a = rvkAddrAdd(base, primitive * 6u);
    return uint3(rvkLoadHalfWord(a), rvkLoadHalfWord(rvkAddrAdd(a, 2u)), rvkLoadHalfWord(rvkAddrAdd(a, 4u)));
}

float rvkSignExtend16(uint v) { return float(int(v << 16) >> 16); }

float2 rvkUnpackSnorm2x16(uint w) { return clamp(float2(rvkSignExtend16(w), rvkSignExtend16(w >> 16)) / 32767.0, -1.0, 1.0); }
float2 rvkUnpackUnorm2x16(uint w) { return float2(w & 0xFFFFu, w >> 16) / 65535.0; }
float2 rvkUnpackHalf2x16(uint w) { return float2(f16tof32(w & 0xFFFFu), f16tof32(w >> 16)); }

// a vertex attribute as the D3D9 vertex shader reads it (missing components = 0, w = 1; absent = (0, 0, 0, 1))
float4 rvkRtAttribute(RvkRtRecord r, uint vertex, uint attribWord) {
    if (attribWord == RVK_RT_ABSENT) return float4(0.0, 0.0, 0.0, 1.0);
    uint type = attribWord >> 16;
    RvkAddr a = rvkAddrAdd(rvkAddr(r.vertices), vertex * r.stride + (attribWord & 0xFFFFu));
    uint w0 = rvkLoadWord(a);
    switch (type) {
    case RVK_DECL_FLOAT1: return float4(asfloat(w0), 0.0, 0.0, 1.0);
    case RVK_DECL_FLOAT2: return float4(asfloat(w0), asfloat(rvkLoadWord(rvkAddrAdd(a, 4u))), 0.0, 1.0);
    case RVK_DECL_FLOAT3: return float4(asfloat(w0), asfloat(rvkLoadWord(rvkAddrAdd(a, 4u))), asfloat(rvkLoadWord(rvkAddrAdd(a, 8u))), 1.0);
    case RVK_DECL_FLOAT4:
        return float4(asfloat(w0), asfloat(rvkLoadWord(rvkAddrAdd(a, 4u))), asfloat(rvkLoadWord(rvkAddrAdd(a, 8u))), asfloat(rvkLoadWord(rvkAddrAdd(a, 12u))));
    case RVK_DECL_D3DCOLOR: return rvkUnpackUnorm4x8(w0).zyxw;  // BGRA bytes -> RGBA
    case RVK_DECL_UBYTE4: return float4(uint4(w0, w0 >> 8, w0 >> 16, w0 >> 24) & 0xFFu);
    case RVK_DECL_SHORT2: return float4(rvkSignExtend16(w0), rvkSignExtend16(w0 >> 16), 0.0, 1.0);
    case RVK_DECL_SHORT4: {
        uint w1 = rvkLoadWord(rvkAddrAdd(a, 4u));
        return float4(rvkSignExtend16(w0), rvkSignExtend16(w0 >> 16), rvkSignExtend16(w1), rvkSignExtend16(w1 >> 16));
    }
    case RVK_DECL_UBYTE4N: return rvkUnpackUnorm4x8(w0);
    case RVK_DECL_SHORT2N: return float4(rvkUnpackSnorm2x16(w0), 0.0, 1.0);
    case RVK_DECL_SHORT4N: return float4(rvkUnpackSnorm2x16(w0), rvkUnpackSnorm2x16(rvkLoadWord(rvkAddrAdd(a, 4u))));
    case RVK_DECL_USHORT2N: return float4(rvkUnpackUnorm2x16(w0), 0.0, 1.0);
    case RVK_DECL_USHORT4N: return float4(rvkUnpackUnorm2x16(w0), rvkUnpackUnorm2x16(rvkLoadWord(rvkAddrAdd(a, 4u))));
    case RVK_DECL_FLOAT16_2: return float4(rvkUnpackHalf2x16(w0), 0.0, 1.0);
    case RVK_DECL_FLOAT16_4: return float4(rvkUnpackHalf2x16(w0), rvkUnpackHalf2x16(rvkLoadWord(rvkAddrAdd(a, 4u))));
    default: return float4(0.0, 0.0, 0.0, 1.0);
    }
}

// the object-space position of a vertex (POSITION FLOAT3 at offset 0: what the BLAS was built from)
float3 rvkRtPosition(RvkRtRecord r, uint vertex) { return rvkRtAttribute(r, vertex, RVK_DECL_FLOAT3 << 16).xyz; }

// barycentric weights of a ray query hit (u, v of vertices 1 and 2)
float3 rvkRtWeights(float2 bary) { return float3(1.0 - bary.x - bary.y, bary.x, bary.y); }

// the surface at a hit, object space
struct RvkRtSurface {
    float3 position;
    float3 normal;     // interpolated vertex normal (not normalised; (0, 0, 0) when the declaration has none)
    float2 uv0;
    float2 uv1;
    float2 uvLightmap;
    float3 geometricNormal;  // from the triangle's edges
};

RvkRtSurface rvkRtSurface(RvkRtRecord r, uint primitive, float2 bary) {
    uint3 t = rvkRtTriangle(r, primitive);
    float3 w = rvkRtWeights(bary);
    RvkRtSurface s;
    float3 p0 = rvkRtPosition(r, t.x), p1 = rvkRtPosition(r, t.y), p2 = rvkRtPosition(r, t.z);
    s.position = p0 * w.x + p1 * w.y + p2 * w.z;
    s.geometricNormal = cross(p1 - p0, p2 - p0);
    s.normal = r.normal == RVK_RT_ABSENT ? float3(0.0, 0.0, 0.0)
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
float3 rvkRtWorldNormal(float3x4 worldToObject, float3 n) { return normalize(mul(n, (float3x3)worldToObject)); }
float3 rvkRtWorldPosition(float3x4 objectToWorld, float3 p) { return mul(objectToWorld, float4(p, 1.0)); }

// ---- materials (M9 P2, docs/research/rt-reflections.md 3.2) ------------------------------------------------------------------

// a segmented record (a cascade's merged range of a level mesh): the material of the segment holding `primitive`
void rvkRtApplySegment(inout RvkRtRecord r, uint primitive) {
    if ((r.flags & RVK_RT_SEGMENTED) == 0u || r.segmentCount == 0u) return;
    uint lo = 0u, hi = r.segmentCount;
    for (int i = 0; i < 16 && lo < hi; ++i) {
        uint mid = (lo + hi) >> 1;
        RvkRtSegmentData s = rvkPtr<RvkRtSegmentData, 16>(rvkPairAdd(r.segments, mid * 64u)).Get();
        uint firstPrimitive = s.w0.x, primitiveCount = s.w0.y;
        if (primitive < firstPrimitive) {
            hi = mid;
        } else if (primitive >= firstPrimitive + primitiveCount) {
            lo = mid + 1u;
        } else {
            r.albedoTex = s.w0.z;
            r.albedoSampler = s.w0.w;
            r.alpha = s.w1.x;
            r.uvSet = s.w1.y;
            r.uvMap0 = asfloat(s.uvMap0);
            r.uvMap1 = asfloat(s.uvMap1);
            return;
        }
    }
}

// the diffuse coordinate of a vertex: its uv set through the material's affine map (the pixel shader's coordinate)
float2 rvkRtMaterialUv(RvkRtRecord r, uint vertex) {
    uint word = (r.uvSet & RVK_RT_UV_SET_MASK) == 1u ? r.texcoord1 : r.texcoord0;
    float2 uv = rvkRtAttribute(r, vertex, word).xy;
    return float2(dot(r.uvMap0.xy, uv) + r.uvMap0.z, dot(r.uvMap1.xy, uv) + r.uvMap1.z);
}

// a triangle's object-space positions and diffuse coordinates
struct RvkRtMaterialTri {
    float3 p0, p1, p2;
    float2 uv0, uv1, uv2;
};
RvkRtMaterialTri rvkRtMaterialTriangle(RvkRtRecord r, uint primitive) {
    uint3 i = rvkRtTriangle(r, primitive);
    RvkRtMaterialTri t;
    t.p0 = rvkRtPosition(r, i.x);
    t.p1 = rvkRtPosition(r, i.y);
    t.p2 = rvkRtPosition(r, i.z);
    t.uv0 = rvkRtMaterialUv(r, i.x);
    t.uv1 = rvkRtMaterialUv(r, i.y);
    t.uv2 = rvkRtMaterialUv(r, i.z);
    return t;
}
float2 rvkRtMaterialTriUv(RvkRtMaterialTri t, float2 bary) { return t.uv0 + bary.x * (t.uv1 - t.uv0) + bary.y * (t.uv2 - t.uv0); }

// Ray-cone mip level (Akenine-Moeller et al., "Improved shader and texture level of detail using ray cones"; as in GD3D11's
// RtScene.hlsl TextureLod): the texel-to-world area ratio of the triangle (world space: the instance's scale counts) plus
// the cone's width at the hit over the incidence cosine.
float rvkRtTextureLod(RvkRtMaterialTri t, float3x4 objectToWorld, int2 texSize, float coneWidth, float cosine) {
    float2 d1 = t.uv1 - t.uv0, d2 = t.uv2 - t.uv0;
    float ta = abs(d1.x * d2.y - d2.x * d1.y) * float(texSize.x) * float(texSize.y);
    float3x3 m = (float3x3)objectToWorld;
    float pa = length(cross(mul(m, t.p1 - t.p0), mul(m, t.p2 - t.p0)));
    return 0.5 * log2(max(ta, 1e-12) / max(pa, 1e-12)) + log2(max(coneWidth, 1e-4) / max(cosine, 0.1));
}

// the D3D9 alpha test of the record (D3DCMPFUNC on the alpha quantised to 0..255 against the reference)
bool rvkRtAlphaTest(RvkRtRecord r, float alpha) {
    float q = round(clamp(alpha, 0.0, 1.0) * 255.0);
    float ref = float((r.alpha >> 8) & 0xFFu);
    switch (r.alpha & 0xFFu) {
    case 1u: return false;
    case 2u: return q < ref;
    case 3u: return q == ref;
    case 4u: return q <= ref;
    case 5u: return q > ref;
    case 6u: return q != ref;
    case 7u: return q >= ref;
    default: return true;
    }
}

// the opacity the alpha test reads (the diffuse alpha, or the opacity texture's channel), at mip `lod`
float rvkRtOpacity(RvkRtRecord r, float2 uv, float lod) {
    if ((r.uvSet & RVK_RT_UV_OPACITY_OPAQUE) != 0u) return 1.0;
    uint tex = (r.uvSet & RVK_RT_UV_OPACITY_TEXTURE) != 0u ? asuint(r.uvMap1.w) : r.albedoTex;
    if (tex == 0u) return 1.0;
    float4 s = rvkSample2DLod(tex, r.albedoSampler, uv, lod);
    return s[(r.uvSet >> RVK_RT_UV_OPACITY_SHIFT) & 3u];
}

// whether a candidate hit of an alpha-tested instance stands (`coneWidth`: the ray cone's width at the hit)
bool rvkRtCandidatePasses(RvkRtRecord r, uint primitive, float2 bary, float3x4 objectToWorld, float coneWidth) {
    rvkRtApplySegment(r, primitive);
    if ((r.flags & RVK_RT_ALPHA_TESTED) == 0u || (r.uvSet & RVK_RT_UV_OPACITY_OPAQUE) != 0u) return true;
    uint tex = (r.uvSet & RVK_RT_UV_OPACITY_TEXTURE) != 0u ? asuint(r.uvMap1.w) : r.albedoTex;
    if (tex == 0u) return true;
    RvkRtMaterialTri t = rvkRtMaterialTriangle(r, primitive);
    float lod = max(rvkRtTextureLod(t, objectToWorld, rvkSize2D(tex), coneWidth, 1.0), 0.0);
    return rvkRtAlphaTest(r, rvkRtOpacity(r, rvkRtMaterialTriUv(t, bary), lod));
}

// the diffuse colour (rgba as sampled) at `uv`, mip `lod`; the record's constant colour without a texture
float4 rvkRtAlbedo(RvkRtRecord r, float2 uv, float lod) {
    if (r.albedoTex == 0u || (r.uvSet & RVK_RT_UV_NO_TEXTURE) != 0u) return rvkUnpackUnorm4x8(asuint(r.uvMap0.w));
    return rvkSample2DLod(r.albedoTex, r.albedoSampler, uv, lod);
}
