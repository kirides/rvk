// Included inside D3D12DeviceImpl (private section): the game device's state and helper types.

public:
    // ---- shader modules: the DXIL kept for the PSO factory ----
    struct ModuleBlob {
        std::vector<std::uint8_t> dxil;
        std::string name;
    };
    std::shared_ptr<ModuleBlob> moduleBlob(ShaderModule m) {
        const std::lock_guard lock(moduleMutex_);
        const auto it = modules_.find(static_cast<std::uint64_t>(m));
        return it == modules_.end() ? nullptr : it->second;
    }

    // ---- pipelines: one entry per PipelineDesc (shaders, targets, blend, layout); a PSO per raster / depth / stencil state of a draw ----
    struct RasterKey {
        std::uint32_t cull = 0, depthTest = 0, depthWrite = 0, depthCompare = 0, stencilTest = 0;
        std::uint32_t front[3] = {}, back[3] = {};
        std::uint32_t frontCompare = 0, backCompare = 0, readMask = 0, writeMask = 0;
        std::int32_t bias = 0;
        float slope = 0.0f;
        bool operator==(const RasterKey& o) const { return std::memcmp(this, &o, sizeof(*this)) == 0; }
    };
    struct PsoEntry {
        PipelineDesc desc{};
        std::uint32_t strides[kMaxVertexBindings] = {};
        std::shared_mutex mutex;
        struct Slot {
            RasterKey key;
            ComPtr<ID3D12PipelineState> pso;  // null: being built by a PSO thread, or failed (logged once)
        };
        std::vector<Slot> psos;
    };
    static RasterKey rasterKey(const DynamicStateValues& d) {
        RasterKey k;  // (zero-initialised: padding included)
        std::memset(&k, 0, sizeof(k));
        k.cull = d.cull;
        k.depthTest = d.depthTest != 0;
        k.depthWrite = d.depthWrite != 0;
        k.depthCompare = k.depthTest ? static_cast<std::uint32_t>(d.depthCompare) : 0;
        k.stencilTest = d.stencilTest != 0;
        if (k.stencilTest) {
            for (int i = 0; i < 3; ++i) {
                k.front[i] = d.stencilFront[i];
                k.back[i] = d.stencilBack[i];
            }
            k.frontCompare = d.stencilFrontCompare;
            k.backCompare = d.stencilBackCompare;
            k.readMask = d.stencilMasks[0];
            k.writeMask = d.stencilMasks[1];
        }
        if (d.depthBiasEnable != 0) {
            k.bias = static_cast<std::int32_t>(d.depthBias[0] >= 0.0f ? std::ceil(d.depthBias[0]) : std::floor(d.depthBias[0]));  // (the fractional units of Vulkan's float constant factor round away from zero: a bias below one unit must not vanish)
            k.slope = d.depthBias[1];
        }
        return k;
    }
    PsoEntry* entryOf(GamePipeline p) {
        const std::uint64_t i = static_cast<std::uint64_t>(p);
        if (i == 0) return nullptr;
        const std::shared_lock lock(entryMutex_);
        return i <= entries_.size() ? entries_[i - 1].get() : nullptr;
    }
    ID3D12PipelineState* psoFor(PsoEntry& e, const DynamicStateValues& dyn);
    ComPtr<ID3D12PipelineState> buildPso(const PsoEntry& e, const RasterKey& k);
    // PSOs are built by background threads (a draw whose PSO is still pending is skipped, like a pipeline that still compiles on Vulkan)
    struct PsoJob {
        PsoEntry* entry;
        RasterKey key;
    };
    std::vector<std::thread> psoThreads_;
    std::deque<PsoJob> psoQueue_;
    std::mutex psoMutex_;
    std::condition_variable psoCv_;
    bool psoQuit_ = false;
    bool psoStopped_ = false;  // stopPipelineBuilds (device teardown): no new PSO threads
    std::atomic<std::uint32_t> psoPending_{0};
    std::atomic<std::uint64_t> psoSkipped_{0};
    std::uint64_t pipelineSkips() const override { return psoSkipped_.load(std::memory_order_relaxed); }
    void startPsoThreads();
    void stopPsoThreads();

    // ---- recording state of one command list (the frame's inline path and each chunk) ----
    struct RecState {
        const VertexInputValues* lastVi = nullptr;
        const DynamicStateValues* lastDyn = nullptr;
        PsoEntry* viEntry = nullptr;  // the entry the vertex buffer views were built for (strides)
        PsoEntry* psoEntry = nullptr;
        const DynamicStateValues* psoDyn = nullptr;
        ID3D12PipelineState* pso = nullptr;
        std::uint64_t ib = 0, ibOffset = 0;
        bool ib32 = false, ibValid = false;
        bool dynValid = false;
        DynamicStateValues dyn{};
        D3D_PRIMITIVE_TOPOLOGY topology = D3D_PRIMITIVE_TOPOLOGY_UNDEFINED;
        std::uint64_t binds = 0;
        void reset() {
            lastVi = nullptr;
            lastDyn = nullptr;
            viEntry = nullptr;
            psoEntry = nullptr;
            psoDyn = nullptr;
            pso = nullptr;
            ibValid = false;
            dynValid = false;
            topology = D3D_PRIMITIVE_TOPOLOGY_UNDEFINED;
        }
    };
    // the render targets of the open scope (CPU descriptors made at beginScope; chunks set them on their own lists)
    struct Scope {
        bool open = false, secondary = false;
        std::uint32_t colorCount = 0;
        D3D12_CPU_DESCRIPTOR_HANDLE rtv[kMaxColorTargets] = {};
        D3D12_CPU_DESCRIPTOR_HANDLE dsv = {};
        bool hasDepth = false;
    };
    void applyPacket(RecState& rs, ID3D12GraphicsCommandList* cmd, const DrawPacket& p);
    void bindScope(ID3D12GraphicsCommandList* cmd, const Scope& sc);
    void clearIn(ID3D12GraphicsCommandList* cmd, const Scope& sc, const ClearOp& op);
    ID3D12CommandSignature* sceneSig_ = nullptr;  // F2b-1: ExecuteIndirect of DrawIndexedIndirect packets (stride kIndirectStride)
    void fixupPacket(DrawPacket& p);  // triangle fans -> an index list (engine thread: allocates from the ring)

    // ---- command list pools ----
    struct ListPool {
        ComPtr<ID3D12CommandAllocator> alloc[rhi::kFramesInFlight];
        std::vector<ComPtr<ID3D12GraphicsCommandList4>> lists[rhi::kFramesInFlight];
        std::uint32_t used[rhi::kFramesInFlight] = {};
    };
    bool initPool(ListPool& p);
    ID3D12GraphicsCommandList4* acquireList(ListPool& p, std::uint32_t slot);
    void resetPool(ListPool& p, std::uint32_t slot);
    void openMainSegment();
    void closeMainSegment();

    // ---- record threads (the opaque passes' chunks) ----
    struct ChunkEntry {
        DrawChunk draw;
        std::uint32_t slot = 0;
        Scope scope;
        ID3D12GraphicsCommandList* list = nullptr;
        std::uint64_t binds = 0;
        std::atomic<bool> done{false};
    };
    struct RecordThread {
        std::thread thread;
        ListPool pool;
    };
    void recordChunk(RecordThread& t, ChunkEntry& c);
    void recordThreadMain(RecordThread& t);
    void startRecordThreads();
    void stopRecordThreads();
    bool wantThreads() const { return std::thread::hardware_concurrency() >= 4; }

    // ---- blits (StretchRect, mip chains, the present copy): a draw into the destination subresource ----
    bool blitDraw(ID3D12GraphicsCommandList* list, D3DTexture& dst, Subresource dsub, const D3D12_RECT& dstRect, D3DTexture& src, Subresource ssub,
                  const float uvRect[4], bool linear);
    void invalidateMainRec() { mainRec_.reset(); }
    ID3D12PipelineState* blitPso(DXGI_FORMAT rtv);
    D3D12_CPU_DESCRIPTOR_HANDLE makeRtv(D3DTexture& t, std::uint32_t mip, std::uint32_t layer);
    D3D12_CPU_DESCRIPTOR_HANDLE makeDsv(D3DTexture& t, std::uint32_t mip, std::uint32_t layer);

    // ---- everything below is the game device's ----
    bool gameMode_ = false;
    bool tearing_ = false;
    GameDeviceOptions gameOptions_;
    ComPtr<ID3D12Fence> frameFence_;
    HANDLE frameEvent_ = nullptr;
    std::uint64_t lastSubmitted_ = 0;
    double lastSlotWaitMs_ = 0.0;
    std::mutex retireMutex_;
    std::vector<std::pair<std::uint64_t, std::function<void()>>> retired_;
    void runRetired();

    ListPool mainPool_, uploadPool_, labelPool_;  // (labelPool_: the tiny lists of labels inside a threaded scope: an allocator records one list at a time)
    std::unique_ptr<D3DCommandList> mainList_, uploadList_;
    ID3D12GraphicsCommandList4* curMain_ = nullptr;
    ID3D12GraphicsCommandList4* curUpload_ = nullptr;
    bool uploadOpen_ = false;
    std::vector<ID3D12CommandList*> frameLists_;  // the main stream in order (segments and chunk lists), closed
    RecState mainRec_;
    Scope scope_;
    std::uint64_t pipelineBinds_ = 0;
    bool bindlessInit_ = false;

    struct RingChunk {
        std::unique_ptr<D3DBuffer> buf;
        std::uint64_t size = 0;
    };
    struct SlotRing {
        std::vector<RingChunk> chunks;
        std::size_t next = 0;
    };
    SlotRing rings_[rhi::kFramesInFlight];
    // the ring's / capture pool's slot: rotated per frame (memory epoch), not per submission; the last fence value that recorded with each
    std::uint32_t memSlot_ = 0;
    std::uint64_t memLastValue_[rhi::kFramesInFlight] = {};
    std::unique_ptr<D3DBuffer> nullVertex_;

    // mesh arena
    struct ArenaBlock {
        std::unique_ptr<D3DBuffer> buf;
        std::uint64_t size = 0, used = 0;
        std::map<std::uint64_t, std::uint64_t> free;  // offset -> size
    };
    std::vector<std::unique_ptr<ArenaBlock>> arena_;
    std::mutex arenaMutex_;
    void arenaFree(std::uint64_t block, std::uint64_t offset, std::uint64_t size);

    // transient images of the passes
    struct TransientEntry {
        std::unique_ptr<Texture> tex;
        TransientDesc desc;
    };
    std::map<std::string, TransientEntry, std::less<>> transients_;

    // pipelines
    std::unordered_map<std::uint64_t, std::shared_ptr<ModuleBlob>> modules_;
    std::mutex moduleMutex_;
    std::uint64_t nextModule_ = 1;
    struct DescHash {
        std::size_t operator()(const PipelineDesc& d) const {
            // 8 bytes per step (the byte-wise FNV over the ~520-byte description was 2 % of the engine thread on D3D12: no
            // dynamic vertex input / blend there, so the direct-mapped recent cache in front misses more often)
            std::uint64_t h = 1469598103934665603ull;
            const auto* p = reinterpret_cast<const std::uint8_t*>(&d);
            std::size_t i = 0;
            for (; i + 8 <= sizeof(d); i += 8) {
                std::uint64_t w;
                std::memcpy(&w, p + i, 8);
                h = (h ^ w) * 0x9E3779B97F4A7C15ull;
                h ^= h >> 29;
            }
            for (; i < sizeof(d); ++i) h = (h ^ p[i]) * 1099511628211ull;
            return static_cast<std::size_t>(h);
        }
    };
    struct DescEq {
        bool operator()(const PipelineDesc& a, const PipelineDesc& b) const { return std::memcmp(&a, &b, sizeof(a)) == 0; }
    };
    std::unordered_map<PipelineDesc, std::uint64_t, DescHash, DescEq> entryIndex_;
    std::vector<std::unique_ptr<PsoEntry>> entries_;
    std::shared_mutex entryMutex_;
    std::atomic<std::uint64_t> psoBuilt_{0};
    std::atomic<std::uint64_t> psoNs_{0};
    std::map<DXGI_FORMAT, ComPtr<ID3D12PipelineState>> blitPsos_;
    std::mutex blitMutex_;
    std::uint32_t blitSamplers_[2] = {0, 0};  // point, linear (clamp)

    // record threads
    static constexpr std::uint32_t kRecordThreads = 2;
    std::vector<std::unique_ptr<RecordThread>> recordThreads_;
    std::unique_ptr<RecordThread> recordSelf_;
    std::vector<std::unique_ptr<ChunkEntry>> chunkPool_;
    std::vector<ChunkEntry*> scopeChunks_;
    std::uint32_t chunksUsed_ = 0;
    std::deque<ChunkEntry*> recordQueue_;
    std::mutex recordMutex_;
    std::condition_variable recordCv_;
    std::atomic<std::uint32_t> recordPending_{0};
    std::atomic<std::uint32_t> recordSleepers_{0};
    bool recordQuit_ = false;
    std::atomic<std::uint64_t> chunkBinds_{0};

    // GPU timestamps ([debug] perf): a query heap of kTsPerSlot marks per frame slot, resolved into a readback buffer at the frame's submission
    static constexpr std::uint32_t kTsPerSlot = 1024;
    struct TsMark {
        std::uint8_t stream = 0;
        std::string label;
    };
    struct TsSlot {
        std::vector<TsMark> marks;
        bool details = false;
    };
    ComPtr<ID3D12QueryHeap> tsHeap_;
    std::unique_ptr<Buffer> tsReadback_;
    TsSlot ts_[rhi::kFramesInFlight];
    double tsNsPerTick_ = 0.0;
    void resolveTimestamps(ID3D12GraphicsCommandList* list, std::uint32_t slot);

    // The vertex capture pool (allocCapture): chunks in a GPU upload heap with UAV access (the CPU writes the headers, vertex shaders / compute store the
    // vertices), per frame slot. State: UNORDERED_ACCESS while rendering scopes write it, a read state for builds / dispatches that read it. The slot of
    // the frame before stays readable (the capture variants' vertex shaders load the previous positions).
    struct CaptureChunk {
        std::unique_ptr<D3DBuffer> buf;
        std::uint64_t size = 0, used = 0;
    };
    struct CapturePool {
        std::vector<CaptureChunk> chunks;
        std::size_t cur = 0;
    };
    CapturePool capture_[rhi::kFramesInFlight];
    bool captureHold_ = false;       // a compute pass writes the pool: dispatches leave its state alone
    bool captureAllRead_ = false;    // both slots are known to be in the read state
    bool gpuUploadHeap_ = false;
    ComPtr<IDXGIAdapter3> adapter3_;  // QueryVideoMemoryInfo (memory budget, WP9)
    std::uint64_t vramDedicated_ = 0, vramShared_ = 0;
    bool dxr11_ = false;
    void setCaptureState(ID3D12GraphicsCommandList* list, std::uint32_t slot, D3D12_RESOURCE_STATES to);

    std::unique_ptr<GamePresenter> presenter_;
    std::unique_ptr<Texture> defaultCube_, default3d_;
    friend class D3D12Presenter;
private:
