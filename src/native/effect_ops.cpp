#include "native/effect_ops.h"

#include <windows.h>

#include <algorithm>
#include <cstring>
#include <format>

#include <spdlog/spdlog.h>

#include "d3d9/defs.h"
#include "engine/memread.h"
#include "engine/symbols.h"
#include "fx/effect_binary.h"
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

// eTEffectParam<T> / eSImageEffectParam* vtables (Engine.dll RVAs; README.md 13): the value's byte count N of SetParameter(handle,
// +0x58, N), -1 = transposed bCMatrix
struct ParamClass {
    std::uint32_t vtable;
    std::int32_t bytes;
};
constexpr ParamClass kValueClasses[] = {
    {0x9edcb8, 4},    // eTEffectParam<float>
    {0x9ed928, 4},    // eTEffectParam<int>
    {0x9ed828, 4},    // eTEffectParam<long>
    {0x9edab8, 8},    // eTEffectParam<bCVector2>
    {0x9edb38, 12},   // eTEffectParam<bCVector>
    {0x9edeb8, 16},   // eTEffectParam<bCVector4>
    {0x9edf38, 64},   // eTEffectParam<bCMatrix>
    {0x9eddb8, -1},   // eTEffectParamTransposedMatrix<bCMatrix>
};
// eTEffectParamArray<T>: a heap buffer at +0x58 of elements (+0x30) x the element's bytes
constexpr ParamClass kArrayClasses[] = {
    {0x9eda38, 4},   // int
    {0x9ed9a8, 4},   // long
    {0x9ed8a8, 4},   // float
    {0x9edd38, 8},   // bCVector2
    {0x9edbb8, 12},  // bCVector
    {0x9edc38, 16},  // bCVector4
};
constexpr std::uint32_t kParamTexture = 0x9ee880;    // eSImageEffectParamTexture (+0x58 image, +0x60 mode)
constexpr std::uint32_t kParamImageSize = 0x9ee280;  // TEXTURESIZEn (+0x58 the suffix)
constexpr std::uint32_t kParamCamera = 0x9ee480;     // VIEW .. FOCALRANGEFAR (NEARFAR: camera +0xbc)

// illumination admin fields (E-shadows-lights-sky.md 2.5)
constexpr std::size_t kIllumSsao = 0x430, kIllumBlurSsao1 = 0x438, kIllumBlurSsao2 = 0x440, kIllumMask = 0x448, kIllumDepthCopy = 0x450,
                      kIllumBlur = 0x458, kIllumTerrain = 0x358;

constexpr const char* kUnitNames[] = {"-", "chain", "mask", "terrain blur", "copy", "post", "gui"};

}  // namespace

const char* EffectOps::name(Skip s) {
    static constexpr const char* kNames[] = {"none", "no effect", "technique", "parameter class", "semantic", "input", "image", "visual",
                                             "point-light pass", "unreadable", "buffer", "sorted"};
    static_assert(std::size(kNames) == static_cast<std::size_t>(Skip::Count));
    return kNames[static_cast<std::size_t>(s)];
}

void EffectOps::Stats::add(const Stats& o) {
    counts.add(o.counts);
    for (std::size_t i = 0; i < skips.size(); ++i) skips[i] += o.skips[i];
    issuedDraws += o.issuedDraws;
    droppedDraws += o.droppedDraws;
    ownChecked += o.ownChecked;
    ownMismatch += o.ownMismatch;
}

EffectOps::EffectOps(hal::vulkan::HalDevice& hal) : hal_(hal), dev_(hal.dev()) {
    scratch_ = std::make_unique<fxc::EffectDraw>();
    saved_ = std::make_unique<fxc::EffectDraw>();
}

EffectOps::~EffectOps() {
    engine::scene::setEffectTap(nullptr);
    setupTake_ = setupCompare_ = false;
    installSetupTap();
}

void EffectOps::setMode(bool takeOver, bool compare) {
    takeOver_ = takeOver;
    compare_ = compare;
    if (active() && verified_ == 0) verified_ = verify() ? 1 : -1;
    if (setupOn() && setupVerified_ == 0 && verified_ > 0) setupVerified_ = verifySetup() ? 1 : -1;
    if (postOn() && postVerified_ == 0 && verified_ > 0) postVerified_ = verifyPost() ? 1 : -1;
    if (guiOn() && guiVerified_ == 0 && verified_ > 0) guiVerified_ = verifyGui() ? 1 : -1;
    installSetupTap();
    engine::scene::setEffectTap(active() && verified_ > 0 ? this : nullptr);
}

void EffectOps::setPostMode(bool takeOver, bool compare) {
    postTake_ = takeOver;
    postCompare_ = compare;
    setMode(takeOver_, compare_);
}

void EffectOps::setGuiMode(bool takeOver, bool compare) {
    guiTake_ = takeOver;
    guiCompare_ = compare;
    setMode(takeOver_, compare_);
}

void EffectOps::setSetupMode(bool takeOver, bool compare) {
    setupTake_ = takeOver;
    setupCompare_ = compare;
    setMode(takeOver_, compare_);
}

