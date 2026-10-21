// eCAPIDevice on d3d9::Device (docs/research/hal-structs.md sections 2.x).

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>
#include <format>
#include <string>

#include <spdlog/spdlog.h>

#include "api/capture.h"
#include "core/caps.h"
#include "core/exports.h"
#include "core/settings.h"
#include "d3d9/fvf.h"
#include "engine/scene_hooks.h"
#include "engine/vfs.h"
#include "hal/vulkan/hal_vulkan.h"
#include "res/dds.h"
#include "vk/formats.h"
#include "vk/raytracing.h"

namespace rvk::hal::vulkan {

using namespace rvk::d3d9;

namespace {
std::atomic<bool> g_logFrame{false};
}  // namespace

bool logThisFrame() { return g_logFrame.load(std::memory_order_relaxed); }

// -LogFrame: every key HAL call of the frame, prefixed with the frame phase (view/pass, engine/scene_hooks.h)
#define HAL_LOG(fmt, ...)                                                                                     \
    do {                                                                                                      \
        if (logThisFrame()) spdlog::info("[{}] " fmt, engine::scene::currentLabel() __VA_OPT__(, ) __VA_ARGS__); \
    } while (0)

void HalDevice::updateLogFrame() {
    g_logFrame = options_.logFrame != 0 && dev_.stats().frames + 1 == options_.logFrame;
}

namespace {

// the engine's CPU-only DX9 functions (D3DX preprocessing / effect compilation / aux buffers) called on a stand-in `this`
template <class Fn>
Fn dx9(const char* name) {
    static Fn fn = nullptr;
    if (fn == nullptr) {
        fn = exports::resolveAs<Fn>(L"Engine.dll", name);
    }
    return fn;
}

}  // namespace

HalDevice::HalDevice() {
    // eCDX9Device::CompileEffect* return 0 when the IDirect3DDevice9 at +0x10 is null; nothing else of `this` is used
    dx9Standin_[2] = 1;  // +0x10
}

HalDevice::~HalDevice() {
    engine::scene::detach(&dev_);
    settings::registry().removeCallbacks(this);
    if (overlay_) {
        dev_.setOverlay(nullptr, false);
        // ImGui's shutdown waits for the device (vkDeviceWaitIdle touches the queue the present thread uses)
        auto lock = dev_.ctx().queueLock();
        overlay_.reset();
    }
    if (settings::registry().dirty() && !options_.settingsFile.empty()) {
        settings::registry().save(options_.settingsFile);
    }
    debugView_.reset();  // removes its pass, retires its pipelines
    {
        const std::lock_guard lock(handleMutex_);
        for (auto& [h, d] : decls_) dev_.release(std::move(d));
        for (auto& [f, d] : fvfDecls_) dev_.release(std::move(d));
        for (auto& [h, t] : depthBuffers_) dev_.release(std::move(t));
        decls_.clear();
        fvfDecls_.clear();
        depthBuffers_.clear();
    }
    dev_.destroy();
}

bool HalDevice::create(const Options& options, const eCAPIDevice::eSAPIStartParameters& p) {
    options_ = options;
    DeviceOptions o;
    o.window = p.window;
    o.width = p.backBufferWidth;
    o.height = p.backBufferHeight;
    engineVsync_ = p.vsync;
    const auto& rs = settings::renderer();
    if (rs.vsync != nullptr) {
        // player settings (config.toml overrides are locks on them: bootstrap)
        const std::string_view v = rs.vsync->asEnum();
        o.vsync = v == "on" ? true : v == "off" ? false : p.vsync;
    } else {
        o.vsync = options.vsync < 0 ? p.vsync : options.vsync != 0;
    }
    o.perf = options.perf;
    o.pipelineLibraries = options.pipelineLibraries;
    o.perfFrame = options.perfFrame;
    o.depthFormat = static_cast<std::uint32_t>(p.depthFormat);
    o.validation = options.validation;
    o.syncValidation = options.syncValidation;
    o.adapter = options.adapter;
    o.maxFps = rs.maxFps != nullptr ? rs.maxFps->value() : options.maxFps;
    o.cacheDir = options.cacheDir;
    o.logDraws = options.logDraws;
    // rt.allow is a restart setting: the value the session started with decides (its effective value waits for the report)
    o.allowRayTracing = rs.rtAllow == nullptr || rs.rtAllow->startValue() != 0.0;
    if (!p.windowed) {
        spdlog::warn("hal: fullscreen requested, running windowed (exclusive fullscreen is not supported yet)");
    }
    if (!dev_.create(o)) {
        return false;
    }
    spdlog::info("hal: Vulkan device for window {} ({}x{}, vsync {}, depth {})", static_cast<void*>(p.window), o.width, o.height,
                 o.vsync, vk::d3dFormatName(o.depthFormat));
    engine::scene::attach(&dev_);  // the frame phase markers (no-ops when they are not installed)
    auto& reg = settings::registry();
    // capability report (core/caps.h, docs/research/raytracing.md 3): logged once, the overlay's "Capabilities", and the
    // resolution of settings with requirements (greyed + parity fallback when unmet)
    {
        const caps::Report report = dev_.ctx().capabilities(options.pipelineLibraries);
        for (const auto& line : report.lines()) spdlog::info("caps: {}", line);
        caps::setCurrent(report);
        reg.setCapabilities(report);
    }
    if (options.rtSelfTest) {
        const vk::rt::SelfTestResult t = vk::rt::selfTest(dev_.ctx());
        if (!t.ran) {
            spdlog::info("rt selftest: skipped: {}", t.detail);
        } else {
            spdlog::log(t.pass ? spdlog::level::info : spdlog::level::err, "rt selftest: {} ({:.1f} ms): {}", t.pass ? "PASS" : "FAIL",
                        t.milliseconds, t.detail);
        }
    }
    if (rs.vsync != nullptr) reg.onChange(*rs.vsync, [this](const settings::Setting&) { applyVsync(); }, this);
    if (rs.maxFps != nullptr) {
        reg.onChange(*rs.maxFps, [this](const settings::Setting& s) {
            dev_.setMaxFps(s.value());
            spdlog::info("hal: settings: frame rate limit {}", s.value() > 0.0 ? std::format("{} fps", s.value()) : std::string("none"));
        }, this);
    }
    // debug views and draw records (d3d9/draws.h, docs/research/draw-tags.md)
    if (rs.tintDraws != nullptr) {
        const auto applyTint = [this](const settings::Setting& s) {
            const std::string_view v = s.asEnum();
            dev_.setTintMode(v == "kind" ? draws::TintMode::Kind : v == "pass" ? draws::TintMode::Pass : draws::TintMode::Off);
            spdlog::info("hal: settings: tint draws {}", v);
        };
        if (!rs.tintDraws->isDefault()) applyTint(*rs.tintDraws);
        reg.onChange(*rs.tintDraws, applyTint, this);
    }
    if (rs.drawRecords != nullptr) {
        dev_.setDrawRecords(rs.drawRecords->asBool());
        reg.onChange(*rs.drawRecords, [this](const settings::Setting& s) {
            dev_.setDrawRecords(s.asBool());
            spdlog::info("hal: settings: draw records {}", s.asBool() ? "on" : "off");
        }, this);
    }
    // enhancements (docs/research/injection.md): the debug injection view
    debugView_ = std::make_unique<enh::DebugView>(dev_);
    if (rs.injectView != nullptr) {
        const auto applyInject = [this](const settings::Setting& s) {
            debugView_->setMode(enh::DebugView::parse(s.asEnum()));
            spdlog::info("hal: settings: injection test view {}", s.asEnum());
        };
        if (!rs.injectView->isDefault()) applyInject(*rs.injectView);
        reg.onChange(*rs.injectView, applyInject, this);
    }
    if (options.overlay && dev_.swapchain().valid()) {
        overlay_ = std::make_unique<overlay::Overlay>();
        overlay::Options oo;
        oo.intoBackBuffer = options.overlayInScreenshots;
        oo.commands = options.overlayCommands;
        oo.settingsFile = options.settingsFile;
        dev_.setOverlay(overlay_.get(), oo.intoBackBuffer);  // first: the overlay asks the device for its colour format
        if (!overlay_->init(dev_, p.window, oo)) {
            dev_.setOverlay(nullptr, false);
            overlay_.reset();
            spdlog::warn("hal: the settings overlay is not available this session");
        }
    }
    return true;
}

void HalDevice::applyVsync() {
    const auto& rs = settings::renderer();
    if (rs.vsync == nullptr) return;
    const std::string_view v = rs.vsync->asEnum();
    const bool on = v == "on" ? true : v == "off" ? false : engineVsync_;
    if (on != dev_.vsync()) {
        spdlog::info("hal: settings: vsync {} ({})", on ? "on" : "off", v == "engine" ? "the game's option" : "player setting");
    }
    dev_.setVsync(on);
}

// render thread, before each present: setting callbacks, debounced saving of settings.toml
void HalDevice::serviceSettings() {
    auto& reg = settings::registry();
    reg.dispatch();
    if (!reg.dirty() || options_.settingsFile.empty()) {
        settingsDirtySince_ = {};
        return;
    }
    const auto now = std::chrono::steady_clock::now();
    if (settingsDirtySince_ == std::chrono::steady_clock::time_point{}) {
        settingsDirtySince_ = now;
    } else if (now - settingsDirtySince_ >= std::chrono::seconds(1)) {
        std::string error;
        if (reg.save(options_.settingsFile, &error)) {
            spdlog::info("settings: saved {}", api::utf8(options_.settingsFile));
        } else {
            spdlog::warn("settings: saving {} failed: {}", api::utf8(options_.settingsFile), error);
            reg.clearDirty();
        }
        settingsDirtySince_ = {};
    }
}

void HalDevice::setRenderState(std::uint32_t rs, std::uint32_t value) {
    if (rs >= RS_COUNT) return;
    if (blockRecording_ && !blockSaved_[rs]) {
        blockSaved_[rs] = true;
        blockValues_[rs] = dev_.renderState(rs);
    }
    dev_.setRenderState(rs, value);
}

void HalDevice::unbindTextures() {
    for (std::uint32_t s = 0; s < 16; ++s) {
        dev_.setTexture(s, nullptr);
    }
}

d3d9::VertexDecl* HalDevice::declFromHandle(void* handle) { return static_cast<d3d9::VertexDecl*>(handle); }

// ---- resources ------------------------------------------------------------------------------------------------------

bEResult HalDevice::CreatePrimititve(eCAPIPrimitive** a0) {
    if (a0 == nullptr) return bEResult_False;
    *a0 = new HalPrimitive(*this);
    return bEResult_Ok;
}

bEResult HalDevice::CreateIndexData(eCGfxShared::eSGfxIndexBufDesc const& a0, eCAPIIndexData** a1) {
    if (a1 == nullptr) return bEResult_False;
    auto b = dev_.createBuffer(a0.length, a0.usage, a0.pool, true, a0.format == 102 /* D3DFMT_INDEX32 */);
    *a1 = new HalIndexData(*this, a0, std::move(b));
    return bEResult_Ok;
}

namespace {

// "Engine.dll+0x1234 < Engine.dll+0x5678 < ..." for the callers of the current function (diagnostics)
std::string callerChain(int skip, int frames) {
    void* stack[16]{};
    const USHORT n = RtlCaptureStackBackTrace(static_cast<DWORD>(skip + 1), static_cast<DWORD>(std::min(frames, 16)), stack, nullptr);
    std::string s;
    for (USHORT i = 0; i < n; ++i) {
        HMODULE mod = nullptr;
        GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           static_cast<LPCWSTR>(stack[i]), &mod);
        char name[MAX_PATH]{};
        if (mod != nullptr) GetModuleFileNameA(mod, name, MAX_PATH);
        const char* base = std::strrchr(name, '\\') != nullptr ? std::strrchr(name, '\\') + 1 : name;
        s += fmt::format("{}{}+0x{:x}", i ? " < " : "", mod != nullptr ? base : "?",
                         reinterpret_cast<std::uintptr_t>(stack[i]) - reinterpret_cast<std::uintptr_t>(mod));
    }
    return s;
}

}  // namespace

