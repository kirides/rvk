#include "native/static_ops.h"

#include <windows.h>

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstring>
#include <format>

#include <spdlog/spdlog.h>

#include "d3d9/defs.h"
#include "engine/memread.h"
#include "engine/scene_hooks.h"
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

inline bool rd(const void* p, std::size_t off, void* out, std::size_t n) {
    return p != nullptr && safeRead(static_cast<U8*>(p) + off, out, n);
}
template <class T>
inline T get(const void* p, std::size_t off, T fallback = T{}) {
    T v = fallback;
    if (!readAt(p, off, v)) return fallback;
    return v;
}
inline U8* ptr(const void* p, std::size_t off) { return get<U8*>(p, off, nullptr); }

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
                                             "pass kind", "instance sort", "texture", "overlay shader", "decal entity proxy",
                                             "buffers not created yet", "visual shader", "sky camera", "sky object rebuilt this frame",
                                             "more than 66 bones / a bone outside the palette", "a shadow-map z op",
                                             "simulated inside the slot (no live particle at the entry)", "an instanced emitter"};
    const auto i = static_cast<std::size_t>(s);
    return i < std::size(kNames) ? kNames[i] : "?";
}

const char* StaticOps::name(Group g) {
    switch (g) {
        case Group::Static: return "static ops";
        case Group::Mesh: return "scene ops [alpha / refracted meshes]";
        case Group::Decals: return "scene ops [decals]";
        case Group::Sky: return "scene ops [sky dome]";
        case Group::Billboards: return "scene ops [billboards]";
        case Group::SkyCube: return "scene ops [sky cube faces]";
        case Group::Actors: return "scene ops [skinned actors]";
        case Group::Trees: return "scene ops [SpeedTrees + tree billboards]";
        case Group::Vegetation: return "scene ops [vegetation]";
        case Group::ShadowViews: return "scene ops [shadow views]";
        case Group::Particles: return "scene ops [particles]";
        case Group::Strips: return "scene ops [strips + light streaks]";
        case Group::Precipitation: return "scene ops [precipitation]";
        default: return "?";
    }
}

StaticOps::StaticOps(hal::vulkan::HalDevice& hal) : hal_(hal), dev_(hal.dev()), scratch_(std::make_unique<sc::DrawDesc>()) {}

StaticOps::~StaticOps() = default;

bool StaticOps::active() const {
    return std::any_of(groups_.begin(), groups_.end(), [](const GroupState& g) { return g.active(); });
}

