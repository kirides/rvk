// N5.2c scene ops: the main view's sky dome op (list +0x1530, docs/research/engine-frame/README.md "9. N5.2c result"), re-written
// from eCSkydome_PS::Render 0x6e4730 (disassembled; dumps n52c_dump1.txt, skyrender*_dump.txt): the camera hack (the active
// camera's view translation zeroed, projection _33 = 1 / _43 = 0, its inverse view and view-projection recomputed with
// SharedBase's bCMatrix functions, eCGfxCamera::Execute -> SetCamera), the stars / sun / moon through eCSkyObject::Render
// 0x6e25e0 (eCShaderDefault billboard shaders; Render writes their BlendMode, sampler image and colour constant first; the
// sun's billboard is rewritten every frame), the camera restored the same way, then the dome (eCShaderSkyDome,
// eCShaderVersionSkyDome_2_0: one pass, its own element offsets and states, eCShaderBase's register map). Part of StaticOps
// (static_ops.h); the GetPasses / ExecutePass rules of the default shader are static_ops.cpp's.

#include <windows.h>

#include <algorithm>
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

void column(const float m[16], std::uint32_t k, float out[4]) {
    out[0] = m[k];
    out[1] = m[4 + k];
    out[2] = m[8 + k];
    out[3] = m[12 + k];
}

constexpr std::uint32_t kSkydomeRender = 0x6e4730;   // eCSkydome_PS::Render
constexpr std::uint32_t kSkyDomeVtable = 0x9da1e8;   // ??_7eCShaderSkyDome@@6B@
constexpr std::uint32_t kVersionSkyDome = 0x9da420;  // eCShaderVersionSkyDome_2_0 (unexported)
constexpr std::pair<std::uint32_t, std::uint32_t> kSkyDomeSlots[] = {
    {0xf0, 0x6d2840},   // SetSwitch (eCShaderBase)
    {0x190, 0x6d7b80},  // HasOpacity: false
    {0x198, 0x6d2ef0},  // IsBlended
    {0x1a8, 0x6d2f60},  // IsRefracted: false
    {0x1b8, 0x6d2f80},  // GetGlobalShaderConstants: null (no global pool)
    {0x1c0, 0x6d2f90},  // ExecuteShaderConstants
    {0x1c8, 0x6d3170},  // OnGetPasses
    {0x1d0, 0x6d2060},  // ExecutePass
    {0x218, 0x6d33d0},  // GetGlobalAlpha: 1.0
};
constexpr std::pair<std::uint32_t, std::uint32_t> kVersionSkyDomeSlots[] = {
    {0x18, 0x6d91a0},  // GetPasses: one pass {version, flags 0, illumination, switch}
    {0x20, 0x6d90d0},  // Execute: elements +0x1a8 / +0x1c8 (+0x1b8 / +0x1d8 with SunOcclusion +0x324), states 0x6d9740
    {0x38, 0x6d9740},  // the states
    {0x58, 0x6d9800},  // global constants: none
};
// instruction sites of eCSkydome_PS::Render whose meaning the composition relies on
struct Bytes {
    std::uint32_t rva;
    std::array<std::uint8_t, 8> b;
    std::uint32_t n;
};
constexpr Bytes kRenderSites[] = {
    {0x6e47fa, {0x48, 0xc7, 0x45, 0xb0, 0, 0, 0, 0}, 8},     // view copy: _41 = _42 = 0
    {0x6e4806, {0xc7, 0x45, 0xb8, 0, 0, 0, 0}, 7},           // _43 = 0
    {0x6e4887, {0xc7, 0x45, 0xe8, 0, 0, 0x80, 0x3f}, 7},     // projection copy: _33 = 1
    {0x6e488e, {0xc7, 0x45, 0xf8, 0, 0, 0, 0}, 7},           // _43 = 0
    {0x6e48f7, {0xc7, 0x43, 0x50, 7, 0, 0, 0}, 7},           // stars: BlendMode 7
    {0x6e4acb, {0xc7, 0x43, 0x50, 5, 0, 0, 0}, 7},           // moon: BlendMode 5
    {0x6e4998, {0xf3, 0x0f, 0x10, 0x05, 0xb4, 0xce, 0x30, 0}, 7},  // sun billboard z = DAT_1809f1854 (-1)
};

}  // namespace