bEResult HalDevice::CreateVertexData(eCGfxShared::eSGfxVertexBufDesc const& a0, eCAPIVertexData** a1) {
    if (a1 == nullptr) return bEResult_False;
    // eCDX9VertexData 0x74dcf0: the stride is D3DXGetFVFVertexSize(fvf) for FVF buffers, desc.stride only when fvf == 0
    std::uint32_t stride = a0.stride;
    if (a0.fvf != 0) {
        stride = fvfVertexSize(a0.fvf);
        static std::atomic<int> logged{0};
        if (logged.fetch_add(1) < 2) {
            spdlog::info("hal: FVF vertex buffer 0x{:x} ({} bytes, stride {}, desc.stride {}) from {}", a0.fvf, a0.length, stride,
                         a0.stride, callerChain(1, 6));
        }
    }
    auto b = dev_.createBuffer(a0.length, a0.usage, a0.pool, false, false);
    *a1 = new HalVertexData(*this, a0, stride, std::move(b));
    return bEResult_Ok;
}

d3d9::VertexDecl* HalDevice::fvfDecl(std::uint32_t fvf) {
    const std::lock_guard lock(handleMutex_);
    if (auto it = fvfDecls_.find(fvf); it != fvfDecls_.end()) {
        return it->second.get();
    }
    const auto elements = fvfDeclaration(fvf);
    if (elements.empty()) {
        spdlog::warn("hal: invalid FVF 0x{:x}", fvf);
        return nullptr;
    }
    auto decl = dev_.createVertexDecl(elements);
    d3d9::VertexDecl* p = decl.get();
    fvfDecls_.emplace(fvf, std::move(decl));
    return p;
}

bEResult HalDevice::GetFVFSize(unsigned long a0, unsigned long* a1) {
    if (a1 == nullptr) return bEResult_False;
    *a1 = fvfVertexSize(static_cast<std::uint32_t>(a0));
    return bEResult_Ok;
}

bEResult HalDevice::PreprocessShader(eCGfxShared::eSGfxHLSLShaderSrc const& a0, bCString& a1) {
    using Fn = bEResult (*)(void*, const eCGfxShared::eSGfxHLSLShaderSrc*, bCString*);
    const Fn fn = dx9<Fn>("?PreprocessShader@eCDX9Device@@UEAA?AW4bEResult@@AEBUeSGfxHLSLShaderSrc@eCGfxShared@@AEAVbCString@@@Z");
    return fn != nullptr ? fn(dx9Standin_, &a0, &a1) : bEResult_False;
}

bEResult HalDevice::CompileEffect(eCGfxShared::eSGfxEffectSource const& a0, void** a1, void** a2) {
    using Fn = bEResult (*)(void*, const eCGfxShared::eSGfxEffectSource*, void**, void**);
    const Fn fn = dx9<Fn>("?CompileEffect@eCDX9Device@@UEAA?AW4bEResult@@AEBUeSGfxEffectSource@eCGfxShared@@PEAPEAX1@Z");
    return fn != nullptr ? fn(dx9Standin_, &a0, a1, a2) : bEResult_False;
}

bEResult HalDevice::CompileEffectFromFile(eCGfxShared::eSGfxEffectFileSource const& a0, void** a1, void** a2) {
    using Fn = bEResult (*)(void*, const eCGfxShared::eSGfxEffectFileSource*, void**, void**);
    const Fn fn = dx9<Fn>("?CompileEffectFromFile@eCDX9Device@@UEAA?AW4bEResult@@AEBUeSGfxEffectFileSource@eCGfxShared@@PEAPEAX1@Z");
    return fn != nullptr ? fn(dx9Standin_, &a0, a1, a2) : bEResult_False;
}

bEResult HalDevice::CreateAuxBuffer(unsigned long a0, void** a1) {
    using Fn = bEResult (*)(void*, unsigned long, void**);
    const Fn fn = dx9<Fn>("?CreateAuxBuffer@eCDX9Device@@UEAA?AW4bEResult@@KPEAPEAX@Z");
    return fn != nullptr ? fn(dx9Standin_, a0, a1) : bEResult_False;
}

bEResult HalDevice::DestroyAuxBuffer(void* a0) {
    using Fn = bEResult (*)(void*, void*);
    const Fn fn = dx9<Fn>("?DestroyAuxBuffer@eCDX9Device@@UEAA?AW4bEResult@@PEAX@Z");
    return fn != nullptr ? fn(dx9Standin_, a0) : bEResult_False;
}

bEResult HalDevice::GetAuxBuffer(void* a0, void** a1, unsigned long* a2) {
    using Fn = bEResult (*)(void*, void*, void**, unsigned long*);
    const Fn fn = dx9<Fn>("?GetAuxBuffer@eCDX9Device@@UEAA?AW4bEResult@@PEAXPEAPEAXPEAK@Z");
    return fn != nullptr ? fn(dx9Standin_, a0, a1, a2) : bEResult_False;
}

bEResult HalDevice::CreateHLSLShader(eCAPIHLSLShader** a0) {
    if (a0 == nullptr) return bEResult_False;
    *a0 = new HalHLSLShader(*this);
    return bEResult_Ok;
}

namespace {

TextureDesc textureDesc(const eCGfxShared::eSGfxLayersDesc& d, TextureType type) {
    TextureDesc t;
    t.type = type;
    t.width = std::max(1u, d.width);
    t.height = std::max(1u, type == TextureType::Cube ? d.width : d.height);
    t.depth = std::max(1u, d.depth);
    t.levels = d.levels;
    t.format = d.format;
    t.usage = d.usage;
    t.pool = d.pool;
    return t;
}

}  // namespace

bEResult HalDevice::CreateTexture(eCGfxShared::eSGfxLayersDesc const& a0, eCAPITexture** a1) {
    if (a1 == nullptr) return bEResult_False;
    auto t = dev_.createTexture(textureDesc(a0, TextureType::Tex2D));
    if (!t) {
        *a1 = nullptr;
        return bEResult_False;
    }
    *a1 = new HalTexture(*this, std::move(t));
    return bEResult_Ok;
}

bEResult HalDevice::CreateCubeMap(eCGfxShared::eSGfxLayersDesc const& a0, eCAPICubeMap** a1) {
    if (a1 == nullptr) return bEResult_False;
    auto t = dev_.createTexture(textureDesc(a0, TextureType::Cube));
    if (!t) {
        *a1 = nullptr;
        return bEResult_False;
    }
    *a1 = new HalCubeMap(*this, std::move(t));
    return bEResult_Ok;
}

bEResult HalDevice::CreateVolume(eCGfxShared::eSGfxLayersDesc const& a0, eCAPIVolume** a1) {
    if (a1 == nullptr) return bEResult_False;
    auto t = dev_.createTexture(textureDesc(a0, TextureType::Volume));
    if (!t) {
        *a1 = nullptr;
        return bEResult_False;
    }
    *a1 = new HalVolume(*this, std::move(t));
    return bEResult_Ok;
}

