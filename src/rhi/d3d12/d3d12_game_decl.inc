// Included inside D3D12DeviceImpl (d3d12_device.cpp): the declarations of the GameDevice interface (rhi/game_device.h) on D3D12. The definitions are in
// d3d12_game_impl.inc (after the command list class), the private state in d3d12_game_priv.inc.
// Design: docs/research/dx12-game-port.md ("Architecture", "WP3 / WP4").

    // ---- creation / lifetime ----
    bool gameMode() const { return gameMode_; }
    bool createGame(const GameDeviceOptions& options);
    void shutdownGame();

    // ---- GameDevice ----
    std::uint32_t refShift() const override { return 32; }  // a GpuRef is {heap slot, byte offset}
    ShaderModule createShaderModule(const Shader& code, const char* name) override;
    void destroyShaderModule(ShaderModule module, bool deferred) override;
    bool formatSupport(VkFormat format, VkImageUsageFlags usage, bool blend) const override;
    CommandList& frameCommands() override;
    CommandList& uploadCommands() override;
    CommandList& passCommands() override;
    void* nativeCommandBuffer(CommandList&) override { return nullptr; }
    RingAlloc allocRingSlow(std::uint64_t size, std::uint64_t alignment, std::uint64_t minOffset) override;
    RingAlloc allocCapture(std::uint64_t size, std::uint64_t alignment) override { return allocRing(size, alignment, 0); }
    Buffer& ringBuffer(const RingAlloc& alloc) override { return *reinterpret_cast<D3DBuffer*>(alloc.buffer); }
    std::uint64_t completedFrame() const override { return frameFence_ ? frameFence_->GetCompletedValue() : 0; }
    void waitFrame(std::uint64_t value) override;
    void retire(std::function<void()> fn) override;
    MeshAlloc allocMesh(std::uint64_t size, std::uint32_t multiple) override;
    void freeMesh(const MeshAlloc& alloc, bool deferred) override;
    Texture* transient(std::string_view key, const TransientDesc& desc) override;
    void releaseTransient(std::string_view key) override;
    std::uint64_t transientBytes() const override;
    bool enableTimestamps() override { return false; }  // (WP9: a timestamp query heap)
    bool timestampsEnabled() const override { return false; }
    void timestampStart(std::uint32_t, bool) override {}
    bool timestampDetails(std::uint32_t) const override { return false; }
    void timestampMark(CommandList&, std::uint32_t, TimestampStream, const char*) override {}
    void timestampMark(CommandList&, std::uint32_t, TimestampStream, std::string&&) override {}
    TimestampResult timestampCollect(std::uint32_t) override { return {}; }
    bool createPipelineCaches(const std::filesystem::path& cacheFile, bool libraries) override;
    GamePipeline resolvePipeline(const PipelineDesc& desc, bool* created) override;
    GamePipeline findPipeline(const PipelineDesc& desc) override;
    bool pipelineContains(const PipelineDesc& desc) override { return findPipeline(desc) != GamePipeline::None; }
    void prewarmPipeline(const PipelineDesc& desc) override { resolvePipeline(desc, nullptr); }
    void prewarmOncePipeline(const PipelineDesc& desc) override { resolvePipeline(desc, nullptr); }
    void preparePipelineShader(ShaderModule, VkShaderStageFlagBits) override {}
    void savePipelineCache() override {}
    PipelineStats pipelineStats() override;
    void beginScope(const RenderingInfo& info, bool secondary) override;
    void endScope() override;
    void scopeChunk(DrawChunk&& chunk) override;
    void applyDrawPacket(const DrawPacket& packet) override;
    void scopeLabel(bool, const char*) override {}
    void invalidateRecordingState() override;
    std::uint64_t takePipelineBinds() override;
    void resetRecordPools() override {}  // (beginFrame resets the slot's allocators)
    void probeFrameGeneration(const std::filesystem::path&) override {}
    caps::Report capabilityReport(bool pipelineLibraries) override;
    void runRaytracingSelfTest() override { spdlog::info("rt selftest: skipped: ray tracing on D3D12 comes with WP6"); }
    void logMemoryStats() override;
    std::unique_ptr<Upscaler> createUpscaler() override { return nullptr; }
    std::unique_ptr<FrameGenerator> createFrameGenerator() override { return nullptr; }
    std::unique_ptr<ImGuiBackend> createImGuiBackend() override { return nullptr; }
    void beginFrame() override;
    double lastSlotWaitMs() const override { return lastSlotWaitMs_; }
    void submitFrame(const char* finalMark) override;
    bool namesEnabled() const override { return debugNamesEnabled(); }
    SamplerIndex createGameSampler(const GameSamplerDesc& desc, const std::string& name) override;
    void clearAttachments(const ClearOp& op) override;
    bool regionsAvailable() const override { return false; }
    void labelBegin(const char*) override {}
    void labelEnd() override {}
    bool setCrashMarkers(bool) override { return false; }
    void crashMarker(bool, const char*) override {}
    GamePresenter& presenter() override { return *presenter_; }
