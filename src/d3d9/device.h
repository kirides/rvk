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
#include <mutex>
#include <span>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "d3d9/defs.h"
#include "d3d9/draws.h"
#include "d3d9/identity.h"
#include "d3d9/inject.h"
#include "d3d9/scene.h"
#include "d3d9/semantics.h"
#include "shader/abi.h"
#include "shader/glsl.h"
#include "vk/bindless.h"
#include "vk/context.h"
#include "vk/formats.h"
#include "vk/frames.h"
#include "vk/gputimer.h"
#include "vk/image.h"
#include "vk/pacer.h"
#include "vk/passes.h"
#include "vk/pipeline.h"
#include "vk/presenter.h"
#include "vk/swapchain.h"

struct HWND__;

namespace rvk::d3d9 {

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
    const vk::FormatInfo& format() const { return fmt_; }
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
    vk::Image& image() { return image_; }
    std::uint32_t bindlessSlot() const { return slot_; }
    vk::TextureKind kind() const { return kind_; }
    bool prepareForSampling(VkCommandBuffer cmd);  // uploads pending data, transitions to shader-read; false if nothing to sample
    // what the engine uses the image for (Device::tagImage; render thread)
    scene::ImageRole role() const { return role_; }
    // persistent identity (d3d9/identity.h): the generation counts uploads of CPU data
    ResourceRef ref() const { return {id_, generation_.load(std::memory_order_relaxed)}; }

    // bytes held in CPU copies of all textures (diagnostics)
    static std::uint64_t cpuBytesTotal();

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
    void upload(VkCommandBuffer cmd);

    Device& dev_;
    TextureDesc desc_;
    vk::FormatInfo fmt_{};
    bool valid_ = false;
    bool isNull_ = false;
    vk::Image image_;
    vk::TextureKind kind_ = vk::TextureKind::Tex2D;
    std::uint32_t slot_ = 0;
    std::mutex mutex_;
    std::vector<Level> cpu_;            // [face * levels + level], allocated on demand
    std::atomic<bool> pending_{false};  // some level is dirty
    bool lockedOnce_ = false;           // ever locked: keep the CPU copies (D3D9 managed semantics)
    std::atomic<bool> gpuWritten_{false};  // a blit destination (Device::stretchRect): its image holds data the CPU copy lacks
    bool restoreWarned_ = false;
    std::uint64_t lastUseFrame_ = 0;
    std::vector<std::unique_ptr<Surface>> surfaces_;
    scene::ImageRole role_ = scene::ImageRole::None;
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
        VkBuffer buffer = VK_NULL_HANDLE;
        VkDeviceSize offset = 0;
    };
    Binding gpu();
    // render thread, after gpu(): the device address of the static GPU copy (ray tracing tier only: BLAS inputs, vertex
    // fetches of RT hit shaders; docs/research/rt-reflections.md 3); 0 for ring-copied buffers or without the tier. Valid
    // until the next generation (Device::notifyBufferChanged tells the observers)
    VkDeviceAddress deviceAddress();

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
    std::uint8_t* boundPtr_ = nullptr;  // CPU address of the frame's ring copy (dynamic buffers)
    VkBuffer static_ = VK_NULL_HANDLE;
    VmaAllocation staticAlloc_ = VK_NULL_HANDLE;
    VkDeviceAddress staticAddress_ = 0;  // of static_ (lazy)
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

private:
    std::vector<VertexElement> elements_;
    bool blendIndices_ = false;
    bool texcoord4_ = false;
};

class Shader {
public:
    shader::Stage stage = shader::Stage::Vertex;
    VkShaderModule module = VK_NULL_HANDLE;
    shader::Reflection reflection;
    std::vector<std::uint32_t> bytecode;  // the D3D9 tokens (kept for GetFunctionData)
    std::uint64_t hash = 0;
    ShaderSemantics semantics;  // named constants / samplers from the CTAB (d3d9/semantics.h)
    // pixel shaders: the tint debug view's variant (GlslOptions::tintEpilogue), built on first use (render thread)
    VkShaderModule tintModule = VK_NULL_HANDLE;
    bool tintFailed = false;
    // vertex shaders of SpeedTree billboards: the fade distance variant (Device::lodModule, engine/lod.h)
    VkShaderModule lodModule = VK_NULL_HANDLE;
    bool lodFailed = false;
};

