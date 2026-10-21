// Tracing passthrough (mode = "trace", PLAN.md M1): our wrappers sit between the engine and its D3D9 HAL (eCDX9*), count and
// log every call, and capture data the Vulkan backend needs (shader bytecode, effect sources, vertex declarations, texture
// formats). Wrapping is also the live check of the generated vtable layouts: a wrong slot or signature crashes at once.

#include "hal/trace.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <fstream>
#include <mutex>
#include <set>
#include <shared_mutex>
#include <unordered_map>
#include <vector>

#include <spdlog/spdlog.h>

#include "hal/trace_generated.h"
#include "shader/sm_tokens.h"

namespace rvk::hal::trace {
namespace {

Options g_options;
std::array<std::array<SlotStats, kMaxSlots>, kMaxInterfaces> g_stats;
std::atomic<int> g_logFirst{3};

std::shared_mutex g_mapMutex;
std::unordered_map<void*, void*> g_innerOfWrapper;  // wrapper -> engine object
std::unordered_map<void*, void*> g_wrapperOfInner;  // engine object -> wrapper

const char* slotName(int iface, int slot) {
    static const std::array<std::span<const char* const>, kInterfaceCount> tables = {
        keCAPIVertexDataSlots, keCAPIIndexDataSlots, keCAPITextureSlots,   keCAPICubeMapSlots,
        keCAPIVolumeSlots,     keCAPIPrimitiveSlots, keCAPIHLSLShaderSlots, keCAPIEffectSlots,
        keCAPISpriteSlots,     keCAPIFontSlots,      keCAPIDeviceSlots,    keCAPIInstanceSlots,
    };
    if (iface < 0 || iface >= kInterfaceCount || slot < 0 || static_cast<std::size_t>(slot) >= tables[iface].size()) {
        return "?";
    }
    return tables[iface][static_cast<std::size_t>(slot)];
}

std::uint64_t fnv1a(const void* data, std::size_t size) {
    std::uint64_t h = 0xcbf29ce484222325ull;
    const auto* p = static_cast<const unsigned char*>(data);
    for (std::size_t i = 0; i < size; ++i) {
        h = (h ^ p[i]) * 0x100000001b3ull;
    }
    return h;
}

// ---- capture ---------------------------------------------------------------------------------------------------------

std::mutex g_captureMutex;
std::set<std::uint64_t> g_captured;

void captureBlob(const char* prefix, const void* data, std::size_t size) {
    if (!g_options.capture || data == nullptr || size == 0) {
        return;
    }
    const std::uint64_t h = fnv1a(data, size);
    {
        const std::lock_guard lock(g_captureMutex);
        if (!g_captured.insert(h).second) {
            return;
        }
    }
    std::error_code ec;
    std::filesystem::create_directories(g_options.captureDir, ec);
    const auto path = g_options.captureDir / std::format("{}_{:016x}.bin", prefix, h);
    if (std::filesystem::exists(path, ec)) {
        return;
    }
    std::ofstream out(path, std::ios::binary);
    out.write(static_cast<const char*>(data), static_cast<std::streamsize>(size));
}

void captureShader(const char* prefix, const void* bytecode) {
    if (bytecode == nullptr) {
        return;
    }
    const std::size_t len = shader::byteLength(static_cast<const std::uint32_t*>(bytecode), 1u << 20);
    if (len == 0) {
        spdlog::warn("trace: {} bytecode without END token at {}", prefix, bytecode);
        return;
    }
    captureBlob(prefix, bytecode, len);
}

// D3DVERTEXELEMENT9: WORD Stream, WORD Offset, BYTE Type, BYTE Method, BYTE Usage, BYTE UsageIndex; ends with Stream 0xFF
std::string describeDecl(const void* elements) {
    std::string s;
    const auto* e = static_cast<const unsigned char*>(elements);
    for (int i = 0; i < 64; ++i, e += 8) {
        const auto stream = static_cast<unsigned>(e[0] | (e[1] << 8));
        if (stream == 0xFF) {
            break;
        }
        const auto offset = static_cast<unsigned>(e[2] | (e[3] << 8));
        s += std::format("{}[s{} +{} type{} m{} usage{}.{}]", s.empty() ? "" : " ", stream, offset, e[4], e[5], e[6], e[7]);
    }
    return s;
}

std::mutex g_seenMutex;
std::set<std::string> g_seen;  // unique decls / texture descs already logged

void logOnce(const std::string& what) {
    {
        const std::lock_guard lock(g_seenMutex);
        if (!g_seen.insert(what).second) {
            return;
        }
    }
    spdlog::info("trace: {}", what);
}

std::string describeLayers(const void* desc) {
    // eSGfxLayersDesc (engine-gfx.md 5): +0 width, +4 height, +8 ?, +0xc flag, +0x10 levels, +0x14 usage, +0x18 format, +0x1c pool
    const auto* d = static_cast<const std::uint32_t*>(desc);
    return std::format("{}x{} [+8]={} [+c]={} levels={} usage={} format={} pool={}", d[0], d[1], d[2], d[3], d[4], d[5], d[6], d[7]);
}

// ---- the capturing device ------------------------------------------------------------------------------------------

class CaptureDevice final : public TraceDevice {
public:
    using TraceDevice::TraceDevice;

