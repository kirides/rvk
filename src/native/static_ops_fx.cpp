// N5.6 scene ops: the main view's particle, strip, light streak and precipitation ops (docs/research/engine-frame/README.md
// "14. N5.6 result"). Part of StaticOps (static_ops.h); the GetPasses / ExecutePass rules are static_ops.cpp's, the CPU vertex
// fills re-written from the disassembly of the test copy's Engine.dll (helpers in B:\DevDrive\risen-ghidra\n56\):
//   * strips (eCStrip_PS): the RenderAlpha slot 0x701120 = UpdateVertices 0x6fd960 + RenderVertices 0x6fd800 when the strip has
//     >= 2 segments (PS +0xe8): 2 vertices per segment, colours lerped with SharedBase's bCFloatAlphaColor functions; generated
//     natively at the op's entry and compared byte for byte with the engine's dynamic buffer;
//   * light streaks (eCLightStreaks_PS::Render 0x5c4ae0): static buffers built once (CreateBuffers), eCShaderLightStreaks;
//   * particles (eCParticle_PS::Render 0x5e0080 -> RenderVertices 0x5d56a0, UpdateVertices 0x5d58b0);
//   * precipitation (the late list's slot 0x719b00).

#include <windows.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <format>

#include <spdlog/spdlog.h>

#include "d3d9/defs.h"
#include "engine/memread.h"
#include "engine/symbols.h"
#include "hal/vulkan/hal_vulkan.h"
#include "native/static_ops.h"