// ---- device -------------------------------------------------------------------------------------------------------

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
    bool instanced = false;                   // stream 0 frequency INDEXEDDATA
    std::uint32_t instances = 1;              // its instance count (1 when not instanced)
    Texture* const* textures = nullptr;       // the bound textures s0..s15 (pixel)
    const Shader* engineVs = nullptr;
    const Shader* enginePs = nullptr;
    std::shared_ptr<Shader> engineVsRef;      // the engine's vertex shader (a replacement may keep it: DrawReplacement::vs)
    const VertexDecl* decl = nullptr;         // the draw's vertex declaration
    bool blended = false;                     // alpha blending SRCALPHA / INVSRCALPHA on
    bool blending = false;                    // alpha blending on (any factors)
    std::uint8_t srcBlend = 2, dstBlend = 1;  // D3DBLEND of the colour (when blending)
    bool alphaTest = false;                   // alpha test on with a function other than ALWAYS
    std::uint32_t alphaFunc = 8;              // D3DCMPFUNC of the alpha test (8 = always)
    std::uint32_t alphaRef = 0;               // its reference 0..255
    bool depthWrite = true;                   // z test + z write on (a depth target bound)
    bool depthBias = false;                   // a depth bias (constant or slope-scaled) is set
};

struct DrawReplacement {
    enum class Action : std::uint8_t {
        Engine,      // draw it as the engine asked
        Skip,        // not drawn (still counted and recorded, flagged replaced)
        Shaders,     // the engine's geometry and constants with `vs` / `ps`
        Procedural,  // `vertices` vertices of `vs` / `ps` without vertex / index buffers (vkCmdDraw)
    };
    Action action = Action::Engine;
    std::shared_ptr<Shader> vs, ps;     // reflections decide vertex inputs and constant uploads (all of c# when 256)
    VkDeviceAddress block = 0;          // replaces the DrawBlock address (push constant `draw`); 0 = keep
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

// Draw observation (enh/rt_scene: the ray tracing scene, docs/research/rt-reflections.md 3): an enhancement that reads the
// engine's geometry and per-draw constants (world rows in the vertex shader constants) without changing anything. Indexed
// draws from buffers of the views / kinds it observes are shown to it in prepareDraw, before the draw is recorded (the
// constants and the bound state are the draw's). Render thread.
class DrawObserver {
public:
    virtual ~DrawObserver() = default;
    // whether a draw of this kind in this phase is observed (called for every indexed draw while registered: keep it cheap)
    virtual bool observes(draws::Kind kind, scene::Phase phase) const = 0;
    // must not change device state (Device::vsConstant, streamSource, Buffer::gpu / readCpu / deviceAddress are fine)
    virtual void observe(Device& device, const ReplaceQuery& query) = 0;
    // the buffer `id` got a new static GPU copy (a new generation: the old VkBuffer and its address are retired) or is
    // released (its GPU copy dies after the frame): anything built from it must be forgotten
    virtual void bufferChanged(std::uint64_t /*bufferId*/) {}
    // the texture is about to be destroyed (render thread, Device::processReleases): pointers to it must be forgotten
    virtual void textureReleased(const Texture* /*texture*/) {}
};

// The ray tracing scene of the frame, for the FrameBlock (shader/abi.h FrameBlock::rtScene / rtInfo): valid only in the
// submission that built it (`frame` = Device::frameValue() at the build; a mid-frame flush starts another one)
struct RtSceneRef {
    VkDeviceAddress tlas = 0;
    VkDeviceAddress records = 0;
    std::uint32_t instances = 0;
    std::uint32_t recordCount = 0;
    std::uint64_t frame = 0;
};

// Drawn on top of the frame at present (the in-game overlay, src/overlay): after the back buffer was copied into the
// swapchain image, so captures of the back buffer (rvk_screenshot, the engine snapshot) never contain it; or, for debugging,
// into the back buffer itself (Device::drawOverlayIntoBackBuffer). Render thread.
class PresentOverlay {
public:
    virtual ~PresentOverlay() = default;
    // anything to draw this frame? false leaves the present path untouched
    virtual bool active() = 0;
    // records draws inside a dynamic rendering scope of one colour attachment of `extent` (LOAD / STORE; viewport, scissor,
    // pipeline and descriptor sets are the overlay's own)
    virtual void record(VkCommandBuffer cmd, VkExtent2D extent) = 0;
};

// What an injected pass gets (Device::inject, d3d9/inject.h; render thread). `cmd` is the frame's command buffer outside any
// rendering scope; the pass records its own barriers (vk::Image::transition, Device::readable), binds its own pipeline,
// descriptor sets and push constants, and may begin / end its own rendering scopes. It must not change the D3D9 state.
struct PassContext {
    Device& device;
    VkCommandBuffer cmd;
    inject::Point point;
    std::uint32_t mainView = 0;      // 1 = the frame's first main view, 2 = the second, ...; 0 for BeforeGui
    VkDeviceAddress frameBlock = 0;  // shader::abi::FrameBlock of this frame (RVK_PASS_PUSH_HEADER's frame)
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
    bool validation = false;
    bool syncValidation = false;  // + synchronization validation ([vulkan] sync_validation)
    int adapter = -1;
    std::filesystem::path cacheDir;  // pipeline + shader caches; empty = no disk cache
    bool offscreen = false;          // no swapchain (tests)
    std::uint32_t logDraws = 0;      // log the first N draws in detail
    bool perf = false;               // [debug] perf: GPU timestamps + CPU timings (Device::takePerf)
    bool pipelineLibraries = false;  // [vulkan] pipeline_libraries (off: AMD driver crash, vk/pipeline_library.cpp)
    std::uint32_t perfFrame = 0;     // [debug] perf_frame: log the GPU time of every pass of this frame
    bool allowRayTracing = true;     // rt.allow: enable the ray tracing tier when the GPU has it (vk::ContextOptions)
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
    std::uint32_t lastFrameDraws = 0;
    std::uint64_t acquireFailures = 0;
    std::uint64_t loadingFramesDropped = 0;  // loading-screen frames rendered but not shown (setUnpaced)
    std::uint64_t presentsQueued = 0;
    std::uint64_t presentSuboptimal = 0;
    std::uint64_t swapchainRecreates = 0;
    double acquireMs = 0.0;  // summed time in vkAcquireNextImageKHR / vkQueuePresentKHR (per stats interval)
    double presentMs = 0.0;  // on the present thread
    double presentWaitMs = 0.0;  // the render thread waiting for the present thread before it acquires
    double pacerMs = 0.0;  // summed time the frame pacer slept
    // cumulative since creation
    std::uint64_t totalDraws = 0;
    std::uint64_t skippedNoShader = 0;
    std::uint64_t skippedNoTarget = 0;
    std::uint64_t skippedVertexInput = 0;
    std::uint64_t skippedPipeline = 0;
    std::uint64_t totalClears = 0;
    std::uint64_t totalRenderPasses = 0;
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
    std::uint64_t frameBlocks = 0;         // FrameBlock uploads
    std::vector<float> frameIntervalsMs;   // present-to-present
    vk::FramePacer::Stats pacer;
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
    void setVertexShader(std::shared_ptr<Shader> shader);
    void setPixelShader(std::shared_ptr<Shader> shader);
    const Shader* vertexShader() const { return vs_.get(); }
    const Shader* pixelShader() const { return ps_.get(); }
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
    bool readImageLevel(vk::Image& img, std::uint32_t level, std::uint32_t layer, VkExtent3D extent, VkDeviceSize size,
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
    // present-rate cap (0 = the refresh rate with vsync, none without); applies at the next present
    void setMaxFps(double fps);
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
    bool fullScreenExclusiveHeld() const { return swapchain_.fullScreenExclusiveHeld(); }
    bool fullScreenExclusiveConfigured() const { return swapchain_.fullScreenExclusiveConfigured(); }
    VkPresentModeKHR presentMode() const { return swapchain_.presentMode(); }
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
    const vk::Swapchain& swapchain() const { return swapchain_; }
    // the last frame's numbers for the overlay's performance panel (cheap, always on; GPU only with [debug] perf)
    struct LiveFrame {
        float intervalMs = 0.0f;  // present to present
        float cpuMs = 0.0f;       // engine + HAL: end of one present() to the start of the next
        float gpuMs = -1.0f;      // GPU time of the latest measured submission (-1: no GPU timer)
    };
    const LiveFrame& liveFrame() const { return live_; }
    Surface* backBuffer() { return backBufferSurface_; }
    Surface* autoDepthStencil() { return depthSurface_; }
    const FrameStats& stats() const { return stats_; }
    // [debug] perf: the numbers since the last call (zeroed afterwards)
    PerfStats takePerf();
    bool perfEnabled() const { return options_.perf; }
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
    void tagImage(Texture* texture, scene::ImageRole role);
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
    void setTintMode(draws::TintMode mode) { tint_ = mode; }
    draws::TintMode tintMode() const { return tint_; }
    // detail distances (engine/lod.h lod.trees): the SpeedTree billboards' fade-in distances times `scale` (1 = the engine's
    // vertex shader unchanged; else its fade variant, FrameBlock params[kFrameParamLod].x)
    void setBillboardFadeScale(float scale);

    // ---- enhancements: frame block, injection points, transient images (docs/research/injection.md; render thread) ----
    // the FrameBlock of the frame being recorded (shader/abi.h): uploaded into the frame's ring on first use and again after
    // the camera, an image role or a feature parameter changed; every draw's push constants carry it
    VkDeviceAddress frameBlockAddress();
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
    vk::Transients& transients() { return transients_; }
    // draw replacement (enh/water, enh/shadows): a replacer must outlive its registration; adding twice is a no-op
    void addDrawReplacer(DrawReplacer* replacer);
    void removeDrawReplacer(DrawReplacer* replacer);
    // draw observation (enh/rt_scene): an observer must outlive its registration; adding twice is a no-op
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
        frameAddr_ = 0;
    }
    const RtSceneRef& rtScene() const { return rtScene_; }
    bool rtSceneValid() const { return rtScene_.tlas != 0 && rtScene_.frame == frameValue(); }
    // one-line status texts of enhancements for the overlay's performance panel (key -> text; empty text removes it)
    void setStatus(const std::string& key, std::string text) {
        if (text.empty()) status_.erase(key);
        else status_[key] = std::move(text);
    }
    const std::map<std::string, std::string>& status() const { return status_; }
    // the sun cascades' depth maps rendered so far this frame (the scene hooks count each ExecuteShadowMap into a DF24 /
    // DF16 depth texture; enh/shadows knows the frame's last cascade by it)
    void countShadowDepthMap() { ++shadowDepthMaps_; }
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
    bool readable(Texture* texture, VkCommandBuffer cmd);