    bEResult CreateVertexProgram(void* a0, void* a1, unsigned long* a2, bool a3) override {
        captureShader("vs", a1);  // a0 is unused by eCDX9Device; a1 is the bytecode (IDirect3DDevice9::CreateVertexShader)
        return TraceDevice::CreateVertexProgram(a0, a1, a2, a3);
    }
    bEResult CreatePixelProgram(void* a0, unsigned long* a1) override {
        captureShader("ps", a0);
        return TraceDevice::CreatePixelProgram(a0, a1);
    }
    bEResult CreateEffect(void const* a0, unsigned int a1, eCAPIEffect** a2) override {
        captureBlob("fx", a0, a1);
        return TraceDevice::CreateEffect(a0, a1, a2);
    }
    bEResult AssembleShaderProgram(char* const a0, unsigned long a1, eCGfxShared::eSGfxMacro* a2, void** a3, void** a4) override {
        captureBlob("asm", a0, a1);
        return TraceDevice::AssembleShaderProgram(a0, a1, a2, a3, a4);
    }
    bEResult CreateVertexDeclaration(eCGfxShared::eSGfxDclElement const* a0, void** a1) override {
        if (a0 != nullptr) {
            logOnce("vertex decl " + describeDecl(a0));
        }
        return TraceDevice::CreateVertexDeclaration(a0, a1);
    }
    bEResult CreateTexture(eCGfxShared::eSGfxLayersDesc const& a0, eCAPITexture** a1) override {
        logOnce("CreateTexture " + describeLayers(&a0));
        return TraceDevice::CreateTexture(a0, a1);
    }
    bEResult CreateCubeMap(eCGfxShared::eSGfxLayersDesc const& a0, eCAPICubeMap** a1) override {
        logOnce("CreateCubeMap " + describeLayers(&a0));
        return TraceDevice::CreateCubeMap(a0, a1);
    }
    bEResult CreateDepthBuffer(unsigned long a0, unsigned long a1, eCGfxShared::eEDepthFormat a2, void** a3) override {
        logOnce(std::format("CreateDepthBuffer {}x{} format={}", a0, a1, static_cast<int>(a2)));
        return TraceDevice::CreateDepthBuffer(a0, a1, a2, a3);
    }
    bEResult SwapScreen(HWND__* a0, bCRect const* a1, bCRect const* a2) override {
        const bEResult r = TraceDevice::SwapScreen(a0, a1, a2);
        ++frames_;
        const auto now = std::chrono::steady_clock::now();
        if (now - lastStats_ > std::chrono::seconds(5)) {
            lastStats_ = now;
            writeStats();
        }
        return r;
    }

private:
    std::uint64_t frames_ = 0;
    std::chrono::steady_clock::time_point lastStats_ = std::chrono::steady_clock::now();
};

// Material shaders come from the shader pool (Read: u32 size, bytecode, u32 type) or are compiled at runtime (Compile);
// both end in an ID3DXBuffer that GetFunctionData exposes without side effects.
class CaptureHLSLShader final : public TraceHLSLShader {
public:
    using TraceHLSLShader::TraceHLSLShader;