bool EffectOps::verify() {
    engine_ = reinterpret_cast<U8*>(GetModuleHandleW(L"Engine.dll"));
    const auto& s = engine::sym();
    if (engine_ == nullptr || s.imageFilterExecuteEffect == nullptr || s.imageFilterCopyUpdate == nullptr || s.materialAdminGetInstance == nullptr ||
        s.appInstancePtr == nullptr || s.matrixInvert == nullptr || s.matrixMulAssign == nullptr || s.matrixSetToScaling == nullptr ||
        s.matrixSetIdentity == nullptr) {
        spdlog::warn("native: effect draws: engine symbols missing: the shadow mask / terrain blur stay the engine's");
        return false;
    }
    vtEffect_ = s.imageFilterEffectVftable;
    vtMask_ = s.imageFilterShadowMaskVftable;
    vtCopy_ = s.imageFilterCopyVftable;
    vtTexture_ = s.imageFilterTextureVftable;
    vtTextureRandom_ = s.imageFilterTextureRandomVftable;
    // the classes the composition knows by RVA (README.md 13) and the vtable slots ExecuteEffect / OnSetParams use
    const auto at = [&](const void* p, std::uint32_t rva) { return p == engine_ + rva; };
    if (!at(vtEffect_, 0x9ee0b8) || !at(vtMask_, 0x9c87b0) || !at(vtCopy_, 0x9c61a0) || !at(vtTexture_, 0x9c8d98) || !at(vtTextureRandom_, 0x9a26d8)) {
        spdlog::warn("native: effect draws: the image filters' vtables are not at the expected RVAs: off");
        return false;
    }
    static constexpr std::pair<std::uint32_t, std::uint32_t> kSlots[] = {
        {0x148, 0x668370}, {0x178, 0x75adf0}, {0x180, 0x75aec0}, {0x188, 0x75c740}, {0x198, 0x75b090},
        {0x1a0, 0x75b230}, {0x1a8, 0x75b1a0}, {0x1b0, 0x75ca60}, {0x1b8, 0x75af70}};
    for (const void* vt : {vtEffect_, vtMask_}) {
        for (const auto& [off, rva] : kSlots) {
            const void* f = nullptr;
            if (!readAt(vt, off, f) || f != engine_ + rva) {
                spdlog::warn("native: effect draws: filter vtable +{:#x} is not RVA {:#x}: off", off, rva);
                return false;
            }
        }
    }
    if (s.imageFilterExecuteEffect != engine_ + 0x75af70) return false;
    // the constants of the mask's OnUpdateOutput 0x668560 and UpdateCamera
    const auto f = [&](std::uint32_t rva, float& out) { return readAt(engine_, rva, out); };
    if (!f(0x9f1718, k_.kernel) || !f(0x9f184c, k_.negHalf) || !f(0x9f1630, k_.half) || !f(0x9f15d4, k_.fadeFraction) || !f(0x9f16f8, k_.packedKernel) ||
        !f(0x9f1698, one_)) {
        return false;
    }
    materialAdmin_ = reinterpret_cast<void* (*)()>(s.materialAdminGetInstance);
    matInvert_ = reinterpret_cast<MatFn>(s.matrixInvert);
    matIdentity_ = reinterpret_cast<MatFn>(s.matrixSetIdentity);
    matMulAssign_ = reinterpret_cast<MatMulAssignFn>(s.matrixMulAssign);
    matScale_ = reinterpret_cast<MatScaleFn>(s.matrixSetToScaling);
    spdlog::info("native: effect draws: engine checks passed (filter vtables, constants kernel {} offsets {} / {} fade {} packed {})", k_.kernel,
                 k_.negHalf, k_.half, k_.fadeFraction, k_.packedKernel);
    return true;
}

const EffectOps::Live& EffectOps::liveOf(const d3d9::Shader* s) {
    auto [it, inserted] = live_.try_emplace(s);
    if (inserted && s != nullptr) {
        // every register the bytecode reads (an effect shader's preshader outputs are not in its CTAB) and the CTAB's
        const bool vertex = s->stage == shader::Stage::Vertex;
        for (std::uint32_t reg = 0; reg < s->reflection.floatConstCount; ++reg) {
            if (vertex && reg < fxc::kVsRegs) it->second.vs.set(reg);
            if (!vertex && reg < fxc::kPsRegs) it->second.ps.set(reg);
        }
        const auto r = fx::parseConstantTable(s->bytecode);
        if (r.ok()) {
            for (const fx::Constant& c : r.table.constants) {
                if (c.registerSet != fx::RegisterSet::Float4) continue;
                for (std::uint32_t i = 0; i < c.registerCount; ++i) {
                    const std::uint32_t reg = c.registerIndex + i;
                    if (s->stage == shader::Stage::Vertex && reg < fxc::kVsRegs) it->second.vs.set(reg);
                    if (s->stage != shader::Stage::Vertex && reg < fxc::kPsRegs) it->second.ps.set(reg);
                }
            }
        }
    }
    return it->second;
}

d3d9::Texture* EffectOps::textureOfImage(const void* imageData) const {
    U8* api = ptr(imageData, 0x60);
    if (api == nullptr) return nullptr;
    d3d9::Texture* t = hal_.textureOfApi(api);
    return t != nullptr ? t : hal_.cubeOfApi(api);
}

d3d9::Surface* EffectOps::surfaceOfImage(const void* imageData) const {
    d3d9::Texture* t = textureOfImage(imageData);
    return t != nullptr ? t->surface(0, 0) : nullptr;
}

bool EffectOps::outputImage(const std::uint8_t* filter, std::int32_t index, const void*& image) const {
    image = nullptr;
    if (filter == nullptr) return true;  // an unconnected input: AttachImmImageData(handle, null)
    const void* vt = ptr(filter, 0);
    if (vt == vtTexture_ || vt == vtTextureRandom_) {
        // eCImageFilterTexture::GetOutputAt 0x66b360: index 0 only; mode +0x38: 0 the own image +0x40, 1 the refraction copy, 2 the depth map
        if (index != 0) return true;
        const std::int32_t mode = get<std::int32_t>(filter, 0x38, -1);
        if (mode == 0) image = ptr(filter, 0x40);
        else if (mode == 1 || mode == 2) image = ptr(materialAdmin_(), mode == 1 ? 0x268 : 0x290);
        return mode >= 0 && mode <= 2;
    }
    if (vt == vtCopy_ || vt == vtEffect_ || vt == vtMask_) {
        // eCImageFilterRTBase::GetOutputAt 0x6680f0: one output (+0x70) when the output mode +0x38 is 0 (a texture)
        if (index == 0 && get<std::int32_t>(filter, 0x38, -1) == 0) image = ptr(filter, 0x70);
        return true;
    }
    return false;
}

const std::uint8_t* EffectOps::camera() const {
    U8* app = ptr(engine::sym().appInstancePtr, 0);
    U8* cam = ptr(app, 0x38);
    return cam != nullptr ? cam : ptr(app, 0xc8);
}

// ---- device state ------------------------------------------------------------------------------------------------------------

void EffectOps::captureDevice(fxc::EffectDraw& out) const {
    const d3d9::Device& d = dev_;
    out.vs = d.vertexShader();
    out.ps = d.pixelShader();
    out.decl = d.vertexDecl();
    for (std::uint32_t i = 0; i < 4; ++i) {
        const auto s = d.streamSource(i);
        out.streams[i] = {s.buffer, s.offset, s.stride, s.freq};
    }
    out.indices = d.indexBuffer();
    for (std::uint32_t rs = 0; rs < fxc::kRenderStates; ++rs) out.rs[rs] = d.renderState(rs);
    for (std::uint32_t st = 0; st < fxc::kStages; ++st) {
        out.textures[st] = d.texture(st);
        for (std::uint32_t s = 1; s < fxc::kSamplerStates; ++s) out.ss[st][s] = d.samplerState(st, s);
    }
    std::memcpy(out.vsF, d.vsConstants(), sizeof(out.vsF));
    std::memcpy(out.psF, d.psConstants(), sizeof(out.psF));
    std::memcpy(out.vsB.data(), d.vsBoolConstants(), sizeof(out.vsB));
    std::memcpy(out.psB.data(), d.psBoolConstants(), sizeof(out.psB));
    std::memcpy(out.vsI, d.vsIntConstants(), sizeof(out.vsI));
    std::memcpy(out.psI, d.psIntConstants(), sizeof(out.psI));
    out.rt0 = d.renderTarget(0);
    out.depth = d.depthStencil();
    const d3d9::Viewport& v = d.viewport();
    out.viewport = {v.x, v.y, v.width, v.height, v.minZ, v.maxZ};
    out.rsWritten.reset();
    out.vsRegsWritten = out.psRegsWritten = 0;
    out.unsupported = false;
}