bool StaticOps::verifySky() {
    const auto& s = engine::sym();
    if (s.gfxActiveCamera == nullptr || s.shaderSkyDomeVftable == nullptr || s.matrixInvert == nullptr || s.matrixMul == nullptr ||
        s.matrixMulAssign == nullptr || s.matrixGetInverted == nullptr || s.skydomeRender == nullptr || engine_ == nullptr) {
        spdlog::warn("native: scene ops: sky symbols missing: the sky stays the engine's");
        return false;
    }
    if (s.skydomeRender != engine_ + kSkydomeRender || s.shaderSkyDomeVftable != engine_ + kSkyDomeVtable) {
        spdlog::warn("native: scene ops: eCSkydome_PS::Render / eCShaderSkyDome's vtable not at the expected RVAs: the sky stays the engine's");
        return false;
    }
    const auto slotsOk = [&](std::uint32_t vtable, auto& slots, const char* what) {
        for (const auto& [off, fn] : slots) {
            const void* v = nullptr;
            if (!readAt(engine_ + vtable, off, v) || v != engine_ + fn) {
                spdlog::warn("native: scene ops: {} vtable +{:#x} is not RVA {:#x}: the sky stays the engine's", what, off, fn);
                return false;
            }
        }
        return true;
    };
    if (!slotsOk(kSkyDomeVtable, kSkyDomeSlots, "eCShaderSkyDome") || !slotsOk(kVersionSkyDome, kVersionSkyDomeSlots, "eCShaderVersionSkyDome_2_0")) {
        return false;
    }
    for (const Bytes& b : kRenderSites) {
        std::array<std::uint8_t, 8> got{};
        if (!safeRead(engine_ + b.rva, got.data(), b.n) || !std::equal(got.begin(), got.begin() + b.n, b.b.begin())) {
            spdlog::warn("native: scene ops: eCSkydome_PS::Render differs at RVA {:#x}: the sky stays the engine's", b.rva);
            return false;
        }
    }
    matInvert_ = reinterpret_cast<MatInvertFn>(s.matrixInvert);
    matMul_ = reinterpret_cast<MatMulFn>(s.matrixMul);
    matMulAssign_ = reinterpret_cast<MatMulAssignFn>(s.matrixMulAssign);
    matInverted_ = reinterpret_cast<MatInvertedFn>(s.matrixGetInverted);
    spdlog::info("native: scene ops: sky checks passed (Render sites, eCShaderSkyDome / eCShaderVersionSkyDome_2_0 slots)");
    return true;
}

void StaticOps::setCamera(const float view[16], const float proj[16], const float dbProj[16]) {
    // eCSkydome_PS::Render: +0x120 = view; Invert(+0x120); +0xe0 = proj * view (bCMatrix::operator*); then eCGfxCamera::Execute
    // -> SetCamera: admin +0x184 = +0xe0, +0x1c4 = +0xa0 *= +0x20, c4..c7 = GetInverted(+0x60), c8..c10 = +0x120 (3 rows)
    alignas(16) float v[16], p[16], inv[16], vp[16], invProj[16], db[16];
    std::memcpy(v, view, 64);
    std::memcpy(p, proj, 64);
    std::memcpy(inv, view, 64);
    matInvert_(inv);
    matMul_(p, vp, v);
    matInverted_(p, invProj);
    std::memcpy(db, dbProj, 64);
    matMulAssign_(db, v);
    for (std::uint32_t k = 0; k < 4; ++k) {
        column(vp, k, view_.vp[k]);
        column(db, k, view_.dbvp[k]);
        column(invProj, k, view_.invProj[k]);
        std::memcpy(mirror_.adminVs[4 + k], view_.invProj[k], 16);
    }
    for (std::uint32_t k = 0; k < 3; ++k) {
        view_.invView[k][0] = inv[k];
        view_.invView[k][1] = inv[4 + k];
        view_.invView[k][2] = inv[8 + k];
        std::memcpy(mirror_.adminVs[8 + k], view_.invView[k], 12);
    }
}