std::unique_ptr<d3d9::Texture> HalDevice::textureFromDds(const char* what, const void* data, std::uint32_t size, std::uint32_t skip,
                                                        int levelsWanted, std::uint32_t pool, std::uint32_t usage, int wantCube,
                                                        eCGfxShared::eSGfxLayersDesc& out) {
    std::string err;
    const auto dds = res::parseDds({static_cast<const std::uint8_t*>(data), size}, &err);
    if (!dds) {
        static std::atomic<int> logged{0};
        if (logged.fetch_add(1) < 10) spdlog::warn("hal: {}: {} ({} bytes)", what, err, size);
        return nullptr;
    }
    if (dds->volume || (wantCube >= 0 && dds->cube != (wantCube != 0))) {
        spdlog::warn("hal: {}: a {} DDS file is not supported here", what, dds->volume ? "volume" : dds->cube ? "cube" : "2D");
        return nullptr;
    }
    // skip `skip` top levels; levels <= 0: all of the file's
    if (skip >= dds->mips) skip = dds->mips - 1;
    std::uint32_t levels = dds->mips - skip;
    if (levelsWanted > 0) levels = std::min(levels, static_cast<std::uint32_t>(levelsWanted));
    TextureDesc td;
    td.type = dds->cube ? TextureType::Cube : TextureType::Tex2D;
    td.width = std::max(1u, dds->width >> skip);
    td.height = std::max(1u, dds->height >> skip);
    td.levels = levels;
    td.format = dds->d3dFormat;
    td.usage = usage;
    td.pool = pool;
    auto t = dev_.createTexture(td);
    if (!t) {
        return nullptr;
    }
    for (std::uint32_t face = 0; face < dds->faces; ++face) {
        for (std::uint32_t l = 0; l < levels; ++l) {
            if (const auto lv = dds->level(face, l + skip)) {
                t->setLevelData(l, face, lv->bytes, lv->rowPitch);
            }
        }
    }
    out.width = td.width;
    out.height = td.height;
    out.levels = levels;
    out.format = td.format;
    out.cubeMap = dds->cube;
    return t;
}

bEResult HalDevice::CreateImageFromMemory(eCGfxShared::eEPoolType a0, int a1, unsigned char a2, void const* a3, unsigned int a4,
                                          eCGfxShared::eSGfxLayersDesc& a5, void*& a6) {
    // levels -1 / -2: from the file for DDS (eCDX9Device::CreateImageFromMemory 0x73f030); skip a2 top levels
    a6 = nullptr;
    auto t = textureFromDds("CreateImageFromMemory", a3, a4, a2, a1, static_cast<std::uint32_t>(a0), 0, -1, a5);
    if (!t) {
        return bEResult_False;
    }
    a5.pool = static_cast<std::uint32_t>(a0);
    if (a5.cubeMap) {
        a6 = static_cast<eCAPICubeMap*>(new HalCubeMap(*this, std::move(t)));
    } else {
        a6 = static_cast<eCAPITexture*>(new HalTexture(*this, std::move(t)));
    }
    return bEResult_Ok;
}

namespace {

// eCDX9Texture / eCDX9CubeMap "from memory" ctors (0x74c650 / 0x73caa0): D3DXCreate(Cube)TextureFromFileInMemoryEx with the
// descriptor's size, levels, usage, format, pool, filter BOX | (skip << 26). We load DDS files as stored: a size or format
// that would need D3DX's resampling / conversion is refused. Returns the skip count, or -1 when refused.
int fromMemoryRequest(const char* what, const eCGfxShared::eSGfxLayersDesc& d, unsigned char skipLevels, const void* data,
                      unsigned int size) {
    constexpr std::uint32_t kDefault = 0xffffffffu, kDefaultNonPow2 = 0xfffffffeu, kFromFile = 0xfffffffdu;
    // D3D9: levels in (0, skip] clamp the skip to levels - 1
    std::uint32_t skip = skipLevels;
    if (d.levels != 0 && d.levels != kFromFile && d.levels <= skip) skip = d.levels - 1;
    const auto dds = res::parseDds({static_cast<const std::uint8_t*>(data), size});
    if (!dds) return static_cast<int>(skip);  // textureFromDds logs the reason
    const std::uint32_t s = std::min(skip, dds->mips - 1);
    const auto sizeOk = [](std::uint32_t want, std::uint32_t file) {
        return want == 0 || want == kDefault || want == kDefaultNonPow2 || want == kFromFile || want == file;
    };
    const bool formatOk = d.format == 0 || d.format == kFromFile || d.format == dds->d3dFormat;
    if (!sizeOk(d.width, std::max(1u, dds->width >> s)) || !sizeOk(d.height, std::max(1u, dds->height >> s)) || !formatOk ||
        (d.usage & (USAGE_RENDERTARGET | USAGE_DEPTHSTENCIL)) != 0) {
        spdlog::warn("hal: {}: {}x{} {} usage 0x{:x} from a {}x{} {} file needs D3DX resampling / conversion: not supported", what,
                     d.width, d.height, vk::d3dFormatName(d.format), d.usage, dds->width, dds->height,
                     vk::d3dFormatName(dds->d3dFormat));
        return -1;
    }
    return static_cast<int>(skip);
}

int levelsRequest(std::uint32_t levels) {
    // 0 / D3DX_DEFAULT / D3DX_FROM_FILE: the file's chain
    return levels == 0 || levels >= 0xfffffffdu ? 0 : static_cast<int>(levels);
}

}  // namespace

bEResult HalDevice::CreateTextureFromMemory(eCGfxShared::eSGfxLayersDesc& a0, unsigned char a1, void* a2, unsigned int a3,
                                            eCAPITexture** a4) {
    if (a4 == nullptr) return bEResult_False;
    *a4 = nullptr;
    const int skip = fromMemoryRequest("CreateTextureFromMemory", a0, a1, a2, a3);
    if (skip < 0) return bEResult_False;
    auto t = textureFromDds("CreateTextureFromMemory", a2, a3, static_cast<std::uint32_t>(skip), levelsRequest(a0.levels), a0.pool,
                            a0.usage & USAGE_DYNAMIC, 0, a0);
    if (!t) return bEResult_False;
    *a4 = new HalTexture(*this, std::move(t));
    return bEResult_Ok;
}

bEResult HalDevice::CreateCubeMapFromMemory(eCGfxShared::eSGfxLayersDesc& a0, unsigned char a1, void* a2, unsigned int a3,
                                            eCAPICubeMap** a4) {
    if (a4 == nullptr) return bEResult_False;
    *a4 = nullptr;
    const int skip = fromMemoryRequest("CreateCubeMapFromMemory", a0, a1, a2, a3);
    if (skip < 0) return bEResult_False;
    auto t = textureFromDds("CreateCubeMapFromMemory", a2, a3, static_cast<std::uint32_t>(skip), levelsRequest(a0.levels), a0.pool,
                            a0.usage & USAGE_DYNAMIC, 1, a0);
    if (!t) return bEResult_False;
    *a4 = new HalCubeMap(*this, std::move(t));
    return bEResult_Ok;
}

bEResult HalDevice::GetImageInfoFromMemory(void const* a0, unsigned int a1, eCGfxShared::eSGfxImageInfo& a2) {
    const auto dds = res::parseDds({static_cast<const std::uint8_t*>(a0), a1});
    if (!dds) return bEResult_False;
    a2.width = dds->width;
    a2.height = dds->height;
    a2.depth = dds->depth;
    a2.mipLevels = dds->mips;
    a2.format = dds->d3dFormat;
    a2.resourceType = dds->cube ? 5u : dds->volume ? 4u : 3u;
    a2.imageFileFormat = 4;  // D3DXIFF_DDS
    return bEResult_Ok;
}

bEResult HalDevice::CheckDeviceFormat(eCGfxShared::eSGfxLayersDesc const& a0, bool a1) {
    const bEResult r = checkFormat(a0, a1);
    spdlog::debug("hal: CheckDeviceFormat {} (0x{:x}) {}x{} usage 0x{:x} blend {}: {}", vk::d3dFormatName(a0.format), a0.format,
                  a0.width, a0.height, a0.usage, a1, r == bEResult_Ok ? "yes" : "no");
    return r;
}

bEResult HalDevice::checkFormat(eCGfxShared::eSGfxLayersDesc const& a0, bool a1) {
    using namespace vk::d3dfmt;
    const std::uint32_t f = a0.format;
    if (f == NULLFMT) {
        return bEResult_Ok;
    }
    // Shadow maps (eCShaderMaterialAdmin::PostInitialize 0x6ba9d0 probes R32F, R16F, D16, D24X8, DF16, DF24 in this order and
    // takes the last supported one; docs/research/engine-gfx.md 5). The ATI depth textures DF16 / DF24 are sampled as a plain
    // depth value (no compare, no fetch4 in post_SSM.fx): exactly a sampled Vulkan depth image, the D3D9 reference path on
    // AMD. D16 / D24 depth textures would need NVIDIA's implicit hardware-PCF compare on tex2Dproj and INTZ is not used:
    // reported unsupported. Without DF24 the engine falls back to R16F colour maps (16-bit float depth: striped shadows).
    if (f == DF16 || f == DF24) {
        if ((a0.usage & USAGE_DEPTHSTENCIL) == 0 || a0.depth > 1 || a0.cubeMap) {
            return bEResult_False;
        }
        const auto info = vk::formatInfo(f, dev_.d24s8Format());
        if (!info) return bEResult_False;
        VkFormatProperties fp{};
        vkGetPhysicalDeviceFormatProperties(dev_.ctx().physicalDevice(), info->format, &fp);
        const VkFormatFeatureFlags need = VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT;
        return (fp.optimalTilingFeatures & need) == need ? bEResult_Ok : bEResult_False;
    }
    if (f == INTZ || (a0.usage & USAGE_DEPTHSTENCIL) != 0) {
        return bEResult_False;
    }
    const auto info = vk::formatInfo(f, dev_.d24s8Format());
    if (!info || info->depth) {
        return bEResult_False;
    }
    VkFormatProperties fp{};
    vkGetPhysicalDeviceFormatProperties(dev_.ctx().physicalDevice(), info->format, &fp);
    VkFormatFeatureFlags need = VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT;
    if ((a0.usage & USAGE_RENDERTARGET) != 0) {
        need |= VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT;
        if (a1) need |= VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BLEND_BIT;
    }
    return (fp.optimalTilingFeatures & need) == need ? bEResult_Ok : bEResult_False;
}

