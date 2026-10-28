// N5.8, group 1024 of native.scene_ops_groups (with the shadow mask chain): the post_SSM PSSM point-light pass
// (docs/research/engine-frame/README.md "16. N5.8 result"). eCImageFilterShadowMask::OnUpdateOutput 0x668560, after the mask's
// ExecuteEffect units: SetTechinque("PSSM" [+ "_Low" without HighQuality +0x114]) + ViewProj (application camera P *= V) and
// TexViewProj (the texel-centre bias (0.5, -0.5, 0.5 / w + 0.5, 0.5 / h + 0.5) *= ViewProj); per packed map with cells (+0x128,
// 0xb0 each, count +0x130; cells +0x68 / +0x70) DizzerKernelSize (2 / size), ShadowTexture (map +0x18), PCFFilterOffset (-0.5,
// -0.5, 0.5, 0.5) / size, PCFFilterScale (size, size); UpdatePackedShadowMapVisual 0x669e00 (the pyramids: per cell the four far
// corners = the NDC corners (+-1, +-1, 1) through the inverted cell matrix +0x90, the apex = the inverted view +0x10's
// translation, COLOR0 ~flags +0x11c, TEXCOORD0..3 = cell +0xd0 *= the depth -> world matrix, one DISCARD lock); then
// UpdateEffect + BeginEffectTechnique (OnSetParams) and per cell the camera-sphere test (bCFrustum::SetFrustum(cell +0x90) x
// Intersects(near sphere: radius camera +0xbc / cos(+0xd4), centre +0x90)) and per pass SetMasterStencilTesting(2, data),
// ExecuteEffectPass, the packed visual (+0x138: render mode, cull / depth func written per pass, depth write, the primitive with
// the base vertex 5 c). Composed at UpdatePackedShadowMapVisual's return (the detour), compared with the engine's draws (every
// field class + the vertex / index bytes); take-over issues them from our own vertex buffer and drops the engine's. Render thread.

#include "native/effect_ops.h"

#include <windows.h>

#include <cstring>
#include <format>

#include <spdlog/spdlog.h>

#include "d3d9/defs.h"
#include "engine/memread.h"
#include "engine/symbols.h"
#include "hal/vulkan/hal_vulkan.h"

