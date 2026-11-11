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
#include <chrono>
#include <cstring>
#include <format>

#include <spdlog/spdlog.h>

#include "d3d9/defs.h"
#include "engine/memread.h"
#include "engine/symbols.h"
#include "hal/vulkan/hal_vulkan.h"
#include "native/f2c_measure.h"
#include "native/own_post.h"
#include "native/static_ops.h"
#include "gpuscene/material_table.h"

namespace rvk::native {

namespace {

using engine::readAt;
using engine::safeRead;
using U8 = const std::uint8_t;

template <class T>
inline T get(const void* p, std::size_t off, T fallback = T{}) {
    T v = fallback;
    if (!readAt(p, off, v)) return fallback;
    return v;
}
inline U8* ptr(const void* p, std::size_t off) { return get<U8*>(p, off, nullptr); }
inline bool rd(const void* p, std::size_t off, void* out, std::size_t n) { return p != nullptr && safeRead(static_cast<U8*>(p) + off, out, n); }

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
    // the billboards FillVertexBuffer writes (scratch reused across calls: no heap traffic per sky object / cube face)
    std::vector<std::uint8_t>& billboards = skyBb_;
    billboards.resize(static_cast<std::size_t>(n) * sc::kSkyBillboardSize);
    if (bb == nullptr && billboardsOverride_ != nullptr && billboardsOverride_->size() == billboards.size()) {
        std::memcpy(billboards.data(), billboardsOverride_->data(), billboards.size());
    } else if (bb != nullptr) {
        if (n != 1) return Skip::Unreadable;
        std::memcpy(billboards.data(), bb, sc::kSkyBillboardSize);
    } else {
        if (static_cast<std::uint32_t>(std::max(get<std::int32_t>(object, 0x18), 0)) < n) return Skip::Unreadable;
        if (!rd(ptr(object, 0x10), 0, billboards.data(), billboards.size())) return Skip::Unreadable;
    }
    // the stars' billboards are the same for every cube face and frame: the converted vertices are kept while the input bytes are
    // equal (F1; native.sky_cache, A/B)
    // (one entry per sky object: the sun / moon / stars alternate within a cube face)
    if (skyCaches_.size() > 64) skyCaches_.clear();
    SkyCache& sk = skyCache_ ? skyCaches_[object] : skyScratch_;
    std::vector<std::uint8_t>& vertices = sk.verts;
    std::vector<std::uint8_t>& quads = sk.quads;
    if (!skyCache_ || sk.key.size() != billboards.size() || std::memcmp(sk.key.data(), billboards.data(), billboards.size()) != 0) {
        sc::skyBillboardVertices(billboards, n, vertices);
        sk.key = billboards;
    }
    if (sk.quadCount != dd.startIndex / 6 + n || !skyCache_) {
        sc::skyQuadIndices(dd.startIndex / 6 + n, quads);
        sk.quadCount = dd.startIndex / 6 + n;
    }
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
    // F12 owned record (frame-ownership.md 14.34): a lean frame draws the dome from its record (the composition below skipped); every other frame composes it
    // and refreshes the record (check frames compare first)
    SkyRecord* rec = nullptr;
    std::uint64_t stamp = 0;
    using PClock = std::chrono::steady_clock;
    const PClock::time_point ct0 = PClock::now();
    if (OwnPost* own = ownPost(); own != nullptr && own->sceneMode(OwnPost::Scene::Sky) == OwnPost::Mode::Owned) {
        if (rec_ != nullptr || f2c::on()) {
            ++skyOwnedW_.composed;
            if (skyOwnedW_.why.empty()) skyOwnedW_.why = "a retained-record / measurement recorder is on";
        } else {
            rec = &skyRecords_[adminStagesUnbound_ ? 1 : 0];
            stamp = skyStamp(shader, visual, version, vsElem, psElem);
            if (leanFrame()) {
                const char* why = !rec->valid ? "no record yet" : rec->stamp != stamp ? "an input changed (stamp)" : rec->desc.vs != vs || rec->desc.ps != ps ? "the shaders changed" : nullptr;
                if (why == nullptr && skyOwnedDraw(shader, visual, stamp, *rec)) {
                    ++skyOwnedW_.owned;
                    skyOwnedW_.ownedNs += static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(PClock::now() - ct0).count());
                    own->countOwned(OwnPost::Scene::Sky);
                    return Skip::None;
                }
                ++skyOwnedW_.composed;
                if (why != nullptr && rec->valid && rec->stamp != stamp) {
                    ++skyOwnedW_.stampChanged;
                    if (skyStampLogged_ < 8) {
                        std::size_t at = 0;
                        while (at < skyStampBytes_.size() && at < rec->stampBytes.size() && rec->stampBytes[at] == skyStampBytes_[at]) ++at;
                        ++skyStampLogged_;
                        // (offsets: 48 pointers, then the shader object from +0x48 (stamp byte 48 = +0x48), ...)
                        spdlog::info("native: own sky: owned record ({}): stamp byte {} of {} / {} changed ({} -> {}; stamp bytes 48.. = the shader object's read ranges; frame {})", adminStagesUnbound_ ? "cube" : "main", at, rec->stampBytes.size(), skyStampBytes_.size(),
                                     at < rec->stampBytes.size() ? rec->stampBytes[at] : 0, at < skyStampBytes_.size() ? skyStampBytes_[at] : 0, dev_.stats().frames);
                    }
                }
                if (skyOwnedW_.why.empty()) skyOwnedW_.why = why != nullptr ? why : "a texture of the record was re-made";
            }
        }
    }
    std::array<SkyTex, sc::kStages> capture{};
    struct CaptureScope {
        std::array<SkyTex, sc::kStages>*& at;
        ~CaptureScope() { at = nullptr; }
    } captureScope{skyTexCapture_};
    if (rec != nullptr) skyTexCapture_ = &capture;
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
    d.skyDome = true;
    d.primType = static_cast<std::uint32_t>(dd.type);
    d.baseVertex = dd.baseVertexIndex;
    d.vs = vs;
    d.ps = ps;
    d.states = sc::deviceStates(rs, g.visual, get<std::uint32_t>(admin, 0x340));
    constantsInto(d, shader, shader, false);
    if (const Skip s = materialSamplers(d, shader, psElem); s != Skip::None) return s;
    if (const Skip s = adminSamplers(d, shader, ps); s != Skip::None) return s;
    if (rec != nullptr) {
        skyTexCapture_ = nullptr;
        // a check frame: the record's form (its states, samplers, geometry; the registers are the composed ones in both) against the composition
        leanUpdateFrame();
        if (leanFrameKind_ == sc::LeanFrame::Check && rec->valid && rec->stamp == stamp) {
            ++skyOwnedW_.checked;
            std::string why;
            if (!skyRecordEqual(rec->desc, d, why)) {
                ++skyOwnedW_.differing;
                ++skyOwnedDifferingTotal_;
                if (skyOwnedW_.firstDiff.empty()) skyOwnedW_.firstDiff = why;
                if (skyOwnedLogged_++ < 12) spdlog::warn("native: own sky: owned record ({}) DIFFERS from the composed dome: {}", adminStagesUnbound_ ? "cube face" : "main view", why);
            }
        }
        rec->desc = d;
        rec->stamp = stamp;
        rec->stampBytes = skyStampBytes_;
        rec->tex = capture;
        rec->texMask = 0;
        for (std::uint32_t st = 0; st < sc::kStages; ++st) {
            if (capture[st].data != nullptr || capture[st].res != nullptr || capture[st].material) rec->texMask |= static_cast<std::uint16_t>(1u << st);
        }
        rec->alphaTest = static_cast<std::uint8_t>(rs.alphaTest);
        rec->alphaRef = rs.alphaRef;
        rec->valid = true;
        skyOwnedW_.composedNs += static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(PClock::now() - ct0).count());
        ++skyOwnedW_.composedTimed;
    }
    return Skip::None;
}