    // internals used by resources
    vk::Context& ctx() { return ctx_; }
    vk::Frames& frames() { return frames_; }
    vk::Bindless& bindless() { return bindless_; }
    std::uint64_t frameValue() const { return frames_.currentValue(); }
    VkFormat d24s8Format() const { return d24s8_; }
    bool recordingThread() const;
    // the frame's upload command buffer (submitted before the frame's draws); render thread
    VkCommandBuffer uploadCommands();

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
    void submitFrame(VkSemaphore waitAcquire, VkSemaphore signalRendered);
    void processReleases();
    bool prepareTextures();
    void endRendering();
    bool beginRendering();
    // the geometry of a draw (for its record)
    struct DrawGeom {
        bool indexed = false;
        bool userPointer = false;
        std::uint32_t first = 0;
        std::uint32_t primitives = 0;
        std::int32_t baseVertex = 0;
    };
    bool prepareDraw(std::uint32_t primitiveType, const DrawGeom& geom);
    void logWaterDraw(draws::Kind kind);
    VkShaderModule tintModule(Shader& ps);  // the tint variant of a pixel shader (null when it fails to build)
    VkShaderModule lodModule(Shader& vs);   // the billboard fade variant of a vertex shader (null when it fails to build)
    VkShaderModule variantModule(const Shader& s, const shader::GlslOptions& options, std::string_view tag);
    bool bindVertexInput(vk::PipelineDesc& pd);
    void setDynamicState(std::uint32_t primitiveType);
    std::uint32_t samplerIndex(std::uint32_t slot);
    std::uint32_t lookupSampler(std::uint32_t slot);
    void uploadConstants();
    void blit(vk::Image& src, std::uint32_t srcLevel, std::uint32_t srcLayer, const VkOffset3D srcBox[2], vk::Image& dst,
              std::uint32_t dstLevel, std::uint32_t dstLayer, const VkOffset3D dstBox[2], VkFilter filter);
    static std::uint32_t slotOf(std::uint32_t d3dSampler);

