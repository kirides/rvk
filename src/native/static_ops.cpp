#include "native/static_ops.h"

#include <windows.h>

#include <algorithm>
#include <cmath>
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

using engine::readAt;
using engine::safeRead;
using U8 = const std::uint8_t;

// ---- Engine.dll facts this module relies on (RVAs; C-materials.md, engine-frame/README.md 7) ----------------------------
constexpr std::uint32_t kShaderDefaultVtable = 0x97bdb0;    // ??_7eCShaderDefault@@6B@
constexpr std::uint32_t kVersionDefault3Vtable = 0x9d7e40;  // eCShaderVersionDefault_3_0 (unexported)
// eCShaderDefault vtable slots and the functions whose logic static_compose reproduces
constexpr std::pair<std::uint32_t, std::uint32_t> kShaderSlots[] = {
    {0xf0, 0x6d2840},   // SetSwitch (eCShaderBase: the switch elements in reverse order)
    {0x190, 0x6c9d00},  // HasOpacity (+0x330)
    {0x198, 0x6d2ef0},  // IsBlended
    {0x1a0, 0x6d2f50},  // IsMasked (+0x50 == 1)
    {0x1a8, 0x6c9d20},  // IsRefracted (+0x420)
    {0x1b8, 0xad0d0},   // GetGlobalShaderConstants (ms_pGlobalShaderConstants)
    {0x1c0, 0x6d2f90},  // ExecuteShaderConstants
    {0x1c8, 0x6c9d50},  // OnGetPasses (-> eCShaderBase::OnGetPasses 0x6d3170)
    {0x1d0, 0x6d2060},  // ExecutePass
    {0x1d8, 0x6d1cd0},  // ExecuteSpecialPass
    {0x200, 0x6c9df0},  // GetMaxInstanceCount (50)
    {0x208, 0x6c9e00},  // SetWorldMatrix (global VS pool c33 + 4i)
    {0x210, 0x6c9e30},  // SetGlobalAlpha (c32 + 4i .w)
    {0x218, 0x6c9e80},  // GetGlobalAlpha
};
constexpr std::pair<std::uint32_t, std::uint32_t> kVersionSlots[] = {
    {0x18, 0x6cc970}, {0x20, 0x6cc610}, {0x28, 0x6d9b40}, {0x58, 0x6cce70}, {0x60, 0x6d9a70},
};
// switch elements: their SetSwitch (vtable +0xf0)
constexpr std::uint32_t kConstantSwitchSetSwitch = 0x6c0c60;  // eCColorSrcConstantSwitch
constexpr std::uint32_t kSamplerSetSwitch = 0x6c5310;         // eCColorSrcSampler
constexpr std::uint32_t kElementSetSwitchNoop = 0x6cdfc0;     // eCShaderEllementBase::SetSwitch (ret)

// code sites whose operands give unexported data / functions (bytes checked before use)
struct Site {
    std::uint32_t rva;
    std::array<std::uint8_t, 8> bytes;
    std::uint32_t length;
};
// FUN_1806cd1d0 +0xf: mov rdi, [rip + rel32] (the lighting pool DAT_180c62968)
constexpr Site kLightingSite{0x6cd1df, {0x48, 0x8b, 0x3d, 0, 0, 0, 0, 0}, 3};
// eCImageResource2::GetPlaceHolderImageData +0x19: lea rcx, [rip + rel32] (the per-stage placeholder images)
constexpr Site kPlaceholderSite{0x68c669, {0x48, 0x8d, 0x0d, 0, 0, 0, 0, 0}, 3};
// eCGfxImageData::Execute +0x25: mov rax, [rdi + rcx*8 + disp32] (the per-stage override images; rdi = image base)
constexpr Site kOverrideSite{0x7551e5, {0x48, 0x8b, 0x84, 0xcf, 0, 0, 0, 0}, 4};
// eCRenderContextMesh::Render +0x52: lea r9, [rip + rel32] (SortRenderInstances 0x75f690) ... call qsort 0x4a4aa0
constexpr std::uint32_t kInstanceSortLea = 0x5c7162, kInstanceCmp = 0x75f690, kQsort = 0x4a4aa0;

constexpr std::uint32_t kNoState = 0xABABABABu;  // a sampler slot value meaning "the gfx admin's default filter"

bool rd(const void* p, std::size_t off, void* out, std::size_t n) {
    return p != nullptr && safeRead(static_cast<U8*>(p) + off, out, n);
}
template <class T>
T get(const void* p, std::size_t off, T fallback = T{}) {
    T v = fallback;
    if (!readAt(p, off, v)) return fallback;
    return v;
}
U8* ptr(const void* p, std::size_t off) { return get<U8*>(p, off, nullptr); }

// a register k of FUN_180225e30 / FUN_180238390: column k of the row-vector matrix
void column(const float m[16], std::uint32_t k, float out[4]) {
    out[0] = m[k];
    out[1] = m[4 + k];
    out[2] = m[8 + k];
    out[3] = m[12 + k];
}

void mul(const float a[16], const float b[16], float out[16]) {
    for (int r = 0; r < 4; ++r) {
        for (int c = 0; c < 4; ++c) {
            out[r * 4 + c] = a[r * 4 + 0] * b[0 * 4 + c] + a[r * 4 + 1] * b[1 * 4 + c] + a[r * 4 + 2] * b[2 * 4 + c] + a[r * 4 + 3] * b[3 * 4 + c];
        }
    }
}

bool invert(const float m[16], float out[16]) {
    double a[4][8];
    for (int r = 0; r < 4; ++r) {
        for (int c = 0; c < 4; ++c) {
            a[r][c] = m[r * 4 + c];
            a[r][4 + c] = r == c ? 1.0 : 0.0;
        }
    }
    for (int c = 0; c < 4; ++c) {
        int p = c;
        for (int r = c + 1; r < 4; ++r) {
            if (std::abs(a[r][c]) > std::abs(a[p][c])) p = r;
        }
        if (std::abs(a[p][c]) < 1e-30) return false;
        if (p != c) {
            for (int k = 0; k < 8; ++k) std::swap(a[p][k], a[c][k]);
        }
        const double inv = 1.0 / a[c][c];
        for (int k = 0; k < 8; ++k) a[c][k] *= inv;
        for (int r = 0; r < 4; ++r) {
            if (r == c) continue;
            const double f = a[r][c];
            for (int k = 0; k < 8; ++k) a[r][k] -= f * a[c][k];
        }
    }
    for (int r = 0; r < 4; ++r) {
        for (int c = 0; c < 4; ++c) out[r * 4 + c] = static_cast<float>(a[r][4 + c]);
    }
    return true;
}

bool bitsEqual(const void* a, const void* b, std::size_t n) { return std::memcmp(a, b, n) == 0; }

}  // namespace

const char* StaticOps::name(Skip s) {
    static constexpr const char* kNames[] = {"none", "kind", "overlay passes", "shader class", "shader version", "invalid shader",
                                             "permutation not compiled", "switch element", "primitive", "image override", "unreadable",
                                             "pass kind", "instance sort", "texture"};
    const auto i = static_cast<std::size_t>(s);
    return i < std::size(kNames) ? kNames[i] : "?";
}

StaticOps::StaticOps(hal::vulkan::HalDevice& hal) : hal_(hal), dev_(hal.dev()), scratch_(std::make_unique<sc::DrawDesc>()) {}

StaticOps::~StaticOps() = default;

void StaticOps::setMode(bool takeOver, bool compare) {
    if (takeOver == takeOver_ && compare == compare_) return;
    takeOver_ = takeOver;
    compare_ = compare;
    window_ = {};
    skips_ = {};
    spdlog::info("native: static ops: {} (the main view's static mesh z / solid ops{}{})", active() ? "on" : "off",
                 takeOver_ ? ", drawn by the renderer" : "", compare_ ? ", compared with the engine's draws" : "");
}