std::uint64_t StaticOps::skyStamp(const std::uint8_t* shader, const std::uint8_t* visual, const std::uint8_t* version, const std::uint8_t* vsElem, const std::uint8_t* psElem) const {
    // (the bytes are kept: a changed stamp names its first differing input in the log)
    std::vector<std::uint8_t>& out = skyStampBytes_;
    out.clear();
    const auto mix = [&](const void* p, std::size_t n) {
        const auto* b = static_cast<const std::uint8_t*>(p);
        out.insert(out.end(), b, b + n);
    };
    const auto mixp = [&](const void* p) { mix(&p, sizeof(p)); };
    mixp(shader);
    mixp(visual);
    mixp(version);
    mixp(vsElem);
    mixp(psElem);
    mixp(view_.admin);
    // the shader object: blend inputs (+0x50 / +0x58 / +0x79), the version cap +0x68, the depth-bias flag +0x78, the pools' pointers / counts, the material slots
    // (+0xf8 + 0x28 k: sampler states, image +0x20), the slot base / count +0x219 / +0x21b, the invalid flag +0x2a8, SunOcclusion +0x324
    // (only what the composition reads: the engine changes other bytes of the object every frame / on its own frames, e.g. +0x2b8 and the pool header +0xa8)
    struct Range {
        std::uint32_t at, n;
    };
    static constexpr Range kRead[] = {{0x48, 0x50}, {0x9c, 4}, {0xb0, 8}, {0xc1, 1}, {0xcc, 4}, {0xe0, 8}, {0xf1, 1}, {0xf8, 0x1b8}, {0x2a8, 1}, {0x324, 1}};
    for (const Range& r : kRead) {
        std::uint8_t bytes[0x1b8];
        if (!engine::safeRead(shader + r.at, bytes, r.n)) std::memset(bytes, 0, r.n);
        mix(bytes, r.n);
    }
    // the slots' images and their data (the textures they resolve to are re-checked by skyOwnedDraw)
    const std::uint32_t slots = std::min<std::uint32_t>(get<std::uint8_t>(shader, 0x21b), 16);
    for (std::uint32_t k = 0; k < slots; ++k) {
        U8* res = ptr(shader, 0xf8 + 0x28 * k + 0x20);
        mixp(res);
        mixp(res != nullptr ? ptr(res, 0x70) : nullptr);
        if (res != nullptr && ptr(res, 0x70) == nullptr) mixp(ptr(placeholderTable_, 8 * (get<std::uint8_t>(shader, 0x219) + k)));
    }
    mixp(ptr(psElem, 0x38));
    // the admin: the version +0x170, the invalid count +0x31c, the colour-write / fill state +0x340; its sampler block + overrides (the snapshot), the lightmaps
    const std::int32_t a170 = get<std::int32_t>(view_.admin, 0x170);
    const std::uint32_t a31c = get<std::uint32_t>(view_.admin, 0x31c), a340 = get<std::uint32_t>(view_.admin, 0x340);
    mix(&a170, 4);
    mix(&a31c, 4);
    mix(&a340, 4);
    const std::uint64_t snap = const_cast<StaticOps*>(this)->snapHash();
    mix(&snap, 8);
    mixp(mirror_.lightmapSlot[0]);
    mixp(mirror_.lightmapSlot[1]);
    const std::uint8_t unbound = adminStagesUnbound_ ? 1 : 0;
    mix(&unbound, 1);
    // the visual (+0x10 the primitive's API object, +0x18 .. +0x24 its states, +0x60 a visual shader) and the primitive's ranges / buffers
    std::uint8_t vb[0x58];
    if (!engine::safeRead(visual + 0x10, vb, sizeof(vb))) std::memset(vb, 0, sizeof(vb));
    mix(vb, sizeof(vb));
    if (const auto* prim = hal::vulkan::HalPrimitive::fromApi(ptr(visual, 0x10)); prim != nullptr) {
        const auto& dd = prim->drawDesc();
        const std::uint32_t r[6] = {static_cast<std::uint32_t>(dd.type), static_cast<std::uint32_t>(dd.baseVertexIndex), dd.minVertexIndex, dd.numVertices, dd.startIndex,
                                    dd.primitiveCount};
        mix(r, sizeof(r));
        mixp(prim->vertexDecl());
        mixp(prim->indexBuffer());
        for (std::uint32_t i = 0; i < 4; ++i) {
            const auto v = prim->streamView(i);
            mixp(v.buffer);
            const std::uint32_t s[3] = {v.offset, v.stride, v.enabled ? v.freq : 0u};
            mix(s, sizeof(s));
        }
    }
    std::uint64_t h = 0xcbf29ce484222325ull;
    for (const std::uint8_t b : out) {
        h ^= b;
        h *= 0x100000001b3ull;
    }
    return h;
}

