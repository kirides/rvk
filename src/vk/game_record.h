#pragma once

// The Vulkan game device's draw-path recording: the D3D9 model's draw packets, chunks and rendering scopes (rhi::DrawPacket
// / rhi::DrawChunk / rhi::RenderingInfo in rhi/game_device.h) recorded to Vulkan: pipeline binds, vertex input (VK_EXT_vertex_input_dynamic_state),
// dynamic state (VK_EXT_extended_dynamic_state3), push constants, draws. The opaque passes' scopes begin with
// VK_RENDERING_CONTENTS_SECONDARY_COMMAND_BUFFERS_BIT: their chunks are recorded on two worker threads (plus the engine
// thread's own pools, work stealing) into secondary command buffers while the engine thread goes on, and executed in
// order at the scope's end (docs/research/performance.md 12 + 14). The pipeline cache (with VK_EXT_graphics_pipeline_library
// and the saved VkPipelineCache blob) lives here too: the game device owns it.

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <memory>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <vector>

#include "vk/pipeline.h"
#include "vk/frames.h"
#include "vk/bindless.h"
#include "vk/context.h"
#include "vk/vk.h"

namespace rvk::vk {

// GamePipeline <-> VkPipeline (the handle value is the VkPipeline; rhi/game_device.h)
inline VkPipeline vkPipeline(rhi::GamePipeline p) { return reinterpret_cast<VkPipeline>(static_cast<std::uint64_t>(p)); }
inline rhi::GamePipeline rhiPipeline(VkPipeline p) { return static_cast<rhi::GamePipeline>(reinterpret_cast<std::uint64_t>(p)); }

// What one command buffer being recorded has already set (draw recording skips redundant commands): the frame list's
// inline path (rec_) and one per secondary command buffer of a record chunk
struct GameVI {
    bool buffersValid = false;
    bool layoutValid = false;
    std::uint32_t bufferCount = 0;
    VkBuffer buffers[rhi::kMaxVertexBindings]{};
    VkDeviceSize offsets[rhi::kMaxVertexBindings]{};
    std::uint32_t bindingCount = 0;
    std::uint32_t attributeCount = 0;
    rhi::VertexBinding bindings[rhi::kMaxVertexBindings]{};
    rhi::VertexAttribute attributes[rhi::kMaxVertexAttributes]{};
};
struct GameIB {
    bool valid = false;
    bool index32 = false;
    VkBuffer buffer = VK_NULL_HANDLE;
    VkDeviceSize offset = 0;
};
struct GameDyn : rhi::DynamicStateValues {
    bool valid = false;
};
struct GameRecordState {
    VkPipeline bound = VK_NULL_HANDLE;
    GameVI vi;
    GameIB ib;
    GameDyn dyn;
    const rhi::VertexInputValues* lastVi = nullptr;  // the packets' interned blocks last applied (equal pointer = equal contents)
    const rhi::DynamicStateValues* lastDyn = nullptr;
    std::uint64_t pipelineBinds = 0;
    void reset() {
        lastVi = nullptr;
        lastDyn = nullptr;
        bound = VK_NULL_HANDLE;
        vi.buffersValid = vi.layoutValid = false;
        vi.bufferCount = 0;
        ib.valid = false;
        dyn.valid = false;
    }
};

class GameRecord {
public:
    // the device objects the recording works on (set once at device creation)
    void init(Context& ctx, Frames& frames, Bindless& bindless);

    // the device is going away: join the workers, destroy the pipeline cache (vk::GameDeviceImpl calls shutdown()
    // before its members go; ~GameRecord runs later and finds nothing left to stop)
    ~GameRecord();

    // the pipeline cache (create() after the bindless set exists; destroyed with the device)
    bool create(Context& ctx, VkPipelineLayout layout, const std::filesystem::path& cacheFile, bool libraries);
    rhi::GamePipeline resolvePipeline(const PipelineDesc& desc, bool* created = nullptr, bool wait = true);
    rhi::GamePipeline findPipeline(const PipelineDesc& desc) const;
    bool containsPipeline(const PipelineDesc& desc) const;
    void prewarmPipeline(const PipelineDesc& desc);
    void prewarmOncePipeline(const PipelineDesc& desc);
    void prepareShader(rhi::ShaderModule module, VkShaderStageFlagBits stage);
    void saveCache();
    void stopPipelineBuilds();  // the pipeline cache's prewarm / optimiser threads joined (device teardown, before the modules go)
    rhi::PipelineStats stats() const;