StaticOps::Skip StaticOps::skyObjectDraws(const std::uint8_t* object, const float* world, const std::uint8_t* bb, std::uint32_t renderCount) {
    U8* shader = ptr(object, 0x20);
    U8* visual = ptr(object, 0x40);
    // eCSkyObject::Render: a shader, FillVertexBuffer (vertex data +0x28, billboards to draw = the record's primitives / 2)
    if (shader == nullptr || ptr(object, 0x28) == nullptr || visual == nullptr) return Skip::None;
    const auto* prim = hal::vulkan::HalPrimitive::fromApi(ptr(visual, 0x10));
    if (prim == nullptr) return Skip::Primitive;
    const auto& dd = prim->drawDesc();
    const std::uint32_t prims = renderCount != 0 ? renderCount * 2 : dd.primitiveCount;
    const std::uint32_t n = prims >> 1;
    if (n == 0) return Skip::None;
    if (!dd.indexed || dd.startIndex % 6 != 0) return Skip::Primitive;
    if (!shaderClassOk(shader)) return Skip::ShaderClass;
    if (ptr(shader, 0x220) != view_.admin) return Skip::Unreadable;
    if (get<std::uint8_t>(shader, 0x2a8) == 1 || get<std::uint32_t>(view_.admin, 0x31c) != 0) return Skip::InvalidShader;
    if (ptr(visual, 0x60) != nullptr) return Skip::VisualShader;
    // the billboards FillVertexBuffer writes
    std::vector<std::uint8_t> billboards(static_cast<std::size_t>(n) * sc::kSkyBillboardSize);
    if (bb != nullptr) {
        if (n != 1) return Skip::Unreadable;
        std::memcpy(billboards.data(), bb, sc::kSkyBillboardSize);
    } else {
        if (static_cast<std::uint32_t>(std::max(get<std::int32_t>(object, 0x18), 0)) < n) return Skip::Unreadable;
        if (!rd(ptr(object, 0x10), 0, billboards.data(), billboards.size())) return Skip::Unreadable;
    }
    std::vector<std::uint8_t> vertices, quads;
    sc::skyBillboardVertices(billboards, n, vertices);
    sc::skyQuadIndices(dd.startIndex / 6 + n, quads);
    // SetWorldMatrix(world, 0), SetGlobalAlpha(1, 0), GetPasses(shader, 0, null, 0, false, 0), ExecutePass + eCGfxVisual::Execute
    writeWorld(0, world);
    writeAlpha(0, 1.0f);
    Geom g;
    if (!geometryOfVisual(visual, g)) return Skip::Primitive;
    g.minIndex = dd.minVertexIndex;
    g.numVertices = dd.numVertices;
    g.startIndex = dd.startIndex;
    g.primCount = prims;
    sc::PassInputs in;
    std::vector<sc::Pass> passes;
    const std::uint8_t* version = nullptr;
    if (const Skip k = shaderPasses(shader, in, version, passes); k != Skip::None) return k;
    if (version == nullptr) return Skip::None;
    for (const sc::Pass& p : passes) {
        const std::size_t before = drawCount_;
        if (const Skip s = passDraw(shader, shader, version, p, g, false); s != Skip::None) return s;
        for (std::size_t d = before; d < drawCount_; ++d) {
            sc::DrawDesc& x = *draws_[d];
            x.drawClass = sc::DrawClass::Sky;
            x.primType = static_cast<std::uint32_t>(dd.type);
            x.baseVertex = dd.baseVertexIndex;
            x.vertexStride = sc::kSkyVertexStride;
            x.vertexBytes = vertices;
            x.indexBytes.assign(quads.begin() + static_cast<std::ptrdiff_t>(dd.startIndex) * 2,
                                quads.begin() + static_cast<std::ptrdiff_t>(dd.startIndex) * 2 + static_cast<std::ptrdiff_t>(prims) * 6);
        }
    }
    return Skip::None;
}

