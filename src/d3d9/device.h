#pragma once

// Direct3D 9's device model on Vulkan (no COM, no D3D headers): render/sampler states, textures, streams, shaders, constants,
// render targets, draws, clears, copies, present. The engine HAL (eCAPIDevice etc.) and the effect runtime are written against
// this class; it owns every Vulkan object of the renderer.
//
// Threading: resource creation, Lock/Unlock and destruction may happen on any thread (the game loads on worker threads);
// state setters, draws and present belong to the render thread. Uploads are recorded lazily by the render thread when a
// resource is first used (into the frame's upload command buffer, or inline when it was already used in the frame).

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <filesystem>
#include <map>
#include <memory>
#include <condition_variable>
#include <deque>
#include <functional>
#include <future>
#include <mutex>
#include <span>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "d3d9/wp0.h"
#include "gpuscene/owned_compact.h"
#include "core/id_hash.h"
#include "d3d9/defs.h"
#include "d3d9/draws.h"
#include "d3d9/identity.h"
#include "d3d9/inject.h"
#include "d3d9/material_source.h"
#include "d3d9/owned_windows.h"
#include "d3d9/scene.h"
#include "d3d9/semantics.h"
#include "rhi/game_device.h"
#include "rhi/rhi.h"
#include "shader/abi.h"
#include "shader/translate.h"
#include "shader/hlsl.h"
#include "core/pacer.h"
#include "rhi/d3d_formats.h"  // (the D3D9 format table: VkFormat vocabulary, no Vulkan objects)

struct HWND__;

namespace rvk::d3d9 {

// The draw path's value vocabulary (packets, chunks, pipeline descriptions) is the seam's: d3d9 builds it,
// the backend records it (rhi/game_device.h)
using rhi::DrawPacket;
using rhi::DrawChunk;
using rhi::VertexInputValues;
using rhi::DynamicStateValues;
using rhi::ClearOp;
using rhi::PipelineDesc;
using rhi::BlendAttachment;
using rhi::PresentOverlay;
using rhi::kMaxVertexBindings;
using rhi::kMaxVertexAttributes;
using rhi::GamePipeline;
using rhi::kChunkPackets;

class Device;

// ---- resources -------------------------------------------------------------------------------------------------------

enum class TextureType : std::uint8_t { Tex2D, Cube, Volume };

struct TextureDesc {
    TextureType type = TextureType::Tex2D;
    std::uint32_t width = 1, height = 1, depth = 1;
    std::uint32_t levels = 1;  // 0 = full chain
    std::uint32_t format = 0;  // D3DFORMAT
    std::uint32_t usage = 0;   // USAGE_*
    std::uint32_t pool = POOL_MANAGED;
    bool lowHeapSlot = false;  // the bindless slot must be < 16384 (material pack maps; TextureDesc::lowHeapSlot)
};

class Surface;

class Texture {
public:
    Texture(Device& device, const TextureDesc& desc);
    ~Texture();
    Texture(const Texture&) = delete;
    Texture& operator=(const Texture&) = delete;

    bool valid() const { return valid_; }
    const TextureDesc& desc() const { return desc_; }
    const rhi::FormatInfo& format() const { return fmt_; }
    std::uint32_t levels() const { return desc_.levels; }
    std::uint32_t faces() const { return desc_.type == TextureType::Cube ? 6 : 1; }
    std::uint32_t levelWidth(std::uint32_t level) const { return std::max(1u, desc_.width >> level); }
    std::uint32_t levelHeight(std::uint32_t level) const { return std::max(1u, desc_.height >> level); }
    std::uint32_t levelDepth(std::uint32_t level) const { return std::max(1u, desc_.depth >> level); }
    bool isRenderTarget() const { return (desc_.usage & USAGE_RENDERTARGET) != 0; }
    bool isDepth() const { return fmt_.depth; }
    bool isNull() const { return isNull_; }  // the NULL FOURCC render target: no memory

    // D3D9 LockRect / LockBox: a pointer into the CPU copy of the level (D3D9 pitch); rect = {left, top, right, bottom}.
    // Like D3D9's managed pool, a texture that was ever locked keeps its CPU copy (partial locks, e.g. the engine's glyph
    // atlases, must see the earlier contents); only the locked region is uploaded. File textures (setLevelData only) drop
    // theirs after the upload; a later lock without DISCARD reads the level back from the GPU first.
    struct Locked {
        void* bits = nullptr;
        std::uint32_t rowPitch = 0;
        std::uint32_t slicePitch = 0;
    };
    bool lock(std::uint32_t level, std::uint32_t face, const std::int32_t* rect, std::uint32_t flags, Locked& out);
    void unlock(std::uint32_t level, std::uint32_t face);
    // fill a level from D3D9-layout data (file loading); thread-safe
    void setLevelData(std::uint32_t level, std::uint32_t face, std::span<const std::uint8_t> data, std::uint32_t rowPitch);

    // the render-target / depth surface of a level (created on demand, owned by the texture)
    Surface* surface(std::uint32_t level, std::uint32_t face);

    // render thread
    // the RHI texture behind it (the image and its layout tracking: rhi::Texture::layout / CommandList::transition)
    rhi::Texture& rhi() { return *tex_; }
    // the tracked layout of the image (UNDEFINED for the NULL render target, which has none)
    VkImageLayout layout() const { return tex_ ? tex_->layout() : VK_IMAGE_LAYOUT_UNDEFINED; }
    std::uint32_t bindlessSlot() const { return slot_; }
    bool prepareForSampling(rhi::CommandList& cl);  // uploads pending data, transitions to shader-read; false if nothing to sample
    // what the engine uses the image for (Device::tagImage; render thread)
    scene::ImageRole role() const { return role_; }
    // persistent identity (d3d9/identity.h): the generation counts uploads of CPU data
    ResourceRef ref() const { return {id_, generation_.load(std::memory_order_relaxed)}; }
    // the debug name of the image: "<label> #<index> | tex <id> WxH FORMAT [slot N]" (vk/debug_names.h). The label is
    // the role the scene tracker found ("depth map", "cascade shadow map" #k, ...): a static string, renamed when it changes
    // (Device::tagImage). Render thread
    void setDebugLabel(const char* label, std::uint32_t index = ~0u);
    const char* debugLabel() const { return label_; }

    // bytes held in CPU copies of all textures (diagnostics)
    static std::uint64_t cpuBytesTotal();
    // S-7 (native/own_post): small R32F render targets (<= 64 x 64: the post chain's luminance levels) are created with storage usage too, so a compute
    // pass can write the engine's persistent 1x1 images; textures made before the switch keep their usage (the pass checks `storageCapable`)
    static void setSmallStorage(bool on);
    static bool smallStorage();
    bool storageCapable() const { return (tex_ != nullptr) && (tex_->desc().usage & VK_IMAGE_USAGE_STORAGE_BIT) != 0; }

private:
    friend class Device;
    // Device::destroy with the texture still alive (the engine releases some at process exit, after its device): the GPU
    // objects are freed now, the destructor later touches nothing of the device
    void orphan();
    bool orphaned_ = false;  // under Device::liveMutex_
    struct Level {
        std::vector<std::uint8_t> data;  // D3D9 layout
        std::uint32_t rowPitch = 0;
        std::uint32_t slicePitch = 0;
        bool dirty = false;
        bool dropped = false;            // the CPU copy was freed after an upload: the image holds the only copy
        bool uploaded = false;           // the image level was written once (the first upload is always whole)
        std::uint32_t box[6]{};          // dirty region x0, y0, z0, x1, y1, z1 in texels (valid while dirty)
    };
    Level& cpuLevel(std::uint32_t level, std::uint32_t face);
    void markDirty(Level& l, std::uint32_t level, const std::int32_t* rect);
    void freeLevel(Level& l);
    bool restoreLevel(std::uint32_t level, std::uint32_t face);  // read a dropped level back from the image; mutex_ not held
    void upload(rhi::CommandList& cl);

    Device& dev_;
    TextureDesc desc_;
    rhi::FormatInfo fmt_{};
    bool valid_ = false;
    bool isNull_ = false;
    std::unique_ptr<rhi::Texture> tex_;  // the backend's image + its tracked layout + the bindless slot (owned: destroyed through the device's retirement)
    std::uint32_t slot_ = 0;             // tex_->heapSampled()
    std::mutex mutex_;
    std::vector<Level> cpu_;            // [face * levels + level], allocated on demand
    std::atomic<bool> pending_{false};  // some level is dirty
    bool lockedOnce_ = false;           // ever locked: keep the CPU copies (D3D9 managed semantics)
    std::atomic<bool> gpuWritten_{false};  // a blit destination (Device::stretchRect): its image holds data the CPU copy lacks
    bool restoreWarned_ = false;
    std::uint64_t lastUseFrame_ = 0;
    bool reverseZ_ = false;  // a depth buffer holding reversed depth (Device::setReverseZ; set by its last clear)
    std::vector<std::unique_ptr<Surface>> surfaces_;
    scene::ImageRole role_ = scene::ImageRole::None;
    const char* label_ = nullptr;  // setDebugLabel
    std::uint32_t labelIndex_ = ~0u;
    std::string debugName() const;
    void applyDebugName();
    std::uint64_t id_ = newResourceId();
    std::atomic<std::uint32_t> generation_{0};
};

// A render target / depth stencil: one level/face of a texture, or a standalone depth buffer (CreateDepthBuffer).
class Surface {
public:
    Surface(Texture* texture, std::uint32_t level, std::uint32_t face, bool ownsTexture);
    ~Surface();
    Texture* texture() const { return texture_; }
    std::uint32_t level() const { return level_; }
    std::uint32_t face() const { return face_; }
    std::uint32_t width() const { return texture_->levelWidth(level_); }
    std::uint32_t height() const { return texture_->levelHeight(level_); }

private:
    Texture* texture_;
    std::uint32_t level_;
    std::uint32_t face_;
    bool owns_;
};

class Buffer {
public:
    Buffer(Device& device, std::uint32_t size, std::uint32_t usage, std::uint32_t pool, bool index, bool index32);
    ~Buffer();
    std::uint32_t size() const { return size_; }
    bool isIndex() const { return index_; }
    bool index32() const { return index32_; }
    bool dynamic() const { return (usage_ & USAGE_DYNAMIC) != 0; }
    // a fresh copy in the frame's upload ring per use (dynamic / system-memory buffers) instead of one static GPU buffer
    bool ringCopied() const { return dynamic() || pool_ == POOL_SYSTEMMEM; }
    // persistent identity (d3d9/identity.h): the generation counts the GPU copies (render thread)
    ResourceRef ref() const { return {id_, copies_.generation()}; }
    std::uint64_t id() const { return id_; }

    // D3D9 Lock: pointer into the CPU copy (offset/size 0 = whole buffer); flags LOCK_*
    void* lock(std::uint32_t offset, std::uint32_t size, std::uint32_t flags);
    void unlock();
    // a copy of bytes of the CPU copy (any thread; false when out of range): enhancements read engine geometry with it
    bool readCpu(std::uint32_t offset, std::uint32_t size, void* out);