void EffectOps::capture(const d3d9::DrawTapInfo& t, fxc::EffectDraw& out, fxc::Live& live) {
    captureDevice(out);
    out.indexed = t.indexed;
    out.primType = t.primitiveType;
    out.primCount = t.primitives;
    if (t.indexed) {
        out.baseVertex = t.baseVertex;
        out.minIndex = t.minIndex;
        out.numVertices = t.numVertices;
        out.startIndex = t.first;
        out.startVertex = 0;
    } else {
        out.startVertex = t.first;
        out.baseVertex = 0;
        out.minIndex = out.numVertices = out.startIndex = 0;
    }
    const auto* vs = static_cast<const d3d9::Shader*>(out.vs);
    const auto* ps = static_cast<const d3d9::Shader*>(out.ps);
    live.vs = liveOf(vs).vs;
    live.ps = liveOf(ps).ps;
    live.stages = ps != nullptr ? static_cast<std::uint16_t>(ps->samplerMask & 0xffffu) : 0;
}

void EffectOps::applyDevice(const fxc::EffectDraw& d) {
    using namespace d3d9;
    dev_.setVertexShaderRef(ShaderRef(const_cast<Shader*>(static_cast<const Shader*>(d.vs))));
    dev_.setPixelShaderRef(ShaderRef(const_cast<Shader*>(static_cast<const Shader*>(d.ps))));
    dev_.setVertexDecl(const_cast<VertexDecl*>(static_cast<const VertexDecl*>(d.decl)));
    for (std::uint32_t i = 0; i < 4; ++i) {
        const auto& s = d.streams[i];
        const auto cur = dev_.streamSource(i);
        if (cur.buffer != s.buffer || cur.offset != s.offset || cur.stride != s.stride) {
            dev_.setStreamSource(i, const_cast<Buffer*>(static_cast<const Buffer*>(s.buffer)), s.offset, s.stride);
        }
        if (dev_.streamSource(i).freq != s.freq) dev_.setStreamSourceFreq(i, s.freq);
    }
    dev_.setIndices(const_cast<Buffer*>(static_cast<const Buffer*>(d.indices)));
    for (std::uint32_t rs = 0; rs < fxc::kRenderStates; ++rs) {
        if (dev_.renderState(rs) != d.rs[rs]) dev_.setRenderState(rs, d.rs[rs]);
    }
    for (std::uint32_t st = 0; st < fxc::kStages; ++st) {
        if (dev_.texture(st) != d.textures[st]) dev_.setTexture(st, const_cast<Texture*>(static_cast<const Texture*>(d.textures[st])));
        for (std::uint32_t s = 1; s < fxc::kSamplerStates; ++s) {
            if (dev_.samplerState(st, s) != d.ss[st][s]) dev_.setSamplerState(st, s, d.ss[st][s]);
        }
    }
    for (std::uint32_t r = 0; r < fxc::kVsRegs; ++r) {
        if (std::memcmp(dev_.vsConstant(r), d.vsF[r], 16) != 0) dev_.setVertexShaderConstantF(r, d.vsF[r], 1);
    }
    for (std::uint32_t r = 0; r < fxc::kPsRegs; ++r) {
        if (std::memcmp(dev_.psConstant(r), d.psF[r], 16) != 0) dev_.setPixelShaderConstantF(r, d.psF[r], 1);
    }
    if (std::memcmp(dev_.vsBoolConstants(), d.vsB.data(), sizeof(d.vsB)) != 0) {
        std::array<std::int32_t, 16> b{};
        for (std::size_t i = 0; i < 16; ++i) b[i] = static_cast<std::int32_t>(d.vsB[i]);
        dev_.setVertexShaderConstantB(0, b.data(), 16);
    }
    if (std::memcmp(dev_.psBoolConstants(), d.psB.data(), sizeof(d.psB)) != 0) {
        std::array<std::int32_t, 16> b{};
        for (std::size_t i = 0; i < 16; ++i) b[i] = static_cast<std::int32_t>(d.psB[i]);
        dev_.setPixelShaderConstantB(0, b.data(), 16);
    }
    if (std::memcmp(dev_.vsIntConstants(), d.vsI, sizeof(d.vsI)) != 0) dev_.setVertexShaderConstantI(0, &d.vsI[0][0], 16);
    if (std::memcmp(dev_.psIntConstants(), d.psI, sizeof(d.psI)) != 0) dev_.setPixelShaderConstantI(0, &d.psI[0][0], 16);
    if (dev_.renderTarget(0) != d.rt0) dev_.setRenderTarget(0, const_cast<Surface*>(static_cast<const Surface*>(d.rt0)));
    if (dev_.depthStencil() != d.depth) dev_.setDepthStencil(const_cast<Surface*>(static_cast<const Surface*>(d.depth)));
    const Viewport& v = dev_.viewport();
    if (v.x != d.viewport.x || v.y != d.viewport.y || v.width != d.viewport.width || v.height != d.viewport.height || v.minZ != d.viewport.minZ ||
        v.maxZ != d.viewport.maxZ) {
        dev_.setViewport({d.viewport.x, d.viewport.y, d.viewport.width, d.viewport.height, d.viewport.minZ, d.viewport.maxZ});
    }
}

// ---- the mask -------------------------------------------------------------------------------------------------------------------