    DeviceOptions options_;
    vk::Context ctx_;
    vk::Frames frames_;
    vk::Bindless bindless_;
    vk::PipelineCache pipelines_;
    vk::Swapchain swapchain_;
    vk::Presenter presenter_;
    void recreateSwapchain();
    vk::FramePacer pacer_;
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
    VkSemaphore acquireSemaphores_[vk::Frames::kInFlight]{};

    // shader cache
    std::mutex shaderMutex_;
    std::unordered_map<std::uint64_t, std::shared_ptr<Shader>> shaders_;
    std::unordered_map<VkShaderModule, std::uint64_t> moduleHashes_;

    // Pipelines of earlier runs (<cacheDir>/pipelines.keys): the shader hashes + the rest of the description. When both
    // shaders of a known pipeline exist (they are created while the game loads), it is built in the background, so its
    // first draw does not stall. New pipelines are appended and the file is rewritten every 10 s.
    struct PipelineKey {
        std::uint64_t vs = 0;
        std::uint64_t ps = 0;
        vk::PipelineDesc desc{};  // vs / fs = VK_NULL_HANDLE
    };
    void loadPipelineKeys();
    void savePipelineKeys();
    void recordPipelineKey(const vk::PipelineDesc& pd);
    void prewarmPipelines(std::uint64_t shaderHash);
    std::mutex keysMutex_;
    std::vector<PipelineKey> keys_;
    std::unordered_multimap<std::uint64_t, std::size_t> pendingKeys_;  // shader hash -> index into keys_, not yet built
    std::unordered_set<std::string> keySet_;                             // raw bytes of every key in keys_
    bool keysDirty_ = false;
    std::thread keySaver_;