    // render thread: the GPU copy for a draw (uploads / renames as needed)
    struct Binding {
        std::uint64_t buffer = 0;  // the backend's buffer handle (rhi::RingAlloc::buffer / MeshAlloc::native; Vulkan: the VkBuffer), 0 = none
        std::uint64_t offset = 0;
    };
    // strideHint: the first use's vertex stride / index size: a new static copy starts at a multiple of it in the mesh arena,
    // so a draw can bind the arena at offset 0 and pass the offset as vertexOffset / firstIndex (0 = no hint)
    Binding gpu(std::uint32_t strideHint = 0);
    // render thread, after gpu(): the GPU address (shader reference + raw address) of the static GPU copy (ray tracing tier only: BLAS inputs, vertex
    // fetches of RT hit shaders; docs/research/rt-reflections.md 3); 0 for ring-copied buffers or without the tier. Valid
    // until the next generation (Device::notifyBufferChanged tells the observers)
    rhi::GpuAddr gpuAddress();
    // the static GPU copy's address (shader reference + raw) without the ray tracing gate: null for ring-copied buffers, before the first draw uploaded it and where the
    // arena has no device addresses (Vulkan without the RT tier). Render thread (gpuscene/mesh_parts)
    rhi::GpuAddr arenaAddress();

private:
    Device& dev_;
    std::uint32_t size_;
    std::uint32_t usage_;
    std::uint32_t pool_;
    bool index_;
    bool index32_;
    std::vector<std::uint8_t> cpu_;
    std::mutex mutex_;
    BufferCopyState copies_;  // writes since the last GPU copy, what the next use copies, the generation
    std::uint64_t id_ = newResourceId();
    Binding binding_;
    // gpu()'s fast path (most draws): a static copy that no lock touched since it was made; binding_ is final then.
    // Cleared under mutex_ by lock() / orphan(), set by gpu() after a static copy
    std::atomic<bool> cleanStatic_{false};
    // dynamic buffers: the frame's ring copy holds the bytes [ringLo_, ringHi_) at ringPtr_ (d3d9::RingBlock)
    std::uint8_t* ringPtr_ = nullptr;
    std::uint32_t ringLo_ = 0, ringHi_ = 0;
    rhi::MeshAlloc static_;  // the static GPU copy, in the device's mesh arena
    friend class Device;
    void orphan();           // as Texture::orphan
    bool orphaned_ = false;  // under Device::liveMutex_
};

class VertexDecl {
public:
    explicit VertexDecl(std::span<const VertexElement> elements);
    const std::vector<VertexElement>& elements() const { return elements_; }
    bool hasBlendIndices() const { return blendIndices_; }  // skinning (or SpeedTree wind) data
    bool hasTexcoord4() const { return texcoord4_; }        // the engine's instance index
    std::uint64_t id() const { return id_; }                // unique for the process (never reused: a cache key)

private:
    std::vector<VertexElement> elements_;
    std::uint64_t id_ = 0;
    bool blendIndices_ = false;
    bool texcoord4_ = false;
};

inline constexpr std::size_t kVariantSlots = 30;  // VariantSlot::Count

class Shader {
public:
    shader::Stage stage = shader::Stage::Vertex;
    // the SPIR-V module: set by the shader workers (Device::createShader), null while compiling and when the compile failed; read it with wait() where the
    // draw needs it, directly where a missing module only means "not yet" (the prewarm of pipelines)
    std::atomic<rhi::ShaderModule> module{rhi::ShaderModule::None};
    std::shared_future<void> ready;  // valid for shaders compiled by the workers
    std::atomic<bool> compiling{false};  // queued / being compiled by the workers: a draw that needs it is skipped meanwhile (Device::prioritizeShader)
    std::atomic<bool> promoted{false};   // moved to the front of the compile queue by a draw
    rhi::ShaderModule wait() const {
        if (ready.valid()) ready.wait();
        return module.load(std::memory_order_acquire);
    }
    shader::Reflection reflection;
    std::vector<std::uint32_t> bytecode;  // the D3D9 tokens (kept for GetFunctionData)
    std::uint64_t hash = 0;
    ShaderSemantics semantics;  // named constants / samplers from the CTAB (d3d9/semantics.h)
    // the engine request this pool shader was found by (set once by engine/material_source, any thread; null: unknown / not a
    // material shader). Owned by the registry there
    std::atomic<const MaterialSource*> source{nullptr};
    // vertex shaders: the vertex layout plans (Device::vertexPlan) of the last declarations they were drawn with (render thread)
    struct PlanSlot {
        std::uint64_t declId = 0;
        const void* plan = nullptr;
    };
    std::array<PlanSlot, 4> planSlots{};
    std::uint32_t planNext = 0;
    std::uint32_t samplerMask = 0;  // bit s: reflection.samplers[s] is used (per-draw loops visit only these)
    // pixel shaders: the tint debug view's variant (TranslateOptions::tintEpilogue), built on first use (render thread)
    rhi::ShaderModule tintModule = rhi::ShaderModule::None;
    bool tintFailed = false;
    // vertex shaders of SpeedTree billboards: the fade distance variant (Device::lodModule, engine/lod.h)
    rhi::ShaderModule lodModule = rhi::ShaderModule::None;
    bool lodFailed = false;
    // variants of enhancements built in the background (Device::variant, VariantSlot): the module once ready
    std::array<std::atomic<rhi::ShaderModule>, kVariantSlots> variants{};  // (VariantSlot::Count)
    std::array<std::atomic<std::uint8_t>, kVariantSlots> variantState{};  // 0 not requested, 1 queued, 2 ready, 3 failed / not applicable
    std::atomic<std::uint32_t> uberSlots{0};  // S-1a: bit i = variants[i] was built from the HLSL ubershader (not the translated bytecode)
};

// A non-owning reference to the device's current shader (setVertexShader / setPixelShader / a replacement's): switching
// shaders per draw must not cost two atomic reference counts. Created shaders live in Device::shaders_ until the device is
// destroyed; a replacement's shaders are held by its replacer / the draw's DrawReplacement while it is drawn.
class ShaderRef {
public:
    ShaderRef() = default;
    ShaderRef(const std::shared_ptr<Shader>& s) : p_(s.get()) {}  // NOLINT(google-explicit-constructor)
    explicit ShaderRef(Shader* s) : p_(s) {}  // a shader of Device::shaders_ (lives as long as the device)
    Shader* operator->() const { return p_; }
    Shader& operator*() const { return *p_; }
    Shader* get() const { return p_; }
    explicit operator bool() const { return p_ != nullptr; }
    bool operator==(const ShaderRef&) const = default;
    void reset() { p_ = nullptr; }

private:
    Shader* p_ = nullptr;
};

// Shader variants an enhancement supplies (Device::setVariantProvider), translated + compiled on a background thread on
// their first request; the draw uses the engine's own module until the variant (and its pipeline) is ready
enum class VariantSlot : std::uint8_t {
    Gi = 0,       // pixel shaders: the ambient constants from the GI probe volume (enh/gi)
    Capture = 1,  // vertex shaders: world positions + TEXCOORD0 written to a buffer (skinned draws in the RT scene, enh/rt_scene)
    Gbuffer = 2,  // pixel shaders: GI + AO + the G-buffer outputs normal + motion (enh/gbuffer, enh/material_variant)
    GbufferMotion = 3,  // ... + per-object motion from the capture vertex shader's previous world position
    Base = 4,     // pixel shaders: the plain GLSL material shader (Device::setUbershader), no enhancement hooks
    GbufferM = 5,        // as Gbuffer with the motion output only (FSR / frame generation read no normal)
    GbufferMotionM = 6,  // as GbufferMotion with the motion output only
    GbufferA = 7,        // GI + AO / screen probes, no G-buffer output (no consumer reads the targets)
    // scene forms (TranslateOptions::sceneRecord, frame-ownership.md 7.10 / 7.11: the draw's data from a DrawRecord): SceneVs of a vertex shader;
    // the pixel shader's plain module and the Gi / Gbuffer variants as scene forms (Device::sceneSlotOf)
    SceneVs = 8,
    ScenePs = 9,
    SceneGi = 10,
    SceneGbuffer = 11,
    SceneGbufferM = 12,
    SceneGbufferA = 13,
    SceneVsLod = 14,  // F4-1: the billboard fade variant of a vertex shader (Device::lodModule) as a scene form
    // scene ABI v2 (TranslateOptions::sceneView, frame-ownership.md 7.19): the same forms with the view table; slot = the v1 slot + 7 (sceneView2Of)
    SceneVs2 = 15,
    ScenePs2 = 16,
    SceneGi2 = 17,
    SceneGbuffer2 = 18,
    SceneGbufferM2 = 19,
    SceneGbufferA2 = 20,
    // S-4 (native.vertex_pull, frame-ownership.md 14.12): the scene ABI v2 vertex shader in its pulled form (TranslateOptions::pullVertices: inputs from the draw's
    // VertexFetch block, no vertex input but the ref stream). Not a "+7" form: Device::variantWorker treats it by name
    SceneVsPull = 21,
    // bone-history motion (native.bone_motion, frame-ownership.md 14.13): BoneVs = the provider slot of the skinned vertex shaders' bone-motion variant (the translated program run a
    // second time with the previous frame's bone image of the bone table: its world position -> rvk_prevWorld, location 15); never drawn as such, only its scene forms. The
    // G-buffer motion pixel variants (GbufferMotion / GbufferMotionM) get scene forms too. v1 forms 23..25, their v2 forms 26..28 (= v1 + 3), the v2 vertex shader's pulled form 29
    BoneVs = 22,
    SceneVsBone = 23,
    SceneGbufferMotion = 24,
    SceneGbufferMotionM = 25,
    SceneVsBone2 = 26,
    SceneGbufferMotion2 = 27,
    SceneGbufferMotionM2 = 28,
    SceneVsBonePull = 29,
    Count
};
static_assert(static_cast<std::size_t>(VariantSlot::Count) == kVariantSlots, "Shader::variants / variantState sizes");
// the slot a scene form is built from (its provider's options + sceneRecord); Count = none (SceneVs / ScenePs: sceneRecord alone)
// a scene form (built by Device::variantWorker from its base slot's provider + sceneRecord); BoneVs is a provider slot only
inline bool isSceneSlot(VariantSlot s) { return s >= VariantSlot::SceneVs && s != VariantSlot::BoneVs; }
inline bool isBoneScene(VariantSlot s) { return s >= VariantSlot::SceneVsBone && s < VariantSlot::Count; }
inline bool isSceneView(VariantSlot s) { return isBoneScene(s) ? s >= VariantSlot::SceneVsBone2 : s >= VariantSlot::SceneVs2 && s <= VariantSlot::SceneVsPull; }
// SceneVs..SceneGbufferA -> the v2 form (+7); SceneVsBone..SceneGbufferMotionM -> theirs (+3)
inline VariantSlot sceneView2Of(VariantSlot v1) {
    return static_cast<VariantSlot>(static_cast<std::uint8_t>(v1) + (isBoneScene(v1) ? 3 : 7));
}
// the v1 form of a v2 (or pulled) form
inline VariantSlot sceneView1Of(VariantSlot s) {
    if (s == VariantSlot::SceneVsPull) return VariantSlot::SceneVs;
    if (s == VariantSlot::SceneVsBonePull) return VariantSlot::SceneVsBone;
    if (!isSceneView(s)) return s;
    return static_cast<VariantSlot>(static_cast<std::uint8_t>(s) - (isBoneScene(s) ? 3 : 7));
}
inline VariantSlot sceneBaseSlot(VariantSlot s) {
    switch (sceneView1Of(s)) {
    case VariantSlot::SceneGi: return VariantSlot::Gi;
    case VariantSlot::SceneGbuffer: return VariantSlot::Gbuffer;
    case VariantSlot::SceneGbufferM: return VariantSlot::GbufferM;
    case VariantSlot::SceneGbufferA: return VariantSlot::GbufferA;
    case VariantSlot::SceneVsBone: return VariantSlot::BoneVs;
    case VariantSlot::SceneGbufferMotion: return VariantSlot::GbufferMotion;
    case VariantSlot::SceneGbufferMotionM: return VariantSlot::GbufferMotionM;
    default: return VariantSlot::Count;
    }
}
// the scene form of a pixel shader's variant slot (Base = the plain module); Count = the variant has none (the capture / motion variants)
inline VariantSlot sceneSlotOf(VariantSlot base) {
    switch (base) {
    case VariantSlot::Base: return VariantSlot::ScenePs;
    case VariantSlot::Gi: return VariantSlot::SceneGi;
    case VariantSlot::Gbuffer: return VariantSlot::SceneGbuffer;
    case VariantSlot::GbufferM: return VariantSlot::SceneGbufferM;
    case VariantSlot::GbufferA: return VariantSlot::SceneGbufferA;
    case VariantSlot::GbufferMotion: return VariantSlot::SceneGbufferMotion;  // (native.bone_motion's skinned draws only: Device::sceneDecide)
    case VariantSlot::GbufferMotionM: return VariantSlot::SceneGbufferMotionM;
    default: return VariantSlot::Count;
    }
}
struct VariantProvider {
    std::string tag;  // part of the SPIR-V cache name (bump it when the variant's GLSL changes)
    // the options of a shader's variant; false = not applicable (the draw keeps the engine's module). Any thread
    std::function<bool(const Shader&, shader::TranslateOptions&)> options;
};

// ---- device -------------------------------------------------------------------------------------------------------

// F8-0 (docs/research/frame-ownership.md 7.20): the identity of an indexed draw as the observer loops know it before any query is built
// (DrawObserver::coversDraw): everything the RT scene's BLAS key and its "is this draw the one of a table entry" test read
struct CoverQuery {
    draws::Kind kind = draws::Kind::Unknown;
    scene::Phase phase{};
    const draws::Tag* tag = nullptr;
    const Buffer* vertexBuffer = nullptr;
    const Buffer* indexBuffer = nullptr;
    std::uint32_t vertexOffset = 0, vertexStride = 0, primitiveType = 4, first = 0, primitives = 0;
    std::int32_t baseVertex = 0;
    bool instanced = false, blending = false;
    // the inputs of an RT material (RtMaterials::signature): the engine's shaders and the bound textures s0..s15
    const Shader* engineVs = nullptr;
    const Shader* enginePs = nullptr;
    Texture* const* textures = nullptr;
};

// Draw replacement (enhancements that take over engine draws, e.g. enh/water: docs/research/water.md). For draws of camera
// views tagged as water (draws::Tag::water, or kind Refracted) the device asks the replacer what to do with the draw before
// it records anything: draw it as the engine asked, skip it, draw the same geometry with other shaders, or draw procedural
// geometry (vkCmdDraw without vertex / index buffers) instead. Render thread.
struct ReplaceQuery {
    draws::Kind kind = draws::Kind::Unknown;
    scene::Phase phase{};
    const draws::Tag* tag = nullptr;
    std::uint32_t mainView = 0;               // 1 = the frame's first main view
    Buffer* vertexBuffer = nullptr;           // stream 0
    std::uint32_t vertexOffset = 0, vertexStride = 0;
    Buffer* indexBuffer = nullptr;            // null for non-indexed draws
    bool indexed = false;
    std::uint32_t primitiveType = 4;          // D3DPRIMITIVETYPE (4 triangle list, 5 strip, 6 fan)
    std::uint32_t first = 0, primitives = 0;  // start index / vertex, primitive count
    std::int32_t baseVertex = 0;
    std::uint32_t minIndex = 0, numVertices = 0;  // DrawIndexedPrimitive's vertex range (indexed draws; 0 / 0 when unknown)
    bool instanced = false;                   // stream 0 frequency INDEXEDDATA
    std::uint32_t instances = 1;              // its instance count (1 when not instanced)
    Texture* const* textures = nullptr;       // the bound textures s0..s15 (pixel)
    const Shader* engineVs = nullptr;
    const Shader* enginePs = nullptr;
    // the device's reference to the engine's vertex shader (a replacement may keep it: DrawReplacement::vs); a pointer, so
    // building a query per observer / replacer costs no atomic reference count
    const VertexDecl* decl = nullptr;         // the draw's vertex declaration
    bool blended = false;                     // alpha blending SRCALPHA / INVSRCALPHA on
    bool blending = false;                    // alpha blending on (any factors)
    std::uint8_t srcBlend = 2, dstBlend = 1;  // D3DBLEND of the colour (when blending)
    bool alphaTest = false;                   // alpha test on with a function other than ALWAYS
    std::uint32_t alphaFunc = 8;              // D3DCMPFUNC of the alpha test (8 = always)
    std::uint32_t alphaRef = 0;               // its reference 0..255
    bool depthWrite = true;                   // z test + z write on (a depth target bound)
    bool depthBias = false;                   // a depth bias (constant or slope-scaled) is set
    // F2c-2 (native.owned_records): the draw's world rows c33..c35 (3 float4) as the object table holds them; null = read the register file (c33..c35)
    const float* objectRows = nullptr;
    // streams 1..3 (index k - 1): a skinned head's morph targets (POSITION1..3; enh/rt_scene's skinning pass reads them, 14.15 c); null buffer = not bound
    Buffer* morphBuffer[3] = {nullptr, nullptr, nullptr};
    std::uint32_t morphOffset[3] = {0, 0, 0}, morphStride[3] = {0, 0, 0};
};

// (defined with the observer loops' helper: the CoverQuery of a built query)
inline CoverQuery coverOf(const ReplaceQuery& q) {
    return {q.kind, q.phase, q.tag, q.vertexBuffer, q.indexBuffer, q.vertexOffset, q.vertexStride, q.primitiveType, q.first, q.primitives, q.baseVertex,
            q.instanced, q.blending, q.engineVs, q.enginePs, q.textures};
}

struct DrawReplacement {
    enum class Action : std::uint8_t {
        Engine,      // draw it as the engine asked
        Skip,        // not drawn (still counted and recorded, flagged replaced)
        Shaders,     // the engine's geometry and constants with `vs` / `ps`
        Procedural,  // `vertices` vertices of `vs` / `ps` without vertex / index buffers (vkCmdDraw)
    };
    Action action = Action::Engine;
    std::shared_ptr<Shader> vs, ps;     // reflections decide vertex inputs and constant uploads (all of c# when 256)
    rhi::GpuRef block;                  // replaces the DrawBlock reference (push constant `draw`); null = keep
    std::uint32_t vertices = 0;         // Procedural
    bool cullNone = false;              // override the cull mode
    bool opaque = true;                 // blending off, all channels written
    std::array<Texture*, 16> sampled{};  // made readable (SHADER_READ) before the rendering scope
    draws::Water water = draws::Water::None;  // the body as the replacer classified it (goes into the record)
};

class DrawReplacer {
public:
    virtual ~DrawReplacer() = default;
    // whether the replacer is asked about this draw (called for every draw while it is registered: keep it cheap); the
    // first registered replacer that handles a draw decides it
    virtual bool handles(draws::Kind kind, scene::Phase phase, const draws::Tag& tag) const = 0;
    virtual void replace(Device& device, const ReplaceQuery& query, DrawReplacement& out) = 0;
    // the main view's opaque static meshes (Solid pass, indexed, from buffers), when wanted: an enhancement reads the
    // engine's geometry (enh/water: the sea floor field). Called before the draw is recorded; must not change device state.
    virtual bool wantsOpaque() const { return false; }
    virtual void observeOpaque(Device& /*device*/, const ReplaceQuery& /*query*/) {}
};

// The N5.0 op tap (native/frame_tap.*): every draw, at the start of prepareDraw (before anything is decided), with the tag
// and the phase. Must not change device state. Render thread. onDraw returning true drops the draw (N5.1 native/static_ops:
// an op whose draws were issued by the renderer itself): nothing of it is recorded or observed.
struct DrawTapInfo {
    std::uint32_t index = 0;  // the device's draw number (stats().draws)
    scene::Phase phase{};
    draws::Kind kind = draws::Kind::Unknown;
    const draws::Tag* tag = nullptr;
    const Device* device = nullptr;  // the full state of the draw (getters only)
    bool indexed = false;
    std::uint32_t primitiveType = 0;
    std::uint32_t first = 0;  // start index / vertex
    std::int32_t baseVertex = 0;
    std::uint32_t minIndex = 0, numVertices = 0;
    std::uint32_t primitives = 0;
    std::uint32_t instances = 1;
    bool instanced = false;  // stream 0 frequency INDEXEDDATA
    bool additive = false;   // alpha blending on with destination ONE
    bool colorWrite = true;  // any colour channel written (COLORWRITEENABLE != 0)
    std::uint64_t vsHash = 0;  // the vertex shader's hash (0 = none bound)
    std::uint64_t psHash = 0;
    std::uint32_t blend = 0;   // (ALPHABLENDENABLE != 0) | SRCBLEND << 8 | DESTBLEND << 16 | ZFUNC << 24 (diagnostics)
};
class DrawTap {
public:
    virtual ~DrawTap() = default;
    virtual bool onDraw(const DrawTapInfo& draw) = 0;  // true: drop the draw
};

// Draw observation (enh/rt_scene: the ray tracing scene, docs/research/rt-reflections.md 3): an enhancement that reads the
// engine's geometry and per-draw constants (world rows in the vertex shader constants) without changing anything. Indexed
// draws from buffers of the views / kinds it observes are shown to it in prepareDraw, before the draw is recorded (the
// constants and the bound state are the draw's). Render thread.
struct OwnedFrame;
class DrawObserver {
public:
    virtual ~DrawObserver() = default;
    // whether a draw of this kind in this phase is observed (called for every indexed draw while registered: keep it cheap)
    virtual bool observes(draws::Kind kind, scene::Phase phase) const = 0;
    // must not change device state (Device::vsConstant, streamSource, Buffer::gpu / readCpu / deviceAddress are fine)
    virtual void observe(Device& device, const ReplaceQuery& query) = 0;
    // F8-0 pre-filter, asked right after `observes` and before the query is built: true = the observer already holds everything observe() would
    // learn from this draw (a persistent table entry): the draw is not shown to it. Must be cheap and must not change device state.
    virtual bool coversDraw(const CoverQuery& /*draw*/) { return false; }
    // F2c-3 (native.owned_static): what the observer reads of the draw's constant registers. An owned draw has no composed register file: `Material` = the
    // world rows (ReplaceQuery::objectRows) and the registers of the material / view / object windows (the device loads their CPU images before observe),
    // `All` (the default) = anything: owned draws are not made while such an observer is registered
    // `MaterialLighting` (F-VAR) = `Material` + the lighting registers c14..c37 of the draw's pass (Gi, material RT: the ambient / sun / point lights / indoor registers), composed on the
    // CPU from the illumination table's records for the owned draw (LightingImages::cpuImage)
    enum class Registers : std::uint8_t { None, Material, MaterialLighting, All };
    virtual Registers registersRead() const { return Registers::All; }
    // the observer's name for the owned path's gate reasons (rvk.log `native: owned static: gated: ...`, the stage line)
    virtual const char* observerName() const { return "an unnamed observer"; }
    // the buffer `id` got a new static GPU copy (a new generation: the old VkBuffer and its address are retired) or is
    // released (its GPU copy dies after the frame): anything built from it must be forgotten
    virtual void bufferChanged(std::uint64_t /*bufferId*/) {}
    // the texture is about to be destroyed (render thread, Device::processReleases): pointers to it must be forgotten
    virtual void textureReleased(const Texture* /*texture*/) {}
    // F2c-4b (docs/research/frame-ownership.md 7.23; native.owned_persistent): an observer that returns true from bulkOwned() is not shown the persistent owned static
    // draws (plain StaticMesh, not instanced / blended) one by one before they are queued, but once per owned pass (and before an op of the pass is composed late) through
    // observeOwned: the draws of the pass with their immutable record views, `entered` / `left` deltas against the pass kind's previous pass. The observer may keep
    // per-record state (OwnedRecordView::id) and skip what it already holds; it must not request captures for these draws (they are queued already). The default
    // implementation is the per-draw loop (observes / coversDraw / registers loaded / observe), so an observer that only opts in behaves as before.
    virtual bool bulkOwned() const { return false; }
    // F4-3 (frame-ownership.md 7.25b): an observer of the main view's SpeedTree draws that can be served by Device::ownedTreeObserve (per run of instances: the template's
    // query, the instance rows in the register file c32 + 4 i, the identity ramp as stream 1, the template's CPU window images loaded) instead of the engine-order draw
    // calls. It must not need a vertex capture for these draws (they are queued already): false while it would (the RT scene's animated trees)
    virtual bool ownedTreesOk() const { return false; }
    // F-VAR2 (frame-ownership.md 7.45): an observer of the main view's vegetation (grass) draws that is served by Device::ownedVegObserve (one call per batch: the template's pixel
    // windows with the batch's lighting entry loaded into the register file, instances = the batch's count); it must not need a vertex capture for these draws
    virtual bool ownedVegOk() const { return false; }
    virtual void observeOwned(Device& device, const OwnedFrame& frame);
};

// The ray tracing scene of the frame, for the FrameBlock (shader/abi.h FrameBlock::rtScene / rtInfo): valid only in the
// submission that built it (`frame` = Device::frameValue() at the build; a mid-frame flush starts another one)
struct RtSceneRef {
    std::uint64_t tlas = 0;  // the TLAS's raw address (vkGetAccelerationStructureDeviceAddressKHR; the D3D12 form is tlasSlot)
    rhi::GpuRef records;
    std::uint32_t tlasSlot = 0;  // the TLAS in the bindless acceleration structure array (HLSL ray queries; abi.h kBindingTlas)
    std::uint32_t instances = 0;
    std::uint32_t recordCount = 0;
    std::uint64_t frame = 0;
};

// What an injected pass gets (Device::inject, d3d9/inject.h; render thread). `cmd` is the frame's command list outside any
// rendering scope (with nothing bound: GameDevice::passCommands); the pass records its own barriers (CommandList::transition, Device::readable), binds
// its own pipeline and push constants, and may begin / end its own rendering scopes. It must not change the D3D9 state.
struct PassContext {
    Device& device;
    rhi::CommandList& cmd;
    inject::Point point;
    std::uint32_t mainView = 0;      // 1 = the frame's first main view, 2 = the second, ...; 0 for BeforeGui
    rhi::GpuRef frameBlock;          // shader::abi::FrameBlock of this frame (RVK_PASS_PUSH_HEADER's frame)
    Texture* sceneHdr = nullptr;     // tagged images (scene::ImageRole), null when the engine has not revealed them
    Texture* depthMap = nullptr;
    Texture* refraction = nullptr;
    Texture* target = nullptr;       // render target 0's texture (null when none is bound; may be the back buffer)
};

struct DeviceOptions {
    HWND__* window = nullptr;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    bool vsync = true;
    double maxFps = 0.0;  // present-rate cap; 0 = the window monitor's refresh rate with vsync, none without
    std::uint32_t depthFormat = 0;  // D3DFORMAT of the auto depth stencil, 0 = none
    bool d3d12Messages = false;  // [debug] d3d12_messages (GameDeviceOptions)
    bool d3d12Dred = true;       // [debug] d3d12_dred
    bool validation = false;
    bool syncValidation = false;  // + synchronization validation ([vulkan] sync_validation)
    int adapter = -1;
    std::filesystem::path cacheDir;  // pipeline + shader caches; empty = no disk cache
    bool offscreen = false;          // no swapchain (tests)
    std::uint32_t logDraws = 0;      // log the first N draws in detail
    bool perf = false;               // [debug] perf: GPU timestamps + CPU timings (Device::takePerf)
    bool pipelineLibraries = false;  // [vulkan] pipeline_libraries (off: AMD driver crash, vk/pipeline_library.cpp)
    std::uint32_t perfFrame = 0;     // [debug] perf_frame: log the GPU time of every pass of this frame
    bool allowRayTracing = true;     // rt.allow: enable the ray tracing tier when the GPU has it (GameDeviceOptions)
    bool frameGenQueues = false;     // framegen.mode != off at start: the extra queues of the frame generation swapchain
    rhi::Backend backend = rhi::Backend::Vulkan;  // the game device's API
};

struct Viewport {
    std::uint32_t x = 0, y = 0, width = 0, height = 0;
    float minZ = 0.0f, maxZ = 1.0f;
};

struct FrameStats {
    std::uint64_t frames = 0;
    std::uint32_t draws = 0;
    std::uint32_t renderPasses = 0;
    std::uint32_t pipelines = 0;
    std::uint32_t drawsSkipped = 0;  // draws without a usable shader / target
    std::uint64_t drawsTakenOver = 0;  // engine draws dropped because the renderer drew them itself (cumulative; DrawTap)
    std::uint32_t lastFrameDraws = 0;
    std::uint64_t acquireFailures = 0;
    std::uint64_t loadingFramesDropped = 0;  // loading-screen frames rendered but not shown (setUnpaced)
    std::uint64_t presentsQueued = 0;
    std::uint64_t generatedPresented = 0;  // generated frames shown by the manual frame generation (included in presentsQueued)
    std::uint64_t presentSuboptimal = 0;
    std::uint64_t swapchainRecreates = 0;
    double acquireMs = 0.0;  // summed time in vkAcquireNextImageKHR / vkQueuePresentKHR (per stats interval)
    double presentMs = 0.0;  // on the present thread
    double presentWaitMs = 0.0;  // the render thread waiting for the present thread before it acquires
    double pacerMs = 0.0;  // summed time the frame pacer slept
    // cumulative since creation
    std::uint64_t totalDraws = 0;
    std::uint64_t skippedNoShader = 0;
    std::uint64_t skippedCompiling = 0;  // a shader of the draw was still compiling on the workers
    std::uint64_t skippedNoTarget = 0;
    std::uint64_t skippedVertexInput = 0;
    std::uint64_t skippedPipeline = 0;
    std::uint64_t totalClears = 0;
    std::uint64_t totalRenderPasses = 0;
    std::uint64_t scopeClears = 0;       // clears folded into a scope's load ops (no vkCmdClearAttachments)
    mutable std::uint64_t stencilPlaneDraws = 0;   // draws with the stencil test on, on a surface with a stencil plane
    mutable std::uint64_t stencilNoPlaneDraws = 0;  // ... on a depth surface without stencil bits (D24X8, DF24: a D3D9 test that passes; counted, expected 0)
};

// [debug] perf: timings and volumes summed over a stats interval (Device::takePerf). CPU times are wall-clock ms on the render
// thread; GPU times come from timestamps (per submission, read back two submissions later).
struct PerfStats {
    std::uint64_t frames = 0;
    double cpuFrameMs = 0.0;    // from the end of one present() to the start of the next: the engine's frame incl. our HAL
    double drawMs = 0.0;        // inside Device draw calls (state translation, uploads, pipeline lookup, command recording)
    double drawRecordMs = 0.0;  // of that: the draw's histogram entry + record (d3d9/draws.h)
    double presentMs = 0.0;     // inside present() (blit recording, submit, pacer, acquire, vkQueuePresentKHR, next frame begin)
    double submitMs = 0.0;      // ending the command buffers + vkQueueSubmit2
    double slotWaitMs = 0.0;    // waiting for a free frame slot (the GPU is a full slot behind)
    std::uint32_t readbacks = 0;  // GPU -> CPU copies that flush and wait for the GPU mid-frame
    double readbackMs = 0.0;
    std::uint64_t gpuSubmits = 0;  // submissions with valid timestamps
    double gpuMs = 0.0;            // main command buffers
    double gpuUploadMs = 0.0;      // upload command buffers
    std::uint64_t draws = 0;
    std::uint64_t renderPasses = 0;
    std::uint64_t pipelineBinds = 0;
    std::uint64_t batchedDraws = 0;        // draws recorded through a batch (flushBatch)
    std::uint64_t batches = 0;             // batches recorded
    std::uint64_t ringBytes = 0;           // transient ring memory handed out (constants, dynamic buffers, staging)
    std::uint64_t textureUploadBytes = 0;  // texel bytes copied to images
    std::uint64_t bufferUploadBytes = 0;   // bytes copied into dynamic-buffer ring copies / static buffers
    std::uint64_t pipelinesBuilt = 0;      // pipelines created at draw time (monolithic or linked from libraries)
    double pipelineMs = 0.0;
    std::uint64_t libraries = 0;           // pipeline libraries compiled (shader creation; any thread)
    double libraryMs = 0.0;
    std::uint64_t optimized = 0;           // link-time-optimised pipelines built in the background
    double optimizeMs = 0.0;
    std::uint64_t prewarmed = 0;           // pipelines of earlier runs built in the background before their first draw
    double prewarmMs = 0.0;
    std::uint64_t shadersBuilt = 0;        // translated shaders (decode + GLSL + module; glslang only on SPIR-V cache misses)
    std::uint64_t shadersCompiled = 0;     // of those, compiled by glslang (SPIR-V disk cache miss)
    double shaderMs = 0.0;
    std::uint64_t injections = 0;          // injection points that ran passes (d3d9/inject.h)
    double injectMs = 0.0;                 // CPU time recording them
    std::map<std::string, double> injectPassMs;  // ... per pass (name at point; [debug] perf only)
    std::uint64_t frameBlocks = 0;         // FrameBlock uploads
    std::uint64_t constUploads[2] = {};    // StageConsts blocks uploaded (VS, PS): one per draw whose stage was dirty
    std::uint64_t constBytes = 0;          // bytes copied into them
    std::uint64_t constIntBoolSkipped = 0; // ... of the blocks, those without the int / bool part (the shader reads none)
    std::uint64_t constSetsUnchanged = 0;  // register setter calls that changed nothing on a clean stage (no upload follows)
    // split constants (abi.h windows): per stage and window the blocks uploaded / reused from the last upload / bytes written,
    // draws whose stage was split / legacy, mismatches found by debug.split_verify
    std::uint64_t splitBlocks[2][4] = {};
    std::uint64_t splitReused[2][4] = {};
    std::uint64_t splitBytes[2][4] = {};
    std::uint64_t splitDraws[2] = {};
    std::uint64_t legacyDraws[2] = {};
    std::uint64_t splitMismatches = 0;
    std::vector<float> frameIntervalsMs;   // present-to-present
    // per frame (same order as frameIntervalsMs): engine + HAL CPU before the present, the present call itself, the frame
    // slot wait inside the frame; the perf line names the spike frames' share of each (performance.md, frame spikes)
    struct FrameSplit {
        float cpuMs = 0.0f, presentMs = 0.0f, slotWaitMs = 0.0f;
    };
    std::vector<FrameSplit> frameSplits;
    pacing::FramePacer::Stats pacer;
    double pacerRate = 0.0;
    bool pacerVblank = false;
};

class Device {
public:
    Device();
    ~Device();
    bool create(const DeviceOptions& options);
    void destroy();

    // resources (any thread). Destroy through release(): the render thread unbinds and deletes them at the next frame
    // boundary (D3D9 kept bound objects alive through reference counting; the game frees on loader threads).
    std::unique_ptr<Texture> createTexture(const TextureDesc& desc);
    std::unique_ptr<Buffer> createBuffer(std::uint32_t size, std::uint32_t usage, std::uint32_t pool, bool index, bool index32);
    std::shared_ptr<Shader> createShader(std::span<const std::uint32_t> bytecode);  // cached by bytecode hash
    std::unique_ptr<VertexDecl> createVertexDecl(std::span<const VertexElement> elements);
    void release(std::unique_ptr<Texture> texture);
    void release(std::unique_ptr<Buffer> buffer);
    void release(std::unique_ptr<VertexDecl> decl);
    // live textures / buffers (their constructors / destructors): destroy() frees the GPU objects of those still alive, so
    // resources the engine releases only after its device (at process exit) never touch a destroyed device. Returns false
    // (untrack) when the resource was orphaned: its destructor must not touch the device.
    void trackLive(Texture* texture);
    void trackLive(Buffer* buffer);
    bool untrackLive(Texture* texture);
    bool untrackLive(Buffer* buffer);