void EffectOps::maskBegin(void* illum) {
    if (illum != nullptr) illum_ = static_cast<U8*>(illum);  // also for the setup (the working buffers)
    if (verified_ <= 0 || !maskOn()) return;
    illum_ = static_cast<U8*>(illum);
    inMask_ = true;
    ++masks_;
    maskDepth_ = dev_.depthStencil();
    overrides_.clear();
    plan_.clear();
    planCascade_.clear();
    planNext_ = 0;
    // eCImageFilterShadowMask::OnUpdateOutput 0x668560: UpscaleSSAO with SSAO (+0xfc), then with cascades (+0x118, split count
    // +0x28 > 0) one SSM (terrain map +0x120) / DSSM quad per split (suffix "_Low" without HighQuality +0x114), then VSSM with a
    // terrain map, then PSSM per packed map with cells (+0x128 / +0x130: not composed)
    U8* mask = ptr(illum_, kIllumMask);
    if (mask == nullptr) return;
    const bool ssao = get<std::uint8_t>(mask, 0xfc) != 0;
    const bool hq = get<std::uint8_t>(mask, 0x114) != 0;
    U8* cascade = ptr(mask, 0x118);
    U8* terrain = ptr(mask, 0x120);
    if (ssao) {
        plan_.push_back("UpscaleSSAO");
        planCascade_.push_back(-1);
    }
    const std::int32_t splits = cascade != nullptr ? get<std::int32_t>(cascade, 0x28) : 0;
    for (std::int32_t i = 0; i < splits; ++i) {
        plan_.push_back(std::string(terrain != nullptr ? "SSM" : "DSSM") + (hq ? "" : "_Low"));
        planCascade_.push_back(i);
    }
    if (terrain != nullptr) {
        plan_.push_back("VSSM");
        planCascade_.push_back(-1);
    }
    // the packed point-light maps (+0x128, 0xb0 each, count +0x130) with cells (+0x70): the PSSM pass (not composed: its draws are
    // counted as "outside the units")
    if (U8* packed = ptr(mask, 0x128); packed != nullptr) {
        const std::int32_t maps = get<std::int32_t>(mask, 0x130);
        for (std::int32_t m = 0; m < maps; ++m) {
            U8* map = packed + 0xb0 * static_cast<std::size_t>(m);
            const std::int32_t cells = get<std::int32_t>(map, 0x70);
            if (cells > 0) {
                ++packedMasks_;
                // the cells' colour flags (+0x11c of the 0x120-byte cells at +0x68): the PSSM pyramids' vertex colour is ~flags, the
                // pass writes max(1 - lit, colour) through ZERO / SRCCOLOR, so a channel whose colour bit is 0 receives the shadow
                if (packedLogged_ < 3) {
                    ++packedLogged_;
                    std::string flags;
                    U8* cellArray = ptr(map, 0x68);
                    for (std::int32_t c = 0; c < cells && c < 12; ++c) {
                        flags += std::format("{}{:#x}", flags.empty() ? "" : " ", get<std::uint32_t>(cellArray + 0x120 * static_cast<std::size_t>(c), 0x11c));
                    }
                    spdlog::info("native: effect draws: point-light shadow cells: map {} of {}^2, {} cells, flags {} (the PSSM pass: left to the engine)", m,
                                 get<std::int32_t>(map, 0xc), cells, flags);
                }
                break;
            }
        }
    }
    // R = inv(V) *= inv(diag(P11, P22, 1)) *= scale(far) (SharedBase's bCMatrix functions, as the mask builds it)
    U8* cam = camera();
    float v[16], p[16], sc[16];
    if (cam == nullptr || !readAt(cam, 0x10, v)) return;
    matInvert_(v);
    matIdentity_(p);
    p[0] = get<float>(cam, 0x50);
    p[5] = get<float>(cam, 0x64);
    matInvert_(p);
    matScale_(sc, get<float>(cam, 0xc0));
    std::memcpy(r_, v, sizeof(r_));
    matMulAssign_(r_, p);
    matMulAssign_(r_, sc);
}

void EffectOps::maskEnd() {
    inMask_ = false;
    if (unit_ != Unit::None) finishUnit();
}

bool EffectOps::maskStep(std::size_t step, std::string& technique) {
    if (step >= plan_.size()) return false;
    technique = plan_[step];
    U8* mask = ptr(illum_, kIllumMask);
    U8* cascade = ptr(mask, 0x118);
    U8* terrain = ptr(mask, 0x120);
    U8* cam = camera();
    const auto bytes = [&](const char* name, const void* data, std::size_t n) {
        Override& o = overrides_[name];
        o.bytes.assign(static_cast<U8*>(data), static_cast<U8*>(data) + n);
        o.isImage = false;
    };
    const auto image = [&](const char* name, const void* img) {
        Override& o = overrides_[name];
        o.image = img;
        o.isImage = true;
    };
    const auto varianceMatrix = [&]() {
        float m[16];
        if (!readAt(terrain, 0xa0, m)) return false;
        matMulAssign_(m, r_);
        bytes("DepthToVarianceShadowMatrix", m, 64);
        return true;
    };
    const std::int32_t i = planCascade_[step];
    if (i >= 0) {
        // the per-cascade values (the first cascade also writes the shared ones, in the engine's order)
        U8* splits = ptr(cascade, 0x30);
        U8* split = splits != nullptr ? splits + 0x168 * static_cast<std::size_t>(i) : nullptr;
        if (split == nullptr || cam == nullptr) return false;
        const float maxDistance = get<float>(cascade, 0x1c);
        if (i == 0) {
            float kernel, offset[4], scale[2], fade[2];
            fxc::cascadeFilter(k_, get<std::int32_t>(ptr(cascade, 0x30), 0xc), kernel, offset, scale);
            bytes("DizzerKernelSize", &kernel, 4);
            bytes("PCFFilterOffset", offset, 16);
            bytes("PCFFilterScale", scale, 8);
            fxc::fadeOut(k_, maxDistance, get<float>(cam, 0xc0), fade);
            bytes("FadOut", fade, 8);
            if (terrain != nullptr) {
                if (!varianceMatrix()) return false;
                image("VarianceShadowTexture", ptr(terrain, 0x18));
            }
        }
        float m[16];
        if (!readAt(split, 0xa0, m)) return false;
        matMulAssign_(m, r_);
        bytes("DepthToShadowMatrix", m, 64);
        const std::int32_t count = get<std::int32_t>(cascade, 0x38);
        const float f = i + 1 < count ? get<float>(split + 0x168, 0x160) : maxDistance;
        const float z = fxc::zDistance(f, get<float>(cam, 0x78), get<float>(cam, 0x88));
        bytes("ZDistance", &z, 4);
        image("ShadowTexture", ptr(split, 0x18));
        float kernel, offset[4], scale[2];
        fxc::cascadeFilter(k_, get<std::int32_t>(split, 0xc), kernel, offset, scale);
        bytes("PCFFilterOffset", offset, 16);
        bytes("PCFFilterScale", scale, 8);
        bytes("DizzerKernelSize", &kernel, 4);
    } else if (technique == "VSSM") {
        if (!varianceMatrix()) return false;
        bytes("ZDistance", &one_, 4);
        image("VarianceShadowTexture", ptr(terrain, 0x18));
    }
    return true;
}

