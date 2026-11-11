#include "shader/dxc.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <format>
#include <fstream>
#include <iterator>
#include <memory>
#include <mutex>
#include <thread>

#include <wrl/client.h>

#include <directx-dxc/dxcapi.h>
#include <directx-dxc/d3d12shader.h>

#include "shader/embedded.h"
#include "core/profile.h"

namespace rvk::shader::dxc {
namespace {

namespace fs = std::filesystem;
using Microsoft::WRL::ComPtr;

// ---------------------------------------------------------------------------------------------------------------------
// state
// ---------------------------------------------------------------------------------------------------------------------

struct Library {
    HMODULE module = nullptr;
    DxcCreateInstanceProc create = nullptr;
    std::string version;
    std::string failure;
    bool ok = false;
};

std::mutex g_mutex;  // guards the settings below
SourceReader g_reader;
fs::path g_cacheDir;
fs::path g_libraryDir;

std::wstring widen(std::string_view s) {
    if (s.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring w(static_cast<std::size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), n);
    return w;
}

std::string narrow(std::wstring_view w) {
    if (w.empty()) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
    std::string s(static_cast<std::size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), s.data(), n, nullptr, nullptr);
    return s;
}

std::uint64_t fnvBytes(const void* data, std::size_t size, std::uint64_t h = 0xcbf29ce484222325ull) {
    const auto* p = static_cast<const unsigned char*>(data);
    for (std::size_t i = 0; i < size; ++i) {
        h ^= p[i];
        h *= 0x100000001b3ull;
    }
    return h;
}
std::uint64_t fnv1a(std::string_view s, std::uint64_t h = 0xcbf29ce484222325ull) { return fnvBytes(s.data(), s.size(), h); }

fs::path moduleDirectory(HMODULE module) {
    wchar_t buf[MAX_PATH * 2];
    const DWORD n = GetModuleFileNameW(module, buf, static_cast<DWORD>(std::size(buf)));
    if (n == 0 || n >= std::size(buf)) return {};
    return fs::path(buf).parent_path();
}

HMODULE thisModule() {
    HMODULE m = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       reinterpret_cast<LPCWSTR>(&thisModule), &m);
    return m;
}

// ---------------------------------------------------------------------------------------------------------------------
// sources
// ---------------------------------------------------------------------------------------------------------------------

bool readDefault(std::string_view path, std::string& text) {
    static const std::string dir = [] {
        // (the Win32 block: the proxy sets [debug] environment variables with SetEnvironmentVariableW in DllMain, which the CRT's copy does not see)
        char buf[1024];
        const DWORD n = GetEnvironmentVariableA("RVK_SHADER_DIR", buf, sizeof buf);
        return n > 0 && n < sizeof buf ? std::string(buf, n) : std::string();
    }();
    if (!dir.empty()) {
        std::ifstream in(fs::path(dir) / fs::path(path), std::ios::binary);
        if (in) {
            text.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
            return true;
        }
    }
    const std::string_view embeddedText = embedded::hlslSource(path);
    if (embeddedText.empty()) return false;
    text.assign(embeddedText);
    return true;
}

SourceReader currentReader() {
    const std::lock_guard lock(g_mutex);
    return g_reader ? g_reader : SourceReader(readDefault);
}

// "./a/../include\\x.hlsli" -> "include/x.hlsli"
std::string normalizePath(std::string_view p) {
    std::string s(p);
    for (char& c : s) {
        if (c == '\\') c = '/';
    }
    return fs::path(s).lexically_normal().generic_string();
}

