#include "shader/spirv.h"

#include <atomic>

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
    r.optLevel = 1;
    r.cache = false;
    r.half16 = half16;
    r.debugInfo = debugInfo();
    dxc::Result c = dxc::compile(r);
    result.words = std::move(c.words);
    result.log = std::move(c.log);
    return result;
}

DxilResult compileHlslDxil(Stage stage, std::string_view hlsl, std::string_view name, bool half16) {
    DxilResult result;
    dxc::Request r;
    r.file = std::string(name);
    r.source = std::string(hlsl);
    r.entry = stage == Stage::Vertex ? "VSMain" : "PSMain";
    r.profile = stage == Stage::Vertex ? "vs_6_6" : "ps_6_6";
    r.target = dxc::Target::Dxil;
    r.optLevel = 1;
    r.cache = false;
    r.half16 = half16;
    dxc::Result c = dxc::compile(r);
    result.bytes = std::move(c.bytes);
    result.log = std::move(c.log);
    return result;
}

}  // namespace rvk::shader