void EffectOps::effectBegin(void* filterPtr) {
    if (verified_ <= 0 || !(maskOn() || postOn())) return;
    auto* filter = static_cast<U8*>(filterPtr);
    if (unit_ != Unit::None) finishUnit();
    Unit unit = Unit::None;
    std::string technique;
    const void* input0 = nullptr;
    const void* target = nullptr;
    bool screen = false;
    U8* terrain = ptr(illum_, kIllumTerrain);
    if (postOn() && postVerified_ > 0 && postDepth_ > 0 && !engine::scene::inSceneJob() && dev_.phases().view() == d3d9::scene::View::Post &&
        ptr(filter, 0) == vtEffect_) {
        // N5.7: a job of the post chain (any effect filter eCImageEffect::Execute runs outside the scene job)
        unit = Unit::Post;
    } else if (!maskOn()) {
        return;
    } else if (inMask_ && illum_ != nullptr) {
        if (filter == ptr(illum_, kIllumMask)) unit = Unit::Mask;
        else if (filter == ptr(illum_, kIllumSsao) || filter == ptr(illum_, kIllumBlurSsao1) || filter == ptr(illum_, kIllumBlurSsao2) ||
                 filter == ptr(illum_, kIllumBlur)) {
            unit = Unit::Chain;
        }
    } else if (terrain != nullptr && filter == ptr(terrain, 0x110)) {
        // eCShadowMapDirectionalLight2VS::RenderShadowMap 0x5a1b40: Blur7H (map +0x18 -> temp +0x108), Blur7V (temp -> map); the
        // filter draws into the bound target (output mode 1), without depth
        unit = Unit::TerrainBlur;
        const std::uint64_t frame = dev_.stats().frames;
        if (frame != blurFrame_) {
            blurFrame_ = frame;
            blurIndex_ = 0;
        }
        const bool horizontal = blurIndex_++ % 2 == 0;
        technique = horizontal ? "Blur7H" : "Blur7V";
        input0 = ptr(terrain, horizontal ? 0x18 : 0x108);
        target = ptr(terrain, horizontal ? 0x108 : 0x18);
        screen = true;
    }
    if (unit == Unit::None) return;
    unit_ = unit;
    unitFilter_ = filter;
    unitTake_ = unit == Unit::Post ? postTake_ : takeOver_;
    unitCompare_ = unit == Unit::Post ? postCompare_ : compare_;
    if (unit == Unit::Mask) {
        if (!maskStep(planNext_++, technique)) technique = "?";
    } else if (unit == Unit::Chain || unit == Unit::Post) {
        technique.clear();  // the filter's own (set at configuration: SSAO, BlurSSAO, Blur; the post filters' from the graph data)
    }
    const Skip s = composeEffect(filter, technique, input0, target, screen);
    if (unit == Unit::Post) unitName_ = engine::readCString(filter + 0x10) + (unitName_.empty() ? std::string() : " " + unitName_);
    unitName_ = std::string(kUnitNames[static_cast<std::size_t>(unit)]) + (unitName_.empty() ? std::string() : " " + unitName_);
    stats_ = &window_[unitName_];
    ++stats_->counts.units;
    if (s != Skip::None) {
        ++stats_->skips[static_cast<std::size_t>(s)];
        drawCount_ = 0;
        unit_ = Unit::None;  // left to the engine
        return;
    }
    ++stats_->counts.unitsComposed;
    engineDraws_ = 0;
    taken_ = false;
    if (unitTake_) {
        issue();
        taken_ = true;
    }
}

void EffectOps::effectEnd(void* filter) {
    if (unit_ != Unit::None && filter == unitFilter_) finishUnit();
}