    // samplers
    std::mutex samplerMutex_;
    std::unordered_map<SamplerKey, std::uint32_t, SamplerKeyHash> samplers_;
    std::vector<VkSampler> samplerObjects_;

    // null vertex stream (inputs the declaration does not provide read (0,0,0,1))
    VkBuffer nullVertexBuffer_ = VK_NULL_HANDLE;
    VmaAllocation nullVertexAlloc_ = VK_NULL_HANDLE;

    // D3D9 state
    std::array<std::uint32_t, RS_COUNT> rs_{};
    std::array<std::array<std::uint32_t, SAMP_COUNT>, kSamplerCount> samp_{};
    std::array<Texture*, kSamplerCount> tex_{};
    std::array<Stream, kStreams> streams_{};
    Buffer* indices_ = nullptr;
    VertexDecl* decl_ = nullptr;
    std::shared_ptr<Shader> vs_;
    std::shared_ptr<Shader> ps_;
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
        VkBuffer buffer = VK_NULL_HANDLE;
        VkDeviceSize offset = 0;
        std::uint32_t stride = 0;
    } up_;

    // recording state
    bool rendering_ = false;
    bool uploadUsed_ = false;
    std::uint32_t colorCount_ = 0;
    VkFormat colorFormats_[kRenderTargets]{};
    VkFormat depthFormat_ = VK_FORMAT_UNDEFINED;
    VkFormat stencilFormat_ = VK_FORMAT_UNDEFINED;
    std::uint32_t targetWidth_ = 0;
    std::uint32_t targetHeight_ = 0;
    bool vsConstDirty_ = true;
    bool psConstDirty_ = true;
    VkDeviceAddress vsConstAddr_ = 0;
    VkDeviceAddress psConstAddr_ = 0;
    VkPipeline boundPipeline_ = VK_NULL_HANDLE;
    // redundant-command filters, valid within one command buffer (reset by beginFrame)
    vk::PipelineDesc lastPipelineDesc_{};
    VkPipeline lastPipeline_ = VK_NULL_HANDLE;
    bool lastPipelineValid_ = false;
    struct VertexInputCache {
        bool buffersValid = false;
        bool layoutValid = false;
        std::uint32_t bufferCount = 0;
        VkBuffer buffers[vk::kMaxVertexBindings]{};
        VkDeviceSize offsets[vk::kMaxVertexBindings]{};
        std::uint32_t bindingCount = 0;
        std::uint32_t attributeCount = 0;
        VkVertexInputBindingDescription2EXT bindings[vk::kMaxVertexBindings]{};
        VkVertexInputAttributeDescription2EXT attributes[vk::kMaxVertexAttributes]{};
    } viCache_;
    // the dynamic state last recorded (every pipeline declares the same dynamic states, so it survives pipeline binds)
    struct DynamicStateCache {
        bool valid = false;
        VkViewport viewport{};
        VkRect2D scissor{};
        VkCullModeFlags cull = 0;
        VkPrimitiveTopology topology{};
        VkBool32 depthTest = 0;
        VkBool32 depthWrite = 0;
        VkCompareOp depthCompare{};
        VkBool32 stencilTest = 0;
        VkStencilOp stencilFront[3]{};
        VkCompareOp stencilFrontCompare{};
        VkStencilOp stencilBack[3]{};
        VkCompareOp stencilBackCompare{};
        std::uint32_t stencilMasks[3]{};  // compare, write, reference
        VkBool32 depthBiasEnable = 0;
        float depthBias[2]{};             // constant, slope
        float blendConstants[4]{};
        std::uint32_t colorCount = 0;
        VkBool32 blendEnable[kRenderTargets]{};
        VkColorBlendEquationEXT blendEquation[kRenderTargets]{};
        VkColorComponentFlags writeMask[kRenderTargets]{};
    } dyn_;
    // bindless sampler index per slot (UINT32_MAX = look it up again after a SetSamplerState)
    std::array<std::uint32_t, kSamplerCount> samplerSlotCache_{};
    std::uint32_t instanceCount_ = 1;
    FrameStats stats_;

