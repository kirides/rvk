#pragma once

// Draw tags and per-frame draw records (roadmap do-now item 2, docs/research/draw-tags.md): what each draw is (kind: static
// mesh, water, skinned, SpeedTree, particles, sky, ...), where in the frame it happens (view / pass from scene.h), which engine
// objects issued it (render context, entity, sub-mesh, material), its geometry identity (buffer ids + generations, ranges),
// the GPU addresses of its constants and draw block, and its textures. The engine hooks (src/engine/scene_hooks.*) keep a
// current Tag on d3d9::Device; every recorded draw copies it. Pure logic, no Vulkan, no engine access (unit tests:
// tests/draws_tests.cpp). Render thread only.

#include <array>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "d3d9/identity.h"
#include "d3d9/scene.h"

namespace rvk::d3d9::draws {

// who issued the draw: an engine render context (eCRenderContext* vtable slots) or a render function (detours)
enum class Source : std::uint8_t {
    None,
    Mesh,                // eCRenderContextMesh (static meshes, instanced batches, water)
    Skinned,             // eCRenderContextAnimation2 (characters, animals: GPU skinning)
    SpeedTree,           // eCRenderContextSpeedTree (trees, palms: branches, fronds, leaves)
    SpeedTreeBillboard,  // eCRenderContextSpeedTreeBB (distant tree billboards)
    Vegetation,          // eCRenderContextVegetation (grass, instanced)
    Debug,               // eCRenderContextDebug
    Sky,                 // eCSkydome_PS::Render
    Particle,            // eCParticle_PS::Render
    Strip,               // eCStrip_PS::Render (trails)
    Decal,               // eCDecalAdmin::RenderDecals
    Billboard,           // eCBillboardAdmin::RenderBillboards
    Precipitation,       // eCPrecipitationRenderContext (rain / snow: one dynamic mesh, also drawn when it is dry)
    Count
};

// what the draw is (Source refined by the phase); the tint view and the histogram use it
enum class Kind : std::uint8_t {
    Unknown,             // no tag (a draw path not hooked yet)
    StaticMesh,
    InstancedMesh,       // a mesh's instanced batch (render-op node 0xffffff)
    Refracted,           // a mesh in the Refracted pass: water (and other refracting materials)
    Skinned,
    SpeedTree,
    SpeedTreeBillboard,
    Vegetation,
    Sky,
    Particle,
    Strip,
    Decal,
    Billboard,
    Precipitation,
    Debug,
    ShadowMask,          // untagged draws of the ShadowMask pass (the screen-space shadow mask)
    Post,                // untagged draws of the image-effect chain
    Gui,                 // untagged draws in eCApplication::Draw
    Count
};

// Which body of water a draw belongs to, from its material's name (docs/research/water.md 2): the engine has no water class;
// water meshes are ordinary meshes with a refracting material. None = not water (glass, other materials).
enum class Water : std::uint8_t {
    None,
    Ocean,      // Special_Water_Ocean_*: the sea around the island
    Lake,       // lakes, ponds (Special_Water_Island_*, *Lake*)
    River,      // rivers, streams, water flows (Special_Water_River_*, *Flow*)
    Swamp,      // Special_Water_Swamp_*
    Waterfall,  // Special_Water_Fall*_*: vertical sheets (Alpha pass, not refracted)
    Shore,      // Special_Water_Shore_*: the engine's fake shore, scrolling foam strips along the sea coast (additive, Alpha
                // pass; Levelmesh_Ocean_Shore_01/02)
    Other,      // another material with "water" in its name (bath tubs, puddles)
    Count
};

const char* name(Source s);
const char* name(Kind k);
const char* name(Water w);
// the water body of a material name (case-insensitive substrings; "" / unknown -> None)
Water classifyWater(std::string_view materialName);

// the engine's current context (set by the hooks around the original calls; nested scopes save and restore it)
struct Tag {
    Source source = Source::None;
    std::uint32_t opDesc = 0;  // eCRenderOp2 +8: sub-mesh index | hierarchy node << 8 (0xffffff = instanced batch); SpeedTree:
                               // 0 branches, 1 fronds, 2 / 3 leaves
    const void* context = nullptr;       // eCRenderContext* / eCParticle_PS* / eCSkydome_PS* ...
    const void* entity = nullptr;        // render context +0x10 (world matrix at +0x60)
    const void* subMesh = nullptr;       // eCSubMesh* (eCSubMesh::Render)
    const void* material = nullptr;      // eCMaterialResource2* (sub-mesh +0x38)
    const void* meshResource = nullptr;  // eCMeshResource2*
    Water water = Water::None;           // the material's water body (only resolved in the Refracted / Alpha passes)
    const char* materialName = nullptr;  // its name (engine hooks' cache: valid while the process runs), null = not read
};

Kind classify(Source source, std::uint32_t opDesc, scene::View view, scene::Pass pass);

// one draw of the frame
struct Record {
    enum : std::uint16_t {
        Indexed = 1u << 0,
        UserPointer = 1u << 1,     // DrawPrimitiveUP / DrawIndexedPrimitiveUP (vertices in the ring, no buffer identity)
        Instanced = 1u << 2,       // stream 0 frequency INDEXEDDATA (hardware instancing)
        DeclBlendIndices = 1u << 3,  // the vertex declaration has BLENDINDICES
        DeclTexcoord4 = 1u << 4,     // ... TEXCOORD4 (the engine's instance index)
        Tinted = 1u << 5,          // drawn with the tint debug view's shader variant
    };
    std::uint32_t index = 0;  // draw number within the frame
    scene::View view = scene::View::None;
    scene::Pass pass = scene::Pass::None;
    Kind kind = Kind::Unknown;
    Source source = Source::None;
    std::uint16_t flags = 0;
    std::uint8_t primitiveType = 0;  // D3DPRIMITIVETYPE
    std::uint32_t opDesc = 0;
    const void* context = nullptr;
    const void* entity = nullptr;
    const void* subMesh = nullptr;
    const void* material = nullptr;
    const void* meshResource = nullptr;
    Water water = Water::None;
    bool replaced = false;  // drawn by an enhancement's replacement shaders (water.enabled) instead of the engine's
    // geometry
    ResourceRef vertexBuffer;  // stream 0
    ResourceRef indexBuffer;
    std::uint32_t vertexOffset = 0, vertexStride = 0;  // stream 0
    std::uint32_t first = 0;       // start index (indexed) or start vertex
    std::uint32_t primitives = 0;
    std::int32_t baseVertex = 0;
    std::uint32_t instances = 1;
    // shaders (hash = Device's shader cache key) and their semantics flags (d3d9/semantics.h)
    std::uint64_t vsHash = 0, psHash = 0;
    std::uint32_t vsFlags = 0, psFlags = 0;
    // device addresses in the frame's ring (valid until the frame's slot is reused): StageConsts of both stages, DrawBlock
    std::uint64_t vsConsts = 0, psConsts = 0, drawBlock = 0;
    std::array<std::uint64_t, 16> textures{};  // ids (identity.h) of the pixel shader's samplers s0..s15 (0 = none / unused)
};

// draws per kind and view of a frame
struct Histogram {
    std::array<std::array<std::uint32_t, static_cast<std::size_t>(scene::View::Count)>, static_cast<std::size_t>(Kind::Count)> counts{};
    std::uint32_t total = 0;
    std::uint32_t tinted = 0;
    void add(Kind k, scene::View v) {
        ++counts[static_cast<std::size_t>(k)][static_cast<std::size_t>(v)];
        ++total;
    }
    std::uint32_t kind(Kind k) const;
    // "StaticMesh 702 [Main 502, Shadow 200], Skinned 40 [Main 30, Shadow 10], ..." (by count, descending)
    std::string summary() const;
};

// The records + histogram of the frame being recorded and of the last finished one
class FrameDraws {
public:
    static constexpr std::size_t kMaxRecords = 8192;