// DXC hands the handler whatever its search produced (the including file's directory + the name, or an absolute path). Our names are
// relative to shaders/: try the path as it is, then with leading components dropped, then include/<file name> (the old -I include).
bool resolveInclude(const SourceReader& reader, std::string_view requested, std::string& resolvedPath, std::string& text) {
    std::string p = normalizePath(requested);
    if (p.size() > 1 && p[1] == ':') p.erase(0, p.find('/') == std::string::npos ? p.size() : p.find('/') + 1);
    while (!p.empty()) {
        if (reader(p, text)) {
            resolvedPath = p;
            return true;
        }
        const auto slash = p.find('/');
        if (slash == std::string::npos) break;
        p.erase(0, slash + 1);
    }
    const std::string base = fs::path(normalizePath(requested)).filename().generic_string();
    const std::string viaInclude = "include/" + base;
    if (!base.empty() && reader(viaInclude, text)) {
        resolvedPath = viaInclude;
        return true;
    }
    return false;
}

class IncludeHandler final : public IDxcIncludeHandler {
public:
    IncludeHandler(IDxcUtils* utils, const SourceReader& reader) : utils_(utils), reader_(reader) {}

    HRESULT STDMETHODCALLTYPE LoadSource(LPCWSTR fileName, IDxcBlob** includeSource) override {
        *includeSource = nullptr;
        std::string path;
        std::string text;
        const std::string requested = narrow(fileName);
        if (!resolveInclude(reader_, requested, path, text)) {
            missing_.push_back(requested);
            return E_FAIL;
        }
        ComPtr<IDxcBlobEncoding> blob;
        if (FAILED(utils_->CreateBlob(text.data(), static_cast<UINT32>(text.size()), DXC_CP_UTF8, &blob))) return E_FAIL;
        bool known = false;
        for (const auto& inc : includes_) known = known || inc.first == path;
        if (!known) includes_.emplace_back(path, fnv1a(text));
        *includeSource = blob.Detach();
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** object) override {
        if (riid == __uuidof(IDxcIncludeHandler) || riid == __uuidof(IUnknown)) {
            *object = static_cast<IDxcIncludeHandler*>(this);
            AddRef();
            return S_OK;
        }
        *object = nullptr;
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++refs_; }
    ULONG STDMETHODCALLTYPE Release() override {
        const ULONG r = --refs_;
        if (r == 0) delete this;
        return r;
    }

    const std::vector<std::pair<std::string, std::uint64_t>>& includes() const { return includes_; }
    const std::vector<std::string>& missing() const { return missing_; }

private:
    IDxcUtils* utils_;
    const SourceReader& reader_;
    std::atomic<ULONG> refs_{1};
    std::vector<std::pair<std::string, std::uint64_t>> includes_;  // resolved path, hash of the text
    std::vector<std::string> missing_;
};

// ---------------------------------------------------------------------------------------------------------------------
// library
// ---------------------------------------------------------------------------------------------------------------------