bEResult HalDevice::CreateVertexDeclaration(eCGfxShared::eSGfxDclElement const* a0, void** a1) {
    if (a0 == nullptr || a1 == nullptr) return bEResult_False;
    std::size_t n = 0;
    while (n < 64 && a0[n].stream != 0xFF) ++n;
    auto decl = dev_.createVertexDecl({reinterpret_cast<const VertexElement*>(a0), n + 1});
    void* handle = decl.get();
    const std::lock_guard lock(handleMutex_);
    decls_.emplace(handle, std::move(decl));
    *a1 = handle;
    return bEResult_Ok;
}

bEResult HalDevice::DestroyVertexDeclaration(void* a0) {
    const std::lock_guard lock(handleMutex_);
    if (auto it = decls_.find(a0); it != decls_.end()) {
        dev_.release(std::move(it->second));
        decls_.erase(it);
    }
    return bEResult_Ok;
}

bEResult HalDevice::GetVertexDeclarationData(void* a0, eCGfxShared::eSGfxDclElement* a1, unsigned int* a2) {
    auto* decl = declFromHandle(a0);
    if (decl == nullptr || a2 == nullptr) return bEResult_False;
    const auto& e = decl->elements();
    const unsigned int count = static_cast<unsigned int>(e.size() + 1);  // incl. D3DDECL_END
    if (a1 != nullptr) {
        std::memcpy(a1, e.data(), e.size() * sizeof(VertexElement));
        a1[e.size()] = {0xFF, 0, 17, 0, 0, 0};
    }
    *a2 = count;
    return bEResult_Ok;
}

bEResult HalDevice::SetVertexDeclaration(void* a0) {
    HAL_LOG("hal> SetVertexDeclaration {}", a0);
    dev_.setVertexDecl(declFromHandle(a0));
    return bEResult_Ok;
}

bEResult HalDevice::CreateDepthBuffer(unsigned long a0, unsigned long a1, eCGfxShared::eEDepthFormat a2, void** a3) {
    if (a3 == nullptr) return bEResult_False;
    TextureDesc d;
    d.width = static_cast<std::uint32_t>(a0);
    d.height = static_cast<std::uint32_t>(a1);
    d.format = static_cast<std::uint32_t>(a2);
    d.usage = USAGE_DEPTHSTENCIL;
    d.pool = POOL_DEFAULT;
    auto t = dev_.createTexture(d);
    if (!t) return bEResult_False;
    d3d9::Surface* s = t->surface(0, 0);
    const std::lock_guard lock(handleMutex_);
    depthBuffers_.emplace(s, std::move(t));
    *a3 = s;
    return bEResult_Ok;
}

bEResult HalDevice::DestroyDepthBuffer(void* a0) {
    const std::lock_guard lock(handleMutex_);
    if (auto it = depthBuffers_.find(a0); it != depthBuffers_.end()) {
        dev_.release(std::move(it->second));
        depthBuffers_.erase(it);
    }
    return bEResult_Ok;
}

// ---- modes / frame --------------------------------------------------------------------------------------------------

bEResult HalDevice::ToggleNewMode(eCAPIDevice::eSAPIModeParameters const& a0) {
    spdlog::info("hal: ToggleNewMode {}x{} (windowed {}, vsync {})", a0.backBufferWidth, a0.backBufferHeight, a0.windowed, a0.vsync);
    engineVsync_ = a0.vsync;
    applyVsync();
    return dev_.resize(a0.backBufferWidth, a0.backBufferHeight) ? bEResult_Ok : bEResult_False;
}

bEResult HalDevice::IsResetNecessary(bool* a0) {
    if (a0 != nullptr) *a0 = true;  // eCDX9Device: true while the device is fine
    return bEResult_Ok;
}

bEResult HalDevice::IsResetNeeded(bool* a0) {
    if (a0 != nullptr) *a0 = false;
    return bEResult_Ok;
}

bEResult HalDevice::IsDeviceNotReset(bool* a0) {
    if (a0 != nullptr) *a0 = false;
    return bEResult_Ok;
}

bEResult HalDevice::Reset() { return bEResult_Ok; }

bEResult HalDevice::SetViewport(eCGfxShared::eSGfxViewport const& a0) {
    Viewport vp;
    vp.x = static_cast<std::uint32_t>(std::max(0, a0.left));
    vp.y = static_cast<std::uint32_t>(std::max(0, a0.top));
    const std::int32_t w = a0.right - a0.left, h = a0.bottom - a0.top;
    vp.width = w <= 0 ? 32u : static_cast<std::uint32_t>(w);  // eCDX9Device: a zero size becomes 32
    vp.height = h <= 0 ? 32u : static_cast<std::uint32_t>(h);
    vp.minZ = a0.minZ;
    vp.maxZ = a0.maxZ;
    dev_.setViewport(vp);
    return bEResult_Ok;
}

bEResult HalDevice::GetViewport(eCGfxShared::eSGfxViewport& a0) {
    const Viewport& vp = dev_.viewport();
    a0.left = static_cast<std::int32_t>(vp.x);
    a0.top = static_cast<std::int32_t>(vp.y);
    a0.right = static_cast<std::int32_t>(vp.x + vp.width);
    a0.bottom = static_cast<std::int32_t>(vp.y + vp.height);
    a0.minZ = vp.minZ;
    a0.maxZ = vp.maxZ;
    return bEResult_Ok;
}

bEResult HalDevice::ClearViewport(eCGfxShared::eEViewportClear a0, unsigned long a1, float a2, unsigned long a3) {
    HAL_LOG("hal> Clear flags {} color {:08x} z {}", static_cast<int>(a0), a1, a2);
    dev_.clear(static_cast<std::uint32_t>(a0), static_cast<std::uint32_t>(a1), a2, static_cast<std::uint32_t>(a3));
    return bEResult_Ok;
}

bEResult HalDevice::SwapScreen(HWND__*, bCRect const*, bCRect const*) {
    const std::uint64_t frame = dev_.stats().frames + 1;
    serviceSettings();
    if (overlay_) {
        overlay_->onPresent(frame);
        // [debug] overlay_in_screenshots: into the back buffer, before the captures read it
        dev_.drawOverlayIntoBackBuffer();
    }
    auto& captures = api::captures();
    // [debug] screenshot_frames: a request of the tool API's queue, due at this present
    if (std::ranges::find(options_.screenshotFrames, static_cast<std::uint32_t>(frame)) != options_.screenshotFrames.end()) {
        captures.request(options_.screenshotDir / std::format("frame_{}.png", frame), api::ImageFormat::Png, 0, nullptr);
    }
    // rvk_screenshot: the finished back buffer (scene + post + GUI) before it is shown, as D3D9's Present hook / DoSnapshot see it
    captures.onPresent(frame, [this](std::vector<std::uint8_t>& bgra, std::uint32_t& w, std::uint32_t& h) {
        return dev_.readBackBuffer(bgra, w, h);
    });
    HAL_LOG("hal> SwapScreen");
    dev_.present();
    const auto& s = dev_.stats();
    api::publishPresent(s.frames, api::FrameNumbers{
                                      .drawsLastFrame = s.lastFrameDraws,
                                      .unimplementedSlots = static_cast<std::uint32_t>(unimplementedCount()),
                                      .drawsTotal = s.totalDraws,
                                      .drawsSkippedTotal = s.skippedNoShader + s.skippedNoTarget + s.skippedVertexInput +
                                                           s.skippedPipeline,
                                      .textureCpuBytes = d3d9::Texture::cpuBytesTotal(),
                                  });
    if (const auto& cam = dev_.lastCamera(); cam.valid) {
        ++cameraFrames_;
        if (cam.prevValid && cam.position != cam.prevPosition) ++cameraMovedFrames_;
    }
    if (logThisFrame()) logFrameStructure();  // the -LogFrame frame, now complete
    updateLogFrame();
    logFrameStats();
    return bEResult_Ok;
}

// The last presented frame's phase sequence and main camera (docs/research/frame-phases.md), every 5 s and for -LogFrame
void HalDevice::logFrameStructure() {
    const auto& ph = dev_.phases();
    spdlog::info("hal: frame {} structure ({} draws{}): {}", dev_.stats().frames, ph.lastDraws(),
                 ph.unbalanced() != 0 ? std::format(", {} unbalanced markers", ph.unbalanced()) : std::string(),
                 d3d9::scene::PhaseTracker::summary(ph.lastFrame()));
    const auto& dr = dev_.drawRecords();
    spdlog::info("hal: frame {} draw kinds ({} records{}{}): {}", dev_.stats().frames, dr.last().size(),
                 dr.lastDropped() != 0 ? std::format(", {} dropped", dr.lastDropped()) : std::string(),
                 dr.lastHistogram().tinted != 0 ? std::format(", {} tinted", dr.lastHistogram().tinted) : std::string(),
                 dr.lastHistogram().summary());
    // untagged draws of camera views: a draw path without a hook (first three of the frame)
    int unknown = 0;
    for (const auto& r : dr.last()) {
        if (r.kind != draws::Kind::Unknown || unknown++ >= 3) continue;
        spdlog::info("hal:   untagged draw #{} in {}/{}: vs {:016x} ({}) ps {:016x} ({}), {} primitives, type {}, flags 0x{:x}, "
                     "vertex buffer {}#{}", r.index, d3d9::scene::name(r.view), d3d9::scene::name(r.pass), r.vsHash,
                     flagNames(r.vsFlags), r.psHash, flagNames(r.psFlags), r.primitives, r.primitiveType, r.flags,
                     r.vertexBuffer.id, r.vertexBuffer.generation);
    }
    const auto& c = dev_.lastCamera();
    if (!c.valid) {
        spdlog::info("hal: frame {}: no main view", dev_.stats().frames);
        return;
    }
    const float moved = c.prevValid ? std::hypot(c.position[0] - c.prevPosition[0], c.position[1] - c.prevPosition[1],
                                                 c.position[2] - c.prevPosition[2])
                                    : 0.0f;
    // the view direction: the inverse view's third row (bCMatrix rows = axes, D3D row-vector convention)
    spdlog::info("hal: camera of frame {}: position ({:.2f}, {:.2f}, {:.2f}), forward ({:.3f}, {:.3f}, {:.3f}), near {:.2f}, far {:.1f} "
                 "(engine {:.1f}), t {:.3f} s, moved {:.3f} since the previous frame{}; the camera moved in {} of {} frames with a "
                 "main view since the last report",
                 c.frame, c.position[0], c.position[1], c.position[2], c.inverseView.at(2, 0), c.inverseView.at(2, 1),
                 c.inverseView.at(2, 2), c.nearZ, c.farZ, c.engineFar, c.time, moved, c.prevValid ? "" : " (no previous camera)",
                 cameraMovedFrames_, cameraFrames_);
    cameraMovedFrames_ = 0;
    cameraFrames_ = 0;
    const d3d9::scene::CameraCheck k = d3d9::scene::checkCamera(c);
    if (k.available) {
        spdlog::info("hal: camera check against the first solid draw's VS constants: c0-c3 = view-projection {} max |d| {:.2g} "
                     "(matrix scale {:.3g}), c4-c7 inverse projection {:.2g}, c8-c10 inverse view {:.2g}, c11.xyz position {:.2g}, "
                     "c11.w {:.1f} (engine far {:.1f}, |d| {:.2g})",
                     k.transposed ? "(transposed)" : "(rows)", k.viewProjection, k.scale, k.inverseProjection, k.inverseView,
                     k.position, c.vsConstants[11][3], c.engineFar, k.farPlane);
    }
}

