#include "shader/spirv.h"

#include <algorithm>
#include <atomic>
#include <cstdlib>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include "shader/dxc.h"

namespace rvk::shader {

static std::atomic<bool> g_debugInfo{false};
void setDebugInfo(bool on) { g_debugInfo.store(on, std::memory_order_relaxed); }
bool debugInfo() { return g_debugInfo.load(std::memory_order_relaxed); }

SpirvResult compileHlsl(Stage stage, std::string_view hlsl, std::string_view name, bool half16) {
    SpirvResult result;
    dxc::Request r;
    r.file = std::string(name);
    r.source = std::string(hlsl);
    r.entry = stage == Stage::Vertex ? "VSMain" : "PSMain";
    r.profile = stage == Stage::Vertex ? "vs_6_6" : "ps_6_6";
    // WS31 (frame-ownership.md 16.21): -O0 by default. The driver compiles the SPIR-V again, the GPU time is the same as with -O1 (jungle -0.2 %, hold within the noise), but a DXC compile
    // costs about half and the variant queue of a cold / updated first session empties in ~80 s instead of ~200. RVK_DXC_OPT_SPIRV=n restores a level (the cache key includes it).
    static const int opt = [] {
        char v[8];
        return GetEnvironmentVariableA("RVK_DXC_OPT_SPIRV", v, sizeof v) != 0 ? std::clamp(std::atoi(v), 0, 3) : 0;
    }();
    r.optLevel = opt;
    r.cache = true;
    r.half16 = half16;
    r.debugInfo = debugInfo();
    dxc::Result c = dxc::compile(r);
    result.words = std::move(c.words);
    result.log = std::move(c.log);
    return result;
}

DxilResult compileHlslDxil(Stage stage, std::string_view hlsl, std::string_view name, bool half16, bool refresh) {
    DxilResult result;
    dxc::Request r;
    r.file = std::string(name);
    r.source = std::string(hlsl);
    r.entry = stage == Stage::Vertex ? "VSMain" : "PSMain";
    r.profile = stage == Stage::Vertex ? "vs_6_6" : "ps_6_6";
    r.target = dxc::Target::Dxil;
    r.optLevel = 1;
    r.cache = true;
    r.half16 = half16;
    r.refresh = refresh;
    dxc::Result c = dxc::compile(r);
    result.fromCache = c.fromCache;
    result.bytes = std::move(c.bytes);
    result.log = std::move(c.log);
    return result;
}

// frame-ownership.md 16.27. SM 6.6 `discard` is OpDemoteToHelperInvocation; a fast-linked pipeline library pixel shader of the AMD driver
// (the fragment-shader library + the separately compiled output part) exports the demoted lanes of a quad that still has live lanes whenever the shader
// also takes a derivative (a texture sample with implicit LOD, fwidth): foliage cut-outs come out wider than the optimised monolithic build's. OpKill
// (what a `discard` was before the demote) does not show it. Every `OpDemoteToHelperInvocation` directly followed by the `OpBranch` that ends its block
// becomes `OpKill`; the capability goes when none is left (a demote in another position keeps the module as it is).
bool demoteToKill(std::vector<std::uint32_t>& words) {
    constexpr std::uint32_t kOpDemote = 5380, kOpBranch = 249, kOpKill = 252, kOpCapability = 17, kCapDemote = 5379;
    if (words.size() < 6 || words[0] != 0x07230203u) return false;
    std::vector<std::uint32_t> out(words.begin(), words.begin() + 5);
    out.reserve(words.size());
    std::size_t capability = 0;  // index in `out` of the demote capability instruction (0 = none)
    std::size_t converted = 0, kept = 0;
    for (std::size_t i = 5; i < words.size();) {
        const std::uint32_t op = words[i] & 0xFFFFu, len = words[i] >> 16;
        if (len == 0 || i + len > words.size()) return false;  // malformed: leave it
        if (op == kOpCapability && len == 2 && words[i + 1] == kCapDemote) capability = out.size();
        if (op == kOpDemote && len == 1) {
            const std::size_t n = i + 1;
            if (n < words.size() && (words[n] & 0xFFFFu) == kOpBranch && (words[n] >> 16) == 2) {
                out.push_back((1u << 16) | kOpKill);
                i = n + 2;
                ++converted;
                continue;
            }
            ++kept;
        }
        out.insert(out.end(), words.begin() + static_cast<std::ptrdiff_t>(i), words.begin() + static_cast<std::ptrdiff_t>(i + len));
        i += len;
    }
    if (converted == 0) return false;
    if (kept == 0 && capability != 0) out.erase(out.begin() + static_cast<std::ptrdiff_t>(capability), out.begin() + static_cast<std::ptrdiff_t>(capability + 2));
    words = std::move(out);
    return true;
}

}  // namespace rvk::shader