bool StaticOps::skyOwnedDraw(const std::uint8_t* shader, const std::uint8_t* visual, std::uint64_t stamp, SkyRecord& r) {
    (void)visual;
    (void)stamp;
    // the textures: re-resolved from the stage's image data (a texture re-made behind the same data: the record is stale, composed)
    for (std::uint32_t st = 0; st < sc::kStages; ++st) {
        if ((r.texMask >> st & 1u) == 0) continue;
        bool ok = false;
        const void* t = r.tex[st].data != nullptr ? textureOfImageData(r.tex[st].data, ok) : nullptr;
        const bool inherit = (r.desc.inheritMask >> st & 1u) != 0;
        if (t != r.desc.samplers[st].texture || (r.tex[st].data != nullptr && ok == inherit)) return false;
    }
    // the composition's side effects: the alpha reference register (an alpha-tested dome), ExecuteShaderConstants' touches of the slots' images
    if (r.alphaTest == 2) mirror_.adminPs[5][3] = static_cast<float>(r.alphaRef) * (1.0f / 256.0f);
    for (std::uint32_t st = 0; st < sc::kStages; ++st) {
        if ((r.texMask >> st & 1u) != 0 && r.tex[st].material && r.tex[st].res != nullptr) leanImage(r.tex[st].res, r.tex[st].touched);
    }
    sc::DrawDesc& d = newDraw();
    const auto instance = d.instance;
    d = r.desc;
    d.instance = instance;
    // the parameter block: the live registers from the mirror (the composed state); the record's copy rewritten only when they changed
    constantsInto(d, shader, shader, false);
    std::uint64_t h = 0xcbf29ce484222325ull;
    const auto mixRegs = [&](const auto& live, const float (*regs)[4], std::uint32_t n) {
        for (std::uint32_t i = 0; i < n; ++i) {
            if (!live.test(i)) continue;
            const auto* b = reinterpret_cast<const std::uint8_t*>(regs[i]);
            for (std::size_t k = 0; k < 16; ++k) {
                h ^= b[k];
                h *= 0x100000001b3ull;
            }
        }
    };
    mixRegs(d.vsLive, d.vsF, sc::kVsRegs);
    mixRegs(d.psLive, d.psF, sc::kPsRegs);
    if (h != r.paramHash) {
        r.paramHash = h;
        std::memcpy(r.desc.vsF, d.vsF, sizeof(d.vsF));
        std::memcpy(r.desc.psF, d.psF, sizeof(d.psF));
        ++skyOwnedW_.paramChanges;
    }
    return true;
}