namespace rvk::native {

namespace {

using U8 = const std::uint8_t;
using engine::readAt;
using engine::safeRead;

template <class T>
T get(const void* p, std::size_t off, T fallback = T{}) {
    T v = fallback;
    if (!readAt(p, off, v)) return fallback;
    return v;
}
U8* ptr(const void* p, std::size_t off) { return get<U8*>(p, off, nullptr); }

constexpr std::size_t kIllumMask = 0x448;
constexpr std::size_t kCellStride = 0x120, kPackedStride = 0xb0;

}  // namespace

bool EffectOps::verifyPssm() {
    const auto& s = engine::sym();
    if (s.maskUpdatePackedVisual != engine_ + 0x669e00 || s.frustumCtor == nullptr || s.frustumDtor == nullptr || s.frustumSetFrustum == nullptr ||
        s.frustumIntersectsSphere == nullptr || s.vectorGetTransformed == nullptr || s.matrixGetInverted == nullptr || s.sharedCosf == nullptr ||
        matInvert_ == nullptr || matMulAssign_ == nullptr || matIdentity_ == nullptr) {
        spdlog::warn("native: pssm: UpdatePackedShadowMapVisual / SharedBase symbols not as expected: the PSSM pass stays the engine's");
        return false;
    }
    // OnUpdateOutput's technique: lea rdx, "PSSM" (0x8f9074) at 0x669329 ... call SetTechinque 0x75cde0 at 0x669341
    char name[5] = {};
    std::uint8_t call = 0;
    std::int32_t rel = 0;
    if (!safeRead(engine_ + 0x8f9074, name, 5) || std::memcmp(name, "PSSM", 5) != 0 || !readAt(engine_, 0x669341, call) || call != 0xe8 ||
        !readAt(engine_, 0x669342, rel) || 0x669341 + 5 + rel != 0x75cde0) {
        spdlog::warn("native: pssm: OnUpdateOutput's PSSM technique site not as expected: the PSSM pass stays the engine's");
        return false;
    }
    // the far corners' NDC constants (UpdatePackedShadowMapVisual copies 48 bytes: 0x9f1bc0 16, 0x9f1d30 16, 0x9f1c20 16)
    if (!safeRead(engine_ + 0x9f1bc0, pssmCornerData_, 16) || !safeRead(engine_ + 0x9f1d30, pssmCornerData_ + 4, 16) ||
        !safeRead(engine_ + 0x9f1c20, pssmCornerData_ + 8, 16)) {
        return false;
    }
    pssmCorners_ = pssmCornerData_;
    frustumCtor_ = reinterpret_cast<FrustumCtorFn>(s.frustumCtor);
    frustumDtor_ = reinterpret_cast<FrustumCtorFn>(s.frustumDtor);
    frustumSet_ = reinterpret_cast<FrustumSetFn>(s.frustumSetFrustum);
    frustumSphere_ = reinterpret_cast<FrustumSphereFn>(s.frustumIntersectsSphere);
    vecTransformed_ = reinterpret_cast<VecTransformedFn>(s.vectorGetTransformed);
    matInverted_ = reinterpret_cast<MatInvertedFn>(s.matrixGetInverted);
    cosf_ = reinterpret_cast<CosFn>(s.sharedCosf);
    spdlog::info("native: pssm: engine checks passed (technique site, corners ({} {} {}) ({} {} {}) ({} {} {}) ({} {} {}))", pssmCornerData_[0],
                 pssmCornerData_[1], pssmCornerData_[2], pssmCornerData_[3], pssmCornerData_[4], pssmCornerData_[5], pssmCornerData_[6],
                 pssmCornerData_[7], pssmCornerData_[8], pssmCornerData_[9], pssmCornerData_[10], pssmCornerData_[11]);
    return true;
}

bool EffectOps::pssmVertices(const std::uint8_t* map, std::int32_t cells) {
    U8* cellArray = ptr(map, 0x68);
    U8* cam = camera();
    if (cellArray == nullptr || cam == nullptr) return false;
    pssmVb_.assign(static_cast<std::size_t>(cells) * fxc::kPssmVerticesPerCell * fxc::kPssmVertex, 0);
    pssmIntersects_.assign(static_cast<std::size_t>(cells), 0);
    // the camera's near sphere: radius +0xbc / cos(+0xd4) (SharedBase g_cosf), centre +0x90
    alignas(16) float sphere[4] = {get<float>(cam, 0xbc) / cosf_(get<float>(cam, 0xd4)), get<float>(cam, 0x90), get<float>(cam, 0x94), get<float>(cam, 0x98)};
    alignas(16) std::uint8_t frustum[0x100]{};
    frustumCtor_(frustum);
    bool ok = true;
    for (std::int32_t c = 0; c < cells && ok; ++c) {
        U8* cell = cellArray + kCellStride * static_cast<std::size_t>(c);
        alignas(16) float inv[16], tex[16], m[16];
        if (!readAt(cell, 0x90, inv) || !readAt(cell, 0xd0, tex) || !readAt(cell, 0x10, m)) {
            ok = false;
            break;
        }
        matInvert_(inv);
        matMulAssign_(tex, r_);
        const std::uint32_t colour = ~get<std::uint32_t>(cell, 0x11c);
        std::uint8_t* v = pssmVb_.data() + static_cast<std::size_t>(c) * fxc::kPssmVerticesPerCell * fxc::kPssmVertex;
        for (int k = 0; k < 4; ++k) {
            alignas(16) float corner[4] = {pssmCorners_[3 * k], pssmCorners_[3 * k + 1], pssmCorners_[3 * k + 2], 0.0f};
            alignas(16) float out[4]{};
            vecTransformed_(corner, out, inv);
            std::memcpy(v + fxc::kPssmVertex * k, out, 12);
            std::memcpy(v + fxc::kPssmVertex * k + 0xc, &colour, 4);
            std::memcpy(v + fxc::kPssmVertex * k + 0x10, tex, 64);
        }
        alignas(16) float invView[16];
        matInverted_(m, invView);
        std::memcpy(v + fxc::kPssmVertex * 4, invView + 12, 12);
        std::memcpy(v + fxc::kPssmVertex * 4 + 0xc, &colour, 4);
        std::memcpy(v + fxc::kPssmVertex * 4 + 0x10, tex, 64);
        // OnUpdateOutput's per-cell test (SetFrustum with the cell matrix +0x90, Intersects the near sphere)
        alignas(16) float cm[16];
        readAt(cell, 0x90, cm);
        frustumSet_(frustum, cm);
        pssmIntersects_[static_cast<std::size_t>(c)] = frustumSphere_(frustum, sphere) ? 1 : 0;
        pssmInside_ += pssmIntersects_[static_cast<std::size_t>(c)];
    }
    frustumDtor_(frustum);
    return ok;
}

void EffectOps::packedVisual(void* maskPtr, void* mapPtr, bool ok) {
    if (!ok || verified_ <= 0 || !maskOn() || !inMask_ || illum_ == nullptr || maskPtr != ptr(illum_, kIllumMask)) return;
    if (pssmVerified_ == 0) pssmVerified_ = verifyPssm() ? 1 : -1;
    if (unit_ != Unit::None) finishUnit();
    auto* mask = static_cast<U8*>(maskPtr);
    auto* map = static_cast<U8*>(mapPtr);
    unit_ = Unit::Pssm;
    unitFilter_ = mask;
    unitTake_ = takeOver_;
    unitCompare_ = compare_;
    const std::int32_t cells = get<std::int32_t>(map, 0x70);
    const std::int32_t size = get<std::int32_t>(map, 0xc);
    std::string technique = std::string("PSSM") + (get<std::uint8_t>(mask, 0x114) != 0 ? "" : "_Low");
    Skip s = Skip::None;
    U8* cam = camera();
    if (pssmVerified_ < 0) {
        s = Skip::Packed;
    } else if (cells <= 0 || cells > static_cast<std::int32_t>(fxc::kPssmMaxCells) || size <= 0 || cam == nullptr) {
        s = Skip::Unreadable;
    } else {
        // the parameters OnUpdateOutput sets before the loop (as the parameter objects' Set receives them)
        const auto bytes = [&](const char* name, const void* data, std::size_t n) {
            Override& o = overrides_[name];
            o.bytes.assign(static_cast<U8*>(data), static_cast<U8*>(data) + n);
            o.isImage = false;
        };
        alignas(16) float vp[16], v[16], tvp[16];
        readAt(cam, 0x50, vp);
        readAt(cam, 0x10, v);
        matMulAssign_(vp, v);
        bytes("ViewProj", vp, 64);
        // TexViewProj: identity, _11 0.5, _22 -0.5, _33 1, _41 / _42 = 0.5 / (the mask target's width / height) + 0.5, _43 0
        // (the viewport GetViewport returns at OnUpdateOutput's start: the mask's own target), then *= ViewProj
        matIdentity_(tvp);
        tvp[0] = k_.half;
        tvp[5] = -k_.half;
        tvp[14] = 0.0f;
        tvp[12] = k_.half / static_cast<float>(get<std::int32_t>(mask, 0x80)) + k_.half;
        tvp[13] = k_.half / static_cast<float>(get<std::int32_t>(mask, 0x84)) + k_.half;
        matMulAssign_(tvp, vp);
        bytes("TexViewProj", tvp, 64);
        const float fs = static_cast<float>(size);
        const float kernel = k_.packedKernel / fs;
        const float offset[4] = {k_.negHalf / fs, k_.negHalf / fs, k_.half / fs, k_.half / fs};
        const float scale[2] = {fs, fs};
        bytes("DizzerKernelSize", &kernel, 4);
        Override& tex = overrides_["ShadowTexture"];
        tex.image = ptr(map, 0x18);
        tex.isImage = true;
        bytes("PCFFilterOffset", offset, 16);
        bytes("PCFFilterScale", scale, 8);
        if (!pssmVertices(map, cells)) s = Skip::Unreadable;
        else s = composeEffect(mask, technique, nullptr, nullptr, false);
        ++pssmUnits_;
        pssmCells_ += static_cast<std::uint64_t>(cells);
    }
    unitName_ = "pssm " + technique;
    stats_ = &window_[unitName_];
    ++stats_->counts.units;
    if (s != Skip::None) {
        ++stats_->skips[static_cast<std::size_t>(s)];
        drawCount_ = 0;
        unit_ = Unit::None;
        leftUnit(mask, s);
        return;
    }
    ++stats_->counts.unitsComposed;
    engineDraws_ = 0;
    taken_ = false;
    if (unitTake_) {
        issuePssm();
        taken_ = true;
    }
}

bool EffectOps::pssmGeometryEqual(std::size_t k, std::string& why) {
    const fxc::EffectDraw& c = *scratch_;  // the captured draw (compareDraw's)
    const std::size_t cell = k / std::max<std::size_t>(1, drawCount_ / std::max<std::size_t>(1, pssmIntersects_.size()));
    const std::size_t bytes = fxc::kPssmVerticesPerCell * fxc::kPssmVertex;
    if ((cell + 1) * bytes > pssmVb_.size()) {
        why = "no composed vertices";
        return false;
    }
    auto* vb = const_cast<d3d9::Buffer*>(static_cast<const d3d9::Buffer*>(c.streams[0].buffer));
    auto* ib = const_cast<d3d9::Buffer*>(static_cast<const d3d9::Buffer*>(c.indices));
    std::vector<std::uint8_t> got(bytes);
    const std::uint32_t at = c.streams[0].offset + static_cast<std::uint32_t>(c.baseVertex + static_cast<std::int32_t>(c.minIndex)) * c.streams[0].stride;
    if (vb == nullptr || c.streams[0].stride != fxc::kPssmVertex || !vb->readCpu(at, static_cast<std::uint32_t>(bytes), got.data())) {
        why = "vertex data unreadable";
        return false;
    }
    if (std::memcmp(got.data(), pssmVb_.data() + cell * bytes, bytes) != 0) {
        std::size_t i = 0;
        while (i < bytes && got[i] == pssmVb_[cell * bytes + i]) ++i;
        why = std::format("cell {} vertex {} byte {:#x} differs", cell, i / fxc::kPssmVertex, i % fxc::kPssmVertex);
        return false;
    }
    std::uint32_t idx[18]{};
    if (ib == nullptr || !ib->readCpu(c.startIndex * 4, sizeof(idx), idx) || std::memcmp(idx, fxc::kPssmIndices, sizeof(idx)) != 0) {
        why = "index data differs / unreadable";
        return false;
    }
    return true;
}

void EffectOps::issuePssm() {
    if (drawCount_ == 0) return;
    const auto need = static_cast<std::uint32_t>(fxc::kPssmMaxCells * fxc::kPssmVerticesPerCell * fxc::kPssmVertex);
    if (!pssmOwnVb_) pssmOwnVb_ = dev_.createBuffer(need, d3d9::USAGE_DYNAMIC | d3d9::USAGE_WRITEONLY, d3d9::POOL_DEFAULT, false, false);
    if (!pssmOwnVb_ || pssmVb_.size() > need) return;
    // one DISCARD lock of the cells' range (as UpdatePackedShadowMapVisual's whole-buffer DISCARD: the engine's offsets)
    if (void* p = pssmOwnVb_->lock(0, static_cast<std::uint32_t>(pssmVb_.size()), d3d9::LOCK_DISCARD)) {
        std::memcpy(p, pssmVb_.data(), pssmVb_.size());
        pssmOwnVb_->unlock();
    }
    captureDevice(*saved_);
    d3d9::draws::Tag& tag = dev_.drawTag();
    const d3d9::draws::Tag savedTag = tag;
    issuing_ = true;
    for (std::size_t k = 0; k < drawCount_; ++k) {
        fxc::EffectDraw& d = *draws_[k];
        const fxc::Stream engineStream = d.streams[0];
        d.streams[0].buffer = pssmOwnVb_.get();
        d.streams[0].offset = 0;
        applyDevice(d);
        issueIndex_ = k;
        dev_.drawIndexedPrimitive(d.primType, d.baseVertex, d.minIndex, d.numVertices, d.startIndex, d.primCount);
        ++stats_->issuedDraws;
        d.streams[0] = engineStream;
    }
    issuing_ = false;
    tag = savedTag;
    applyDevice(*saved_);
}

}  // namespace rvk::native