    // state (render thread)
    void setRenderState(std::uint32_t rs, std::uint32_t value);
    std::uint32_t renderState(std::uint32_t rs) const { return rs < RS_COUNT ? rs_[rs] : 0; }
    void setSamplerState(std::uint32_t sampler, std::uint32_t ss, std::uint32_t value);  // sampler: 0..15, 257..260
    std::uint32_t samplerState(std::uint32_t sampler, std::uint32_t ss) const;
    void setTexture(std::uint32_t sampler, Texture* texture);
    Texture* texture(std::uint32_t sampler) const;
    void setStreamSource(std::uint32_t stream, Buffer* buffer, std::uint32_t offset, std::uint32_t stride);
    void setStreamSourceFreq(std::uint32_t stream, std::uint32_t setting);
    void setIndices(Buffer* buffer);
    void setVertexDecl(VertexDecl* decl);
    // bulk paths of the native composition (native/static_ops, native/effect_ops; render thread): a pixel stage's sampler state
    // row (SAMP_COUNT entries, index = D3DSAMPLERSTATETYPE) and several of its states at once, its texture without the slot map
    const std::uint32_t* samplerStateRow(std::uint32_t stage) const { return samp_[stage < 16 ? stage : 0].data(); }
    void setSamplerStates(std::uint32_t stage, const std::uint32_t* states, const std::uint32_t* values, std::uint32_t n) {
        if (stage >= 16) return;
        auto& row = samp_[stage];
        bool changed = false;
        for (std::uint32_t i = 0; i < n; ++i) {
            if (states[i] < SAMP_COUNT && row[states[i]] != values[i]) {
                row[states[i]] = values[i];
                changed = true;
            }
        }
        if (changed) samplerSlotCache_[stage] = UINT32_MAX;
    }
    Texture* stageTexture(std::uint32_t stage) const { return tex_[stage < 16 ? stage : 0]; }
    void setStageTexture(std::uint32_t stage, Texture* texture) {
        if (stage < 16) tex_[stage] = texture;
    }
    void setVertexShader(const std::shared_ptr<Shader>& shader);
    void setPixelShader(const std::shared_ptr<Shader>& shader);
    // the sampler slots the bound shaders read: pixel samplers 0..15 from the pixel shader, vertex samplers 16.. from the vertex shader
    std::uint32_t boundSamplerMask() const { return (ps_->samplerMask & 0xFFFFu) | (vs_->samplerMask & ~0xFFFFu); }
    const Shader* vertexShader() const { return vs_.get(); }
    const Shader* pixelShader() const { return ps_.get(); }
    // the current shaders as references (save / restore around draws the renderer issues itself: native/static_ops)
    ShaderRef vertexShaderRef() const { return vs_; }
    ShaderRef pixelShaderRef() const { return ps_; }
    void setVertexShaderRef(ShaderRef shader) {
        if (shader != vs_) {
            vs_ = shader;
            vsConstDirty_ = true;
        }
    }
    void setPixelShaderRef(ShaderRef shader) {
        if (shader != ps_) {
            ps_ = shader;
            psConstDirty_ = true;
        }
    }
    VertexDecl* vertexDecl() const { return decl_; }
    Buffer* indexBuffer() const { return indices_; }
    void setVertexShaderConstantF(std::uint32_t start, const float* data, std::uint32_t count);
    void setPixelShaderConstantF(std::uint32_t start, const float* data, std::uint32_t count);
    void setVertexShaderConstantI(std::uint32_t start, const std::int32_t* data, std::uint32_t count);
    void setPixelShaderConstantI(std::uint32_t start, const std::int32_t* data, std::uint32_t count);
    void setVertexShaderConstantB(std::uint32_t start, const std::int32_t* data, std::uint32_t count);
    void setPixelShaderConstantB(std::uint32_t start, const std::int32_t* data, std::uint32_t count);
    void getVertexShaderConstantF(std::uint32_t start, float* data, std::uint32_t count) const;
    void getPixelShaderConstantF(std::uint32_t start, float* data, std::uint32_t count) const;
    void setRenderTarget(std::uint32_t index, Surface* surface);  // index 0 also resets the viewport (D3D9)
    void setDepthStencil(Surface* surface);
    Surface* renderTarget(std::uint32_t index) const { return rt_[index]; }
    Surface* depthStencil() const { return ds_; }
    void setViewport(const Viewport& vp);
    const Viewport& viewport() const { return viewport_; }
    void setScissorRect(std::int32_t left, std::int32_t top, std::int32_t right, std::int32_t bottom);
    const std::int32_t* scissorRect() const { return scissor_; }  // left, top, right, bottom

    // commands (render thread)
    void clear(std::uint32_t flags, std::uint32_t color, float z, std::uint32_t stencil);
    void drawPrimitive(std::uint32_t type, std::uint32_t startVertex, std::uint32_t primitiveCount);
    void drawIndexedPrimitive(std::uint32_t type, std::int32_t baseVertex, std::uint32_t minIndex, std::uint32_t numVertices,
                              std::uint32_t startIndex, std::uint32_t primitiveCount);
    void drawPrimitiveUP(std::uint32_t type, std::uint32_t primitiveCount, const void* data, std::uint32_t stride);
    void drawIndexedPrimitiveUP(std::uint32_t type, std::uint32_t minIndex, std::uint32_t numVertices, std::uint32_t primitiveCount,
                                const void* indices, bool index32, const void* vertices, std::uint32_t stride);
    // StretchRect: rects = {left, top, right, bottom} or null for the whole surface; linear = D3DTEXF_LINEAR
    void stretchRect(Surface* src, const std::int32_t* srcRect, Surface* dst, const std::int32_t* dstRect, bool linear);
    // GetRenderTargetData: copy a render target into a CPU-side texture level (waits for the GPU)
    bool readRenderTarget(Surface* src, Texture* dst, std::uint32_t dstLevel);
    // D3DXLoadSurfaceFromSurface between 2D texture levels on the GPU (blit: format conversion incl. BC decompression, scaling;
    // linear = a filtering D3DX_FILTER). A CPU-side destination gets the result through a temporary render target read back
    // into its CPU copy (waits for the GPU). Rects {left, top, right, bottom} or null; render thread only. `why` on failure.
    bool loadSurfaceFromSurface(Texture* dst, std::uint32_t dstLevel, const std::int32_t* dstRect, Texture* src,
                                std::uint32_t srcLevel, const std::int32_t* srcRect, bool linear, std::string* why = nullptr);
    // copy one level / layer of an image into `out` (storage layout, tightly packed; waits for the GPU)
    bool readImageLevel(rhi::Texture& tex, std::uint32_t level, std::uint32_t layer, VkExtent3D extent, std::uint64_t size,
                        std::vector<std::uint8_t>& out);
    // read the back buffer as tightly packed BGRA8 (waits for the GPU)
    bool readBackBuffer(std::vector<std::uint8_t>& bgra, std::uint32_t& width, std::uint32_t& height);

    // frame
    void present();
    bool resize(std::uint32_t width, std::uint32_t height);
    // the window's client size changed (borderless placement): the swapchain follows it now (the back buffer keeps its size)
    void windowResized();
    void setVsync(bool vsync);
    bool vsync() const { return options_.vsync; }
    // ---- AMD frame generation swapchain (vk/fg_swapchain.h, enh/framegen.h) ----
    // Switches the presentation to AMD's swapchain (or back to ours). Render thread, between frames: waits for the GPU and
    // re-creates the swapchain. `loaderDll` = the FidelityFX loader. True when the swapchain is the requested kind afterwards;
    // false: the reason is frameGenReason() (ours is in use; a minimised window is retried by the caller).
    bool setFrameGenSwapchain(bool want, const std::filesystem::path& loaderDll);
    bool frameGenSwapchainActive() const { return game_->presenter().frameGenActive(); }
    // shutdown: the generator's context goes (FrameGenClient), then AMD's swapchain, without building another
    void shutdownFrameGenSwapchain();
    rhi::PresentWrapper* frameGenSwapchain() { return presentWrapper(); }
    // the AMD swapchain as the RHI's PresentWrapper (null while it is not in use)
    rhi::PresentWrapper* presentWrapper();
    // The generator's owner: told (render thread, presenter idle) before the swapchain is re-created or destroyed so that it
    // drops what refers to it (the generator context must go before the swapchain's)
    struct FrameGenClient {
        virtual ~FrameGenClient() = default;
        virtual void swapchainWillChange() = 0;
    };
    void setFrameGenClient(FrameGenClient* client) { fgClient_ = client; }
    // the generator runs for this present (set from its BeforePresent pass): the pacer holds the base rate at half the output rate
    void setFrameGenRunning(bool on);
    bool frameGenRunning() const { return fgRunning_; }
    // frame generation swapchain: until the previous present returned and the GPU work it queued (interpolation) is done (the
    // generator's calls are recorded afterwards: its view ring, frame-generation.md 9.4)
    bool waitFrameGenPresent();
    // submits the frame's recorded commands now, waits for the GPU and starts a new command buffer (what a read-back does)
    void flushFrameAndWait();
    // numbers since the last call, for the 5 s log lines
    struct FrameGenReport {
        bool swapchain = false;
        double seconds = 0.0;
        std::uint64_t basePresents = 0;     // our presents (rendered frames shown)
        std::uint64_t realPresents = 0;     // the swapchain's presents to the display (generated frames included)
        std::uint64_t callbacks = 0, generated = 0, uiFrames = 0;
        double callbackMs = 0.0;
        std::vector<double> gapsMs;         // the real presents' gaps (the sampler, [debug] perf only)
        std::vector<double> times;          // ... and their times (seconds)
    };
    FrameGenReport takeFrameGenReport();
    // present-rate cap (0 = the refresh rate with vsync, none without); applies at the next present
    void setMaxFps(double fps);
    // the opaque passes' draws grouped by pipeline (vulkan.batch_draws, docs/research/performance.md 12); applies from the next
    // draw (a pending batch is recorded first)
    void setClusters(bool on);
    bool clustersOn() const { return clusters_; }
    // F5-2 (native.cluster_cull, frame-ownership.md 7.34): the persistent owned records of clustered draws carry their cluster rows; the compaction pass culls them per view
    void setClusterCull(bool on) { clusterCull_ = on; }
    void setClusterCullShadows(bool on) { clusterCullShadows_ = on; }  // F3-2 (7.37): the sun cascades' owned flushes cull too (side planes only)
    bool clusterCullOn() const { return clusterCull_; }
    // frame-ownership.md 7.47: whether the main view that began last runs owned (its ops left the lists; the frame tap sets it at each main view's start). The picture check reads it
    void setMainViewOwned(bool owned) { mainViewOwned_ = owned; }
    bool mainViewOwned() const { return mainViewOwned_; }
    // tests: the CPU twin of an object window's world rows (c33..c35) for the cull's check frames, in place of the object table's
    // check frames, at the present (OwnedCompact::flush): the exact per-cluster verdict from the CPU twins as they stand NOW = what the GPU reads
    void cullCheckFinish(gpuscene::OwnedCompact::CullItem& ci);
    void setCullWorldRowsHook(std::function<bool(std::uint64_t, float*)> hook) { cullWorldRowsHook_ = std::move(hook); }
    void setBatching(bool on);
    // the opaque passes' draws recorded on worker threads (debug.record_threads); applies from the next rendering scope
    void setRecordThreads(bool on) { recordThreadsWanted_ = on; }
    bool batching() const { return batching_; }
    // register setters compare a clean stage's registers first: a write that changes nothing keeps its constant block
    // (debug.constant_compare, vulkanize plan.md V1)
    void setConstantCompare(bool on) { constCompare_ = on; }
    // split constants (debug.split_constants, plan.md V2): the draws' constants uploaded by register window
    void setSplitConstants(bool on) {
        splitConstants_ = on;
        dirtyConstWindows();
    }
    void setSplitVerify(bool on) { splitVerify_ = on; }
    // Window override (frame-ownership.md 7.8, F1a): the register windows `w` covers are not uploaded for the draws prepared while it is set (null =
    // none); the caller keeps the object alive until it clears it. Render thread.
    void setOwnedWindows(const OwnedWindows* w) { owned_ = w; }
    const OwnedWindows* ownedWindows() const { return owned_; }  // (F4-5: the RT scene reads the wind registers the draw reads)
    // ---- F2b-1 scene draws (docs/research/frame-ownership.md 7.11; src/d3d9/scene_draws.cpp): the static ops' main-view z / solid draws are recorded as
    // DrawRecords and drawn by one indirect call per run of draws with the same pipeline and state. setSceneDraws = the setting (false without the
    // capabilities: logged once), setSceneIssue = the caller is issuing such draws now (native/static_ops)
    struct SceneDrawStats {
        std::uint64_t candidates = 0, sceneDraws = 0, replayedSceneDraws = 0, indirectCalls = 0, records = 0, maxRun = 0, groups = 0;
        std::uint64_t breaks[5] = {};     // runs cut inside one pipeline: vertex input, dynamic state, index binding, frame block, run size
        std::uint64_t fbNotBatchable = 0, fbVariantKind = 0, fbVariantFailed = 0, fbVariantPending = 0, fbPipelinePending = 0, fbNoRoom = 0, fbRingFull = 0, fbNullAddress = 0;
        std::uint64_t earlyPipelines = 0;  // F15-1: v2 / pulled pipelines requested while the draw's v1 pipeline was still being built
        std::uint64_t checks = 0, checkedRecords = 0, checkedBytes = 0, checkDiffering = 0, checkSkipped = 0;
        // S-1a: pixel shader form per draw kind x UberWhy: counted per scene draw decision (decided) and per owned item per flush (owned)
        std::uint64_t uberDecided[static_cast<std::size_t>(draws::Kind::Count)][7] = {};  // [kind][UberWhy]
        std::uint64_t uberOwned[static_cast<std::size_t>(draws::Kind::Count)][7] = {};
        // S-1b: vertex shader form per draw kind: 0 translated (not covered) / 1 ubershader / 2 failed (translated)
        std::uint64_t uberVsDecided[static_cast<std::size_t>(draws::Kind::Count)][3] = {};
        std::uint64_t uberVsOwned[static_cast<std::size_t>(draws::Kind::Count)][3] = {};
    };
    void setSceneDraws(bool on, std::uint32_t checkEvery);
    // F2c-4-0 (frame-ownership.md 7.19): scene ABI v2 (the view table). `on` = the retained draws also get the v2 form's pipeline (CachedDraw::scenePipeline2); the table itself
    // is gpuscene::ViewTable (it calls setSceneViewTable once its buffer exists: base, the CPU twin of its slots, the slot count)
    void setSceneViews(bool on) { if (on != sceneV2_) ++variantEpoch_; sceneV2_ = on; }
    void setSceneViewTable(rhi::GpuRef base, const shader::abi::ViewBlock2* shadow, std::uint32_t slots) { sceneViews_ = {base, shadow, slots}; }
    // the posFix of the view bound now (half pixel + jitter + depth remap): what a view table slot of the view must hold
    void viewPosFix(float out[4]) { float jx, jy; currentPosFix(out, jx, jy); }
    bool sceneViewsReady() const { return sceneV2_ && !sceneViews_.base.null() && sceneViews_.shadow != nullptr; }
    void setSceneIssue(bool on) { sceneIssue_ = on; }
    // F3-1 (frame-ownership.md 7.36, native.owned_shadows): the sun cascades' static mesh z draws (Shadow / ShadowDepth) get their scene forms too (v1 and the v2 form of the owned path)
    void setSceneShadows(bool on) { if (on != sceneShadows_) ++variantEpoch_; sceneShadows_ = on; }
    // F6-1 (frame-ownership.md 7.40, native.owned_actors): the skinned actors' z / solid draws of the main view and z draws of the cascades get their scene forms too
    void setSceneActors(bool on) { if (on != sceneActors_) ++variantEpoch_; sceneActors_ = on; }
    // F2f (frame-ownership.md 14.19, native.owned_instances): the main view's instance-list draws (draws::Kind::InstancedMesh, not hardware-instanced) get their scene forms like the
    // static meshes' (their VS window 2 is an instance row image or an object record), and a rigid one with an object history takes the scene bone forms instead of the push form
    void setSceneInstances(bool on) { if (on != sceneInstances_) ++variantEpoch_; sceneInstances_ = on; }
    bool sceneInstances() const { return sceneInstances_; }
    // F9 / F14 (frame-ownership.md 14.20, native.owned_ordered): the main view's order-dependent static mesh draws (the solid list's blended / no-z-write draws, the alpha and
    // refracted lists' mesh draws) get their scene forms too although they are not batchable: the owned path draws them at their op's place in the engine's order
    // (an ordered ownedFlush: one indirect command per run of consecutive equal buckets, recorded after the pending batch), never grouped by pipeline
    void setSceneOrdered(bool on) { if (on != sceneOrdered_) ++variantEpoch_; sceneOrdered_ = on; }
    bool sceneOrdered() const { return sceneOrdered_; }
    // (an ordered owned draw: the bulk observers' contract (plain opaque static draws) does not hold; its observers see it per draw)
    void setOwnedBulkSuspended(bool on) { ownedBulkSuspended_ = on; }
    // F9 / F14: a run of ordered owned draws is pending (consecutive ordered ops of a list, one flush for all): anything else the device records first queues the run
    // (submitInterned, flushBatch: every draw, clear, scope end, copy), so the run stays at its place in the frame. `fn(ctx)` queues it (and clears the flag)
    using OrderedHook = void (*)(void*);
    void setOrderedHook(OrderedHook fn, void* ctx) {
        orderedHook_ = fn;
        orderedHookCtx_ = ctx;
    }
    void setOrderedPending(bool on) { orderedPending_ = on && orderedHook_ != nullptr; }
    bool orderedPending() const { return orderedPending_; }
    void orderedFlushPending() {
        if (!orderedPending_) return;
        orderedPending_ = false;
        orderedHook_(orderedHookCtx_);
    }
    // F9 / F14: the register file is saved the first time an observer loads an owned draw's windows into it (loadOwnedRegisters) while armed; restore puts it back (the
    // ordered ops restore per op without copying 8 KiB per op)
    void ownedRegsArm() { ownedRegSnap_ = 1; }
    void ownedRegsRestore() {
        if (ownedRegSnap_ == 2) {
            setVertexShaderConstantF(0, ownedRegSnapVs_[0], kVsFloatConsts);  // (through the setters: the stage blocks are marked as the writes say)
            setPixelShaderConstantF(0, ownedRegSnapPs_[0], kPsFloatConsts);
        }
        ownedRegSnap_ = 0;
    }
    // ---- bone-history motion (native.bone_motion = off | on | compare, frame-ownership.md 14.13; d3d9/bone_motion.cpp). On: a skinned main-view solid draw whose VS window 2 is
    // a bone table image with history is drawn with the bone-motion forms (its vertex shader runs the translated program a second time on the previous frame's image: the previous
    // world position for the G-buffer's motion), so object motion / the RT scene need no vertex capture of it and its owned form stays owned. Compare: the captures as before and
    // the capture vertex shader also runs the bone path and compares (rvk.log `native: bone motion compare (5 s)`)
    void setBoneMotion(int mode);
    int boneMotion() const { return boneMode_; }
    // observers (enh/object_motion, enh/rt_scene): the draw being shown is a bone-motion draw (no capture needed for its motion)
    bool boneMotionDraw() const { return obsBone_; }
    // ... or will be once its forms are built (14.15: while they compile the draw keeps the plain form, its motion that frame is the camera's: no capture as fallback)
    bool boneMotionCovered() const { return obsBone_ || obsNoMotion_ || ((obsBonePending_ || obsRigidStill_) && !nocapOld()); }
    // 14.22: a rigid draw whose VS window 2 is an instance row image (F2f): no motion of its own (the record's stamp holds the instance's matrix, a moved instance remakes it)
    bool rigidRowWindow() const;
    // 14.26: the render instance the next draws belong to (StaticOps' instance-list loop; null = unknown): rigidListPredict keys its history by it
    void setInstanceIdentity(const void* id) { instanceId_ = id; }
    // F6-2 (frame-ownership.md 14.23): a skinned main-solid draw that writes no G-buffer motion (the highlighted focus NPC's additive overlay passes, a fading actor's blended
    bool noMotionNow(draws::Kind kind, const scene::Phase& phase) const;
    // 14.15 b (native.rigid_motion, restart; needs native.bone_motion=on): the static meshes' main solid draws whose VS window 2 is an object table record take the
    // bone-motion forms too (their second run reads the object table's previous-frame record), while object motion asks for moving rigid draws (upscale.object_motion=all)
    void setRigidMotion(bool on) { if (on != rigidMode_) ++variantEpoch_; rigidMode_ = on; }
    void setRigidMotionWanted(bool on) { if (on != rigidWanted_) ++variantEpoch_; rigidWanted_ = on; }
    bool rigidMotionOn() const { return rigidMode_ && rigidWanted_ && boneMode_ == 1; }
    // the history window base of a VS window 2 base (the bone table's or the object table's; null = none)
    rhi::GpuRef historyOf(rhi::GpuRef base) const;
    // compare mode: the bit the capture header carries for a main solid skinned draw whose window 2 is a bone image with history (0 = none / not compare mode)
    std::uint32_t boneCompareBit() const;
    struct BoneMotionStats {
        std::uint64_t predicted = 0, chosen = 0, chosen2 = 0, chosenPull = 0, pending = 0, noHistory = 0, records = 0, recordsNoPrev = 0, compareDraws = 0;
        std::uint64_t rigidPredicted = 0, rigidNoWindow = 0, rigidNoHistory = 0, rigidPending = 0, pushPredicted = 0, pushDrawn = 0;  // 14.15 b
        // 14.22: instanced-mesh draws without an object record (an uploaded VS window 2 / a row image): still (rows seen last frame / a row image), moving with the previous
        // rows as a per-frame block, no source (new / ambiguous list / a jump / the shader reads c36): camera motion only
        std::uint64_t listStill = 0, rowStill = 0, listMoving = 0, listNew = 0, listAmbiguous = 0, listRefused = 0;
        std::uint64_t instMoving = 0, instStill = 0;  // 14.26: of them, draws keyed by their instance's identity (the composition named it): moved / still
        std::uint64_t replayBoneRefused = 0;  // 14.22: packet-cache replays of draws that now take the bone-motion forms (cached without them): drawn by the normal path
        std::uint64_t compared = 0, compareMiss = 0, compareOver = 0;  // compare mode, from the GPU: vertices compared, without a previous capture, above 0.01 px
        float maxPx = 0.0f, maxWorld = 0.0f;  // (the largest seen; winPx / winWorld = of the current 5 s window)
        float winPx = 0.0f, winWorld = 0.0f, winMotionBone = 0.0f, winMotionCapture = 0.0f, winRerun = 0.0f;
    } boneStats;
    static bool boneVsOptions(const Shader& s, shader::TranslateOptions& o);  // the BoneVs provider (vertex shaders)
    static void boneCompareText(shader::TranslateOptions& o, const Shader& s);  // compare mode: added to the capture variant's text (enh/rt_scene captureOptions)
    static bool boneCompareOn();
    static void boneCompareForce(bool on);  // tools (rvk_shadertool --capture-bone): the compare text without a device
    static const char* captureTag() { return boneCompareOn() ? "cap6c8" : "cap6"; }  // the capture variant's cache tag (its text changes in compare mode)
    void setSceneVeg(bool on) { if (on != sceneVeg_) ++variantEpoch_; sceneVeg_ = on; }  // F7-2: the grass draws' scene ABI v2 form
    void setSceneBillboards(bool on) { if (on != sceneBb_) ++variantEpoch_; sceneBb_ = on; }  // F4-1: the tree billboards' opaque draws are batchable scene draws too
    void setSceneFaultInject(bool on) { sceneFaultInject_ = on; }  // tests: every scene packet looks like it has a null address (the guard must refuse it)
    bool sceneDrawsActive() const { return sceneDrawsOn_ && sceneAvail_; }
    SceneDrawStats& sceneStats() { return sceneStats_; }
    void sceneReport();  // the 5 s log line (frame start)
    OwnedStats& ownedStats() { return ownedStats_; }
    // Reversed infinite depth (debug.reverse_z, docs/research/reverse-z.md): a depth buffer cleared in a main view holds
    // near / view z (1 at the near plane, 0 at infinity and for the cleared sky) instead of the engine's D3D value. The engine's
    // vertex output is remapped in the shaders (DrawBlock::posFix.zw: z' = z (1 + zw.x) + zw.y w, = w - k z with k = 1 - near / far
    // of the main camera), the depth tests and the bias are flipped, the clear value mapped. Applies from the next clear.
    void setReverseZ(bool on);
    bool reverseZ() const { return reverseZ_; }
    // S-6 (native.depth_z, frame-ownership.md 14.10): `PsLess` = colour-less passes' draws whose pixel shader only writes colour get no pixel shader; `Main` = the main
    // view's z pre-pass also loses its R32F depth-map attachment (the DepthZ pass derives the map from the hardware depth at AfterDepthPrepass: armed only while that
    // pass can run). Applied at the next rendering scope
    enum class DepthZ : std::uint8_t { Off, PsLess, Main };
    void setDepthZ(DepthZ mode);
    DepthZ depthZ() const { return depthZ_; }
    // counters of the last 5 s window for the rvk.log line (render thread)
    struct DepthZStats {
        std::uint64_t scopesDropped = 0;     // main z scopes begun without the depth map attachment
        std::uint64_t scopesRefused = 0;     // ... where the conditions did not hold (the engine's colour attachment stays)
        std::uint64_t psDropped = 0;         // draws whose pixel shader was left out
        std::uint64_t psKept = 0;            // colour-less draws that kept it (texkill / depth write / alpha test)
        std::uint64_t psDroppedMain = 0, psKeptMain = 0;  // ... of them in the main view's z pass (the rest: the shadow views)
    };
    // the main view's z pass ran without its depth map attachment (set by the scope, consumed by the DepthZ pass at AfterDepthPrepass: true = the map must be derived)
    bool takeDepthMapDropped() {
        const bool d = depthMapDropped_;
        depthMapDropped_ = false;
        return d;
    }
    std::uint64_t depthZActs() const { return depthZActs_; }  // cumulative: scopes without the attachment + pixel shaders left out (native: stages line)
    DepthZStats takeDepthZStats() {
        DepthZStats s = depthZStats_;
        depthZStats_ = {};
        return s;
    }
    // k of the mapping above (the main camera's, as the FrameBlock holds it)
    float reverseDepthK() const;
    // the depth buffer bound now holds reversed depth
    bool boundDepthReversed() const;
    // posFix.zw of a draw into the bound depth buffer: identity (0, 0) unless it is reversed
    void depthRemap(float& zScale, float& zBias) const;
    double maxFps() const { return options_.maxFps; }
    // a further cap while a menu page is open (0 = none; the HAL sets it every present from engine/menu_state.h); a change
    // applies at the next present
    void setMenuCap(double fps);
    double menuCap() const { return menuCap_; }
    // loading screen: presents never wait (any cap's or vsync's wait would add to the load time: the engine loads on the
    // thread that presents, one present after each load step; docs/research/menu-fps.md 3): at most 30 frames a second
    // are shown, the others are rendered but not presented
    void setUnpaced(bool unpaced) { unpaced_ = unpaced; }
    bool unpaced() const { return unpaced_; }
    // Exclusive fullscreen (docs/research/window-mode.md 8). `monitor` != null: the swapchain is built with the exclusive
    // structures for that monitor (re-created now when this changes); `hold`: the window is in front, take the exclusive
    // mode (vkAcquireFullScreenExclusiveModeEXT, retried while it fails) - false releases it. Engine thread, before present.
    // Returns true when the swapchain was re-created.
    bool setFullScreenExclusive(void* monitor, bool hold);
    bool fullScreenExclusiveHeld() const { return game_->presenter().fullScreenExclusiveHeld(); }
    bool fullScreenExclusiveConfigured() const { return game_->presenter().fullScreenExclusiveConfigured(); }
    int presentMode() const { return game_->presenter().info().presentMode; }
    // the window's display mode changed (exclusive fullscreen): the pacer queries the monitor's refresh rate again
    void displayModeChanged();
    // the game's refresh rate as a frame cap with vsync (borderless fullscreen honouring a refresh choice below the
    // desktop's; 0 = none). docs/research/window-mode.md 8.4
    void setRefreshCap(double hz);
    double refreshCap() const { return refreshCap_; }
    double pacerRate() const { return pacer_.rate(); }
    // the overlay drawn at present (null = none). intoBackBuffer: the HAL calls drawOverlayIntoBackBuffer before its captures
    // instead (screenshots then contain the overlay; [debug] overlay_in_screenshots)
    void setOverlay(PresentOverlay* overlay, bool intoBackBuffer);
    void drawOverlayIntoBackBuffer();
    // colour format the overlay renders to (the swapchain's, or the back buffer's with intoBackBuffer)
    VkFormat overlayFormat() const;
    bool presentValid() const { return game_->presenter().valid(); }
    // what the presentation offers the enhancements (the swapchain's size / format, why AMD's swapchain was not used)
    VkExtent2D presentExtent() const { return game_->presenter().info().extent; }
    VkFormat presentFormat() const { return game_->presenter().info().format; }
    bool frameGenWanted() const { return game_->presenter().frameGenWanted(); }
    const std::string& frameGenReason() const { return game_->presenter().frameGenReason(); }
    // the swapchain's image counts (the overlay's UI backend buffers its resources for them)
    std::uint32_t presentMinImageCount() const { return game_->presenter().info().minImageCount; }
    std::uint32_t presentImageCount() const { return game_->presenter().info().imageCount; }
    // the last frame's numbers for the overlay's performance panel (cheap, always on; GPU only with [debug] perf)
    struct LiveFrame {
        float intervalMs = 0.0f;  // present to present
        float cpuMs = 0.0f;       // engine + HAL: end of one present() to the start of the next
        float gpuMs = -1.0f;      // GPU time of the latest measured submission (-1: no GPU timer)
        float baseFps = 0.0f;     // frame generation: rendered frames per second (last second); 0 = no frame generation
        float outputFps = 0.0f;   // ... and frames shown per second (generated ones included)
    };
    const LiveFrame& liveFrame() const { return live_; }
    Surface* backBuffer() { return backBufferSurface_; }
    // Manual frame generation (enh/framegen.h): set from the BeforePresent pass with the interpolated frame (display size, same
    // format family as the back buffer, usable as a transfer source); present() shows it first and the real frame half a base
    // interval later, then forgets it. Cleared when the frame is not presented.
    void setGeneratedFrame(rhi::Texture* image) { generated_ = image; }
    Surface* autoDepthStencil() { return depthSurface_; }
    const FrameStats& stats() const { return stats_; }
    // [debug] perf: the numbers since the last call (zeroed afterwards)
    PerfStats takePerf();
    bool perfEnabled() const { return options_.perf; }

