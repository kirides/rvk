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
#include <cstdint>
#include <filesystem>
#include <memory>
#include <mutex>
#include <span>
#include <unordered_map>
#include <vector>

#include "d3d9/defs.h"
#include "shader/glsl.h"
#include "vk/bindless.h"
#include "vk/context.h"
#include "vk/formats.h"
#include "vk/frames.h"
#include "vk/image.h"
#include "vk/pipeline.h"
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

    // D3D9 LockRect / LockBox: a pointer into the CPU copy of the level (D3D9 pitch); rect = {left, top, right, bottom}
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

private:
    friend class Device;
    struct Level {
        std::vector<std::uint8_t> data;  // D3D9 layout
        std::uint32_t rowPitch = 0;
        std::uint32_t slicePitch = 0;
        bool dirty = false;
    };
    Level& cpuLevel(std::uint32_t level, std::uint32_t face);
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
    std::uint64_t lastUseFrame_ = 0;
    std::vector<std::unique_ptr<Surface>> surfaces_;
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

    // D3D9 Lock: pointer into the CPU copy (offset/size 0 = whole buffer); flags LOCK_*
    void* lock(std::uint32_t offset, std::uint32_t size, std::uint32_t flags);
    void unlock();

    // render thread: the GPU copy for a draw (uploads / renames as needed)
    struct Binding {
        VkBuffer buffer = VK_NULL_HANDLE;
        VkDeviceSize offset = 0;
    };
    Binding gpu();

private:
    Device& dev_;
    std::uint32_t size_;
    std::uint32_t usage_;
    std::uint32_t pool_;
    bool index_;
    bool index32_;
    std::vector<std::uint8_t> cpu_;
    std::mutex mutex_;
    bool dirty_ = true;
    std::uint64_t gpuFrame_ = 0;  // frame of the ring copy (dynamic buffers)
    Binding binding_;
    std::uint8_t* boundPtr_ = nullptr;  // CPU address of the frame's ring copy (dynamic buffers)
    VkBuffer static_ = VK_NULL_HANDLE;
    VmaAllocation staticAlloc_ = VK_NULL_HANDLE;
};

class VertexDecl {
public:
    explicit VertexDecl(std::span<const VertexElement> elements);
    const std::vector<VertexElement>& elements() const { return elements_; }

private:
    std::vector<VertexElement> elements_;
};

class Shader {
public:
    shader::Stage stage = shader::Stage::Vertex;
    VkShaderModule module = VK_NULL_HANDLE;
    shader::Reflection reflection;
    std::vector<std::uint32_t> bytecode;  // the D3D9 tokens (kept for GetFunctionData)
    std::uint64_t hash = 0;
};

// ---- device -------------------------------------------------------------------------------------------------------

struct DeviceOptions {
    HWND__* window = nullptr;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    bool vsync = true;
    std::uint32_t depthFormat = 0;  // D3DFORMAT of the auto depth stencil, 0 = none
    bool validation = false;
    int adapter = -1;
    std::filesystem::path cacheDir;  // pipeline + shader caches; empty = no disk cache
    bool offscreen = false;          // no swapchain (tests)
    std::uint32_t logDraws = 0;      // log the first N draws in detail
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
    // cumulative since creation
    std::uint64_t totalDraws = 0;
    std::uint64_t skippedNoShader = 0;
    std::uint64_t skippedNoTarget = 0;
    std::uint64_t skippedVertexInput = 0;
    std::uint64_t skippedPipeline = 0;
    std::uint64_t totalClears = 0;
    std::uint64_t totalRenderPasses = 0;
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
    // read the back buffer as tightly packed BGRA8 (waits for the GPU)
    bool readBackBuffer(std::vector<std::uint8_t>& bgra, std::uint32_t& width, std::uint32_t& height);

    // frame
    void present();
    bool resize(std::uint32_t width, std::uint32_t height);
    Surface* backBuffer() { return backBufferSurface_; }
    Surface* autoDepthStencil() { return depthSurface_; }
    const FrameStats& stats() const { return stats_; }

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
    bool prepareDraw(std::uint32_t primitiveType);
    bool bindVertexInput(vk::PipelineDesc& pd);
    void setDynamicState(std::uint32_t primitiveType);
    std::uint32_t samplerIndex(std::uint32_t slot);
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
    std::int32_t vsI_[16][4]{};
    std::int32_t psI_[16][4]{};
    std::uint32_t vsB_[16]{};
    std::uint32_t psB_[16]{};
    std::array<Surface*, kRenderTargets> rt_{};
    Surface* ds_ = nullptr;
    Viewport viewport_;
    std::int32_t scissor_[4]{};

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
    std::uint32_t instanceCount_ = 1;
    FrameStats stats_;
};

}  // namespace rvk::d3d9
