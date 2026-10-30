#include "shader/spirv.h"

#include <atomic>

#include "shader/dxc.h"

namespace rvk::shader {

static std::atomic<bool> g_debugInfo{false};
void setDebugInfo(bool on) { g_debugInfo.store(on, std::memory_order_relaxed); }
bool debugInfo() { return g_debugInfo.load(std::memory_order_relaxed); }

SpirvResult compileHlsl(Stage stage, std::string_view hlsl, std::string_view name) {
    SpirvResult result;
    dxc::Request r;
    r.file = std::string(name);
    r.source = std::string(hlsl);
    r.entry = stage == Stage::Vertex ? "VSMain" : "PSMain";
    r.profile = stage == Stage::Vertex ? "vs_6_6" : "ps_6_6";
    r.optLevel = 1;
    r.cache = false;
    r.debugInfo = debugInfo();
    dxc::Result c = dxc::compile(r);
    result.words = std::move(c.words);
    result.log = std::move(c.log);
    return result;
}

}  // namespace rvk::shader