    // ---- packet retention (docs/research/engine-frame/retained-signals.md 8): a finished draw packet kept by the caller and replayed
    // without prepareDraw. A draw is captured while it is drawn normally (beginCapture ... endCapture) and replayed later by replayDraw,
    // which validates that everything the packet was derived from still holds (shaders, buffers' GPU bindings, textures, viewport and
    // targets, phase) and recomputes the per-frame parts (constant windows, DrawBlock's c5 / depth remap, draw id). The caller guarantees
    // the rest by comparing the draw's description (shaders, declaration, streams, ranges, states, samplers) with the captured one.
    struct CachedDraw {
        bool valid = false;
        DrawPacket packet;  // (vi / dyn point into a per-frame arena: the values are kept below and interned again at replay)
        VertexInputValues vi;
        DynamicStateValues dyn;
        shader::abi::DrawBlock block{};
        ReplaceQuery query;  // what the observers / opaque-observing replacers were shown (tag, textures and mainView are refreshed at replay)
        draws::Kind kind = draws::Kind::Unknown;
        scene::Phase phase;
        const Shader* vs = nullptr;
        const Shader* ps = nullptr;
        GamePipeline scenePipeline = GamePipeline::None;  // F2b-1: the scene form's pipeline when the draw was eligible and it existed (None: drawn normally)
        GamePipeline scenePipeline2 = GamePipeline::None;  // F2c-4-0: the scene ABI v2 form's pipeline (only while setSceneViews is on; None otherwise)
        // S-4 (native.vertex_pull): the pulled form's pipeline (scenePipeline2's pixel shader + the SceneVsPull vertex shader, the ref stream as the only vertex input)
        // and the draw's VertexFetch block (valid = kVertexFetchValid when every input can be pulled); None / invalid = the draw keeps its vertex input
        GamePipeline scenePipelinePull = GamePipeline::None;
        bool ordered = false;     // F9 / F14 (native.owned_ordered): not batchable, its scene forms were made for the ordered owned path (drawn at its op's place)
        bool boneMotion = false;  // 14.13 (native.bone_motion): the scene forms above are the bone-motion forms (the record's prevVs = the bone window's history image)
        // 14.22: what the observers of the composing draw were told (replayDraw repeats it): 0 nothing, 1 bone forms predicted but not taken (declined: no form
        // for the draw's state), 2 forms pending, 3 rigid without motion of its own
        std::uint8_t boneObs = 0;
        shader::abi::VertexFetch pull{};
        rhi::ShaderModule vsModule = rhi::ShaderModule::None, fsModule = rhi::ShaderModule::None;  // vsModule: the one drawn with (the engine's, or the billboards' fade variant); fsModule: the engine's
        // the enhancement variant decision of prepareDraw and what it was derived from (retained-signals.md 13): replayBuild re-derives the cheap inputs
        // (settings, the ambient registers, the variant's build state) and refuses the replay when the decision would differ
        struct Variant {
            bool gate = false;       // (giDraws_.on || the material pack) when the packet was made
            // camera jitter (upscaling): the sampler indexes of the plain and the LOD-biased state of each sampled stage, the bias they were made for
            float biasFor = 0.0f;
            std::uint32_t smpPlain[16] = {}, smpBias[16] = {};
            bool gbufGate = false;   // the G-buffer was on
            bool gbufStatic = false; // gbufferEligible without its ambient test
            bool gbufWanted = false; // ... the G-buffer's material pass took the draw (eligible, lit, in the G-buffer's rendering scope)
            // F-VAR: a decision of this draw's variant reads the ambient registers (the GI / pack gate, or the G-buffer's lit test): owned draws answer it from the record's lighting
            bool needsAmbient() const { return (gate && staticOk) || (gbufGate && gbufStatic); }
            bool drawn = false;      // the draw uses the pack word (giDraw || the G-buffer write)
            bool giOn = false, replaceLightmaps = false, pack = false;
            bool staticOk = false;   // giEligible without its ambient-register test (phase, water, blend, outputs: fixed by the draw's description)
            bool wanted = false;     // the draw asked for the GI / pack variant (gate && staticOk && the ambient registers non-zero)
            bool giDraw = false;     // ... and drew with it
            rhi::ShaderModule fsAsked = rhi::ShaderModule::None;  // the variant module the draw was offered (None = not built yet)
            std::uint32_t minEnd = 0;  // the pixel window's minimum end (uploadConstants)
            bool packStamped = false;           // packStamp / packTex / packTexSlot are valid (the pack has a change stamp)
            std::uint32_t packStamp = 0;
            const Texture* packTex = nullptr;   // the pack word's map (null: the word has none)
            std::uint32_t packTexSlot = 0;      // its bindless slot when the word was made
            bool sceneActive = false;   // F2b-1: scene draws were on for this draw (a change remakes the packet)
            bool scenePending = false;  // ... and its scene variants / pipeline were not ready: retried until they are
            rhi::ShaderModule vsUsed = rhi::ShaderModule::None;  // the vertex module drawn with (the billboards' fade variant while lod.trees != 1)
            rhi::ShaderModule uberAsked = rhi::ShaderModule::None;  // S-1a: the plain HLSL ubershader module the draw was offered (debug.ubershader; None = not built / not covered)
            std::uint8_t uberPs = 0;  // S-1a: which pixel shader the scene form took: see UberWhy
            std::uint8_t uberVs = 0;  // S-1b: which vertex shader: 0 translated, 1 ubershader, 2 failed (Device::uberVsFormOf)
        } v;
        std::uint32_t streamMask = 0;
        bool clusterUsing = false;      // ... or it drew with the entry's clustered copy (the draw is remade when the entry is gone)
        std::uint64_t clusterWait = 0;  // F5-1: the draw was a cluster candidate whose build was pending at capture (the entry's id): remade once that entry has settled
        Buffer::Binding stream[4]{};
        Buffer::Binding ib{};
        std::uint32_t texMask = 0;
        Texture* tex[kSamplerCount]{};
        std::uint32_t texSlot[kSamplerCount]{};
        struct Env {
            Viewport vp;
            std::int32_t scissor[4];
            std::uint32_t colorCount;
            VkFormat colorFormats[kRenderTargets];
            VkFormat depthFormat, stencilFormat;
            std::uint32_t depthReversed;
        } env{};
    };
    // the buffers a draw reads (the caller's description of it)
    struct ReplayInputs {
        Buffer* streams[4]{};
        std::uint32_t strides[4]{};
        Buffer* indices = nullptr;
    };
    void beginCapture(CachedDraw* into) { capture_ = into; if (into != nullptr) into->valid = false; }
    bool endCapture() { const bool ok = capture_ != nullptr && capture_->valid; capture_ = nullptr; return ok; }
    // true = drawn from the retained packet; false = not valid / not allowed: nothing was drawn, draw normally
    bool replayDraw(const CachedDraw& c, const ReplayInputs& in);
    // verify mode: would replaying `cached` have produced `fresh` (a draw just made normally)? false = a difference (`why`)
    // `valid` false: the cached one would not have been used (not a difference)
    bool replayMatches(const CachedDraw& cached, const CachedDraw& fresh, const ReplayInputs& in, bool& valid, std::string& why);
    // the consumers of prepareDraw's results are idle for this draw (observers, replacers, variants, tint, taps, logging)
    bool replayAllowed(const CachedDraw& c) const;
    // ---- F2c-2 op records (docs/research/frame-ownership.md 7.14; scene_draws.cpp, native/static_ops_records.cpp): the scene packet of a draw as a plain value
    // (what an indirect scene draw is made of, with the ring addresses left out), taken from the composed draw (setSceneProbe: toScenePacket fills it) or
    // derived from a retained CachedDraw + the per-draw inputs of an op record (deriveScene). Compare only: nothing derived is submitted.
    struct SceneView {
        bool valid = false;  // the probe: toScenePacket ran for the draw; a derivation: it was produced
        bool scene = false;  // the draw is (to be) a scene draw
        bool capture = false;  // (probe) the draw went with a capture variant (an observer asked for the vertex shader's outputs): it is no scene draw by design
        GamePipeline pipeline = GamePipeline::None;
        std::uint64_t viHash = 0, dynHash = 0, indexBuffer = 0, indexOffset = 0;
        const rhi::VertexInputValues* vi = nullptr;  // the vertex input (interned: valid within the frame; for the DIFFERING detail only)
        std::uint32_t first = 0, count = 0, flags = 0;
        std::int32_t vertexOffset = 0;
        bool index32 = false;
        shader::abi::DrawBlock block{};
    };
    void setSceneProbe(SceneView* p) { probe_ = p; }
    std::uint64_t variantEpoch() const { return variantEpoch_; }
    // the inputs of a record's draw that the composition would otherwise leave in the device
    struct DeriveIn {
        Buffer* streams[4]{};
        std::uint32_t strides[4]{};
        Buffer* indices = nullptr;
        Texture* tex[kSamplerCount]{};            // the composed textures of the pixel stages (null = none)
        std::uint32_t ss[kSamplerCount][7]{};           // their sampler states (the seven the composition owns)
        std::uint32_t alphaFunc = 8;                    // D3DCMP of the draw's alpha test (8 = always), derived from the pass's states
        float alphaRef = 0.0f;
        float psC5[4]{};                                // pixel window 1 (c5) as the draw's pass sets it (.w only when it sets the mask)
        bool ambientLit = false;                        // the draw's ambient registers (c15..c21 of its lighting) are not all zero, composed from the record's illumination
    };
    // 0 = derived into `out`; else why not (1 stale module, 2 phase, 3 consumers, 4 variant, 5 env, 6 stream / index binding, 7 texture, 8 jitter flags, 9 scene form)
    int deriveScene(const CachedDraw& c, const DeriveIn& in, SceneView& out);
    // ---- F2c-3 (native.owned_static, frame-ownership.md 7.16; scene_draws.cpp): an op record's draw submitted as a scene draw WITHOUT a composition: the block is
    // derived (deriveScene), every register window comes from the caller's tables (`windows`), the register file is not read. The caller set the draw tag,
    // the vertex / pixel shader refs of the record, setSceneIssue(true) and the windows' tables; 0 = queued, else the refusal reason (nothing queued):
    // 1..9 deriveScene's codes, 10 scene draws not active, 11 shaders not bound / record invalid, 12 no rendering scope, 13 an observer wants a capture,
    // 14 a window the shader reads is not served, 15 the scene packet's guard (null address); scene ABI v2 (OwnedWindows::viewSlot set): 16 no v2 pipeline / no view table,
    // 17 the view slot is out of range, 18 the view windows are not the slot's, 19 the view block's posFix / c5 are not the draw's derived ones
    struct OwnedReplayStats {
        std::uint64_t attempts = 0, drawn = 0, registersLoaded = 0, drawnV2 = 0;
        std::uint64_t why[24] = {};
    } ownedReplayStats;
    int replayOwned(const CachedDraw& c, const DeriveIn& in, const OwnedWindows& windows);
    // ---- F2c-4a (native.owned_persistent, frame-ownership.md 7.21; owned_persist.cpp): owned draws as PERSISTENT records. replayOwned derives, builds and queues every draw every frame;
    // here the derivation result (a DrawRecord + a DrawBlock in gpuscene::drawrecords()) stays, is rewritten only when an input of the derivation moved, and a frame submits one
    // indirect draw per bucket (v2 scene pipeline, vertex input, dynamic state, index binding, view slot) over a CPU-written ref list + argument block.
    //   ownedEnvEpoch: a number that changes when anything the derivation reads from the device (phase, enhancement state, viewport / targets, jitter, sampler states, view table ...)
    //   differs from this pass kind's last call; ok = false: the pass cannot take persistent draws (no scene issue / v2 / table / ...).
    //   ownedDrawSig: the per-draw inputs (textures' bindless slot / state, buffer bindings, ambient test, replay allowance, pack word, the four window bases) as one hash.
    //   ownedDerive: replayOwned up to the block: the refusal code (replayOwned's codes) or 0 with `out` made for `slot` (DrawBlock posFix / c5.xyz left zero: the v2 shaders read the view's).
    //   ownedObserve: the observers' / opaque-observing replacers' turn for the draw (false = one wants a capture: the op is composed instead).
    //   ownedReserve / ownedFlush: the frame's ref list + argument ring blocks for up to `maxDraws` items, then the packets of the items bucket by bucket.
    struct OwnedPersist {
        shader::abi::DrawRecord rec{};
        shader::abi::DrawBlock blk{};
        shader::abi::VertexFetch fetch{};  // S-4: the slot's vertex fetch block (zero while native.vertex_pull is off or the draw cannot be pulled)
        std::uint64_t bucketKey = 0;
    };
    std::uint64_t ownedEnvHash(bool solid, std::uint32_t viewSlot, bool& ok) const;
    std::uint64_t ownedEnvEpoch(bool solid, std::uint32_t viewSlot, bool& ok);
    bool ownedEnvSame(bool solid, std::uint32_t viewSlot, std::uint64_t epoch) const;
    // F9 / F14: the env epoch of an ordered pass segment (`index` = kOwnedEnvOrdered + 0 solid list, 1 refracted, 2 alpha). The ordered ops sit between composed ops that leave
    // other device state behind (sampler rows the derivation does not own): an index keeps the last few hashes with their epochs, so a recurring state gets its old epoch back
    static constexpr std::size_t kOwnedEnvOrdered = 8;
    std::uint64_t ownedEnvEpochAt(std::size_t index, bool solid, std::uint32_t viewSlot, bool& ok);
    bool ownedEnvSameAt(std::size_t index, bool solid, std::uint32_t viewSlot, std::uint64_t epoch) const;  // the check frames: nothing the derivation reads moved since `epoch` (no state is touched)
    std::uint64_t ownedDrawSig(const CachedDraw& c, const void* const* streams, const std::uint32_t* strides, const void* indices, const void* const* tex, bool ambientLit, const OwnedWindows& ow, bool& allowed);
    // the DrawRecord + DrawBlock of a derived draw (pure: no register file, no device constants): 14 a window the shaders read is not served, 15 a null address
    int ownedBuild(const CachedDraw& c, const SceneView& sv, const OwnedWindows& ow, rhi::GpuRef blockAddress, rhi::GpuRef zeroBlock, OwnedPersist& out);
    // deriveScene + the view checks + ownedBuild; `checkView` false = the view table's slot is not this frame's (the check frames compare the content only)
    int ownedDerive(const CachedDraw& c, const DeriveIn& in, const OwnedWindows& ow, rhi::GpuRef blockAddress, rhi::GpuRef zeroBlock, bool checkView, OwnedPersist& out);
    // F2c-4b: what the persistent path tells the observers about the draw it has just made / found valid (`id` = its persistent slot, `changed` = its slot was rewritten this frame)
    struct OwnedObsInfo {
        std::uint32_t id = 0;
        std::uint64_t sig = 0, epoch = 0;
        bool changed = false;
    };
    bool ownedObserve(const CachedDraw& c, const OwnedWindows& ow, const OwnedObsInfo& info);
    // one owned draw as the bulk observers see it (immutable for the call): the retained draw, its windows (the record's own persistent ones), the tag it is drawn under
    struct OwnedRecordView {
        const CachedDraw* cd = nullptr;
        const OwnedWindows* ow = nullptr;
        draws::Tag tag;
        std::uint32_t id = 0;
        std::uint32_t wanted = 0;  // bit i: the observer at observers_[i] wants it (bulkOwned and observes)
        std::uint64_t sig = 0, epoch = 0;
        bool entered = false;      // not in the previous pass of this kind
        bool changed = false;
    };
    // a pass of persistent owned draws begins (z pre-pass / solid): the bulk list and the entered / left bookkeeping of the pass kind; ownedBulkFlush delivers the draws added so far
    // (`last` = the end of the pass: the `left` list goes with it). Called before an op is composed late (its record may be dropped) and at the pass's end
    void ownedBulkBegin(bool solid, bool bulk = true);  // bulk = false (a shadow view's pass, F3-1): no bulk list, the per-draw observers' loop
    void ownedBulkFlush(bool last);
    bool ownedBulkOn() const { return ownedBulkOn_; }
    struct OwnedBulkStats {
        std::uint64_t flushes = 0, records = 0, entered = 0, left = 0, lateCapture = 0, perDrawCovered = 0;
    } ownedBulkStats;
    // the device state of one owned draw while an observer looks at it: the tag, the draw's windows, the draw's query (OwnedFrame consumers call observe from inside it)
    class OwnedDrawScope {
    public:
        OwnedDrawScope(Device& d, const OwnedRecordView& r);
        ~OwnedDrawScope();
        OwnedDrawScope(const OwnedDrawScope&) = delete;
        OwnedDrawScope& operator=(const OwnedDrawScope&) = delete;
        const ReplaceQuery& query() const { return q_; }
        // the windows' register images into the register file (for observers that read material constants); the pass restores the file
        void loadRegisters();
        // an observer asked for a capture of this draw (too late for a draw that is queued: counted)
        bool captureRequested() const { return !d_.captureRequest_.null(); }
    private:
        Device& d_;
        const OwnedWindows* savedOwned_;
        draws::Tag savedTag_;
        const OwnedWindows* ow_;
        bool loaded_ = false;
        ReplaceQuery q_;
    };
    std::uint64_t ownedBucketKey(const CachedDraw& c, std::uint32_t viewSlot) const;
    std::uint64_t ownedPullKey(const CachedDraw& c, std::uint32_t viewSlot) const;  // S-4: the bucket key of a pulled item (no vertex input)
    struct OwnedItem {
        const CachedDraw* cd = nullptr;
        std::uint64_t bucket = 0;
        std::uint32_t slot = 0;
    };
    struct OwnedArena {
        rhi::RingAlloc list, args;
        std::uint32_t capacity = 0;
        bool valid() const { return capacity != 0; }
    };
    // the rendering scope the owned draws are drawn in, begun before anything is derived (the device's targets / formats of the scope are part of the derivation's environment)
    bool ownedBeginScope();
    bool ownedReserve(std::uint32_t maxDraws, OwnedArena& arena);
    // true = every item drawn (queued as indirect packets); false = nothing queued (no rendering scope / a guard), the caller composes the items' ops
    // `shadow` / `slotCount`: the records table's CPU twin: an item whose slot is out of range or whose record was never written (no block address) is refused (GPU safety)
    gpuscene::OwnedCompact::CullItem cullCheckItem(const shader::abi::DrawRecord& rec, std::uint32_t viewSlot, std::uint32_t pos, std::uint32_t bucket);
    // `ordered` (F9 / F14): the items keep the engine's order exactly: a bucket is a run of consecutive items with one key (no merging of non-adjacent ones) and every packet is
    // recorded at once after the pending batch (not batchable: never regrouped by pipeline)
    bool ownedFlush(const std::vector<OwnedItem>& items, OwnedArena& arena, std::uint32_t viewSlot, rhi::GpuRef recordsRef, const shader::abi::DrawRecord* shadow, std::uint32_t slotCount,
                    bool ordered = false);
    struct OwnedPersistStats {
        std::uint64_t derived = 0, refused = 0, observed = 0, buckets = 0, packets = 0, items = 0, bucketKeyClash = 0;
    } ownedPersistStats;
    // S-4 vertex pulling (native.vertex_pull, frame-ownership.md 14.12; Full, the persistent owned draws): an owned item whose draw has a pulled pipeline and a written
    // VertexFetch slot is bucketed without its vertex input (key = pulled pipeline x dynamic state x index binding x view slot) and drawn with vertexOffset 0
    // (the variant modules are shared by their text only while it is on: off = the module set of before, exactly)
    void setVertexPull(bool on) {
        if (on != vertexPull_) ++variantEpoch_;
        vertexPull_ = on;
        shareModules_.store(on, std::memory_order_relaxed);
    }
    bool vertexPull() const { return vertexPull_; }
    struct VertexPullStats {
        // per view kind (0 main z, 1 main solid, 2 other, 3 cascades): flushes, items, items pulled, buckets (= indirect commands), buckets of pulled items
        std::uint64_t flushes[4] = {}, items[4] = {}, pulled[4] = {}, buckets[4] = {}, pulledBuckets[4] = {};
        std::uint64_t pipelines[4] = {}, indexBindings[4] = {};  // distinct per flush, summed
        // pulled items: indices drawn (= vertex shader invocations before the post-transform reuse, before the cluster cull) and the bytes they fetch (all inputs / POSITION0)
        std::uint64_t indices[4] = {}, vtxBytes[4] = {}, posBytes[4] = {};
        // why an owned item was not pulled: no pulled pipeline (the draw's form / pending), no fetch block in its slot
        std::uint64_t noPipeline = 0, noFetch = 0;
        // prepareDraw: draws of the scene v2 path whose inputs could / could not be pulled (format, per-instance / user-pointer stream, no arena address, alignment)
        std::uint64_t layoutOk = 0, layoutRefused[6] = {};
        std::uint64_t pipelinesPending = 0;
    };
    VertexPullStats& vertexPullStats() { return vertexPullStats_; }
    std::uint64_t vertexPullActs() const { return vertexPullActs_; }  // cumulative pulled items (native: stages line)
    // F2c-4-0 item 6 (RVK_F2C4_MEASURE): the draw's bucket = what a GPU compaction pass could merge into one indirect call: the v2 scene pipeline, the vertex input and
    // dynamic state (by content), the index binding. Not the frame block (one per frame)
    std::uint64_t f2c4BucketKey(const CachedDraw& c) const;
    // every registered observer reads no more than an owned draw provides (DrawObserver::Registers)
    int variantRefusal(int site, const CachedDraw& c);  // (deriveScene's code 4, logged)
    // F15-1 (frame-ownership.md 14.20): the last code-4 refusal's site and whether the record's scene forms were still being built when it was composed (the composed-late census)
    // F15-1: a texture an owned draw samples that is not ready for sampling (a render target still in its attachment layout, a pending upload): ownedTextureNeedsPrep
    // asks (no side effects), ownedPrepTexture makes it ready exactly as prepareTextures does for a composed draw at the same place (the caller flushed its queued items first)
    bool ownedTextureNeedsPrep(const Texture* t) const;
    void ownedPrepTexture(Texture* t);
    // F15-1: the variant build queue now: jobs waiting + building, and the run's built / failed totals (the composed-late census)
    void variantQueue(std::size_t& waiting, std::uint64_t& built, std::uint64_t& failed);
    int lastVariantSite() const { return lastVariantSite_; }
    bool lastVariantPending() const { return lastVariantPending_; }
    int lastVariantSite_ = 0;
    bool lastVariantPending_ = false;
    // F15-1: request every scene form + pipeline of a draw at its first sight (default; RVK_F15_BASELINE=1 = the old one-after-the-other chain, A/B)
    std::uint64_t ownedTexturePreps_ = 0;  // F15-1: textures an owned draw made ready (instead of composing its op late)
    bool earlyForms_ = GetEnvironmentVariableA("RVK_F15_BASELINE", nullptr, 0) == 0;
    bool ownedObserversOk(std::string* why = nullptr) const;  // (`why`: the first observer that gates, named)
    bool observersWantLighting() const;  // F-VAR: an observer reads the lighting registers (Registers::MaterialLighting)
    bool ownedReplayActive() const { return ownedReplay_; }
    // F4-3 (owned_trees.cpp, docs/research/frame-ownership.md 7.25): the SpeedTree draws of the main view as scene draws. `setSceneTrees`: a SpeedTree draw issued while a scene issue is
    // on gets its scene ABI v2 form (pipeline only; the composed draw itself is NOT turned into a scene packet: its instance count would be lost).
    // ownedTreeObserversOk: no observer wants the main view's tree draws. allocOwnedRing: a ring block of this frame (the caller's rows). ownedTreeFlush: the frame's per-instance
    // records / ref list / arguments written for `items` (consecutive items with one bucket key = one indirect call; `instances` list positions each, in item order: the caller
    // laid its rows out the same way) and queued; false = nothing queued. treeEnvEpoch = ownedEnvEpoch with the tree passes' own arrays.
    void setSceneTrees(bool on) {
        if (on != sceneTrees_) ++variantEpoch_;
        sceneTrees_ = on;
    }
    bool ownedTreeObserversOk(std::string* why = nullptr) const;  // (`why`: the first observer that gates, named)
    // at least one observer wants this template's draws (kind / phase), so ownedTreeObserve is to be called for its runs
    bool ownedTreeObserved(const CachedDraw& c) const;
    // F-VAR2: show the observers one owned vegetation batch (`ow` = the template's windows with the batch's lighting entry); returns the batches shown (0 or 1)
    std::uint32_t ownedVegObserve(const CachedDraw& c, const OwnedWindows& ow, std::uint32_t instances);
    // the observers' turn for one run of owned tree instances (`rows` = 16 floats per instance = VS c32 + 4 i .. c35 + 4 i; chunks of the engine's batch size): returns the number of
    // instances shown; the caller has set the draw tag and saves / restores the register file (this loads the template's window images and the rows into it)
    std::uint32_t ownedTreeObserve(const CachedDraw& c, const OwnedWindows& ow, Buffer* ramp, const float* rows, std::uint32_t count);
    std::uint64_t ownedTreeObservedFrame() const { return treeObservedFrame_; }
    std::uint64_t treeObservedFrame_ = 0;
    std::uint64_t treeObserveCaptures = 0;  // capture requests an observer made for an owned tree draw (refused: 0 expected)
    rhi::RingAlloc allocOwnedRing(std::uint64_t bytes, std::uint64_t align) { return game_->allocRing(bytes, align); }
    struct TreeItem {
        const CachedDraw* cd = nullptr;
        std::uint64_t bucket = 0;
        std::uint32_t instances = 0;  // the item's positions (consecutive): ONE indirect command, whatever records its instances use
    };
    // recs = the DrawRecords (one per template x lighting entry), posRec[p] = the record of list position p (positions = posRec.size() = the items' instance sum)
    // F7-2 (frame-ownership.md 7.42): `inst` = the vegetation's instance bytes of the pass (positions x stride, in list order): copied into a ring block of this frame, which stands in
    // for each item's stream 1 (the binding whose buffer is the template's stream-1 buffer: at the block's start, instance-rate; an item's firstInstance = its first list position)
    struct VegInstances {
        const std::uint8_t* data = nullptr;
        std::uint32_t stride = 0;
    };
    bool ownedTreeFlush(const std::vector<TreeItem>& items, const std::vector<shader::abi::DrawRecord>& recs, const std::vector<std::uint32_t>& posRec, std::uint32_t viewSlot, std::uint32_t positions,
                        const VegInstances* inst = nullptr);
    // no observer wants the main view's vegetation draws (the owned vegetation pass shows them nothing)
    bool ownedVegObserversOk(std::string* why = nullptr) const;  // (`why`: the first observer that gates, named)
    std::uint64_t treeEnvEpoch(bool solid, std::uint32_t viewSlot, bool& ok);
    bool treeEnvSame(bool solid, std::uint32_t viewSlot, std::uint64_t epoch) const;
    struct TreeFlushStats {
        std::uint64_t flushes = 0, items = 0, instances = 0, packets = 0, refused = 0, commands = 0, records = 0;
        // S-5 measurement (frame-ownership.md 14.14): per kind (0 trees, 1 grass) the flushes, the indirect calls made, and the calls a pulled form would need at best
        // (distinct vertex shader x pixel shader x dynamic state x index binding per flush: the vertex input out of the key, items sorted)
        std::uint64_t kindFlushes[2] = {}, kindCalls[2] = {}, kindPullKeys[2] = {}, kindItems[2] = {};
    } treeFlushStats;
    // F4-1 (owned_submit.cpp, docs/research/frame-ownership.md 7.17): a record's draw submitted with nothing composed: derived block + the caller's persistent windows, as a scene draw.
    // `dryRun` = every check, nothing submitted (the caller checks a whole op before it submits any of its draws). Anything but Submitted: nothing was drawn.
    enum class OwnedWhy : std::uint8_t { Submitted, NoScene, Split, Consumers, Window, Derive, Rendering, Packet, Count };
    OwnedWhy submitOwned(const CachedDraw& c, const DeriveIn& in, const OwnedWindows& win, bool dryRun, int* deriveWhy = nullptr);
    std::uint64_t ownedSubmitted() const { return ownedSubmitted_; }
    bool ownedTextureLive(const void* texture);  // one Texture a record's poll reads is still alive
    bool ownedBindingsLive(const DeriveIn& in);  // every Buffer / Texture a record's draw reads is still alive (a record outlives them; the composition would have re-resolved them)
    // counters of the owned object rows handed to observers (ReplaceQuery::objectRows) against the register file
    struct ObjectRowStats {
        std::uint64_t given = 0, differing = 0;
    } objectRowStats;
    // the last replayDraw was refused because the packet is out of date (shaders, variants, bindings, textures): remake it from a normal draw
    bool replayWasStale() const { return replayStale_; }
    // false = draws that took an enhancement variant (GI / material pack word / billboard fade module) are not retained (the behaviour before the variant packets; A/B)
    void setRetainVariants(bool on) { retainVariants_ = on; ++variantEpoch_; }
    struct ReplayStats {
        std::uint64_t replayed = 0, refused = 0, captured = 0, notCaptured = 0;
        // why a replay was refused: shaders / modules, phase, consumers, env, jitter, stream binding, index binding, texture, observer capture, no scope, variant decision, sky cube
        std::uint64_t why[12] = {};
        std::uint64_t ncWhy[12] = {};  // capture refused by prepareDraw: replaced, tint, captureDraw, variant not ready, gbuf write, custom, not indexed, replacement, uber, module, request
    } replayStats;
    // resources report what they copy to the GPU (render thread)
    void countUpload(bool texture, std::uint64_t bytes) {
        (texture ? perf_.textureUploadBytes : perf_.bufferUploadBytes) += bytes;
    }
    void resetTimings() {
        stats_.acquireMs = 0.0;
        stats_.presentMs = 0.0;
        stats_.presentWaitMs = 0.0;
        stats_.pacerMs = 0.0;
    }