bool StaticOps::verify() {
    engine_ = reinterpret_cast<U8*>(GetModuleHandleW(L"Engine.dll"));
    if (engine_ == nullptr) return false;
    const auto slotsOk = [&](std::uint32_t vtable, auto& slots, const char* what) {
        for (const auto& [off, fn] : slots) {
            const void* v = nullptr;
            if (!readAt(engine_ + vtable, off, v) || v != engine_ + fn) {
                spdlog::warn("native: static ops: {} vtable +{:#x} is not RVA {:#x}: static ops stay off", what, off, fn);
                return false;
            }
        }
        return true;
    };
    if (engine::sym().shaderDefaultVftable != engine_ + kShaderDefaultVtable) {
        spdlog::warn("native: static ops: eCShaderDefault's vtable is not at RVA {:#x}: static ops stay off", kShaderDefaultVtable);
        return false;
    }
    if (!slotsOk(kShaderDefaultVtable, kShaderSlots, "eCShaderDefault") ||
        !slotsOk(kVersionDefault3Vtable, kVersionSlots, "eCShaderVersionDefault_3_0")) {
        return false;
    }
    const auto site = [&](const Site& s, std::int64_t& target) {
        std::array<std::uint8_t, 8> b{};
        if (!safeRead(engine_ + s.rva, b.data(), s.length + 4)) return false;
        if (!std::equal(b.begin(), b.begin() + s.length, s.bytes.begin())) return false;
        std::int32_t v = 0;
        std::memcpy(&v, b.data() + s.length, 4);
        target = v;
        return true;
    };
    std::int64_t v = 0;
    if (!site(kLightingSite, v)) return false;
    lightingPoolVar_ = engine_ + kLightingSite.rva + 7 + v;
    if (!site(kPlaceholderSite, v)) return false;
    placeholderTable_ = engine_ + kPlaceholderSite.rva + 7 + v;
    if (!site(kOverrideSite, v)) return false;
    imageOverride_ = engine_ + v;  // rdi = the image base
    // the instance sort of eCRenderContextMesh::Render: lea r9, [rip + cmp]; mov rdx, rax; mov r8d, 0x58; call qsort
    std::array<std::uint8_t, 21> b{};
    if (!safeRead(engine_ + kInstanceSortLea, b.data(), b.size()) || b[0] != 0x4c || b[1] != 0x8d || b[2] != 0x0d || b[16] != 0xe8) return false;
    std::int32_t rel = 0;
    std::memcpy(&rel, &b[3], 4);
    std::int32_t call = 0;
    std::memcpy(&call, &b[17], 4);
    if (engine_ + kInstanceSortLea + 7 + rel != engine_ + kInstanceCmp || engine_ + kInstanceSortLea + 16 + 5 + call != engine_ + kQsort) return false;
    instanceCmp_ = reinterpret_cast<int (*)(const void*, const void*)>(const_cast<std::uint8_t*>(engine_ + kInstanceCmp));
    qsort_ = reinterpret_cast<QsortFn>(const_cast<std::uint8_t*>(engine_ + kQsort));
    if (engine::sym().shaderDefaultGlobalConstants == nullptr || engine::sym().appInstancePtr == nullptr) return false;
    spdlog::info("native: static ops: engine checks passed (shader / version vtables, lighting pool {:#x}, placeholders {:#x}, image "
                 "overrides {:#x}, instance sort)",
                 lightingPoolVar_ - engine_, placeholderTable_ - engine_, imageOverride_ - engine_);
    return true;
}

// ---- per view ---------------------------------------------------------------------------------------------------------------

void StaticOps::viewBegin(void* db, void* desc) {
    if (!hal::vulkan::logThisFrame()) frameLogged_ = 0;
    db_ = db;
    desc_ = desc;
    inView_ = true;
    view_ = {};
    op_ = nullptr;
}

void StaticOps::viewEnd() {
    finishOp();
    inView_ = false;
}

bool StaticOps::composeView(const std::uint8_t* admin) {
    view_ = {};
    view_.admin = admin;
    view_.gfx = ptr(admin, 0x38);
    U8* base = ptr(db_, 0x14c0);
    U8* cam = ptr(base, 0x1738);
    if (cam == nullptr) return false;
    float view[16], proj[16], vp[16], db[16], invView[16];
    if (!rd(cam, 0x20, view, 64) || !rd(cam, 0x60, proj, 64) || !rd(cam, 0xa0, db, 64) || !rd(cam, 0xe0, vp, 64) ||
        !rd(cam, 0x120, invView, 64) || !rd(cam, 0x150, view_.camPos, 12)) {
        return false;
    }
    for (std::uint32_t k = 0; k < 4; ++k) column(vp, k, view_.vp[k]);
    // admin +0x1c4 = camera +0xa0 *= camera +0x20 (bCMatrix::operator*=): the order is checked against the admin's copy
    float pv[16], vpb[16], adminDb[16];
    mul(db, view, pv);
    mul(view, db, vpb);
    const float* chosen = pv;
    if (view_.admin != nullptr && rd(view_.admin, 0x1c4, adminDb, 64)) {
        view_.dbOrder = bitsEqual(pv, adminDb, 64) ? 0 : bitsEqual(vpb, adminDb, 64) ? 1 : -1;
        if (view_.dbOrder == 1) chosen = vpb;
    }
    for (std::uint32_t k = 0; k < 4; ++k) column(chosen, k, view_.dbvp[k]);
    float inv[16];
    if (invert(proj, inv)) {
        for (std::uint32_t k = 0; k < 4; ++k) column(inv, k, view_.invProj[k]);
    }
    // FUN_180225ec0: c8 = (m0, m4, m8), c9 = (m1, m5, m9), c10 = (m2, m6, m10); .w untouched
    for (std::uint32_t k = 0; k < 3; ++k) {
        view_.invView[k][0] = invView[k];
        view_.invView[k][1] = invView[4 + k];
        view_.invView[k][2] = invView[8 + k];
    }
    // the application camera: eCApplication +0x38 (else +0xc8): far +0xc0, camera obstacle distances +0xe4
    U8* app = ptr(engine::sym().appInstancePtr, 0);
    U8* appCam = ptr(app, 0x38);
    if (appCam == nullptr) appCam = ptr(app, 0xc8);
    if (appCam == nullptr) return false;
    view_.camPos[3] = get<float>(appCam, 0xc0);
    sc::camObstacle(get<float>(appCam, 0xe4), view_.obstacleX, view_.obstacleY);
    // eCGfxFog::Execute(db +0x14c8), unless eCGfxFog::ms_pMasterFog is set
    U8* fog = engine::sym().gfxMasterFog != nullptr ? ptr(engine::sym().gfxMasterFog, 0) : nullptr;
    if (fog == nullptr) fog = ptr(db_, 0x14c8);
    if (fog != nullptr) {
        sc::fogConstants(get<std::uint8_t>(fog, 0x18) == 1, get<std::uint32_t>(fog, 0x1c), get<float>(fog, 0x28), get<float>(fog, 0x2c),
                         get<float>(fog, 0x30), view_.fogC3, view_.fogC4);
    }
    // BeginZPass(NULL, true, h != 0): colour writes when the desc's depth image has an API texture (its render target)
    U8* depthImage = ptr(desc_, 8);
    view_.zColorWrite = depthImage != nullptr && ptr(depthImage, 0x60) != nullptr;
    view_.valid = true;
    return true;
}

// ---- helpers ----------------------------------------------------------------------------------------------------------------

bool StaticOps::shaderClassOk(const void* shader) { return get<const void*>(shader, 0) == engine_ + kShaderDefaultVtable; }

const StaticOps::Live& StaticOps::liveOf(const d3d9::Shader* s) {
    auto [it, inserted] = live_.try_emplace(s);
    if (inserted && s != nullptr) {
        const auto r = fx::parseConstantTable(s->bytecode);
        if (r.ok()) {
            for (const fx::Constant& c : r.table.constants) {
                if (c.registerSet == fx::RegisterSet::Float4) {
                    for (std::uint32_t i = 0; i < c.registerCount && c.registerIndex + i < sc::kVsRegs; ++i) it->second.f.set(c.registerIndex + i);
                } else if (c.registerSet != fx::RegisterSet::Sampler) {
                    it->second.other = true;
                }
            }
        }
    }
    return it->second;
}

d3d9::Shader* StaticOps::shaderOfElement(const void* element) {
    if (element == nullptr) return nullptr;
    if (const auto it = elementShader_.find(element); it != elementShader_.end()) return it->second;
    d3d9::Shader* s = hal::vulkan::HalHLSLShader::shaderOf(const_cast<std::uint8_t*>(ptr(ptr(element, 0x18), 0x18)));
    if (s != nullptr) elementShader_[element] = s;  // pool elements live as long as the pool (the cache is never wrong)
    return s;
}

d3d9::Texture* StaticOps::textureOfImageData(const void* imageData, bool& ok) {
    ok = false;
    U8* api = ptr(imageData, 0x60);
    if (api == nullptr) return nullptr;
    d3d9::Texture* t = hal_.textureOfApi(api);
    if (t == nullptr) t = hal_.cubeOfApi(api);
    ok = t != nullptr;
    return t;
}

sc::DrawDesc& StaticOps::newDraw() {
    if (drawCount_ == draws_.size()) draws_.push_back(std::make_unique<sc::DrawDesc>());
    sc::DrawDesc& d = *draws_[drawCount_++];
    d.vs = d.ps = d.decl = d.indices = d.subMesh = d.material = nullptr;
    d.streams = {};
    d.samplers = {};
    d.samplerMask = d.inheritMask = 0;
    d.vsLive.reset();
    d.psLive.reset();
    d.vsSrc.fill(sc::Src::None);
    d.psSrc.fill(sc::Src::None);
    d.passFlags = 0;
    d.zPass = false;
    return d;
}

StaticOps::Mirror::ShaderPools* StaticOps::poolsOf(const void* shader) {
    auto [it, inserted] = mirror_.shaders.try_emplace(shader);
    Mirror::ShaderPools& p = it->second;
    if (inserted) {
        p.vsFirst = get<std::uint32_t>(shader, 0x9c, 232);
        p.psFirst = get<std::uint32_t>(shader, 0xcc, 6);
        p.vsCount = std::min<std::uint32_t>(get<std::uint8_t>(shader, 0xc1), 32);
        p.psCount = std::min<std::uint32_t>(get<std::uint8_t>(shader, 0xf1), 16);
        if ((p.vsCount != 0 && !rd(ptr(shader, 0xb0), 0, p.vs, p.vsCount * 16)) || (p.psCount != 0 && !rd(ptr(shader, 0xe0), 0, p.ps, p.psCount * 16))) {
            p.vsCount = p.psCount = 0;
        }
        p.slots = std::min<std::uint32_t>(get<std::uint8_t>(shader, 0x21b), 16);
        for (std::uint32_t k = 0; k < p.slots; ++k) p.slotImage[k] = ptr(shader, 0xf8 + 0x28 * k + 0x20);
    }
    return &p;
}

void StaticOps::initMirror() {
    mirror_.shaders.clear();
    U8* admin = view_.admin;
    rd(ptr(admin, 0x240), 0, mirror_.adminVs, sizeof(mirror_.adminVs));
    rd(ptr(admin, 0x220), 0, mirror_.adminPs, sizeof(mirror_.adminPs));
    U8* global = ptr(engine::sym().shaderDefaultGlobalConstants, 0);
    rd(ptr(global, 0x38), 0, mirror_.global, sizeof(mirror_.global));
    U8* lighting = ptr(lightingPoolVar_, 0);
    rd(ptr(lighting, 0x18), 0, mirror_.lighting, sizeof(mirror_.lighting));
    mirror_.lightmapSlot = {ptr(admin, 0x2e0), ptr(admin, 0x308)};
    // the per-view constants, composed natively (the admin pools hold the engine's copies)
    for (std::uint32_t k = 0; k < 4; ++k) std::memcpy(mirror_.adminVs[4 + k], view_.invProj[k], 16);
    for (std::uint32_t k = 0; k < 3; ++k) std::memcpy(mirror_.adminVs[8 + k], view_.invView[k], 12);
    std::memcpy(mirror_.adminVs[11], view_.camPos, 16);
    std::memcpy(mirror_.adminVs[30], view_.fogC4, 16);
    std::memcpy(mirror_.adminPs[0], view_.camPos, 16);
    std::int32_t w = 0, h = 0;
    {
        const d3d9::Viewport& vp = dev_.viewport();
        w = static_cast<std::int32_t>(vp.width);
        h = static_cast<std::int32_t>(vp.height);
    }
    sc::viewToScreen(w, h, mirror_.adminPs[1]);
    std::memcpy(mirror_.adminPs[3], view_.fogC3, 16);
    std::memcpy(mirror_.adminPs[4], view_.fogC4, 16);
    mirror_.adminPs[5][0] = view_.obstacleX;
    mirror_.adminPs[5][1] = view_.obstacleY;
}

void StaticOps::writeWorld(std::uint32_t slot, const float* m) {
    const std::uint32_t r = 33 + 4 * slot - 31;
    if (r + 3 > 201) return;
    for (std::uint32_t k = 0; k < 3; ++k) column(m, k, mirror_.global[r + k]);
}

void StaticOps::writeAlpha(std::uint32_t slot, float a) {
    const std::uint32_t r = 32 + 4 * slot - 31;
    if (r < 201) mirror_.global[r][3] = a;
}

// FUN_1806cd1d0: the pass's lighting into the lighting pool (c14 .. c37)
void StaticOps::writeLighting(const sc::Pass& pass) {
    U8* illum = static_cast<U8*>(pass.illum);
    U8* list = ptr(illum, 0);
    if (illum == nullptr || list == nullptr) return;
    float (*L)[4] = mirror_.lighting;  // L[r - 14]
    rd(list, 0x128, L[0], 12);
    float sh[7][4]{}, staticSh[7][4]{}, indoor[3]{};
    rd(list, 0xb8, sh, sizeof(sh));
    rd(list, 0x128, indoor, 12);
    const bool hasStatic = get<std::uint8_t>(illum, 0x90) != 0;
    if (hasStatic) rd(illum, 0x94, staticSh, sizeof(staticSh));
    sc::ambientSh(sh, indoor, get<float>(illum, 8), hasStatic ? &staticSh[0][0] : nullptr, &L[1]);
    rd(list, 4, L[8], 48);
    const std::uint32_t n = sc::passLights(pass.flags);
    const std::uint32_t nStatic = get<std::uint8_t>(illum, 0x0d);
    for (std::uint32_t j = 0; j < n; ++j) {
        const std::uint32_t idx = pass.firstLight + j;
        U8* src = idx < nStatic ? illum + 0x10 + 0x40 * idx : list + 0x38 + 0x40 * (idx - nStatic);
        if (11 + 4 * j + 4 <= 24) rd(src, 0, L[11 + 4 * j], 64);
    }
}

// eCMeshResource2::BindLightmaps 0x764220 (the admin's s4 / s5 slots updated as SetLightmap does)
std::uint32_t StaticOps::bindLightmaps(const std::uint8_t* res) {
    std::uint32_t combi = 0;
    for (std::uint32_t i = 0; i < 2; ++i) {
        U8* lm = ptr(res, 0x90 + 8 * i);
        if (lm == nullptr || get<std::int32_t>(lm, 0x38) == 0) continue;
        U8* data = ptr(lm, 0x70);
        if (data == nullptr) return 0;
        combi = 1;
        mirror_.lightmapSlot[i] = data;
    }
    return combi;
}

// eCShaderBase::ExecutePass's SetSwitch: the shader's switch elements, last first
void StaticOps::applySwitch(const std::uint8_t* shader, std::int32_t s, Skip& skip) {
    const std::int32_t count = get<std::int32_t>(shader, 0x250);
    U8* elements = ptr(shader, 0x248);
    if (count <= 0) return;
    Mirror::ShaderPools* pools = poolsOf(shader);
    for (std::int32_t i = count - 1; i >= 0; --i) {
        U8* e = ptr(elements, 8 * static_cast<std::size_t>(i));
        const void* fn = ptr(ptr(e, 0), 0xf0);
        if (fn == engine_ + kElementSetSwitchNoop) continue;
        if (fn == engine_ + kConstantSwitchSetSwitch) {
            const std::int32_t idx = sc::constantSwitchIndex(s, get<std::int32_t>(e, 0x58), get<std::int32_t>(e, 0x68));
            U8* pool = ptr(e, 0x70);
            const std::int32_t reg = get<std::int32_t>(e, 0x78, -1);
            if (get<std::int32_t>(e, 0x58) <= 0) continue;
            if (idx < 0) {
                skip = Skip::Switch;
                return;
            }
            U8* colour = ptr(e, 0x50) + 0x20 * static_cast<std::size_t>(idx);
            float v[4] = {get<float>(colour, 8), get<float>(colour, 0xc), get<float>(colour, 0x10), get<float>(colour, 0x18)};
            if (pool == nullptr || reg < 0) continue;
            if (pool == shader + 0xc8 && static_cast<std::uint32_t>(reg) >= pools->psFirst && static_cast<std::uint32_t>(reg) - pools->psFirst < 16) {
                std::memcpy(pools->ps[reg - pools->psFirst], v, 16);
            } else if (pool == shader + 0x98 && static_cast<std::uint32_t>(reg) >= pools->vsFirst && static_cast<std::uint32_t>(reg) - pools->vsFirst < 32) {
                std::memcpy(pools->vs[reg - pools->vsFirst], v, 16);
            } else {
                skip = Skip::Switch;
                return;
            }
            continue;
        }
        if (fn == engine_ + kSamplerSetSwitch) {
            const std::int32_t idx = sc::samplerSwitchIndex(s, get<std::int32_t>(e, 0x88), get<std::int32_t>(e, 0xe8), get<std::int32_t>(e, 0x80),
                                                            get<std::int32_t>(e, 0xd4));
            if (idx < 0) continue;  // not a switch sampler: SetSwitch changes nothing
            U8* image = ptr(ptr(e, 0xe0), 8 * static_cast<std::size_t>(idx));
            U8* pool = ptr(e, 0x90);
            const std::int32_t slot = get<std::int32_t>(e, 0x98, -1);
            if (image == nullptr) continue;
            if (pool != shader + 0xf8 || slot < 0 || slot >= 16) {
                skip = Skip::Switch;
                return;
            }
            pools->slotImage[static_cast<std::size_t>(slot)] = image;
            continue;
        }
        skip = Skip::Switch;
        return;
    }
}

bool StaticOps::geometryOf(const std::uint8_t* res, Geom& g, int stream3) {
    U8* visual = ptr(res, 0x88);
    const auto* prim = hal::vulkan::HalPrimitive::fromApi(ptr(visual, 0x10));
    if (prim == nullptr) return false;
    g.decl = prim->vertexDecl();
    g.indices = prim->indexBuffer();
    for (std::uint32_t i = 0; i < 4; ++i) {
        const auto v = prim->streamView(i);
        // HalPrimitive::Execute binds enabled streams with data (and sets their frequency), unbinds the others; the op path
        // itself switches stream 3 (RenderZPass: off, the instanced batches: on)
        const bool enabled = i == 3 && stream3 >= 0 ? stream3 == 1 : v.enabled;
        g.streams[i] = enabled && v.buffer != nullptr ? sc::DrawDesc::Stream{v.buffer, v.offset, v.stride, v.freq} : sc::DrawDesc::Stream{};
    }
    g.visual.fill = get<std::uint32_t>(visual, 0x18, 3);
    g.visual.cull = get<std::uint32_t>(visual, 0x1c, 2);
    g.visual.depthFunc = get<std::uint32_t>(visual, 0x20, 4);
    g.visual.depthWrite = get<std::uint32_t>(visual, 0x24, 2);
    return true;
}

sc::DrawDesc& StaticOps::drawFrom(const Geom& g) {
    sc::DrawDesc& d = newDraw();
    d.decl = g.decl;
    d.streams = g.streams;
    d.indices = g.indices;
    d.primType = 4;    // eCSubMesh::Render / RenderZPass / ExecuteRenderInstances: an indexed triangle list
    d.baseVertex = 0;  // RenderZPass sets it 0; nothing else of the static path writes it
    d.minIndex = g.minIndex;
    d.numVertices = g.numVertices;
    d.startIndex = g.startIndex;
    d.primCount = g.primCount;
    d.subMesh = g.subMesh;
    d.material = g.material;
    return d;
}

// every live register of the draw's shaders from the mirror (+ the shader's own pools)
void StaticOps::constantsInto(sc::DrawDesc& d, const std::uint8_t* shader) {
    const auto* vs = static_cast<const d3d9::Shader*>(d.vs);
    const auto* ps = static_cast<const d3d9::Shader*>(d.ps);
    const Live& lv = liveOf(vs);
    const Live& lp = liveOf(ps);
    for (std::uint32_t r = 0; r < sc::kVsRegs; ++r) d.vsLive[r] = lv.f[r];
    for (std::uint32_t r = 0; r < sc::kPsRegs; ++r) d.psLive[r] = lp.f[r];
    // c0..c3: the view-projection, or the depth-bias one for UseDepthBias materials (ExecutePass / ExecuteSpecialPass)
    const bool depthBias = get<std::uint8_t>(shader, 0x78) != 0;
    for (std::uint32_t k = 0; k < 4; ++k) std::memcpy(mirror_.adminVs[k], depthBias ? view_.dbvp[k] : view_.vp[k], 16);
    const Mirror::ShaderPools* pools = poolsOf(shader);
    for (std::uint32_t r = 0; r < sc::kVsRegs; ++r) {
        if (!d.vsLive[r]) continue;
        const float* v = nullptr;
        sc::Src src = sc::Src::Pool;
        if (r < 31) {
            v = mirror_.adminVs[r];
            if (r <= 11 || r == 30) src = sc::Src::Native;
        } else if (r < 232) {
            v = mirror_.global[r - 31];
            if (r >= 32) src = sc::Src::Native;  // the instances' world rows and fade (.xyz of c32 + 4i: carry-over)
        } else if (r >= pools->vsFirst && r - pools->vsFirst < pools->vsCount) {
            v = pools->vs[r - pools->vsFirst];
        }
        if (v == nullptr) continue;
        std::memcpy(d.vsF[r], v, 16);
        d.vsSrc[r] = src;
    }
    for (std::uint32_t r = 0; r < sc::kPsRegs; ++r) {
        if (!d.psLive[r]) continue;
        const float* v = nullptr;
        sc::Src src = sc::Src::Pool;
        if (r < 6) {
            v = mirror_.adminPs[r];
            if (r != 2) src = sc::Src::Native;
        } else if (r < 14) {
            if (r >= pools->psFirst && r - pools->psFirst < pools->psCount) v = pools->ps[r - pools->psFirst];
        } else if (r < 38) {
            v = mirror_.lighting[r - 14];
            src = sc::Src::Native;
        }
        if (v == nullptr) continue;
        std::memcpy(d.psF[r], v, 16);
        d.psSrc[r] = src;
    }
}

// eCShaderBase::ExecuteShaderConstants' material samplers (pool slot k -> stage base + k when the PS uses it)
StaticOps::Skip StaticOps::materialSamplers(sc::DrawDesc& d, const std::uint8_t* shader, const void* psElement) {
    const std::uint16_t mask = get<std::uint16_t>(psElement, 0x38);
    const std::uint32_t base = get<std::uint8_t>(shader, 0x219);
    const Mirror::ShaderPools* pools = poolsOf(shader);
    U8* gfx = view_.gfx;
    const std::uint32_t defFilter = get<std::uint32_t>(gfx, 0x640);
    for (std::uint32_t k = 0; k < pools->slots; ++k) {
        const std::uint32_t stage = base + k;
        if (stage >= sc::kStages || (mask >> stage & 1u) == 0) continue;
        U8* slot = shader + 0xf8 + 0x28 * k;
        U8* res = static_cast<U8*>(pools->slotImage[k]);
        U8* data = nullptr;
        if (res != nullptr) {
            data = ptr(res, 0x70);
            if (data == nullptr) {
                U8* ph = ptr(placeholderTable_, 8 * stage);
                if (ph != nullptr && ptr(ph, 0x60) != nullptr) data = ph;
            }
        }
        if (data == nullptr) data = ptr(view_.admin, 0x348);  // the ErrorImage
        if (ptr(imageOverride_, 8 * stage) != nullptr) return Skip::ImageOverride;
        bool ok = false;
        sc::DrawDesc::Sampler& s = d.samplers[stage];
        s.texture = textureOfImageData(data, ok);
        if (!ok) d.inheritMask |= static_cast<std::uint16_t>(1u << stage);
        std::uint32_t raw[7] = {get<std::uint32_t>(slot, 0), get<std::uint32_t>(slot, 4), get<std::uint32_t>(slot, 0xc), get<std::uint32_t>(slot, 8),
                                get<std::uint32_t>(slot, 0x10), get<std::uint32_t>(slot, 0x18), 0};
        for (std::uint32_t i = 0; i < 6; ++i) s.ss[i] = raw[i] == kNoState ? defFilter : raw[i];
        s.ss[6] = static_cast<std::uint32_t>(static_cast<std::int32_t>(get<std::int8_t>(slot, 0x14)));
    }
    return Skip::None;
}

// ---- composition ------------------------------------------------------------------------------------------------------------

StaticOps::Skip StaticOps::specialPassDraw(const std::uint8_t* shader, const Geom& geometry) {
    U8* admin = view_.admin;
    // ExecuteSpecialPass 0x6d1cd0: the mask reference first (PS c5.w), then the version's special pass
    if (get<std::int32_t>(shader, 0x50) == 1) mirror_.adminPs[5][3] = static_cast<float>(get<std::uint8_t>(shader, 0x58)) * (1.0f / 256.0f);
    const std::int32_t v = get<std::int32_t>(admin, 0x170);
    U8* version = ptr(shader, 0x258 + 8 * static_cast<std::size_t>(v));
    if (version == nullptr) return Skip::None;  // no draw
    if (get<const void*>(version, 0) != engine_ + kVersionDefault3Vtable) return Skip::Version;
    const std::int32_t type = get<std::int32_t>(admin, 0x314, 6);
    if (type == 6) return Skip::None;
    const std::size_t idx = static_cast<std::size_t>(type) * 2;
    if (get<std::uint8_t>(version, 0x18 + idx) == 0) return Skip::None;
    U8* vsElem = ptr(version, 0x30 + 16 * idx);
    U8* psElem = ptr(version, 0xf0 + 16 * idx);
    if (vsElem == nullptr || psElem == nullptr) return Skip::NotCompiled;
    d3d9::Shader* vs = shaderOfElement(vsElem);
    d3d9::Shader* ps = shaderOfElement(psElem);
    if (vs == nullptr || ps == nullptr) return Skip::None;
    sc::DrawDesc& d = drawFrom(geometry);
    const sc::Visual& vis = geometry.visual;
    d.zPass = true;
    d.vs = vs;
    d.ps = ps;
    d.states = sc::deviceStates(sc::zPassStates(get<std::uint8_t>(admin, 0x313) != 0, view_.zColorWrite), vis, get<std::uint32_t>(admin, 0x340));
    constantsInto(d, shader);
    const Skip s = materialSamplers(d, shader, psElem);
    if (s != Skip::None) return s;
    // stages the z PS samples beyond the material's: whatever is bound (BeginMaterial has not run yet)
    for (std::uint32_t st = 0; st < sc::kStages; ++st) {
        if (ps->reflection.samplers[st] == shader::TextureType::Unknown) continue;
        d.samplerMask |= static_cast<std::uint16_t>(1u << st);
        const std::uint32_t base = get<std::uint8_t>(shader, 0x219);
        if (st < base || st >= base + poolsOf(shader)->slots) d.inheritMask |= static_cast<std::uint16_t>(1u << st);
    }
    return Skip::None;
}

StaticOps::Skip StaticOps::passDraw(const std::uint8_t* shader, const std::uint8_t* version, const sc::Pass& pass, const Geom& geometry) {
    U8* admin = view_.admin;
    Skip skip = Skip::None;
    applySwitch(shader, pass.switchValue, skip);
    if (skip != Skip::None) return skip;
    const std::uint32_t vsIdx = sc::vsIndex(pass.flags), psIdx = sc::psIndex(pass.flags);
    U8* vsElem = ptr(version, 0x1b0 + 16 * vsIdx);
    U8* psElem = ptr(version, 0x210 + 16 * psIdx);
    if (vsElem == nullptr || psElem == nullptr) return Skip::NotCompiled;
    writeLighting(pass);
    sc::BlendInputs b;
    b.mode = get<std::int32_t>(shader, 0x50);
    b.maskRef = get<std::uint8_t>(shader, 0x58);
    b.hasOpacity = get<std::uint8_t>(shader, 0x330) != 0;
    b.refracted = get<std::uint8_t>(shader, 0x420) != 0;
    b.tradeAlphaAsSolid = get<std::uint8_t>(shader, 0x79) != 0;
    const sc::RenderStates rs = sc::passStates(pass.flags, b, mirror_.global[1][3]);
    if (!rs.valid) return Skip::PassKind;
    if (rs.alphaTest == 2) mirror_.adminPs[5][3] = static_cast<float>(rs.alphaRef) * (1.0f / 256.0f);
    d3d9::Shader* vs = shaderOfElement(vsElem);
    d3d9::Shader* ps = shaderOfElement(psElem);
    if (vs == nullptr || ps == nullptr) return Skip::NotCompiled;
    sc::DrawDesc& d = drawFrom(geometry);
    const sc::Visual& vis = geometry.visual;
    d.passFlags = pass.flags;
    d.vs = vs;
    d.ps = ps;
    d.states = sc::deviceStates(rs, vis, get<std::uint32_t>(admin, 0x340));
    constantsInto(d, shader);
    if (const Skip s = materialSamplers(d, shader, psElem); s != Skip::None) return s;
    // the admin's samplers bound by BeginMaterial (s0 mask, s1 refraction, s2 depth map) and BindLightmaps (s4, s5)
    struct AdminSlot {
        std::uint32_t stage, slot;
        const void* image;
    };
    const AdminSlot slots[] = {{0, 0x298, ptr(admin, 0x2b8)}, {1, 0x248, ptr(admin, 0x268)}, {2, 0x270, ptr(admin, 0x290)},
                               {4, 0x2c0, mirror_.lightmapSlot[0]}, {5, 0x2e8, mirror_.lightmapSlot[1]}};
    const std::uint32_t defFilter = get<std::uint32_t>(view_.gfx, 0x640);
    for (const AdminSlot& a : slots) {
        U8* slot = admin + a.slot;
        const void* data = a.image != nullptr ? a.image : ptr(admin, 0x348);
        if (ptr(imageOverride_, 8 * a.stage) != nullptr) return Skip::ImageOverride;
        bool ok = false;
        sc::DrawDesc::Sampler& s = d.samplers[a.stage];
        s.texture = textureOfImageData(data, ok);
        if (!ok) d.inheritMask |= static_cast<std::uint16_t>(1u << a.stage);
        std::uint32_t raw[6] = {get<std::uint32_t>(slot, 0), get<std::uint32_t>(slot, 4), get<std::uint32_t>(slot, 0xc), get<std::uint32_t>(slot, 8),
                                get<std::uint32_t>(slot, 0x10), get<std::uint32_t>(slot, 0x18)};
        for (std::uint32_t i = 0; i < 6; ++i) s.ss[i] = raw[i] == kNoState ? defFilter : raw[i];
        s.ss[6] = static_cast<std::uint32_t>(static_cast<std::int32_t>(get<std::int8_t>(slot, 0x14)));
    }
    const std::uint32_t base = get<std::uint8_t>(shader, 0x219);
    for (std::uint32_t st = 0; st < sc::kStages; ++st) {
        if (ps->reflection.samplers[st] == shader::TextureType::Unknown) continue;
        d.samplerMask |= static_cast<std::uint16_t>(1u << st);
        const bool material = st >= base && st < base + poolsOf(shader)->slots;
        const bool adminStage = st == 0 || st == 1 || st == 2 || st == 4 || st == 5;
        if (!material && !adminStage) d.inheritMask |= static_cast<std::uint16_t>(1u << st);
    }
    return Skip::None;
}

// eCSubMesh::Render 0x5c7eb0 (+ the GetPasses / ExecutePass chain)
StaticOps::Skip StaticOps::subMeshDraws(const std::uint8_t* res, const std::uint8_t* sub, std::uint32_t node, std::int32_t switchValue,
                                        std::uint32_t combi, const std::uint8_t* illum, float fade, const float* world, bool instanced,
                                        std::uint8_t flags, std::uint32_t batchCount) {
    U8* material = ptr(sub, 0x38);
    U8* shader = ptr(material, 0x70);
    if (ptr(res, 0x88) == nullptr || material == nullptr || shader == nullptr) return Skip::None;
    if (!shaderClassOk(shader)) return Skip::ShaderClass;
    if (flags != 0) return Skip::Overlay;
    U8* admin = ptr(shader, 0x220);
    if (admin != view_.admin) return Skip::Unreadable;
    if (!instanced) {
        writeAlpha(0, fade);
        writeWorld(0, world);
    }
    Geom geometry;
    if (!geometryOf(res, geometry, instanced ? 1 : -1)) return Skip::Primitive;
    if (instanced) {
        for (std::uint32_t i = 0; i < 3; ++i) {
            if (geometry.streams[i].buffer != nullptr) geometry.streams[i].freq = batchCount | 0x40000000u;
        }
    }
    geometry.minIndex = get<std::uint32_t>(sub, 0x30);
    geometry.startIndex = get<std::uint32_t>(sub, 0x28);
    geometry.numVertices = get<std::uint32_t>(sub, 0x34);
    geometry.primCount = get<std::uint32_t>(sub, 0x2c) / 3;
    if (node != 0) {
        U8* h = ptr(sub, 0x40) + 0x28 * static_cast<std::size_t>(node);
        const std::int32_t first = get<std::int32_t>(h, 0x20);
        const std::int32_t end = get<std::int32_t>(ptr(h, 0), 0x20);
        geometry.startIndex = static_cast<std::uint32_t>(first) + get<std::uint32_t>(sub, 0x28);
        geometry.primCount = static_cast<std::uint32_t>(end - first) / 3;
    }
    geometry.subMesh = sub;
    geometry.material = material;
    // GetPasses 0x6d1ee0 -> OnGetPasses 0x6d3170 -> eCShaderVersionDefault_3_0::GetPasses 0x6cc970
    if (get<std::uint8_t>(shader, 0x2a8) == 1) return Skip::InvalidShader;
    if (get<std::uint32_t>(admin, 0x31c) != 0) return Skip::InvalidShader;
    std::int32_t v = std::min(get<std::int32_t>(admin, 0x170), get<std::int32_t>(shader, 0x68));
    U8* version = ptr(shader, 0x258 + 8 * static_cast<std::size_t>(std::max(v, 0)));
    while (version == nullptr && v > 0) version = ptr(shader, 0x258 + 8 * static_cast<std::size_t>(--v));
    if (version == nullptr) return Skip::None;
    if (get<const void*>(version, 0) != engine_ + kVersionDefault3Vtable) return Skip::Version;
    sc::BlendInputs b;
    b.mode = get<std::int32_t>(shader, 0x50);
    b.hasOpacity = get<std::uint8_t>(shader, 0x330) != 0;
    b.refracted = get<std::uint8_t>(shader, 0x420) != 0;
    b.tradeAlphaAsSolid = get<std::uint8_t>(shader, 0x79) != 0;
    sc::PassInputs in;
    in.zFill = get<std::uint8_t>(shader, 0x2d8) != 0 && sc::isBlended(b);
    in.illum = illum;
    U8* list = ptr(illum, 0);
    in.hasList = list != nullptr;
    in.listSun = get<std::uint8_t>(list, 0);
    in.illumNoSun = get<std::uint8_t>(illum, 0xc);
    in.staticLights = get<std::uint8_t>(illum, 0xd);
    in.dynamicLights = get<std::uint8_t>(list, 0x34);
    in.combi = combi;
    in.instanced = instanced;
    in.switchValue = switchValue;
    std::vector<sc::Pass> passes;
    sc::buildPasses(in, passes);
    for (const sc::Pass& p : passes) {
        if (const Skip s = passDraw(shader, version, p, geometry); s != Skip::None) return s;
    }
    return Skip::None;
}

StaticOps::Skip StaticOps::composeZ(const Op& op) {
    U8* ctx = static_cast<U8*>(op.raw.context);
    U8* res = ptr(ctx, 0x28);
    U8* entity = ptr(ctx, 0x10);
    if (res == nullptr) return Skip::None;
    if (get<std::int32_t>(res, 0x38) != 3 || ptr(res, 0x88) == nullptr) return Skip::None;
    float world[16];
    if (!rd(entity, 0x60, world, 64)) return Skip::Unreadable;
    Geom geometry;
    if (!geometryOf(res, geometry, 0)) return Skip::Primitive;
    U8* subs = ptr(res, 0x58);
    const std::int32_t count = get<std::int32_t>(res, 0x60);
    const std::uint32_t sub = op.raw.desc & 0xffu, node = op.raw.desc >> 8;
    const auto shaderOfSub = [&](std::uint32_t i, bool needCached) -> U8* {
        U8* m = ptr(subs + 0x48 * static_cast<std::size_t>(i), 0x38);
        if (m == nullptr || (needCached && get<std::int32_t>(m, 0x38) != 3)) return nullptr;
        return ptr(m, 0x70);
    };
    if (sub == 0xff) {
        std::vector<sc::ZSub> zs(static_cast<std::size_t>(std::max(count, 0)));
        for (std::int32_t i = 0; i < count; ++i) {
            U8* s = subs + 0x48 * static_cast<std::size_t>(i);
            U8* shader = shaderOfSub(static_cast<std::uint32_t>(i), true);
            sc::ZSub& z = zs[static_cast<std::size_t>(i)];
            if (shader == nullptr) continue;
            if (!shaderClassOk(shader)) return Skip::ShaderClass;
            if (ptr(shader, 0x220) != view_.admin) return Skip::Unreadable;
            z.ready = true;
            sc::BlendInputs b;
            b.mode = get<std::int32_t>(shader, 0x50);
            b.hasOpacity = get<std::uint8_t>(shader, 0x330) != 0;
            b.refracted = get<std::uint8_t>(shader, 0x420) != 0;
            b.tradeAlphaAsSolid = get<std::uint8_t>(shader, 0x79) != 0;
            z.blended = sc::isBlended(b);
            z.masked = b.mode == 1;
            z.firstVertex = get<std::uint32_t>(s, 0x30);
            z.vertexCount = get<std::uint32_t>(s, 0x34);
            z.startIndex = get<std::uint32_t>(s, 0x28);
            z.indexCount = get<std::uint32_t>(s, 0x2c);
        }
        // SetWorldMatrix / SetGlobalAlpha(1, 0) for every ready sub-mesh: the same values
        writeWorld(0, world);
        writeAlpha(0, 1.0f);
        std::vector<sc::ZDraw> plan;
        sc::planWholeMeshZ(zs, plan);
        for (const sc::ZDraw& z : plan) {
            Geom g = geometry;
            g.minIndex = z.minIndex;
            g.numVertices = z.numVertices;
            g.startIndex = z.startIndex;
            g.primCount = z.primCount;
            g.subMesh = subs + 0x48 * static_cast<std::size_t>(z.shaderOf);
            g.material = ptr(g.subMesh, 0x38);
            if (const Skip s = specialPassDraw(shaderOfSub(z.shaderOf, true), g); s != Skip::None) return s;
        }
        return Skip::None;
    }
    if (static_cast<std::int32_t>(sub) >= count) return Skip::Unreadable;
    U8* s = subs + 0x48 * static_cast<std::size_t>(sub);
    U8* shader = shaderOfSub(sub, true);
    if (shader == nullptr) return Skip::None;
    if (!shaderClassOk(shader)) return Skip::ShaderClass;
    if (ptr(shader, 0x220) != view_.admin) return Skip::Unreadable;
    writeWorld(0, world);
    writeAlpha(0, 1.0f);
    U8* h = ptr(s, 0x40) + 0x28 * static_cast<std::size_t>(node);
    const std::int32_t first = get<std::int32_t>(h, 0x20);
    const std::int32_t end = get<std::int32_t>(ptr(h, 0), 0x20);
    Geom g = geometry;
    g.minIndex = get<std::uint32_t>(s, 0x30);
    g.startIndex = get<std::uint32_t>(s, 0x28) + static_cast<std::uint32_t>(first);
    g.numVertices = get<std::uint32_t>(s, 0x34);
    g.primCount = static_cast<std::uint32_t>(end - first) / 3;
    g.subMesh = s;
    g.material = ptr(s, 0x38);
    if (g.primCount == 0) return Skip::None;
    return specialPassDraw(shader, g);
}

StaticOps::Skip StaticOps::composeSolid(const Op& op) {
    U8* ctx = static_cast<U8*>(op.raw.context);
    U8* res = ptr(ctx, 0x28);
    if (res == nullptr) return Skip::None;
    const std::uint32_t sub = op.raw.desc & 0xffu, node = op.raw.desc >> 8;
    U8* subs = ptr(res, 0x58);
    const std::int32_t count = get<std::int32_t>(res, 0x60);
    if (node == 0xffffffu) {
        // eCMeshResource2::ExecuteRenderInstances 0x7607b0 (instances sorted once per frame by Render)
        const std::int32_t n = get<std::int32_t>(res, 0xb8);
        if (n <= 0) return Skip::None;
        if (static_cast<std::int32_t>(sub) >= count) return Skip::Unreadable;
        const std::uint32_t combi = bindLightmaps(res);
        U8* s = subs + 0x48 * static_cast<std::size_t>(sub);
        U8* material = ptr(s, 0x38);
        U8* shader = ptr(material, 0x70);
        if (material == nullptr || shader == nullptr) return Skip::None;
        if (!shaderClassOk(shader)) return Skip::ShaderClass;
        std::vector<std::uint8_t> inst(static_cast<std::size_t>(n) * 0x58);
        if (!rd(ptr(res, 0xb0), 0, inst.data(), inst.size())) return Skip::Unreadable;
        if (get<std::int32_t>(res, 0xa4) != 0 && n > 1) qsort_(inst.data(), static_cast<std::size_t>(n), 0x58, instanceCmp_);
        const auto at = [&](std::int32_t i) { return inst.data() + 0x58 * static_cast<std::size_t>(i); };
        const auto illumOf = [&](std::int32_t i) { U8* p = nullptr; std::memcpy(&p, at(i) + 0x48, 8); return p; };
        const auto switchOf = [&](std::int32_t i) { std::int32_t v = 0; std::memcpy(&v, at(i) + 0x40, 4); return v; };
        if (get<std::uint8_t>(res, 0xa0) == 0 || n < 2 || combi == 1) {
            for (std::int32_t i = 0; i < n; ++i) {
                const float* m = reinterpret_cast<const float*>(at(i));
                if (const Skip k = subMeshDraws(res, s, 0, switchOf(i), combi, illumOf(i), 1.0f, m, false, at(i)[0x50], 0); k != Skip::None) return k;
            }
            return Skip::None;
        }
        std::vector<sc::Instance> list(static_cast<std::size_t>(n));
        for (std::int32_t i = 0; i < n; ++i) {
            U8* il = illumOf(i);
            sc::Instance& x = list[static_cast<std::size_t>(i)];
            x.list = ptr(il, 0);
            x.switchValue = switchOf(i);
            x.flags = at(i)[0x50];
            x.f = get<float>(il, 8);
            x.alone = get<std::uint8_t>(il, 0xd) != 0 || get<std::uint8_t>(il, 0x90) != 0;
        }
        std::vector<sc::Batch> batches;
        sc::planBatches(list, 50, batches);
        for (const sc::Batch& bt : batches) {
            for (std::uint32_t k = 0; k < bt.count; ++k) {
                writeWorld(k, reinterpret_cast<const float*>(at(static_cast<std::int32_t>(bt.first + k))));
                writeAlpha(k, 1.0f);
            }
            const std::int32_t last = static_cast<std::int32_t>(bt.first + bt.count - 1);
            if (const Skip k = subMeshDraws(res, s, 0, switchOf(last), 0, illumOf(last), 1.0f, nullptr, true, at(last)[0x50], bt.count);
                k != Skip::None) {
                return k;
            }
        }
        return Skip::None;
    }
    // eCRenderContextMesh::Render 0x5c7110 -> eCMeshResource2::Render 0x760ca0
    U8* entity = ptr(ctx, 0x10);
    U8* illumPs = ptr(entity, 0xf8);
    U8* illum = illumPs != nullptr ? illumPs + 0x78 : nullptr;
    const std::uint32_t eflags = get<std::uint32_t>(entity, 0x28);
    std::uint8_t flags = 0;
    if (eflags & 0x200000u) flags |= 2;
    if (eflags & 0x400000u) flags |= 1;
    if (eflags & 0x800000u) flags |= 4;
    const std::int32_t switchValue = get<std::int32_t>(ptr(ctx, 0x18), 0x20);
    float world[16];
    if (!rd(entity, 0x60, world, 64)) return Skip::Unreadable;
    if (ptr(res, 0x88) == nullptr) return Skip::None;
    const std::uint32_t combi = bindLightmaps(res);
    if (sub == 0xff) {
        for (std::int32_t i = 0; i < count; ++i) {
            if (const Skip k = subMeshDraws(res, subs + 0x48 * static_cast<std::size_t>(i), node, switchValue, combi, illum, op.raw.fade, world, false, flags, 0);
                k != Skip::None) {
                return k;
            }
        }
        return Skip::None;
    }
    if (static_cast<std::int32_t>(sub) >= count) return Skip::Unreadable;
    return subMeshDraws(res, subs + 0x48 * static_cast<std::size_t>(sub), node, switchValue, combi, illum, op.raw.fade, world, false, flags, 0);
}

StaticOps::Skip StaticOps::composeOp(const Op& op) {
    if (op.kind != OpKind::Mesh || (op.list != List::ZPass && op.list != List::Solid)) return Skip::Kind;
    if (!view_.valid) {
        // the admin from the op's shader (eCShaderBase +0x220): any material shader of the view
        U8* shader = static_cast<U8*>(op.shader);
        if (shader == nullptr) {
            U8* res = ptr(op.raw.context, 0x28);
            U8* subs = ptr(res, 0x58);
            if (get<std::int32_t>(res, 0x60) > 0) shader = ptr(ptr(subs, 0x38), 0x70);
        }
        U8* admin = ptr(shader, 0x220);
        if (admin == nullptr || !composeView(admin)) return Skip::Unreadable;
        if (view_.dbOrder < 0) spdlog::warn("native: static ops: the depth-bias view-projection matches neither product order");
    }
    initMirror();
    return op.list == List::ZPass ? composeZ(op) : composeSolid(op);
}

// ---- per op -----------------------------------------------------------------------------------------------------------------

void StaticOps::opEnter(const Op& op) {
    finishOp();
    if (!active() || !inView_) return;
    if (verified_ == 0) verified_ = verify() ? 1 : -1;
    if (verified_ < 0) return;
    if (op.kind != OpKind::Mesh || (op.list != List::ZPass && op.list != List::Solid)) return;
    drawCount_ = 0;
    ++window_.ops;
    const Skip s = composeOp(op);
    if (s != Skip::None) {
        ++skips_[static_cast<std::size_t>(s)];
        ++window_.opsNotComposed;
        drawCount_ = 0;
        return;
    }
    ++window_.opsComposed;
    op_ = &op;
    engineDraws_ = 0;
    opDiffLogged_ = false;
    taken_ = false;
    if (takeOver_) {
        issue();
        taken_ = true;
    }
}

void StaticOps::finishOp() {
    if (op_ == nullptr) return;
    if (compare_ && engineDraws_ != drawCount_) {
        ++window_.mismatches[static_cast<std::size_t>(sc::Field::DrawCount)];
        if (mismatchLogged_ < 24) {
            ++mismatchLogged_;
            spdlog::info("native: static ops: mismatch: {} composed {} draws, the engine made {}", describe(*op_), drawCount_, engineDraws_);
        }
    }
    op_ = nullptr;
    taken_ = false;
}

void StaticOps::capture(const d3d9::DrawTapInfo& t, sc::DrawDesc& out) {
    const d3d9::Device& d = *t.device;
    out.vs = d.vertexShader();
    out.ps = d.pixelShader();
    out.decl = d.vertexDecl();
    for (std::uint32_t i = 0; i < 4; ++i) {
        const auto s = d.streamSource(i);
        out.streams[i] = {s.buffer, s.offset, s.stride, s.freq};
    }
    out.indices = d.indexBuffer();
    out.primType = t.primitiveType;
    out.baseVertex = t.baseVertex;
    out.minIndex = t.minIndex;
    out.numVertices = t.numVertices;
    out.startIndex = t.first;
    out.primCount = t.primitives;
    using namespace d3d9;
    out.states[sc::StZFunc] = d.renderState(RS_ZFUNC);
    out.states[sc::StZWrite] = d.renderState(RS_ZWRITEENABLE);
    out.states[sc::StBlend] = d.renderState(RS_ALPHABLENDENABLE);
    out.states[sc::StSrc] = d.renderState(RS_SRCBLEND);
    out.states[sc::StDst] = d.renderState(RS_DESTBLEND);
    out.states[sc::StColorWrite] = d.renderState(RS_COLORWRITEENABLE);
    out.states[sc::StAlphaTest] = d.renderState(RS_ALPHATESTENABLE);
    out.states[sc::StCull] = d.renderState(RS_CULLMODE);
    out.states[sc::StFill] = d.renderState(RS_FILLMODE);
    out.states[sc::StFog] = d.renderState(RS_FOGENABLE);
    out.samplerMask = 0;
    const auto* ps = d.pixelShader();
    for (std::uint32_t st = 0; st < sc::kStages; ++st) {
        if (ps == nullptr || ps->reflection.samplers[st] == shader::TextureType::Unknown) continue;
        out.samplerMask |= static_cast<std::uint16_t>(1u << st);
        auto& s = out.samplers[st];
        s.texture = d.texture(st);
        static constexpr std::uint32_t kSs[7] = {SAMP_ADDRESSU, SAMP_ADDRESSV, SAMP_MAGFILTER, SAMP_MINFILTER, SAMP_MIPFILTER, SAMP_MIPMAPLODBIAS,
                                                 SAMP_SRGBTEXTURE};
        for (std::uint32_t i = 0; i < 7; ++i) s.ss[i] = d.samplerState(st, kSs[i]);
    }
    const Live& lv = liveOf(d.vertexShader());
    const Live& lp = liveOf(ps);
    for (std::uint32_t r = 0; r < sc::kVsRegs; ++r) {
        out.vsLive[r] = lv.f[r];
        if (lv.f[r]) std::memcpy(out.vsF[r], d.vsConstant(r), 16);
    }
    for (std::uint32_t r = 0; r < sc::kPsRegs; ++r) {
        out.psLive[r] = lp.f[r];
        if (lp.f[r]) std::memcpy(out.psF[r], d.psConstant(r), 16);
    }
}

void StaticOps::ownDraw(const d3d9::DrawTapInfo& t) {
    if (!compare_ || issueIndex_ >= drawCount_) return;
    capture(t, *scratch_);
    const sc::Diff diff = sc::compare(*draws_[issueIndex_], *scratch_);
    ++ownChecked_;
    if (diff.fields.any()) {
        ++ownMismatch_;
        if (ownMismatch_ <= 8) spdlog::info("native: static ops: our own draw differs from its composition: {}", diff.first);
    }
}