bool StaticOps::skyRecordEqual(const sc::DrawDesc& a, const sc::DrawDesc& b, std::string& why) {
    if (!packetSameDesc(a, b)) {
        why = "states / samplers / geometry / shaders";
        return false;
    }
    if (a.vsLive != b.vsLive || a.psLive != b.psLive) {
        why = "the live registers";
        return false;
    }
    if (a.vsSet != b.vsSet || a.psSet != b.psSet) {
        why = "the set registers";
        return false;
    }
    if (a.vsB != b.vsB || a.psB != b.psB || std::memcmp(a.vsI, b.vsI, sizeof(a.vsI)) != 0 || std::memcmp(a.psI, b.psI, sizeof(a.psI)) != 0) {
        why = "bool / int registers";
        return false;
    }
    if (a.alphaFunc != b.alphaFunc || a.alphaRef != b.alphaRef || a.skyDome != b.skyDome || a.vertexBytes != b.vertexBytes || a.indexBytes != b.indexBytes ||
        a.cpuStream != b.cpuStream || a.maskSet != b.maskSet) {
        why = "alpha / class fields";
        return false;
    }
    for (std::uint32_t st = 0; st < sc::kStages; ++st) {
        if (a.samplers[st].texture != b.samplers[st].texture) {
            why = std::format("the texture of stage {}", st);
            return false;
        }
    }
    return true;
}