StaticOps::Skip StaticOps::skyDomeDraw(const std::uint8_t* shader, const std::uint8_t* visual) {
    U8* admin = view_.admin;
    if (get<const void*>(shader, 0) != engine_ + kSkyDomeVtable) return Skip::ShaderClass;
    if (ptr(shader, 0x220) != admin) return Skip::Unreadable;
    if (get<std::uint8_t>(shader, 0x2a8) == 1 || get<std::uint32_t>(admin, 0x31c) != 0) return Skip::InvalidShader;
    if (ptr(visual, 0x60) != nullptr) return Skip::VisualShader;
    const auto* prim = hal::vulkan::HalPrimitive::fromApi(ptr(visual, 0x10));
    if (prim == nullptr) return Skip::Primitive;
    const auto& dd = prim->drawDesc();
    if (!dd.indexed) return Skip::Primitive;
    // eCShaderBase::OnGetPasses: version min(admin +0x170, +0x68), falling back to a lower existing one; GetPasses 0x6d91a0
    std::int32_t v = std::min(get<std::int32_t>(admin, 0x170), get<std::int32_t>(shader, 0x68));
    U8* version = ptr(shader, 0x258 + 8 * static_cast<std::size_t>(std::max(v, 0)));
    while (version == nullptr && v > 0) version = ptr(shader, 0x258 + 8 * static_cast<std::size_t>(--v));
    if (version == nullptr) return Skip::None;
    if (get<const void*>(version, 0) != engine_ + kVersionSkyDome) return Skip::Version;
    // ExecutePass(0): SetSwitch(pass switch 0), the depth-bias c0 switch, the version's Execute 0x6d90d0
    Skip skip = Skip::None;
    applySwitch(shader, 0, skip);
    if (skip != Skip::None) return skip;
    const bool sunOcclusion = get<std::uint8_t>(shader, 0x324) != 0;
    U8* vsElem = ptr(version, sunOcclusion ? 0x1c0 : 0x1b0);
    U8* psElem = ptr(version, sunOcclusion ? 0x1e0 : 0x1d0);
    if (vsElem == nullptr || psElem == nullptr) return Skip::NotCompiled;
    d3d9::Shader* vs = shaderOfElement(vsElem);
    d3d9::Shader* ps = shaderOfElement(psElem);
    if (vs == nullptr || ps == nullptr) return Skip::NotCompiled;
    sc::BlendInputs b;
    b.mode = get<std::int32_t>(shader, 0x50);
    b.maskRef = get<std::uint8_t>(shader, 0x58);
    b.tradeAlphaAsSolid = get<std::uint8_t>(shader, 0x79) != 0;
    const sc::RenderStates rs = sc::skyDomeStates(b, 1.0f);
    if (rs.alphaTest == 2) mirror_.adminPs[5][3] = static_cast<float>(rs.alphaRef) * (1.0f / 256.0f);
    Geom g;
    if (!geometryOfVisual(visual, g)) return Skip::Primitive;
    g.minIndex = dd.minVertexIndex;
    g.numVertices = dd.numVertices;
    g.startIndex = dd.startIndex;
    g.primCount = dd.primitiveCount;
    sc::DrawDesc& d = drawFrom(g);
    d.drawClass = sc::DrawClass::Sky;
    d.primType = static_cast<std::uint32_t>(dd.type);
    d.baseVertex = dd.baseVertexIndex;
    d.vs = vs;
    d.ps = ps;
    d.states = sc::deviceStates(rs, g.visual, get<std::uint32_t>(admin, 0x340));
    constantsInto(d, shader, shader, false);
    if (const Skip s = materialSamplers(d, shader, psElem); s != Skip::None) return s;
    return adminSamplers(d, shader, ps);
}

StaticOps::Skip StaticOps::composeSky(const Op& op) {
    U8* sky = ptr(op.raw.context, 0x18);  // the context's slot 0x6e8a10: eCSkydome_PS (+0x18), else nothing
    if (sky == nullptr || (get<std::uint8_t>(sky, 0x18) & 1) == 0) return Skip::None;
    if (get<std::uint8_t>(sky, 0x3cc) != 0) return Skip::SkyCamera;  // the cube camera (only in UpdateSkydomeEnvCubemap)
    // the camera: eCGfxCamera::ms_pActiveCamera (the main view's after ExecuteGfxCamera); Execute would render through a master
    U8* cam = ptr(engine::sym().gfxActiveCamera, 0);
    if (cam == nullptr || cam != ptr(ptr(db_, 0x14c0), 0x1738) || ptr(engine::sym().gfxCameraMaster, 0) != nullptr) return Skip::SkyCamera;
    U8* dome = ptr(sky, 0xf0);
    if (ptr(sky, 0xe0) == nullptr || dome == nullptr) return Skip::NotCreated;  // CreateBuffers / CreateShader run in the first Render
    if (!view_.valid) {
        U8* a = ptr(dome, 0x220);
        if (a == nullptr || !composeView(a)) return Skip::Unreadable;
    }
    return composeSkyDraws(sky, cam);
}