EffectOps::Skip EffectOps::composeEffect(const std::uint8_t* filter, const std::string& technique, const void* input0, const void* target,
                                         bool screenTarget) {
    drawCount_ = 0;
    unitName_.clear();
    const auto* info = hal::vulkan::EffectInfo::fromApi(ptr(ptr(filter, 0xa8), 0x18));
    if (info == nullptr) return Skip::NoEffect;
    const fx::Effect& e = info->fxEffect();
    // the technique: by name (the engine's SetTechinque searches the filter's list +0xc8, 0x18 each, in declaration order = the
    // effect's), else the filter's index +0xf0
    std::uint32_t tech = fx::kNone;
    if (!technique.empty()) {
        for (std::uint32_t t = 0; t < e.techniques.size(); ++t) {
            if (e.techniques[t].name == technique) {
                tech = t;
                break;
            }
        }
    } else {
        tech = get<std::uint32_t>(filter, 0xf0, fx::kNone);
    }
    if (tech >= e.techniques.size() || tech >= 32) return Skip::Technique;
    unitName_ = e.techniques[tech].name;
    const bool post = unit_ == Unit::Post;
    // the parameter values OnSetParams pushes: every parameter object used by the technique (+0x50 bit), its +0x58 bytes, the
    // overrides (the mask's), the semantic ones from their sources; then the inputs' images. The post chain starts from the values
    // the effect holds (a class that sets nothing this call leaves the last value: the carry-over)
    fx::ParameterValues values = post ? info->currentValues() : fx::ParameterValues::defaults(e);
    std::vector<const void*> paramTextures(e.parameters.size(), nullptr);
    if (post) {
        for (std::uint32_t i = 0; i < e.parameters.size(); ++i) paramTextures[i] = info->currentTexture(i);
    }
    const std::int32_t inputs = get<std::int32_t>(filter, 0xc0);
    U8* inputArray = ptr(filter, 0xb8);
    const auto inputImage = [&](std::int32_t k, const void*& img) {
        U8* entry = inputArray + 0x60 * static_cast<std::size_t>(k);
        if (k == 0 && input0 != nullptr) {
            img = input0;
            return true;
        }
        if (post) return graphOutput(ptr(entry, 0), get<std::int32_t>(entry, 0x10), img);
        return outputImage(ptr(entry, 0), get<std::int32_t>(entry, 0x10), img);
    };
    const std::int32_t params = get<std::int32_t>(filter, 0xe8);
    U8* paramArray = ptr(filter, 0xe0);
    for (std::int32_t k = 0; k < params; ++k) {
        U8* obj = ptr(paramArray, 8 * static_cast<std::size_t>(k));
        if (obj == nullptr) continue;
        if ((get<std::uint32_t>(obj, 0x50) >> tech & 1u) == 0) {
            ++unusedParams_;
            continue;
        }
        const std::uint32_t pi = info->parameterOf(ptr(obj, 0x10));
        if (pi == fx::kNone || pi >= e.parameters.size()) return Skip::Unreadable;
        auto& data = values.data[pi];
        const fx::Parameter& param = e.parameters[pi];
        const auto vt = static_cast<std::uint32_t>(static_cast<U8*>(ptr(obj, 0)) - engine_);
        const auto it = overrides_.find(param.name);
        const Override* ov = unitFilter_ == ptr(illum_, kIllumMask) && it != overrides_.end() ? &it->second : nullptr;
        const auto store = [&](const void* src, std::size_t n) { std::memcpy(data.data(), src, std::min(n, data.size())); };
        if (ov != nullptr) ++nativeParams_;
        if (post && vt != kParamImageSize && vt != kParamTexture) {
            // N5.7: the semantic classes (timer, weather, image filter admin, camera, sky dome, image) and the stored values
            std::vector<std::uint8_t> bytes;
            bool set = false;
            const Skip sk = semanticValue(obj, vt, param, bytes, set);
            if (sk == Skip::ParamClass) {
                // not a semantic class: a stored value (below)
            } else if (sk != Skip::None) {
                return sk;
            } else {
                if (fx::isTexture(param.type)) {
                    // eSImageEffectParamImage: AttachImmImageData of the resource's image (bytes = the image pointer)
                    const void* img = nullptr;
                    if (bytes.size() == sizeof(img)) std::memcpy(&img, bytes.data(), sizeof(img));
                    paramTextures[pi] = img != nullptr ? textureOfImage(img) : nullptr;
                } else if (set) {
                    store(bytes.data(), bytes.size());
                }
                continue;
            }
        }
        if (vt == kParamCamera) {
            if (param.semantic != "NEARFAR") return Skip::Semantic;
            float nf[2];
            if (!readAt(camera(), 0xbc, nf)) return Skip::Unreadable;
            store(nf, 8);
        } else if (vt == kParamImageSize) {
            const std::string suffix = engine::readCString(obj + 0x58);
            std::int32_t match = -1;
            for (std::int32_t j = 0; j < inputs; ++j) {
                if (engine::readCString(inputArray + 0x60 * static_cast<std::size_t>(j) + 0x58) == suffix) {
                    match = j;
                    break;
                }
            }
            const void* img = nullptr;
            if (match < 0 && post) continue;  // ImageSize::Set finds no input with the suffix: nothing is set
            if (match < 0 || !inputImage(match, img)) return Skip::Input;
            if (img == nullptr) continue;  // ImageSize::Apply fails without an image: nothing is set
            const float wh[2] = {static_cast<float>(get<std::uint32_t>(img, 0x18)), static_cast<float>(get<std::uint32_t>(img, 0x1c))};
            store(wh, 8);
        } else if (vt == kParamTexture) {
            const std::int32_t mode = get<std::int32_t>(obj, 0x60);
            const void* img = nullptr;
            if (ov != nullptr && ov->isImage) img = ov->image;
            else if (mode == 0) img = ptr(obj, 0x58);
            else if (mode == 1 || mode == 2) img = ptr(materialAdmin_(), mode == 1 ? 0x268 : 0x290);
            paramTextures[pi] = img != nullptr ? textureOfImage(img) : nullptr;
        } else {
            std::int32_t n = 0, element = 0;
            for (const ParamClass& c : kValueClasses) {
                if (c.vtable == vt) n = c.bytes;
            }
            for (const ParamClass& c : kArrayClasses) {
                if (c.vtable == vt) element = c.bytes;
            }
            if (element != 0) {
                const std::uint32_t count = get<std::uint32_t>(obj, 0x30);
                std::vector<std::uint8_t> arr(static_cast<std::size_t>(element) * count);
                if (ov != nullptr && !ov->isImage && ov->bytes.size() == arr.size()) arr = ov->bytes;
                else if (!arr.empty() && !safeRead(ptr(obj, 0x58), arr.data(), arr.size())) return Skip::Unreadable;
                store(arr.data(), arr.size());
                continue;
            }
            if (n == 0) {
                if (paramClassLogged_++ < 4) spdlog::info("native: effect draws: parameter '{}' of class vtable RVA {:#x}: not composed", param.name, vt);
                return Skip::ParamClass;
            }
            std::uint8_t buf[64];
            const std::size_t bytes = n < 0 ? 64u : static_cast<std::size_t>(n);
            if (ov != nullptr && !ov->isImage && ov->bytes.size() == bytes) std::memcpy(buf, ov->bytes.data(), bytes);
            else if (!safeRead(obj + 0x58, buf, bytes)) return Skip::Unreadable;
            if (n < 0) {
                float m[16], t[16];
                std::memcpy(m, buf, 64);
                fxc::transpose(m, t);
                std::memcpy(buf, t, 64);
            }
            store(buf, bytes);
        }
    }
    for (std::int32_t k = 0; k < inputs; ++k) {
        U8* entry = inputArray + 0x60 * static_cast<std::size_t>(k);
        const std::uint32_t pi = info->parameterOf(ptr(entry, 0x20));
        if (pi == fx::kNone || pi >= e.parameters.size()) continue;  // AttachImmImageData without a handle: nothing
        const void* img = nullptr;
        if (!inputImage(k, img)) return Skip::Input;
        paramTextures[pi] = img != nullptr ? textureOfImage(img) : nullptr;
    }
    // the visual (+0xb0) and its quad
    U8* visual = ptr(filter, 0xb0);
    const auto* prim = hal::vulkan::HalPrimitive::fromApi(ptr(visual, 0x10));
    if (visual == nullptr || prim == nullptr || ptr(visual, 0x60) != nullptr) return Skip::Visual;
    std::vector<const void*> shaders(e.shaders.size(), nullptr);
    for (std::uint32_t s = 0; s < e.shaders.size(); ++s) shaders[s] = info->fxShader(s);
    // the device at the call (the target bound by the filter's SetRenderTarget, the other states as they are), then per pass
    if (draws_.empty()) draws_.push_back(std::make_unique<fxc::EffectDraw>());
    fxc::EffectDraw& base = *draws_[0];
    captureDevice(base);
    if (post) {
        // eCImageFilterRTBase::SetRenderTarget 0x668370: a screen-mode filter (+0x38 == 1, no output image) draws into what is
        // bound (the target / depth / viewport at Execute's entry: the back buffer); a texture-mode one binds its output image with
        // the saved depth (the one bound at Execute's entry: each job's ResetRenderTarget restores it) and (0, 0, +0x80, +0x84, 0, 1)
        if (get<std::int32_t>(filter, 0x38) == 1 || ptr(filter, 0x70) == nullptr) {
            base.rt0 = postRt_;
            base.depth = postDepthSurface_;
            base.viewport = postViewport_;
        } else {
            d3d9::Surface* t = surfaceOfImage(ptr(filter, 0x70));
            if (t == nullptr) return Skip::Image;
            base.rt0 = t;
            base.depth = postDepthSurface_;
            base.viewport = {0, 0, get<std::uint32_t>(filter, 0x80), get<std::uint32_t>(filter, 0x84), 0.0f, one_};
        }
    } else if (screenTarget) {
        // the target the caller bound (D3D9 resets the viewport to it), no depth
        d3d9::Surface* t = surfaceOfImage(target);
        if (t == nullptr) return Skip::Image;
        base.rt0 = t;
        base.depth = nullptr;
        base.viewport = {0, 0, t->width(), t->height(), 0.0f, 1.0f};
    } else {
        // eCImageFilterRTBase::SetRenderTarget 0x668370: the output image's surface, the saved depth, viewport (0, 0, +0x80, +0x84, 0, 1)
        d3d9::Surface* t = surfaceOfImage(ptr(filter, 0x70));
        if (t == nullptr) return Skip::Image;
        base.rt0 = t;
        base.depth = maskDepth_;
        base.viewport = {0, 0, get<std::uint32_t>(filter, 0x80), get<std::uint32_t>(filter, 0x84), 0.0f, one_};
    }
    const fxc::Masters masters = hal_.visualMasters();
    const auto& passes = e.techniques[tech].passes;
    const auto& dc = prim->drawDesc();
    for (std::size_t p = 0; p < passes.size(); ++p) {
        if (draws_.size() <= p) draws_.push_back(std::make_unique<fxc::EffectDraw>());
        fxc::EffectDraw& d = *draws_[p];
        if (p > 0) d = *draws_[p - 1];
        // eCGfxEffect::Execute (ExecuteEffectPass): skipped for the effect already active and not dirty while ms_bOptimize
        if (!post || !executeSkipped(ptr(filter, 0xa8))) fxc::applyPass(e, passes[p], values, paramTextures, shaders, d);
        fxc::applyVisual(d, get<std::uint32_t>(visual, 0x18), get<std::uint32_t>(visual, 0x1c), get<std::uint32_t>(visual, 0x20),
                         get<std::uint32_t>(visual, 0x24), masters);
        // eCAPIPrimitive::Execute (HalPrimitive): enabled streams with data bound (their frequency set), the others unbound
        d.decl = prim->vertexDecl();
        for (std::uint32_t i = 0; i < 4; ++i) {
            const auto v = prim->streamView(i);
            if (v.buffer == nullptr || !v.enabled) d.streams[i] = {nullptr, 0, 0, d.streams[i].freq};
            else d.streams[i] = {v.buffer, v.offset, v.stride, v.freq};
        }
        d.indices = prim->indexBuffer();
        d.indexed = dc.indexed;
        d.primType = static_cast<std::uint32_t>(dc.type);
        d.primCount = dc.primitiveCount;
        if (dc.indexed) {
            d.baseVertex = dc.baseVertexIndex;
            d.minIndex = dc.minVertexIndex;
            d.numVertices = dc.numVertices;
            d.startIndex = dc.startIndex;
            d.startVertex = 0;
        } else {
            d.startVertex = dc.startVertex;
            d.baseVertex = 0;
            d.minIndex = d.numVertices = d.startIndex = 0;
        }
        if (d.unsupported) return Skip::Technique;
        drawCount_ = p + 1;
    }
    return Skip::None;
}