void StaticOps::skyOwnedReport() {
    SkyOwnedStats& s = skyOwnedW_;
    OwnPost* own = ownPost();
    if ((own != nullptr && own->sceneMode(OwnPost::Scene::Sky) == OwnPost::Mode::Owned) || s.owned != 0 || s.checked != 0) {
        spdlog::info("native: own sky: owned record (5 s): {} dome draws from the record ({:.2f} us each), {} composed ({:.2f} us each, {} with an input changed){}; "
                     "parameter block rewritten {} times; check frames: {} compared with the composed dome, DIFFERING {} (run {}){}",
                     s.owned, s.owned != 0 ? static_cast<double>(s.ownedNs) / 1000.0 / static_cast<double>(s.owned) : 0.0, s.composed,
                     s.composedTimed != 0 ? static_cast<double>(s.composedNs) / 1000.0 / static_cast<double>(s.composedTimed) : 0.0, s.stampChanged,
                     s.why.empty() ? std::string() : std::format(" (first reason: {})", s.why), s.paramChanges, s.checked, s.differing, skyOwnedDifferingTotal_,
                     s.firstDiff.empty() ? std::string() : std::format(" (first: {})", s.firstDiff));
    }
    if (s.cubeFaces != 0) {
        spdlog::info("native: own sky: sky cube faces (5 s): {} faces composed ({:.2f} per frame, {:.2f} us each: stars / sun / moon / dome; the issue cost is the scene ops' [sky cube faces] line)",
                     s.cubeFaces, static_cast<double>(s.cubeFaces) / static_cast<double>(std::max<std::uint64_t>(1, dev_.stats().frames - s.frame0)),
                     static_cast<double>(s.cubeFaceNs) / 1000.0 / static_cast<double>(s.cubeFaces));
    }
    s = {};
    s.frame0 = dev_.stats().frames;
}

