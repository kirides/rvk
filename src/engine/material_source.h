#pragma once

// The engine's request for each material shader (docs/research/shader-permutations.md 8): the hook on
// eCShaderMaterialAdmin::CompileShaderPoolEllement(eSGfxHLSLShaderSrc const&, version, flags) 0x6b8140 runs for every material
// shader the engine asks for (pool hits included) and sees the effect file, entry function, profile and the final macro list
// (the material graph's XFX_PS_* snippets are macro values). The returned pool element holds our HAL shader (element +0x18 =
// the engine's eCGfxHLSLShader, its +0x18 = the API shader): the request is attached to that shader's d3d9::Shader as
// `source`, and optionally written out as JSON lines keyed by the shader's hash (the corpus name without the stage prefix).
// Vulkan mode only; the hook only observes.

#include <cstddef>
#include <filesystem>

namespace rvk::d3d9 {
class Shader;
}

namespace rvk::engine::matsrc {

// the d3d9 shader of an engine API shader pointer (the HAL's HLSL shader), null when it is not ours
using Resolver = d3d9::Shader* (*)(void* apiShader);

void setResolver(Resolver r);
// any thread: collecting on / off (off: the detour calls straight through)
void setEnabled(bool on);
// JSON lines (rewritten when something new was seen, at most every 15 s from onPresent); empty = no file
void setOutput(std::filesystem::path file);

// Creates the detour (enabled with the others by hooks::enableAll); false when the symbol is missing
bool install();

// engine thread, once per present: the periodic dump
void onPresent();

struct Stats {
    std::size_t requests = 0;    // detour calls with a result
    std::size_t resolved = 0;    // ... whose element's API shader is one of ours
    std::size_t sources = 0;     // shaders with a source
    std::size_t unresolved = 0;  // results whose API shader is not ours
};
Stats stats();

}  // namespace rvk::engine::matsrc