void StaticOps::setMode(Group group, bool takeOver, bool compare) {
    GroupState& g = groups_[static_cast<std::size_t>(group)];
    if (takeOver == g.takeOver && compare == g.compare) return;
    g.takeOver = takeOver;
    g.compare = compare;
    g.window = {};
    g.skips = {};
    static constexpr const char* kWhat[kGroups] = {"the main view's static mesh z / solid ops", "the main view's alpha / refracted mesh ops",
                                                   "the main view's decals (eCDecalAdmin::RenderDecals)",
                                                   "the main view's sky dome op (stars, sun, moon, dome)",
                                                   "the main view's billboards (the opaque queue eCBillboardAdmin::RenderBillboards, the alpha ops RenderBillboard)",
                                                   "the sky environment cube's faces (eCSkydome_PS::Render, cube camera)",
                                                   "the main view's skinned actor ops (eCRenderContextAnimation2: z / solid / alpha / refracted)",
                                                   "the main view's SpeedTree ops (branches, fronds, leaves: batches, LODs) and tree billboard ops",
                                                   "the main view's vegetation ops (grass: instanced batches from the shared buffer)",
                                                   "the shadow maps' z ops (sun cascades, terrain variance map, point-light atlas cells)",
                                                   "the main view's particle ops (eCParticle_PS: CPU quads)",
                                                   "the main view's strip and light streak ops (eCStrip_PS trails, eCLightStreaks_PS)",
                                                   "the main view's precipitation op (eCPrecipitation_PS: rain / snow, the late list)"};
    spdlog::info("native: {}: {} ({}{}{})", name(group), g.active() ? "on" : "off", kWhat[static_cast<std::size_t>(group)],
                 takeOver ? ", drawn by the renderer" : "", compare ? ", compared with the engine's draws" : "");
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

void StaticOps::viewBegin(void* db, void* desc, const FrameDesc& view) {
    if (!hal::vulkan::logThisFrame()) frameLogged_ = 0;
    db_ = db;
    desc_ = desc;
    inView_ = true;
    view_ = {};
    if (mirrorMode_ != 2) ++mirrorEpoch_;
    op_ = nullptr;
    viewKind_ = view.kind;
    shadowMap_ = view.shadowMap;
    shadowFormat_ = view.mapFormat;
    cascade_ = view.cascade;
    if (viewKind_ != ViewKind::Main && groups_[static_cast<std::size_t>(Group::ShadowViews)].active()) {
        ++shadowWindow_.views[static_cast<std::size_t>(viewKind_)];
    }
}

void StaticOps::ShadowStats::add(const ShadowStats& o) {
    for (std::size_t v = 0; v < kViewKindCount; ++v) {
        views[v] += o.views[v];
        for (std::size_t k = 0; k < kOpKindCount; ++k) {
            ops[v][k] += o.ops[v][k];
            draws[v][k] += o.draws[v][k];
            clean[v][k] += o.clean[v][k];
            composed[v][k] += o.composed[v][k];
        }
    }
}

void StaticOps::viewEnd() {
    finishOp();
    simulatedOp_ = nullptr;
    inView_ = false;
}

bool StaticOps::composeView(const std::uint8_t* admin, const std::uint8_t* camera) {
    view_ = {};
    view_.admin = admin;
    view_.gfx = ptr(admin, 0x38);
    U8* cam = camera != nullptr ? camera : ptr(ptr(db_, 0x14c0), 0x1738);
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
    // eCGfxFog::Execute(db +0x14c8), unless eCGfxFog::ms_pMasterFog is set; ExecuteShadowMap executes no fog (PS c3 / c4, VS c30 as
    // the admin's pools hold them)
    U8* fog = engine::sym().gfxMasterFog != nullptr ? ptr(engine::sym().gfxMasterFog, 0) : nullptr;
    if (fog == nullptr && camera == nullptr) fog = ptr(db_, 0x14c8);
    view_.fog = camera == nullptr && viewKind_ == ViewKind::Main;
    if (view_.fog && fog != nullptr) {
        sc::fogConstants(get<std::uint8_t>(fog, 0x18) == 1, get<std::uint32_t>(fog, 0x1c), get<float>(fog, 0x28), get<float>(fog, 0x2c),
                         get<float>(fog, 0x30), view_.fogC3, view_.fogC4);
    }
    // BeginZPass(NULL, true, h != 0): colour writes when the desc's depth image has an API texture (its render target); in a shadow
    // map BeginZPass(map, true, false) 0x6b9720 decides by the map's format (+8): colour unless D16 / D24S8 / DF16 / DF24 (3..6)
    U8* depthImage = ptr(desc_, 8);
    view_.zColorWrite = depthImage != nullptr && ptr(depthImage, 0x60) != nullptr;
    if (viewKind_ != ViewKind::Main) view_.zColorWrite = static_cast<std::uint32_t>(shadowFormat_ - 3) > 3u;
    view_.valid = true;
    return true;
}

// ---- helpers ----------------------------------------------------------------------------------------------------------------

bool StaticOps::shaderClassOk(const void* shader) {
    const void* vt = get<const void*>(shader, 0);
    // eCShaderLeafs (the SpeedTree leaves' class) shares every slot the composition relies on (checked in verifyTrees)
    return vt == engine_ + kShaderDefaultVtable || (leafsVtable_ != nullptr && vt == leafsVtable_);
}

int StaticOps::blendModeOf(const std::uint8_t* owner) const {
    return owner == blendOverrideShader_ && blendOverride_ >= 0 ? blendOverride_ : get<std::int32_t>(owner, 0x50);
}

const StaticOps::Live& StaticOps::liveOf(const d3d9::Shader* s) {
    auto [it, inserted] = live_.try_emplace(s);
    if (inserted && s != nullptr) {
        const auto r = fx::parseConstantTable(s->bytecode);
        if (r.ok()) {
            for (const fx::Constant& c : r.table.constants) {
                if (c.registerSet == fx::RegisterSet::Float4) {
                    for (std::uint32_t i = 0; i < c.registerCount && c.registerIndex + i < sc::kVsRegs; ++i) {
                        const std::uint32_t reg = c.registerIndex + i;
                        it->second.f.set(reg);
                        it->second.words[reg >> 6] |= 1ull << (reg & 63);
                    }
                } else if (c.registerSet != fx::RegisterSet::Sampler) {
                    it->second.boolInt += c.registerCount;
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
    // O1: the HAL's registry (a mutex + a hash map) behind a small cache, valid while the registry's generation holds
    if (const std::uint64_t gen = hal_.apiTextureGeneration(); gen != texCacheGen_) {
        texCache_.fill({});
        texCacheGen_ = gen;
    }
    const auto key = reinterpret_cast<std::uintptr_t>(api);
    const std::size_t h = static_cast<std::size_t>(((key >> 4) * 0x9E3779B97F4A7C15ull) >> (64 - kTexCacheBits));
    for (std::size_t i = 0; i < 4; ++i) {
        const TexCacheEntry& e = texCache_[(h + i) & (texCache_.size() - 1)];
        if (e.api == api) {
            ok = e.texture != nullptr;
            return e.texture;
        }
        if (e.api == nullptr) break;
    }
    d3d9::Texture* t = hal_.textureOfApi(api);
    if (t == nullptr) t = hal_.cubeOfApi(api);
    std::size_t slot = h;
    for (std::size_t i = 0; i < 4; ++i) {
        if (texCache_[(h + i) & (texCache_.size() - 1)].api == nullptr) {
            slot = h + i;
            break;
        }
    }
    texCache_[slot & (texCache_.size() - 1)] = {api, t};
    ok = t != nullptr;
    return t;
}

sc::DrawDesc& StaticOps::newDraw() {
    if (drawCount_ == draws_.size()) draws_.push_back(std::make_unique<sc::DrawDesc>());
    sc::DrawDesc& d = *draws_[drawCount_++];
    d.vs = d.ps = d.decl = d.indices = d.subMesh = d.material = nullptr;
    d.indexed = true;
    d.streams = {};
    d.samplers = {};
    d.samplerMask = d.inheritMask = 0;
    d.vsLive.reset();
    d.psLive.reset();
    d.vsSrc.fill(sc::Src::None);
    d.psSrc.fill(sc::Src::None);
    d.vsSet = {};
    d.psSet = {};
    d.passFlags = 0;
    d.zPass = false;
    d.tagWater = 0;
    d.tagMaterialName = nullptr;
    d.drawClass = sc::DrawClass::Base;
    d.vertexStride = 0;
    d.vertexBytes.clear();
    d.indexBytes.clear();
    d.cpuStream = 0;
    d.vsB = entryVsB_;
    d.psB = entryPsB_;
    std::memcpy(d.vsI, entryVsI_, sizeof(d.vsI));
    std::memcpy(d.psI, entryPsI_, sizeof(d.psI));
    d.biLive = 0;
    return d;
}

StaticOps::Mirror::ShaderPools* StaticOps::poolsOf(const void* shader) {
    auto [it, inserted] = mirror_.shaders.try_emplace(shader);
    Mirror::ShaderPools& p = it->second;
    if (inserted || p.epoch != mirrorEpoch_) {
        // (re-)read once per epoch: the engine writes these pools outside the slots (SetTime in eCShaderMaterialAdmin::Process,
        // property notifies); inside them only SetSwitch, which the composition applies to this copy per op
        p.epoch = mirrorEpoch_;
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

bool StaticOps::readEnginePools(float (*global)[4], float (*lighting)[4], std::array<const void*, 2>& lightmaps) const {
    U8* g = ptr(ptr(engine::sym().shaderDefaultGlobalConstants, 0), 0x38);
    U8* l = ptr(ptr(lightingPoolVar_, 0), 0x18);
    const bool ok = rd(g, 0, global, sizeof(float) * 4 * 201) && rd(l, 0, lighting, sizeof(float) * 4 * 24);
    lightmaps = {ptr(view_.admin, 0x2e0), ptr(view_.admin, 0x308)};
    return ok;
}

// an op / unit whose engine code we do not compose is about to run: the engine's pools before it (the registers it changes are
// merged into the mirror at its next use)
void StaticOps::markLeft() {
    if (!mirrorValid_ || leftPending_ || mirrorMode_ != 0 || view_.admin == nullptr) return;
    ++mergedLeft_;
    leftPending_ = readEnginePools(leftSnap_.global, leftSnap_.lighting, leftSnap_.lightmapSlot);
    if (!leftPending_) mirrorValid_ = false;  // unreadable: read whole at the next use
}

void StaticOps::initMirror() {
    nativeVs_ = 0xfffu | 1u << 30;  // c0..c11, c30 (the camera, fog)
    U8* admin = view_.admin;
    if (mirrorMode_ == 2) ++mirrorEpoch_;  // N5.x: every engine pool re-read at every op
    if (mirror_.shaders.size() > mirrorShaderCap_) mirror_.shaders.clear();  // (dead shaders' entries: bounded)
    // the global pool c31..c231, the lighting pool c14..c37 and the admin's lightmap slots: written by the slots only (world /
    // fade / instance data, bones, lighting, BindLightmaps), i.e. by our composition: read whole once (mode 1: once per view,
    // mode 2: per op) and after a reallocation
    const void* gsrc = ptr(ptr(engine::sym().shaderDefaultGlobalConstants, 0), 0x38);
    const void* lsrc = ptr(ptr(lightingPoolVar_, 0), 0x18);
    const bool reread = !mirrorValid_ || gsrc != mirrorGlobalSrc_ || lsrc != mirrorLightingSrc_ || (mirrorMode_ != 0 && adminEpoch_ != mirrorEpoch_);
    if (reread) {
        mirrorValid_ = readEnginePools(mirror_.global, mirror_.lighting, mirror_.lightmapSlot);
        mirrorGlobalSrc_ = gsrc;
        mirrorLightingSrc_ = lsrc;
        leftPending_ = false;
    } else if (leftPending_) {
        // merge what the engine code we left to it changed (register by register against the snapshot before it)
        leftPending_ = false;
        EngineSnap& now = leftNow_;
        if (readEnginePools(now.global, now.lighting, now.lightmapSlot)) {
            for (std::uint32_t r = 0; r < 201; ++r) {
                if (std::memcmp(now.global[r], leftSnap_.global[r], 16) != 0) {
                    std::memcpy(mirror_.global[r], now.global[r], 16);
                    ++mergedRegs_;
                }
            }
            for (std::uint32_t r = 0; r < 24; ++r) {
                if (std::memcmp(now.lighting[r], leftSnap_.lighting[r], 16) != 0) {
                    std::memcpy(mirror_.lighting[r], now.lighting[r], 16);
                    ++mergedRegs_;
                }
            }
            for (std::size_t i = 0; i < 2; ++i) {
                if (now.lightmapSlot[i] != leftSnap_.lightmapSlot[i]) mirror_.lightmapSlot[i] = now.lightmapSlot[i];
            }
        } else {
            mirrorValid_ = readEnginePools(mirror_.global, mirror_.lighting, mirror_.lightmapSlot);
        }
    }
    // the admin VS / PS pools: written outside the slots (the brackets: camera, fog, obstacle distances; the database's Request:
    // the SpeedTree wind; Process: the vegetation wind): re-read once per view; of the slot-written registers the mask reference
    // (PS c5.w: ExecuteSpecialPass, the masked passes) stays ours
    if (adminEpoch_ != mirrorEpoch_) {
        const float maskRef = mirror_.adminPs[5][3];
        const bool keepMask = mirrorMode_ == 0 && adminEpoch_ != 0;
        rd(ptr(admin, 0x240), 0, mirror_.adminVs, sizeof(mirror_.adminVs));
        rd(ptr(admin, 0x220), 0, mirror_.adminPs, sizeof(mirror_.adminPs));
        if (keepMask) mirror_.adminPs[5][3] = maskRef;
        adminEpoch_ = mirrorEpoch_;
    }
    // the per-view constants, composed natively (the admin pools hold the engine's copies)
    for (std::uint32_t k = 0; k < 4; ++k) std::memcpy(mirror_.adminVs[4 + k], view_.invProj[k], 16);
    for (std::uint32_t k = 0; k < 3; ++k) std::memcpy(mirror_.adminVs[8 + k], view_.invView[k], 12);
    std::memcpy(mirror_.adminVs[11], view_.camPos, 16);
    if (view_.fog) std::memcpy(mirror_.adminVs[30], view_.fogC4, 16);
    std::memcpy(mirror_.adminPs[0], view_.camPos, 16);
    std::int32_t w = 0, h = 0;
    {
        const d3d9::Viewport& vp = dev_.viewport();
        w = static_cast<std::int32_t>(vp.width);
        h = static_cast<std::int32_t>(vp.height);
    }
    sc::viewToScreen(w, h, mirror_.adminPs[1]);
    if (view_.fog) {
        std::memcpy(mirror_.adminPs[3], view_.fogC3, 16);
        std::memcpy(mirror_.adminPs[4], view_.fogC4, 16);
    }
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

void StaticOps::writeTreeInstance(std::uint32_t slot, const float v[2]) {
    const std::uint32_t r = 32 + 4 * slot - 31;
    if (r >= 201) return;
    mirror_.global[r][0] = v[0];
    mirror_.global[r][1] = v[1];
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
    if (leanCollect_ && res != nullptr) leanLightmaps_.push_back(res);  // O2a: the engine's BindLightmaps(1) at the lean commit
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
    // eCSubMesh::Render / RenderZPass / ExecuteRenderInstances: an indexed triangle list, base vertex 0 (RenderZPass sets it 0;
    // nothing else of the static path writes it); a skinned primitive: the record's (RenderPrimitive writes the ranges only)
    d.primType = g.primType;
    d.baseVertex = g.baseVertex;
    d.minIndex = g.minIndex;
    d.numVertices = g.numVertices;
    d.startIndex = g.startIndex;
    d.primCount = g.primCount;
    d.subMesh = g.subMesh;
    d.material = g.material;
    d.tagSubMesh = d.tagMaterial = d.tagMeshResource = nullptr;
    return d;
}

// every live register of the draw's shaders from the mirror (+ the shader's own pools)
void StaticOps::constantsInto(sc::DrawDesc& d, const std::uint8_t* shader, const std::uint8_t* biasShader, bool defaultClass) {
    const auto* vs = static_cast<const d3d9::Shader*>(d.vs);
    const auto* ps = static_cast<const d3d9::Shader*>(d.ps);
    const Live& lv = liveOf(vs);
    const Live& lp = liveOf(ps);
    d.vsLive = lv.f;
    d.psLive = lp.f;
    // c0..c3: the view-projection, or the depth-bias one for UseDepthBias materials (ExecutePass / ExecuteSpecialPass)
    // (ExecutePass reads the base shader's flag, also for an overlay's passes)
    const bool depthBias = get<std::uint8_t>(biasShader != nullptr ? biasShader : shader, 0x78) != 0;
    std::memcpy(mirror_.adminVs[0], depthBias ? view_.dbvp : view_.vp, 64);
    const Mirror::ShaderPools* pools = poolsOf(shader);
    // O1: the live registers by source region (masks), copied as runs; the sources (diagnostics of the compare) only with it
    const bool trackSrc = g_ != nullptr && g_->compare;
    const auto take = [&](const sc::RegMask& m, const float (*src)[4], std::uint32_t srcFirst, float (*dst)[4], sc::RegMask& set) {
        sc::forEachRun(m, [&](std::uint32_t r, std::uint32_t n) { std::memcpy(dst[r], src[r - srcFirst], 16 * n); });
        sc::orInto(set, m);
    };
    const auto mark = [](const sc::RegMask& m, sc::Src* out, auto&& srcOf) {
        for (std::uint32_t w = 0; w < 4; ++w) {
            for (std::uint64_t bits = m[w]; bits != 0; bits &= bits - 1) {
                const std::uint32_t r = w * 64 + sc::lowBit(bits);
                out[r] = srcOf(r);
            }
        }
    };
    static const sc::RegMask kAdminVs = sc::regRange(0, 31), kGlobal = sc::regRange(31, 201), kAdminPs = sc::regRange(0, 6),
                             kLighting = sc::regRange(14, 24);
    const sc::RegMask vsPool = sc::andMask(lv.words, sc::regRange(pools->vsFirst, pools->vsCount));
    const auto adminSrc = [&](std::uint32_t r) { return (nativeVs_ >> r & 1u) != 0 ? sc::Src::Native : sc::Src::Pool; };
    if (defaultClass) {
        // eCShaderDefault: admin c0..c30, the global pool c31..c231, the shader's pools from c232
        const sc::RegMask admin = sc::andMask(lv.words, kAdminVs), global = sc::andMask(lv.words, kGlobal), pool = sc::andNot(vsPool, sc::regRange(0, 232));
        take(admin, mirror_.adminVs, 0, d.vsF, d.vsSet);
        take(global, mirror_.global, 31, d.vsF, d.vsSet);
        take(pool, pools->vs, pools->vsFirst, d.vsF, d.vsSet);
        if (trackSrc) {
            mark(admin, d.vsSrc.data(), adminSrc);
            // the instances' world rows and fade are native (.xyz of c32 + 4i: carry-over)
            mark(global, d.vsSrc.data(), [](std::uint32_t r) { return r >= 32 ? sc::Src::Native : sc::Src::Pool; });
            mark(pool, d.vsSrc.data(), [](std::uint32_t) { return sc::Src::Pool; });
        }
    } else {
        // eCShaderBase: the shader's pools where they are, admin c0..c30, a class's own global pool (eCShaderParticle: c31..c60,
        // world c31..c33 and alpha c34.w composed natively, flushed before the shader's); GetGlobalShaderConstants is null
        const sc::RegMask admin = sc::andNot(sc::andMask(lv.words, kAdminVs), vsPool);
        const sc::RegMask fx = fxPoolCount_ != 0 ? sc::andNot(sc::andNot(sc::andMask(lv.words, sc::regRange(fxPoolFirst_, fxPoolCount_)), vsPool), kAdminVs) : sc::RegMask{};
        take(vsPool, pools->vs, pools->vsFirst, d.vsF, d.vsSet);
        take(admin, mirror_.adminVs, 0, d.vsF, d.vsSet);
        if (fxPoolCount_ != 0) take(fx, fxPool_, fxPoolFirst_, d.vsF, d.vsSet);
        if (trackSrc) {
            mark(vsPool, d.vsSrc.data(), [](std::uint32_t) { return sc::Src::Pool; });
            mark(admin, d.vsSrc.data(), adminSrc);
            mark(fx, d.vsSrc.data(), [&](std::uint32_t r) { return r <= fxPoolNativeLast_ ? sc::Src::Native : sc::Src::Pool; });
        }
    }
    // PS: admin c0..c5 (c2 not native), the shader's pools (eCShaderDefault: within c6..c13), the lighting pool c14..c37
    const sc::RegMask psLive = sc::andMask(lp.words, sc::regRange(0, sc::kPsRegs));
    const sc::RegMask psAdmin = sc::andMask(psLive, kAdminPs);
    const sc::RegMask psPool = sc::andMask(sc::andNot(sc::andMask(psLive, sc::regRange(pools->psFirst, pools->psCount)), kAdminPs),
                                           defaultClass ? sc::regRange(6, 8) : sc::regRange(6, sc::kPsRegs - 6));
    const sc::RegMask psLight = defaultClass ? sc::andMask(psLive, kLighting) : sc::RegMask{};
    take(psAdmin, mirror_.adminPs, 0, d.psF, d.psSet);
    take(psPool, pools->ps, pools->psFirst, d.psF, d.psSet);
    take(psLight, mirror_.lighting, 14, d.psF, d.psSet);
    if (trackSrc) {
        mark(psAdmin, d.psSrc.data(), [](std::uint32_t r) { return r != 2 ? sc::Src::Native : sc::Src::Pool; });
        mark(psPool, d.psSrc.data(), [](std::uint32_t) { return sc::Src::Pool; });
        mark(psLight, d.psSrc.data(), [](std::uint32_t) { return sc::Src::Native; });
    }
}

const StaticOps::SamplerSnap& StaticOps::samplerSnap() {
    if (snap_.serial != opSerial_ || snap_.admin != view_.admin) {
        snap_.serial = opSerial_;
        snap_.admin = view_.admin;
        if (view_.admin == nullptr || !engine::safeReadN<sizeof(snap_.block)>(view_.admin + 0x248, snap_.block)) std::memset(snap_.block, 0, sizeof(snap_.block));
        if (imageOverride_ == nullptr || !engine::safeReadN<sizeof(snap_.overrides)>(imageOverride_, snap_.overrides.data())) snap_.overrides = {};
        snap_.defFilter = get<std::uint32_t>(view_.gfx, 0x640);
    }
    return snap_;
}

// eCShaderBase::ExecuteShaderConstants' material samplers (pool slot k -> stage base + k when the PS uses it)
StaticOps::Skip StaticOps::materialSamplers(sc::DrawDesc& d, const std::uint8_t* shader, const void* psElement) {
    const std::uint16_t mask = get<std::uint16_t>(psElement, 0x38);
    const std::uint32_t base = get<std::uint8_t>(shader, 0x219);
    const Mirror::ShaderPools* pools = poolsOf(shader);
    const SamplerSnap& sn = samplerSnap();
    const std::uint32_t defFilter = sn.defFilter;
    for (std::uint32_t k = 0; k < pools->slots; ++k) {
        const std::uint32_t stage = base + k;
        if (stage >= sc::kStages || (mask >> stage & 1u) == 0) continue;
        // the slot's 0x28 bytes at once (unreadable: zeros, as the field reads' fallbacks)
        std::uint8_t slot[0x28];
        if (!engine::safeReadN<sizeof(slot)>(shader + 0xf8 + 0x28 * k, slot)) std::memset(slot, 0, sizeof(slot));
        const auto field = [&](std::uint32_t off) {
            std::uint32_t v;
            std::memcpy(&v, slot + off, 4);
            return v;
        };
        U8* res = static_cast<U8*>(pools->slotImage[k]);
        U8* data = nullptr;
        if (res != nullptr) {
            data = ptr(res, 0x70);
            leanImage(res, data);  // O2a: ExecuteShaderConstants' touch of the slot's image (+ the video / placeholder checks)
            if (data == nullptr) {
                U8* ph = ptr(placeholderTable_, 8 * stage);
                if (ph != nullptr && ptr(ph, 0x60) != nullptr) data = ph;
            }
        }
        if (data == nullptr) data = sn.at<U8*>(0x348);  // the ErrorImage
        if (sn.overrides[stage] != nullptr) return Skip::ImageOverride;
        bool ok = false;
        sc::DrawDesc::Sampler& s = d.samplers[stage];
        s.texture = textureOfImageData(data, ok);
        if (!ok) d.inheritMask |= static_cast<std::uint16_t>(1u << stage);
        const std::uint32_t raw[6] = {field(0), field(4), field(0xc), field(8), field(0x10), field(0x18)};
        for (std::uint32_t i = 0; i < 6; ++i) s.ss[i] = raw[i] == kNoState ? defFilter : raw[i];
        s.ss[6] = static_cast<std::uint32_t>(static_cast<std::int32_t>(static_cast<std::int8_t>(slot[0x14])));
    }
    return Skip::None;
}

// ---- composition ------------------------------------------------------------------------------------------------------------

std::uint8_t StaticOps::maskRefOf(const std::uint8_t* owner) const {
    return owner == maskRefShader_ ? maskRefOverride_ : get<std::uint8_t>(owner, 0x58);
}

StaticOps::Skip StaticOps::specialPassDraw(const std::uint8_t* shader, const Geom& geometry, bool option) {
    U8* admin = view_.admin;
    // ExecuteSpecialPass 0x6d1cd0: the mask reference first (PS c5.w), then the version's special pass
    if (get<std::int32_t>(shader, 0x50) == 1) mirror_.adminPs[5][3] = static_cast<float>(maskRefOf(shader)) * (1.0f / 256.0f);
    const std::int32_t v = get<std::int32_t>(admin, 0x170);
    U8* version = ptr(shader, 0x258 + 8 * static_cast<std::size_t>(v));
    if (version == nullptr) return Skip::None;  // no draw
    if (get<const void*>(version, 0) != engine_ + kVersionDefault3Vtable) return Skip::Version;
    const std::int32_t type = get<std::int32_t>(admin, 0x314, 6);
    if (type == 6) return Skip::None;
    const std::size_t idx = static_cast<std::size_t>(type) * 2 + (option ? 1u : 0u);
    if (get<std::uint8_t>(version, 0x18 + idx) == 0) return Skip::None;
    U8* vsElem = ptr(version, 0x30 + 16 * idx);
    U8* psElem = ptr(version, 0xf0 + 16 * idx);
    if (vsElem == nullptr || psElem == nullptr) {
        skipWhy_ = std::format("special pass {} not compiled (vs {}, ps {}; flag {}, version {})", idx, static_cast<const void*>(vsElem),
                               static_cast<const void*>(psElem), get<std::uint8_t>(version, 0x18 + idx), static_cast<const void*>(version));
        pendingVersion_ = version;
        pendingIdx_ = idx;
        return Skip::NotCompiled;
    }
    d3d9::Shader* vs = shaderOfElement(vsElem);
    d3d9::Shader* ps = shaderOfElement(psElem);
    if (vs == nullptr || ps == nullptr) return Skip::None;
    sc::DrawDesc& d = drawFrom(geometry);
    const sc::Visual& vis = geometry.visual;
    d.zPass = true;
    d.drawClass = sc::DrawClass::ZPass;
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

StaticOps::Skip StaticOps::passDraw(const std::uint8_t* shader, const std::uint8_t* owner, const std::uint8_t* version, const sc::Pass& pass,
                                    const Geom& geometry, bool overlay) {
    U8* admin = view_.admin;
    Skip skip = Skip::None;
    applySwitch(shader, pass.switchValue, skip);
    if (skip != Skip::None) return skip;
    const std::uint32_t vsIdx = sc::vsIndex(pass.flags), psIdx = sc::psIndex(pass.flags);
    U8* vsElem = ptr(version, 0x1b0 + 16 * vsIdx);
    U8* psElem = ptr(version, 0x210 + 16 * psIdx);
    if (vsElem == nullptr || psElem == nullptr) {
        skipWhy_ = std::format("pass flags {:#x}: vs {} ({}) / ps {} ({}) not compiled", pass.flags, vsIdx, static_cast<const void*>(vsElem), psIdx,
                               static_cast<const void*>(psElem));
        return Skip::NotCompiled;
    }
    writeLighting(pass);
    sc::BlendInputs b;
    b.mode = blendModeOf(owner);
    b.maskRef = maskRefOf(owner);
    b.hasOpacity = get<std::uint8_t>(owner, 0x330) != 0;
    b.refracted = get<std::uint8_t>(owner, 0x420) != 0;
    b.tradeAlphaAsSolid = get<std::uint8_t>(owner, 0x79) != 0;
    const sc::RenderStates rs = sc::passStates(pass.flags, b, mirror_.global[1][3]);
    if (!rs.valid) return Skip::PassKind;
    if (rs.alphaTest == 2) mirror_.adminPs[5][3] = static_cast<float>(rs.alphaRef) * (1.0f / 256.0f);
    d3d9::Shader* vs = shaderOfElement(vsElem);
    d3d9::Shader* ps = shaderOfElement(psElem);
    if (vs == nullptr || ps == nullptr) {
        skipWhy_ = std::format("pass flags {:#x}: no translated shader for vs {} / ps {}", pass.flags, vsIdx, psIdx);
        return Skip::NotCompiled;
    }
    sc::DrawDesc& d = drawFrom(geometry);
    const sc::Visual& vis = geometry.visual;
    d.passFlags = pass.flags;
    d.drawClass = sc::passClass(pass.flags, overlay);
    if (sc::passNonInstanced(pass.flags) && geometry.subMesh != nullptr) {
        // eCSubMesh::Render's tag (engine/scene_hooks: TagScope(sub-mesh, mesh resource))
        d.tagSubMesh = geometry.subMesh;
        d.tagMaterial = geometry.material;
        d.tagMeshResource = geometry.meshResource;
        // ... and the water classification eCSubMesh::Render's tag gets (the Refracted / Alpha passes)
        d3d9::draws::Water water = d3d9::draws::Water::None;
        engine::scene::subMeshWater(geometry.material, geometry.meshResource, water, d.tagMaterialName);
        d.tagWater = static_cast<std::uint8_t>(water);
    }
    d.vs = vs;
    d.ps = ps;
    d.states = sc::deviceStates(rs, vis, get<std::uint32_t>(admin, 0x340));
    constantsInto(d, owner, shader);
    if (const Skip s = materialSamplers(d, owner, psElem); s != Skip::None) return s;
    return adminSamplers(d, owner, ps);
}

// the admin's samplers bound by BeginMaterial (s0 mask, s1 refraction, s2 depth map) and BindLightmaps (s4, s5); stages the PS
// samples beyond those and the material's are inherited
StaticOps::Skip StaticOps::adminSamplers(sc::DrawDesc& d, const std::uint8_t* owner, const d3d9::Shader* ps) {
    const SamplerSnap& sn = samplerSnap();
    struct AdminSlot {
        std::uint32_t stage, slot;
        const void* image;
    };
    const AdminSlot slots[] = {{0, 0x298, sn.at<const void*>(0x2b8)}, {1, 0x248, sn.at<const void*>(0x268)}, {2, 0x270, sn.at<const void*>(0x290)},
                               {4, 0x2c0, mirror_.lightmapSlot[0]}, {5, 0x2e8, mirror_.lightmapSlot[1]}};
    const std::uint32_t defFilter = sn.defFilter;
    for (const AdminSlot& a : slots) {
        const void* data = a.image != nullptr ? a.image : sn.at<const void*>(0x348);
        if (sn.overrides[a.stage] != nullptr) return Skip::ImageOverride;
        bool ok = false;
        sc::DrawDesc::Sampler& s = d.samplers[a.stage];
        s.texture = adminStagesUnbound_ ? nullptr : textureOfImageData(data, ok);
        if (!ok && !adminStagesUnbound_) d.inheritMask |= static_cast<std::uint16_t>(1u << a.stage);
        const std::uint32_t raw[6] = {sn.at<std::uint32_t>(a.slot), sn.at<std::uint32_t>(a.slot + 4), sn.at<std::uint32_t>(a.slot + 0xc),
                                      sn.at<std::uint32_t>(a.slot + 8), sn.at<std::uint32_t>(a.slot + 0x10), sn.at<std::uint32_t>(a.slot + 0x18)};
        for (std::uint32_t i = 0; i < 6; ++i) s.ss[i] = raw[i] == kNoState ? defFilter : raw[i];
        s.ss[6] = static_cast<std::uint32_t>(static_cast<std::int32_t>(sn.at<std::int8_t>(a.slot + 0x14)));
    }
    const std::uint32_t base = get<std::uint8_t>(owner, 0x219);
    for (std::uint32_t st = 0; st < sc::kStages; ++st) {
        if (ps->reflection.samplers[st] == shader::TextureType::Unknown) continue;
        d.samplerMask |= static_cast<std::uint16_t>(1u << st);
        const bool material = st >= base && st < base + poolsOf(owner)->slots;
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
    leanTouch(material);  // O2a: eCSubMesh::Render's QueueData / CacheResource of the material (also done at request time)
    if (!shaderClassOk(shader)) return Skip::ShaderClass;
    U8* admin = ptr(shader, 0x220);
    if (admin != view_.admin) return Skip::Unreadable;
    if (!instanced && world != nullptr) {
        // eCSubMesh::Render: SetGlobalAlpha(*fade, 0) / SetWorldMatrix(world, 0) when given (the tree billboards pass neither)
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
    geometry.meshResource = res;
    return materialDraws(shader, illum, combi, instanced, switchValue, flags, geometry, world);
}

// GetPasses 0x6d1ee0 -> OnGetPasses 0x6d3170 -> eCShaderVersionDefault_3_0::GetPasses 0x6cc970, ExecutePass per pass
StaticOps::Skip StaticOps::materialDraws(const std::uint8_t* shader, const std::uint8_t* illum, std::uint32_t combi, bool instanced,
                                         std::int32_t switchValue, std::uint8_t flags, const Geom& geometry, const float* world) {
    U8* admin = view_.admin;
    if (get<std::uint8_t>(shader, 0x2a8) == 1) return Skip::InvalidShader;
    if (get<std::uint32_t>(admin, 0x31c) != 0) return Skip::InvalidShader;
    if (flags != 0 && !overlays_) return Skip::Overlay;
    sc::PassInputs in;
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
    if (g_ != nullptr) {
        const std::uint32_t total = in.hasList ? std::min<std::uint32_t>(in.dynamicLights + in.staticLights, 4u) : 0u;
        ++g_->lights[total];
        if (total == 4 && fourLightsLogged_ < 16 && world != nullptr) {
            ++fourLightsLogged_;
            spdlog::info("native: lights: a sub-mesh with {} dynamic + {} static lights (an additive light pass) at ({:.0f}, {:.0f}, {:.0f}), "
                         "material '{}'", in.dynamicLights, in.staticLights, world[12], world[13], world[14],
                         geometry.material != nullptr ? engine::readCString(static_cast<U8*>(geometry.material) + 0x40) : std::string());
        }
    }
    // the base shader's passes, then (OnGetPasses, flags 1 burned +0x88, 2 frozen +0x80, 4 highlighted +0x90) each overlay
    // material's shader's own OnGetPasses with the same switch, illumination, combi and instancing, flags 0
    struct Owner {
        const std::uint8_t* shader;
        const std::uint8_t* version;
        std::size_t first, count;
        bool overlay;
    };
    std::array<Owner, 4> owners{};
    std::size_t ownerCount = 0;
    std::vector<sc::Pass>& passes = scr_.passes;
    passes.clear();
    {
        const std::uint8_t* version = nullptr;
        const std::size_t before = passes.size();
        if (const Skip k = shaderPasses(shader, in, version, passes); k != Skip::None) return k;
        if (version == nullptr) return Skip::None;
        owners[ownerCount++] = {shader, version, before, passes.size() - before, false};
    }
    static constexpr std::pair<std::uint8_t, std::uint32_t> kOverlays[] = {{1, 0x88}, {2, 0x80}, {4, 0x90}};
    for (const auto& [bit, off] : kOverlays) {
        if ((flags & bit) == 0) continue;
        U8* ovRes = ptr(shader, off);
        leanTouch(ovRes);  // O2a: OnGetPasses' RequestData(overlay, 1), every frame while the flag is set
        U8* ov = ptr(ovRes, 0x70);  // RequestData(res, 1) first (the engine's; it does not wait), then the shader if there is one
        if (ovRes == nullptr || ov == nullptr) continue;
        if (!shaderClassOk(ov)) return Skip::OverlayShader;
        if (ptr(ov, 0x220) != view_.admin) return Skip::Unreadable;
        const std::uint8_t* version = nullptr;
        const std::size_t before = passes.size();
        if (const Skip k = shaderPasses(ov, in, version, passes); k != Skip::None) return k;
        if (version != nullptr) owners[ownerCount++] = {ov, version, before, passes.size() - before, true};
    }
    for (std::size_t o = 0; o < ownerCount; ++o) {
        const Owner& w = owners[o];
        for (std::size_t i = 0; i < w.count; ++i) {
            if (const Skip s = passDraw(shader, w.shader, w.version, passes[w.first + i], geometry, w.overlay); s != Skip::None) return s;
        }
    }
    return Skip::None;
}

// eCShaderBase::OnGetPasses 0x6d3170 for one shader: its version (min(admin +0x170, shader +0x68), falling back to a lower
// existing one) and eCShaderVersionDefault_3_0::GetPasses 0x6cc970 (the z-fill pass first for EnableAlphaZWrite blended
// materials); version = null: no passes
StaticOps::Skip StaticOps::shaderPasses(const std::uint8_t* owner, const sc::PassInputs& base, const std::uint8_t*& version,
                                        std::vector<sc::Pass>& out) {
    version = nullptr;
    std::int32_t v = std::min(get<std::int32_t>(view_.admin, 0x170), get<std::int32_t>(owner, 0x68));
    U8* ver = ptr(owner, 0x258 + 8 * static_cast<std::size_t>(std::max(v, 0)));
    while (ver == nullptr && v > 0) ver = ptr(owner, 0x258 + 8 * static_cast<std::size_t>(--v));
    if (ver == nullptr) return Skip::None;
    if (get<const void*>(ver, 0) != engine_ + kVersionDefault3Vtable) return Skip::Version;
    sc::BlendInputs b;
    b.mode = blendModeOf(owner);
    b.hasOpacity = get<std::uint8_t>(owner, 0x330) != 0;
    b.refracted = get<std::uint8_t>(owner, 0x420) != 0;
    b.tradeAlphaAsSolid = get<std::uint8_t>(owner, 0x79) != 0;
    sc::PassInputs in = base;
    in.zFill = get<std::uint8_t>(owner, 0x2d8) != 0 && sc::isBlended(b);
    const std::size_t before = out.size();
    sc::buildPasses(in, out);
    // TransformationType 11 (SpeedTree billboards, GetPasses 0x6cc970's end): every pass of the shared array becomes VS variant 1
    // and is appended again as variant 2 (the array: only this shader's passes when it is the base shader)
    if (get<std::int32_t>(owner, 0x2b8) == 11) {
        if (before != 0) return Skip::Overlay;  // an overlay material of the billboards' class: the array holds the base's passes too
        sc::billboardPassPairs(out);
    }
    version = ver;
    return Skip::None;
}

StaticOps::Skip StaticOps::composeZ(const Op& op) {
    U8* ctx = static_cast<U8*>(op.raw.context);
    U8* res = ptr(ctx, 0x28);
    U8* entity = ptr(ctx, 0x10);
    if (res == nullptr) return Skip::None;
    float world[16];
    if (!rd(entity, 0x60, world, 64)) return Skip::Unreadable;
    return meshZDraws(res, op.raw.desc & 0xffu, op.raw.desc >> 8, world, false);
}

StaticOps::Skip StaticOps::meshZDraws(const std::uint8_t* res, std::uint32_t sub, std::uint32_t node, const float* world, bool option) {
    if (get<std::int32_t>(res, 0x38) != 3 || ptr(res, 0x88) == nullptr) return Skip::None;
    Geom geometry;
    if (!geometryOf(res, geometry, 0)) return Skip::Primitive;
    leanStream3(res, false);  // O2a: RenderZPass' EnableStream(3, false) before the sub-mesh loop
    U8* subs = ptr(res, 0x58);
    const std::int32_t count = get<std::int32_t>(res, 0x60);
    const auto shaderOfSub = [&](std::uint32_t i, bool needCached) -> U8* {
        U8* m = ptr(subs + 0x48 * static_cast<std::size_t>(i), 0x38);
        if (m == nullptr || (needCached && get<std::int32_t>(m, 0x38) != 3)) return nullptr;
        return ptr(m, 0x70);
    };
    if (sub == 0xff) {
        std::vector<sc::ZSub>& zs = scr_.zs;
        zs.assign(static_cast<std::size_t>(std::max(count, 0)), sc::ZSub{});
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
        if (world != nullptr) {
            writeWorld(0, world);
            writeAlpha(0, 1.0f);
        }
        std::vector<sc::ZDraw>& plan = scr_.zplan;
        plan.clear();
        sc::planWholeMeshZ(zs, plan);
        for (const sc::ZDraw& z : plan) {
            Geom g = geometry;
            g.minIndex = z.minIndex;
            g.numVertices = z.numVertices;
            g.startIndex = z.startIndex;
            g.primCount = z.primCount;
            g.subMesh = subs + 0x48 * static_cast<std::size_t>(z.shaderOf);
            g.material = ptr(g.subMesh, 0x38);
            if (const Skip s = specialPassDraw(shaderOfSub(z.shaderOf, true), g, option); s != Skip::None) return s;
        }
        return Skip::None;
    }
    if (static_cast<std::int32_t>(sub) >= count) return Skip::Unreadable;
    U8* s = subs + 0x48 * static_cast<std::size_t>(sub);
    U8* shader = shaderOfSub(sub, true);
    if (shader == nullptr) return Skip::None;
    if (!shaderClassOk(shader)) return Skip::ShaderClass;
    if (ptr(shader, 0x220) != view_.admin) return Skip::Unreadable;
    if (world != nullptr) {
        writeWorld(0, world);
        writeAlpha(0, 1.0f);
    }
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
    return specialPassDraw(shader, g, option);
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
        leanSortInstances(res);  // O2a: Render's in-place sort + res +0xa4 = 0 (then the composition reads the sorted list)
        const std::int32_t n = get<std::int32_t>(res, 0xb8);
        if (n <= 0) return Skip::None;
        if (static_cast<std::int32_t>(sub) >= count) return Skip::Unreadable;
        const std::uint32_t combi = bindLightmaps(res);
        U8* s = subs + 0x48 * static_cast<std::size_t>(sub);
        U8* material = ptr(s, 0x38);
        U8* shader = ptr(material, 0x70);
        if (material == nullptr || shader == nullptr) return Skip::None;
        if (!shaderClassOk(shader)) return Skip::ShaderClass;
        std::vector<std::uint8_t>& inst = scr_.inst;
        inst.resize(static_cast<std::size_t>(n) * 0x58);
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
        leanStream3(res, true);  // O2a: the hardware path's EnableStream(3, true) before its batch loop
        leanTouch(material);     // (each batch flush touches the material: once per frame is the same for the cache)
        std::vector<sc::Instance>& list = scr_.list;
        list.assign(static_cast<std::size_t>(n), sc::Instance{});
        for (std::int32_t i = 0; i < n; ++i) {
            U8* il = illumOf(i);
            sc::Instance& x = list[static_cast<std::size_t>(i)];
            x.list = ptr(il, 0);
            x.switchValue = switchOf(i);
            x.flags = at(i)[0x50];
            x.f = get<float>(il, 8);
            x.alone = get<std::uint8_t>(il, 0xd) != 0 || get<std::uint8_t>(il, 0x90) != 0;
        }
        std::vector<sc::Batch>& batches = scr_.batches;
        batches.clear();
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
    if (op.kind == OpKind::Particle || op.kind == OpKind::Strip || op.kind == OpKind::LightStreaks || op.kind == OpKind::Precipitation) {
        return composeFx(op);
    }
    if (op.kind == OpKind::Animation) return composeActor(op);
    if (op.kind == OpKind::SpeedTree) return composeTree(op);
    if (op.kind == OpKind::SpeedTreeBB) return composeTreeBillboard(op);
    if (op.kind == OpKind::Vegetation) return composeVegetation(op);
    if (op.kind == OpKind::Billboard && op.list == List::Alpha) {
        const Skip s = composeBillboardAlpha(op);
        blendOverrideShader_ = nullptr;
        blendOverride_ = -1;
        return s;
    }
    if (op.kind != OpKind::Mesh || op.list == List::Debug || op.list == List::Sky || op.list == List::Late) return Skip::Kind;
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

std::string_view StaticOps::leftName(Group g, int code) {
    const int key = static_cast<int>(g) * 1000 + code;
    auto it = leftNames_.find(key);
    if (it != leftNames_.end()) return it->second;
    const char* what = code == kLeftCompareOnly ? "composed, compare only"
                       : code == kLeftGroupOff  ? "group off"
                       : code == kLeftNotCovered ? "kind / list not covered"
                       : code == kLeftChecks     ? "engine checks failed / outside a view"
                       : code >= 0 && code < static_cast<int>(Skip::Count) ? name(static_cast<Skip>(code)) : "?";
    return leftNames_.emplace(key, std::format("{}: {}", g == Group::Count ? "static ops" : name(g), what)).first->second;
}

std::string_view StaticOps::leftReason(const Op* op) const {
    if (op == nullptr) return leftNonOp_ ? leftNonOpWhy_ : std::string_view();
    return op == leftOp_ ? leftWhy_ : std::string_view();
}

bool StaticOps::opEnter(const Op& op) {
    leftGroup_ = Group::Count;
    leftCode_ = kLeftChecks;
    opEnterImpl(op);
    leanAbort();
    leftOp_ = &op;
    if (op_ != &op) markLeft();
    if (op_ == &op && taken_) {
        leftWhy_ = {};
    } else {
        if (op_ == &op) leftCode_ = kLeftCompareOnly;
        if (leftCode_ == kLeftNotCovered) {
            // the op kind / list no group composes: named by both
            const int key = 1000000 + static_cast<int>(op.kind) * 100 + static_cast<int>(op.list);
            auto it = leftNames_.find(key);
            if (it == leftNames_.end()) {
                it = leftNames_.emplace(key, std::format("no group composes {} ops of the {} list", native::name(op.kind), native::name(op.list))).first;
            }
            leftWhy_ = it->second;
        } else {
            leftWhy_ = leftName(leftGroup_, leftCode_);
        }
    }
    return op_ == &op && slotSkipped_;
}

void StaticOps::opEnterImpl(const Op& op) {
    finishOp();
    if (pendingVersion_ != nullptr) {
        // diagnostics: a special pass found uncompiled at the last op's entry, after the engine's slot ran
        if (pendingLogged_ < 12) {
            ++pendingLogged_;
            spdlog::info("native: special pass {} of version {} after the engine's slot (frame {}): vs {} ps {} flag {}", pendingIdx_,
                         static_cast<const void*>(pendingVersion_), dev_.stats().frames,
                         static_cast<const void*>(ptr(pendingVersion_, 0x30 + 16 * pendingIdx_)), static_cast<const void*>(ptr(pendingVersion_, 0xf0 + 16 * pendingIdx_)),
                         get<std::uint8_t>(pendingVersion_, 0x18 + pendingIdx_));
        }
        pendingVersion_ = nullptr;
    }
    if (!active() || !inView_) return;
    if (verified_ == 0) verified_ = verify() ? 1 : -1;
    if (verified_ < 0) return;
    const bool treeKind = op.kind == OpKind::SpeedTree || op.kind == OpKind::SpeedTreeBB || op.kind == OpKind::Vegetation;
    Group group = Group::Count;
    if (viewKind_ != ViewKind::Main) {
        // N5.5: a shadow map's z ops, whatever their kind (the main view's groups never act in a shadow view)
        if (op.list != List::ZPass || !groups_[static_cast<std::size_t>(Group::ShadowViews)].active()) {
            leftGroup_ = Group::ShadowViews;
            leftCode_ = kLeftGroupOff;
            return;
        }
        if (op.kind != OpKind::Mesh && op.kind != OpKind::Animation && !treeKind) {
            leftCode_ = kLeftNotCovered;
            return;
        }
        if (op.kind == OpKind::Animation) {
            if (actorsVerified_ == 0) actorsVerified_ = verifyActors() ? 1 : -1;
            if (actorsVerified_ < 0) return;
        }
        if (treeKind) {
            if (treesVerified_ == 0) treesVerified_ = verifyTrees() ? 1 : -1;
            if (treesVerified_ < 0) return;
        }
        group = Group::ShadowViews;
        ++shadowWindow_.ops[static_cast<std::size_t>(viewKind_)][static_cast<std::size_t>(op.kind)];
    } else if (op.kind == OpKind::Skydome && op.list == List::Sky) {
        GroupState& gs = groups_[static_cast<std::size_t>(Group::Sky)];
        leftGroup_ = Group::Sky;
        leftCode_ = kLeftGroupOff;
        if (!gs.active()) return;
        leftCode_ = kLeftChecks;
        if (skyVerified_ == 0) skyVerified_ = verifySky() ? 1 : -1;
        if (skyVerified_ < 0) return;
        g_ = &gs;
        drawCount_ = 0;
        ++g_->window.ops;
        std::memcpy(entryVsB_.data(), dev_.vsBoolConstants(), sizeof(entryVsB_));
        std::memcpy(entryPsB_.data(), dev_.psBoolConstants(), sizeof(entryPsB_));
        std::memcpy(entryVsI_, dev_.vsIntConstants(), sizeof(entryVsI_));
        std::memcpy(entryPsI_, dev_.psIntConstants(), sizeof(entryPsI_));
        leanBegin(Group::Sky);
        const Skip s = composeSky(op);
        blendOverrideShader_ = nullptr;
        blendOverride_ = -1;
        leftCode_ = static_cast<int>(s);
        if (s != Skip::None) {
            leanAbort();
            ++g_->skips[static_cast<std::size_t>(s)];
            ++g_->window.opsNotComposed;
            drawCount_ = 0;
            g_ = nullptr;
            return;
        }
        ++g_->window.opsComposed;
        op_ = &op;
        engineDraws_ = 0;
        opDiffLogged_ = false;
        taken_ = false;
        if (g_->takeOver) {
            // eCSkydome_PS::Render's draw tag (engine/scene_hooks: TagScope(Sky, the property set, entity +0x10), the op kept)
            d3d9::draws::Tag& tag = dev_.drawTag();
            const d3d9::draws::Tag saved = tag;
            const void* sky = ptr(op.raw.context, 0x18);
            const void* op0 = tag.op;
            tag = {};  // opDesc 0: the scope is opened without an op of its own
            tag.source = d3d9::draws::Source::Sky;
            tag.context = sky;
            tag.op = op0;
            tag.entity = ptr(sky, 0x10);
            issue();
            tag = saved;
            taken_ = true;
        }
        if (taken_) leanCommit();
        return;
    }
    const bool fxKind = op.kind == OpKind::Particle || op.kind == OpKind::Strip || op.kind == OpKind::LightStreaks || op.kind == OpKind::Precipitation;
    if (group != Group::Count) {
        // (a shadow view's op: chosen above)
    } else if (fxKind) {
        // N5.6: the particle / strip / light streak ops of the alpha list, the precipitation op of the late list (+0x1548)
        if (op.kind == OpKind::Precipitation ? op.list == List::Late : op.list == List::Alpha) {
            group = op.kind == OpKind::Particle ? Group::Particles : op.kind == OpKind::Precipitation ? Group::Precipitation : Group::Strips;
        }
        if (group != Group::Count && groups_[static_cast<std::size_t>(group)].active()) {
            if (fxVerified_ == 0) fxVerified_ = verifyFx() ? 1 : -1;
            if (fxVerified_ < 0) return;
        }
    } else if (op.kind == OpKind::Billboard && op.list == List::Alpha) {
        // N5.8: a blended / fading billboard set's alpha op (RenderBillboard with alpha = true): beside the opaque queue (group 16)
        group = Group::Billboards;
        if (groups_[static_cast<std::size_t>(group)].active()) {
            if (billboardsVerified_ == 0) billboardsVerified_ = verifyBillboards() ? 1 : -1;
            if (billboardsVerified_ < 0) return;
        }
    } else if (op.kind != OpKind::Mesh && op.kind != OpKind::Animation && !treeKind) {
        leftCode_ = kLeftNotCovered;  // e.g. blended billboard ops of the alpha list, the debug list
        return;
    } else if (treeKind) {
        // N5.4: the SpeedTree / tree billboard / vegetation contexts' z (RenderZPass) and solid (Render) ops
        if (op.list == List::ZPass || op.list == List::Solid) group = op.kind == OpKind::Vegetation ? Group::Vegetation : Group::Trees;
        if (group != Group::Count && groups_[static_cast<std::size_t>(group)].active()) {
            if (treesVerified_ == 0) treesVerified_ = verifyTrees() ? 1 : -1;
            if (treesVerified_ < 0) return;
        }
    } else if (op.kind == OpKind::Animation) {
        // eCRenderContextAnimation2: Render / RenderAlpha / RenderZPass (slots 0 / 1 / 3); its RenderRefracted is empty
        if (op.list == List::ZPass || op.list == List::Solid || op.list == List::Alpha || op.list == List::Refracted) group = Group::Actors;
        if (group != Group::Count && groups_[static_cast<std::size_t>(group)].active()) {
            if (actorsVerified_ == 0) actorsVerified_ = verifyActors() ? 1 : -1;
            if (actorsVerified_ < 0) return;
        }
    } else if (op.list == List::ZPass || op.list == List::Solid) {
        group = Group::Static;
    } else if (op.list == List::Alpha || op.list == List::Refracted) {
        group = Group::Mesh;
    }
    leftGroup_ = group;
    leftCode_ = group == Group::Count ? kLeftNotCovered : kLeftGroupOff;
    if (group == Group::Count || !groups_[static_cast<std::size_t>(group)].active()) return;
    leftCode_ = kLeftChecks;
    g_ = &groups_[static_cast<std::size_t>(group)];
    drawCount_ = 0;
    ++g_->window.ops;
    // the bool / int register files as the op finds them (nothing of the op's path writes them)
    std::memcpy(entryVsB_.data(), dev_.vsBoolConstants(), sizeof(entryVsB_));
    std::memcpy(entryPsB_.data(), dev_.psBoolConstants(), sizeof(entryPsB_));
    std::memcpy(entryVsI_, dev_.vsIntConstants(), sizeof(entryVsI_));
    std::memcpy(entryPsI_, dev_.psIntConstants(), sizeof(entryPsI_));
    skipWhy_.clear();
    leanBegin(group);
    const Skip s = composeOp(op);
    maskRefShader_ = nullptr;
    leftCode_ = static_cast<int>(s);
    if (s != Skip::None) {
        leanAbort();
        ++g_->skips[static_cast<std::size_t>(s)];
        if (s == Skip::Simulated) {
            simulatedOp_ = &op;
            simulatedDrew_ = false;
            ++fxWindow_.simulatedOps;
        }
        if ((group == Group::Trees || group == Group::Vegetation || group == Group::ShadowViews) && treeSkipsLogged_ < 24) {
            ++treeSkipsLogged_;
            std::string mat;
            if (s == Skip::ShaderClass) skipWhy_ = std::format("shader class vtable RVA {:#x}", static_cast<const std::uint8_t*>(skipShader_) - engine_);
            spdlog::info("native: {}: left to the engine ({}) in frame {}: {}{}{}", name(group), name(s), dev_.stats().frames, describe(op),
                         skipWhy_.empty() ? "" : ": ", skipWhy_);
        }
        ++g_->window.opsNotComposed;
        drawCount_ = 0;
        g_ = nullptr;
        return;
    }
    ++g_->window.opsComposed;
    if (group == Group::ShadowViews) ++shadowWindow_.composed[static_cast<std::size_t>(viewKind_)][static_cast<std::size_t>(op.kind)];
    op_ = &op;
    engineDraws_ = 0;
    opDiffLogged_ = false;
    taken_ = false;
    if (g_->takeOver) {
        issue();
        taken_ = true;
    }
    if (taken_) leanCommit();
}

void StaticOps::finishOp() {
    ++opSerial_;
    tagOverride_ = false;
    const bool skipped = slotSkipped_;
    slotSkipped_ = false;
    leanCollect_ = false;
    if (op_ == nullptr) return;
    // (a lean op: the engine's code did not run, it made no draws to compare)
    if (!skipped && engineDraws_ != drawCount_ && comparing()) {
        ++g_->window.mismatches[static_cast<std::size_t>(sc::Field::DrawCount)];
        if (mismatchLogged_ < 24) {
            ++mismatchLogged_;
            spdlog::info("native: {}: mismatch: {} composed {} draws, the engine made {}", name(static_cast<Group>(g_ - groups_.data())),
                         describe(*op_), drawCount_, engineDraws_);
        }
    }
    op_ = nullptr;
    g_ = nullptr;
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
    out.tagSubMesh = t.tag != nullptr ? t.tag->subMesh : nullptr;
    out.tagMaterial = t.tag != nullptr ? t.tag->material : nullptr;
    out.tagMeshResource = t.tag != nullptr ? t.tag->meshResource : nullptr;
    out.tagWater = t.tag != nullptr ? static_cast<std::uint8_t>(t.tag->water) : 0;
    out.tagMaterialName = t.tag != nullptr ? t.tag->materialName : nullptr;
    std::memcpy(out.vsB.data(), d.vsBoolConstants(), sizeof(out.vsB));
    std::memcpy(out.psB.data(), d.psBoolConstants(), sizeof(out.psB));
    std::memcpy(out.vsI, d.vsIntConstants(), sizeof(out.vsI));
    std::memcpy(out.psI, d.psIntConstants(), sizeof(out.psI));
    out.indexed = t.indexed;
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
    out.biLive = lv.boolInt + lp.boolInt;
    out.vsLive = lv.f;
    out.psLive = lp.f;
    sc::forEachRun(lv.words, [&](std::uint32_t r, std::uint32_t n) { std::memcpy(out.vsF[r], d.vsConstants()[r], 16 * n); });
    sc::forEachRun(lp.words, [&](std::uint32_t r, std::uint32_t n) {
        if (r < sc::kPsRegs) std::memcpy(out.psF[r], d.psConstants()[r], 16 * std::min(n, sc::kPsRegs - r));
    });
}

void StaticOps::ownDraw(const d3d9::DrawTapInfo& t) {
    if (g_ == nullptr || !g_->compare || issueIndex_ >= drawCount_) return;
    capture(t, *scratch_);
    const sc::DrawDesc& composed = *draws_[issueIndex_];
    std::string why;
    const bool bytesOk = geometryBytesEqual(composed, *scratch_, why);
    // CPU-generated geometry is drawn from our own buffers (the same bytes at the same positions)
    const std::uint32_t cs = composed.cpuStream;
    if (!composed.vertexBytes.empty() && cs == 0 && scratch_->streams[0].buffer == ownVb_.get()) scratch_->streams[0].buffer = composed.streams[0].buffer;
    if (!composed.vertexBytes.empty() && cs != 0 && scratch_->streams[cs].buffer == ownInstVb_.get() && issueIndex_ < ownOffset_.size() &&
        scratch_->streams[cs].offset == ownOffset_[issueIndex_]) {
        scratch_->streams[cs].buffer = composed.streams[cs].buffer;
        scratch_->streams[cs].offset = composed.streams[cs].offset;
    }
    if (!composed.indexBytes.empty() && scratch_->indices == ownIb_.get()) scratch_->indices = composed.indices;
    sc::Diff diff = sc::compare(composed, *scratch_);
    if (!bytesOk) {
        diff.fields.set(static_cast<std::size_t>(sc::Field::GeometryData));
        if (diff.first.empty()) diff.first = why;
    }
    if (ownKind_.size() < drawCount_) ownKind_.resize(drawCount_);
    ownKind_[issueIndex_] = static_cast<std::uint8_t>(t.kind);
    ++g_->ownChecked;
    if (diff.fields.any()) {
        ++g_->ownMismatch;
        if (g_->ownMismatch <= 8) spdlog::info("native: {}: our own draw differs from its composition: {}", name(static_cast<Group>(g_ - groups_.data())), diff.first);
    }
}

bool StaticOps::engineDraw(const d3d9::DrawTapInfo& t, const Op* op) {
    if (op != nullptr && op == simulatedOp_ && !simulatedDrew_) {
        simulatedDrew_ = true;
        ++fxWindow_.simulatedDrew;
    }
    if (op == nullptr && inNonOp_) op = op_;  // a non-op path's draws (decals) carry no op
    if (op_ == nullptr || op != op_ || issuing_ || g_ == nullptr) return false;
    const std::uint32_t k = engineDraws_++;
    sc::CompareCounts& w = g_->window;
    if (comparing()) {
        ++w.draws;
        if (k >= drawCount_) {
            // counted once per op in finishOp
        } else {
            capture(t, *scratch_);
            sc::Diff diff = sc::compare(*draws_[k], *scratch_);
            if (std::string why; !geometryBytesEqual(*draws_[k], *scratch_, why)) {
                diff.fields.set(static_cast<std::size_t>(sc::Field::GeometryData));
                if (diff.first.empty()) diff.first = why;
            }
            // take-over: our own draw of this position must have been classified as the engine's (draws::Kind: the
            // replacers / observers / debug views decide by it)
            if (taken_ && k < ownKind_.size() && ownKind_[k] != static_cast<std::uint8_t>(t.kind)) {
                diff.fields.set(static_cast<std::size_t>(sc::Field::Tag));
                if (diff.first.empty()) {
                    diff.first = std::format("draw kind {} vs engine {}", d3d9::draws::name(static_cast<d3d9::draws::Kind>(ownKind_[k])),
                                             d3d9::draws::name(t.kind));
                }
            }
            const sc::DrawDesc& c = *draws_[k];
            if (hal::vulkan::logThisFrame() && frameLogged_ < 12) {
                ++frameLogged_;
                const auto* vs = static_cast<const d3d9::Shader*>(c.vs);
                const auto* ps = static_cast<const d3d9::Shader*>(c.ps);
                spdlog::info("native: {}: {} draw {} ({}): vs {:016x} ps {:016x}, {} vs + {} ps live registers, sampled stages {:#x} "
                             "(inherited {:#x}), {} prims from {}, states z {} zw {} blend {} {}/{} cw {:#x} cull {}; vs c33 ({}, {}, {}, {}), "
                             "ps c15 ({}, {}, {}, {}); {} registers / {} stages compared, {}",
                             name(static_cast<Group>(g_ - groups_.data())), describe(*op), k, sc::name(c.drawClass), vs != nullptr ? vs->hash : 0,
                             ps != nullptr ? ps->hash : 0, scratch_->vsLive.count(), scratch_->psLive.count(), scratch_->samplerMask, c.inheritMask,
                             c.primCount, c.startIndex, c.states[sc::StZFunc], c.states[sc::StZWrite], c.states[sc::StBlend], c.states[sc::StSrc],
                             c.states[sc::StDst], c.states[sc::StColorWrite], c.states[sc::StCull], c.vsF[33][0], c.vsF[33][1], c.vsF[33][2],
                             c.vsF[33][3], c.psF[15][0], c.psF[15][1], c.psF[15][2], c.psF[15][3], diff.regs, diff.stages,
                             diff.fields.none() ? "equal" : diff.first);
            }
            w.closeFloats += diff.closeFloats;
            w.regs += diff.regs;
            w.stages += diff.stages;
            w.biRegs += diff.biRegs;
            w.biLive += scratch_->biLive;
            const auto cls = static_cast<std::size_t>(c.drawClass);
            const auto list = static_cast<std::size_t>(op->list);
            ++w.classDraws[cls];
            if (list < w.listDraws.size()) ++w.listDraws[list];
            if (g_ == &groups_[static_cast<std::size_t>(Group::ShadowViews)]) {
                const auto v = static_cast<std::size_t>(viewKind_), kk = static_cast<std::size_t>(op->kind);
                ++shadowWindow_.draws[v][kk];
                if (diff.fields.none()) ++shadowWindow_.clean[v][kk];
            }
            if (diff.fields.none()) {
                ++w.cleanDraws;
                ++w.classClean[cls];
                if (list < w.listClean.size()) ++w.listClean[list];
            } else {
                for (std::size_t f = 0; f < sc::kFieldCount; ++f) {
                    if (diff.fields.test(f)) ++w.mismatches[f];
                }
                if (mismatchLogged_ < 24 || (hal::vulkan::logThisFrame() && !opDiffLogged_)) {
                    ++mismatchLogged_;
                    opDiffLogged_ = true;
                    std::string mat;
                    if (c.material != nullptr) mat = engine::readCString(static_cast<U8*>(c.material) + 0x40);
                    spdlog::info("native: {}: mismatch: {} draw {} ({}, pass flags {:#x}{}) material '{}': {}", name(static_cast<Group>(g_ - groups_.data())),
                                 describe(*op), k, sc::name(c.drawClass), c.passFlags, c.zPass ? ", z" : "", mat, diff.first);
                }
            }
        }
    }
    if (taken_) ++g_->droppedDraws;
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
    // save (O1: only what the op's draws change: their constant registers and sampled stages; shaders, declaration, streams,
    // indices and the ten owned render states are set by every draw)
    std::array<std::uint64_t, 4> vsU{}, psU{};
    std::uint32_t stagesU = 0;
    for (std::size_t k = 0; k < drawCount_; ++k) {
        const sc::DrawDesc& d = *draws_[k];
        for (std::size_t w = 0; w < 4; ++w) {
            vsU[w] |= d.vsSet[w];
            psU[w] |= d.psSet[w];
        }
        stagesU |= static_cast<std::uint32_t>(d.samplerMask & ~d.inheritMask);
    }
    const ShaderRef vs = dev_.vertexShaderRef(), ps = dev_.pixelShaderRef();
    VertexDecl* decl = dev_.vertexDecl();
    Buffer* ib = dev_.indexBuffer();
    std::array<Device::StreamSource, 4> streams{};
    for (std::uint32_t i = 0; i < 4; ++i) streams[i] = dev_.streamSource(i);
    std::array<std::uint32_t, sc::StOwnedCount> rs{};
    for (std::uint32_t i = 0; i < sc::StOwnedCount; ++i) rs[i] = dev_.renderState(kRs[i]);
    std::array<Texture*, sc::kStages> tex{};
    std::array<std::array<std::uint32_t, 7>, sc::kStages> ss{};
    for (std::uint32_t m = stagesU; m != 0; m &= m - 1) {
        const auto st = sc::lowBit(m);
        tex[st] = dev_.stageTexture(st);
        const std::uint32_t* row = dev_.samplerStateRow(st);
        for (std::uint32_t i = 0; i < 7; ++i) ss[st][i] = row[kSs[i]];
    }
    const float (*devVs)[4] = dev_.vsConstants();
    const float (*devPs)[4] = dev_.psConstants();
    sc::forEachRun(vsU, [&](std::uint32_t r, std::uint32_t n) { std::memcpy(savedVs_[r], devVs[r], 16 * n); });
    sc::forEachRun(psU, [&](std::uint32_t r, std::uint32_t n) { std::memcpy(savedPs_[r], devPs[r], 16 * n); });
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
        if (!d.vertexBytes.empty() && d.cpuStream != 0) {
            // the vegetation's instance data (stream 1): appended to our own instance buffer within the frame (NOOVERWRITE: one
            // ring copy per frame, patched), DISCARD at the frame's first write or when full; the stream points at our offset
            if (!ownInstVb_) ownInstVb_ = dev_.createBuffer(kOwnInstBytes, USAGE_DYNAMIC | USAGE_WRITEONLY, POOL_DEFAULT, false, false);
            const auto size = static_cast<std::uint32_t>(d.vertexBytes.size());
            const std::uint64_t frame = dev_.stats().frames;
            std::uint32_t flags = LOCK_NOOVERWRITE;
            if (frame != ownInstFrame_ || ownInstCursor_ + size > ownInstVb_->size()) {
                flags = LOCK_DISCARD;
                ownInstCursor_ = 0;
                ownInstFrame_ = frame;
            }
            if (size <= ownInstVb_->size()) {
                if (void* p = ownInstVb_->lock(ownInstCursor_, size, flags)) {
                    std::memcpy(p, d.vertexBytes.data(), size);
                    ownInstVb_->unlock();
                }
                const std::uint32_t cs = d.cpuStream;
                dev_.setStreamSource(cs, ownInstVb_.get(), ownInstCursor_, d.streams[cs].stride);
                if (ownOffset_.size() < drawCount_) ownOffset_.resize(drawCount_);
                ownOffset_[k] = ownInstCursor_;
                ownInstCursor_ += (size + 15u) & ~15u;
            }
        } else if (!d.vertexBytes.empty()) {
            // CPU-generated geometry on stream 0 (decals 360,000 / 20,000 bytes, billboards 208,000 / 12,000, the sky's stars 0x80 /
            // 12 bytes per star): our own dynamic buffers, the composed bytes at the engine's positions
            if (!ownVb_) ownVb_ = dev_.createBuffer(kOwnVbBytes, USAGE_DYNAMIC | USAGE_WRITEONLY, POOL_DEFAULT, false, false);
            if (!ownIb_) ownIb_ = dev_.createBuffer(64 * 1024, USAGE_DYNAMIC | USAGE_WRITEONLY, POOL_DEFAULT, true, false);
            // (a non-indexed draw: its vertices from the first vertex startIndex on, no index data)
            const std::uint32_t vOff = d.streams[0].offset + (d.indexed ? d.minIndex : d.startIndex) * d.vertexStride, iOff = d.startIndex * 2;
            if (vOff + d.vertexBytes.size() <= ownVb_->size() && iOff + d.indexBytes.size() <= ownIb_->size()) {
                // DISCARD of just the range the draw reads: the fresh GPU copy takes only those bytes (BufferCopyState)
                if (void* p = ownVb_->lock(vOff, static_cast<std::uint32_t>(d.vertexBytes.size()), LOCK_DISCARD)) {
                    std::memcpy(p, d.vertexBytes.data(), d.vertexBytes.size());
                    ownVb_->unlock();
                }
                if (!d.indexBytes.empty()) {
                    if (void* p = ownIb_->lock(iOff, static_cast<std::uint32_t>(d.indexBytes.size()), LOCK_DISCARD)) {
                        std::memcpy(p, d.indexBytes.data(), d.indexBytes.size());
                        ownIb_->unlock();
                    }
                    dev_.setIndices(ownIb_.get());
                }
                dev_.setStreamSource(0, ownVb_.get(), d.streams[0].offset, d.streams[0].stride);
            }
        }
        for (std::uint32_t i = 0; i < sc::StOwnedCount; ++i) dev_.setRenderState(kRs[i], d.states[i]);
        for (std::uint32_t m = static_cast<std::uint32_t>(d.samplerMask & ~d.inheritMask); m != 0; m &= m - 1) {
            const auto st = sc::lowBit(m);
            dev_.setStageTexture(st, const_cast<Texture*>(static_cast<const Texture*>(d.samplers[st].texture)));
            dev_.setSamplerStates(st, kSs, d.samplers[st].ss.data(), 7);
        }
        // the composed registers as ranges (one copy per run of consecutive registers)
        sc::forEachRun(d.vsSet, [&](std::uint32_t r, std::uint32_t n) { dev_.setVertexShaderConstantF(r, d.vsF[r], n); });
        sc::forEachRun(d.psSet, [&](std::uint32_t r, std::uint32_t n) { dev_.setPixelShaderConstantF(r, d.psF[r], n); });
        tag = tagOverride_ ? tagOverrideValue_ : savedTag;
        if (d.tagSubMesh != nullptr) {
            tag.subMesh = d.tagSubMesh;
            tag.material = d.tagMaterial;
            tag.meshResource = d.tagMeshResource;
            tag.water = static_cast<draws::Water>(d.tagWater);
            tag.materialName = d.tagMaterialName;
        }
        issueIndex_ = k;
        if (d.indexed) {
            dev_.drawIndexedPrimitive(d.primType, d.baseVertex, d.minIndex, d.numVertices, d.startIndex, d.primCount);
        } else {
            dev_.drawPrimitive(d.primType, d.startIndex, d.primCount);
        }
        ++g_->issuedDraws;
    }
    issuing_ = false;
    ++g_->issuedOps;

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
    for (std::uint32_t m = stagesU; m != 0; m &= m - 1) {
        const auto st = sc::lowBit(m);
        dev_.setStageTexture(st, tex[st]);
        dev_.setSamplerStates(st, kSs, ss[st].data(), 7);
    }
    sc::forEachRun(vsU, [&](std::uint32_t r, std::uint32_t n) { dev_.setVertexShaderConstantF(r, savedVs_[r], n); });
    sc::forEachRun(psU, [&](std::uint32_t r, std::uint32_t n) { dev_.setPixelShaderConstantF(r, savedPs_[r], n); });
}

void StaticOps::report() {
    if (verified_ < 0 && active()) {
        spdlog::info("native: static ops: idle (the engine checks failed)");
        return;
    }
    leanReport();
    static constexpr const char* kListNames[] = {"z", "solid", "debug", "sky", "refracted", "alpha", "late", "non-op"};
    if (active()) {
        spdlog::info("native: register file: mode {} ({}), engine code left to the engine {} times, {} registers merged from it (total), {} shaders' pools held",
                     mirrorMode_, mirrorMode_ == 0 ? "ours" : mirrorMode_ == 1 ? "engine pools per view" : "engine pools per op", mergedLeft_, mergedRegs_,
                     mirror_.shaders.size());
    }
    for (std::size_t gi = 0; gi < kGroups; ++gi) {
        GroupState& g = groups_[gi];
        if (!g.active()) continue;
        const char* what = name(static_cast<Group>(gi));
        g.total.add(g.window);
        for (std::size_t i = 0; i < g.skips.size(); ++i) g.skipsTotal[i] += g.skips[i];
        std::string skips, skipsTotal;
        for (std::size_t i = 1; i < g.skips.size(); ++i) {
            if (g.skips[i] != 0) skips += std::format("{}{} {}", skips.empty() ? "" : ", ", name(static_cast<Skip>(i)), g.skips[i]);
            if (g.skipsTotal[i] != 0) skipsTotal += std::format("{}{} {}", skipsTotal.empty() ? "" : ", ", name(static_cast<Skip>(i)), g.skipsTotal[i]);
        }
        if (g.compare) {
            spdlog::info("native: {}: compare (5 s): {}{}", what, sc::summary(g.window, kListNames, std::size(kListNames)),
                         skips.empty() ? "" : "; left to the engine: " + skips);
            spdlog::info("native: {}: compare (total): {}{}", what, sc::summary(g.total, kListNames, std::size(kListNames)),
                         skipsTotal.empty() ? "" : "; left to the engine: " + skipsTotal);
        } else {
            spdlog::info("native: {} (5 s): {} ops, {} composed{}", what, g.window.ops, g.window.opsComposed,
                         skips.empty() ? "" : "; left to the engine: " + skips);
        }
        spdlog::info("native: {}: sub-mesh draws by lights (total): 0 {}, 1 {}, 2 {}, 3 {}, 4 {}", what, g.lights[0], g.lights[1], g.lights[2],
                     g.lights[3], g.lights[4]);
        if (g.takeOver) {
            spdlog::info("native: {}: drawn by the renderer: {} ops, {} draws (total); engine draws dropped {}; own draws checked {}, "
                         "differing from their composition {}",
                         what, g.issuedOps, g.issuedDraws, g.droppedDraws, g.ownChecked, g.ownMismatch);
        }
        g.window = {};
        g.skips = {};
    }
    if (groups_[static_cast<std::size_t>(Group::Actors)].active()) {
        const auto line = [](const ActorStats& a) {
            return std::format("{} ops ({} body-part ops, {} refracted with the empty slot), palettes read {} / composed {} (stamp not this frame), "
                               "{} bones, LOD 0 {} / 1 {} / 2 {} / 3 {} / 4 {}, morph ops {} (active weights {}), overlay ops {} (burned {}, frozen "
                               "{}, highlighted {})",
                               a.ops, a.bodyParts, a.emptySlot, a.palettesStored, a.palettesComposed, a.bones, a.lod[0], a.lod[1], a.lod[2], a.lod[3],
                               a.lod[4], a.morphOps, a.morphActive, a.overlayOps, a.overlayKinds[0], a.overlayKinds[1], a.overlayKinds[2]);
        };
        ActorStats& t = actorTotal_;
        const ActorStats& w = actorWindow_;
        t.ops += w.ops;
        t.palettesStored += w.palettesStored;
        t.palettesComposed += w.palettesComposed;
        t.morphActive += w.morphActive;
        t.morphOps += w.morphOps;
        t.bodyParts += w.bodyParts;
        t.bones += w.bones;
        t.emptySlot += w.emptySlot;
        t.overlayOps += w.overlayOps;
        for (std::size_t i = 0; i < t.lod.size(); ++i) t.lod[i] += w.lod[i];
        for (std::size_t i = 0; i < t.overlayKinds.size(); ++i) t.overlayKinds[i] += w.overlayKinds[i];
        spdlog::info("native: skinned actors (5 s): {}", line(w));
        spdlog::info("native: skinned actors (total): {}", line(t));
        actorWindow_ = {};
    }
    if (groups_[static_cast<std::size_t>(Group::ShadowViews)].active()) {
        const auto line = [](const ShadowStats& a) {
            std::string out;
            for (std::size_t v = 1; v < kViewKindCount; ++v) {
                if (a.views[v] == 0) continue;
                std::string kinds;
                for (std::size_t k = 0; k < kOpKindCount; ++k) {
                    if (a.ops[v][k] == 0) continue;
                    kinds += std::format("{}{} ops {} (composed {}) draws {} / {}", kinds.empty() ? "" : ", ", native::name(static_cast<OpKind>(k)), a.ops[v][k],
                                         a.composed[v][k], a.clean[v][k], a.draws[v][k]);
                }
                out += std::format("{}{} {} views: {}", out.empty() ? "" : "; ", native::name(static_cast<ViewKind>(v)), a.views[v], kinds.empty() ? "no ops" : kinds);
            }
            return out.empty() ? std::string("no shadow view") : out;
        };
        shadowTotal_.add(shadowWindow_);
        spdlog::info("native: shadow views (5 s): {} (draws equal / compared)", line(shadowWindow_));
        spdlog::info("native: shadow views (total): {}", line(shadowTotal_));
        shadowWindow_ = {};
    }
    if (groups_[static_cast<std::size_t>(Group::Trees)].active() || groups_[static_cast<std::size_t>(Group::Vegetation)].active() ||
        groups_[static_cast<std::size_t>(Group::ShadowViews)].active()) {
        const auto line = [](const TreeStats& a) {
            return std::format("SpeedTree ops {} (branches {}, fronds {}, leaf cards {}, leaf meshes {}; 0 draws {}, wrong TransformationType {}), "
                               "instancing hardware {} / software {} ops, {} instances in {} batches ({} not drawn: strip length 0), {} leaf LOD "
                               "passes, queue copies sorted {}, shadow fronds skipped (leaf-card vertices > frond vertices) {}; tree billboard ops {} ({} tex-coord uploads); vegetation ops {} (0 draws {}, mesh "
                               "not cached {}), {} nodes, {} instances, {} draws, buffer wraps {}, c12 from the pool (the clock moved since Process) {}",
                               a.ops, a.types[0], a.types[1], a.types[2], a.types[3], a.zeroOps, a.wrongTransformation, a.hardware, a.software,
                               a.instances, a.batches, a.undrawnBatches, a.leafLods, a.sortedCopies, a.shadowFrondsSkipped, a.billboardOps, a.billboardTexCoords, a.grassOps,
                               a.grassZeroOps, a.grassNotCached, a.grassNodes, a.grassInstances, a.grassDraws, a.grassWraps, a.grassClockMoved);
        };
        TreeStats& t = treeTotal_;
        const TreeStats& w = treeWindow_;
        t.ops += w.ops;
        t.zeroOps += w.zeroOps;
        t.hardware += w.hardware;
        t.software += w.software;
        t.instances += w.instances;
        t.batches += w.batches;
        t.sortedCopies += w.sortedCopies;
        t.undrawnBatches += w.undrawnBatches;
        for (std::size_t i = 0; i < t.types.size(); ++i) t.types[i] += w.types[i];
        t.leafLods += w.leafLods;
        t.shadowFrondsSkipped += w.shadowFrondsSkipped;
        t.wrongTransformation += w.wrongTransformation;
        t.billboardOps += w.billboardOps;
        t.billboardTexCoords += w.billboardTexCoords;
        t.grassOps += w.grassOps;
        t.grassZeroOps += w.grassZeroOps;
        t.grassNodes += w.grassNodes;
        t.grassInstances += w.grassInstances;
        t.grassDraws += w.grassDraws;
        t.grassWraps += w.grassWraps;
        t.grassNotCached += w.grassNotCached;
        t.grassClockMoved += w.grassClockMoved;
        spdlog::info("native: trees (5 s): {}", line(w));
        spdlog::info("native: trees (total): {}", line(t));
        treeWindow_ = {};
    }
    if (groups_[static_cast<std::size_t>(Group::Particles)].active() || groups_[static_cast<std::size_t>(Group::Strips)].active() ||
        groups_[static_cast<std::size_t>(Group::Precipitation)].active()) {
        const auto line = [](const FxStats& a) {
            return std::format("particle ops {} ({} quads; 0 quads built {}; generated natively {} (sorted copies), the engine's bytes of this tick "
                               "{}; orientation 0 {} / 1 {} / 2 {} / 3 {} / 4 {} / other {}; world {} / entity-relative {}; frame blending {}; lit "
                               "{}, instanced emitters {}; no live particle at the entry {}, of them drawn by the engine {}), strip ops {} ({} segments), "
                               "light streak ops {}, precipitation ops {} ({} drops; off {}; billboard records re-done from the drops equal {} / differ {})",
                               a.particleOps, a.particleQuads, a.particleZeroQuads, a.particleSorted, a.particleEngineBytes, a.orient[0], a.orient[1],
                               a.orient[2], a.orient[3], a.orient[4], a.orient[5], a.coords[0], a.coords[1], a.frameBlend, a.lit, a.instancedEmitters,
                               a.simulatedOps, a.simulatedDrew, a.stripOps, a.stripSegments, a.streakOps, a.precipOps, a.precipDrops, a.precipOff, a.precipRecordsEqual, a.precipRecordsDiffer);
        };
        FxStats& t = fxTotal_;
        const FxStats& w = fxWindow_;
        t.particleOps += w.particleOps;
        t.particleQuads += w.particleQuads;
        t.particleSkipped += w.particleSkipped;
        t.particleEngineBytes += w.particleEngineBytes;
        t.particleZeroQuads += w.particleZeroQuads;
        t.particleSorted += w.particleSorted;
        for (std::size_t i = 0; i < t.orient.size(); ++i) t.orient[i] += w.orient[i];
        for (std::size_t i = 0; i < t.coords.size(); ++i) t.coords[i] += w.coords[i];
        t.rotated += w.rotated;
        t.frameBlend += w.frameBlend;
        t.lit += w.lit;
        t.instancedEmitters += w.instancedEmitters;
        t.stripOps += w.stripOps;
        t.stripSegments += w.stripSegments;
        t.streakOps += w.streakOps;
        t.precipOps += w.precipOps;
        t.precipDrops += w.precipDrops;
        t.precipOff += w.precipOff;
        t.simulatedOps += w.simulatedOps;
        t.simulatedDrew += w.simulatedDrew;
        t.precipRecordsEqual += w.precipRecordsEqual;
        t.precipRecordsDiffer += w.precipRecordsDiffer;
        spdlog::info("native: effects (5 s): {}", line(w));
        spdlog::info("native: effects (total): {}", line(t));
        fxWindow_ = {};
    }
}

}  // namespace rvk::native