void StaticOps::skyCubeBegin(void* skyPs) {
    finishOp();
    GroupState& gs = groups_[static_cast<std::size_t>(Group::SkyCube)];
    if (!gs.active() || inView_ || inSkyCube_) return;
    if (verified_ == 0) verified_ = verify() ? 1 : -1;
    if (verified_ < 0) return;
    if (skyVerified_ == 0) skyVerified_ = verifySky() ? 1 : -1;
    if (skyVerified_ < 0) return;
    U8* sky = static_cast<U8*>(skyPs);
    g_ = &gs;
    drawCount_ = 0;
    std::memcpy(entryVsB_.data(), dev_.vsBoolConstants(), sizeof(entryVsB_));
    std::memcpy(entryPsB_.data(), dev_.psBoolConstants(), sizeof(entryPsB_));
    std::memcpy(entryVsI_, dev_.vsIntConstants(), sizeof(entryVsI_));
    std::memcpy(entryPsI_, dev_.psIntConstants(), sizeof(entryPsI_));
    nonOp_ = {};
    nonOp_.raw.context = sky;
    nonOp_.list = List::Count;
    Skip s = Skip::None;
    // the cube face: Render with +0x3cc = 1 draws through the cube camera +0x3c0 (its view at the origin, 90 deg, near 1, far
    // 1000); no fog executed (PS c3 / c4 as the pools hold them); the face's viewport (128 x 128) set before
    U8* cam = ptr(sky, 0x3c0);
    U8* dome = ptr(sky, 0xf0);
    if ((get<std::uint8_t>(sky, 0x18) & 1) == 0) {
        s = Skip::None;  // nothing drawn
    } else if (get<std::uint8_t>(sky, 0x3cc) != 1 || cam == nullptr || ptr(engine::sym().gfxCameraMaster, 0) != nullptr) {
        s = Skip::SkyCamera;
    } else if (ptr(sky, 0xe0) == nullptr || dome == nullptr) {
        s = Skip::NotCreated;
    } else if (U8* a = ptr(dome, 0x220); a == nullptr || !composeView(a, cam)) {
        s = Skip::Unreadable;
    } else {
        adminStagesUnbound_ = true;
        s = composeSkyDraws(sky, cam);
        adminStagesUnbound_ = false;
    }
    blendOverrideShader_ = nullptr;
    blendOverride_ = -1;
    inSkyCube_ = true;
    nonOpGroupComposed(s);
    if (s != Skip::None) inSkyCube_ = false;
}

