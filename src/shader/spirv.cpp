#include "shader/spirv.h"

#include <atomic>
#include <mutex>

#include <glslang/Public/ResourceLimits.h>
#include <glslang/Public/ShaderLang.h>
#include <glslang/SPIRV/GlslangToSpv.h>

namespace rvk::shader {
namespace {

void initGlslang() {
    static std::once_flag once;
    std::call_once(once, [] { glslang::InitializeProcess(); });
}

}  // namespace

static std::atomic<bool> g_debugInfo{false};
void setDebugInfo(bool on) { g_debugInfo.store(on, std::memory_order_relaxed); }
bool debugInfo() { return g_debugInfo.load(std::memory_order_relaxed); }

SpirvResult compileGlsl(Stage stage, std::string_view glsl) {
    initGlslang();
    const bool debug = debugInfo();
    SpirvResult result;
    const EShLanguage lang = stage == Stage::Vertex ? EShLangVertex : EShLangFragment;
    glslang::TShader shader(lang);
    const char* text = glsl.data();
    const int length = static_cast<int>(glsl.size());
    shader.setStringsWithLengths(&text, &length, 1);
    shader.setEnvInput(glslang::EShSourceGlsl, lang, glslang::EShClientVulkan, 100);
    shader.setEnvClient(glslang::EShClientVulkan, glslang::EShTargetVulkan_1_3);
    shader.setEnvTarget(glslang::EShTargetSpv, glslang::EShTargetSpv_1_6);
    const auto messages = static_cast<EShMessages>(EShMsgSpvRules | EShMsgVulkanRules | (debug ? EShMsgDebugInfo : 0));
    if (!shader.parse(GetDefaultResources(), 460, false, messages)) {
        result.log = shader.getInfoLog();
        return result;
    }
    glslang::TProgram program;
    program.addShader(&shader);
    if (!program.link(messages)) {
        result.log = program.getInfoLog();
        return result;
    }
    glslang::SpvOptions options;
    options.generateDebugInfo = debug;
    options.disableOptimizer = debug;
    options.optimizeSize = false;
    spv::SpvBuildLogger logger;
    glslang::GlslangToSpv(*program.getIntermediate(lang), result.words, &logger, &options);
    result.log = logger.getAllMessages();
    return result;
}

}  // namespace rvk::shader