// The engine's snapshot (eCApplication::Snapshot / SnapshotEx -> Draw -> DoSnapshot, after the GUI and before SwapScreen).
// eCDX9Device: GetRenderTarget(0) -> D3DXSaveSurfaceToFileInMemory(D3DXIFF_BMP) -> eCArchiveManager file, whatever the
// extension (missing-slots.md 3). Here: read the back buffer, resolve the virtual path through the engine's VFS, refuse
// anything inside <game>\data (junctions into the game install), write the BMP D3DX writes (32 bpp, byte for byte).
bEResult HalDevice::SaveBackbufferToFile(char* const a0) {
    std::string error;
    const auto file = engine::physicalPath(a0, &error);
    if (!file) {
        spdlog::warn("hal: SaveBackbufferToFile('{}'): {}", a0 != nullptr ? a0 : "", error);
        return bEResult_False;
    }
    if (const auto refused = api::refuseDataPath(*file, api::gameDir())) {
        spdlog::warn("hal: SaveBackbufferToFile('{}'): refused, {}", a0, *refused);
        return bEResult_False;
    }
    if (dev_.renderTarget(0) != dev_.backBuffer()) {
        spdlog::warn("hal: SaveBackbufferToFile: render target 0 is not the back buffer; saving the back buffer");
    }
    std::vector<std::uint8_t> bgra;
    std::uint32_t w = 0, h = 0;
    if (!dev_.readBackBuffer(bgra, w, h)) {
        spdlog::warn("hal: SaveBackbufferToFile: back-buffer read-back failed");
        return bEResult_False;
    }
    const std::vector<std::uint8_t> bmp = api::encodeBmpLikeD3DX(bgra, w, h);
    if (bmp.empty() || !api::writeFileReplace(*file, bmp, &error)) {
        spdlog::warn("hal: SaveBackbufferToFile({}): {}", api::utf8(*file), error.empty() ? "encoding failed" : error);
        return bEResult_False;
    }
    spdlog::info("hal: snapshot '{}' -> {} ({}x{}, frame {})", a0, api::utf8(*file), w, h, dev_.stats().frames + 1);
    return bEResult_Ok;
}

void HalDevice::logFrameStats() {
    const auto& s = dev_.stats();
    const auto now = std::chrono::steady_clock::now();
    if (s.frames == 1 || now - lastStatsTime_ >= std::chrono::seconds(5)) {
        const double secs = std::chrono::duration<double>(now - lastStatsTime_).count();
        const double frames = std::max(1.0, static_cast<double>(s.frames - lastStatsFrame_));
        spdlog::info("hal: {:.1f} fps, {} draws in the last frame; presents {}, acquire failures {}, suboptimal {}; avg acquire "
                     "{:.2f} ms, present {:.2f} ms (present thread; render thread waited {:.2f} ms), pacing {:.2f} ms",
                     static_cast<double>(s.frames - lastStatsFrame_) / secs, s.lastFrameDraws, s.presentsQueued, s.acquireFailures,
                     s.presentSuboptimal, s.acquireMs / frames, s.presentMs / frames, s.presentWaitMs / frames, s.pacerMs / frames);
        dev_.resetTimings();
        if (dev_.perfEnabled()) {
            logPerf(dev_.takePerf());
        }
        lastStatsTime_ = now;
        lastStatsFrame_ = s.frames;
        logFrameStructure();
        spdlog::info("hal: frame {}: {} draws total, {} clears, {} render passes; skipped: {} no shader, {} no target, {} vertex "
                     "input, {} pipeline; HAL Draw calls {}; {} unimplemented slots seen; texture CPU copies {:.1f} MiB",
                     s.frames, s.totalDraws, s.totalClears, s.totalRenderPasses, s.skippedNoShader, s.skippedNoTarget,
                     s.skippedVertexInput, s.skippedPipeline, halDraws_, unimplementedCount(),
                     static_cast<double>(d3d9::Texture::cpuBytesTotal()) / (1024.0 * 1024.0));
    }
}

// [debug] perf: one line per stats interval (docs/research/performance.md explains the fields)
void HalDevice::logPerf(d3d9::PerfStats p) {
    if (p.frames == 0) {
        return;
    }
    const double n = static_cast<double>(p.frames);
    auto& iv = p.frameIntervalsMs;
    std::ranges::sort(iv);
    const auto pct = [&](double q) { return iv.empty() ? 0.0 : static_cast<double>(iv[std::min(iv.size() - 1, static_cast<std::size_t>(q * static_cast<double>(iv.size())))]); };
    const double other = p.cpuFrameMs - p.drawMs - p.readbackMs;
    spdlog::info("perf: {} frames, interval p50 {:.2f} / p95 {:.2f} / max {:.2f} ms; CPU per frame: engine+HAL {:.2f} ms (draw calls {:.2f}, "
                 "read-backs {:.2f} = {} per frame, engine+HAL rest {:.2f}), present {:.2f} ms (submit {:.2f}, slot wait {:.2f}); "
                 "GPU per frame {:.2f} ms (+ uploads {:.3f}) over {:.2f} submissions",
                 p.frames, pct(0.5), pct(0.95), iv.empty() ? 0.0 : static_cast<double>(iv.back()), p.cpuFrameMs / n, p.drawMs / n,
                 p.readbackMs / n, static_cast<double>(p.readbacks) / n, other / n, p.presentMs / n, p.submitMs / n, p.slotWaitMs / n,
                 p.gpuSubmits > 0 ? p.gpuMs / n : 0.0, p.gpuSubmits > 0 ? p.gpuUploadMs / n : 0.0, static_cast<double>(p.gpuSubmits) / n);
    // GPU memory of the renderer (VMA's heap statistics: cheap); the ray tracing tier's buffer usage flags show up here
    {
        const VkPhysicalDeviceMemoryProperties* mp = nullptr;
        vmaGetMemoryProperties(dev_.ctx().allocator(), &mp);
        VmaBudget budgets[VK_MAX_MEMORY_HEAPS]{};
        vmaGetHeapBudgets(dev_.ctx().allocator(), budgets);
        std::uint64_t blocks = 0, allocs = 0, count = 0, local = 0;
        for (std::uint32_t h = 0; h < mp->memoryHeapCount; ++h) {
            blocks += budgets[h].statistics.blockBytes;
            allocs += budgets[h].statistics.allocationBytes;
            count += budgets[h].statistics.allocationCount;
            if ((mp->memoryHeaps[h].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) != 0) local += budgets[h].statistics.blockBytes;
        }
        spdlog::info("perf: GPU memory {:.1f} MiB in blocks ({:.1f} MiB device-local), {:.1f} MiB in {} allocations; ray tracing tier {}",
                     static_cast<double>(blocks) / 1048576.0, static_cast<double>(local) / 1048576.0,
                     static_cast<double>(allocs) / 1048576.0, count, dev_.ctx().features().rayTracing.enabled ? "on" : "off");
    }
    spdlog::info("perf: draw tags + records {:.3f} ms per frame ({:.0f} ns per draw)", p.drawRecordMs / n,
                 p.draws > 0 ? p.drawRecordMs * 1e6 / static_cast<double>(p.draws) : 0.0);
    // enhancement plumbing (docs/research/injection.md): FrameBlock uploads, injection points that ran passes, their CPU time
    spdlog::info("perf: frame blocks {:.2f} per frame; injected passes: {:.2f} points per frame, {:.3f} ms per frame (CPU recording)",
                 static_cast<double>(p.frameBlocks) / n, static_cast<double>(p.injections) / n, p.injectMs / n);
    spdlog::info("perf: per frame {:.0f} draws, {:.1f} render passes, {:.0f} pipeline binds; ring {:.0f} KiB, texture uploads {:.0f} KiB, "
                 "buffer uploads {:.0f} KiB; in the interval: {} pipelines built ({:.1f} ms), {} shaders translated ({} compiled, {:.1f} ms)",
                 static_cast<double>(p.draws) / n, static_cast<double>(p.renderPasses) / n, static_cast<double>(p.pipelineBinds) / n,
                 static_cast<double>(p.ringBytes) / n / 1024.0, static_cast<double>(p.textureUploadBytes) / n / 1024.0,
                 static_cast<double>(p.bufferUploadBytes) / n / 1024.0, p.pipelinesBuilt, p.pipelineMs, p.shadersBuilt, p.shadersCompiled,
                 p.shaderMs);
    if (p.prewarmed > 0) {
        spdlog::info("perf: {} pipelines of earlier runs built in the background ({:.1f} ms)", p.prewarmed, p.prewarmMs);
    }
    if (p.libraries > 0 || p.optimized > 0) {
        spdlog::info("perf: pipeline libraries compiled {} ({:.1f} ms, mostly on loader threads), optimised pipelines {} ({:.1f} ms, "
                     "background)",
                     p.libraries, p.libraryMs, p.optimized, p.optimizeMs);
    }
    if (p.pacerRate > 0.0) {
        const double w = std::max(1.0, static_cast<double>(p.pacer.waits));
        spdlog::info("perf: pacer {:.3f} fps{}: {} of {} frames waited, avg sleep {:.2f} ms + spin {:.3f} ms, timer late avg {:.3f} / "
                     "max {:.3f} ms, release error avg {:.3f} ms, {} cadence restarts",
                     p.pacerRate, p.pacerVblank ? " (vblank-locked)" : "", p.pacer.waits, p.frames, p.pacer.sleptMs / w,
                     p.pacer.spunMs / w, p.pacer.lateMs / w, p.pacer.maxLateMs, p.pacer.releaseErrorMs / w, p.pacer.restarts);
    }
}

