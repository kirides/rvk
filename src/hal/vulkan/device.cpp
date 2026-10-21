// eCAPIDevice on d3d9::Device (docs/research/hal-structs.md sections 2.x).

#include <atomic>
#include <chrono>
#include <cstring>

#include <spdlog/spdlog.h>

#include "core/exports.h"
#include "core/png.h"
#include "hal/vulkan/hal_vulkan.h"
#include "res/dds.h"
#include "vk/formats.h"

namespace rvk::hal::vulkan {

using namespace rvk::d3d9;

namespace {
std::atomic<bool> g_logFrame{false};
}  // namespace

bool logThisFrame() { return g_logFrame.load(std::memory_order_relaxed); }

#define HAL_LOG(...)                                       do {                                                       if (logThisFrame()) spdlog::info(__VA_ARGS__);     } while (0)

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
    {
        const std::lock_guard lock(handleMutex_);
        for (auto& [h, d] : decls_) dev_.release(std::move(d));
        for (auto& [h, t] : depthBuffers_) dev_.release(std::move(t));
        decls_.clear();
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
    o.vsync = p.vsync;
    o.depthFormat = static_cast<std::uint32_t>(p.depthFormat);
    o.validation = options.validation;
    o.adapter = options.adapter;
    o.cacheDir = options.cacheDir;
    o.logDraws = options.logDraws;
    if (!p.windowed) {
        spdlog::warn("hal: fullscreen requested, running windowed (exclusive fullscreen is not supported yet)");
    }
    if (!dev_.create(o)) {
        return false;
    }
    spdlog::info("hal: Vulkan device for window {} ({}x{}, vsync {}, depth {})", static_cast<void*>(p.window), o.width, o.height,
                 o.vsync, vk::d3dFormatName(o.depthFormat));
    return true;
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

bEResult HalDevice::CreateVertexData(eCGfxShared::eSGfxVertexBufDesc const& a0, eCAPIVertexData** a1) {
    if (a1 == nullptr) return bEResult_False;
    if (a0.fvf != 0) {
        spdlog::warn("hal: FVF vertex buffer (0x{:x}): stride from the description", a0.fvf);
    }
    auto b = dev_.createBuffer(a0.length, a0.usage, a0.pool, false, false);
    *a1 = new HalVertexData(*this, a0, std::move(b));
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

bEResult HalDevice::CreateImageFromMemory(eCGfxShared::eEPoolType a0, int a1, unsigned char a2, void const* a3, unsigned int a4,
                                          eCGfxShared::eSGfxLayersDesc& a5, void*& a6) {
    a6 = nullptr;
    std::string err;
    const auto dds = res::parseDds({static_cast<const std::uint8_t*>(a3), a4}, &err);
    if (!dds) {
        static int logged = 0;
        if (logged++ < 10) spdlog::warn("hal: CreateImageFromMemory: {} ({} bytes)", err, a4);
        return bEResult_False;
    }
    if (dds->volume) {
        spdlog::warn("hal: volume DDS files are not supported");
        return bEResult_False;
    }
    // levels -1 / -2: from the file for DDS (eCDX9Device::CreateImageFromMemory 0x73f030); skip a2 top levels
    std::uint32_t skip = a2;
    if (skip >= dds->mips) skip = dds->mips - 1;
    std::uint32_t levels = dds->mips - skip;
    if (a1 > 0) levels = std::min(levels, static_cast<std::uint32_t>(a1));
    TextureDesc td;
    td.type = dds->cube ? TextureType::Cube : TextureType::Tex2D;
    td.width = std::max(1u, dds->width >> skip);
    td.height = std::max(1u, dds->height >> skip);
    td.levels = levels;
    td.format = dds->d3dFormat;
    td.pool = static_cast<std::uint32_t>(a0);
    auto t = dev_.createTexture(td);
    if (!t) {
        return bEResult_False;
    }
    for (std::uint32_t face = 0; face < dds->faces; ++face) {
        for (std::uint32_t l = 0; l < levels; ++l) {
            if (const auto lv = dds->level(face, l + skip)) {
                t->setLevelData(l, face, lv->bytes, lv->rowPitch);
            }
        }
    }
    a5.width = td.width;
    a5.height = td.height;
    a5.levels = levels;
    a5.format = td.format;
    a5.pool = td.pool;
    a5.cubeMap = dds->cube;
    if (dds->cube) {
        a6 = static_cast<eCAPICubeMap*>(new HalCubeMap(*this, std::move(t)));
    } else {
        a6 = static_cast<eCAPITexture*>(new HalTexture(*this, std::move(t)));
    }
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
    using namespace vk::d3dfmt;
    const std::uint32_t f = a0.format;
    // shadow-map choice (eCDepthMap::SetupImageDesc): no depth textures (they would need fetch4 / hardware-PCF emulation),
    // so the engine renders colour shadow maps and compares in the shader
    if (f == DF16 || f == DF24 || f == INTZ || (a0.usage & USAGE_DEPTHSTENCIL) != 0) {
        return bEResult_False;
    }
    if (f == NULLFMT) {
        return bEResult_Ok;
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
    if (std::ranges::find(options_.screenshotFrames, static_cast<std::uint32_t>(frame)) != options_.screenshotFrames.end()) {
        std::vector<std::uint8_t> bgra;
        std::uint32_t w = 0, h = 0;
        const auto file = options_.screenshotDir / std::format("frame_{}.png", frame);
        if (dev_.readBackBuffer(bgra, w, h) && writePngBgra(file, bgra, w, h)) {
            spdlog::info("hal: screenshot {}", file.string());
        }
    }
    HAL_LOG("hal> SwapScreen");
    dev_.present();
    updateLogFrame();
    logFrameStats();
    return bEResult_Ok;
}

void HalDevice::logFrameStats() {
    const auto& s = dev_.stats();
    const auto now = std::chrono::steady_clock::now();
    if (s.frames == 1 || now - lastStatsTime_ >= std::chrono::seconds(5)) {
        const double secs = std::chrono::duration<double>(now - lastStatsTime_).count();
        spdlog::info("hal: {:.1f} fps, {} draws in the last frame", static_cast<double>(s.frames - lastStatsFrame_) / secs, s.lastFrameDraws);
        lastStatsTime_ = now;
        lastStatsFrame_ = s.frames;
        spdlog::info("hal: frame {}: {} draws total, {} clears, {} render passes; skipped: {} no shader, {} no target, {} vertex "
                     "input, {} pipeline; HAL Draw calls {}; {} unimplemented slots seen",
                     s.frames, s.totalDraws, s.totalClears, s.totalRenderPasses, s.skippedNoShader, s.skippedNoTarget,
                     s.skippedVertexInput, s.skippedPipeline, halDraws_, unimplementedCount());
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
    dev_.setRenderTarget(0, static_cast<d3d9::Surface*>(a0));
    dev_.setDepthStencil(static_cast<d3d9::Surface*>(a1));
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