Library loadLibrary() {
    Library lib;
    std::vector<fs::path> dirs;
    {
        const std::lock_guard lock(g_mutex);
        if (!g_libraryDir.empty()) dirs.push_back(g_libraryDir);
    }
    if (const HMODULE self = thisModule()) dirs.push_back(moduleDirectory(self));
    dirs.push_back(moduleDirectory(nullptr));
    std::string tried;
    for (const fs::path& dir : dirs) {
        if (dir.empty()) continue;
        const fs::path dll = dir / "dxcompiler.dll";
        std::error_code ec;
        if (!fs::exists(dll, ec)) {
            tried += std::format("{} (missing); ", dll.string());
            continue;
        }
        // an explicit path only: the default search order would find an inbox dxcompiler.dll without SPIR-V
        lib.module = LoadLibraryExW(dll.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
        if (lib.module) break;
        tried += std::format("{} (load error {}); ", dll.string(), GetLastError());
    }
    if (!lib.module) {
        lib.failure = "dxcompiler.dll not found: " + (tried.empty() ? std::string("no search directory") : tried);
        return lib;
    }
    lib.create = reinterpret_cast<DxcCreateInstanceProc>(GetProcAddress(lib.module, "DxcCreateInstance"));
    if (!lib.create) {
        lib.failure = "dxcompiler.dll has no DxcCreateInstance";
        return lib;
    }
    ComPtr<IDxcCompiler3> compiler;
    if (SUCCEEDED(lib.create(CLSID_DxcCompiler, IID_PPV_ARGS(&compiler)))) {
        ComPtr<IDxcVersionInfo> info;
        if (SUCCEEDED(compiler.As(&info))) {
            UINT32 major = 0, minor = 0;
            info->GetVersion(&major, &minor);
            lib.version = std::format("{}.{}", major, minor);
            ComPtr<IDxcVersionInfo2> info2;
            if (SUCCEEDED(compiler.As(&info2))) {
                UINT32 commits = 0;
                char* hash = nullptr;
                if (SUCCEEDED(info2->GetCommitInfo(&commits, &hash)) && hash) {
                    lib.version += std::format(".{}", commits);
                    CoTaskMemFree(hash);
                }
            }
        }
    }
    lib.ok = true;
    return lib;
}

Library& library() {
    static Library lib = [] {
        Library l = loadLibrary();
        if (!l.ok) return l;
        // probe: SPIR-V code generation must exist (the inbox compiler and old builds compile to DXIL only)
        Library& probeLib = l;
        ComPtr<IDxcUtils> utils;
        ComPtr<IDxcCompiler3> compiler;
        if (FAILED(probeLib.create(CLSID_DxcUtils, IID_PPV_ARGS(&utils))) ||
            FAILED(probeLib.create(CLSID_DxcCompiler, IID_PPV_ARGS(&compiler)))) {
            l.ok = false;
            l.failure = "DxcCreateInstance failed";
            return l;
        }
        static constexpr char kProbe[] = "[numthreads(1,1,1)] void main() {}";
        DxcBuffer buf{kProbe, sizeof(kProbe) - 1, DXC_CP_UTF8};
        LPCWSTR args[] = {L"-E", L"main", L"-T", L"cs_6_0", L"-spirv", L"-fspv-target-env=vulkan1.3"};
        ComPtr<IDxcResult> result;
        HRESULT status = E_FAIL;
        if (SUCCEEDED(compiler->Compile(&buf, args, static_cast<UINT32>(std::size(args)), nullptr, IID_PPV_ARGS(&result)))) {
            result->GetStatus(&status);
        }
        if (FAILED(status)) {
            l.ok = false;
            l.failure = "dxcompiler.dll compiled no SPIR-V (a build without -DENABLE_SPIRV_CODEGEN)";
        }
        return l;
    }();
    return lib;
}

// ---------------------------------------------------------------------------------------------------------------------
// disk cache: [magic][args revision][include count][{u16 length, path, u64 hash}...][u64 payload size][payload]
// ---------------------------------------------------------------------------------------------------------------------

constexpr std::uint32_t kCacheMagic = 0x43585652u;  // "RVXC"

std::uint64_t requestKey(const Request& r, std::string_view sourceText, const std::vector<std::string>& args) {
    std::uint64_t h = fnvBytes(&kArgsRevision, sizeof(kArgsRevision));
    h = fnv1a(library().version, h);
    h = fnv1a(r.file, h);
    for (const std::string& a : args) {
        h = fnv1a(a, h);
        h = fnv1a("\n", h);
    }
    return fnv1a(sourceText, h);
}

bool loadCache(const fs::path& file, const SourceReader& reader, Request const& r, std::vector<std::uint8_t>& payload) {
    std::ifstream in(file, std::ios::binary);
    if (!in) return false;
    auto get = [&](void* dst, std::size_t n) { return static_cast<bool>(in.read(static_cast<char*>(dst), static_cast<std::streamsize>(n))); };
    std::uint32_t magic = 0, revision = 0, count = 0;
    if (!get(&magic, 4) || !get(&revision, 4) || !get(&count, 4) || magic != kCacheMagic || revision != kArgsRevision || count > 256) return false;
    for (std::uint32_t i = 0; i < count; ++i) {
        std::uint16_t len = 0;
        std::uint64_t hash = 0;
        if (!get(&len, 2)) return false;
        std::string path(len, '\0');
        if (!get(path.data(), len) || !get(&hash, 8)) return false;
        std::string text;
        if (!reader(path, text) || fnv1a(text) != hash) return false;  // an include changed or vanished
    }
    std::uint64_t size = 0;
    if (!get(&size, 8) || size == 0 || size > (64u << 20)) return false;
    payload.resize(size);
    (void)r;
    return get(payload.data(), size);
}

void storeCache(const fs::path& file, const std::vector<std::pair<std::string, std::uint64_t>>& includes, const void* payload, std::size_t size) {
    std::error_code ec;
    fs::create_directories(file.parent_path(), ec);
    const fs::path tmp = fs::path(file).concat(".tmp");
    {
        std::ofstream out(tmp, std::ios::binary);
        if (!out) return;
        auto put = [&](const void* src, std::size_t n) { out.write(static_cast<const char*>(src), static_cast<std::streamsize>(n)); };
        const std::uint32_t revision = kArgsRevision, count = static_cast<std::uint32_t>(includes.size());
        put(&kCacheMagic, 4);
        put(&revision, 4);
        put(&count, 4);
        for (const auto& [path, hash] : includes) {
            const std::uint16_t len = static_cast<std::uint16_t>(path.size());
            put(&len, 2);
            put(path.data(), path.size());
            put(&hash, 8);
        }
        const std::uint64_t n = size;
        put(&n, 8);
        put(payload, size);
        if (!out) return;
    }
    fs::rename(tmp, file, ec);
    if (ec) fs::remove(tmp, ec);
}

}  // namespace