bEResult HalDevice::BeginScene() {
    HAL_LOG("hal> BeginScene");
    ++sceneDepth_;
    return bEResult_Ok;
}

bEResult HalDevice::EndScene() {
    if (sceneDepth_ > 0) --sceneDepth_;
    return bEResult_Ok;
}

// ---- master states (hal-structs.md 2.4) ----------------------------------------------------------------------------

bEResult HalDevice::SetMasterRenderMode(eCGfxShared::eEMasterRenderMode a0) {
    master_[M_RENDERMODE] = a0 != 0;
    if (a0 != 0) setRenderState(RS_FILLMODE, static_cast<std::uint32_t>(a0));
    return bEResult_Ok;
}
bEResult HalDevice::GetMasterRenderMode(eCGfxShared::eEMasterRenderMode* a0) {
    if (a0) *a0 = static_cast<eCGfxShared::eEMasterRenderMode>(master_[M_RENDERMODE] ? dev_.renderState(RS_FILLMODE) : 0);
    return bEResult_Ok;
}
bEResult HalDevice::SetMasterShadeMode(eCGfxShared::eEMasterShadeMode a0) {
    master_[M_SHADE] = a0 != 0;
    if (a0 != 0) setRenderState(RS_SHADEMODE, static_cast<std::uint32_t>(a0));
    return bEResult_Ok;
}
bEResult HalDevice::GetMasterShadeMode(eCGfxShared::eEMasterShadeMode* a0) {
    if (a0) *a0 = static_cast<eCGfxShared::eEMasterShadeMode>(master_[M_SHADE] ? dev_.renderState(RS_SHADEMODE) : 0);
    return bEResult_Ok;
}
bEResult HalDevice::SetMasterCullingMode(eCGfxShared::eEMasterCullingMode a0) {
    master_[M_CULL] = a0 != 0;
    if (a0 != 0) setRenderState(RS_CULLMODE, static_cast<std::uint32_t>(a0));
    return bEResult_Ok;
}
bEResult HalDevice::GetMasterCullingMode(eCGfxShared::eEMasterCullingMode* a0) {
    if (a0) *a0 = static_cast<eCGfxShared::eEMasterCullingMode>(master_[M_CULL] ? dev_.renderState(RS_CULLMODE) : 0);
    return bEResult_Ok;
}
bEResult HalDevice::SetMasterDepthTesting(eCGfxShared::eEMasterCmpFunc a0) {
    master_[M_DEPTHFUNC] = a0 != 0;
    if (a0 != 0) setRenderState(RS_ZFUNC, static_cast<std::uint32_t>(a0));
    return bEResult_Ok;
}
bEResult HalDevice::GetMasterDepthTesting(eCGfxShared::eEMasterCmpFunc* a0) {
    if (a0) *a0 = static_cast<eCGfxShared::eEMasterCmpFunc>(master_[M_DEPTHFUNC] ? dev_.renderState(RS_ZFUNC) : 0);
    return bEResult_Ok;
}
bEResult HalDevice::SetMasterDepthWrite(eCGfxShared::eEMasterWrite a0) {
    master_[M_DEPTHWRITE] = a0 != 0;
    if (a0 != 0) setRenderState(RS_ZWRITEENABLE, a0 == 2 ? 1u : 0u);
    return bEResult_Ok;
}
bEResult HalDevice::GetMasterDepthWrite(eCGfxShared::eEMasterWrite* a0) {
    if (a0) {
        *a0 = static_cast<eCGfxShared::eEMasterWrite>(master_[M_DEPTHWRITE] ? (dev_.renderState(RS_ZWRITEENABLE) == 1 ? 2 : 1) : 0);
    }
    return bEResult_Ok;
}
bEResult HalDevice::SetMasterDepthBias(bool a0, float a1, float a2) {
    HAL_LOG("hal> SetMasterDepthBias {} {} {}", a0, a1, a2);
    master_[M_DEPTHBIAS] = a0;
    if (a0) {
        setRenderState(RS_DEPTHBIAS, asDword(a1));
        setRenderState(RS_SLOPESCALEDEPTHBIAS, asDword(a2));
    }
    return bEResult_Ok;
}
bEResult HalDevice::GetMasterDepthBias(bool* a0, float*, float*) {
    if (a0) *a0 = master_[M_DEPTHBIAS];  // eCDX9Device returns no floats
    return bEResult_Ok;
}
void HalDevice::applyStencil(bool enable, const eCGfxShared::eSGfxStencilData& d) {
    setRenderState(RS_STENCILENABLE, enable ? 1u : 0u);
    if (enable) {
        setRenderState(RS_STENCILFAIL, d.failOp);
        setRenderState(RS_STENCILZFAIL, d.depthFailOp);
        setRenderState(RS_STENCILPASS, d.passOp);
        setRenderState(RS_STENCILFUNC, static_cast<std::uint32_t>(d.func));
        setRenderState(RS_STENCILREF, d.ref);
        setRenderState(RS_STENCILMASK, d.mask);
        setRenderState(RS_STENCILWRITEMASK, d.writeMask);
    }
}
bEResult HalDevice::SetMasterStencilTesting(eCGfxShared::eEMasterStencil a0, eCGfxShared::eSGfxStencilData const& a1) {
    master_[M_STENCIL] = a0 != 0;
    if (a0 != 0) applyStencil(a0 == 2, a1);
    return bEResult_Ok;
}
bEResult HalDevice::GetMasterStencilTesting(eCGfxShared::eEMasterStencil* a0, eCGfxShared::eSGfxStencilData* a1) {
    if (a0) *a0 = static_cast<eCGfxShared::eEMasterStencil>(master_[M_STENCIL] ? (dev_.renderState(RS_STENCILENABLE) ? 2 : 1) : 0);
    if (a1 && master_[M_STENCIL]) {
        a1->failOp = dev_.renderState(RS_STENCILFAIL);
        a1->depthFailOp = dev_.renderState(RS_STENCILZFAIL);
        a1->passOp = dev_.renderState(RS_STENCILPASS);
        a1->func = static_cast<eCGfxShared::eECmpFunc>(dev_.renderState(RS_STENCILFUNC));
        a1->ref = dev_.renderState(RS_STENCILREF);
        a1->mask = dev_.renderState(RS_STENCILMASK);
        a1->writeMask = dev_.renderState(RS_STENCILWRITEMASK);
    }
    return bEResult_Ok;
}
bEResult HalDevice::SetMasterBlending(eCGfxShared::eEMasterBlending a0, eCGfxShared::eEMasterBlending a1) {
    master_[M_BLEND] = a0 != 0 && a1 != 0;
    if (master_[M_BLEND]) {
        setRenderState(RS_SRCBLEND, static_cast<std::uint32_t>(a0));
        setRenderState(RS_DESTBLEND, static_cast<std::uint32_t>(a1));
    }
    return bEResult_Ok;
}
bEResult HalDevice::GetMasterBlending(eCGfxShared::eEMasterBlending* a0, eCGfxShared::eEMasterBlending* a1) {
    if (a0) *a0 = static_cast<eCGfxShared::eEMasterBlending>(master_[M_BLEND] ? dev_.renderState(RS_SRCBLEND) : 0);
    if (a1) *a1 = static_cast<eCGfxShared::eEMasterBlending>(master_[M_BLEND] ? dev_.renderState(RS_DESTBLEND) : 0);
    return bEResult_Ok;
}
void HalDevice::applyAlphaTest(bool enable, const eCGfxShared::eSGfxAlphaTestData& d) {
    setRenderState(RS_ALPHATESTENABLE, enable ? 1u : 0u);
    if (enable) {
        setRenderState(RS_ALPHAREF, d.ref);
        setRenderState(RS_ALPHAFUNC, static_cast<std::uint32_t>(d.func));
    }
}
bEResult HalDevice::SetMasterAlphaTesting(eCGfxShared::eEMasterAlphaTest a0, eCGfxShared::eSGfxAlphaTestData const& a1) {
    master_[M_ALPHATEST] = a0 != 0;
    if (a0 != 0) applyAlphaTest(a0 == 2, a1);
    return bEResult_Ok;
}
bEResult HalDevice::GetMasterAlphaTesting(eCGfxShared::eEMasterAlphaTest* a0, eCGfxShared::eSGfxAlphaTestData* a1) {
    if (a0) {
        *a0 = static_cast<eCGfxShared::eEMasterAlphaTest>(master_[M_ALPHATEST] ? (dev_.renderState(RS_ALPHATESTENABLE) ? 2 : 1) : 0);
    }
    if (a1 && master_[M_ALPHATEST]) {
        a1->ref = dev_.renderState(RS_ALPHAREF);
        a1->func = static_cast<eCGfxShared::eECmpFunc>(dev_.renderState(RS_ALPHAFUNC));
    }
    return bEResult_Ok;
}
bEResult HalDevice::SetMasterFogging(eCGfxShared::eEMasterFogging a0) {
    master_[M_FOG] = a0 == 1;
    if (a0 == 1) setRenderState(RS_FOGENABLE, 0);
    return bEResult_Ok;
}
bEResult HalDevice::GetMasterFogging(eCGfxShared::eEMasterFogging* a0) {
    if (a0) *a0 = static_cast<eCGfxShared::eEMasterFogging>(master_[M_FOG] ? 1 : 0);
    return bEResult_Ok;
}