    bEResult Compile(eCGfxShared::eSGfxHLSLShaderSrc const& a0) override {
        const bEResult r = TraceHLSLShader::Compile(a0);
        capture();
        return r;
    }
    bEResult Read(bCIStream& a0) override {
        const bEResult r = TraceHLSLShader::Read(a0);
        capture();
        return r;
    }

private:
    void capture() {
        void* data = nullptr;
        unsigned long size = 0;
        if (!g_options.capture || inner_->GetFunctionData(&data, size) != bEResult_Ok || data == nullptr || size < 4) {
            return;
        }
        const auto version = shader::parseVersion(*static_cast<const std::uint32_t*>(data));
        if (!version) {
            spdlog::warn("trace: HLSL shader with an unknown version token 0x{:08x}", *static_cast<const std::uint32_t*>(data));
            return;
        }
        const std::string prefix = std::format("{}{}{}", version->stage == shader::Stage::Vertex ? "vs" : "ps", version->major,
                                               version->minor);
        captureBlob(prefix.c_str(), data, size);
    }
};

template <class Wrapper, class T>
T* wrapWith(T* inner) {
    if (inner == nullptr) {
        return nullptr;
    }
    {
        const std::shared_lock lock(g_mapMutex);
        if (auto it = g_wrapperOfInner.find(inner); it != g_wrapperOfInner.end()) {
            return static_cast<T*>(it->second);
        }
        if (g_innerOfWrapper.contains(inner)) {
            return inner;  // already a wrapper
        }
    }
    return new Wrapper(inner);
}

}  // namespace

// ---- trace_support.h ---------------------------------------------------------------------------------------------------

SlotStats& stats(int iface, int slot) {
    return g_stats[static_cast<std::size_t>(std::clamp(iface, 0, kMaxInterfaces - 1))]
                  [static_cast<std::size_t>(std::clamp(slot, 0, kMaxSlots - 1))];
}
int logFirst() { return g_logFirst.load(std::memory_order_relaxed); }
void setLogFirst(int n) { g_logFirst = n; }

void logCall(int iface, int slot, std::uint64_t n, const std::string& args) {
    spdlog::info("trace: {}::{} #{} ({})", kInterfaceNames[iface], slotName(iface, slot), n, args);
}
void logResult(int iface, int slot, const std::string& result) {
    spdlog::info("trace:   {}::{} -> {}", kInterfaceNames[iface], slotName(iface, slot), result);
}

void registerWrapper(void* wrapper, void* inner) {
    const std::unique_lock lock(g_mapMutex);
    g_innerOfWrapper[wrapper] = inner;
    if (inner != nullptr) {
        g_wrapperOfInner[inner] = wrapper;
    }
}
void unregisterWrapper(void* wrapper) {
    const std::unique_lock lock(g_mapMutex);
    if (auto it = g_innerOfWrapper.find(wrapper); it != g_innerOfWrapper.end()) {
        g_wrapperOfInner.erase(it->second);
        g_innerOfWrapper.erase(it);
    }
}
void* innerOf(void* maybeWrapper) {
    if (maybeWrapper == nullptr) {
        return nullptr;
    }
    const std::shared_lock lock(g_mapMutex);
    auto it = g_innerOfWrapper.find(maybeWrapper);
    return it != g_innerOfWrapper.end() ? it->second : maybeWrapper;
}
void* wrapperOf(void* inner) {
    const std::shared_lock lock(g_mapMutex);
    auto it = g_wrapperOfInner.find(inner);
    return it != g_wrapperOfInner.end() ? it->second : nullptr;
}

eCAPIVertexData* wrap(eCAPIVertexData* p) { return wrapWith<TraceVertexData>(p); }
eCAPIIndexData* wrap(eCAPIIndexData* p) { return wrapWith<TraceIndexData>(p); }
eCAPITexture* wrap(eCAPITexture* p) { return wrapWith<TraceTexture>(p); }
eCAPICubeMap* wrap(eCAPICubeMap* p) { return wrapWith<TraceCubeMap>(p); }
eCAPIVolume* wrap(eCAPIVolume* p) { return wrapWith<TraceVolume>(p); }
eCAPIPrimitive* wrap(eCAPIPrimitive* p) { return wrapWith<TracePrimitive>(p); }
eCAPIHLSLShader* wrap(eCAPIHLSLShader* p) { return wrapWith<CaptureHLSLShader>(p); }
eCAPIEffect* wrap(eCAPIEffect* p) { return wrapWith<TraceEffect>(p); }
eCAPISprite* wrap(eCAPISprite* p) { return wrapWith<TraceSprite>(p); }
eCAPIFont* wrap(eCAPIFont* p) { return wrapWith<TraceFont>(p); }
eCAPIDevice* wrap(eCAPIDevice* p) { return wrapWith<CaptureDevice>(p); }
eCAPIInstance* wrap(eCAPIInstance* p) { return wrapWith<TraceInstance>(p); }

// ---- trace.h ---------------------------------------------------------------------------------------------------------

void configure(const Options& options) {
    g_options = options;
    setLogFirst(options.logFirst);
}

bool install(void** apiInstanceSlot) {
    auto* inner = static_cast<eCAPIInstance*>(*apiInstanceSlot);
    if (inner == nullptr) {
        spdlog::error("trace: eCGfxAdmin::ms_pAPIInstance is null");
        return false;
    }
    // layout self-check: the engine's AddRef/Release use +8 (generated classes derive from an empty EngineObject)
    static_assert(sizeof(EngineObject) == 1);
    TraceInstance* w = static_cast<TraceInstance*>(wrap(inner));
    const auto refOffset = reinterpret_cast<const char*>(&w->refCount) - reinterpret_cast<const char*>(w);
    if (refOffset != 8) {
        spdlog::error("trace: refCount at +{} instead of +8: generated layout is wrong", refOffset);
        return false;
    }
    *apiInstanceSlot = w;
    spdlog::info("trace: eCAPIInstance {} wrapped by {}", static_cast<void*>(inner), static_cast<void*>(w));
    return true;
}

void writeStats() {
    if (g_options.statsFile.empty()) {
        return;
    }
    struct Row {
        std::uint64_t calls;
        int iface;
        int slot;
    };
    std::vector<Row> rows;
    for (int i = 0; i < kInterfaceCount; ++i) {
        for (int s = 0; s < kMaxSlots; ++s) {
            if (const auto n = g_stats[static_cast<std::size_t>(i)][static_cast<std::size_t>(s)].calls.load(); n != 0) {
                rows.push_back({n, i, s});
            }
        }
    }
    std::ranges::sort(rows, [](const Row& a, const Row& b) { return a.iface != b.iface ? a.iface < b.iface : a.slot < b.slot; });
    std::ofstream out(g_options.statsFile, std::ios::trunc);
    out << "# interface slot calls (risen-enhanced-graphics trace mode)\n";
    for (const auto& r : rows) {
        out << std::format("{:16s} {:3d} {:40s} {}\n", kInterfaceNames[r.iface], r.slot, slotName(r.iface, r.slot), r.calls);
    }
}

}  // namespace rvk::hal::trace