StaticOps::Skip StaticOps::composeSkyDraws(const std::uint8_t* sky, const std::uint8_t* cam) {
    U8* domeVisual = ptr(sky, 0xe0);
    U8* dome = ptr(sky, 0xf0);
    initMirror();
    // the later ops of the view see the camera as before the sky (the restore recomputes the same matrices): the view's camera
    // constants come back on every path out of here
    struct ViewGuard {
        View& v;
        View saved;
        ~ViewGuard() { v = saved; }
    } guard{view_, view_};
    alignas(16) float view[16], proj[16], dbProj[16];
    if (!rd(cam, 0x20, view, 64) || !rd(cam, 0x60, proj, 64) || !rd(cam, 0xa0, dbProj, 64)) return Skip::Unreadable;
    // the hack: view translation zeroed (_41 _42 _43), projection _33 = 1, _43 = 0 (depth exactly 1, the sky follows the camera)
    alignas(16) float hv[16], hp[16];
    std::memcpy(hv, view, 64);
    hv[12] = hv[13] = hv[14] = 0.0f;
    std::memcpy(hp, proj, 64);
    hp[10] = 1.0f;
    hp[14] = 0.0f;
    setCamera(hv, hp, dbProj);
    // eCColorSrcSampler::SetImageResource(sampler, image): the sampler's pool slot gets the image (UpdateSampler)
    const auto setImage = [&](U8* sampler, U8* shader, const void* image) {
        if (sampler == nullptr || ptr(sampler, 0x90) != shader + 0xf8) return false;
        const std::int32_t slot = get<std::int32_t>(sampler, 0x98, -1);
        if (slot < 0 || slot >= 16) return false;
        poolsOf(shader)->slotImage[static_cast<std::size_t>(slot)] = image;
        return true;
    };
    // eCColorSrcConstant::SetColor(colour): r g b from the colour (+8), a = the element's +0x60, into its pool register
    // (OnNotifyPropertyValueChangedExitEx 0x6bfda0: pool +0x68, register +0x70)
    const auto setColor = [&](U8* element, U8* shader, U8* colour) {
        if (element == nullptr) return false;
        U8* pool = ptr(element, 0x68);
        const std::int32_t reg = get<std::int32_t>(element, 0x70, -1);
        if (pool == nullptr || reg < 0) return true;
        const float v[4] = {get<float>(colour, 8), get<float>(colour, 0xc), get<float>(colour, 0x10), get<float>(element, 0x60)};
        Mirror::ShaderPools* p = poolsOf(shader);
        const auto r = static_cast<std::uint32_t>(reg);
        if (pool == shader + 0xc8 && r >= p->psFirst && r - p->psFirst < 16) {
            std::memcpy(p->ps[r - p->psFirst], v, 16);
        } else if (pool == shader + 0x98 && r >= p->vsFirst && r - p->vsFirst < 32) {
            std::memcpy(p->vs[r - p->vsFirst], v, 16);
        } else {
            return false;
        }
        return true;
    };
    U8* billboardShader = ptr(sky, 0x148);  // stars and moon
    // stars (CreateStars 0x6e3ee0: a texture +0x140; the billboards rebuilt when StarCount +0x28 != the object's count +0x290)
    if (ptr(sky, 0x140) != nullptr) {
        const std::int32_t want = get<std::int32_t>(sky, 0x28), have = get<std::int32_t>(sky, 0x290);
        if (have != want && want != 0) return Skip::SkyRebuilt;
        if (have == want && want != 0) {
            if (billboardShader == nullptr || ptr(sky, 0x278 + 0x20) != billboardShader) return Skip::Unreadable;
            blendOverrideShader_ = billboardShader;
            blendOverride_ = 7;
            if (!setImage(ptr(sky, 0x160), billboardShader, ptr(sky, 0x140)) || !setColor(ptr(sky, 0x168), billboardShader, sky + 0x390)) {
                return Skip::Unreadable;
            }
            float m[16];
            if (!rd(sky, 0x2d0, m, 64)) return Skip::Unreadable;
            if (const Skip k = skyObjectDraws(sky + 0x278, m, nullptr, 0); k != Skip::None) return k;
        }
    }
    // the sun (a texture +0x138): its one billboard rewritten: position (0, 0, -1), s = (size, 0), r = (0, size), white, uv 0..1,
    // size = SunSize +0x3a8 x +0x50; SetBillboardRenderCount(1)
    if (U8* image = ptr(sky, 0x138); image != nullptr) {
        U8* sunShader = ptr(sky, 0x150);
        if (sunShader == nullptr || ptr(sky, 0x170 + 0x20) != sunShader) return Skip::Unreadable;
        if (get<std::int32_t>(sky, 0x170 + 0x18) != 1) return Skip::SkyRebuilt;  // the array (re)made in this Render
        if (!setImage(ptr(sky, 0x158), sunShader, image)) return Skip::Unreadable;
        const float size = get<float>(sky, 0x3a8) * get<float>(sky, 0x50);
        std::uint8_t bb[sc::kSkyBillboardSize]{};
        const float pos[3] = {0.0f, 0.0f, -1.0f};
        const float axes[4] = {size, 0.0f, 0.0f, size};
        const std::uint32_t white = 0xffffffffu;
        const float uv[4] = {0.0f, 0.0f, 1.0f, 1.0f};
        std::memcpy(bb, pos, 12);
        std::memcpy(bb + 0xc, axes, 16);
        std::memcpy(bb + 0x1c, &white, 4);
        std::memcpy(bb + 0x20, uv, 16);
        float m[16];
        if (!rd(sky, 0x310, m, 64)) return Skip::Unreadable;
        if (const Skip k = skyObjectDraws(sky + 0x170, m, bb, 1); k != Skip::None) return k;
    }
    // the moon (CreateMoon 0x6e3c30: a texture +0x130; the billboard made when the object has none)
    if (ptr(sky, 0x130) != nullptr) {
        if (get<std::int32_t>(sky, 0x220 + 0x18) == 0) return Skip::SkyRebuilt;
        if (billboardShader == nullptr || ptr(sky, 0x220 + 0x20) != billboardShader) return Skip::Unreadable;
        blendOverrideShader_ = billboardShader;
        blendOverride_ = 5;
        if (!setImage(ptr(sky, 0x160), billboardShader, ptr(sky, 0x130)) || !setColor(ptr(sky, 0x168), billboardShader, sky + 0x58)) {
            return Skip::Unreadable;
        }
        float m[16];
        if (!rd(sky, 0x350, m, 64)) return Skip::Unreadable;
        if (const Skip k = skyObjectDraws(sky + 0x220, m, nullptr, 0); k != Skip::None) return k;
    }
    blendOverrideShader_ = nullptr;
    blendOverride_ = -1;
    // the camera restored: view = the saved one, Invert, view-projection; projection = the saved one, view-projection; Execute
    setCamera(view, proj, dbProj);
    return skyDomeDraw(dome, domeVisual);
}

}  // namespace rvk::native