// ---- normal setters (no-ops while their master flag is set) --------------------------------------------------------

bEResult HalDevice::SetRenderMode(eCGfxShared::eERenderMode a0) {
    if (!master_[M_RENDERMODE]) normal(RS_FILLMODE, static_cast<std::uint32_t>(a0));
    return bEResult_Ok;
}
bEResult HalDevice::SetShadeMode(eCGfxShared::eEShadeMode a0) {
    if (!master_[M_SHADE]) normal(RS_SHADEMODE, static_cast<std::uint32_t>(a0));
    return bEResult_Ok;
}
bEResult HalDevice::SetCullingMode(eCGfxShared::eECullingMode a0) {
    if (!master_[M_CULL]) normal(RS_CULLMODE, static_cast<std::uint32_t>(a0));
    return bEResult_Ok;
}
bEResult HalDevice::SetDepthTesting(eCGfxShared::eECmpFunc a0) {
    if (!master_[M_DEPTHFUNC]) normal(RS_ZFUNC, static_cast<std::uint32_t>(a0));
    return bEResult_Ok;
}
bEResult HalDevice::SetDepthWrite(eCGfxShared::eEWrite a0) {
    if (!master_[M_DEPTHWRITE]) normal(RS_ZWRITEENABLE, a0 == 2 ? 1u : 0u);
    return bEResult_Ok;
}
bEResult HalDevice::SetDepthBias(float a0, float a1) {
    HAL_LOG("hal> SetDepthBias {} {}{}", a0, a1, master_[M_DEPTHBIAS] ? " (master set)" : "");
    if (!master_[M_DEPTHBIAS]) {
        normal(RS_DEPTHBIAS, asDword(a0));
        normal(RS_SLOPESCALEDEPTHBIAS, asDword(a1));
    }
    return bEResult_Ok;
}
bEResult HalDevice::SetStencilTesting(eCGfxShared::eEStencil a0, eCGfxShared::eSGfxStencilData const& a1) {
    if (!master_[M_STENCIL]) applyStencil(a0 == 2, a1);
    return bEResult_Ok;
}
bEResult HalDevice::SetBlending(eCGfxShared::eEBlending a0, eCGfxShared::eEBlending a1) {
    if (!master_[M_BLEND]) {
        normal(RS_SRCBLEND, static_cast<std::uint32_t>(a0));
        normal(RS_DESTBLEND, static_cast<std::uint32_t>(a1));
    }
    return bEResult_Ok;
}
bEResult HalDevice::SetAlphaTesting(eCGfxShared::eEAlphaTest a0, eCGfxShared::eSGfxAlphaTestData const& a1) {
    if (!master_[M_ALPHATEST]) applyAlphaTest(a0 == 2, a1);
    return bEResult_Ok;
}
bEResult HalDevice::SetFog(eCGfxShared::eSGfxFogData& a0) {
    if (master_[M_FOG]) return bEResult_Ok;
    normal(RS_FOGENABLE, a0.enable ? 1u : 0u);
    if (a0.enable) {
        normal(RS_RANGEFOGENABLE, a0.rangeFog ? 1u : 0u);
        normal(RS_FOGCOLOR, a0.color);
        normal(a0.vertexFog == 1 ? RS_FOGVERTEXMODE : RS_FOGTABLEMODE, a0.mode);
        normal(a0.vertexFog == 1 ? RS_FOGTABLEMODE : RS_FOGVERTEXMODE, 0);
        normal(RS_FOGSTART, asDword(a0.start));
        normal(RS_FOGEND, asDword(a0.end));
        normal(RS_FOGDENSITY, asDword(a0.density));
    }
    return bEResult_Ok;
}
bEResult HalDevice::SetColorFactor(unsigned long a0) {
    normal(RS_TEXTUREFACTOR, static_cast<std::uint32_t>(a0));
    return bEResult_Ok;
}
bEResult HalDevice::SetLighting(bool a0) {
    normal(RS_LIGHTING, a0 ? 1u : 0u);
    return bEResult_Ok;
}
bEResult HalDevice::GetLighting(bool* a0) {
    if (a0) *a0 = dev_.renderState(RS_LIGHTING) != 0;
    return bEResult_Ok;
}
bEResult HalDevice::SetAlphaBlending(bool a0) {
    normal(RS_ALPHABLENDENABLE, a0 ? 1u : 0u);
    return bEResult_Ok;
}
bEResult HalDevice::GetAlphaBlending(bool* a0) {
    if (a0) *a0 = dev_.renderState(RS_ALPHABLENDENABLE) != 0;
    return bEResult_Ok;
}
bEResult HalDevice::SetColorWriting(bool a0) {
    normal(RS_COLORWRITEENABLE, (dev_.renderState(RS_COLORWRITEENABLE) & 8u) | (a0 ? 7u : 0u));
    return bEResult_Ok;
}
bEResult HalDevice::GetColorWriting(bool* a0) {
    if (a0) *a0 = (dev_.renderState(RS_COLORWRITEENABLE) & 7u) != 0;
    return bEResult_Ok;
}
bEResult HalDevice::SetAlphaWriting(bool a0) {
    normal(RS_COLORWRITEENABLE, (dev_.renderState(RS_COLORWRITEENABLE) & 7u) | (a0 ? 8u : 0u));
    return bEResult_Ok;
}
bEResult HalDevice::GetAlphaWriting(bool* a0) {
    if (a0) *a0 = (dev_.renderState(RS_COLORWRITEENABLE) & 8u) != 0;
    return bEResult_Ok;
}
bEResult HalDevice::SetSpecularEffect(bool a0) {
    normal(RS_SPECULARENABLE, a0 ? 1u : 0u);
    return bEResult_Ok;
}
bEResult HalDevice::GetSpecularEffect(bool* a0) {
    if (a0) *a0 = dev_.renderState(RS_SPECULARENABLE) != 0;
    return bEResult_Ok;
}
bEResult HalDevice::SetGammaRamp(bool, eCGfxShared::eSGfxGammaRamp const* a1) {
    if (a1) gamma_ = *a1;  // windowed D3D9 ignores the gamma ramp as well
    return bEResult_Ok;
}
bEResult HalDevice::GetGammaRamp(eCGfxShared::eSGfxGammaRamp* a0) {
    if (a0) *a0 = gamma_;
    return bEResult_Ok;
}
bEResult HalDevice::EnableClipping(bool a0) {
    normal(RS_CLIPPING, a0 ? 1u : 0u);
    return bEResult_Ok;
}
bEResult HalDevice::GetClippingEnables(bool& a0) {
    a0 = dev_.renderState(RS_CLIPPING) != 0;
    return bEResult_Ok;
}
bEResult HalDevice::SetScissor(bCRect const& a0) {
    dev_.setScissorRect(a0.left, a0.top, a0.right, a0.bottom);
    return bEResult_Ok;
}
bEResult HalDevice::GetScissor(bCRect& a0) {
    a0 = {0, 0, static_cast<std::int32_t>(dev_.viewport().width), static_cast<std::int32_t>(dev_.viewport().height)};
    return bEResult_Ok;
}
bEResult HalDevice::EnableScissor(bool a0) {
    normal(RS_SCISSORTESTENABLE, a0 ? 1u : 0u);
    return bEResult_Ok;
}
bool HalDevice::IsScissorEnabled() { return dev_.renderState(RS_SCISSORTESTENABLE) != 0; }
bEResult HalDevice::SetClipPlane(unsigned long a0) {
    normal(RS_CLIPPLANEENABLE, static_cast<std::uint32_t>(a0));
    if (a0 != 0) {
        static bool logged = false;
        if (!logged) {
            logged = true;
            spdlog::warn("hal: user clip planes (mask 0x{:x}) are not emulated", a0);
        }
    }
    return bEResult_Ok;
}
bEResult HalDevice::SetClipPlaneData(unsigned long a0, eCGfxShared::eSGfxPlaneData& a1) {
    if (a0 < 6) std::memcpy(clipPlanes_[a0], &a1, 16);
    return bEResult_Ok;
}
bEResult HalDevice::GetClipPlaneData(unsigned long a0, eCGfxShared::eSGfxPlaneData& a1) {
    if (a0 < 6) std::memcpy(&a1, clipPlanes_[a0], 16);
    return bEResult_Ok;
}

void HalDevice::BeginRenderStateBlock() {
    blockRecording_ = true;
    blockSaved_.fill(false);
}

void HalDevice::EndRenderStateBlock() {
    blockRecording_ = false;
    for (std::uint32_t rs = 0; rs < RS_COUNT; ++rs) {
        if (blockSaved_[rs]) {
            dev_.setRenderState(rs, blockValues_[rs]);
        }
    }
    blockSaved_.fill(false);
}

// ---- transforms (fixed function only: stored for the getters) -------------------------------------------------------

