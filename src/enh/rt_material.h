#pragma once

// Materials of the ray tracing scene (M9 P2, docs/research/rt-reflections.md 3.2): what a ray hit needs to shade a draw's
// surface like the raster: the diffuse (albedo) texture + sampler, the affine map from the vertex uv to the pixel shader's
// diffuse coordinate, the opacity source and the alpha test of masked materials.
//
// * Slot: the engine's material graph of the executing shader (engine/materials.h: the first sampler of the Diffuse tree
//   is g_SamplerData[k] = pixel sampler s(9 + k), docs/research/rt-albedo-facts.md 1.2 (a)); the texture is what the draw
//   binds there. Without a readable graph: s9 when it is a 2D sampler (the bytecode fallback, 1.2 (c)).
// * uv: the draw's own vertex and pixel shaders are evaluated on the CPU (shader/sm_eval.h) on probe vertices with the
//   draw's constants: the pixel shader's coordinate of that sampler's first texld as a function of the vertex's TEXCOORD0 /
//   TEXCOORD1 (5 probes give the affine map, a 6th checks that it is affine, a 7th that it does not depend on the position).
//   Cached per (vertex shader, pixel shader, sampler) and the values of the constants the coordinate depends on (a
//   backward slice of both programs), so scrolling / animated uv maps re-evaluate only when their constants change.
// * Entries are interned and reference-counted by their users (the RT scene's BLASes); a released texture marks the entries
//   that point to it dead (their users resolve again at their next sighting). Render thread.

#include <array>
#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "d3d9/device.h"
#include "shader/sm_eval.h"

namespace rvk::enh {

struct RtMaterial {
    d3d9::Texture* albedo = nullptr;   // the diffuse texture (null: none, or it died)
    d3d9::Texture* opacity = nullptr;  // the opacity's own texture (kRtUvOpacityTexture), else null
    std::uint32_t sampler = 0;         // bindless sampler of the diffuse slot
    std::uint32_t uvSet = 0;           // shader::abi::kRtUv* word
    float uvMap[2][3]{{1.0f, 0.0f, 0.0f}, {0.0f, 1.0f, 0.0f}};
    std::uint32_t colour = 0xFF808080u;  // RGBA8 (kRtUvNoTexture)
    std::uint32_t alpha = 0;            // the material's own alpha test (masked: GREATER | MaskReference << 8), 0 = none
    bool masked = false;                // blend mode Masked (alpha-tested geometry whatever the draw's states)
    bool dead = false;                  // a texture it pointed to was released
    std::string name;                   // diagnostics: the diffuse's pool image (or the engine material)
    bool operator==(const RtMaterial& o) const;
};

class RtMaterials {
public:
    // The material of the draw `q` (render thread, inside the draw: its constants and textures are bound): an entry id
    // with one reference for the caller (release it), 0 when the draw has no material shaders.
    std::uint32_t resolve(d3d9::Device& dev, const d3d9::ReplaceQuery& q);
    // a cheap signature of the draw's material state (shaders, the engine shader, the textures s9..s15): unchanged = the
    // last resolve still holds (apart from animated constants)
    static std::uint64_t signature(const d3d9::ReplaceQuery& q);
    const RtMaterial* get(std::uint32_t id) const;
    void addRef(std::uint32_t id);
    void release(std::uint32_t id);
    void textureReleased(const d3d9::Texture* t);
    void clear();
    std::size_t size() const { return entries_.size(); }

    struct Counters {
        std::uint64_t resolves = 0, evaluations = 0, graph = 0, heuristic = 0, noTexture = 0, approx = 0, failed = 0, pool = 0;
        std::uint64_t shaderMismatch = 0;  // ms_pActiveShader != the draw's material's shader
        double ms = 0.0;
    };
    Counters takeCounters() {
        Counters c = iv_;
        iv_ = {};
        return c;
    }

private:
    struct Program {
        bool ok = false;
        shader::Program program;
    };
    struct Analysis {  // per (vs, ps, sampler register)
        bool ok = false;
        std::string error;
        std::size_t texAt = 0;
        std::vector<std::uint16_t> vsConsts, psConsts;
        bool usesInts = false;
        std::unordered_map<std::uint64_t, std::array<float, 7>> maps;  // constant values hash -> map (2 x 3) + uvSet bits
    };
    struct Entry {
        RtMaterial m;
        std::uint64_t key = 0;
        std::uint32_t refs = 0;
    };

    const Program* program(const d3d9::Shader* s);
    Analysis* analysis(const d3d9::Shader* vs, const d3d9::Shader* ps, std::uint32_t reg);
    // the uv map of `reg` for this draw: map[0..5] + uvSet bits in map[6]
    bool uvMap(d3d9::Device& dev, const d3d9::ReplaceQuery& q, std::uint32_t reg, std::array<float, 7>& out, std::string& why);
    std::uint32_t intern(const RtMaterial& m);

    std::unordered_map<std::uint64_t, Program> programs_;
    std::unordered_map<std::uint64_t, Analysis> analyses_;
    std::unordered_map<std::uint32_t, Entry, IdHash> entries_;
    std::unordered_map<std::uint64_t, std::uint32_t, IdHash> byKey_;
    std::unordered_multimap<const d3d9::Texture*, std::uint32_t> byTexture_;
    std::uint32_t nextId_ = 1;
    std::unordered_set<std::uint64_t> logged_;  // material x pixel shader
    Counters iv_;
};

}  // namespace rvk::enh