    // ---- frame semantics (d3d9/scene.h, docs/research/frame-phases.md; render thread) ----
    // the frame / view / pass state, driven by the engine phase hooks (src/engine/scene_hooks.*)
    scene::PhaseTracker& phases() { return phases_; }
    const scene::PhaseTracker& phases() const { return phases_; }
    // the main camera of the frame being recorded (valid once its main view began) and of the last presented frame
    const scene::CameraBlock& camera() const { return camera_; }
    const scene::CameraBlock& lastCamera() const { return lastCamera_; }
    // the first main view of a frame sets the camera; later ones are ignored (returns whether it was taken)
    bool setMainCamera(const scene::CameraInput& in);
    // give an engine image a role (logged when a texture gets a new role); the latest texture per role is queryable
    // `label` (static string) / `labelIndex` refine the debug name of the image ("cascade shadow map" #k); default the role's name
    void tagImage(Texture* texture, scene::ImageRole role, const char* label = nullptr, std::uint32_t labelIndex = ~0u);
    Texture* imageWithRole(scene::ImageRole role) const { return roles_[static_cast<std::size_t>(role)]; }
    void forgetImage(Texture* texture);  // a tagged texture dies
    // the sky environment cube (eCSkydome_PS::UpdateSkydomeEnvCubemap, docs/research/ocean.md 9.8): faces bound so far (the
    // HAL counts each face SetRenderTarget in the SkyCube view), and the request to refresh the whole cube every frame
    // instead of the engine's one face per frame every ~10 s (an enhancement asks for it: the enhanced water's reflection)
    void countSkyCubeFace() { ++skyCubeFaces_; }
    std::uint64_t skyCubeFaces() const { return skyCubeFaces_; }
    void setSkyCubeEveryFrame(bool on) { skyCubeEveryFrame_ = on; }
    bool skyCubeEveryFrame() const { return skyCubeEveryFrame_; }
    double secondsSinceCreate() const;
    // draw tags (d3d9/draws.h, docs/research/draw-tags.md): the engine hooks keep the current tag; every draw copies it into
    // the frame's records (when recording) and counts its kind in the frame's histogram
    draws::Tag& drawTag() { return tag_; }
    const draws::FrameDraws& drawRecords() const { return draws_; }
    void setDrawRecords(bool on) { draws_.setRecording(on); }
    // debug view: tint colour draws by kind / pass (draws::tintColor; the pixel shaders' tint variants are built on demand)
    void setTintMode(draws::TintMode mode) { tint_ = mode; ++variantEpoch_; }
    draws::TintMode tintMode() const { return tint_; }
    // detail distances (engine/lod.h lod.trees): the SpeedTree billboards' fade-in distances times `scale` (1 = the engine's
    // vertex shader unchanged; else its fade variant, FrameBlock params[kFrameParamLod].x)
    void setBillboardFadeScale(float scale);