void EffectOps::issue() {
    if (drawCount_ == 0) return;
    captureDevice(*saved_);
    d3d9::draws::Tag& tag = dev_.drawTag();
    const d3d9::draws::Tag savedTag = tag;
    issuing_ = true;
    for (std::size_t k = 0; k < drawCount_; ++k) {
        const fxc::EffectDraw& d = *draws_[k];
        applyDevice(d);
        issueIndex_ = k;
        if (d.indexed) dev_.drawIndexedPrimitive(d.primType, d.baseVertex, d.minIndex, d.numVertices, d.startIndex, d.primCount);
        else dev_.drawPrimitive(d.primType, d.startVertex, d.primCount);
        ++stats_->issuedDraws;
    }
    issuing_ = false;
    tag = savedTag;
    applyDevice(*saved_);
}

void EffectOps::compareDraw(const fxc::EffectDraw& composed, const d3d9::DrawTapInfo& t, bool own) {
    fxc::Live live;
    capture(t, *scratch_, live);
    fxc::Diff diff = fxc::compare(composed, *scratch_, live);
    if (unit_ == Unit::Gui) {
        // N5.7: the sprite quads' vertex / index bytes against the buffers the draw reads
        std::string why;
        if (!spriteGeometryEqual(own ? issueIndex_ : engineDraws_ - 1, t, own, why)) {
            diff.fields.set(static_cast<std::size_t>(fxc::Field::Geometry));
            if (diff.first.empty()) diff.first = why;
        }
    }
    if (own) {
        ++stats_->ownChecked;
        if (diff.fields.any()) ++stats_->ownMismatch;
    } else {
        stats_->counts.add(diff);
    }
    if (diff.fields.any() && (mismatchLogged_ < 24 || hal::vulkan::logThisFrame())) {
        ++mismatchLogged_;
        spdlog::info("native: effect draws: mismatch{}: {} draw {}: {}", own ? " (own draw vs its composition)" : "", unitName_,
                     own ? issueIndex_ : engineDraws_ - 1, diff.first);
    } else if (hal::vulkan::logThisFrame() && !own) {
        spdlog::info("native: effect draws: {} draw {}: equal ({} registers, {} stages, {} states)", unitName_, engineDraws_ - 1, diff.regs, diff.stages,
                     diff.states);
    }
}

void EffectOps::ownDraw(const d3d9::DrawTapInfo& t) {
    if (stats_ == nullptr || issueIndex_ >= drawCount_) return;
    if (ownKind_.size() < drawCount_) ownKind_.resize(drawCount_);
    ownKind_[issueIndex_] = static_cast<std::uint8_t>(t.kind);
    if (unitCompare_) compareDraw(*draws_[issueIndex_], t, true);
}

bool EffectOps::engineDraw(const d3d9::DrawTapInfo& t) {
    if (unit_ == Unit::None || unit_ == Unit::Copy) {
        if (postDepth_ > 0 && !engine::scene::inSceneJob() && t.phase.view == d3d9::scene::View::Post) ++postDrawsOutside_;
        if (guiOn() && t.phase.view == d3d9::scene::View::Gui) ++guiOutside_;
        if (inMask_) {
            ++outsideDraws_;
            if (outsideDraws_ < 4) spdlog::info("native: effect draws: an engine draw inside UpdateShadowMask outside the composed units (vs {:016x} ps {:016x})", t.vsHash, t.psHash);
        }
        return false;
    }
    const std::uint32_t k = engineDraws_++;
    if (unitCompare_ && k < drawCount_) {
        compareDraw(*draws_[k], t, false);
        if (taken_ && k < ownKind_.size() && ownKind_[k] != static_cast<std::uint8_t>(t.kind)) {
            ++stats_->counts.mismatches[static_cast<std::size_t>(fxc::Field::DrawCount)];
            if (mismatchLogged_++ < 24) spdlog::info("native: effect draws: {} draw {}: own draw kind {} vs engine {}", unitName_, k, ownKind_[k], static_cast<int>(t.kind));
        }
    }
    if (taken_) ++stats_->droppedDraws;
    return taken_;
}