StaticOps::Skip StaticOps::composeSky(const Op& op) {
    U8* sky = ptr(op.raw.context, 0x18);  // the context's slot 0x6e8a10: eCSkydome_PS (+0x18), else nothing
    if (sky == nullptr || (get<std::uint8_t>(sky, 0x18) & 1) == 0) return Skip::None;
    leanSkyPre(sky);  // O2a: Render's first-use creation and the cloud fields, before composing (the engine's order)
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

bool StaticOps::skyCubeBegin(void* skyPs) {
    finishOp();
    GroupState& gs = groups_[static_cast<std::size_t>(Group::SkyCube)];
    if (!inView_ && !inSkyCube_) {
        leftNonOp_ = true;
        leftNonOpWhy_ = leftName(Group::SkyCube, gs.active() ? kLeftChecks : kLeftGroupOff);
    }
    if (!gs.active() || inView_ || inSkyCube_) {
        if (!inView_ && !inSkyCube_) markLeft();
        return false;
    }
    if (verified_ == 0) verified_ = verify() ? 1 : -1;
    if (verified_ < 0) return false;
    if (skyVerified_ == 0) skyVerified_ = verifySky() ? 1 : -1;
    if (skyVerified_ < 0) return false;
    if (mirrorMode_ != 2) ++mirrorEpoch_;  // outside a view: the admin / shader pools re-read for the face
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
    leanBegin(Group::SkyCube);
    leanSkyPre(sky);  // O2a: Render's first-use creation and the cloud fields, before composing (the engine's order)
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
        // (14.34 (b): the cube faces' composition cost, measured for the one-pass question)
        const auto f0 = std::chrono::steady_clock::now();
        adminStagesUnbound_ = true;
        s = composeSkyDraws(sky, cam);
        adminStagesUnbound_ = false;
        skyOwnedW_.cubeFaceNs += static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - f0).count());
        ++skyOwnedW_.cubeFaces;
    }
    blendOverrideShader_ = nullptr;
    blendOverride_ = -1;
    inSkyCube_ = true;
    nonOpGroupComposed(s);
    if (s != Skip::None) inSkyCube_ = false;
    return slotSkipped_;
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
        gpuscene::materialtable().noteSwitchWrite(shader, 2, static_cast<std::uint32_t>(slot));
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
            gpuscene::materialtable().noteSwitchWrite(shader, 1, r - p->psFirst);
        } else if (pool == shader + 0x98 && r >= p->vsFirst && r - p->vsFirst < 32) {
            std::memcpy(p->vs[r - p->vsFirst], v, 16);
            gpuscene::materialtable().noteSwitchWrite(shader, 0, r - p->vsFirst);
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

// ---- F12 (frame-ownership.md 14.24, native.own_sky): the sky dome on our own shaders ----

bool StaticOps::skyOwnBefore(const sc::DrawDesc& d) {
    skyCmpScratch_ = skyCmpTarget_ = nullptr;
    OwnPost* own = ownPost();
    if (own == nullptr) return false;
    const OwnPost::Mode m = own->sceneMode(OwnPost::Scene::Sky);
    if (m == OwnPost::Mode::Off) return false;
    const char* why = nullptr;
    const d3d9::Shader* vs = own->fxPort(d.vs, &why);
    const d3d9::Shader* ps = own->fxPort(d.ps, &why);
    const bool ported = vs != nullptr && ps != nullptr;
    own->countScene(OwnPost::Scene::Sky, ported, why);
    if (!ported) return false;
    using d3d9::Shader;
    using d3d9::ShaderRef;
    if (OwnPost::portsOn(m)) {
        dev_.setVertexShaderRef(ShaderRef(const_cast<Shader*>(vs)));
        dev_.setPixelShaderRef(ShaderRef(const_cast<Shader*>(ps)));
        return true;
    }
    // compare: the port's draw into a scratch copy of the target (a main view's scene image, or a sky cube face), the other targets unbound (the dome blends:
    // drawn twice they would differ), depth tested as the dome's draw (it writes none), the viewport / scissor as the device has them
    d3d9::Surface* target = dev_.renderTarget(0);
    if (target == nullptr) return false;
    // (a pending run of ordered owned draws (F9 / F14) belongs to the target before the copy: queued by the next recorded command, it would land in the scratch)
    dev_.orderedFlushPending();
    skyCmpSkips_ = own->drawSkips();
    d3d9::Surface* scratch = own->scratchOf(target, 2, true);
    if (scratch == nullptr) return false;
    const d3d9::Viewport vp = dev_.viewport();
    std::int32_t sc[4];
    std::memcpy(sc, dev_.scissorRect(), sizeof(sc));
    d3d9::Surface* more[3] = {dev_.renderTarget(1), dev_.renderTarget(2), dev_.renderTarget(3)};
    const std::uint32_t zwrite = dev_.renderState(d3d9::RS_ZWRITEENABLE), stencilMask = dev_.renderState(d3d9::RS_STENCILWRITEMASK);
    const ShaderRef vsSaved = dev_.vertexShaderRef(), psSaved = dev_.pixelShaderRef();
    for (std::uint32_t i = 0; i < 3; ++i) {
        if (more[i] != nullptr) dev_.setRenderTarget(i + 1, nullptr);
    }
    dev_.setRenderTarget(0, scratch);
    dev_.setViewport(vp);
    dev_.setScissorRect(sc[0], sc[1], sc[2], sc[3]);
    if (zwrite != 0) dev_.setRenderState(d3d9::RS_ZWRITEENABLE, 0);
    if (stencilMask != 0) dev_.setRenderState(d3d9::RS_STENCILWRITEMASK, 0);
    dev_.setVertexShaderRef(ShaderRef(const_cast<Shader*>(vs)));
    dev_.setPixelShaderRef(ShaderRef(const_cast<Shader*>(ps)));
    const bool savedIssuing = issuing_;
    const std::size_t savedIndex = issueIndex_;
    issuing_ = true;
    issueIndex_ = ~std::size_t{0};  // (the compare draw has no composition of its own: the own-draw check skips it)
    if (d.indexed) dev_.drawIndexedPrimitive(d.primType, d.baseVertex, d.minIndex, d.numVertices, d.startIndex, d.primCount);
    else dev_.drawPrimitive(d.primType, d.startIndex, d.primCount);
    issuing_ = savedIssuing;
    issueIndex_ = savedIndex;
    dev_.setVertexShaderRef(vsSaved);
    dev_.setPixelShaderRef(psSaved);
    if (zwrite != 0) dev_.setRenderState(d3d9::RS_ZWRITEENABLE, zwrite);
    if (stencilMask != 0) dev_.setRenderState(d3d9::RS_STENCILWRITEMASK, stencilMask);
    dev_.setRenderTarget(0, target);
    for (std::uint32_t i = 0; i < 3; ++i) {
        if (more[i] != nullptr) dev_.setRenderTarget(i + 1, more[i]);
    }
    dev_.setViewport(vp);
    dev_.setScissorRect(sc[0], sc[1], sc[2], sc[3]);
    skyCmpScratch_ = scratch;
    skyCmpTarget_ = target;
    return false;
}

void StaticOps::skyOwnAfter(const sc::DrawDesc& d, bool ported) {
    if (ported) {
        dev_.setVertexShaderRef(d3d9::ShaderRef(const_cast<d3d9::Shader*>(static_cast<const d3d9::Shader*>(d.vs))));
        dev_.setPixelShaderRef(d3d9::ShaderRef(const_cast<d3d9::Shader*>(static_cast<const d3d9::Shader*>(d.ps))));
    }
    if (skyCmpScratch_ == nullptr) return;
    OwnPost* own = ownPost();
    d3d9::Surface* target = skyCmpTarget_;
    d3d9::Surface* ours = skyCmpScratch_;
    skyCmpScratch_ = skyCmpTarget_ = nullptr;
    if (own == nullptr) return;
    if (own->drawSkips() != skyCmpSkips_) {
        own->countDroppedCompare();  // (one of the two draws was skipped: its shader / pipeline was still building)
        return;
    }
    // (a face / level other than the first: the engine's result copied into a second scratch image of that size first; the compare reads level 0, layer 0)
    const bool cube = target->face() != 0 || target->level() != 0 || target->texture()->desc().type != d3d9::TextureType::Tex2D;
    d3d9::Surface* engine = cube ? own->scratchOf(target, 3, true) : target;
    if (engine == nullptr) return;
    own->compareUnit(cube ? "sky_dome.frag (cube face)" : "sky_dome.frag", ours, engine);
}

}  // namespace rvk::native