    // ---- enhancements: frame block, injection points, transient images (docs/research/injection.md; render thread) ----
    // the FrameBlock of the frame being recorded (shader/abi.h): uploaded into the frame's ring on first use and again after
    // the camera, an image role or a feature parameter changed; every draw's push constants carry it
    rhi::GpuRef frameBlockAddress();
    // FrameBlock params[index] (abi::kFrameParam*); applies from the next upload
    void setFrameParam(std::uint32_t index, const std::array<float, 4>& value);
    // passes at injection points (d3d9/inject.h): add returns the pass id; add / remove not from inside a pass
    std::uint32_t addPass(inject::Point point, std::string name, inject::PassFn fn, const void* owner = nullptr, int order = 0);
    void removePass(std::uint32_t id);
    void removePasses(const void* owner);
    const inject::Registry& passes() const { return passes_; }
    // the scene hooks: a main view begins (the once-per-view guard of its points); an injection point is reached
    void beginMainView();
    void inject(inject::Point point);
    // S-7: one ad-hoc pass recorded NOW (mid-frame, from an engine hook such as the post chain's units): the open rendering scope is left, `fn` records
    // its commands with nothing bound, the device's recording state is restored after it (the body of `inject` for one pass; no firing guard). False
    // when the device is not created or a pass is already being recorded. Render thread.
    bool runPassNow(const char* name, const std::function<void(PassContext&)>& fn);
    // Camera jitter (TAA / upscaling, enh/upscale): `source` is asked once per frame at its first main view for that frame's
    // sub-pixel jitter in pixels of the scene target (null = no jitter). The offset reaches the main view's geometry draws
    // through DrawBlock::posFix (geometryJitter) and shaders through FrameBlock::jitter; the motion vectors stay jitter-free
    // (they come from the unjittered camera matrices).
    using JitterSource = std::function<std::array<float, 2>(std::uint64_t frameIndex)>;
    void setJitterSource(JitterSource source);
    // NDC offset to add to posFix for the draw being recorded now: non-zero only for geometry of the main view's scene passes
    // (depth prepass, solid, refracted, alpha) into a target of the scene's size
    void geometryJitter(float& ndcX, float& ndcY) const;
    // Debug regions (debug.regions): VK_EXT_debug_utils command buffer labels in the frame's command buffer, so graphics
    // captures (RenderDoc, PIX, Radeon GPU Profiler) show "Frame N > Post > Main #1 > Solid > ..." and every injected pass by
    // name instead of one flat list of draws. The view / pass regions follow the phase tracker (reconciled when a draw is
    // recorded and before an injection); regions never span submissions: they are closed before a submit and re-opened in
    // the next command buffer.
    // Zero allocation: the open regions live in inline buffers (names are copied into them: a literal or a short-lived
    // c_str() both do), RegionScope below is the RAII way: `RegionScope r(device, "Name");` or `RegionScope r(device, "Shadow", 2)`
    // for "Shadow #2". Not for threads other than the render thread.
    void setRegionsEnabled(bool on);
    // debug.crash_markers (vk/crash_markers.h): AMD buffer markers at every region's start / end in the primary command buffer; the
    // unfinished regions are logged with the device fault report after a loss
    void setCrashMarkers(bool on);
    bool regionsEnabled() const { return regionsOn_; }
    static constexpr std::uint32_t kNoRegionIndex = ~0u;
    void regionBegin(const char* name, std::uint32_t index = kNoRegionIndex);
    void regionEnd();
    // texture LOD bias added to the samplers of the draws the jitter applies to (FSR: log2(render / display) - 1, so the
    // temporal filter has the detail to accumulate); 0 = none
    void setGeometryLodBias(float bias) { if (bias != geometryLodBias_) ++variantEpoch_; geometryLodBias_ = bias; }
    // hashed alpha test on the jittered draws (upscale.alpha_dither: abi.h kAlphaHashed; docs/research/upscaling.md 17)
    void setAlphaDither(bool on) { if (on != alphaDither_) ++variantEpoch_; alphaDither_ = on; }
    // cut-out strands of the jittered draws kept about a render pixel wide (upscale.cutout_thicken: abi.h kAlphaThicken, 0 = off;
    // docs/research/upscaling.md 19)
    void setCutoutThicken(float strength) {
        const float v = strength < 0.0f ? 0.0f : strength > 1.0f ? 1.0f : strength;
        if (v != cutoutThicken_) ++variantEpoch_;
        cutoutThicken_ = v;
    }
    // Render scale (enh/upscale): while the engine renders the main view, engine/scene_hooks shrinks the main camera's screen
    // rect by this ratio (>= 1; 1 = off), so the scene target, depth map, viewports and masks are all that much smaller; the
    // rect's real size (the display / output size) is reported back through setDisplaySize
    void setRenderRatio(float ratio) {
        const float v = ratio < 1.0f ? 1.0f : ratio;
        if (v != renderRatio_) ++variantEpoch_;
        renderRatio_ = v;
    }
    float renderRatio() const { return renderRatio_; }
    void setDisplaySize(std::uint32_t width, std::uint32_t height) { displaySize_ = {width, height}; }
    std::array<std::uint32_t, 2> displaySize() const { return displaySize_; }
    // render scale: the post chain sizes its scene-relative images (and TEXTURESIZE) at the display size (engine/scene_hooks)
    void setPostAtDisplaySize(bool on) { postAtDisplaySize_ = on; }
    bool postAtDisplaySize() const { return postAtDisplaySize_; }
    // the post chain's input: until the frame ends, draws that sample `from` get the bindless slot `slot` instead (the upscaled
    // scene, which has a different size than the engine's scene target; the engine samples both by uv)
    void setSceneSubstitute(const Texture* from, std::uint32_t slot) { substituteFrom_ = from; substituteSlot_ = slot; }
    const Texture* sceneSubstituteFrom() const { return substituteFrom_; }
    std::uint32_t sceneSubstituteSlot() const { return substituteSlot_; }
    // this frame's jitter in pixels of the scene target (0, 0 = none)
    std::array<float, 2> jitter() const { return {jitter_[0], jitter_[1]}; }
    // draw replacement (enh/water, enh/shadows): a replacer must outlive its registration; adding twice is a no-op
    void addDrawReplacer(DrawReplacer* replacer);
    void removeDrawReplacer(DrawReplacer* replacer);
    // draw observation (enh/rt_scene): an observer must outlive its registration; adding twice is a no-op
    // the N5.0 op tap (native/frame_tap): null = off
    void setDrawTap(DrawTap* tap) { drawTap_ = tap; }
    void addDrawObserver(DrawObserver* observer);
    void removeDrawObserver(DrawObserver* observer);
    // Buffer: its static GPU copy was replaced, or it is released (render thread; tells the observers)
    void notifyBufferChanged(std::uint64_t bufferId) {
        for (DrawObserver* o : observers_) o->bufferChanged(bufferId);
    }
    // a vertex stream as last set (render thread): instance data of instanced draws (TEXCOORD4 = the instance index)
    struct StreamSource {
        Buffer* buffer = nullptr;
        std::uint32_t offset = 0, stride = 0, freq = 1;
    };
    StreamSource streamSource(std::uint32_t stream) const {
        if (stream >= kStreams) return {};
        const Stream& s = streams_[stream];
        return {s.buffer, s.offset, s.stride, s.freq};
    }
    // the ray tracing scene of the frame (enh/rt_scene): its TLAS + geometry records go into the FrameBlock while the
    // submission that built it is recorded
    void setRtScene(const RtSceneRef& scene) {
        rtScene_ = scene;
        frameAddr_ = {};
    }
    const RtSceneRef& rtScene() const { return rtScene_; }
    bool rtSceneValid() const { return rtScene_.tlas != 0 && rtScene_.frame == frameValue(); }
    // ---- GI (enh/gi, docs/research/rtgi.md 5) ----
    // the probe volume's header (shaders/include/rvk_gi.glsl RvkGiVolume): copied into each FrameBlock's ring upload from now
    // on (FrameBlock::gi); empty = none
    void setGiHeader(std::span<const std::uint8_t> header) {
        giHeader_.assign(header.begin(), header.end());
        frameAddr_ = {};
    }
    // which draws take the pixel shader's GI variant (VariantSlot::Gi): the main view's lit material draws (a first pass with
    // the engine's ambient constants set) while `on`; replaceLightmaps: their lightmap samplers read the black texture
    struct GiDraws {
        bool on = false;
        bool replaceLightmaps = false;
    };
    void setGiDraws(const GiDraws& g) {
        if (g.on != giDraws_.on || g.replaceLightmaps != giDraws_.replaceLightmaps) ++variantEpoch_;
        giDraws_ = g;
    }
    // ---- material pack (enh/material_pack, docs/research/material-pack.md) ----
    // With a provider set, the main view's lit material draws take the pixel shader's variant (like GI draws) and carry the provider's word for
    // their material in the DrawBlock (pad[1] = alpha.w; 0 = no entry: the variant leaves the pixels alone). `texture` (null: constants only)
    // is uploaded here before the word is used. Render thread.
    struct PackHit {
        std::uint32_t word = 0;
        Texture* texture = nullptr;
    };
    using PackProvider = std::function<PackHit(const draws::Tag&)>;
    void setPackProvider(PackProvider p) { packProvider_ = std::move(p); ++variantEpoch_; }
    // the pack's change stamp (any value change = a lookup may answer differently): a retained packet keeps its pack word while it and the map's slot are unchanged
    void setPackStamp(std::function<std::uint32_t()> f) { packStamp_ = std::move(f); }
    // test switch (materials.alternate): while set the pack's variants are not used at all (the engine's own shaders draw), as without a pack
    void setPackBypass(bool on) { if (on != packBypass_) ++variantEpoch_; packBypass_ = on; }
    std::uint64_t giDrawsThisInterval() const { return giDrawCount_; }
    // ---- thin G-buffer (enh/gbuffer, docs/research/rtgi.md 10) ----
    // Extra colour attachments of the first main view's solid pass (scene HDR target only), only those a consumer reads (no unread
    // outputs): `normal` (A2B10G10R10: xy the octahedral world normal, z material channel, w id / 3) and `motion` (RG16F: previous uv -
    // this uv; cleared to kGbufferMotionUnwritten, the fill pass writes the camera's motion there), in that order after the engine's
    // target, cleared at the frame's first use. Either may be null (`active` with none = the variants' AO / screen probes only, no
    // attachment). The material pixel shaders' Gbuffer variants write them; every other draw of the scope keeps them (write mask 0).
    // The pipelines of such a scope differ from the plain ones: the device first builds them in the background (prewarm, the frame's
    // solid draws as the template) and arms the attachments once a frame's solid pass found all of them (or after 10 s). Render
    // thread; active == false = off.
    struct GbufferTargets {
        rhi::Texture* normal = nullptr;  // (only together with motion)
        rhi::Texture* motion = nullptr;
        std::uint32_t width = 0, height = 0;
        bool active = false;
        float motionClear = 0.0f;  // the motion target's clear value (kGbufferMotionUnwritten once the fill pass can replace it)
        std::uint32_t attachments() const { return (normal != nullptr ? 1u : 0u) + (motion != nullptr ? 1u : 0u); }
    };
    static constexpr float kGbufferMotionUnwritten = 60000.0f;  // (exact in half precision; motion is a uv delta, |x| <= ~2; gtao.comp tests > 30000)
    void setGbuffer(const GbufferTargets& g);
    bool gbufferArmed() const { return gbufArmed_; }
    bool gbufferActive() const { return gbuf_.active; }
    // the G-buffer was written in this frame's main view (its contents are this frame's)
    bool gbufferWritten() const { return gbufWrittenFrame_ == frameValue() && gbufWrittenFrame_ != 0; }
    // FrameBlock::ao (shader/abi.h): last frame's AO image for the material shaders' Gbuffer variants
    void setFrameAo(const std::array<std::uint32_t, 4>& ao) {
        if (ao_ == ao) return;
        ao_ = ao;
        frameAddr_ = {};
    }
    // FrameBlock::sgi: last frame's screen probe GI ratio (enh/gi)
    void setFrameSgi(const std::array<std::uint32_t, 4>& sgi) {
        if (sgi_ == sgi) return;
        sgi_ = sgi;
        frameAddr_ = {};
    }
    const std::array<std::uint32_t, 4>& frameSgi() const { return sgi_; }
    // ---- shader variants of enhancements (render thread) ----
    void setVariantProvider(VariantSlot slot, VariantProvider provider);
    // the GLSL material shader (shader/uber.h) instead of the translated bytecode for the engine's material pixel shaders whose
    // request is known (Shader::source): the plain module (VariantSlot::Base) and the enhancement variants (GI, G-buffer) are
    // built from it in the background; a shader it cannot build stays translated. Applies to modules requested afterwards
    void setUbershader(bool on);
    void setUbershaderSkip(std::uint32_t mask);  // classes of material shader that stay translated (shader::uberClassMask)
    // S-1a (frame-ownership.md 14.6): the pixel shader form a scene draw took, per draw kind (counted where the scene form is decided and again per owned item)
    enum class UberWhy : std::uint8_t {
        None = 0,        // the setting is off / not a material pixel shader (sky, particles ...): the translated shader, not a fallback
        Uber = 1,        // the HLSL ubershader
        Leaf = 2,        // an unknown ge_default_leafs.fx entry (S-1b: PS_Default / PS_DefaultZPass are covered): stays translated
        Skipped = 3,     // class kept translated by debug.ubershader_skip
        NoRequest = 4,   // the engine's request of this shader is unknown (no Shader::source)
        Failed = 5,      // the ubershader did not build (or the translated scene form was taken first): translated
        NoPixel = 6,     // F15-1: no pixel shader at all (S-6 native.depth_z: a depth-only z / cascade draw without texkill has no fragment stage); not a fallback
        Count
    };
    std::uint8_t uberVsFormOf(const Shader& vs, VariantSlot slot) const;
    UberWhy uberWhyOf(const Shader& ps, VariantSlot slot) const;  // the form a scene pixel shader of this slot took (device.cpp)
    // the plain ubershader module a draw of `ps` is offered now (None: off, no request, not built / not covered): read only; CachedDraw::Variant::uberAsked is its value at capture
    rhi::ShaderModule uberAskedNow(const Shader& ps) const;
    bool ubershader() const { return ubershader_.load(std::memory_order_relaxed); }
    void sourceAttached(Shader& s);  // 14.22: engine/material_source attached the request to `s` (notifySourceAttached)
    // the ubershader's lit core in real 16-bit arithmetic: the device has it (rhi::DeviceCaps::shaderFloat16) and RVK_UBER_HALF is not 0
    bool uberHalf() const;
    // the slot's variant of `s` when it is built (queues its build on the first call); null until then / when not applicable
    rhi::ShaderModule variant(const ShaderRef& s, VariantSlot slot);
    // the owning pointer of a device shader (its entry in the shader cache), null for a shader that is not cached
    std::shared_ptr<Shader> shaderOwner(const Shader* s);
    // ---- vertex capture (enh/rt_scene: skinned draws in the RT scene) ----
    // inside DrawObserver::observe: the draw's vertex shader writes, per vertex index i of the draw (index buffer value),
    // the world position (vec4, w = 1) and its TEXCOORD0 input (vec4) to `buffer` + 32 i (the capture variant, once it and
    // its pipeline are ready); vertexCaptured tells whether it ran this frame (render thread)
    // `instances`: an instanced draw may be captured too: instance k writes its vertices at `buffer` + 16 + 32 (k x slots + i),
    // slots = the header's first word (the buffer holds instances x slots x 32 bytes)
    // the capture census (frame-ownership.md 14.15; rvk.log `native: captures (5 s)`): who asks, for which draw kind, from which path
    enum class CaptureBy : std::uint8_t { MotionSkinned, MotionRigid, RtSolidNoJob, RtSolidMotion, RtCascade, RtSkinVerify, RtTree, Other, Count };
    enum class CaptureKind : std::uint8_t { Static, Instanced, Skinned, Morph, SpeedTree, Vegetation, Billboard, Other, Count };
    enum class CaptureSite : std::uint8_t { Composed, Retained, Owned, Count };
    static CaptureKind captureKindOf(const ReplaceQuery& q);
    // `-Environment 'RVK_NOCAP_OLD=1'` (14.15 A/B): the capture requests as before 14.15 (object motion's rigid hash over c31..c37, no morph targets in the skinning pass,
    // a capture while the bone forms build)
    static bool nocapOld();
    void requestVertexCapture(const rhi::RingAlloc& buffer, bool instances = false, CaptureBy by = CaptureBy::Other, CaptureKind kind = CaptureKind::Other) {
        captureRequest_ = buffer.ref;
        captureRequestRw_ = buffer.rw();
        captureInstances_ = instances;
        const CaptureSite site = ownedReplay_ ? CaptureSite::Owned : obsSite_;
        ++captureCensus_.n[static_cast<std::size_t>(by)][static_cast<std::size_t>(kind)][static_cast<std::size_t>(site)];
        const OwnedWindows::Kind w2 = owned_ != nullptr ? owned_->w[0][2].kind : OwnedWindows::Kind::None;
        ++captureCensus_.win2[static_cast<std::size_t>(by)][w2 == OwnedWindows::Kind::Object ? 1 : w2 == OwnedWindows::Kind::Bones ? 2 : 0];
    }
    // a capture the requester left out because a capture-free source serves it now (counted per requester x kind: the census's "avoided" column)
    void noteCaptureAvoided(CaptureBy by, CaptureKind kind) { ++captureCensus_.avoided[static_cast<std::size_t>(by)][static_cast<std::size_t>(kind)]; }
    // the kind of the draw's VS window w (OwnedWindows::Kind::None: no owned windows / a per-draw upload)
    OwnedWindows::Kind ownedVsWindowKind(std::uint32_t w) const { return owned_ != nullptr && w < 4 ? owned_->w[0][w].kind : OwnedWindows::Kind::None; }
    // census diagnostics of the moving rigid draws' captures: VS window 2 = an object table record / a per-draw upload; entity has a scene-table slot
    void noteRigidCapture(bool objectWindow, bool slot) { ++captureCensus_.rigid[objectWindow ? 0 : 1]; if (slot) ++captureCensus_.rigid[2]; }
    // once per present: the 5 s census line
    void captureCensusFrame();
    // a buffer written by a compute pass counts as captured this frame (enh/rt_scene's skinning pass)
    void markVertexCaptured(rhi::GpuRef buffer) { captured_.push_back(buffer); }
    bool vertexCaptured(rhi::GpuRef buffer) const {
        return std::find(captured_.begin(), captured_.end(), buffer) != captured_.end();
    }
    // ... ran in the previous frame (the RT scene built after the z pre-pass uses the previous frame's captures)
    bool vertexCapturedPrev(rhi::GpuRef buffer) const {
        return std::find(capturedPrev_.begin(), capturedPrev_.end(), buffer) != capturedPrev_.end();
    }
    // one-line status texts of enhancements for the overlay's performance panel (key -> text; empty text removes it)
    void setStatus(const std::string& key, std::string text) {
        if (text.empty()) status_.erase(key);
        else status_[key] = std::move(text);
    }
    const std::map<std::string, std::string>& status() const { return status_; }
    // the sun cascades' depth maps rendered so far this frame (the scene hooks count each ExecuteShadowMap into a DF24 /
    // DF16 depth texture; enh/shadows knows the frame's last cascade by it)
    void countShadowDepthMap() { ++shadowDepthMaps_; }
    // a frame that skipped the cascades' update (engine/shadow_maps): the maps it still reads count as drawn
    void creditShadowDepthMaps(std::uint32_t n) { shadowDepthMaps_ += n; }
    std::uint32_t shadowDepthMaps() const { return shadowDepthMaps_; }
    // the D3D9 float constants as last set (render thread)
    const float* vsConstant(std::uint32_t reg) const { return vsF_[reg < kVsFloatConsts ? reg : 0]; }
    const float* psConstant(std::uint32_t reg) const { return psF_[reg < kPsFloatConsts ? reg : 0]; }
    // all of them (render thread; CPU evaluation of the draw's shaders, shader/sm_eval.h)
    const float (*vsConstants() const)[4] { return vsF_; }
    const float (*psConstants() const)[4] { return psF_; }
    const std::int32_t (*vsIntConstants() const)[4] { return vsI_; }
    const std::int32_t (*psIntConstants() const)[4] { return psI_; }
    const std::uint32_t* vsBoolConstants() const { return vsB_; }
    const std::uint32_t* psBoolConstants() const { return psB_; }
    // a bindless sampler index for passes / replacement shaders (linear or point, clamp or wrap, mips linear)
    std::uint32_t samplerFor(bool linear, bool clamp);
    // the texture behind an engine eCAPITexture pointer (the HAL sets the resolver; null when it is none of ours)
    void setEngineTextureResolver(std::function<Texture*(const void*)> resolver) { engineTextureResolver_ = std::move(resolver); }
    Texture* engineTexture(const void* api) const { return engineTextureResolver_ ? engineTextureResolver_(api) : nullptr; }
    // the bindless sampler of the engine's sampler states at pixel slot s (render thread, inside a draw replacement)
    std::uint32_t samplerOfSlot(std::uint32_t slot) { return samplerIndex(slot); }
    // Pipelines of replacement shaders, built in the background (no hitch when a player turns a feature on): registered
    // pairs are prewarmed as soon as the first water draw shows the formats (its pipeline description is the template);
    // prebuildReplacement queues one now (false: no water draw seen yet); replacementReady: its pipeline exists
    void registerReplacement(std::shared_ptr<Shader> vs, std::shared_ptr<Shader> ps, bool procedural);
    bool prebuildReplacement(const Shader& vs, const Shader& ps, bool procedural);
    bool replacementReady(const Shader& vs, const Shader& ps, bool procedural) const;
    // a texture ready for sampling (vertex / fragment / compute) in a pass: pending uploads + layout; false if not sampleable
    bool readable(Texture* texture, rhi::CommandList& cmd);

    // internals used by resources
    // (the backend seam below replaces the old public ctx() / frames() for everything outside the not yet moved frame loop)
    // the transient images of the passes (GameDevice::transient): created on first use, recreated when the description changes
    rhi::Texture* transient(std::string_view key, const rhi::TransientDesc& desc) { return game_->transient(key, desc); }
    // The main view's hardware depth converted to R32F (shaders/upscale/prep.comp) by whichever consumer ran first this frame (FSR's
    // "upscale.depth"): a second consumer at the same size reads that image instead of converting again (performance.md 32)
    // RVK_BW_BASELINE=1 (`-Environment`): the pre-performance.md-32 behaviour (stencil planes attached everywhere, clears as vkCmdClearAttachments,
    // R16 / RGBA8 images, one depth conversion per consumer), to measure the difference in one build
    static bool bwBaseline() {
        static const bool on = GetEnvironmentVariableA("RVK_BW_BASELINE", nullptr, 0) != 0;
        return on;
    }
    void notePreparedDepth(const char* key, VkExtent2D extent) { preparedDepth_ = {key, extent, frameValue()}; }
    const char* preparedDepthKey(VkExtent2D extent) const {
        return preparedDepth_.frame == frameValue() && preparedDepth_.extent.width == extent.width && preparedDepth_.extent.height == extent.height
                   ? preparedDepth_.key
                   : nullptr;
    }
    void releaseTransient(std::string_view key) { game_->releaseTransient(key); }
    // frame ring / frame state (GameDevice)
    rhi::RingAlloc ring(std::uint64_t size, std::uint64_t alignment = 256) { return game_->allocRing(size, alignment); }
    std::uint32_t frameSlot() const { return game_->frameSlot(); }
    std::uint64_t completedFrame() const { return game_->completedFrame(); }
    void retire(std::function<void()> fn) { game_->retire(std::move(fn)); }
    bool namesEnabled() const;
    // the slim RHI over this device's Vulkan objects (rhi/vulkan/wrap.h): the backend seam `game()` as an rhi::Device (passes, textures, acceleration structures)
    rhi::Device* rhi() { return game_.get(); }
    rhi::GameDevice& game() { return *game_; }  // valid between create() and destroy()
    std::uint64_t frameValue() const { return game_->frameValue(); }
    VkFormat d24s8Format() const { return d24s8_; }
    bool recordingThread() const;
    // the frame's upload command list (submitted before the frame's draws), ordered after the earlier work (the backend records the barrier of the
    // first use in a frame); render thread
    rhi::CommandList& uploadList();
    // the frame's main command list (GameDevice::frameCommands)
    rhi::CommandList& frameList() { return game_->frameCommands(); }

private:
    struct Stream {
        Buffer* buffer = nullptr;
        std::uint32_t offset = 0;
        std::uint32_t stride = 0;
        std::uint32_t freq = 1;
    };
    struct SamplerKey {
        std::uint32_t states[SAMP_COUNT];
        bool operator==(const SamplerKey& o) const;
    };
    struct SamplerKeyHash {
        std::size_t operator()(const SamplerKey& k) const;
    };

