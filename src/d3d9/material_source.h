#pragma once

// Where a material shader came from: the engine's request for it (eSGfxHLSLShaderSrc: effect file, entry function, profile and
// the final sorted macro list, with the material graph's XFX_PS_* snippets as macro values). The engine hashes exactly this
// (MD5) to find the precompiled bytecode in ShaderMaterialPool_Master.smp, so a pool shader's defines are only known at the
// request (engine/material_source.*, docs/research/shader-permutations.md 8).

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace rvk::d3d9 {

struct MaterialSource {
    std::string file;   // "ge_default_phong.fx" (or the HLSL text when `inlineSource`)
    std::string entry;  // "PS_Default", "PS_DefaultZPass", "VS_Default", ...
    std::uint32_t profile = 0;  // index into the engine's profile table (0..7 pixel, 8..11 vertex)
    std::uint32_t flags = 0;    // D3DX compile flags
    bool inlineSource = false;
    std::vector<std::pair<std::string, std::string>> macros;  // sorted by name, as the engine hashes them
};

// 14.22: the request whose compile is running on this thread (engine/material_source's detour): Device::createShader attaches it to the shader it creates (or finds without a
// source) before the shader's variants can be queued. Null outside the call
void setPendingMaterialSource(const MaterialSource* s);
class Shader;
void notifySourceAttached(Shader& s);  // 14.22: a request was attached to an existing shader (its variants built without it are rebuilt)
bool pendingMaterialSourceUsed();  // a shader took the pending request since setPendingMaterialSource (it holds the pointer: keep the object alive)

}  // namespace rvk::d3d9