void EffectOps::finishUnit() {
    if (unit_ != Unit::Copy && stats_ != nullptr && unitCompare_ && engineDraws_ != drawCount_) {
        ++stats_->counts.mismatches[static_cast<std::size_t>(fxc::Field::DrawCount)];
        if (mismatchLogged_++ < 24) spdlog::info("native: effect draws: {}: {} engine draws, {} composed", unitName_, engineDraws_, drawCount_);
    }
    unit_ = Unit::None;
    unitFilter_ = nullptr;
    drawCount_ = 0;
    taken_ = false;
}

// ---- the 1/4 depth copy (eCImageFilterCopy::OnUpdateOutput 0x665640) -------------------------------------------------------------

void EffectOps::copyBegin(void* filterPtr) {
    if (postDepth_ > 0 && !engine::scene::inSceneJob() && dev_.phases().view() == d3d9::scene::View::Post) {
        // N5.7: an eCImageFilterCopy of a post graph (Indicator, OverlayHit): its HAL copy stays the engine's
        if (postCopies_++ == 0) spdlog::info("native: post: a copy filter {} in the post chain (left to the engine)", filterPtr);
        return;
    }
    if (verified_ <= 0 || !maskOn() || !inMask_ || illum_ == nullptr || filterPtr != ptr(illum_, kIllumDepthCopy)) return;
    if (unit_ != Unit::None) finishUnit();
    unit_ = Unit::Copy;
    unitFilter_ = filterPtr;
    unitName_ = "copy";
    stats_ = &window_[unitName_];
    ++stats_->counts.units;
    copyCalls_ = 0;
    copy_ = {};
}

void EffectOps::copyEnd(void* filterPtr) {
    if (unit_ != Unit::Copy || filterPtr != unitFilter_) return;
    if (compare_ && copyCalls_ != 1) {
        ++stats_->counts.mismatches[static_cast<std::size_t>(fxc::Field::DrawCount)];
        if (mismatchLogged_++ < 24) spdlog::info("native: effect draws: copy: {} copies", copyCalls_);
    }
    unit_ = Unit::None;
    unitFilter_ = nullptr;
}

bool EffectOps::copyCall(d3d9::Texture* src, const std::int32_t* srcRect, const std::int32_t* dstRect, std::uint32_t filter) {
    if (unit_ != Unit::Copy) return false;
    ++copyCalls_;
    // composed: the input's output image (the depth map filter +0x460), into the own output +0x70's surface at (0, 0, +0x80, +0x84),
    // the whole source, POINT, with the target bound (no depth) and the viewport (0, 0, w, h, 0, 1)
    auto* f = static_cast<U8*>(unitFilter_);
    const void* img = nullptr;
    const bool known = outputImage(ptr(f, 0x98), get<std::int32_t>(f, 0xa8), img);
    copy_.src = img != nullptr ? textureOfImage(img) : nullptr;
    copy_.dst = surfaceOfImage(ptr(f, 0x70));
    const std::int32_t w = get<std::int32_t>(f, 0x80), h = get<std::int32_t>(f, 0x84);
    copy_.dstRect[0] = 0;
    copy_.dstRect[1] = 0;
    copy_.dstRect[2] = w;
    copy_.dstRect[3] = h;
    copy_.filter = 1;
    copy_.viewport = {0, 0, static_cast<std::uint32_t>(w), static_cast<std::uint32_t>(h), 0.0f, one_};
    copy_.valid = known && copy_.src != nullptr && copy_.dst != nullptr;
    if (!copy_.valid) {
        ++stats_->skips[static_cast<std::size_t>(Skip::Image)];
        return false;
    }
    ++stats_->counts.unitsComposed;
    if (compare_) {
        fxc::Diff d;
        const auto miss = [&](fxc::Field fld, std::string what) {
            if (d.first.empty()) d.first = std::move(what);
            d.fields.set(static_cast<std::size_t>(fld));
        };
        if (src != copy_.src) miss(fxc::Field::Texture, std::format("source {} vs engine {}", static_cast<const void*>(copy_.src), static_cast<const void*>(src)));
        if (dev_.renderTarget(0) != copy_.dst || dev_.depthStencil() != nullptr) miss(fxc::Field::Target, "target / depth");
        if (srcRect != nullptr) miss(fxc::Field::Range, "source rect");
        if (dstRect == nullptr || std::memcmp(dstRect, copy_.dstRect, 16) != 0) miss(fxc::Field::Range, "destination rect");
        if (filter != copy_.filter) miss(fxc::Field::SamplerState, std::format("filter {} vs engine {}", copy_.filter, filter));
        const d3d9::Viewport& v = dev_.viewport();
        if (!(fxc::Viewport{v.x, v.y, v.width, v.height, v.minZ, v.maxZ} == copy_.viewport)) miss(fxc::Field::Viewport, "viewport");
        stats_->counts.add(d);
        if (d.fields.any() && mismatchLogged_++ < 24) spdlog::info("native: effect draws: copy mismatch: {}", d.first);
    }
    if (!takeOver_) return false;
    // take-over: our own copy instead of the engine's (POINT = not linear)
    dev_.stretchRect(copy_.src->surface(0, 0), nullptr, copy_.dst, copy_.dstRect, false);
    ++stats_->issuedDraws;
    ++stats_->droppedDraws;
    return true;
}

// ---- report ---------------------------------------------------------------------------------------------------------------------

void EffectOps::report() {
    reportSetup();
    if (!maskOn() && !postOn() && !guiOn()) return;
    if (verified_ < 0) {
        spdlog::info("native: effect draws: idle (the engine checks failed)");
        return;
    }
    reportPost();
    reportGui();
    outsideDrawsTotal_ += outsideDraws_;
    if (maskOn()) spdlog::info("native: effect draws (5 s): {} shadow masks ({} with point-light cells), engine draws outside the units {} (total {}), "
                 "parameters not used by their technique {}, the mask's parameter values composed natively {}",
                 masks_, packedMasks_, outsideDraws_, outsideDrawsTotal_, unusedParams_, nativeParams_);
    masks_ = packedMasks_ = outsideDraws_ = nativeParams_ = 0;
    for (auto& [name, w] : window_) {
        Stats& t = total_[name];
        t.add(w);
        std::string skips;
        for (std::size_t i = 1; i < t.skips.size(); ++i) {
            if (t.skips[i] != 0) skips += std::format("{}{} {}", skips.empty() ? "" : ", ", EffectOps::name(static_cast<Skip>(i)), t.skips[i]);
        }
        spdlog::info("native: effect draws: {}: 5 s {}; total {}{}{}", name, fxc::summary(w.counts), fxc::summary(t.counts),
                     skips.empty() ? "" : "; left to the engine: " + skips,
                     t.issuedDraws != 0 ? std::format("; drawn by the renderer {}, engine draws dropped {}, own draws checked {}, differing {}", t.issuedDraws,
                                             t.droppedDraws, t.ownChecked, t.ownMismatch)
                               : std::string());
        w = {};
    }
}

}  // namespace rvk::native