    void setRecording(bool on) { recording_ = on; }
    bool recording() const { return recording_; }
    // a new record (zeroed) or nullptr (recording off, or the frame's cap reached: counted as dropped)
    Record* add();
    void count(Kind k, scene::View v) { hist_.add(k, v); }
    void countTinted() { ++hist_.tinted; }
    // frame boundary: this frame's records become last()
    void endFrame();
    const std::vector<Record>& current() const { return cur_; }
    const std::vector<Record>& last() const { return last_; }
    const Histogram& lastHistogram() const { return lastHist_; }
    std::uint32_t lastDropped() const { return lastDropped_; }

private:
    bool recording_ = true;
    std::vector<Record> cur_, last_;
    Histogram hist_, lastHist_;
    std::uint32_t dropped_ = 0, lastDropped_ = 0;
};

// ---- debug view "tint draws" (overlay Debug / debug.tint_draws) ----

enum class TintMode : std::uint8_t { Off, Kind, Pass };
// the tint of a draw, packed RGBA8 (R in the low byte; GLSL unpackUnorm4x8; alpha = strength), 0 = draw it untouched.
// Only colour draws of camera views are tinted (Main / Preview / Other: Setup, Solid, Refracted, Alpha, AlphaLate): depth
// maps, shadow maps, the shadow mask, the post chain and the GUI carry data or must stay readable.
std::uint32_t tintColor(TintMode mode, Kind kind, scene::View view, scene::Pass pass);

}  // namespace rvk::d3d9::draws