// ---------------------------------------------------------------------------------------------------------------------
// public
// ---------------------------------------------------------------------------------------------------------------------

void setSourceReader(SourceReader reader) {
    const std::lock_guard lock(g_mutex);
    g_reader = std::move(reader);
}

void setCacheDirectory(fs::path dir) {
    const std::lock_guard lock(g_mutex);
    g_cacheDir = std::move(dir);
}

void setLibraryDirectory(fs::path dir) {
    const std::lock_guard lock(g_mutex);
    g_libraryDir = std::move(dir);
}

bool available() { return library().ok; }
std::string_view unavailableReason() { return library().ok ? std::string_view{} : std::string_view(library().failure); }
std::string_view version() { return library().ok ? std::string_view(library().version) : std::string_view{}; }

std::vector<std::string> arguments(const Request& r) {
    std::vector<std::string> a = {"-E", r.entry, "-T", r.profile, "-HV", "2021"};
    for (const Define& d : r.defines) {
        a.push_back("-D");
        a.push_back(d.value.empty() ? d.name : d.name + "=" + d.value);
    }
    if (r.target == Target::Spirv) {
        // GD3D11's set (gd3d11.md 3.7) for a floor without VK_EXT_mutable_descriptor_type: the typed bindless arrays need
        // no -fvk-bind-resource-heap; storage images carry [[vk::image_format]] instead of -fspv-use-unknown-image-format
        // (-fspv-entrypoint-name: every pipeline create call of the renderer names the entry point "main")
        a.insert(a.end(), {"-spirv", "-fspv-target-env=vulkan1.3", "-fspv-entrypoint-name=main",
                           "-fspv-extension=SPV_KHR_ray_query", "-fspv-extension=SPV_EXT_descriptor_indexing",
                           "-fspv-extension=SPV_KHR_physical_storage_buffer", "-fspv-extension=SPV_KHR_shader_draw_parameters"});
    }
    if (r.target == Target::Dxil) {
        // the sources carry [[vk::...]] attributes (locations, bindings, image formats, push constants): meaningless for DXIL, DXC ignores them
        a.push_back("-Wno-ignored-attributes");
    }
    if (r.half16) a.insert(a.end(), {"-enable-16bit-types", "-D", "RVK_HALF16=1"});  // (the ubershader text picks real half types by this define)
    if (r.debugInfo) {
        a.insert(a.end(), {"-Zi", "-Od"});
        if (r.target == Target::Spirv) a.insert(a.end(), {"-fspv-debug=file", "-fspv-debug=source", "-fspv-debug=line"});
    } else {
        a.push_back(std::format("-O{}", std::clamp(r.optLevel, 0, 3)));
    }
    return a;
}