    // [debug] perf
    struct DrawTimer;
    void gpuMark(const char* what);
    vk::GpuTimer gpuTimer_;
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
    std::chrono::steady_clock::time_point lastPresentStart_{};
    std::chrono::steady_clock::time_point pipelinesSaved_{};
    std::uint64_t ringBytesBase_ = 0;
    std::uint32_t passIndex_ = 0;   // render passes of the current submission (GPU detail labels)
    std::uint32_t passDraws_ = 0;   // draws in the open render pass
    std::uint64_t gpuDetailFrame_[vk::Frames::kInFlight]{};  // frame number whose details a slot records (0 = none)

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
    RtSceneRef rtScene_;
    std::map<std::string, std::string> status_;
    std::uint32_t shadowDepthMaps_ = 0;
    std::uint32_t customVertices_ = 0;    // the draw prepared by prepareDraw is procedural: vkCmdDraw(customVertices_)
    vk::PipelineDesc waterTemplate_{};    // pipeline description of the last water draw (prebuildReplacement)
    bool waterTemplateValid_ = false;
    std::uint32_t lookupSamplerStates(const std::uint32_t* states);
    bool replacementDesc(const Shader& vs, const Shader& ps, bool procedural, vk::PipelineDesc& out) const;
    struct Registered {
        std::shared_ptr<Shader> vs, ps;
        bool procedural = false;
    };
    std::vector<Registered> registered_;

    // enhancements
    VkDeviceAddress frameAddr_ = 0;  // 0 = upload before the next use
    std::array<std::array<float, 4>, shader::abi::kFrameParams> frameParams_{};
    inject::Registry passes_;
    inject::Firings firings_;
    bool injecting_ = false;
    vk::Transients transients_;

    // overlay
    PresentOverlay* overlay_ = nullptr;
    bool overlayIntoBackBuffer_ = false;
    void invalidateRecordingState();
    LiveFrame live_;
};

}  // namespace rvk::d3d9