    // rendering scope: attachments transitioned / viewed at their mip+layer, secondary contents mode when threaded
    void beginScope(const rhi::RenderingInfo& info, bool secondary);
    void endScope();  // waits for the scope's chunks, executes them in order (+ labels), rebinds the frame's bindless set
    void scopeChunk(rhi::DrawChunk&& chunk);  // queued to the record workers when the scope began secondary
    void applyDrawPacket(const rhi::DrawPacket& packet);  // inline path: recorded into the frame's command list now
    void scopeLabel(bool begin, const char* text);
    // pipelines bound into the frame's command list (inline) plus the executed chunks' binds, since the last call
    std::uint64_t takePipelineBinds();

    // recording state of the frame's command list: undefined for the next draw (after a scope end or an external command)
    void invalidateRecordingState();
    // a new frame slot: the secondary command buffer pools of that slot are recorded again
    void resetRecordPools(std::uint32_t slot);

    // joins the record workers and destroys the pipeline cache: vk::GameDeviceImpl calls this in its destructor
    // before the renderer's Context / Frames / Bindless go away; ~GameRecord runs later and is a no-op
    void shutdown();

private:
    class Context* ctx_ = nullptr;
    class Frames* frames_ = nullptr;
    class Bindless* bindless_ = nullptr;

    PipelineCache pipelines_;

    // the frame list's recorded state (the inline path); worker chunks get their own GameRecordState per secondary buffer
    GameRecordState rec_;
    std::atomic<std::uint64_t> chunkBinds_{0};  // executed chunks' pipeline binds since the last takePipelineBinds

    struct ChunkEntry {  // a scope chunk: the packets (moved in) plus the recording's result
        rhi::DrawChunk draw;
        std::uint32_t slot = 0;
        VkDescriptorSet set = VK_NULL_HANDLE;      // the bindless set / layout captured on the render thread
        VkPipelineLayout layout = VK_NULL_HANDLE;
        VkCommandBuffer cmd = VK_NULL_HANDLE;
        std::uint64_t binds = 0;
        std::atomic<bool> done{false};
    };
    // the scope's items in order: chunks and debug labels
    struct ScopeItem {
        enum class Kind : std::uint8_t { Chunk, LabelBegin, LabelEnd };
        Kind kind = Kind::Chunk;
        ChunkEntry* chunk = nullptr;
        char text[48] = {};
    };
    std::vector<std::unique_ptr<ChunkEntry>> chunkPool_;  // reused across scopes (packets cleared)
    std::deque<ScopeItem> scopeItems_;
    std::uint32_t chunksUsed_ = 0;
    bool scopeSecondary_ = false;  // the open scope is a secondary-contents one

    static constexpr std::uint32_t kRecordThreads = 2;
    struct RecordThread {
        std::thread thread;
        VkCommandPool pools[rhi::kFramesInFlight]{};
        std::vector<VkCommandBuffer> buffers[rhi::kFramesInFlight]{};
        std::uint32_t used[rhi::kFramesInFlight]{};
    };
    std::vector<std::unique_ptr<RecordThread>> recordThreads_;
    std::unique_ptr<RecordThread> recordSelf_;  // the engine thread's own pools (work stealing)
    std::deque<ChunkEntry*> recordQueue_;
    std::mutex recordMutex_;
    std::condition_variable recordCv_;
    std::atomic<std::uint32_t> recordPending_{0};
    std::atomic<std::uint32_t> recordSleepers_{0};
    std::atomic<bool> recordQuit_{false};

    void recordThreadMain(RecordThread& t);
    void recordChunk(RecordThread& t, ChunkEntry& c);
    void startRecordThreads();
    void stopRecordThreads();

    static void applyPacket(GameRecordState& rs, VkCommandBuffer cmd, VkPipelineLayout layout, const rhi::DrawPacket& p);
    static void applyVertexInput(GameRecordState& rs, VkCommandBuffer cmd, const rhi::VertexInputValues& vi);
    static void applyDynamicState(GameRecordState& rs, VkCommandBuffer cmd, const rhi::DynamicStateValues& v);
    static void bindIndexBuffer(GameRecordState& rs, VkCommandBuffer cmd, VkBuffer buffer, VkDeviceSize offset, bool index32);
};

// The game device holds it as a member declared after its Context / Frames / Bindless (the ctor inits it once those exist)
using GameRecordHolder = GameRecord;

}  // namespace rvk::vk