namespace {

thread_local const std::atomic<bool>* t_cancel = nullptr;

Result compileNow(const Request& request);

Result cancelledResult(const Request& request) {
    Result r;
    r.cancelled = true;
    r.log = std::format("{}: compile cancelled", request.file);
    return r;
}

// the compile on a helper thread (same priority as the caller), waited for until done or cancelled; an abandoned helper keeps the shared state alive
Result compileCancellable(const Request& request, const std::atomic<bool>& cancel) {
    if (cancel.load(std::memory_order_acquire)) return cancelledResult(request);
    struct Shared {
        std::mutex m;
        std::condition_variable cv;
        bool done = false;
        Result result;
    };
    auto st = std::make_shared<Shared>();
    const int priority = GetThreadPriority(GetCurrentThread());
    std::thread([st, request, priority] {
        SetThreadDescription(GetCurrentThread(), L"rvk dxc compile");
        if (priority != THREAD_PRIORITY_ERROR_RETURN) SetThreadPriority(GetCurrentThread(), priority);
        Result r = compileNow(request);
        {
            const std::lock_guard lock(st->m);
            st->result = std::move(r);
            st->done = true;
        }
        st->cv.notify_all();
    }).detach();
    std::unique_lock lock(st->m);
    while (!st->done) {
        if (cancel.load(std::memory_order_acquire)) return cancelledResult(request);
        st->cv.wait_for(lock, std::chrono::milliseconds(20));
    }
    return std::move(st->result);
}

}  // namespace

void setThreadCancel(const std::atomic<bool>* flag) { t_cancel = flag; }

Result compile(const Request& request) {
    if (const std::atomic<bool>* cancel = t_cancel; cancel != nullptr) return compileCancellable(request, *cancel);
    return compileNow(request);
}