namespace rvk::native {

namespace {

using engine::readAt;
using engine::safeRead;
using U8 = const std::uint8_t;

template <class T>
T get(const void* p, std::size_t off, T fallback = T{}) {
    T v = fallback;
    if (!readAt(p, off, v)) return fallback;
    return v;
}
U8* ptr(const void* p, std::size_t off) { return get<U8*>(p, off, nullptr); }
bool rd(const void* p, std::size_t off, void* out, std::size_t n) { return p != nullptr && safeRead(static_cast<U8*>(p) + off, out, n); }

constexpr float kIdentity[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};

// SharedBase.dll's colour functions (the engine's strip / particle fills call them; bound by decorated name)
struct ColorFns {
    void (*floatColorCtor)(void* self) = nullptr;                                   // ??0bCFloatColor@@QEAA@XZ
    void (*setColor4)(void* self, float r, float g, float b, float a) = nullptr;   // ?SetColor@bCFloatAlphaColor@@QEAAXMMMM@Z
    void (*alphaCtor)(void* self, const void* colour, float a) = nullptr;           // ??0bCFloatAlphaColor@@QEAA@AEBVbCFloatColor@@M@Z
    void (*alphaCopy)(void* self, const void* other) = nullptr;                     // ??0bCFloatAlphaColor@@QEAA@AEBV0@@Z
    void (*sub)(void* self, const void* other) = nullptr;                           // ?Sub@bCFloatAlphaColor@@QEAAXAEBV1@@Z
    void (*multiply)(void* self, float f) = nullptr;                                // ?Multiply@bCFloatAlphaColor@@QEAAXM@Z
    void (*add)(void* self, const void* other) = nullptr;                           // ?Add@bCFloatAlphaColor@@QEAAXAEBV1@@Z
    void (*setColor)(void* self, const void* other) = nullptr;                      // ?SetColor@bCFloatAlphaColor@@QEAAXAEBV1@@Z
    void* (*byteCtor)(void* self, const void* floatColour) = nullptr;               // ??0bCByteAlphaColor@@QEAA@AEBVbCFloatAlphaColor@@@Z
    std::uint32_t (*argb)(const void* self) = nullptr;                              // ?GetColor_ARGB_8888@bCByteAlphaColor@@QEBAKXZ
    const void* alphaVtable = nullptr;                                              // ??_7bCFloatAlphaColor@@6B@
    bool ok() const {
        return floatColorCtor && setColor4 && alphaCtor && alphaCopy && sub && multiply && add && setColor && byteCtor && argb && alphaVtable;
    }
};
ColorFns g_color;

template <class F>
void bindFn(HMODULE m, F& fn, const char* name) {
    fn = reinterpret_cast<F>(reinterpret_cast<void*>(GetProcAddress(m, name)));
}

// a bCFloatAlphaColor on our stack: vtable +0, r +8, g +0xc, b +0x10, a +0x18 (0x20 bytes)
struct alignas(16) FColor {
    std::uint8_t b[0x40]{};
    float alpha() const { float a; std::memcpy(&a, b + 0x18, 4); return a; }
    void setAlpha(float a) { std::memcpy(b + 0x18, &a, 4); }
};
// bCFloatColor ctor + the alpha colour's vtable + SetColor(0, 0, 0, 0) (as UpdateVertices makes its working colours)
void zeroColor(FColor& c) {
    g_color.floatColorCtor(c.b);
    std::memcpy(c.b, &g_color.alphaVtable, 8);
    g_color.setColor4(c.b, 0.0f, 0.0f, 0.0f, 0.0f);
}
std::uint32_t argbOf(const FColor& c) {
    alignas(16) std::uint8_t tmp[0x40]{};
    return g_color.argb(g_color.byteCtor(tmp, c.b));
}

constexpr std::uint32_t kStripStride = 0x20;   // eCStrip_PS::CreateVertices: FVF 0x242, POSITION, COLOR0, TEXCOORD0, TEXCOORD1
constexpr std::uint32_t kSegmentStride = 0x24;  // the segments PS +0xe0 (PushSegment 0x6fe6a0)

}  // namespace

StaticOps::Skip StaticOps::composeFx(const Op& op) {
    U8* ps = ptr(op.raw.context, 0x18);
    if (ps == nullptr) return Skip::None;  // every slot: nothing without the property set
    Skip s = Skip::Kind;
    switch (op.kind) {
        case OpKind::Strip: s = composeStrip(op); break;
        case OpKind::LightStreaks: s = composeStreaks(op); break;
        case OpKind::Particle: s = composeParticle(op); break;
        case OpKind::Precipitation: s = composePrecipitation(op); break;
        default: break;
    }
    blendOverrideShader_ = nullptr;
    blendOverride_ = -1;
    fxPoolCount_ = 0;
    if (s != Skip::None) tagOverride_ = false;
    return s;
}

// ---- strips: the RenderAlpha slot 0x701120 --------------------------------------------------------------------------------------

StaticOps::Skip StaticOps::composeStrip(const Op& op) {
    U8* ps = ptr(op.raw.context, 0x18);
    const std::int32_t segments = get<std::int32_t>(ps, 0xe8);
    if (segments < 2) return Skip::None;  // 0x701120: cmp [PS+0xe8], 1; jle
    ++fxWindow_.stripOps;
    // UpdateVertices: CreateVertices (CreateVisual: the visual +0x118; the vertex data +0x110 made once with capacity +0xec x 64)
    U8* visual = ptr(ps, 0x118);
    U8* vdata = ptr(ps, 0x110);
    if (visual == nullptr || vdata == nullptr || ptr(vdata, 0x20) == nullptr) return Skip::NotCreated;
    if (segments > get<std::int32_t>(ps, 0xec)) return Skip::Unreadable;
    // RenderVertices: the visual and the material (+0x120); its shader (+0x70) or nothing
    U8* material = ptr(ps, 0x120);
    U8* shader = ptr(material, 0x70);
    if (material == nullptr || shader == nullptr) return Skip::None;
    if (!shaderClassOk(shader)) {
        skipShader_ = get<const void*>(shader, 0);
        return Skip::ShaderClass;
    }
    if (!view_.valid) {
        U8* a = ptr(shader, 0x220);
        if (a == nullptr || !composeView(a)) return Skip::Unreadable;
    }
    if (ptr(visual, 0x60) != nullptr) return Skip::VisualShader;
    const auto* prim = hal::vulkan::HalPrimitive::fromApi(ptr(visual, 0x10));
    if (prim == nullptr || prim->drawDesc().indexed) return Skip::Primitive;
    initMirror();
    fxWindow_.stripSegments += static_cast<std::uint32_t>(segments);

    // the vertex fill (UpdateVertices 0x6fd960): per segment i two vertices, u / v accumulated by +0x18 / PS +0x108 and +0x1c / PS
    // +0x10c after their use, colours lerped from colour A (PS +0x20, alpha +0x38) to B (+0x40, +0x58) at u / v, alpha x segment +0x20
    std::vector<std::uint8_t> seg(static_cast<std::size_t>(segments) * kSegmentStride);
    if (!rd(ptr(ps, 0xe0), 0, seg.data(), seg.size())) return Skip::Unreadable;
    const float lenU = get<float>(ps, 0x108), lenV = get<float>(ps, 0x10c);
    FColor c0, c1, a, b, t;
    zeroColor(c0);
    zeroColor(c1);
    g_color.alphaCtor(a.b, ps + 0x20, get<float>(ps, 0x38));
    g_color.alphaCtor(b.b, ps + 0x40, get<float>(ps, 0x58));
    std::vector<std::uint8_t> vb(static_cast<std::size_t>(segments) * 2 * kStripStride);
    float u = 0.0f, v = 0.0f;
    for (std::int32_t i = 0; i < segments; ++i) {
        const std::uint8_t* s = seg.data() + static_cast<std::size_t>(i) * kSegmentStride;
        float f[9];
        std::memcpy(f, s, sizeof(f));
        const float du = f[6] / lenU, dv = f[7] / lenV;
        g_color.alphaCopy(t.b, b.b);
        g_color.sub(t.b, a.b);
        g_color.multiply(t.b, u);
        g_color.add(t.b, a.b);
        g_color.setColor(c0.b, t.b);
        g_color.alphaCopy(t.b, b.b);
        g_color.sub(t.b, a.b);
        g_color.multiply(t.b, v);
        g_color.add(t.b, a.b);
        g_color.setColor(c1.b, t.b);
        c0.setAlpha(c0.alpha() * f[8]);
        c1.setAlpha(c1.alpha() * f[8]);
        const std::uint32_t argb0 = argbOf(c0), argb1 = argbOf(c1);
        std::uint8_t* d = vb.data() + static_cast<std::size_t>(i) * 2 * kStripStride;
        const float zero = 0.0f, one = 1.0f;
        std::memcpy(d + 0x00, &f[0], 12);
        std::memcpy(d + 0x0c, &argb0, 4);
        std::memcpy(d + 0x10, &u, 4);
        std::memcpy(d + 0x14, &zero, 4);
        std::memcpy(d + 0x18, &u, 4);
        std::memcpy(d + 0x1c, &zero, 4);
        u = u + du;
        std::memcpy(d + 0x20, &f[3], 12);
        std::memcpy(d + 0x2c, &argb1, 4);
        std::memcpy(d + 0x30, &v, 4);
        std::memcpy(d + 0x34, &one, 4);
        std::memcpy(d + 0x38, &v, 4);
        std::memcpy(d + 0x3c, &one, 4);
        v = v + dv;
    }

    // RenderVertices 0x6fd800: alpha = fade x clamp(age +0x134 / fade-in +0xa8, 0, 1) unless +0xa0 (or the fade-in is not > 0);
    // SetGlobalAlpha(alpha, 0), SetWorldMatrix(identity, 0), GetPasses(shader, switch +0xa4, null, 0, false, 0), per pass ExecutePass
    // + eCGfxVisual::Execute (a non-indexed strip of 2 n - 2 triangles from the record's first vertex)
    const float alpha = sc::stripAlpha(op.raw.fade, get<std::uint8_t>(ps, 0xa0) != 0, get<float>(ps, 0xa8), get<float>(ps, 0x134));
    writeAlpha(0, alpha);
    writeWorld(0, kIdentity);
    Geom g;
    if (!geometryOfVisual(visual, g)) return Skip::Primitive;
    const auto& dd = prim->drawDesc();
    g.minIndex = 0;
    g.numVertices = 0;
    g.startIndex = dd.startVertex;
    g.primCount = static_cast<std::uint32_t>(segments * 2 - 2);
    g.primType = static_cast<std::uint32_t>(dd.type);
    const std::size_t before = drawCount_;
    if (const Skip k = nonOpPasses(shader, nullptr, 0, g, get<std::int32_t>(ps, 0xa4)); k != Skip::None) return k;
    if (vb.size() > kOwnVbBytes) return Skip::Unreadable;
    for (std::size_t i = before; i < drawCount_; ++i) {
        sc::DrawDesc& x = *draws_[i];
        x.indexed = false;
        x.indices = nullptr;
        x.vertexStride = kStripStride;
        x.vertexBytes = vb;
    }
    return Skip::None;
}

// ---- light streaks: eCLightStreaks_PS::Render 0x5c4ae0 ------------------------------------------------------------------------

// static buffers (CreateBuffers 0x5c2450), eCShaderLightStreaks (its own version class 0x9d96f8: a directional pass + one per point
// light): no light streak op occurred at any sampled place (N5.6), so they are left to the engine (counted, not composed)
StaticOps::Skip StaticOps::composeStreaks(const Op&) {
    ++fxWindow_.streakOps;
    return Skip::Kind;
}

// ---- particles: the RenderAlpha slot 0x5d3360 -> eCParticle_PS::Render 0x5e0080 -----------------------------------------------

namespace {

// SharedBase's vector / matrix functions UpdateVertices 0x5d58b0 calls (by-value returns: the hidden return pointer after `this`)
struct VecFns {
    const float* (*zero)() = nullptr;                                           // ?GetZero@bCVector@@SAAEBV1@XZ
    const float* (*up)() = nullptr;                                             // ?GetUp@bCVector@@SAAEBV1@XZ
    void* (*addAssign)(float* self, const float* o) = nullptr;                  // ??YbCVector@@QEAAAEAV0@AEBV0@@Z
    void* (*scaledV)(const float* self, float* ret, const float* o) = nullptr;  // ?GetScaled@bCVector@@QEBA?AV1@AEBV1@@Z
    void* (*scaledF)(const float* self, float* ret, float f) = nullptr;         // ?GetScaled@bCVector@@QEBA?AV1@M@Z
    void (*transformDirection)(float* self, const float* m) = nullptr;          // ?TransformDirection@bCVector@@QEAAXAEBVbCMatrix@@@Z
    bool (*isEqual)(const float* self, const float* o) = nullptr;               // ?IsEqual@bCVector@@QEBA_NAEBV1@@Z
    void (*cross)(float* self, const float* a, const float* b) = nullptr;       // ?SetCrossProduct@bCVector@@QEAAXAEBV1@0@Z
    void* (*inverted)(const float* self, float* ret) = nullptr;                 // ?GetInverted@bCVector@@QEBA?AV1@XZ
    void* (*mulMatrix)(float* self, const float* m) = nullptr;                  // ??XbCVector@@QEAAAEAV0@AEBVbCMatrix@@@Z
    void* (*mulMatrix3)(float* self, const float* m3) = nullptr;                // ??XbCVector@@QEAAAEAV0@AEBVbCMatrix3@@@Z
    void* (*normalize)(float* self) = nullptr;                                  // ?Normalize@bCVector@@QEAAAEBV1@XZ
    void* (*normalized)(const float* self, float* ret) = nullptr;               // ?GetNormalized@bCVector@@QEBA?AV1@XZ
    void* (*translated)(const float* self, float* ret, const float* o) = nullptr;     // ?GetTranslated@bCVector@@QEBA?AV1@AEBV1@@Z
    void* (*invTranslated)(const float* self, float* ret, const float* o) = nullptr;  // ?GetInvTranslated@bCVector@@QEBA?AV1@AEBV1@@Z
    void (*matInvertedRef)(const float* self, float* out) = nullptr;            // ?GetInverted@bCMatrix@@QEBAXAEAV1@@Z
    void* (*matInverted)(const float* self, float* ret) = nullptr;              // ?GetInverted@bCMatrix@@QEBA?AV1@XZ
    void (*rotation)(float* m3, const float* axisAngle) = nullptr;              // ?SetToRotation@bCMatrix3@@QEAAXAEBVbCAxisAngle@@@Z
    void (*clamp)(void* colour) = nullptr;                                      // ?Clamp@bCFloatAlphaColor@@QEAAXXZ
    void* (*matMulAssign)(float* self, const float* m) = nullptr;               // ??XbCMatrix@@QEAAAEAV0@AEBV0@@Z
    void* (*timer)() = nullptr;                                                 // Engine.dll ?GetInstance@eCTimer@@SAAEAV1@XZ
    bool ok() const {
        return zero && up && addAssign && scaledV && scaledF && transformDirection && isEqual && cross && inverted && mulMatrix &&
               mulMatrix3 && normalize && normalized && translated && invTranslated && matInvertedRef && matInverted && rotation && clamp &&
               matMulAssign && timer;
    }
};
VecFns g_vec;

struct alignas(16) V {
    float v[4]{};
    float* p() { return v; }
    const float* p() const { return v; }
};
V scaled(const V& a, float f) {
    V r;
    g_vec.scaledF(a.p(), r.p(), f);
    return r;
}
V translated(const V& a, const V& b) {
    V r;
    g_vec.translated(a.p(), r.p(), b.p());
    return r;
}
V invTranslated(const V& a, const V& b) {
    V r;
    g_vec.invTranslated(a.p(), r.p(), b.p());
    return r;
}
V cross(const V& a, const V& b) {
    V r;
    g_vec.cross(r.p(), a.p(), b.p());
    return r;
}
V vec3(const std::uint8_t* at) {
    V r;
    std::memcpy(r.v, at, 12);
    return r;
}

// the depth sort's axis (DepthSortParticles 0x5d8680 writes it to 0xc6bde0..0xc6bdec; the comparator 0x5d8780 reads it)
thread_local float t_axis[4];
int particleDepthCmp(const void* a, const void* b) {
    float pa[3], pb[3];
    std::memcpy(pa, static_cast<const std::uint8_t*>(a) + 0xc, 12);
    std::memcpy(pb, static_cast<const std::uint8_t*>(b) + 0xc, 12);
    return sc::particleDepthOrder(t_axis, pa, pb);
}

constexpr std::uint32_t kParticleStride = 0xd0;             // the particles PS +0x388 (count +0x390)
constexpr std::uint32_t kParticleVertex = 0x30;             // CreateVisual 0x5d7620: POSITION, NORMAL, COLOR0, COLOR1, TEXCOORD0, 1
constexpr std::uint32_t kShaderParticleVtable = 0x9b6d38;   // ??_7eCShaderParticle@@6B@
constexpr std::uint32_t kVersionParticleVtable = 0x9d99f0;  // its version object (CreateShaderVersion 0x6d5e30, v 2 only)
constexpr std::uint32_t kParticleVtable = 0x9bbbd8;         // ??_7eCParticle_PS@@6B@
constexpr std::uint32_t kParticleAlphaSite = 0x6d5fc2;      // eCShaderParticle::SetGlobalAlpha: mov rdx, [rip + ms_pGlobalShaderConstants]

}  // namespace

// eCParticle_PS::UpdateVertices 0x5d58b0 re-written (the disassembly's order; every vector operation through the SharedBase function
// the engine calls, inline float arithmetic in the engine's order): the quads of this tick into `out`
bool StaticOps::particleVertices(const std::uint8_t* ps, const std::uint8_t* entity, std::vector<std::uint8_t>& out, std::uint32_t& quads) {
    quads = 0;
    const std::int32_t count = get<std::int32_t>(ps, 0x390);
    const std::int32_t coords = get<std::int32_t>(ps, 0xb0);
    alignas(16) float E[16];
    if (!rd(entity, 0x60, E, 64)) return false;
    // DepthSortParticles: the axis from the application camera (+0x38, else +0xc8) matrix +0x10 (column 2), times the entity matrix
    // for entity-relative particles; the engine's qsort with the comparator's rule on a copy of the particle array
    U8* app = ptr(engine::sym().appInstancePtr, 0);
    U8* appCam = ptr(app, 0x38);
    if (appCam == nullptr) appCam = ptr(app, 0xc8);
    if (appCam == nullptr) return false;
    alignas(16) float cm[16];
    if (!rd(appCam, 0x10, cm, 64)) return false;
    if (coords == 1) g_vec.matMulAssign(cm, E);
    t_axis[0] = cm[2];
    t_axis[1] = cm[6];
    t_axis[2] = cm[10];
    t_axis[3] = cm[14];
    std::vector<std::uint8_t> parts(static_cast<std::size_t>(count) * kParticleStride);
    if (!rd(ptr(ps, 0x388), 0, parts.data(), parts.size())) return false;
    if (count > 1) qsort_(parts.data(), static_cast<std::size_t>(count), kParticleStride, particleDepthCmp);
    ++fxWindow_.particleSorted;
    // the camera: ms_pActiveCamera +0x20 inverted (rows right, up, look, position)
    U8* cam = ptr(engine::sym().gfxActiveCamera, 0);
    alignas(16) float C[16], Ci[16];
    if (cam == nullptr || !rd(cam, 0x20, C, 64)) return false;
    g_vec.matInvertedRef(C, Ci);
    V R0, R1, R2, camPos;
    std::memcpy(R0.v, Ci + 0, 12);
    std::memcpy(R1.v, Ci + 4, 12);
    std::memcpy(R2.v, Ci + 8, 12);
    std::memcpy(camPos.v, Ci + 12, 12);
    V Eo;
    std::memcpy(Eo.v, g_vec.zero(), 12);
    V entityPos;
    std::memcpy(entityPos.v, E + 12, 12);
    if (coords == 0) g_vec.addAssign(Eo.p(), entityPos.p());
    const std::int32_t tx = get<std::int32_t>(ps, 0x228) + 1, ty = get<std::int32_t>(ps, 0x22c) + 1;
    const float invTx = one_ / static_cast<float>(tx), invTy = one_ / static_cast<float>(ty);
    float texX = 0.0f, texY = 0.0f;
    if (U8* img = ptr(ps, 0x440); img != nullptr) {
        const float w = static_cast<float>(get<std::int32_t>(img, 0x58)), h = static_cast<float>(get<std::int32_t>(img, 0x5c));
        if (w != 0.0f) texX = one_ / w;
        if (h != 0.0f) texY = one_ / h;
    }
    FColor col0, col1;
    zeroColor(col1);
    zeroColor(col0);
    const std::int32_t mode = get<std::int32_t>(ps, 0x170);
    const bool axisInEntity = get<std::int32_t>(ps, 0x198) == 1;
    const std::int32_t normalMode = get<std::int32_t>(ps, 0x2b0);
    const float sizeScale = get<float>(ps, 0x418), pivotA = get<float>(ps, 0x184), pivotB = get<float>(ps, 0x188);
    const bool pin = get<std::uint8_t>(ps, 0x200) != 0, rowMajor = get<std::uint8_t>(ps, 0x23b) != 0, blendFrames = get<std::uint8_t>(ps, 0x239) != 0;
    const V axis = vec3(ps + 0x178);
    // pinned corners (PS +0x200): eCEntityPropertySet::GetGeometryEntity(PS) +0x90 = the entity's position (no template flag)
    const V pinPos = coords == 0 ? vec3(entity + 0x90) : V{};
    out.resize(static_cast<std::size_t>(count) * 4 * kParticleVertex);
    alignas(16) float Mi[16]{};
    if (coords == 1) {
        g_vec.matInverted(E, Mi);
        Mi[12] = Mi[13] = Mi[14] = 0.0f;
    }
    for (std::int32_t i = 0; i < count; ++i) {
        const std::uint8_t* p = parts.data() + static_cast<std::size_t>(i) * kParticleStride;
        const auto f = [&](std::size_t off) {
            float x;
            std::memcpy(&x, p + off, 4);
            return x;
        };
        if (f(0xb8) > f(0xbc)) continue;
        std::uint32_t flags = 0;
        std::memcpy(&flags, p + 0xc8, 4);
        if ((flags & 1) != 0) continue;
        // FUN_1801accf0: no alpha, no size
        if (0.0f >= f(0x90) || 0.0f >= f(0xb0)) continue;
        if ((f(0x60) * f(0x60) + f(0x64) * f(0x64)) + f(0x68) * f(0x68) == 0.0f) continue;
        if ((f(0x6c) * f(0x6c) + f(0x70) * f(0x70)) + f(0x74) * f(0x74) == 0.0f) continue;
        const V pos = vec3(p + 0xc);
        V d;
        {
            const V a = vec3(p + 0x18), b = vec3(p + 0x24);
            g_vec.scaledV(a.p(), d.p(), b.p());
        }
        V L, R, U;
        const auto sideOf = [&]() {
            // modes 2 / 4: R from L (its z sign-flipped by xor), U = R x L
            float nz = L.v[2];
            std::uint32_t bits = 0;
            std::memcpy(&bits, &nz, 4);
            bits ^= 0x80000000u;
            std::memcpy(&nz, &bits, 4);
            if (!g_vec.isEqual(L.p(), g_vec.up())) {
                R.v[0] = nz;
                R.v[1] = 0.0f;
                R.v[2] = L.v[0];
            } else {
                R.v[0] = 0.0f;
                R.v[1] = nz;
                R.v[2] = L.v[1];
            }
            U = cross(R, L);
        };
        switch (mode) {
            case 1:
                L = R2;
                R = cross(d, L);
                U = cross(L, R);
                break;
            case 3:
                g_vec.inverted(axis.p(), L.p());
                if (axisInEntity) g_vec.transformDirection(L.p(), E);
                R = cross(d, L);
                U = cross(L, R);
                break;
            case 2:
                L = axis;
                if (axisInEntity) g_vec.transformDirection(L.p(), E);
                sideOf();
                break;
            case 4:
                L = d;
                if (axisInEntity) g_vec.transformDirection(L.p(), E);
                sideOf();
                break;
            default:
                L = R2;
                R = R0;
                U = R1;
                break;
        }
        if (coords == 1) {
            g_vec.mulMatrix(L.p(), Mi);
            g_vec.mulMatrix(U.p(), Mi);
            g_vec.mulMatrix(R.p(), Mi);
        }
        g_vec.normalize(R.p());
        g_vec.normalize(U.p());
        g_vec.normalize(L.p());
        const V s = scaled(vec3(p + 0x60), 0.5f);
        const float hy = (s.v[1] * f(0x70)) * sizeScale;
        const float hx = (s.v[0] * f(0x6c)) * sizeScale;
        const float am = (pivotA - one_) * hx, ap = (pivotA + one_) * hx, bm = (pivotB - one_) * hy, bp = (pivotB + one_) * hy;
        V c0 = translated(scaled(R, am), scaled(U, bp));
        V c3 = translated(scaled(R, ap), scaled(U, bp));
        V c1 = translated(scaled(R, am), scaled(U, bm));
        V c2 = translated(scaled(R, ap), scaled(U, bm));
        V n0 = translated(scaled(R, -one_), scaled(U, one_));
        V n3 = translated(scaled(R, one_), scaled(U, one_));
        V n1 = invTranslated(scaled(R, -one_), scaled(U, one_));
        V n2 = invTranslated(scaled(R, one_), scaled(U, one_));
        const float angles[3] = {f(0x48), f(0x4c), f(0x50)};
        for (int k = 0; k < 3; ++k) {
            if (!(angles[k] != 0.0f)) continue;
            const V& ax = k == 0 ? R : k == 1 ? U : L;
            alignas(16) float aa[4] = {ax.v[0], ax.v[1], ax.v[2], angles[k]};
            alignas(16) float m3[16]{};
            g_vec.rotation(m3, aa);
            for (V* x : {&c0, &c3, &c1, &c2, &n0, &n3, &n1, &n2, &L}) g_vec.mulMatrix3(x->p(), m3);
        }
        for (V* x : {&c0, &c3, &c1, &c2}) g_vec.addAssign(x->p(), pos.p());
        // the facing test: the look vector against the direction to the camera
        V P = pos, Lw = L;
        if (coords == 1) g_vec.addAssign(P.p(), entityPos.p());
        if (axisInEntity) g_vec.transformDirection(Lw.p(), E);
        V n;
        {
            const V t = invTranslated(camPos, P);
            g_vec.normalized(t.p(), n.p());
        }
        const float dot = ((Lw.v[1] * n.v[1]) + (Lw.v[0] * n.v[0])) + (Lw.v[2] * n.v[2]);
        if (0.0f > dot) {
            L.v[0] = L.v[0] * -one_;
            L.v[1] = L.v[1] * -one_;
            L.v[2] = L.v[2] * -one_;
        }
        if (normalMode == 0 || normalMode == 1) {
            n0 = n1 = n2 = n3 = L;
        } else if (normalMode == 2) {
            for (V* x : {&n0, &n3, &n1, &n2}) g_vec.addAssign(x->p(), L.p());
        } else if (normalMode == 3) {
            const V N = invTranslated(pos, Eo);
            L = N;
            n0 = n1 = n2 = n3 = N;
        }
        if (pin) c3 = c2 = pinPos;
        // sprite frames
        float u0 = 0, v0 = 0, u1 = 1, v1 = 1, u0b = 0, v0b = 0, u1b = 1, v1b = 1, frac = 0;
        if (tx > 1 || ty > 1) {
            const float fr = f(0xc0);
            const float fl = std::floor(fr);
            frac = fr - fl;
            const auto i0 = static_cast<std::int32_t>(fl);
            const auto i1 = static_cast<std::int32_t>(std::ceil(fr));
            const auto tile = [&](std::int32_t k, float& a0, float& a1, float& b0, float& b1) {
                const sc::ParticleTile t = sc::particleTile(k, tx, ty, rowMajor, invTx, invTy);
                a0 = t.u0;
                a1 = t.u1;
                b0 = t.v0;
                b1 = t.v1;
            };
            tile(i0, u0, u1, v0, v1);
            if (blendFrames) tile(i1, u0b, u1b, v0b, v1b);
        }
        u0 = u0 + texX;
        u0b = u0b + texX;
        v0 = v0 + texY;
        v0b = v0b + texY;
        // COLOR0 = (age ratio, |dot|, max(dot, 0), 1 - frac) unclamped; COLOR1 = the particle's colour x its tint, clamped
        const float c0f[4] = {f(0xbc) != 0.0f ? f(0xb8) / f(0xbc) : 0.0f, std::fabs(dot), dot > 0.0f ? dot : 0.0f, one_ - frac};
        std::memcpy(col0.b + 8, c0f, 12);
        col0.setAlpha(c0f[3]);
        const float c1f[3] = {f(0xa0) * f(0x80), f(0xa4) * f(0x84), f(0xa8) * f(0x88)};
        std::memcpy(col1.b + 8, c1f, 12);
        col1.setAlpha(f(0xb0) * f(0x90));
        g_vec.clamp(col1.b);
        const std::uint32_t argb0 = argbOf(col0), argb1 = argbOf(col1);
        std::uint8_t* q = out.data() + static_cast<std::size_t>(quads) * 4 * kParticleVertex;
        const auto vert = [&](std::uint32_t k, const V& c, const V& nn, float ua, float va, float ub, float vb) {
            std::uint8_t* x = q + k * kParticleVertex;
            std::memcpy(x, c.v, 12);
            std::memcpy(x + 0xc, nn.v, 12);
            std::memcpy(x + 0x18, &argb0, 4);
            std::memcpy(x + 0x1c, &argb1, 4);
            const float uv[4] = {ua, va, ub, vb};
            std::memcpy(x + 0x20, uv, 16);
        };
        vert(0, c0, n0, u0, v0, u0b, v0b);
        vert(1, c1, n1, u0, v1, u0b, v1b);
        vert(2, c2, n2, u1, v1, u1b, v1b);
        vert(3, c3, n3, u1, v0, u1b, v0b);
        ++quads;
    }
    out.resize(static_cast<std::size_t>(quads) * 4 * kParticleVertex);
    return true;
}

// eCShaderParticle's version (0x9d99f0) Execute 0x6d6c30 for the pass key: VS element version +0x1b0 + 16 ((dir + 2 ambient) 5 +
// lights), PS element +0x2f0 + 16 fog; the light constants (0x6d7020: nothing without an illumination), the states 0x6d9740
// (FillBlendingRenderStates with GetGlobalAlpha, the master depth write), ExecuteShaderConstants (eCShaderBase's map + the particle
// global pool)
StaticOps::Skip StaticOps::particlePassDraw(const std::uint8_t* shader, const std::uint8_t* version, std::uint32_t key, const Geom& geometry,
                                            float alpha) {
    Skip skip = Skip::None;
    applySwitch(shader, 0, skip);
    if (skip != Skip::None) return skip;
    const std::uint32_t fog = key & 1u, ambient = key >> 1 & 1u, dir = key >> 2 & 3u, lights = key >> 4 & 7u;
    const std::uint32_t vsIdx = (dir + 2 * ambient) * 5 + lights;
    U8* vsElem = ptr(version, 0x1b0 + 16 * vsIdx);
    U8* psElem = ptr(version, 0x2f0 + 16 * fog);
    if (vsElem == nullptr || psElem == nullptr) {
        skipWhy_ = std::format("particle pass key {:#x}: vs {} / ps {} not compiled", key, vsIdx, fog);
        return Skip::NotCompiled;
    }
    d3d9::Shader* vs = shaderOfElement(vsElem);
    d3d9::Shader* ps = shaderOfElement(psElem);
    if (vs == nullptr || ps == nullptr) return Skip::NotCompiled;
    sc::BlendInputs b;
    b.mode = blendModeOf(shader);
    b.maskRef = maskRefOf(shader);
    b.hasOpacity = false;  // eCShaderParticle::HasOpacity 0x6d5f60: false
    b.refracted = false;   // eCShaderBase::IsRefracted: false
    b.tradeAlphaAsSolid = get<std::uint8_t>(shader, 0x79) != 0;
    const sc::RenderStates rs = sc::skyDomeStates(b, alpha);
    if (rs.alphaTest == 2) mirror_.adminPs[5][3] = static_cast<float>(rs.alphaRef) * (1.0f / 256.0f);
    sc::DrawDesc& d = drawFrom(geometry);
    d.drawClass = sc::DrawClass::NonOp;
    d.vs = vs;
    d.ps = ps;
    d.states = sc::deviceStates(rs, geometry.visual, get<std::uint32_t>(view_.admin, 0x340));
    constantsInto(d, shader, shader, false);
    if (const Skip s = materialSamplers(d, shader, psElem); s != Skip::None) return s;
    return adminSamplers(d, shader, ps);
}

StaticOps::Skip StaticOps::composeParticle(const Op& op) {
    U8* ps = ptr(op.raw.context, 0x18);
    ++fxWindow_.particleOps;
    if (get<const void*>(ps, 0) != engine_ + kParticleVtable) return Skip::ShaderClass;
    // Render: an instanced emitter draws its master's vertices (eCParticleModule::GetMasterInstance): left to the engine
    if (get<std::uint8_t>(ps, 0x29d) != 0) {
        ++fxWindow_.instancedEmitters;
        return Skip::Instanced;
    }
    // Render: no live particle -> the emitter is simulated inside the slot (vtable +0x248) before RenderVertices
    const std::int32_t live = get<std::int32_t>(ps, 0x390);
    if (live <= 0) return Skip::Simulated;
    const std::int32_t coords = get<std::int32_t>(ps, 0xb0);
    if (coords != 0 && coords != 1) return Skip::Unreadable;  // RenderVertices: no world matrix
    if ((get<std::uint8_t>(ps, 0x18) & 0x10) != 0) return Skip::Unreadable;
    U8* entity = ptr(ps, 0x10);
    U8* visual = ptr(ps, 0x420);
    U8* vdata = ptr(ps, 0x428);
    U8* material = ptr(ps, 0x450);
    if (entity == nullptr || visual == nullptr || vdata == nullptr || ptr(vdata, 0x20) == nullptr || ptr(ps, 0x430) == nullptr) return Skip::NotCreated;
    if (material == nullptr) return Skip::NotCreated;  // CreateMaterial would make it inside the slot
    U8* shader = ptr(material, 0x70);
    if (shader == nullptr) return Skip::None;  // ApplyRenderStates: no eCShaderParticle -> nothing drawn
    if (get<const void*>(shader, 0) != engine_ + kShaderParticleVtable) {
        skipShader_ = get<const void*>(shader, 0);
        return Skip::ShaderClass;
    }
    U8* sampMat = ptr(ps, 0x448);
    U8* image = ptr(ps, 0x440);
    if (sampMat != nullptr && image == nullptr) return Skip::NotCreated;  // CreateImage inside the slot
    U8* admin = ptr(shader, 0x220);
    if (admin == nullptr) return Skip::Unreadable;
    if (!view_.valid && !composeView(admin)) return Skip::Unreadable;
    if (admin != view_.admin) return Skip::Unreadable;
    if (get<std::uint8_t>(shader, 0x2a8) == 1 || get<std::uint32_t>(view_.admin, 0x31c) != 0) return Skip::InvalidShader;
    if (ptr(visual, 0x60) != nullptr) return Skip::VisualShader;
    const auto* prim = hal::vulkan::HalPrimitive::fromApi(ptr(visual, 0x10));
    if (prim == nullptr || !prim->drawDesc().indexed) return Skip::Primitive;
    // RenderPasses: lighting (PS +0x2b0) passes the entity's illumination; its light constants (0x6d7020) are not composed
    if (get<std::int32_t>(ps, 0x2b0) != 0) {
        ++fxWindow_.lit;
        return Skip::PassKind;
    }
    ++fxWindow_.orient[static_cast<std::size_t>(std::clamp(get<std::int32_t>(ps, 0x170), 0, 5))];
    ++fxWindow_.coords[static_cast<std::size_t>(coords)];
    if (get<std::uint8_t>(ps, 0x239) != 0) ++fxWindow_.frameBlend;
    initMirror();

    // the vertex data: UpdateVertices runs once per timer tick (eCTimer +0x50 vs PS +0x3b0); in a tick already updated the engine's
    // buffer holds this tick's quads (read back), else they are generated natively
    std::vector<std::uint8_t> vb;
    std::uint32_t quads = 0;
    U8* timer = static_cast<U8*>(g_vec.timer());
    if (timer == nullptr) return Skip::Unreadable;
    const auto& dd = prim->drawDesc();
    if (get<std::int32_t>(ps, 0x3b0) == get<std::int32_t>(timer, 0x50)) {
        quads = dd.primitiveCount / 2;
        const auto sv = prim->streamView(0);
        auto* buf = const_cast<d3d9::Buffer*>(static_cast<const d3d9::Buffer*>(sv.buffer));
        vb.resize(static_cast<std::size_t>(quads) * 4 * kParticleVertex);
        if (buf == nullptr || (!vb.empty() && !buf->readCpu(sv.offset, static_cast<std::uint32_t>(vb.size()), vb.data()))) return Skip::Unreadable;
        ++fxWindow_.particleEngineBytes;
    } else {
        if (live > get<std::int32_t>(ps, 0x394)) return Skip::Unreadable;  // more than the buffer holds (UpdateVertices has no check)
        if (!particleVertices(ps, entity, vb, quads)) return Skip::Unreadable;
    }
    fxWindow_.particleQuads += quads;
    if (quads == 0) {
        // UpdateVertices returns primitives != 0 (0x5d6e13): live particles but no quad built, nothing drawn
        ++fxWindow_.particleZeroQuads;
        return Skip::None;
    }
    if (vb.size() > kOwnVbBytes) return Skip::Unreadable;  // beyond our own CPU-geometry buffer

    // RenderVertices: the world = identity (world coordinates) or the entity's matrix; ApplyRenderStates 0x5d6e50
    float world[16];
    std::memcpy(world, kIdentity, 64);
    if (coords == 1 && !rd(entity, 0x60, world, 64)) return Skip::Unreadable;
    const float fade = op.raw.fade;
    // the material's samplers get the particle image (eCParticleMaterialSimple / BlendSubdivisions::SetImage: +0x10 (+0x18) ->
    // eCColorSrcSampler::SetImageResource)
    if (sampMat != nullptr) {
        const void* vt = get<const void*>(sampMat, 0);
        const bool two = vt == engine_ + 0x9b7170;
        if (vt != engine_ + 0x9b7160 && !two) return Skip::Unreadable;
        for (std::size_t off : {std::size_t{0x10}, std::size_t{0x18}}) {
            if (off == 0x18 && !two) break;
            U8* sampler = ptr(sampMat, off);
            if (sampler == nullptr) continue;
            if (ptr(sampler, 0x90) != shader + 0xf8) return Skip::Unreadable;
            const std::int32_t slot = get<std::int32_t>(sampler, 0x98, -1);
            if (slot < 0 || slot >= 16) return Skip::Unreadable;
            poolsOf(shader)->slotImage[static_cast<std::size_t>(slot)] = image;
        }
    }
    // BlendMode from PS +0x220 (0 / other: AlphaBlend below a fade of 0.9, else Normal), MaskReference, EnableFog = !PS +0x298
    const std::int32_t blend = sc::particleBlendMode(get<std::int32_t>(ps, 0x220), fade);
    blendOverrideShader_ = shader;
    blendOverride_ = blend;
    maskRefShader_ = shader;
    maskRefOverride_ = get<std::uint8_t>(ps, 0x299) != 0 ? get<std::uint8_t>(ps, 0x29a) : std::uint8_t{0};
    fogOverride_ = get<std::uint8_t>(ps, 0x298) == 0;
    // the particle global pool (c31..c60): world c31..c33 (FUN_180238390: the 3x4 transpose), global alpha c34.w (SetGlobalAlpha)
    U8* global = ptr(particleGlobalVar_, 0);
    if (global == nullptr || get<std::int32_t>(global, 0x24) != 31) return Skip::Unreadable;
    fxPoolFirst_ = 31;
    fxPoolCount_ = 30;
    fxPoolNativeLast_ = 34;
    if (!rd(ptr(global, 0x38), 0, fxPool_, 30 * 16)) {
        fxPoolCount_ = 0;
        return Skip::Unreadable;
    }
    for (std::uint32_t k = 0; k < 3; ++k) {
        fxPool_[k][0] = world[k];
        fxPool_[k][1] = world[4 + k];
        fxPool_[k][2] = world[8 + k];
        fxPool_[k][3] = world[12 + k];
    }
    fxPool_[3][3] = fade;
    // eCShaderBase::OnGetPasses: the version min(admin +0x170, +0x68), falling back to a lower existing one; its GetPasses 0x6d6d50:
    // one pass, key = EnableFog (no illumination)
    std::int32_t v = std::min(get<std::int32_t>(view_.admin, 0x170), get<std::int32_t>(shader, 0x68));
    U8* version = ptr(shader, 0x258 + 8 * static_cast<std::size_t>(std::max(v, 0)));
    while (version == nullptr && v > 0) version = ptr(shader, 0x258 + 8 * static_cast<std::size_t>(--v));
    Skip result = Skip::None;
    if (version != nullptr && get<const void*>(version, 0) != engine_ + kVersionParticleVtable) {
        result = Skip::Version;
    } else if (version != nullptr) {
        Geom g;
        if (!geometryOfVisual(visual, g)) {
            result = Skip::Primitive;
        } else {
            // the visual as ApplyRenderStates sets it: solid, no culling, depth test PS +0x29b ? LessEqual : Always, write +0x29c
            g.visual.fill = 3;
            g.visual.cull = 1;
            g.visual.depthFunc = get<std::uint8_t>(ps, 0x29b) != 0 ? 4u : 8u;
            g.visual.depthWrite = get<std::uint8_t>(ps, 0x29c) != 0 ? 2u : 1u;
            g.minIndex = dd.minVertexIndex;
            g.numVertices = dd.numVertices;
            g.startIndex = dd.startIndex;
            g.primCount = quads * 2;
            g.primType = static_cast<std::uint32_t>(dd.type);
            g.baseVertex = dd.baseVertexIndex;
            const std::size_t before = drawCount_;
            result = particlePassDraw(shader, version, fogOverride_ ? 1u : 0u, g, fade);
            for (std::size_t i = before; i < drawCount_ && result == Skip::None; ++i) {
                sc::DrawDesc& x = *draws_[i];
                x.vertexStride = kParticleVertex;
                x.vertexBytes = vb;
            }
        }
    }
    fxPoolCount_ = 0;
    if (result == Skip::None && drawCount_ != 0) {
        // eCParticle_PS::Render's draw tag (engine/scene_hooks: TagScope(Particle, the property set, the executing op, PS +0x10))
        tagOverride_ = true;
        tagOverrideValue_ = {};
        tagOverrideValue_.source = d3d9::draws::Source::Particle;
        tagOverrideValue_.context = ps;
        tagOverrideValue_.op = dev_.drawTag().op;
        tagOverrideValue_.entity = entity;
    }
    return result;
}

// ---- precipitation ----------------------------------------------------------------------------------------------------------------

// The slot 0x719b00 (= eCPrecipitation_PS::RenderAlpha 0x71d1a0 inlined): when PS +0x18 & 1, the embedded eCSkyObject at PS +0x108
// (vtable 0x989f28; its slot +0x10 = eCSkyObject::Render 0x6e25e0) is rendered with the identity world. Its billboard array (sky
// +0x10, 0x30-byte records) is written by eCPrecipitation_PS::Update (OnProcess, before the frame's rendering: one record per
// live drop, compacted; the primitive count = 2 x records) and is final at the slot's entry; FillVertexBuffer 0x6e1a50 expands
// each record into 4 vertices (the sky objects' rule, sc::skyBillboardVertices, N5.2c), locked DISCARD while the dirty flag +0x49
// is set. The shader (sky +0x20 = PS +0xf8) is a code-built eCShaderDefault (BlendMode 2, TransformationType 7, DisableLighting).
StaticOps::Skip StaticOps::composePrecipitation(const Op& op) {
    U8* ps = ptr(op.raw.context, 0x18);
    if ((get<std::uint8_t>(ps, 0x18) & 1) == 0) {
        ++fxWindow_.precipOff;
        return Skip::None;
    }
    ++fxWindow_.precipOps;
    U8* sky = ps + 0x108;
    if (get<const void*>(sky, 0) != engine_ + 0x989f28) return Skip::Unreadable;  // ??_7eCSkyObject@@6B@
    U8* shader = ptr(sky, 0x20);
    if (shader == nullptr || ptr(sky, 0x28) == nullptr || ptr(sky, 0x40) == nullptr) return Skip::None;  // Render returns early
    if (!shaderClassOk(shader)) {
        skipShader_ = get<const void*>(shader, 0);
        return Skip::ShaderClass;
    }
    if (!view_.valid) {
        U8* a = ptr(shader, 0x220);
        if (a == nullptr || !composeView(a)) return Skip::Unreadable;
    }
    initMirror();
    const std::size_t before = drawCount_;
    if (const Skip k = skyObjectDraws(sky, kIdentity, nullptr, 0); k != Skip::None) return k;
    for (std::size_t i = before; i < drawCount_; ++i) {
        if (draws_[i]->vertexBytes.size() + draws_[i]->minIndex * sc::kSkyVertexStride > kOwnVbBytes) return Skip::Unreadable;
        draws_[i]->drawClass = sc::DrawClass::NonOp;
        fxWindow_.precipDrops += draws_[i]->primCount / 2;
    }
    return Skip::None;
}

bool StaticOps::verifyFx() {
    HMODULE sb = GetModuleHandleW(L"SharedBase.dll");
    if (sb == nullptr || engine_ == nullptr) return false;
    bindFn(sb, g_color.floatColorCtor, "??0bCFloatColor@@QEAA@XZ");
    bindFn(sb, g_color.setColor4, "?SetColor@bCFloatAlphaColor@@QEAAXMMMM@Z");
    bindFn(sb, g_color.alphaCtor, "??0bCFloatAlphaColor@@QEAA@AEBVbCFloatColor@@M@Z");
    bindFn(sb, g_color.alphaCopy, "??0bCFloatAlphaColor@@QEAA@AEBV0@@Z");
    bindFn(sb, g_color.sub, "?Sub@bCFloatAlphaColor@@QEAAXAEBV1@@Z");
    bindFn(sb, g_color.multiply, "?Multiply@bCFloatAlphaColor@@QEAAXM@Z");
    bindFn(sb, g_color.add, "?Add@bCFloatAlphaColor@@QEAAXAEBV1@@Z");
    bindFn(sb, g_color.setColor, "?SetColor@bCFloatAlphaColor@@QEAAXAEBV1@@Z");
    bindFn(sb, g_color.byteCtor, "??0bCByteAlphaColor@@QEAA@AEBVbCFloatAlphaColor@@@Z");
    bindFn(sb, g_color.argb, "?GetColor_ARGB_8888@bCByteAlphaColor@@QEBAKXZ");
    g_color.alphaVtable = reinterpret_cast<const void*>(GetProcAddress(sb, "??_7bCFloatAlphaColor@@6B@"));
    if (!g_color.ok()) {
        spdlog::warn("native: scene ops: SharedBase colour functions missing: particles / strips stay the engine's");
        return false;
    }
    bindFn(sb, g_vec.zero, "?GetZero@bCVector@@SAAEBV1@XZ");
    bindFn(sb, g_vec.up, "?GetUp@bCVector@@SAAEBV1@XZ");
    bindFn(sb, g_vec.addAssign, "??YbCVector@@QEAAAEAV0@AEBV0@@Z");
    bindFn(sb, g_vec.scaledV, "?GetScaled@bCVector@@QEBA?AV1@AEBV1@@Z");
    bindFn(sb, g_vec.scaledF, "?GetScaled@bCVector@@QEBA?AV1@M@Z");
    bindFn(sb, g_vec.transformDirection, "?TransformDirection@bCVector@@QEAAXAEBVbCMatrix@@@Z");
    bindFn(sb, g_vec.isEqual, "?IsEqual@bCVector@@QEBA_NAEBV1@@Z");
    bindFn(sb, g_vec.cross, "?SetCrossProduct@bCVector@@QEAAXAEBV1@0@Z");
    bindFn(sb, g_vec.inverted, "?GetInverted@bCVector@@QEBA?AV1@XZ");
    bindFn(sb, g_vec.mulMatrix, "??XbCVector@@QEAAAEAV0@AEBVbCMatrix@@@Z");
    bindFn(sb, g_vec.mulMatrix3, "??XbCVector@@QEAAAEAV0@AEBVbCMatrix3@@@Z");
    bindFn(sb, g_vec.normalize, "?Normalize@bCVector@@QEAAAEBV1@XZ");
    bindFn(sb, g_vec.normalized, "?GetNormalized@bCVector@@QEBA?AV1@XZ");
    bindFn(sb, g_vec.translated, "?GetTranslated@bCVector@@QEBA?AV1@AEBV1@@Z");
    bindFn(sb, g_vec.invTranslated, "?GetInvTranslated@bCVector@@QEBA?AV1@AEBV1@@Z");
    bindFn(sb, g_vec.matInvertedRef, "?GetInverted@bCMatrix@@QEBAXAEAV1@@Z");
    bindFn(sb, g_vec.matInverted, "?GetInverted@bCMatrix@@QEBA?AV1@XZ");
    bindFn(sb, g_vec.rotation, "?SetToRotation@bCMatrix3@@QEAAXAEBVbCAxisAngle@@@Z");
    bindFn(sb, g_vec.clamp, "?Clamp@bCFloatAlphaColor@@QEAAXXZ");
    bindFn(sb, g_vec.matMulAssign, "??XbCMatrix@@QEAAAEAV0@AEBV0@@Z");
    if (HMODULE e = GetModuleHandleW(L"Engine.dll")) bindFn(e, g_vec.timer, "?GetInstance@eCTimer@@SAAEAV1@XZ");
    if (!g_vec.ok() || engine::sym().gfxActiveCamera == nullptr || engine::sym().appInstancePtr == nullptr) {
        spdlog::warn("native: scene ops: SharedBase vector functions / engine symbols missing: particles stay the engine's");
        return false;
    }
    // eCShaderParticle::SetGlobalAlpha +0x22: mov rdx, [rip + rel32] (eCShaderParticle::ms_pGlobalShaderConstants)
    std::uint8_t b[7] = {};
    if (!safeRead(engine_ + kParticleAlphaSite, b, sizeof(b)) || b[0] != 0x48 || b[1] != 0x8b || b[2] != 0x15) {
        spdlog::warn("native: scene ops: eCShaderParticle::SetGlobalAlpha differs: particles stay the engine's");
        return false;
    }
    std::int32_t rel = 0;
    std::memcpy(&rel, b + 3, 4);
    particleGlobalVar_ = engine_ + kParticleAlphaSite + 7 + rel;
    // the vtables the composition recognises
    static constexpr std::pair<std::uint32_t, const char*> kVtables[] = {
        {kShaderParticleVtable, "??_7eCShaderParticle@@6B@"}, {kParticleVtable, "??_7eCParticle_PS@@6B@"},
        {0x989f28, "??_7eCSkyObject@@6B@"}, {0x9b7160, "??_7eCParticleMaterialSimple@@6B@"},
        {0x9b7170, "??_7eCParticleMaterialBlendSubdivisions@@6B@"}};
    HMODULE em = GetModuleHandleW(L"Engine.dll");
    for (const auto& [rva, name] : kVtables) {
        if (reinterpret_cast<const std::uint8_t*>(GetProcAddress(em, name)) != engine_ + rva) {
            spdlog::warn("native: scene ops: {} is not at RVA {:#x}: particles / precipitation stay the engine's", name, rva);
            return false;
        }
    }
    // the particle version class's GetPasses / Execute, the shared state function
    static constexpr std::pair<std::uint32_t, std::uint32_t> kSlots[] = {{0x18, 0x6d6d50}, {0x20, 0x6d6c30}};
    for (const auto& [off, fn] : kSlots) {
        const void* v = nullptr;
        if (!readAt(engine_ + kVersionParticleVtable, off, v) || v != engine_ + fn) {
            spdlog::warn("native: scene ops: the particle version vtable +{:#x} is not RVA {:#x}: particles stay the engine's", off, fn);
            return false;
        }
    }
    // eCShaderParticle's slots the composition relies on (SetSwitch, ExecutePass, OnGetPasses: eCShaderBase's; HasOpacity false)
    static constexpr std::pair<std::uint32_t, std::uint32_t> kShaderSlots[] = {
        {0xf0, 0x6d2840}, {0x190, 0x6d5f60}, {0x198, 0x6d2ef0}, {0x1a8, 0x6d2f60}, {0x1c0, 0x6d2f90}, {0x1c8, 0x6d3170}, {0x1d0, 0x6d2060},
        {0x208, 0x6d5f80}, {0x210, 0x6d5fa0}, {0x218, 0x6d6030}};
    for (const auto& [off, fn] : kShaderSlots) {
        const void* v = nullptr;
        if (!readAt(engine_ + kShaderParticleVtable, off, v) || v != engine_ + fn) {
            spdlog::warn("native: scene ops: eCShaderParticle vtable +{:#x} is not RVA {:#x}: particles stay the engine's", off, fn);
            return false;
        }
    }
    spdlog::info("native: scene ops: particle / strip / light streak / precipitation checks passed");
    return true;
}

}  // namespace rvk::native