bEResult HalDevice::SetViewAndProjectionMatrices(bCMatrix const* a0, bCMatrix const* a1) {
    if (a0) view_ = *a0;
    if (a1) projection_ = *a1;
    return bEResult_Ok;
}
bEResult HalDevice::SetWorldMatrix(bCMatrix const* a0) {
    if (a0) world_ = *a0;
    return bEResult_Ok;
}
bEResult HalDevice::GetWorldMatrix(bCMatrix* a0) {
    if (a0) *a0 = world_;
    return bEResult_Ok;
}
bEResult HalDevice::SetViewMatrix(bCMatrix const* a0) {
    if (a0) view_ = *a0;
    return bEResult_Ok;
}
bEResult HalDevice::GetViewMatrix(bCMatrix* a0) {
    if (a0) *a0 = view_;
    return bEResult_Ok;
}

// ---- draws ----------------------------------------------------------------------------------------------------------

bEResult HalDevice::Draw(eCGfxShared::eSGfxDrawCallDesc const& a0) {
    ++halDraws_;
    HAL_LOG("hal> Draw indexed {} type {} prims {} start {}/{} base {}", a0.indexed, static_cast<int>(a0.type), a0.primitiveCount,
            a0.startVertex, a0.startIndex, a0.baseVertexIndex);
    const auto type = static_cast<std::uint32_t>(a0.type);
    if (a0.indexed) {
        dev_.drawIndexedPrimitive(type, a0.baseVertexIndex, a0.minVertexIndex, a0.numVertices, a0.startIndex, a0.primitiveCount);
    } else {
        dev_.drawPrimitive(type, a0.startVertex, a0.primitiveCount);
    }
    return bEResult_Ok;
}

bEResult HalDevice::DrawUP(eCGfxShared::eEPrimitiveType a0, unsigned int a1, void* const a2, unsigned int a3) {
    HAL_LOG("hal> DrawUP type {} prims {} stride {}", static_cast<int>(a0), a1, a3);
    dev_.drawPrimitiveUP(static_cast<std::uint32_t>(a0), a1, a2, a3);
    return bEResult_Ok;
}

bEResult HalDevice::DrawIndexedUP(eCGfxShared::eEPrimitiveType a0, unsigned int a1, unsigned int a2, unsigned int a3, void* const a4,
                                  void* const a5, unsigned int a6) {
    // always 16-bit indices (eCDX9Device::DrawIndexedUP 0x742160)
    dev_.drawIndexedPrimitiveUP(static_cast<std::uint32_t>(a0), a1, a2, a3, a4, false, a5, a6);
    return bEResult_Ok;
}

// ---- constants ------------------------------------------------------------------------------------------------------

bEResult HalDevice::SetVertexProgramRegisterF(unsigned long a0, float const* a1, unsigned long a2) {
    dev_.setVertexShaderConstantF(static_cast<std::uint32_t>(a0), a1, static_cast<std::uint32_t>(a2));
    return bEResult_Ok;
}
bEResult HalDevice::SetVertexProgramRegisterI(unsigned long a0, int const* a1, unsigned long a2) {
    dev_.setVertexShaderConstantI(static_cast<std::uint32_t>(a0), a1, static_cast<std::uint32_t>(a2));
    return bEResult_Ok;
}
bEResult HalDevice::SetVertexProgramRegisterB(unsigned long a0, int const* a1, unsigned long a2) {
    dev_.setVertexShaderConstantB(static_cast<std::uint32_t>(a0), a1, static_cast<std::uint32_t>(a2));
    return bEResult_Ok;
}
bEResult HalDevice::GetVertexProgramRegisterF(unsigned long a0, float* a1, unsigned long a2) {
    dev_.getVertexShaderConstantF(static_cast<std::uint32_t>(a0), a1, static_cast<std::uint32_t>(a2));
    return bEResult_Ok;
}
bEResult HalDevice::SetPixelProgramRegisterF(unsigned long a0, float const* a1, unsigned long a2) {
    dev_.setPixelShaderConstantF(static_cast<std::uint32_t>(a0), a1, static_cast<std::uint32_t>(a2));
    return bEResult_Ok;
}
bEResult HalDevice::SetPixelProgramRegisterI(unsigned long a0, int const* a1, unsigned long a2) {
    dev_.setPixelShaderConstantI(static_cast<std::uint32_t>(a0), a1, static_cast<std::uint32_t>(a2));
    return bEResult_Ok;
}
bEResult HalDevice::SetPixelProgramRegisterB(unsigned long a0, int const* a1, unsigned long a2) {
    dev_.setPixelShaderConstantB(static_cast<std::uint32_t>(a0), a1, static_cast<std::uint32_t>(a2));
    return bEResult_Ok;
}
bEResult HalDevice::GetPixelProgramRegisterF(unsigned long a0, float* a1, unsigned long a2) {
    dev_.getPixelShaderConstantF(static_cast<std::uint32_t>(a0), a1, static_cast<std::uint32_t>(a2));
    return bEResult_Ok;
}

// ---- samplers -------------------------------------------------------------------------------------------------------

bEResult HalDevice::SetTextureAdressing(unsigned long a0, eCGfxShared::eETexAddress a1, eCGfxShared::eETexAddress a2,
                                        eCGfxShared::eETexAddress a3) {
    const auto s = static_cast<std::uint32_t>(a0);
    dev_.setSamplerState(s, SAMP_ADDRESSU, static_cast<std::uint32_t>(a1));
    dev_.setSamplerState(s, SAMP_ADDRESSV, static_cast<std::uint32_t>(a2));
    dev_.setSamplerState(s, SAMP_ADDRESSW, static_cast<std::uint32_t>(a3));
    return bEResult_Ok;
}

bEResult HalDevice::SetTextureFiltering(unsigned long a0, eCGfxShared::eETexFilter a1, eCGfxShared::eETexFilter a2,
                                        eCGfxShared::eETexFilter a3) {
    const auto s = static_cast<std::uint32_t>(a0);
    dev_.setSamplerState(s, SAMP_MINFILTER, static_cast<std::uint32_t>(a1));  // argument order min, mag, mip
    dev_.setSamplerState(s, SAMP_MAGFILTER, static_cast<std::uint32_t>(a2));
    dev_.setSamplerState(s, SAMP_MIPFILTER, static_cast<std::uint32_t>(a3));
    return bEResult_Ok;
}

bEResult HalDevice::SetSamplerState(unsigned long a0, eCGfxShared::eESamplerState a1, unsigned long a2) {
    dev_.setSamplerState(static_cast<std::uint32_t>(a0), static_cast<std::uint32_t>(a1), static_cast<std::uint32_t>(a2));
    return bEResult_Ok;
}

bEResult HalDevice::SetTextureNULL(unsigned long a0) {
    dev_.setTexture(static_cast<std::uint32_t>(a0), nullptr);
    return bEResult_Ok;
}
bEResult HalDevice::SetIndexDataNULL() {
    dev_.setIndices(nullptr);
    return bEResult_Ok;
}
bEResult HalDevice::SetVertexDataNULL(unsigned long a0) {
    dev_.setStreamSource(static_cast<std::uint32_t>(a0), nullptr, 0, 0);
    return bEResult_Ok;
}

// ---- render targets (hal-structs.md 2.2) -----------------------------------------------------------------------------

bEResult HalDevice::SetRenderTarget(void* a0, void* a1) {
    HAL_LOG("hal> SetRenderTarget {} {} (back buffer {})", a0, a1, static_cast<void*>(dev_.backBuffer()));
    unbindTextures();
    auto* rt = static_cast<d3d9::Surface*>(a0);
    dev_.setRenderTarget(0, rt);
    dev_.setDepthStencil(static_cast<d3d9::Surface*>(a1));
    // eCSkydome_PS::UpdateSkydomeEnvCubemap binds a face of the sky cube (frame-phases.md 4)
    if (rt != nullptr && rt->texture() != nullptr && rt->texture()->desc().type == d3d9::TextureType::Cube &&
        dev_.phases().view() == d3d9::scene::View::SkyCube) {
        dev_.tagImage(rt->texture(), d3d9::scene::ImageRole::SkyEnvCube);
    }
    return bEResult_Ok;
}

bEResult HalDevice::SetRenderTargetAt(void* a0, int a1) {
    if (a1 < 0 || a1 >= static_cast<int>(kRenderTargets)) return bEResult_False;
    unbindTextures();
    dev_.setRenderTarget(static_cast<std::uint32_t>(a1), static_cast<d3d9::Surface*>(a0));
    return bEResult_Ok;
}

bEResult HalDevice::GetRenderTargetAt(void** a0, int a1) {
    if (a0 == nullptr || a1 < 0 || a1 >= static_cast<int>(kRenderTargets)) return bEResult_False;
    *a0 = dev_.renderTarget(static_cast<std::uint32_t>(a1));
    return bEResult_Ok;
}

bEResult HalDevice::GetRenderTarget(void** a0, void** a1) {
    if (a0) *a0 = dev_.renderTarget(0);
    if (a1) *a1 = dev_.depthStencil();
    return bEResult_Ok;
}

bEResult HalDevice::GetBackBuffer(void** a0, void** a1) {
    if (a0) *a0 = dev_.backBuffer();
    if (a1) *a1 = dev_.autoDepthStencil();
    return bEResult_Ok;
}

// ---- occlusion queries: not emulated yet, everything is visible -----------------------------------------------------

bEResult HalDevice::CreateOcclusionQuery(void*& a0) {
    a0 = reinterpret_cast<void*>(static_cast<std::uintptr_t>(nextQuery_++));
    return bEResult_Ok;
}
bEResult HalDevice::DestroyOcclusionQuery(void* const) { return bEResult_Ok; }
bEResult HalDevice::BeginOcclusionTest(void* const) { return bEResult_Ok; }
bEResult HalDevice::EndOcclusionTest(void* const) { return bEResult_Ok; }
bEResult HalDevice::GetOcclusionTestInfo(void* const, unsigned long& a1, bool) {
    a1 = 100000;  // "visible"
    return bEResult_Ok;
}

}  // namespace rvk::hal::vulkan