namespace {

Result compileNow(const Request& request) {
    RVK_ZONE("shader::dxc compile");
    Result result;
    Library& lib = library();
    if (!lib.ok) {
        result.log = "DXC unavailable: " + lib.failure;
        return result;
    }
    const SourceReader reader = currentReader();
    std::string source;
    if (!request.source.empty()) {
        source = request.source;
    } else if (!reader(request.file, source)) {
        result.log = std::format("{}: no such shader source", request.file);
        return result;
    }
    const std::vector<std::string> args = arguments(request);
    fs::path cacheDir;
    {
        const std::lock_guard lock(g_mutex);
        cacheDir = g_cacheDir;
    }
    fs::path cacheFile;
    if (!cacheDir.empty() && request.cache) {
        cacheFile = cacheDir / std::format("{:016x}.dxo", requestKey(request, source, args));
        std::vector<std::uint8_t> payload;
        if (loadCache(cacheFile, reader, request, payload)) {
            if (request.target == Target::Spirv) {
                result.words.resize(payload.size() / 4);
                std::memcpy(result.words.data(), payload.data(), result.words.size() * 4);
            } else {
                result.bytes = std::move(payload);
                if (result.bytes.size() >= 20 && std::memcmp(result.bytes.data(), "DXBC", 4) == 0) {
                    for (int i = 4; i < 20; ++i) result.dxilSigned = result.dxilSigned || result.bytes[static_cast<std::size_t>(i)] != 0;
                }
            }
            result.fromCache = result.ok();
            if (result.fromCache) return result;
        }
    }

    ComPtr<IDxcUtils> utils;
    ComPtr<IDxcCompiler3> compiler;
    if (FAILED(lib.create(CLSID_DxcUtils, IID_PPV_ARGS(&utils))) || FAILED(lib.create(CLSID_DxcCompiler, IID_PPV_ARGS(&compiler)))) {
        result.log = "DxcCreateInstance failed";
        return result;
    }
    auto* handler = new IncludeHandler(utils.Get(), reader);
    ComPtr<IDxcIncludeHandler> handlerRef;
    handlerRef.Attach(handler);

    std::vector<std::wstring> wide;
    wide.push_back(widen(request.file));  // the source's name: diagnostics and relative includes
    for (const std::string& a : args) wide.push_back(widen(a));
    std::vector<LPCWSTR> argv;
    argv.reserve(wide.size());
    for (const std::wstring& w : wide) argv.push_back(w.c_str());

    const DxcBuffer buffer{source.data(), source.size(), DXC_CP_UTF8};
    ComPtr<IDxcResult> dxcResult;
    const HRESULT hr = compiler->Compile(&buffer, argv.data(), static_cast<UINT32>(argv.size()), handlerRef.Get(), IID_PPV_ARGS(&dxcResult));
    if (FAILED(hr) || !dxcResult) {
        result.log = std::format("{}: IDxcCompiler3::Compile failed (0x{:08x})", request.file, static_cast<unsigned>(hr));
        return result;
    }
    ComPtr<IDxcBlobUtf8> errors;
    if (SUCCEEDED(dxcResult->GetOutput(DXC_OUT_ERRORS, IID_PPV_ARGS(&errors), nullptr)) && errors && errors->GetStringLength() > 0) {
        result.log.assign(errors->GetStringPointer(), errors->GetStringLength());
    }
    for (const std::string& m : handler->missing()) result.log += std::format("\n{}: include \"{}\" not found", request.file, m);
    HRESULT status = E_FAIL;
    dxcResult->GetStatus(&status);
    if (FAILED(status)) return result;
    ComPtr<IDxcBlob> object;
    if (FAILED(dxcResult->GetOutput(DXC_OUT_OBJECT, IID_PPV_ARGS(&object), nullptr)) || !object || object->GetBufferSize() == 0) {
        result.log += "\nno object output";
        return result;
    }
    const auto* data = static_cast<const std::uint8_t*>(object->GetBufferPointer());
    const std::size_t size = object->GetBufferSize();
    if (request.target == Target::Spirv) {
        if (size % 4 != 0) {
            result.log += "\nSPIR-V size is not a multiple of 4";
            return result;
        }
        result.words.resize(size / 4);
        std::memcpy(result.words.data(), data, size);
    } else {
        result.bytes.assign(data, data + size);
        // DXBC container: 'DXBC', 16 bytes of digest (all zero = not signed: dxil.dll was not found next to dxcompiler.dll)
        if (size >= 20 && std::memcmp(data, "DXBC", 4) == 0) {
            for (int i = 4; i < 20; ++i) result.dxilSigned = result.dxilSigned || data[i] != 0;
        }
    }
    if (!cacheFile.empty()) storeCache(cacheFile, handler->includes(), data, size);
    return result;
}

}  // namespace

