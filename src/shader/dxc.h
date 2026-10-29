#pragma once

// Runtime HLSL compiler: the GD3D11 scheme (docs/research/gd3d11.md 3.7, docs/research/hlsl-rhi.md 2). One HLSL source per shader,
// compiled with the DXC COM API (IDxcCompiler3 from a dxcompiler.dll loaded at run time; no import library) to SPIR-V for Vulkan 1.3
// (and, for the DX12 backend, DXIL), with an on-disk cache keyed by the request, the argument set and the hash of every resolved
// #include. The sources are embedded into d3d9.dll (shader::embedded::hlslSources); RVK_SHADER_DIR points the compiler at a directory
// instead (edit, rerun, no rebuild).

#include <cstdint>
#include <filesystem>
#include <functional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace rvk::shader::dxc {

enum class Target { Spirv, Dxil };

struct Define {
    std::string name;
    std::string value;  // empty = "-D name"
};

struct Request {
    std::string file;       // path below shaders/, e.g. "debug/inject_view.hlsl" (also the name in diagnostics and cache keys)
    std::string entry;      // "CSMain"
    std::string profile;    // "cs_6_6", "vs_6_6", "ps_6_6"
    std::vector<Define> defines;
    Target target = Target::Spirv;
    bool debugInfo = false;  // -Zi -Od + SPIR-V debug info (RenderDoc / PIX show names and source); part of the cache key
};

struct Result {
    std::vector<std::uint32_t> words;  // SPIR-V (Target::Spirv)
    std::vector<std::uint8_t> bytes;   // DXIL container (Target::Dxil)
    std::string log;                   // compiler messages (errors on failure, warnings otherwise)
    bool fromCache = false;
    bool ok() const { return !words.empty() || !bytes.empty(); }
};

// Source text of a file below shaders/ ("include/rvk_abi.hlsli"); false when unknown. Replaceable for tests.
using SourceReader = std::function<bool(std::string_view path, std::string& text)>;

// The reader compile() uses: RVK_SHADER_DIR (files on disk, hot reload) when set and the file exists there, else the embedded sources.
// setSourceReader(nullptr) restores that default.
void setSourceReader(SourceReader reader);
// Where the disk cache lives (<dir>/<key>.dxo); empty = no disk cache (default)
void setCacheDirectory(std::filesystem::path dir);
// Where dxcompiler.dll is searched first (next to d3d9.dll: bin\rvk); the executable's and the module's directories follow
void setLibraryDirectory(std::filesystem::path dir);

// dxcompiler.dll found, loaded, and a probe shader compiled to SPIR-V (the Vulkan SDK's / the vcpkg port's DXC both can; an
// inbox DXC cannot). Cached after the first call. Thread-safe.
bool available();
std::string_view unavailableReason();
// "1.9.2602" style version string of the loaded compiler ("" when unavailable)
std::string_view version();

// Compiles one request (thread-safe; every call makes its own compiler instance). The result of a failed compile has an empty
// payload and the compiler's messages in `log`.
Result compile(const Request& request);

// The argument list compile() hands DXC for a request (after the source file name): the shared set both backends and the offline
// checks use; exposed for tools/tests (rvk_shadertool dxc, the dxc.exe cross check).
std::vector<std::string> arguments(const Request& request);

// Bump when the argument set or the cache layout changes (part of every cache key)
inline constexpr std::uint32_t kArgsRevision = 1;

}  // namespace rvk::shader::dxc