bool StaticOps::engineDraw(const d3d9::DrawTapInfo& t, const Op* op) {
    if (op_ == nullptr || op != op_ || issuing_) return false;
    const std::uint32_t k = engineDraws_++;
    if (compare_) {
        ++window_.draws;
        if (k >= drawCount_) {
            // counted once per op in finishOp
        } else {
            capture(t, *scratch_);
            const sc::Diff diff = sc::compare(*draws_[k], *scratch_);
            if (hal::vulkan::logThisFrame() && frameLogged_ < 12) {
                ++frameLogged_;
                const sc::DrawDesc& c = *draws_[k];
                const auto* vs = static_cast<const d3d9::Shader*>(c.vs);
                const auto* ps = static_cast<const d3d9::Shader*>(c.ps);
                spdlog::info("native: static ops: {} draw {}: vs {:016x} ps {:016x}, {} vs + {} ps live registers, sampled stages {:#x} "
                             "(inherited {:#x}), {} prims from {}, states z {} zw {} blend {} cw {:#x} cull {}; vs c33 ({}, {}, {}, {}), "
                             "ps c15 ({}, {}, {}, {}); {} registers / {} stages compared, {}",
                             describe(*op), k, vs != nullptr ? vs->hash : 0, ps != nullptr ? ps->hash : 0, scratch_->vsLive.count(),
                             scratch_->psLive.count(), scratch_->samplerMask, c.inheritMask, c.primCount, c.startIndex,
                             c.states[sc::StZFunc], c.states[sc::StZWrite], c.states[sc::StBlend], c.states[sc::StColorWrite],
                             c.states[sc::StCull], c.vsF[33][0], c.vsF[33][1], c.vsF[33][2], c.vsF[33][3], c.psF[15][0], c.psF[15][1],
                             c.psF[15][2], c.psF[15][3], diff.regs, diff.stages, diff.fields.none() ? "equal" : diff.first);
            }
            window_.closeFloats += diff.closeFloats;
            window_.regs += diff.regs;
            window_.stages += diff.stages;
            if (diff.fields.none()) {
                ++window_.cleanDraws;
            } else {
                for (std::size_t f = 0; f < sc::kFieldCount; ++f) {
                    if (diff.fields.test(f)) ++window_.mismatches[f];
                }
                if (mismatchLogged_ < 24 || (hal::vulkan::logThisFrame() && !opDiffLogged_)) {
                    ++mismatchLogged_;
                    opDiffLogged_ = true;
                    std::string mat;
                    if (draws_[k]->material != nullptr) mat = engine::readCString(static_cast<U8*>(draws_[k]->material) + 0x40);
                    spdlog::info("native: static ops: mismatch: {} draw {} (pass flags {:#x}{}) material '{}': {}", describe(*op), k,
                                 draws_[k]->passFlags, draws_[k]->zPass ? ", z" : "", mat, diff.first);
                }
            }
        }
    }
    if (taken_) ++droppedDraws_;
    return taken_;
}

// our own draws of the op: the device state saved and restored around them
void StaticOps::issue() {
    if (drawCount_ == 0) return;
    using namespace d3d9;
    static constexpr std::uint32_t kRs[sc::StOwnedCount] = {RS_ZFUNC, RS_ZWRITEENABLE, RS_ALPHABLENDENABLE, RS_SRCBLEND, RS_DESTBLEND,
                                                            RS_COLORWRITEENABLE, RS_ALPHATESTENABLE, RS_CULLMODE, RS_FILLMODE, RS_FOGENABLE};
    static constexpr std::uint32_t kSs[7] = {SAMP_ADDRESSU, SAMP_ADDRESSV, SAMP_MAGFILTER, SAMP_MINFILTER, SAMP_MIPFILTER, SAMP_MIPMAPLODBIAS,
                                             SAMP_SRGBTEXTURE};
    // save
    const ShaderRef vs = dev_.vertexShaderRef(), ps = dev_.pixelShaderRef();
    VertexDecl* decl = dev_.vertexDecl();
    Buffer* ib = dev_.indexBuffer();
    std::array<Device::StreamSource, 4> streams{};
    for (std::uint32_t i = 0; i < 4; ++i) streams[i] = dev_.streamSource(i);
    std::array<std::uint32_t, sc::StOwnedCount> rs{};
    for (std::uint32_t i = 0; i < sc::StOwnedCount; ++i) rs[i] = dev_.renderState(kRs[i]);
    std::array<Texture*, sc::kStages> tex{};
    std::array<std::array<std::uint32_t, 7>, sc::kStages> ss{};
    for (std::uint32_t st = 0; st < sc::kStages; ++st) {
        tex[st] = dev_.texture(st);
        for (std::uint32_t i = 0; i < 7; ++i) ss[st][i] = dev_.samplerState(st, kSs[i]);
    }
    static float savedVs[sc::kVsRegs][4], savedPs[sc::kPsRegs][4];
    dev_.getVertexShaderConstantF(0, &savedVs[0][0], sc::kVsRegs);
    dev_.getPixelShaderConstantF(0, &savedPs[0][0], sc::kPsRegs);
    draws::Tag& tag = dev_.drawTag();
    const draws::Tag savedTag = tag;

    issuing_ = true;
    for (std::size_t k = 0; k < drawCount_; ++k) {
        const sc::DrawDesc& d = *draws_[k];
        dev_.setVertexShaderRef(ShaderRef(const_cast<Shader*>(static_cast<const Shader*>(d.vs))));
        dev_.setPixelShaderRef(ShaderRef(const_cast<Shader*>(static_cast<const Shader*>(d.ps))));
        dev_.setVertexDecl(const_cast<VertexDecl*>(static_cast<const VertexDecl*>(d.decl)));
        for (std::uint32_t i = 0; i < 4; ++i) {
            const auto& s = d.streams[i];
            dev_.setStreamSource(i, const_cast<Buffer*>(static_cast<const Buffer*>(s.buffer)), s.offset, s.stride);
            if (s.buffer != nullptr) dev_.setStreamSourceFreq(i, s.freq);
        }
        dev_.setIndices(const_cast<Buffer*>(static_cast<const Buffer*>(d.indices)));
        for (std::uint32_t i = 0; i < sc::StOwnedCount; ++i) dev_.setRenderState(kRs[i], d.states[i]);
        for (std::uint32_t st = 0; st < sc::kStages; ++st) {
            if ((d.samplerMask >> st & 1u) == 0 || (d.inheritMask >> st & 1u) != 0) continue;
            dev_.setTexture(st, const_cast<Texture*>(static_cast<const Texture*>(d.samplers[st].texture)));
            for (std::uint32_t i = 0; i < 7; ++i) dev_.setSamplerState(st, kSs[i], d.samplers[st].ss[i]);
        }
        for (std::uint32_t r = 0; r < sc::kVsRegs; ++r) {
            if (d.vsSrc[r] != sc::Src::None) dev_.setVertexShaderConstantF(r, d.vsF[r], 1);
        }
        for (std::uint32_t r = 0; r < sc::kPsRegs; ++r) {
            if (d.psSrc[r] != sc::Src::None) dev_.setPixelShaderConstantF(r, d.psF[r], 1);
        }
        tag.subMesh = d.subMesh;
        tag.material = d.material;
        issueIndex_ = k;
        dev_.drawIndexedPrimitive(d.primType, d.baseVertex, d.minIndex, d.numVertices, d.startIndex, d.primCount);
        ++issuedDraws_;
    }
    issuing_ = false;
    ++issuedOps_;

    // restore
    tag = savedTag;
    dev_.setVertexShaderRef(vs);
    dev_.setPixelShaderRef(ps);
    dev_.setVertexDecl(decl);
    for (std::uint32_t i = 0; i < 4; ++i) {
        dev_.setStreamSource(i, streams[i].buffer, streams[i].offset, streams[i].stride);
        dev_.setStreamSourceFreq(i, streams[i].freq);
    }
    dev_.setIndices(ib);
    for (std::uint32_t i = 0; i < sc::StOwnedCount; ++i) dev_.setRenderState(kRs[i], rs[i]);
    for (std::uint32_t st = 0; st < sc::kStages; ++st) {
        dev_.setTexture(st, tex[st]);
        for (std::uint32_t i = 0; i < 7; ++i) dev_.setSamplerState(st, kSs[i], ss[st][i]);
    }
    dev_.setVertexShaderConstantF(0, &savedVs[0][0], sc::kVsRegs);
    dev_.setPixelShaderConstantF(0, &savedPs[0][0], sc::kPsRegs);
}

void StaticOps::report() {
    if (!active()) return;
    total_.add(window_);
    for (std::size_t i = 0; i < skips_.size(); ++i) skipsTotal_[i] += skips_[i];
    std::string skips;
    for (std::size_t i = 1; i < skips_.size(); ++i) {
        if (skips_[i] != 0) skips += std::format("{}{} {}", skips.empty() ? "" : ", ", name(static_cast<Skip>(i)), skips_[i]);
    }
    if (verified_ < 0) {
        spdlog::info("native: static ops: idle (the engine checks failed)");
    } else if (compare_) {
        spdlog::info("native: static ops: compare (5 s): {}{}", sc::summary(window_), skips.empty() ? "" : "; left to the engine: " + skips);
        spdlog::info("native: static ops: compare (total): {}", sc::summary(total_));
    } else {
        spdlog::info("native: static ops (5 s): {} ops, {} composed{}", window_.ops, window_.opsComposed, skips.empty() ? "" : "; left to the engine: " + skips);
    }
    if (takeOver_) {
        spdlog::info("native: static ops: drawn by the renderer: {} ops, {} draws (total); engine draws dropped {}; own draws checked {}, "
                     "differing from their composition {}",
                     issuedOps_, issuedDraws_, droppedDraws_, ownChecked_, ownMismatch_);
    }
    window_ = {};
    skips_ = {};
}

}  // namespace rvk::native