std::uint32_t dxilPushBytes(std::span<const std::uint8_t> dxil) {
    Library& lib = library();
    if (!lib.ok) return 0;
    ComPtr<IDxcUtils> utils;
    ComPtr<IDxcContainerReflection> refl;
    ComPtr<IDxcBlobEncoding> blob;
    UINT32 part = 0;
    ComPtr<ID3D12ShaderReflection> sr;
    if (FAILED(lib.create(CLSID_DxcUtils, IID_PPV_ARGS(&utils))) || FAILED(lib.create(CLSID_DxcContainerReflection, IID_PPV_ARGS(&refl))) ||
        FAILED(utils->CreateBlob(dxil.data(), static_cast<UINT32>(dxil.size()), DXC_CP_ACP, &blob)) || FAILED(refl->Load(blob.Get())) ||
        FAILED(refl->FindFirstPartKind(DXC_PART_DXIL, &part)) || FAILED(refl->GetPartReflection(part, IID_PPV_ARGS(&sr)))) {
        return 0;
    }
    D3D12_SHADER_DESC desc{};
    sr->GetDesc(&desc);
    for (UINT i = 0; i < desc.BoundResources; ++i) {
        D3D12_SHADER_INPUT_BIND_DESC b{};
        sr->GetResourceBindingDesc(i, &b);
        if (b.Type == D3D_SIT_CBUFFER && b.BindPoint == 0 && b.Space == 0) {
            D3D12_SHADER_BUFFER_DESC cd{};
            if (ID3D12ShaderReflectionConstantBuffer* cb = sr->GetConstantBufferByName(b.Name); cb != nullptr && SUCCEEDED(cb->GetDesc(&cd))) return cd.Size;
        }
    }
    return 0;
}

std::string dxilContractViolation(std::span<const std::uint8_t> dxil) {
    Library& lib = library();
    if (!lib.ok) return "DXC unavailable";
    ComPtr<IDxcUtils> utils;
    ComPtr<IDxcContainerReflection> refl;
    if (FAILED(lib.create(CLSID_DxcUtils, IID_PPV_ARGS(&utils))) || FAILED(lib.create(CLSID_DxcContainerReflection, IID_PPV_ARGS(&refl)))) {
        return "DxcContainerReflection unavailable";
    }
    ComPtr<IDxcBlobEncoding> blob;
    if (FAILED(utils->CreateBlob(dxil.data(), static_cast<UINT32>(dxil.size()), DXC_CP_ACP, &blob))) return "cannot wrap the DXIL";
    if (FAILED(refl->Load(blob.Get()))) return "not a DXIL container";
    UINT32 part = 0;
    if (FAILED(refl->FindFirstPartKind(DXC_PART_DXIL, &part))) return "no DXIL part";
    ComPtr<ID3D12ShaderReflection> sr;
    if (FAILED(refl->GetPartReflection(part, IID_PPV_ARGS(&sr)))) return "no shader reflection";
    D3D12_SHADER_DESC desc{};
    sr->GetDesc(&desc);
    std::string out;
    for (UINT i = 0; i < desc.BoundResources; ++i) {
        D3D12_SHADER_INPUT_BIND_DESC b{};
        sr->GetResourceBindingDesc(i, &b);
        const std::string what = std::format("{} (type {}) at register {} space {}", b.Name ? b.Name : "?", static_cast<int>(b.Type), b.BindPoint, b.Space);
        bool ok = false;
        switch (b.Type) {
        case D3D_SIT_CBUFFER:
            if (b.BindPoint == 0 && b.Space == 0) {  // the push constants = the root constants (32 DWORDs)
                ID3D12ShaderReflectionConstantBuffer* cb = sr->GetConstantBufferByName(b.Name);
                D3D12_SHADER_BUFFER_DESC cd{};
                if (cb && SUCCEEDED(cb->GetDesc(&cd)) && cd.Size > 128) {
                    out += std::format("push constants of {} bytes (root constants hold 128); ", cd.Size);
                    continue;
                }
                ok = std::string_view(b.Name ? b.Name : "") != "$Globals";
            } else {
                ok = b.BindPoint == 8 && b.Space == 1;  // the root CBV
            }
            break;
        case D3D_SIT_UAV_RWTYPED:
            ok = b.BindPoint < 8 && b.Space == 1;  // the storage image table u0..u7
            break;
        default: break;  // everything else comes from the heaps (SM 6.6 ResourceDescriptorHeap): a bound SRV / sampler / buffer UAV has no root parameter
        }
        if (!ok) out += what + "; ";
    }
    return out;
}

}  // namespace rvk::shader::dxc
