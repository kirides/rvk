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

}  // namespace rvk::shader