    void beginFrame();
    // ends the open scope and submits the frame's work; `presenting`: the frame ends in a present blit (the GPU timestamp label)
    void submitFrame(bool presenting);
    void processReleases();
    bool prepareTextures();
    void endRendering();
    // `clear`: a Device::clear that opened this scope; when it covers the whole attachment area the scope's load ops do it (VK_ATTACHMENT_LOAD_OP_CLEAR)
    // instead of a vkCmdClearAttachments after a LOAD (performance.md 32.5); the flags it performed are in scopeClearFlags_
    struct ScopeClear {
        std::uint32_t flags;
        float color[4];
        float z;
        std::uint32_t stencil;
    };
    bool beginRendering(const ScopeClear* clear = nullptr);
    float clearDepthValue(float z);
    std::uint32_t scopeClearFlags_ = 0;
    // the geometry of a draw (for its record)
    struct DrawGeom {
        bool indexed = false;
        bool userPointer = false;
        std::uint32_t first = 0;
        std::uint32_t primitives = 0;
        std::int32_t baseVertex = 0;
        std::uint32_t minIndex = 0, numVertices = 0;
    };
    bool prepareDraw(std::uint32_t primitiveType, const DrawGeom& geom);
    // F5-0 measurements (RVK_F5_MEASURE=1; d3d9/f5_note.cpp, frame-ownership.md 7.22): the draw's triangles per view / size class, cluster + exact estimates
    void f5Note(std::uint32_t primitiveType, const DrawGeom& geom, draws::Kind kind, scene::Phase phase);
    // ---- F5-1 clusters (native.clusters, frame-ownership.md 7.33; d3d9/cluster_draw.cpp). A static rigid opaque draw of >= 1,024 triangles whose range has a built cluster
    // order is drawn from the arena copy of its indices in cluster order (same triangles, same vertex order of each): packet_.indexBuffer / first are pointed at it
    void clusterApply(std::uint32_t type, std::int32_t baseVertex, std::uint32_t minIndex, std::uint32_t numVertices, std::uint32_t startIndex, std::uint32_t primitives);
    bool clusters_ = false;
    bool clusterCull_ = false;
    bool mainViewOwned_ = false;
    bool clusterCullShadows_ = false;
    std::function<bool(std::uint64_t, float*)> cullWorldRowsHook_;
    std::uint64_t clusterWait_ = 0;
    bool clusterUsing_ = false;
    // GI variant eligibility of the current draw (prepareDraw): the engine set its ambient constants (a first pass)
    bool giEligible(const scene::Phase& phase) const;
    bool giStaticEligible(const scene::Phase& phase) const;  // giEligible without the ambient registers' test
    bool ambientLit() const;                                  // ... that test: the ambient semantic's 7 registers hold something
    std::uint32_t packWord(CachedDraw::Variant* note = nullptr);                                  // the material pack's word for the current tag (0 = none), uploads its map when needed
    // the G-buffer: whether the current targets / phase are its scope (first main view, solid pass, the scene HDR target)
    bool gbufferTargetsMatch() const;
    bool gbufferScope() const { return gbuf_.active && gbufArmed_ && gbufferTargetsMatch(); }
    // the pixel shader variant slot of a G-buffer material draw for the current targets (per-object motion: the capture VS's previous position)
    VariantSlot gbufferSlot(bool perObjectMotion) const {
        if (gbuf_.normal != nullptr) return perObjectMotion ? VariantSlot::GbufferMotion : VariantSlot::Gbuffer;
        if (gbuf_.motion != nullptr) return perObjectMotion ? VariantSlot::GbufferMotionM : VariantSlot::GbufferM;
        return VariantSlot::GbufferA;  // (no target: nothing to write, no per-object motion)
    }
    // a G-buffer material draw: the material pixel shader's Gbuffer variant applies (opaque or alpha-over, depth tested)
    bool gbufferEligible(const scene::Phase& phase, draws::Kind kind) const;
    bool gbufferStaticEligible(const scene::Phase& phase, draws::Kind kind) const;  // ... without the ambient registers' test
    bool gbufferLit() const;                                                        // ... that test
    BlendAttachment attachmentBlend(std::uint32_t index) const;
    void variantWorker();
    void stopVariantWorker();
    void enqueueShaderJob(Shader* shader, std::function<void()> job);  // the compile of a translated shader (createShader)
public:
    // a draw needs `s` now: its compile goes to the front of the queue (once per shader); the draw itself is skipped until the module exists
    void prioritizeShader(Shader& s);
    // translated shaders queued for the workers right now (not yet picked up; a job being compiled is not counted)
    std::size_t queuedShaderJobs() const;
private:
    void shaderWorker();
    void stopShaderWorkers();
    void logWaterDraw(draws::Kind kind);
    // diagnostics: RVK_TRACE_MATERIAL = a substring of material names: every draw state of the matching materials, once per
    // material x view x pass x shaders (names are resolved in the opaque passes only with -RvkLogLevel trace)
    void logTracedMaterial(draws::Kind kind, bool jittered);
    rhi::ShaderModule tintModule(Shader& ps);  // the tint variant of a pixel shader (None when it fails to build)
    rhi::ShaderModule lodModule(Shader& vs);   // the billboard fade variant of a vertex shader (None when it fails to build)
    rhi::ShaderModule dxilModule(shader::Stage stage, const std::string& text, std::string_view name, const std::filesystem::path& spvCacheFile, const std::string& label, std::uint64_t hash, bool half16 = false);
    rhi::ShaderModule variantModule(const Shader& s, const shader::TranslateOptions& options, std::string_view tag, bool* viaUber = nullptr);
    void resetUberVariants();  // S-1a: debug.ubershader toggled at run time: every material pixel shader's variants are built again in the new form
    rhi::ShaderModule uberModule(const Shader& s, const MaterialSource& src, const shader::TranslateOptions& options, std::string_view tag);
    std::atomic<bool> ubershader_{false};
    std::atomic<std::uint64_t> ubershadersBuilt_{0}, ubershadersFailed_{0};
    // A draw's recording state: prepareDraw / the draw call compute the packet (the interned rhi::VertexInputValues /
    // rhi::DynamicStateValues blocks below, plus the push constants); the backend records it: now through
    // GameDevice::applyDrawPacket, or later in a chunk on a record thread (docs/research/performance.md 12 + 14)
    bool computeVertexInput(PipelineDesc& pd, VertexInputValues& out);
    void computeDynamicState(std::uint32_t primitiveType, DynamicStateValues& out) const;
    // records the packet now (after the pending batch) or, when it is batchable and batching is on, appends it to the batch
    void submitPacket(DrawPacket& p);  // p = packet_: interns its vertex input / dynamic state, then records or batches it
    // records the pending batch, its draws grouped by pipeline (the engine's order within a pipeline); before anything else
    // is recorded into the frame's command buffer (endRendering, clear, a draw that is not batched)
    void flushBatch();
    std::uint32_t samplerIndex(std::uint32_t slot);
    std::uint32_t samplerIndexBiased(std::uint32_t slot, float bias);
    std::uint32_t lookupSampler(std::uint32_t slot);
    void uploadConstants(bool forceLegacyPs, std::uint32_t psMinEnd);
    // the draw being prepared: split stages and the window addresses its push constants carry
    bool drawSplit_[2] = {};
    rhi::GpuRef drawWin_[2][shader::abi::kConstWindows]{};
    const OwnedWindows* owned_ = nullptr;      // setOwnedWindows
    std::uint32_t ownedServed_[2] = {};        // per stage: the windows of the draw being prepared that came from owned_ (verifySplit skips them)
    OwnedStats ownedStats_;
    bool ownedReplay_ = false;                 // F2c-3: replayOwned is preparing the draw (the register file is not the draw's: no verification against it)
    bool ownedWindowsServed(std::uint32_t* stageOut = nullptr, std::uint32_t* windowOut = nullptr, std::uint32_t* needOut = nullptr) const;
    void loadOwnedRegisters(const OwnedWindows& ow);
    std::uint32_t psWindowMinEnd_ = 0;
    // F2c-4b: the bulk observation lists (owned_persist.cpp)
    bool ownedBulkOn_ = true;  // RVK_F2C4B_PERDRAW=1 = off (A/B: the per-draw path)
    bool ownedBulkSuspended_ = false;  // F9 / F14: an ordered owned op is being drawn (its draws are observed per draw)
    OrderedHook orderedHook_ = nullptr;
    void* orderedHookCtx_ = nullptr;
    bool orderedPending_ = false;
    int ownedRegSnap_ = 0;  // 0 off, 1 armed, 2 taken
    float ownedRegSnapVs_[kVsFloatConsts][4]{};
    float ownedRegSnapPs_[kPsFloatConsts][4]{};
    std::vector<OwnedRecordView> ownedBulk_;
    std::vector<std::uint32_t> ownedBulkIds_[2];        // this pass's ids per kind (z, solid): the next pass's `left` is made from them
    std::vector<std::uint32_t> ownedBulkPrevIds_[2];
    std::vector<std::uint32_t> ownedBulkSeen_[2];       // per id: the serial of the last pass that held it
    std::uint32_t ownedBulkSerial_[2] = {0, 0}, ownedBulkPrevSerial_[2] = {0, 0};
    std::vector<std::uint32_t> ownedBulkLeft_;
    std::uint32_t ownedBulkKind_ = 0;
    void blit(rhi::Texture& src, std::uint32_t srcLevel, std::uint32_t srcLayer, const VkOffset3D srcBox[2], rhi::Texture& dst,
              std::uint32_t dstLevel, std::uint32_t dstLayer, const VkOffset3D dstBox[2], VkFilter filter);
    static std::uint32_t slotOf(std::uint32_t d3dSampler);

    DeviceOptions options_;
    void recreateSwapchain();
    rhi::GamePresenter& pres() const { return game_->presenter(); }
    pacing::FramePacer pacer_;
    FrameGenClient* fgClient_ = nullptr;
    bool fgRunning_ = false;
    bool fgUiInstalled_ = false;
    // live output / base rate (once a second) and the report window
    std::chrono::steady_clock::time_point fgTick_{};
    std::uint64_t fgTickBase_ = 0, fgTickReal_ = 0, fgTickGen_ = 0;
    std::chrono::steady_clock::time_point fgReportAt_{};
    std::uint64_t fgReportBase_ = 0, fgReportReal_ = 0;
    void fgStatsTick(std::chrono::steady_clock::time_point now);
    void rebuildSwapchain(std::uint32_t width, std::uint32_t height);
    std::chrono::steady_clock::time_point pacerChecked_{};
    void* pacerMonitor_ = nullptr;
    double pacerMonitorRate_ = 0.0;
    double pacerRequested_ = -1.0;
    void updatePacer();
    void resetPacer();
    double refreshCap_ = 0.0;
    double menuCap_ = 0.0;
    bool unpaced_ = false;
    std::chrono::steady_clock::time_point loadingShown_{};  // the last loading-screen frame actually presented
    void* fseMonitor_ = nullptr;
    bool fseHold_ = false;
    std::chrono::steady_clock::time_point fseRetry_{};  // next acquire attempt after a failure / loss
    void serviceFullScreenExclusive();
    VkFormat d24s8_ = VK_FORMAT_D24_UNORM_S8_UINT;
    std::uint32_t renderThread_ = 0;
    bool created_ = false;

    std::unique_ptr<Texture> backBuffer_;
    std::unique_ptr<Texture> depthBuffer_;
    Surface* backBufferSurface_ = nullptr;
    Surface* depthSurface_ = nullptr;

    // shader cache
    std::mutex shaderMutex_;
    std::unordered_map<std::uint64_t, std::shared_ptr<Shader>, IdHash> shaders_;
    std::unordered_map<rhi::ShaderModule, std::uint64_t> moduleHashes_;

    // Pipelines of earlier runs (<cacheDir>/pipelines.keys): the shader hashes + the rest of the description. When both
    // shaders of a known pipeline exist (they are created while the game loads), it is built in the background, so its
    // first draw does not stall. New pipelines are appended and the file is rewritten every 10 s.
    // F15-1 (frame-ownership.md 14.20): a key also names the VARIANT a stage's module is (vsSlot / psSlot = VariantSlot, kBaseModule = the shader's own module):
    // the scene forms of the owned path are variants, and their pipelines are built in the background (prewarmOncePipeline), so before F15-1 they were never
    // remembered. At a shader's creation the variants of its known keys are requested and the pipeline is built once both modules exist (prewarmVariantKeys).
    static constexpr std::uint8_t kBaseModule = 0xff;
    static constexpr std::uint8_t kNoModule = 0xfe;  // (psSlot: no fragment stage, S-6's depth-only forms; ps = the vertex shader's hash)
    struct PipelineKey {
        std::uint64_t vs = 0;
        std::uint64_t ps = 0;
        std::uint8_t vsSlot = kBaseModule, psSlot = kBaseModule;
        std::uint8_t pad[6] = {};
        PipelineDesc desc{};  // vs / fs = VK_NULL_HANDLE
    };
    void loadPipelineKeys();
    void savePipelineKeys();
    void recordPipelineKey(const PipelineDesc& pd);
    void recordBackgroundKey(const PipelineDesc& pd);  // F15-1: a pipeline requested in the background (scene forms), deduplicated by a hash of the description
    void prewarmPipelines(std::uint64_t shaderHash);
    void prewarmVariantKeys(std::uint64_t shaderHash, VariantSlot slot);  // F15-1: a variant module was built: the known keys waiting for it
    bool prewarmKey(std::size_t index);                                    // F15-1: false = a variant module is missing (requested; the key waits for it)
    std::unordered_map<rhi::ShaderModule, std::pair<std::uint64_t, std::uint8_t>> variantHashes_;  // variant module -> (shader hash, slot) (under shaderMutex_)
    std::vector<rhi::ShaderModule> retiredModules_;  // 14.26: variant modules replaced before they reached a slot (under shaderMutex_): destroyed with the device
    std::unordered_multimap<std::uint64_t, std::size_t> variantWaitKeys_;  // mix(shader hash, slot) -> keys_ index waiting for that variant (under keysMutex_)
    std::unordered_set<std::uint64_t> backgroundKeys_;
    std::unordered_set<std::size_t> keysPrewarmed_;
    std::atomic<bool> destroying_{false};  // F15-1: destroy() began: no variant request / key prewarm from a worker thread (they would restart the stopped variant workers)                        // keys_ indices prewarmKey built (under keysMutex_)                     // description hashes recordBackgroundKey saw (under keysMutex_)
    struct KeyStats {
        std::atomic<std::uint64_t> variantKeysLoaded{0}, variantRequests{0}, variantPrewarmed{0}, basePrewarmed{0}, recorded{0}, waits{0};
    } keyStats_;
    std::mutex keysMutex_;
    std::vector<PipelineKey> keys_;
    std::unordered_multimap<std::uint64_t, std::size_t> pendingKeys_;  // shader hash -> index into keys_, not yet built
    std::unordered_set<std::string> keySet_;                             // raw bytes of every key in keys_
    bool keysDirty_ = false;
    std::thread keySaver_;

    // samplers
    std::mutex samplerMutex_;
    std::unordered_map<SamplerKey, std::uint32_t, SamplerKeyHash> samplers_;

    // D3D9 state
    std::array<std::uint32_t, RS_COUNT> rs_{};
    std::array<std::array<std::uint32_t, SAMP_COUNT>, kSamplerCount> samp_{};
    std::array<Texture*, kSamplerCount> tex_{};
    std::array<Stream, kStreams> streams_{};
    Buffer* indices_ = nullptr;
    VertexDecl* decl_ = nullptr;
    ShaderRef vs_;
    ShaderRef ps_;
    float vsF_[kVsFloatConsts][4]{};
    float psF_[kPsFloatConsts][4]{};
    std::function<Texture*(const void*)> engineTextureResolver_;
    std::int32_t vsI_[16][4]{};
    std::int32_t psI_[16][4]{};
    std::uint32_t vsB_[16]{};
    std::uint32_t psB_[16]{};
    std::array<Surface*, kRenderTargets> rt_{};
    Surface* ds_ = nullptr;
    Viewport viewport_;
    std::int32_t scissor_[4]{};

    // live resources (trackLive)
    std::mutex liveMutex_;
    std::unordered_set<Texture*> liveTextures_;
    std::unordered_set<Buffer*> liveBuffers_;
    void orphanLive();
    void upAllocFailed(std::uint64_t bytes);

    // deferred releases
    std::mutex releaseMutex_;
    std::vector<std::unique_ptr<Texture>> releasedTextures_;
    std::vector<std::unique_ptr<Buffer>> releasedBuffers_;
    std::vector<std::unique_ptr<VertexDecl>> releasedDecls_;

    // DrawPrimitiveUP data replaces stream 0 for one draw
    struct UpStream {
        bool active = false;
        std::uint64_t buffer = 0;  // the backend's buffer handle (ring block)
        std::uint64_t offset = 0;
        std::uint32_t stride = 0;
    } up_;

    // recording state
    bool rendering_ = false;
    std::uint32_t colorCount_ = 0;
    VkFormat colorFormats_[kRenderTargets]{};
    VkFormat depthFormat_ = VK_FORMAT_UNDEFINED;
    struct PreparedDepth {
        const char* key = nullptr;
        VkExtent2D extent{};
        std::uint64_t frame = ~0ull;
    } preparedDepth_;
    VkFormat stencilFormat_ = VK_FORMAT_UNDEFINED;
    std::uint32_t targetWidth_ = 0;
    std::uint32_t targetHeight_ = 0;
    bool vsConstDirty_ = true;
    bool psConstDirty_ = true;
    rhi::GpuRef vsConstAddr_;
    rhi::GpuRef psConstAddr_;
    // split constants: per stage and register window (abi.h) the last block written to the frame's ring (base address and host
    // pointer, `extent` registers from the window's first one) and whether a register of the window changed since. All dirty
    // at the start of a frame (ring memory belongs to its frame)
    struct ConstWindow {
        rhi::GpuRef address;
        const std::uint8_t* ptr = nullptr;
        std::uint32_t extent = 0;
    };
    ConstWindow constWin_[2][shader::abi::kConstWindows]{};
    std::uint32_t constWinDirty_[2] = {0xF, 0xF};
    rhi::GpuRef zeroBlock_;  // a zeroed ring block for the windows a shader does not read
    void dirtyConstWindows() {
        constWinDirty_[0] = constWinDirty_[1] = 0xF;
        zeroBlock_ = {};
        for (auto& stage : constWin_) {
            for (auto& w : stage) w = {};
        }
    }
    static std::uint32_t constWindowMask(bool vertex, std::uint32_t start, std::uint32_t count);
    void splitStage(bool vertex, const Shader& sh, rhi::GpuRef (&windows)[shader::abi::kConstWindows]);
    void verifySplit(bool vertex, const Shader& sh);
    void verifyOwned(bool vertex, std::uint32_t w, std::uint32_t need, const OwnedWindows::Win& ow, const float (*regs)[4], const Shader& sh);
    // redundant-command filters, valid within one command buffer (reset by beginFrame)
    PipelineDesc lastPipelineDesc_{};
    GamePipeline lastPipeline_ = GamePipeline::None;
    bool lastPipelineValid_ = false;
    // recently used descriptions -> pipeline (prepareDraw: in front of the backend's pipeline cache)
    struct RecentPipeline {
        PipelineDesc desc{};
        GamePipeline pipeline = GamePipeline::None;
    };
    std::array<RecentPipeline, 256> recentPipelines_{};
    static std::size_t recentSlot(const PipelineDesc& pd);
    GamePipeline findPipeline(const PipelineDesc& d);
    bool sameDesc(const PipelineDesc& a, const PipelineDesc& b) const;
    // what a vertex shader's inputs read from a vertex declaration (computeVertexInput): fixed per (declaration, shader)
    struct VertexPlanInput {
        std::uint32_t location = 0;
        std::uint32_t stream = UINT32_MAX;  // UINT32_MAX: the declaration provides nothing usable (the input reads 0,0,0,1)
        std::uint32_t offset = 0;
        VkFormat format = VK_FORMAT_UNDEFINED;
    };
    struct VertexPlan {
        std::vector<VertexPlanInput> inputs;
    };
    const VertexPlan& vertexPlan(const VertexDecl* decl, Shader& vs);
    std::unordered_map<std::uint64_t, VertexPlan, IdHash> vertexPlans_;  // (declaration id, vertex shader hash)
    const VertexDecl* planDecl_ = nullptr;                               // the last lookup
    const Shader* planShader_ = nullptr;
    std::uint64_t planDeclId_ = 0;
    const VertexPlan* plan_ = nullptr;

    // ---- scope chunks (the backend records them: the backend's GameRecord, docs/research/performance.md 12 + 14) -------------------
    // The opaque passes' rendering scopes are begun with secondary contents by the backend: their draw packets go to it in
    // chunks (kChunkPackets), which it records on worker threads into secondary command buffers while the engine thread
    // goes on; endScope waits for them and executes them in order (+ the debug labels kept in order with the chunks).
    bool wantSecondaryScope() const;            // the current phase's scope is recorded by the record threads
    DrawChunk& currentChunk();                  // the scope's open chunk (built by the D3D9 model)
    void emitPacket(const DrawPacket& p);       // into the scope's open chunk
    void emitClear(const ClearOp& op);
    void closeChunk();                          // the open chunk (if any) to the backend
    void scopeLabel(bool begin, const char* text);
    void finishSecondaryScope();                // endRendering: hand over the last open chunk (the backend waits + executes)
    bool recordThreadsWanted_ = false;          // debug.record_threads
    bool scopeSecondary_ = false;               // the current rendering scope is a secondary-contents one
    DrawChunk workChunk_;
    bool chunksOpen_ = false;
    // the draw's vertexOffset / firstVertex for a per-vertex stream bound at offset 0 (computeVertexInput: mesh arena)
    std::int32_t vertexShift_ = 0;
    // an index buffer binding: an offset that is a multiple of the index size goes into firstIndex (the arena block stays
    // bound); returns the first index, `bindOffset` the binding's offset
    static std::uint32_t foldIndexOffset(VkDeviceSize offset, bool index32, std::uint32_t firstIndex, VkDeviceSize& bindOffset);
    // the draw being prepared (prepareDraw -> the draw call) and the pending batch (flushBatch)
    // packet retention: the capture in progress and what prepareDraw hands to it
    CachedDraw* capture_ = nullptr;
    ReplaceQuery lastQuery_;
    CachedDraw::Variant lastVariant_;
    bool skipObservers_ = false;  // replayDraw already ran the observers for this draw and then refused it
    bool captureOk_ = false;
    bool replayStale_ = false;
    bool retainVariants_ = true;
    shader::abi::DrawBlock lastBlock_{};
    draws::Kind lastKind_ = draws::Kind::Unknown;
    bool replayBuild(const CachedDraw& c, const ReplayInputs& in, DrawPacket& p, shader::abi::DrawBlock& b);
    const float* ownedObjectRows();
    void fillCapture(const DrawPacket& p);
    void submitInterned(DrawPacket& p);
    // F2b-1 (scene_draws.cpp)
    bool sceneActive() const { return sceneIssue_ && sceneDrawsOn_ && sceneAvail_; }
    bool sceneShadows_ = false;
    bool sceneActors_ = false;
    bool sceneInstances_ = false;
    bool sceneOrdered_ = false;   // F9 / F14 (setSceneOrdered)
    // F6-2 / 14.23 item 2: while the ordered owned path is on, a stream bound at instance rate (D3DSTREAMSOURCE_INSTANCEDATA) of a draw that is NOT instanced (stream 0 without
    // INDEXEDDATA: one instance, the attribute reads element 0) is bound per vertex with stride 0 (the same element for every vertex: the same values, and an indirect draw's
    // firstInstance = its record index no longer moves it). `-Environment 'RVK_F62_INSTRATE_OLD=1'` keeps the instance-rate binding (A/B)
    std::uint32_t instConstStreams_ = 0;  // (computeVertexInput: the streams bound that way for the current draw, bit = stream)
public:
    bool instRateAsVertex() const;
    // the draw's streams are instanced for the owned windows (c32 + 4 i): a stream 0 with INDEXEDDATA, or an instance-data stream that is not bound per vertex (instRateAsVertex)
    template <class S>
    bool streamsInstanced(const S& streams) const {
        bool indexed = false, instData = false, other = false;
        for (const auto& st : streams) {
            if (st.buffer == nullptr || st.freq == 1) continue;
            if ((st.freq & STREAMSOURCE_INDEXEDDATA) != 0) indexed = true;
            else if (st.freq == (STREAMSOURCE_INSTANCEDATA | 1u)) instData = true;
            else other = true;
        }
        return indexed || other || (instData && !instRateAsVertex());
    }
    struct InstConstStats { std::uint64_t draws = 0; };
    InstConstStats instConstStats;
private:
    bool orderedNow_ = false;     // prepareDraw: the draw being prepared is an ordered scene candidate (not batchable, the ordered path may own it)
    void sceneDecide(const PipelineDesc& pd, bool giDraw, bool captureDraw, bool uberTaken, draws::Kind kind, const scene::Phase& phase, bool geomOk,
                     rhi::ShaderModule fsBeforeDrop);
    // false: an address the scene shader reads is null, the packet stays a normal one. `viewSlot` >= 0: a scene ABI v2 packet (the record carries no view windows, c5w =
    // the bits of the pixel c5.w; the packet's ref stream carries the slot)
    bool toScenePacket(DrawPacket& p, GamePipeline pipeline, std::int32_t viewSlot = -1, std::uint32_t c5w = 0);
    bool ensureSceneRef();
    bool ensureSceneRefV2(std::uint32_t slot);
    void currentPosFix(float out[4], float& jx, float& jy);
    const VertexInputValues* internSceneVi(const VertexInputValues& normal, std::int32_t viewSlot = -1);
    const VertexInputValues* internSceneViAt(const VertexInputValues& normal, std::uint64_t refBuffer, std::uint64_t refOffset);
    // S-4: the vertex input of a pulled scene draw: the ref stream alone (binding 0, per instance, attribute 24)
    const VertexInputValues* internPullVi(bool dynamicLayout, std::uint64_t refBuffer, std::uint64_t refOffset);
    void sceneFlushRun(const std::vector<std::uint32_t>& run, const std::function<void(const DrawPacket&)>& record);
    int sceneMergeBreak(const DrawPacket& a, const DrawPacket& b) const;  // -1 = mergeable, else the SceneDrawStats::breaks index
    void sceneBeginFrame();
    void sceneCollectCheck();
    bool sceneDrawsOn_ = false, sceneAvail_ = false, sceneIssue_ = false, sceneFaultInject_ = false, sceneBb_ = false, sceneTrees_ = false, sceneVeg_ = false;
    std::uint64_t ownedSubmitted_ = 0;  // F4-1: draws submitted by submitOwned
    std::uint32_t sceneCheckEvery_ = 0;
    GamePipeline scenePipe_ = GamePipeline::None;  // prepareDraw's decision for the draw being submitted
    GamePipeline scenePipe2_ = GamePipeline::None;  // ... its scene ABI v2 form (retained draws only)
    GamePipeline scenePipePull_ = GamePipeline::None;  // ... and its pulled form (S-4)
    // ---- bone-history motion (native.bone_motion, frame-ownership.md 14.13; d3d9/bone_motion.cpp)
    bool rigidMode_ = false, rigidWanted_ = false;  // 14.15 b
    int boneMode_ = 0;              // 0 off, 1 on (the skinned solid draws' motion from the bone table's history), 2 compare (captures as before + a GPU compare)
    bool sceneBone_ = false;        // sceneDecide chose the bone-motion forms for the draw being prepared
    bool obsBone_ = false;          // the draw the observers are being shown is a bone-motion draw (boneMotionDraw)
    bool obsBonePending_ = false;   // ... would be, its forms are still being built (boneMotionCovered)
    bool obsNoMotion_ = false;      // F6-2: ... or it writes no G-buffer motion (noMotionOf / noMotionNow)
    rhi::GpuRef bonePushPrev_;      // 14.15 b: the draw being prepared takes the bone-motion PUSH form (an instanced-mesh op): its object record's history base
    bool obsRigidStill_ = false;    // 14.22: the draw is rigid and has no motion of its own (boneMotionCovered: no capture)
    rhi::GpuRef rigidPrev_;         // 14.22: the draw's previous-frame window 2 block (a per-frame ring block): the bone forms' prevVs instead of historyOf
    // 14.22: the instanced-mesh draws with an uploaded VS window 2, per draw key (entity x buffers x range): the rows c33..c35 seen last frame / this frame
    struct RigidRows {
        std::uint64_t frame = 0;
        std::uint32_t n = 0, prevN = 0;
        float first[3][4] = {}, prevFirst[3][4] = {};
        std::vector<std::uint64_t> hashes, prevHashes;  // (the rows of each instance drawn under the key, this frame / the last; capped at 512)
    };
    std::unordered_map<std::uint64_t, RigidRows> rigidRows_;
    // 14.26: an instance-list draw whose composition named its instance (setInstanceIdentity: the instance's illumination, one per render instance): its rows of this frame
    // and the last keyed by (draw key, instance), so a list in which one instance moves gives that instance its own motion (the sorted position changes with the camera)
    struct InstanceRows {
        std::uint64_t frame = 0;
        float cur[3][4] = {}, prev[3][4] = {};
        bool prevValid = false, conflict = false;  // (conflict: the identity was drawn with other rows in the same frame: ambiguous)
    };
    std::unordered_map<std::uint64_t, InstanceRows> instanceRows_;
    const void* instanceId_ = nullptr;
    std::uint64_t rigidRowsSweep_ = 0;
    bool rigidListPredict(const DrawGeom& geom);  // bonePredict's instanced-mesh branch without an object record
    CaptureSite obsSite_ = CaptureSite::Composed;  // the path showing the observers the draw (capture census)
    struct CaptureCensus {
        std::uint64_t n[static_cast<std::size_t>(CaptureBy::Count)][static_cast<std::size_t>(CaptureKind::Count)][static_cast<std::size_t>(CaptureSite::Count)] = {};
        std::uint64_t avoided[static_cast<std::size_t>(CaptureBy::Count)][static_cast<std::size_t>(CaptureKind::Count)] = {};
        std::uint64_t win2[static_cast<std::size_t>(CaptureBy::Count)][3] = {};  // per requester: VS window 2 uploaded / an object record / a bone image
        std::uint64_t rigid[3] = {};  // moving rigid captures: VS window 2 an object record, a per-draw upload; the entity has a scene-table slot
    } captureCensus_, captureCensusLast_;
    double captureCensusLog_ = -1e9;
    std::uint64_t captureCensusFrames_ = 0, captureCensusFramesLast_ = 0;
    std::unordered_set<std::uint64_t> bonePipes_;  // the scene pipelines (v1, v2, pulled) of the bone-motion forms: toScenePacket gives their records the history base
    bool bonePipe(GamePipeline p) const { return boneMode_ == 1 && !bonePipes_.empty() && bonePipes_.contains(static_cast<std::uint64_t>(p)); }
    bool boneFormsReady();          // the current shaders' bone-motion forms (v1 + v2 VS, the motion PS's scene forms) exist (requested when not)
    bool bonePredict(draws::Kind kind, const scene::Phase& phase, const DrawGeom* geom = nullptr);  // prepareDraw, before the observers: the draw will take the bone-motion forms
    bool noMotionOf(const CachedDraw& c) const;  // (F6-2: see noMotionNow)
    bool boneOfCached(const CachedDraw& c) const { return c.boneMotion && boneMode_ == 1 && gbuf_.active && gbuf_.motion != nullptr; }
    std::unique_ptr<rhi::Buffer> boneCmp_;  // compare mode: kBoneCmpSlots x 16 bytes the capture vertex shaders accumulate into (Readback)
    std::uint64_t boneCmpFrame_ = 0;
    double boneLog_ = -1e9;
    BoneMotionStats boneLast_;
public:
    // once per present (gpuscene::BoneTable::flush): compare mode's readback + the next frame's compare slot (frame param kFrameParamBoneMotion), the 5 s line
    void boneMotionFrame();
private:
    void computePullFetch(const PipelineDesc& pd, const std::uint32_t* streamBinding, std::uint32_t nullBinding);
    // S-4: variant modules shared by their text (Device::moduleTextKey; variantModule / uberModule)
    struct ModuleTextKey {
        std::uint64_t a = 0, b = 0, meta = 0;
        bool operator==(const ModuleTextKey&) const = default;
    };
    struct ModuleTextKeyHash {
        std::size_t operator()(const ModuleTextKey& k) const { return static_cast<std::size_t>(k.a ^ (k.b * 0x9E3779B97F4A7C15ull) ^ k.meta); }
    };
    static ModuleTextKey moduleTextKey(shader::Stage stage, std::string_view text, bool half16);
    rhi::ShaderModule sharedModule(const ModuleTextKey& key);
    rhi::ShaderModule addSharedModule(const ModuleTextKey& key, rhi::ShaderModule m);
    std::mutex sharedModuleMutex_;
    std::unordered_map<ModuleTextKey, rhi::ShaderModule, ModuleTextKeyHash> sharedModules_;
    std::atomic<std::uint64_t> sharedModuleHits_{0}, sharedModuleMisses_{0};
    std::atomic<bool> shareModules_{false};

public:
    // S-4: variant module requests answered by a module of the same text (hits) / modules built (misses), cumulative
    std::uint64_t sharedModuleHits() const { return sharedModuleHits_; }
    std::uint64_t sharedModuleMisses() const { return sharedModuleMisses_; }

private:
    shader::abi::VertexFetch pullFetch_{};             // S-4: the draw's VertexFetch block (computeVertexInput; valid = 0: not pullable)
    bool vertexPull_ = false;
    VertexPullStats vertexPullStats_;
    std::uint64_t vertexPullActs_ = 0;
    bool sceneV2_ = false;
    struct SceneViewTable {
        rhi::GpuRef base;
        const shader::abi::ViewBlock2* shadow = nullptr;
        std::uint32_t slots = 0;
    } sceneViews_;
    static constexpr std::uint32_t kSceneRefSlots = 16;  // ref streams of the v2 view slots 0..15 (a larger slot is refused)
    std::vector<std::uint32_t> sceneRecSlots_;  // parallel to sceneRecs_: 0 = a v1 record, else the v2 view slot + 1
    SceneView* probe_ = nullptr;                      // F2c-2: filled by toScenePacket
    shader::abi::DrawBlock probeBlock_{};              // the DrawBlock of the draw being prepared / replayed (a copy while the probe is set)
    std::uint64_t variantEpoch_ = 1;                   // bumped by every setter that changes a draw's variant / scene decision
    std::vector<shader::abi::DrawRecord> sceneRecs_;  // the batch's records (DrawPacket::scene - 1)
    std::uint32_t sceneRecGen_ = 1;                    // (F6-2: the list's generation, moved by every clear: a packet's sceneGen must be the current one at its run's flush)
    std::vector<std::uint32_t> sceneRun_, batchOrder_;
    struct SceneRefStream {
        std::uint64_t buffer = 0, offset = 0;
        bool valid = false;
    } sceneRef_;  // (i, 0) for i < kSceneRunMax: the instance-rate ref stream, a ring block of this frame
    SceneRefStream sceneRefV2_[kSceneRefSlots];  // scene ABI v2: (i, slot) per view slot, ring blocks of this frame
    std::uint64_t treeEnvHash_[8] = {}, treeEnvEpoch_[8] = {}, treeEnvCounter_ = 0;  // F4-3 (per tree view slot: 14 / 15 main z / solid, 8..11 the cascades' z: treeEnvIndex)
    static std::size_t treeEnvIndex(bool solid, std::uint32_t viewSlot) { return viewSlot >= 8 && viewSlot < 12 ? 2 + (viewSlot - 8) : viewSlot == 12 || viewSlot == 13 ? 6 + (viewSlot - 12) : (solid ? 1 : 0); }
    static constexpr std::size_t kOwnedEnvSlots = 8;
    struct OrderedEnv {  // F9 / F14: the recent (hash, epoch) pairs of an ordered segment's env index
        std::uint64_t hash[4] = {}, epoch[4] = {};
        std::uint32_t next = 0;
    } ownedEnvOrdered_[3];
    static std::size_t ownedEnvIndex(std::uint32_t viewSlot) { return viewSlot < kOwnedEnvSlots ? viewSlot : kOwnedEnvSlots - 1; }
    std::uint64_t ownedEnvHash_[kOwnedEnvSlots] = {}, ownedEnvEpoch_[kOwnedEnvSlots] = {}, ownedEnvCounter_ = 0;  // F2c-4a: ownedEnvEpoch per view slot (0 main z, 1 main solid, 4.. cascades; F3-1)
    std::vector<std::uint32_t> ownedBucketOf_, ownedBucketStart_, ownedBucketCursor_;   // ownedFlush's scratch
    std::vector<std::uint64_t> ownedBucketKeys_;
    VertexInputValues sceneViWork_;
    const VertexInputValues* lastSceneVi_ = nullptr;
    SceneDrawStats sceneStats_;
    bool sceneCheckFrame_ = false;
    struct SceneCheck {
        std::unique_ptr<rhi::Buffer> buffer;
        std::uint64_t bytes = 0, capacity = 0, frameValue = 0;
        bool busy = false;
        std::vector<std::uint8_t> expect;  // what the CPU wrote, at the offsets of the copies into `buffer`
        std::vector<std::pair<std::uint64_t, std::uint64_t>> segments;  // the copied (offset, size) ranges: the padding between them is not compared
    } sceneCheck_;
    double sceneLastReport_ = 0.0;
    SceneDrawStats sceneLast_;
    DrawPacket packet_;
    VertexInputValues workVi_;   // what prepareDraw computes for the draw (packet_.vi points at its interned copy)
    DynamicStateValues workDyn_;
    // blocks of a frame's packets that live until the frame's recording is done (pointers stay valid: pages never move)
    template <class T>
    class PagedArena {
    public:
        T* push(const T& v) {
            if (used_ == pages_.size() * kPage) pages_.push_back(std::make_unique<T[]>(kPage));
            T* p = &pages_[used_ / kPage][used_ % kPage];
            *p = v;
            ++used_;
            return p;
        }
        void clear() { used_ = 0; }

    private:
        static constexpr std::size_t kPage = 64;
        std::vector<std::unique_ptr<T[]>> pages_;
        std::size_t used_ = 0;
    };
    PagedArena<VertexInputValues> viArena_;
    PagedArena<DynamicStateValues> dynArena_;
    const VertexInputValues* lastVi_ = nullptr;
    const DynamicStateValues* lastDyn_ = nullptr;
    const VertexInputValues* internVi(const VertexInputValues& v);
    const DynamicStateValues* internDyn(const DynamicStateValues& v);
    std::vector<DrawPacket> batch_;
    struct RankEntry {
        GamePipeline pipeline = GamePipeline::None;
        std::uint32_t rank = 0;
    };
    std::vector<RankEntry> rankTable_;       // flushBatch: pipeline -> index of its first draw
    std::vector<std::uint64_t> batchKeys_;   // flushBatch: (rank << 32 | draw index), sorted
    bool batching_ = true;  // debug.batch_draws
    bool constCompare_ = true;  // debug.constant_compare
    bool splitConstants_ = true;  // debug.split_constants
    bool splitVerify_ = false;    // debug.split_verify
    bool reverseZ_ = true;  // debug.reverse_z
    DepthZ depthZ_ = DepthZ::Off;  // native.depth_z (S-6)
    DepthZStats depthZStats_;
    std::uint64_t depthZActs_ = 0;
    bool depthMapDropped_ = false;
    bool psDropped_ = false;  // S-6: the draw being prepared leaves its pixel shader out (scene_draws.cpp reads it)
    mutable float reverseK_ = 0.99f;  // the last k computed from a valid camera
    // bindless sampler index per slot (UINT32_MAX = look it up again after a SetSamplerState)
    std::array<std::uint32_t, kSamplerCount> samplerSlotCache_{};
    // O2b: the last two state rows per slot and their samplers (the native composition's per-op device restore flips a stage
    // between the engine's states and a composed draw's: both stay found without the hashed, locked lookup)
    struct SamplerMemo {
        std::array<std::uint32_t, SAMP_COUNT> states{};
        std::uint32_t index = UINT32_MAX;
    };
    std::array<std::array<SamplerMemo, 2>, kSamplerCount> samplerMemo_{};
    std::uint32_t instanceCount_ = 1;
    FrameStats stats_;

    // [debug] perf
    struct DrawTimer;
    void gpuMark(const char* what);
    PerfStats perf_;
    std::uint64_t perfPipelinesBase_ = 0;
    double perfPipelineMsBase_ = 0.0;
    std::uint64_t perfLibrariesBase_ = 0, perfOptimizedBase_ = 0, perfPrewarmedBase_ = 0;
    double perfPrewarmMsBase_ = 0.0;
    double perfLibraryMsBase_ = 0.0, perfOptimizeMsBase_ = 0.0;
    std::atomic<std::uint64_t> shadersBuilt_{0};
    std::atomic<std::uint64_t> shadersCompiled_{0};
    std::atomic<std::uint64_t> shaderNs_{0};
    std::uint64_t perfShadersBase_ = 0, perfShadersCompiledBase_ = 0, perfShaderNsBase_ = 0;
    std::chrono::steady_clock::time_point presentEnd_{};
    std::chrono::steady_clock::time_point presentStart_{};
    float frameSlotWaitMs_ = 0.0f;  // this frame's slot wait (beginFrame), for PerfStats::frameSplits
    std::chrono::steady_clock::time_point lastPresentStart_{};
    std::chrono::steady_clock::time_point pipelinesSaved_{};
    std::uint64_t ringBytesBase_ = 0;
    std::uint32_t passIndex_ = 0;   // render passes of the current submission (GPU detail labels)
    std::uint32_t passDraws_ = 0;   // draws in the open render pass
    std::uint64_t gpuDetailFrame_[rhi::kFramesInFlight]{};  // frame number whose details a slot records (0 = none)

    // frame semantics
    scene::PhaseTracker phases_;
    scene::CameraBlock camera_;
    scene::CameraBlock lastCamera_;
    std::array<Texture*, static_cast<std::size_t>(scene::ImageRole::Count)> roles_{};
    std::uint64_t skyCubeFaces_ = 0;
    bool skyCubeEveryFrame_ = false;
    std::uint32_t roleLogs_ = 0;
    std::chrono::steady_clock::time_point createdAt_ = std::chrono::steady_clock::now();
    draws::Tag tag_;
    draws::FrameDraws draws_;
    draws::Record* lastRecord_ = nullptr;  // the record of the draw being issued (its index buffer is bound after prepareDraw)
    draws::TintMode tint_ = draws::TintMode::Off;
    float billboardFadeScale_ = 1.0f;
    std::unordered_set<std::uint64_t> waterLogged_;  // material x pixel shader pairs whose first water / refracted draw was logged
    std::vector<DrawReplacer*> replacers_;
    std::vector<DrawObserver*> observers_;
    DrawTap* drawTap_ = nullptr;
    RtSceneRef rtScene_;
    std::map<std::string, std::string> status_;
    std::uint32_t shadowDepthMaps_ = 0;
    std::uint32_t customVertices_ = 0;    // the draw prepared by prepareDraw is procedural: vkCmdDraw(customVertices_)
    PipelineDesc waterTemplate_{};    // pipeline description of the last water draw (prebuildReplacement)
    bool waterTemplateValid_ = false;
    std::uint32_t lookupSamplerStates(const std::uint32_t* states);
    bool replacementDesc(const Shader& vs, const Shader& ps, bool procedural, PipelineDesc& out) const;
    struct Registered {
        std::shared_ptr<Shader> vs, ps;
        bool procedural = false;
    };
    std::vector<Registered> registered_;

    // enhancements
    std::vector<std::uint8_t> giHeader_;
    GbufferTargets gbuf_;
    bool gbufArmed_ = false;
    bool scopeGbuf_ = false;                // the open rendering scope has the G-buffer attachments
    std::uint32_t engineColorCount_ = 0;    // the engine's colour attachments of the open scope (the G-buffer's come after)
    bool gbufWrite_ = false;                // the draw being prepared writes the G-buffer
    std::uint32_t ubyteInputs_ = 0;         // D3D12: the vertex inputs read from UBYTE4 attributes (integer bits: the translated vertex shader converts), push flags bits 17..31
    Wp0 wp0_;                               // WP0 spike counters (RVK_WP0=1; d3d9/wp0.h)
    void wp0Note(const PipelineDesc& pd, std::uint32_t primitiveType, bool indexed, bool captureDraw, bool giDraw);
    void wp0Log();
    std::uint64_t gbufWrittenFrame_ = 0;    // frame value whose main view cleared / wrote the G-buffer
    std::uint32_t gbufMissing_ = 0, gbufSolid_ = 0;  // this frame: solid draws whose G-buffer pipeline / variant is not ready, all
    double gbufRequestedAt_ = 0.0;
    std::array<std::uint32_t, 4> ao_{};
    std::array<std::uint32_t, 4> sgi_{};
    GiDraws giDraws_;
    PackProvider packProvider_;
    std::function<std::uint32_t()> packStamp_;
    bool packBypass_ = false;
    std::uint64_t giDrawCount_ = 0;
    bool captureInstances_ = false;         // the request covers an instanced draw (requestVertexCapture)
    rhi::GpuRef captureRequest_;            // the draw being prepared (DrawObserver::observe)
    rhi::GpuRef captureRequestRw_;          // ... the form the vertex shader writes through (D3D12: the UAV slot)
    std::vector<rhi::GpuRef> captured_;     // this frame's captures that ran
    std::vector<rhi::GpuRef> capturedPrev_;  // the previous frame's
    std::array<VariantProvider, static_cast<std::size_t>(VariantSlot::Count)> variantProviders_;
    struct VariantJob {
        std::shared_ptr<Shader> shader;
        VariantSlot slot;
    };
    std::mutex variantMutex_;
    std::condition_variable variantCv_, variantIdleCv_;
    int variantBusy_ = 0;  // jobs a worker is building now (variantMutex_); resetUberVariants waits for 0
    std::atomic<std::uint64_t> uberFallbacks_{0};  // S-1a: ubershader builds that failed and were taken by the translated shader
    std::unordered_set<std::uint64_t> uberFailSeen_;  // 14.22: (shader x slot) of the translated scene forms of covered requests, logged once
    int uberFailLogged_ = 0;
    std::atomic<std::uint64_t> uberLateSource_{0}, uberLateReset_{0};  // 14.22: variants rebuilt because the engine's request arrived during / after their build
    std::deque<VariantJob> variantJobs_;
    std::vector<std::thread> variantThreads_;  // 3 workers, started by the first request
    mutable std::mutex shaderJobMutex_;
    std::condition_variable shaderJobCv_;
    struct ShaderJob {
        Shader* shader = nullptr;
        std::function<void()> run;
    };
    std::deque<ShaderJob> shaderJobs_;
    std::vector<std::thread> shaderThreads_;  // the compile workers of translated shaders (started by the first one)
    bool shaderStop_ = false;
    bool variantStop_ = false;
    std::atomic<std::uint64_t> variantsBuilt_{0}, variantsFailed_{0};
    rhi::GpuRef frameAddr_;  // null = upload before the next use
    JitterSource jitterSource_;
    float geometryLodBias_ = 0.0f;
    bool alphaDither_ = false;
    float cutoutThicken_ = 0.0f;
    bool hashedAlphaLogged_ = false;
    std::unordered_set<const void*> hashedMaterials_;  // -RvkLogLevel trace: the materials drawn with the hashed test (64 at most)
    std::unordered_set<std::uint64_t> tracedMaterialDraws_;  // RVK_TRACE_MATERIAL (logTracedMaterial)
    std::unordered_set<const void*> tracedEntities_;
    bool regionsOn_ = false;
    bool markersOn_ = false;
    struct OpenRegion {
        const char* base = nullptr;  // the name as given (identity of literals: the phase names; compared with `index`)
        std::uint32_t index = kNoRegionIndex;
        char text[48] = {};  // the label: base [+ " #index"]
        bool marked = false;  // a crash marker was written at its start
    };
    static constexpr std::uint32_t kMaxRegions = 32;
    std::array<OpenRegion, kMaxRegions> regions_{};  // the labels open in the current command buffer (outermost first; [0] = the frame)
    std::uint32_t regionCount_ = 0;
    std::uint64_t regionsFrame_ = ~0ull;  // stats_.frames when regions_[0] began
    std::uint64_t phaseKey_ = 0;
    std::uint64_t phasesSeen_ = ~0ull;  // PhaseTracker::generation() at the last syncPhaseRegions
    void syncPhaseRegions();
    void regionsClose();
    void regionsReopen();
    float renderRatio_ = 1.0f;
    std::array<std::uint32_t, 2> displaySize_{};
    bool postAtDisplaySize_ = true;
    rhi::Texture* generated_ = nullptr;
    double baseIntervalMs_ = 0.0;  // smoothed present-to-present interval
    const Texture* substituteFrom_ = nullptr;
    std::uint32_t substituteSlot_ = 0;
    std::array<float, 4> jitter_{};  // this frame's jitter (pixels, xy), the previous frame's (zw)
    std::array<std::array<float, 4>, shader::abi::kFrameParams> frameParams_{};
    inject::Registry passes_;
    inject::Firings firings_;
    bool injecting_ = false;
    std::unique_ptr<rhi::GameDevice> game_;  // the backend seam (created with the context; rhi()): destroyed before frames_ (its retirements run there)

    // overlay
    PresentOverlay* overlay_ = nullptr;
    bool overlayIntoBackBuffer_ = false;
    void invalidateRecordingState();
    LiveFrame live_;
};

// RAII debug region (Device::regionBegin / regionEnd): no allocation, a few instructions when regions are off
class RegionScope {
public:
    RegionScope(Device& device, const char* name, std::uint32_t index = Device::kNoRegionIndex) : device_(device) {
        device_.regionBegin(name, index);
    }
    ~RegionScope() { device_.regionEnd(); }
    RegionScope(const RegionScope&) = delete;
    RegionScope& operator=(const RegionScope&) = delete;

private:
    Device& device_;
};

// what DrawObserver::observeOwned gets (F2c-4b): the owned draws of (part of) one pass in the engine's order
struct OwnedFrame {
    scene::Phase phase;
    const Device::OwnedRecordView* records = nullptr;
    std::size_t count = 0;
    const std::uint32_t* left = nullptr;  // ids that were in the pass kind's previous pass and are not in this one (only with `last`)
    std::size_t leftCount = 0;
    bool last = false;                    // the end of the pass (the `left` list is complete)
    std::uint32_t observerBit = 0;        // this observer's bit in OwnedRecordView::wanted
};

}  // namespace rvk::d3d9
