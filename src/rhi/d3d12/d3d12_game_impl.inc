// Included in d3d12_device.cpp after the command list class and before the exports: the D3D12 game device (rhi/game_device.h).
// docs/research/dx12-game-port.md: queue + fence timeline, 2 frames in flight, per-slot allocators, retire(), ring chunks in upload memory, mesh arena
// in GENERIC_READ blocks, root constants for the draw ABI, PSO per (PipelineDesc, raster / depth / stencil state), chunk command lists on record threads.

// ---- lifetime ----------------------------------------------------------------------------------------------------------------

D3D12DeviceImpl::~D3D12DeviceImpl() {
    shutdownGame();
    if (fenceEvent_ != nullptr) {
        legacyWaitIdle();
        CloseHandle(fenceEvent_);
    }
}

void D3D12DeviceImpl::waitIdle() {
    if (gameMode_) waitFrame(lastSubmitted_);
    else legacyWaitIdle();
}

void D3D12DeviceImpl::waitFrame(std::uint64_t value) {
    if (!frameFence_ || value == 0 || frameFence_->GetCompletedValue() >= value) return;
    HANDLE ev = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    frameFence_->SetEventOnCompletion(value, ev);
    WaitForSingleObject(ev, INFINITE);
    CloseHandle(ev);
}

void D3D12DeviceImpl::retire(std::function<void()> fn) {
    if (!gameMode_) {
        legacyWaitIdle();
        fn();
        return;
    }
    const std::lock_guard lock(retireMutex_);
    retired_.emplace_back(frameValue_, std::move(fn));
}

void D3D12DeviceImpl::runRetired() {
    const std::uint64_t done = frameFence_ ? frameFence_->GetCompletedValue() : ~0ull;
    std::vector<std::function<void()>> run;
    {
        const std::lock_guard lock(retireMutex_);
        std::size_t keep = 0;
        for (auto& r : retired_) {
            if (r.first <= done) run.push_back(std::move(r.second));
            else retired_[keep++] = std::move(r);
        }
        retired_.resize(keep);
    }
    for (auto& f : run) f();
}

void D3D12DeviceImpl::shutdownGame() {
    if (!gameMode_) return;
    waitIdle();
    stopRecordThreads();
    stopPsoThreads();
    presenter_.reset();
    mainList_.reset();
    uploadList_.reset();
    transients_.clear();
    defaultCube_.reset();
    default3d_.reset();
    nullVertex_.reset();
    gameMode_ = false;  // (resources destroyed from here on wait and free at once)
    runRetired();
    {
        const std::lock_guard lock(retireMutex_);
        for (auto& r : retired_) r.second();
        retired_.clear();
    }
    for (auto& r : rings_) r.chunks.clear();
    arena_.clear();
    entries_.clear();
    blitPsos_.clear();
    if (frameEvent_ != nullptr) CloseHandle(frameEvent_);
    frameEvent_ = nullptr;
}

bool D3D12DeviceImpl::initPool(ListPool& p) {
    for (std::uint32_t s = 0; s < rhi::kFramesInFlight; ++s) {
        RVK_HR(device_->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&p.alloc[s])), "CreateCommandAllocator");
    }
    return true;
}

ID3D12GraphicsCommandList4* D3D12DeviceImpl::acquireList(ListPool& p, std::uint32_t slot) {
    auto& lists = p.lists[slot];
    if (p.used[slot] == lists.size()) {
        ComPtr<ID3D12GraphicsCommandList4> l;
        if (FAILED(device_->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, p.alloc[slot].Get(), nullptr, IID_PPV_ARGS(&l)))) return nullptr;
        lists.push_back(std::move(l));
    } else {
        lists[p.used[slot]]->Reset(p.alloc[slot].Get(), nullptr);
    }
    ID3D12GraphicsCommandList4* l = lists[p.used[slot]++].Get();
    ID3D12DescriptorHeap* heaps[2] = {srvHeap_.Get(), samplerHeap_.Get()};
    l->SetDescriptorHeaps(2, heaps);
    return l;
}

void D3D12DeviceImpl::resetPool(ListPool& p, std::uint32_t slot) {
    p.alloc[slot]->Reset();
    p.used[slot] = 0;
}

void D3D12DeviceImpl::openMainSegment() {
    curMain_ = acquireList(mainPool_, frameSlot_);
    mainList_->rebind(curMain_);
    mainRec_.reset();
}

void D3D12DeviceImpl::closeMainSegment() {
    if (curMain_ == nullptr) return;
    curMain_->Close();
    frameLists_.push_back(curMain_);
    curMain_ = nullptr;
}

// ---- creation ----------------------------------------------------------------------------------------------------------------

bool D3D12DeviceImpl::createGame(const GameDeviceOptions& o) {
    gameOptions_ = o;
    DeviceDesc dd;
    dd.backend = Backend::D3D12;
    dd.validation = o.validation || o.syncValidation;
    dd.adapter = o.adapter;
    dd.messages = o.d3d12Messages;
    dd.dred = o.d3d12Dred;
    dd.rayTracing = o.allowRayTracing;
    dd.name = "risen";
    if (!create(dd)) return false;

    D3D12_FEATURE_DATA_D3D12_OPTIONS opt{};
    device_->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS, &opt, sizeof(opt));
    gameCaps_.rayTracing = caps_.rayQuery;
    {
        // the capture pool lives in a GPU upload heap (the CPU writes its headers, the GPU stores into it)
        D3D12_FEATURE_DATA_D3D12_OPTIONS16 o16{};
        gpuUploadHeap_ = SUCCEEDED(device_->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS16, &o16, sizeof(o16))) && o16.GPUUploadHeapSupported != FALSE;
        gameCaps_.vertexStores = gpuUploadHeap_;
    }
    gameCaps_.storageImageExtendedFormats = opt.TypedUAVLoadAdditionalFormats != FALSE;
    gameCaps_.maxComputeWorkGroupInvocations = D3D12_CS_THREAD_GROUP_MAX_THREADS_PER_GROUP;
    gameCaps_.maxComputeWorkGroupSizeX = D3D12_CS_THREAD_GROUP_MAX_X;
    gameCaps_.maxComputeSharedMemory = D3D12_CS_TGSM_REGISTER_COUNT * 4;
    gameCaps_.uploadRowAlign = D3D12_TEXTURE_DATA_PITCH_ALIGNMENT;
    gameCaps_.uploadOffsetAlign = D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT;
    gameCaps_.extendedDynamicState3 = false;
    gameCaps_.vertexInputDynamicState = false;
    gameCaps_.samplerAnisotropy = true;
    gameCaps_.maxAnisotropy = 16.0f;
    gameCaps_.fullScreenExclusive = false;
    caps_.textureBlit = true;  // a draw into the destination (blitDraw)

    // slots 1 and 2: a black texel of the cube and volume dimension (slot 0 = the 2D one made by create())
    {
        auto cube = createTexture({VK_IMAGE_TYPE_2D, VK_FORMAT_R8G8B8A8_UNORM, {1, 1, 1}, 1, 6, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, "rhi default cube", true});
        auto vol = createTexture({VK_IMAGE_TYPE_3D, VK_FORMAT_R8G8B8A8_UNORM, {1, 1, 1}, 1, 1, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, "rhi default volume"});
        auto texel = createBuffer({512 * 7, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, Memory::Upload, "rhi default texels"});
        if (!cube || !vol || !texel) {
            spdlog::error("d3d12: default cube / volume textures");
            return false;
        }
        const std::uint8_t black[4] = {0, 0, 0, 255};
        for (int i = 0; i < 7; ++i) std::memcpy(static_cast<std::uint8_t*>(texel->map()) + i * 512, black, 4);
        submitAndWait([&](CommandList& cl) {
            cl.transition(*cube, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
            for (std::uint32_t f = 0; f < 6; ++f) cl.copyBufferToTexture(*cube, *texel, f * 512ull, {0, f});
            cl.transition(*cube, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
            cl.transition(*vol, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
            cl.copyBufferToTexture(*vol, *texel, 6 * 512ull);
            cl.transition(*vol, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        });
        D3D12_SHADER_RESOURCE_VIEW_DESC sd{};
        sd.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        sd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        sd.ViewDimension = D3D12_SRV_DIMENSION_TEXTURECUBE;
        sd.TextureCube.MipLevels = 1;
        device_->CreateShaderResourceView(static_cast<D3DTexture&>(*cube).resource(), &sd, srvCpu(1));
        sd.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE3D;
        sd.Texture3D.MipLevels = 1;
        device_->CreateShaderResourceView(static_cast<D3DTexture&>(*vol).resource(), &sd, srvCpu(2));
        defaultCube_ = std::move(cube);
        default3d_ = std::move(vol);
        defaultSlots_[0] = 0;
        defaultSlots_[1] = 1;
        defaultSlots_[2] = 2;
    }
    // the 16 bytes of the inputs a declaration does not provide
    {
        nullVertex_.reset(new D3DBuffer(*this, {16, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, Memory::Upload, "rvk null vertex buffer"}));
        if (!nullVertex_->create()) return false;
        const float v[4] = {0.0f, 0.0f, 0.0f, 1.0f};
        std::memcpy(nullVertex_->map(), v, sizeof(v));
        nullVertexBuffer_ = reinterpret_cast<std::uint64_t>(nullVertex_.get());
    }
    blitSamplers_[0] = createSampler({VK_FILTER_NEAREST, VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE, 1.0f});
    blitSamplers_[1] = createSampler({VK_FILTER_LINEAR, VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE, 1.0f});

    RVK_HR(device_->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&frameFence_)), "CreateFence (frames)");
    frameEvent_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!initPool(mainPool_) || !initPool(uploadPool_) || !initPool(labelPool_)) return false;
    {
        BOOL t = FALSE;
        ComPtr<IDXGIFactory5> f5;
        if (SUCCEEDED(factory_.As(&f5)) && SUCCEEDED(f5->CheckFeatureSupport(DXGI_FEATURE_PRESENT_ALLOW_TEARING, &t, sizeof(t)))) tearing_ = t != FALSE;
    }
    mainList_ = std::make_unique<D3DCommandList>(*this, nullptr);
    uploadList_ = std::make_unique<D3DCommandList>(*this, nullptr);
    presenter_ = std::make_unique<D3D12Presenter>(*this);
    setRefShift(32);
    if (o.asyncPipelines) startPsoThreads();
    shader::embedded::setDxilWanted(true);
    gameMode_ = true;
    frameValue_ = 1;
    frameSlot_ = 0;
    spdlog::info("d3d12: game device on '{}' (uploads: rows {} / offsets {} bytes, typed UAV loads {}, tearing {}, ray queries {}, vertex captures {})", caps_.adapterName,
                 gameCaps_.uploadRowAlign, gameCaps_.uploadOffsetAlign, gameCaps_.storageImageExtendedFormats ? "yes" : "no", tearing_ ? "yes" : "no",
                 gameCaps_.rayTracing ? "yes" : "no", gameCaps_.vertexStores ? "yes (GPU upload heap)" : "no (no GPU upload heap)");
    return true;
}

// ---- shader modules ----------------------------------------------------------------------------------------------------------

ShaderModule D3D12DeviceImpl::createShaderModule(const Shader& code, const char* name) {
    if (code.dxil.empty()) return ShaderModule::None;
    auto b = std::make_shared<ModuleBlob>();
    b->dxil.assign(code.dxil.begin(), code.dxil.end());
    if (name != nullptr) b->name = name;
    const std::lock_guard lock(moduleMutex_);
    const std::uint64_t id = nextModule_++;
    modules_.emplace(id, std::move(b));
    return static_cast<ShaderModule>(id);
}

void D3D12DeviceImpl::destroyShaderModule(ShaderModule module, bool) {
    const std::lock_guard lock(moduleMutex_);
    modules_.erase(static_cast<std::uint64_t>(module));  // (PSOs hold their own copy of the bytecode)
}

bool D3D12DeviceImpl::formatSupport(VkFormat format, VkImageUsageFlags usage, bool blend) const {
    // (no hardware has a 24 bit depth buffer any more: AMD stores D24S8 as D32S8 and converts on every access. The D3D9 model falls back to D32S8 like the Vulkan device does)
    if (format == VK_FORMAT_D24_UNORM_S8_UINT && (usage & VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT) != 0) return false;
    const DXGI_FORMAT typed = toDxgi(format);
    if (typed == DXGI_FORMAT_UNKNOWN) return false;
    const auto support = [&](DXGI_FORMAT f) {
        D3D12_FEATURE_DATA_FORMAT_SUPPORT fs{f, D3D12_FORMAT_SUPPORT1_NONE, D3D12_FORMAT_SUPPORT2_NONE};
        device_->CheckFeatureSupport(D3D12_FEATURE_FORMAT_SUPPORT, &fs, sizeof(fs));
        return fs;
    };
    if ((usage & VK_IMAGE_USAGE_SAMPLED_BIT) != 0 && (support(srvFormat(format)).Support1 & D3D12_FORMAT_SUPPORT1_SHADER_SAMPLE) == 0) return false;
    const auto fs = support(typed);
    if ((usage & VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT) != 0 && (fs.Support1 & D3D12_FORMAT_SUPPORT1_RENDER_TARGET) == 0) return false;
    if (blend && (fs.Support1 & D3D12_FORMAT_SUPPORT1_BLENDABLE) == 0) return false;
    if ((usage & VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT) != 0 && (fs.Support1 & D3D12_FORMAT_SUPPORT1_DEPTH_STENCIL) == 0) return false;
    if ((usage & VK_IMAGE_USAGE_STORAGE_BIT) != 0 && (fs.Support1 & D3D12_FORMAT_SUPPORT1_TYPED_UNORDERED_ACCESS_VIEW) == 0) return false;
    return true;
}

// ---- the frame ---------------------------------------------------------------------------------------------------------------

CommandList& D3D12DeviceImpl::frameCommands() { return *mainList_; }

CommandList& D3D12DeviceImpl::passCommands() {
    mainList_->resetBindings();
    mainRec_.reset();
    return *mainList_;
}

CommandList& D3D12DeviceImpl::uploadCommands() {
    if (!uploadOpen_) {
        curUpload_ = acquireList(uploadPool_, frameSlot_);
        uploadList_->rebind(curUpload_);
        uploadOpen_ = true;
        if (timestampsEnabled()) timestampMark(*uploadList_, frameSlot_, TimestampStream::Upload, "upload begin");
    }
    return *uploadList_;
}

void D3D12DeviceImpl::beginFrame() {
    if (device_->GetDeviceRemovedReason() != S_OK) reportRemoved("beginFrame");
    const auto t0 = std::chrono::steady_clock::now();
    if (frameValue_ > rhi::kFramesInFlight) waitFrame(frameValue_ - rhi::kFramesInFlight);
    lastSlotWaitMs_ = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    frameSlot_ = static_cast<std::uint32_t>((frameValue_ - 1) % rhi::kFramesInFlight);
    runRetired();
    resetPool(mainPool_, frameSlot_);
    resetPool(uploadPool_, frameSlot_);
    resetPool(labelPool_, frameSlot_);
    for (auto& t : recordThreads_) resetPool(t->pool, frameSlot_);
    if (recordSelf_) resetPool(recordSelf_->pool, frameSlot_);
    rings_[frameSlot_].next = 0;
    for (auto& c : capture_[frameSlot_].chunks) c.used = 0;
    capture_[frameSlot_].cur = 0;
    captureHold_ = false;
    captureAllRead_ = false;
    ring_.ptr = nullptr;
    ring_.size = ring_.used = 0;
    uavTable_ = 0;
    viewNext_ = 0;
    uploadOpen_ = false;
    curUpload_ = nullptr;
    frameLists_.clear();
    scope_ = {};
    openMainSegment();
}

void D3D12DeviceImpl::submitFrame(const char* finalMark) {
    if (curMain_ == nullptr) return;
    if (timestampsEnabled()) {
        if (uploadOpen_) timestampMark(*uploadList_, frameSlot_, TimestampStream::Upload, "uploads");
        timestampMark(*mainList_, frameSlot_, TimestampStream::Main, finalMark != nullptr ? finalMark : "");
        resolveTimestamps(curMain_, frameSlot_);
    }
    closeMainSegment();
    if (uploadOpen_) {
        curUpload_->Close();
        ID3D12CommandList* u = curUpload_;
        queue_->ExecuteCommandLists(1, &u);  // (its own call: the buffers it wrote are read by the draws as of the next one)
    }
    if (!frameLists_.empty()) queue_->ExecuteCommandLists(static_cast<UINT>(frameLists_.size()), frameLists_.data());
    queue_->Signal(frameFence_.Get(), frameValue_);
    lastSubmitted_ = frameValue_;
    ++frameValue_;
    frameSlot_ = static_cast<std::uint32_t>((frameValue_ - 1) % rhi::kFramesInFlight);
}

// ---- the ring (upload heap chunks) -------------------------------------------------------------------------------------------

RingAlloc D3D12DeviceImpl::allocRingSlow(std::uint64_t size, std::uint64_t alignment, std::uint64_t minOffset) {
    SlotRing& sr = rings_[frameSlot_];
    const std::uint64_t need = ((minOffset + alignment - 1) & ~(alignment - 1)) + size + alignment;
    RingChunk* use = nullptr;
    while (sr.next < sr.chunks.size()) {
        RingChunk& c = sr.chunks[sr.next++];
        if (c.size >= need) {
            use = &c;
            break;
        }
    }
    if (use == nullptr) {
        constexpr std::uint64_t kChunk = 8ull << 20;
        RingChunk c;
        c.size = alignUp(std::max(kChunk, need), 65536);
        c.buf.reset(new D3DBuffer(*this, {c.size, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT |
                                                      VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                                          Memory::Upload, "rvk frame ring"}));
        if (!c.buf->create()) {
            spdlog::error("d3d12: the frame ring could not grow ({} bytes)", c.size);
            return {};
        }
        sr.chunks.push_back(std::move(c));
        sr.next = sr.chunks.size();
        use = &sr.chunks.back();
    }
    const std::uint64_t bytes = ring_.bytes;
    ring_.buffer = reinterpret_cast<std::uint64_t>(use->buf.get());
    ring_.ptr = static_cast<std::uint8_t*>(use->buf->map());
    ring_.ref = GpuRef::fromWords(use->buf->heapIndex(), 0);
    ring_.va = use->buf->gpuAddress();
    ring_.size = use->size;
    ring_.used = 0;
    ring_.bytes = bytes + size;
    return ring_.tryAlloc(size, alignment, (minOffset + alignment - 1) & ~(alignment - 1));
}

// ---- the mesh arena ----------------------------------------------------------------------------------------------------------

MeshAlloc D3D12DeviceImpl::allocMesh(std::uint64_t size, std::uint32_t multiple) {
    const std::uint64_t sz = alignUp(std::max<std::uint64_t>(size, 4), 4);
    const std::uint64_t mult = std::max<std::uint32_t>(multiple, 4);
    const std::lock_guard lock(arenaMutex_);
    const auto take = [&](std::size_t bi, ArenaBlock& b) -> MeshAlloc {
        for (auto it = b.free.begin(); it != b.free.end(); ++it) {
            const std::uint64_t start = it->first, end = it->first + it->second;
            const std::uint64_t off = (start + mult - 1) / mult * mult;
            if (off + sz > end) continue;
            const std::uint64_t fsize = it->second;
            b.free.erase(it);
            if (off > start) b.free[start] = off - start;
            if (off + sz < start + fsize) b.free[off + sz] = start + fsize - (off + sz);
            b.used += sz;
            MeshAlloc a;
            a.buffer = b.buf.get();
            a.native = reinterpret_cast<std::uint64_t>(b.buf.get());
            a.offset = off;
            a.addr.ref = GpuRef::fromWords(b.buf->heapIndex(), static_cast<std::uint32_t>(off));
            a.addr.va = b.buf->gpuAddress() + off;
            a.backend[0] = bi;
            a.backend[1] = sz;
            return a;
        }
        return {};
    };
    for (std::size_t i = 0; i < arena_.size(); ++i) {
        if (MeshAlloc a = take(i, *arena_[i]); a.valid()) return a;
    }
    constexpr std::uint64_t kBlock = 64ull << 20;
    auto b = std::make_unique<ArenaBlock>();
    b->size = alignUp(std::max(kBlock, sz + mult), 65536);
    b->buf.reset(new D3DBuffer(*this, {b->size, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT |
                                                   VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                                       Memory::Device, "rvk mesh arena"}));
    b->buf->arena = true;
    if (!b->buf->create()) {
        spdlog::error("d3d12: the mesh arena could not grow ({} bytes)", b->size);
        return {};
    }
    b->free[0] = b->size;
    arena_.push_back(std::move(b));
    return take(arena_.size() - 1, *arena_.back());
}

void D3D12DeviceImpl::arenaFree(std::uint64_t block, std::uint64_t offset, std::uint64_t size) {
    const std::lock_guard lock(arenaMutex_);
    if (block >= arena_.size()) return;
    ArenaBlock& b = *arena_[block];
    b.used -= size;
    auto it = b.free.emplace(offset, size).first;
    if (auto next = std::next(it); next != b.free.end() && it->first + it->second == next->first) {
        it->second += next->second;
        b.free.erase(next);
    }
    if (it != b.free.begin()) {
        auto prev = std::prev(it);
        if (prev->first + prev->second == it->first) {
            prev->second += it->second;
            b.free.erase(it);
        }
    }
}

void D3D12DeviceImpl::freeMesh(const MeshAlloc& alloc, bool deferred) {
    if (!alloc.valid()) return;
    const std::uint64_t block = alloc.backend[0], offset = alloc.offset, size = alloc.backend[1];
    if (deferred && gameMode_) retire([this, block, offset, size] { arenaFree(block, offset, size); });
    else arenaFree(block, offset, size);
}

// ---- transient images --------------------------------------------------------------------------------------------------------

Texture* D3D12DeviceImpl::transient(std::string_view key, const TransientDesc& desc) {
    auto it = transients_.find(key);
    if (it != transients_.end()) {
        if (it->second.desc == desc) return it->second.tex.get();
        transients_.erase(it);
    }
    TextureDesc td;
    td.format = desc.format;
    td.extent = {desc.extent.width, desc.extent.height, 1};
    td.usage = desc.usage;
    const std::string name(key);
    td.name = name.c_str();
    std::unique_ptr<Texture> t = createTexture(td);
    if (!t) return nullptr;
    spdlog::info("d3d12: transient image '{}' {}x{}", name, desc.extent.width, desc.extent.height);
    Texture* raw = t.get();
    transients_.emplace(name, TransientEntry{std::move(t), desc});
    return raw;
}

void D3D12DeviceImpl::releaseTransient(std::string_view key) {
    const auto it = transients_.find(key);
    if (it != transients_.end()) transients_.erase(it);
}

std::uint64_t D3D12DeviceImpl::transientBytes() const {
    std::uint64_t n = 0;
    for (const auto& [k, e] : transients_) n += std::uint64_t{e.desc.extent.width} * e.desc.extent.height * std::max(1u, formatBytes(e.desc.format));
    return n;
}

// ---- samplers ----------------------------------------------------------------------------------------------------------------

SamplerIndex D3D12DeviceImpl::createGameSampler(const GameSamplerDesc& d, const std::string&) {
    const std::uint32_t i = samplerAlloc_.alloc();
    if (i == 0) return 0;
    D3D12_SAMPLER_DESC sd{};
    const bool aniso = d.maxAnisotropy > 1.0f && d.minFilter == VK_FILTER_LINEAR && d.magFilter == VK_FILTER_LINEAR;
    sd.Filter = aniso ? D3D12_FILTER_ANISOTROPIC
                      : static_cast<D3D12_FILTER>((d.minFilter == VK_FILTER_LINEAR ? 0x10 : 0) | (d.magFilter == VK_FILTER_LINEAR ? 0x4 : 0) |
                                                  (d.mipmap == VK_SAMPLER_MIPMAP_MODE_LINEAR ? 0x1 : 0));
    const auto mode = [](VkSamplerAddressMode m) {
        switch (m) {
        case VK_SAMPLER_ADDRESS_MODE_REPEAT: return D3D12_TEXTURE_ADDRESS_MODE_WRAP;
        case VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT: return D3D12_TEXTURE_ADDRESS_MODE_MIRROR;
        case VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER: return D3D12_TEXTURE_ADDRESS_MODE_BORDER;
        case VK_SAMPLER_ADDRESS_MODE_MIRROR_CLAMP_TO_EDGE: return D3D12_TEXTURE_ADDRESS_MODE_MIRROR_ONCE;
        default: return D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        }
    };
    sd.AddressU = mode(d.addressU);
    sd.AddressV = mode(d.addressV);
    sd.AddressW = mode(d.addressW);
    sd.MipLODBias = d.lodBias;
    sd.MaxAnisotropy = aniso ? static_cast<UINT>(std::clamp(d.maxAnisotropy, 1.0f, 16.0f)) : 1;
    sd.ComparisonFunc = D3D12_COMPARISON_FUNC_NEVER;
    const bool opaque = d.border == VK_BORDER_COLOR_FLOAT_OPAQUE_BLACK || d.border == VK_BORDER_COLOR_INT_OPAQUE_BLACK;
    const bool white = d.border == VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE || d.border == VK_BORDER_COLOR_INT_OPAQUE_WHITE;
    sd.BorderColor[0] = sd.BorderColor[1] = sd.BorderColor[2] = white ? 1.0f : 0.0f;
    sd.BorderColor[3] = white || opaque ? 1.0f : 0.0f;
    sd.MinLOD = d.minLod;
    sd.MaxLOD = d.singleLevel ? d.minLod : D3D12_FLOAT32_MAX;
    device_->CreateSampler(&sd, samplerCpu(i));
    return i;
}

// ---- pipelines ---------------------------------------------------------------------------------------------------------------

bool D3D12DeviceImpl::createPipelineCaches(const std::filesystem::path&, bool) { return true; }

GamePipeline D3D12DeviceImpl::findPipeline(const PipelineDesc& desc) {
    const std::shared_lock lock(entryMutex_);
    const auto it = entryIndex_.find(desc);
    return it == entryIndex_.end() ? GamePipeline::None : static_cast<GamePipeline>(it->second);
}

GamePipeline D3D12DeviceImpl::resolvePipeline(const PipelineDesc& desc, bool* created) {
    if (created != nullptr) *created = false;
    if (desc.vs == ShaderModule::None || desc.fs == ShaderModule::None) return GamePipeline::None;
    {
        const std::shared_lock lock(entryMutex_);
        const auto it = entryIndex_.find(desc);
        if (it != entryIndex_.end()) return static_cast<GamePipeline>(it->second);
    }
    const std::unique_lock lock(entryMutex_);
    const auto it = entryIndex_.find(desc);
    if (it != entryIndex_.end()) return static_cast<GamePipeline>(it->second);
    auto e = std::make_unique<PsoEntry>();
    e->desc = desc;
    for (std::uint32_t i = 0; i < desc.bindingCount && i < kMaxVertexBindings; ++i) e->strides[i] = desc.bindings[i].stride;
    entries_.push_back(std::move(e));
    const std::uint64_t id = entries_.size();
    entryIndex_.emplace(desc, id);
    if (created != nullptr) *created = true;
    return static_cast<GamePipeline>(id);
}

PipelineStats D3D12DeviceImpl::pipelineStats() {
    PipelineStats s;
    s.built = psoBuilt_.load();
    s.buildMs = static_cast<double>(psoNs_.load()) * 1e-6;
    s.cached = entries_.size();
    return s;
}

namespace {
D3D12_STENCIL_OP toStencilOp(std::uint32_t o) {
    switch (o) {
    case VK_STENCIL_OP_ZERO: return D3D12_STENCIL_OP_ZERO;
    case VK_STENCIL_OP_REPLACE: return D3D12_STENCIL_OP_REPLACE;
    case VK_STENCIL_OP_INCREMENT_AND_CLAMP: return D3D12_STENCIL_OP_INCR_SAT;
    case VK_STENCIL_OP_DECREMENT_AND_CLAMP: return D3D12_STENCIL_OP_DECR_SAT;
    case VK_STENCIL_OP_INVERT: return D3D12_STENCIL_OP_INVERT;
    case VK_STENCIL_OP_INCREMENT_AND_WRAP: return D3D12_STENCIL_OP_INCR;
    case VK_STENCIL_OP_DECREMENT_AND_WRAP: return D3D12_STENCIL_OP_DECR;
    default: return D3D12_STENCIL_OP_KEEP;
    }
}
}  // namespace

ComPtr<ID3D12PipelineState> D3D12DeviceImpl::buildPso(const PsoEntry& e, const RasterKey& k) {
    const PipelineDesc& d = e.desc;
    const auto vs = moduleBlob(d.vs);
    const auto ps = moduleBlob(d.fs);
    if (!vs || !ps) return nullptr;
    D3D12_GRAPHICS_PIPELINE_STATE_DESC pd{};
    pd.pRootSignature = rootSignature_.Get();
    pd.VS = {vs->dxil.data(), vs->dxil.size()};
    pd.PS = {ps->dxil.data(), ps->dxil.size()};
    pd.SampleMask = 0xFFFFFFFFu;
    pd.SampleDesc = {1, 0};
    pd.PrimitiveTopologyType = toTopologyType(d.topology);
    std::vector<D3D12_INPUT_ELEMENT_DESC> elements;
    for (std::uint32_t i = 0; i < d.attributeCount; ++i) {
        const VertexAttribute& a = d.attributes[i];
        const bool perInstance = a.binding < d.bindingCount && d.bindings[a.binding].perInstance != 0;
        D3D12_INPUT_ELEMENT_DESC el{};
        el.SemanticName = "ATTR";
        el.SemanticIndex = a.location;
        el.Format = toDxgiVertex(a.format);
        el.InputSlot = a.binding;
        el.AlignedByteOffset = a.offset;
        // a stride 0 binding (the (0, 0, 0, 1) of inputs the declaration lacks) is a per-instance element with step rate 0: the same data for every vertex,
        // without relying on how a driver ranges a zero-stride vertex buffer
        const bool constant = a.binding < d.bindingCount && d.bindings[a.binding].stride == 0;
        el.InputSlotClass = perInstance || constant ? D3D12_INPUT_CLASSIFICATION_PER_INSTANCE_DATA : D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA;
        el.InstanceDataStepRate = constant ? 0 : perInstance ? 1 : 0;
        elements.push_back(el);
    }
    pd.InputLayout = {elements.data(), static_cast<UINT>(elements.size())};
    pd.BlendState.IndependentBlendEnable = TRUE;
    pd.NumRenderTargets = d.colorCount;
    for (std::uint32_t i = 0; i < d.colorCount && i < kMaxColorTargets; ++i) {
        pd.RTVFormats[i] = toDxgi(d.colorFormats[i]);
        const BlendAttachment& b = d.blend[i];
        auto& rt = pd.BlendState.RenderTarget[i];
        rt.BlendEnable = b.enable != 0;
        rt.SrcBlend = toBlend(b.srcColor);
        rt.DestBlend = toBlend(b.dstColor);
        rt.BlendOp = toBlendOp(b.colorOp);
        rt.SrcBlendAlpha = toBlend(b.srcAlpha);
        rt.DestBlendAlpha = toBlend(b.dstAlpha);
        rt.BlendOpAlpha = toBlendOp(b.alphaOp);
        rt.RenderTargetWriteMask = static_cast<UINT8>(b.writeMask & 0xF);
    }
    pd.RasterizerState.FillMode = d.polygonMode == VK_POLYGON_MODE_LINE ? D3D12_FILL_MODE_WIREFRAME : D3D12_FILL_MODE_SOLID;
    pd.RasterizerState.CullMode = (k.cull & VK_CULL_MODE_FRONT_BIT) != 0 && (k.cull & VK_CULL_MODE_BACK_BIT) == 0 ? D3D12_CULL_MODE_FRONT
                                  : (k.cull & VK_CULL_MODE_BACK_BIT) != 0                                          ? D3D12_CULL_MODE_BACK
                                                                                                                   : D3D12_CULL_MODE_NONE;
    pd.RasterizerState.FrontCounterClockwise = FALSE;  // d3d9 / convert.h kFrontFace: front = clockwise in framebuffer space, the same on D3D12
    pd.RasterizerState.DepthBias = k.bias;
    pd.RasterizerState.SlopeScaledDepthBias = k.slope;
    pd.RasterizerState.DepthClipEnable = TRUE;
    pd.DepthStencilState.DepthEnable = k.depthTest != 0;
    pd.DepthStencilState.DepthWriteMask = k.depthWrite != 0 ? D3D12_DEPTH_WRITE_MASK_ALL : D3D12_DEPTH_WRITE_MASK_ZERO;
    pd.DepthStencilState.DepthFunc = toCompare(static_cast<VkCompareOp>(k.depthCompare));
    pd.DepthStencilState.StencilEnable = k.stencilTest != 0;
    pd.DepthStencilState.StencilReadMask = static_cast<UINT8>(k.readMask);
    pd.DepthStencilState.StencilWriteMask = static_cast<UINT8>(k.writeMask);
    pd.DepthStencilState.FrontFace = {toStencilOp(k.front[0]), toStencilOp(k.front[2]), toStencilOp(k.front[1]), toCompare(static_cast<VkCompareOp>(k.frontCompare))};
    pd.DepthStencilState.BackFace = {toStencilOp(k.back[0]), toStencilOp(k.back[2]), toStencilOp(k.back[1]), toCompare(static_cast<VkCompareOp>(k.backCompare))};
    pd.DSVFormat = d.depthFormat != VK_FORMAT_UNDEFINED ? toDxgi(d.depthFormat) : DXGI_FORMAT_UNKNOWN;
    ComPtr<ID3D12PipelineState> pso;
    const auto t0 = std::chrono::steady_clock::now();
    const HRESULT hr = device_->CreateGraphicsPipelineState(&pd, IID_PPV_ARGS(&pso));
    psoNs_ += static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - t0).count());
    ++psoBuilt_;
    if (FAILED(hr)) {
        checkRemoved(hr, "CreateGraphicsPipelineState");
        spdlog::error("d3d12: CreateGraphicsPipelineState ({} / {}) failed (0x{:08x})", vs->name, ps->name, static_cast<unsigned>(hr));
        return nullptr;
    }
    return pso;
}

ID3D12PipelineState* D3D12DeviceImpl::psoFor(PsoEntry& e, const DynamicStateValues& dyn) {
    const RasterKey k = rasterKey(dyn);
    {
        const std::shared_lock lock(e.mutex);
        for (const auto& s : e.psos) {
            if (s.key == k) return s.pso.Get();
        }
    }
    {
        const std::unique_lock lock(e.mutex);
        for (const auto& s : e.psos) {
            if (s.key == k) return s.pso.Get();
        }
        e.psos.push_back({k, nullptr});
    }
    if (psoThreads_.empty()) {  // (no PSO threads: built here)
        ComPtr<ID3D12PipelineState> pso = buildPso(e, k);
        const std::unique_lock lock(e.mutex);
        for (auto& s : e.psos) {
            if (s.key == k) s.pso = pso;
        }
        return pso.Get();
    }
    psoPending_.fetch_add(1);
    {
        const std::lock_guard lock(psoMutex_);
        psoQueue_.push_back({&e, k});
    }
    psoCv_.notify_one();
    return nullptr;
}

void D3D12DeviceImpl::startPsoThreads() {
    if (!psoThreads_.empty()) return;
    const std::uint32_t hw = std::max(2u, std::thread::hardware_concurrency());
    const std::uint32_t n = std::clamp(hw / 3, 2u, 6u);
    psoQuit_ = false;
    for (std::uint32_t i = 0; i < n; ++i) {
        psoThreads_.emplace_back([this] {
            SetThreadDescription(GetCurrentThread(), L"rvk pso");
            for (;;) {
                PsoJob job{};
                {
                    std::unique_lock lock(psoMutex_);
                    psoCv_.wait(lock, [this] { return psoQuit_ || !psoQueue_.empty(); });
                    if (psoQueue_.empty()) return;  // (quitting)
                    job = psoQueue_.front();
                    psoQueue_.pop_front();
                }
                ComPtr<ID3D12PipelineState> pso = buildPso(*job.entry, job.key);
                {
                    const std::unique_lock lock(job.entry->mutex);
                    for (auto& s : job.entry->psos) {
                        if (s.key == job.key) s.pso = std::move(pso);
                    }
                }
                psoPending_.fetch_sub(1);
            }
        });
    }
    spdlog::info("d3d12: {} PSO build threads", n);
}

void D3D12DeviceImpl::stopPsoThreads() {
    {
        const std::lock_guard lock(psoMutex_);
        psoQuit_ = true;
        psoQueue_.clear();
    }
    psoCv_.notify_all();
    for (auto& t : psoThreads_) {
        if (t.joinable()) t.join();
    }
    psoThreads_.clear();
}

// ---- scopes / chunks / packets -----------------------------------------------------------------------------------------------

D3D12_CPU_DESCRIPTOR_HANDLE D3D12DeviceImpl::makeRtv(D3DTexture& t, std::uint32_t mip, std::uint32_t layer) {
    const D3D12_CPU_DESCRIPTOR_HANDLE h = rtvCpu(nextRtv());
    D3D12_RENDER_TARGET_VIEW_DESC rv{};
    rv.Format = toDxgi(t.desc().format);
    if (t.desc().layers > 1) {
        rv.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2DARRAY;
        rv.Texture2DArray.MipSlice = mip;
        rv.Texture2DArray.FirstArraySlice = layer;
        rv.Texture2DArray.ArraySize = 1;
    } else {
        rv.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
        rv.Texture2D.MipSlice = mip;
    }
    device_->CreateRenderTargetView(t.resource(), &rv, h);
    return h;
}

D3D12_CPU_DESCRIPTOR_HANDLE D3D12DeviceImpl::makeDsv(D3DTexture& t, std::uint32_t mip, std::uint32_t layer) {
    const D3D12_CPU_DESCRIPTOR_HANDLE h = dsvCpu(nextDsv());
    D3D12_DEPTH_STENCIL_VIEW_DESC dd{};
    dd.Format = toDxgi(t.desc().format);
    if (t.desc().layers > 1) {
        dd.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2DARRAY;
        dd.Texture2DArray.MipSlice = mip;
        dd.Texture2DArray.FirstArraySlice = layer;
        dd.Texture2DArray.ArraySize = 1;
    } else {
        dd.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
        dd.Texture2D.MipSlice = mip;
    }
    device_->CreateDepthStencilView(t.resource(), &dd, h);
    return h;
}

void D3D12DeviceImpl::bindScope(ID3D12GraphicsCommandList* cmd, const Scope& sc) {
    D3D12_CPU_DESCRIPTOR_HANDLE rtv[kMaxColorTargets];
    for (std::uint32_t i = 0; i < sc.colorCount; ++i) rtv[i] = sc.rtv[i];
    cmd->OMSetRenderTargets(sc.colorCount, sc.colorCount > 0 ? rtv : nullptr, FALSE, sc.hasDepth ? &sc.dsv : nullptr);
}

void D3D12DeviceImpl::clearIn(ID3D12GraphicsCommandList* cmd, const Scope& sc, const ClearOp& op) {
    const D3D12_RECT r{op.rect.rect.offset.x, op.rect.rect.offset.y, op.rect.rect.offset.x + static_cast<LONG>(op.rect.rect.extent.width),
                       op.rect.rect.offset.y + static_cast<LONG>(op.rect.rect.extent.height)};
    for (std::uint32_t i = 0; i < op.count; ++i) {
        const VkClearAttachment& a = op.atts[i];
        if ((a.aspectMask & VK_IMAGE_ASPECT_COLOR_BIT) != 0) {
            if (a.colorAttachment < sc.colorCount) cmd->ClearRenderTargetView(sc.rtv[a.colorAttachment], a.clearValue.color.float32, 1, &r);
        } else if (sc.hasDepth) {
            D3D12_CLEAR_FLAGS f = static_cast<D3D12_CLEAR_FLAGS>(0);
            if ((a.aspectMask & VK_IMAGE_ASPECT_DEPTH_BIT) != 0) f |= D3D12_CLEAR_FLAG_DEPTH;
            if ((a.aspectMask & VK_IMAGE_ASPECT_STENCIL_BIT) != 0) f |= D3D12_CLEAR_FLAG_STENCIL;
            cmd->ClearDepthStencilView(sc.dsv, f, a.clearValue.depthStencil.depth, static_cast<UINT8>(a.clearValue.depthStencil.stencil), 1, &r);
        }
    }
}

void D3D12DeviceImpl::clearAttachments(const ClearOp& op) { clearIn(curMain_, scope_, op); }

void D3D12DeviceImpl::beginScope(const RenderingInfo& info, bool secondary) {
    // the capture pool: this frame's slot is written by the scope's vertex shaders, the previous frame's is read by them
    if (gameCaps_.vertexStores) {
        setCaptureState(curMain_, frameSlot_, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        setCaptureState(curMain_, (frameSlot_ + 1) % rhi::kFramesInFlight, D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE);
        captureHold_ = false;
        captureAllRead_ = false;
    }
    scope_ = {};
    scope_.open = true;
    scope_.secondary = secondary;
    scope_.colorCount = info.colorCount;
    for (std::uint32_t i = 0; i < info.colorCount; ++i) {
        const RenderingInfo::Attachment& a = info.color[i];
        auto& t = static_cast<D3DTexture&>(*a.texture);
        mainList_->transition(t, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
        scope_.rtv[i] = makeRtv(t, a.mip, a.layer);
        if (a.load == VK_ATTACHMENT_LOAD_OP_CLEAR) curMain_->ClearRenderTargetView(scope_.rtv[i], a.clear.float32, 0, nullptr);
    }
    if (info.depth.texture != nullptr) {
        auto& t = static_cast<D3DTexture&>(*info.depth.texture);
        mainList_->transition(t, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL);
        scope_.dsv = makeDsv(t, info.depth.mip, info.depth.layer);
        scope_.hasDepth = true;
        if (info.depth.load == VK_ATTACHMENT_LOAD_OP_CLEAR) {
            const D3D12_CLEAR_FLAGS f = D3D12_CLEAR_FLAG_DEPTH | (hasStencil(t.desc().format) ? D3D12_CLEAR_FLAG_STENCIL : static_cast<D3D12_CLEAR_FLAGS>(0));
            curMain_->ClearDepthStencilView(scope_.dsv, f, info.depthClear.depth, static_cast<UINT8>(info.depthClear.stencil), 0, nullptr);
        }
    }
    bindScope(curMain_, scope_);
    mainRec_.reset();
    if (secondary) startRecordThreads();
}

void D3D12DeviceImpl::scopeChunk(DrawChunk&& chunk) {
    for (DrawPacket& p : chunk.packets) fixupPacket(p);
    if (chunksUsed_ == chunkPool_.size()) chunkPool_.push_back(std::make_unique<ChunkEntry>());
    ChunkEntry* e = chunkPool_[chunksUsed_++].get();
    e->draw = std::move(chunk);
    e->slot = frameSlot_;
    e->scope = scope_;
    e->list = nullptr;
    e->binds = 0;
    e->done.store(false, std::memory_order_relaxed);
    scopeChunks_.push_back(e);
    if (recordThreads_.empty()) {
        e->done.store(true);
        return;
    }
    {
        const std::lock_guard lock(recordMutex_);
        recordQueue_.push_back(e);
        recordPending_.fetch_add(1, std::memory_order_release);
    }
    if (recordSleepers_.load() != 0) recordCv_.notify_one();
}

void D3D12DeviceImpl::endScope() {
    if (scope_.secondary) {
        while (recordSelf_) {
            ChunkEntry* e = nullptr;
            {
                const std::lock_guard lock(recordMutex_);
                if (recordQueue_.empty()) break;
                e = recordQueue_.front();
                recordQueue_.pop_front();
                recordPending_.fetch_sub(1, std::memory_order_release);
            }
            recordChunk(*recordSelf_, *e);
            e->done.store(true, std::memory_order_release);
            e->done.notify_all();
        }
        closeMainSegment();  // everything recorded before the chunks (transitions, clears) runs first
        for (ChunkEntry* e : scopeChunks_) {
            while (!e->done.load(std::memory_order_acquire)) e->done.wait(false, std::memory_order_acquire);
            chunkBinds_ += e->binds;
            if (e->list != nullptr) frameLists_.push_back(e->list);
        }
        scopeChunks_.clear();
        chunksUsed_ = 0;
        openMainSegment();
    }
    scope_.open = false;
    scope_.secondary = false;
    mainRec_.reset();
}

void D3D12DeviceImpl::invalidateRecordingState() { mainRec_.reset(); }

std::uint64_t D3D12DeviceImpl::takePipelineBinds() {
    const std::uint64_t n = mainRec_.binds + chunkBinds_.exchange(0);
    mainRec_.binds = 0;
    return n;
}

void D3D12DeviceImpl::applyDrawPacket(const DrawPacket& packet) {
    DrawPacket p = packet;
    fixupPacket(p);
    if (mainRec_.lastVi == nullptr && mainRec_.psoEntry == nullptr) curMain_->SetGraphicsRootSignature(rootSignature_.Get());
    applyPacket(mainRec_, curMain_, p);
}

// A triangle fan has no D3D12 topology: a plain fan becomes an indexed list of its triangles (index data from the ring; the engine thread).
// Indexed fans cannot be converted on the CPU (the indices live on the GPU): none occur in the game (docs/research/dx12-game-port.md WP0).
void D3D12DeviceImpl::fixupPacket(DrawPacket& p) {
    if (p.op == DrawPacket::Op::Clear || p.dyn == nullptr || p.dyn->topology != VK_PRIMITIVE_TOPOLOGY_TRIANGLE_FAN) return;
    if (p.op == DrawPacket::Op::DrawIndexed || p.count < 3) {
        static std::atomic<bool> logged{false};
        if (p.op == DrawPacket::Op::DrawIndexed && !logged.exchange(true)) spdlog::warn("d3d12: an indexed triangle fan was dropped (not supported)");
        p.count = 0;
        return;
    }
    const std::uint32_t tris = p.count - 2;
    RingAlloc a = allocRing(static_cast<std::uint64_t>(tris) * 12, 16);
    if (a.ptr == nullptr) {
        p.count = 0;
        return;
    }
    auto* idx = static_cast<std::uint32_t*>(a.ptr);
    for (std::uint32_t i = 0; i < tris; ++i) {
        idx[i * 3 + 0] = p.first;
        idx[i * 3 + 1] = p.first + i + 1;
        idx[i * 3 + 2] = p.first + i + 2;
    }
    p.op = DrawPacket::Op::DrawIndexed;
    p.index32 = true;
    p.indexBuffer = a.buffer;
    p.indexOffset = a.offset;
    p.count = tris * 3;
    p.first = 0;
    p.vertexOffset = 0;
}

void D3D12DeviceImpl::applyPacket(RecState& rs, ID3D12GraphicsCommandList* cmd, const DrawPacket& p) {
    if (p.count == 0) return;
    PsoEntry* e = entryOf(p.pipeline);
    if (e == nullptr) return;
    if (p.dyn != rs.psoDyn || e != rs.psoEntry) {
        ID3D12PipelineState* pso = psoFor(*e, *p.dyn);
        rs.psoEntry = e;
        rs.psoDyn = p.dyn;
        if (pso == nullptr) {
            rs.pso = nullptr;
            psoSkipped_.fetch_add(1, std::memory_order_relaxed);
            return;
        }
        if (pso != rs.pso) {
            cmd->SetPipelineState(pso);
            rs.pso = pso;
            ++rs.binds;
        }
    } else if (rs.pso == nullptr) {
        psoSkipped_.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    if (p.dyn != rs.lastDyn) {
        const DynamicStateValues& s = *p.dyn;
        DynamicStateValues& c = rs.dyn;
        if (!rs.dynValid || std::memcmp(&c.viewport, &s.viewport, sizeof(s.viewport)) != 0) {
            // y-flipped viewports of the Vulkan convention (negative height) back to D3D's
            D3D12_VIEWPORT vp{s.viewport.x, s.viewport.height < 0 ? s.viewport.y + s.viewport.height : s.viewport.y, s.viewport.width,
                              s.viewport.height < 0 ? -s.viewport.height : s.viewport.height, s.viewport.minDepth, s.viewport.maxDepth};
            cmd->RSSetViewports(1, &vp);
        }
        if (!rs.dynValid || std::memcmp(&c.scissor, &s.scissor, sizeof(s.scissor)) != 0) {
            const D3D12_RECT r{s.scissor.offset.x, s.scissor.offset.y, s.scissor.offset.x + static_cast<LONG>(s.scissor.extent.width),
                               s.scissor.offset.y + static_cast<LONG>(s.scissor.extent.height)};
            cmd->RSSetScissorRects(1, &r);
        }
        const D3D_PRIMITIVE_TOPOLOGY topo = s.topology == VK_PRIMITIVE_TOPOLOGY_TRIANGLE_FAN ? D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST : toTopology(s.topology);
        if (topo != rs.topology) {
            cmd->IASetPrimitiveTopology(topo);
            rs.topology = topo;
        }
        if (!rs.dynValid || c.stencilMasks[2] != s.stencilMasks[2]) cmd->OMSetStencilRef(s.stencilMasks[2]);
        if (!rs.dynValid || std::memcmp(c.blendConstants, s.blendConstants, sizeof(s.blendConstants)) != 0) cmd->OMSetBlendFactor(s.blendConstants);
        c = s;
        rs.dynValid = true;
        rs.lastDyn = p.dyn;
    }
    if (p.vi != rs.lastVi || e != rs.viEntry) {
        D3D12_VERTEX_BUFFER_VIEW views[kMaxVertexBindings]{};
        const std::uint32_t n = std::min(p.vi->bufferCount, kMaxVertexBindings);
        for (std::uint32_t i = 0; i < n; ++i) {
            auto* b = reinterpret_cast<D3DBuffer*>(p.vi->buffers[i]);
            views[i].BufferLocation = b->gpuAddress() + p.vi->offsets[i];
            views[i].SizeInBytes = static_cast<UINT>(std::min<std::uint64_t>(b->desc().size - p.vi->offsets[i], 0xFFFFFFFFull));
            views[i].StrideInBytes = e->strides[i];
        }
        if (n > 0) cmd->IASetVertexBuffers(0, n, views);
        rs.lastVi = p.vi;
        rs.viEntry = e;
    }
    cmd->SetGraphicsRoot32BitConstants(0, sizeof(p.push) / 4, &p.push, 0);
    if (p.op == DrawPacket::Op::DrawIndexed) {
        if (!rs.ibValid || rs.ib != p.indexBuffer || rs.ibOffset != p.indexOffset || rs.ib32 != p.index32) {
            auto* b = reinterpret_cast<D3DBuffer*>(p.indexBuffer);
            D3D12_INDEX_BUFFER_VIEW v{b->gpuAddress() + p.indexOffset, static_cast<UINT>(std::min<std::uint64_t>(b->desc().size - p.indexOffset, 0xFFFFFFFFull)),
                                      p.index32 ? DXGI_FORMAT_R32_UINT : DXGI_FORMAT_R16_UINT};
            cmd->IASetIndexBuffer(&v);
            rs.ib = p.indexBuffer;
            rs.ibOffset = p.indexOffset;
            rs.ib32 = p.index32;
            rs.ibValid = true;
        }
        cmd->DrawIndexedInstanced(p.count, p.instances, p.first, p.vertexOffset, 0);
    } else {
        cmd->DrawInstanced(p.count, p.instances, p.first, 0);
    }
}

void D3D12DeviceImpl::recordChunk(RecordThread& t, ChunkEntry& c) {
    ID3D12GraphicsCommandList4* l = acquireList(t.pool, c.slot);
    if (l == nullptr) return;
    l->SetGraphicsRootSignature(rootSignature_.Get());
    bindScope(l, c.scope);
    RecState rs;
    for (const DrawPacket& p : c.draw.packets) {
        if (p.op == DrawPacket::Op::Clear) clearIn(l, c.scope, c.draw.clears[p.first]);
        else applyPacket(rs, l, p);
    }
    l->Close();
    c.binds = rs.binds;
    c.list = l;
}

void D3D12DeviceImpl::recordThreadMain(RecordThread& t) {
    SetThreadDescription(GetCurrentThread(), L"rvk record");
    for (;;) {
        ChunkEntry* c = nullptr;
        for (int spin = 0; spin < 4000 && recordPending_.load(std::memory_order_acquire) == 0; ++spin) {
            if (recordQuit_) break;
            YieldProcessor();
        }
        {
            std::unique_lock lock(recordMutex_);
            if (recordQueue_.empty() && !recordQuit_) {
                recordSleepers_.fetch_add(1);
                recordCv_.wait(lock, [this] { return recordQuit_ || !recordQueue_.empty(); });
                recordSleepers_.fetch_sub(1);
            }
            if (recordQueue_.empty()) {
                if (recordQuit_) return;
                continue;
            }
            c = recordQueue_.front();
            recordQueue_.pop_front();
            recordPending_.fetch_sub(1, std::memory_order_release);
        }
        recordChunk(t, *c);
        c->done.store(true, std::memory_order_release);
        c->done.notify_all();
    }
}

void D3D12DeviceImpl::startRecordThreads() {
    if (!recordThreads_.empty() || !wantThreads()) return;
    recordQuit_ = false;
    for (std::uint32_t i = 0; i < kRecordThreads; ++i) {
        auto t = std::make_unique<RecordThread>();
        if (!initPool(t->pool)) break;
        RecordThread* raw = t.get();
        recordThreads_.push_back(std::move(t));
        raw->thread = std::thread([this, raw] { recordThreadMain(*raw); });
    }
    recordSelf_ = std::make_unique<RecordThread>();
    if (!initPool(recordSelf_->pool)) recordSelf_.reset();
    spdlog::info("d3d12: record threads: {} worker(s) + the engine thread", recordThreads_.size());
}

void D3D12DeviceImpl::stopRecordThreads() {
    {
        const std::lock_guard lock(recordMutex_);
        recordQuit_ = true;
    }
    recordCv_.notify_all();
    for (auto& t : recordThreads_) {
        if (t->thread.joinable()) t->thread.join();
    }
    recordThreads_.clear();
    recordSelf_.reset();
    chunkPool_.clear();
}

// ---- blits -------------------------------------------------------------------------------------------------------------------

ID3D12PipelineState* D3D12DeviceImpl::blitPso(DXGI_FORMAT rtv) {
    const std::lock_guard lock(blitMutex_);
    const auto it = blitPsos_.find(rtv);
    if (it != blitPsos_.end()) return it->second.Get();
    const auto vs = shader::embedded::dxil("blit/blit.vert");
    const auto ps = shader::embedded::dxil("blit/blit.frag");
    ComPtr<ID3D12PipelineState> pso;
    if (!vs.empty() && !ps.empty()) {
        D3D12_GRAPHICS_PIPELINE_STATE_DESC pd{};
        pd.pRootSignature = rootSignature_.Get();
        pd.VS = {vs.data(), vs.size()};
        pd.PS = {ps.data(), ps.size()};
        pd.SampleMask = 0xFFFFFFFFu;
        pd.SampleDesc = {1, 0};
        pd.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        pd.NumRenderTargets = 1;
        pd.RTVFormats[0] = rtv;
        pd.BlendState.RenderTarget[0].RenderTargetWriteMask = 0xF;
        pd.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
        pd.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
        pd.RasterizerState.DepthClipEnable = TRUE;
        if (FAILED(device_->CreateGraphicsPipelineState(&pd, IID_PPV_ARGS(&pso)))) spdlog::error("d3d12: the blit pipeline for format {} failed", static_cast<int>(rtv));
    } else {
        spdlog::error("d3d12: the blit shaders did not compile to DXIL");
    }
    blitPsos_[rtv] = pso;
    return pso.Get();
}

namespace {
D3D12_RESOURCE_STATES stateOfMip(const D3DTexture& t, std::uint32_t mip) { return t.split ? t.mipStates[std::min(mip, 15u)] : t.state; }
}  // namespace

// One scaled copy as a draw: `src` subresource sampled (point / linear) over the `uvRect` (u0, v0, du, dv) into `dstRect` of the destination subresource.
// The two subresources are moved to the shader / render target states and back (the tracked states do not change).
bool D3D12DeviceImpl::blitDraw(ID3D12GraphicsCommandList* list, D3DTexture& dst, Subresource dsub, const D3D12_RECT& dstRect, D3DTexture& src, Subresource ssub,
                               const float uvRect[4], bool linear) {
    ID3D12PipelineState* pso = blitPso(toDxgi(dst.desc().format));
    if (pso == nullptr) return false;
    const UINT srcIdx = src.subresource(ssub), dstIdx = dst.subresource(dsub);
    const D3D12_RESOURCE_STATES srcFrom = stateOfMip(src, ssub.mip), dstFrom = stateOfMip(dst, dsub.mip);
    const D3D12_RESOURCE_STATES srcTo = D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE, dstTo = D3D12_RESOURCE_STATE_RENDER_TARGET;
    D3D12_RESOURCE_BARRIER b[2]{};
    UINT nb = 0;
    const auto add = [&](D3DTexture& t, UINT idx, D3D12_RESOURCE_STATES from, D3D12_RESOURCE_STATES to) {
        if (from == to) return;
        b[nb].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b[nb].Transition = {t.resource(), idx, from, to};
        ++nb;
    };
    add(src, srcIdx, srcFrom, srcTo);
    add(dst, dstIdx, dstFrom, dstTo);
    if (nb > 0) list->ResourceBarrier(nb, b);
    // the source view: one mip of one layer as a Texture2D
    const std::uint32_t slot = nextViewSlot();
    D3D12_SHADER_RESOURCE_VIEW_DESC sd{};
    sd.Format = srvFormat(src.desc().format);
    sd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    if (src.desc().layers > 1) {
        sd.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DARRAY;
        sd.Texture2DArray.MostDetailedMip = ssub.mip;
        sd.Texture2DArray.MipLevels = 1;
        sd.Texture2DArray.FirstArraySlice = ssub.layer;
        sd.Texture2DArray.ArraySize = 1;
    } else {
        sd.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        sd.Texture2D.MostDetailedMip = ssub.mip;
        sd.Texture2D.MipLevels = 1;
    }
    device_->CreateShaderResourceView(src.resource(), &sd, srvCpu(slot));
    const D3D12_CPU_DESCRIPTOR_HANDLE rtv = makeRtv(dst, dsub.mip, dsub.layer);
    list->SetGraphicsRootSignature(rootSignature_.Get());
    list->SetPipelineState(pso);
    list->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
    const D3D12_VIEWPORT vp{static_cast<float>(dstRect.left), static_cast<float>(dstRect.top), static_cast<float>(dstRect.right - dstRect.left),
                            static_cast<float>(dstRect.bottom - dstRect.top), 0.0f, 1.0f};
    list->RSSetViewports(1, &vp);
    list->RSSetScissorRects(1, &dstRect);
    list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    const std::uint32_t pc[8] = {slot, blitSamplers_[linear ? 1 : 0], 0, 0, std::bit_cast<std::uint32_t>(uvRect[0]), std::bit_cast<std::uint32_t>(uvRect[1]),
                                 std::bit_cast<std::uint32_t>(uvRect[2]), std::bit_cast<std::uint32_t>(uvRect[3])};
    list->SetGraphicsRoot32BitConstants(0, 8, pc, 0);
    list->DrawInstanced(3, 1, 0, 0);
    nb = 0;
    add(src, srcIdx, srcTo, srcFrom);
    add(dst, dstIdx, dstTo, dstFrom);
    if (nb > 0) list->ResourceBarrier(nb, b);
    return true;
}

// ---- diagnostics -------------------------------------------------------------------------------------------------------------

caps::Report D3D12DeviceImpl::capabilityReport(bool) {
    caps::Report r;
    r.known = true;
    r.gpu = caps_.adapterName;
    r.driver = "Direct3D 12";
    r.api = std::format("D3D12 (feature level 12_0, Shader Model 6.6, binding tier 3{})", caps_.agilitySdkVersion != 0 ? std::format(", Agility SDK {}", caps_.agilitySdkVersion) : std::string());
    const auto set = [&](caps::Cap c, bool on, const char* why) {
        auto& e = r[c];
        e.available = on;
        e.enabled = on;
        if (!on) e.reason = why;
    };
    set(caps::Cap::SamplerAnisotropy, true, "");
    set(caps::Cap::DrawIndirectCount, true, "");
    set(caps::Cap::RayTracingHardware, dxr11_ || caps_.rayQuery, "the GPU / driver has no DXR tier 1.1");
    set(caps::Cap::RayQuery, caps_.rayQuery, "ray tracing is off (rt.allow) or the GPU / driver has no DXR tier 1.1");
    if (caps_.rayQuery) r[caps::Cap::RayQuery].detail = "DXR 1.1 inline ray queries";
    set(caps::Cap::PositionFetch, false, "not available on D3D12 (the RT scene reads its own geometry records)");
    // frame generation (d3d12_ffx.h): the effect DLL + a provider; the swapchain needs no extra queues on D3D12
    {
        const char* const kNoDll = "amd_fidelityfx_framegeneration_dx12.dll is not installed (folder rvk)";
        const std::string reason = !fgProbe_.dllPresent ? kNoDll : !fgProbe_.probed ? "not probed" : fgProbe_.reason;
        set(caps::Cap::FrameGeneration, fgProbe_.dllPresent && fgProbe_.fsr3, reason.c_str());
        r[caps::Cap::FrameGeneration].detail = fgProbe_.versions;
        const std::string mlReason = !fgProbe_.dllPresent ? kNoDll
                                     : fgProbe_.probed    ? "the graphics driver offers no FSR 4 frame generation for this GPU (it needs a Radeon RX 9000 series)"
                                                          : "not probed";
        set(caps::Cap::FrameGenerationMl, fgProbe_.dllPresent && fgProbe_.ml, mlReason.c_str());
        r[caps::Cap::FrameGenerationMl].detail = fgProbe_.versions;
        set(caps::Cap::FrameGenerationQueues, true, "");
        r[caps::Cap::FrameGenerationQueues].detail = "AMD's D3D12 swapchain makes its own queues";
    }
    set(caps::Cap::FullScreenExclusive, false, "exclusive fullscreen is not implemented on D3D12 (the game's fullscreen stays a borderless window)");
    set(caps::Cap::MemoryBudget, adapter3_ != nullptr, "IDXGIAdapter3 is not available");
    if (adapter3_) r[caps::Cap::MemoryBudget].detail = "DXGI QueryVideoMemoryInfo";
    const auto mib = [](std::uint64_t b) { return std::format("{:.0f} MiB", static_cast<double>(b) / 1048576.0); };
    r.properties.emplace_back("video memory (dedicated / shared)", mib(vramDedicated_) + " / " + mib(vramShared_));
    if (adapter3_) {
        DXGI_QUERY_VIDEO_MEMORY_INFO local{}, nonLocal{};
        if (SUCCEEDED(adapter3_->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &local)) &&
            SUCCEEDED(adapter3_->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_NON_LOCAL, &nonLocal))) {
            r.properties.emplace_back("video memory budget (local / system)", mib(local.Budget) + " / " + mib(nonLocal.Budget));
        }
    }
    r.properties.emplace_back("GPU upload heap (resizable BAR)", gpuUploadHeap_ ? "yes" : "no");
    r.properties.emplace_back("tearing (vsync off)", tearing_ ? "yes" : "no");
    return r;
}

bool D3D12DeviceImpl::videoMemory(std::uint64_t& used, std::uint64_t& budget) {
    DXGI_QUERY_VIDEO_MEMORY_INFO local{};
    if (!adapter3_ || FAILED(adapter3_->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &local))) return false;
    used = local.CurrentUsage;
    budget = local.Budget;
    return budget > 0;
}

void D3D12DeviceImpl::logMemoryStats() {
    std::uint64_t used = 0, size = 0;
    for (const auto& b : arena_) {
        used += b->used;
        size += b->size;
    }
    if (adapter3_) {
        DXGI_QUERY_VIDEO_MEMORY_INFO local{}, nonLocal{};
        if (SUCCEEDED(adapter3_->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &local)) &&
            SUCCEEDED(adapter3_->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_NON_LOCAL, &nonLocal))) {
            const auto mib = [](std::uint64_t b) { return static_cast<double>(b) / 1048576.0; };
            spdlog::info("perf: d3d12 video memory: local {:.0f} MiB used of a {:.0f} MiB budget (dedicated {:.0f} MiB), system {:.0f} of {:.0f} MiB{}", mib(local.CurrentUsage), mib(local.Budget),
                         mib(vramDedicated_), mib(nonLocal.CurrentUsage), mib(nonLocal.Budget), local.CurrentUsage > local.Budget ? " (OVER BUDGET)" : "");
        }
    }
    spdlog::info("perf: d3d12 mesh arena {} blocks, {:.1f} MiB used of {:.1f} MiB; {} pipelines ({} PSOs built in {:.0f} ms of build time, {} pending, {} draws skipped while pending)", arena_.size(),
                 static_cast<double>(used) / 1048576.0, static_cast<double>(size) / 1048576.0, entries_.size(), psoBuilt_.load(), static_cast<double>(psoNs_.load()) * 1e-6,
                 psoPending_.load(), psoSkipped_.load());
}

// ---- GPU timestamps ----------------------------------------------------------------------------------------------------------

bool D3D12DeviceImpl::enableTimestamps() {
    if (tsHeap_) return true;
    UINT64 freq = 0;
    if (FAILED(queue_->GetTimestampFrequency(&freq)) || freq == 0) {
        spdlog::warn("d3d12: GPU timestamps unavailable (no timestamp frequency)");
        return false;
    }
    D3D12_QUERY_HEAP_DESC qd{D3D12_QUERY_HEAP_TYPE_TIMESTAMP, kTsPerSlot * rhi::kFramesInFlight, 0};
    if (FAILED(device_->CreateQueryHeap(&qd, IID_PPV_ARGS(&tsHeap_)))) {
        spdlog::warn("d3d12: GPU timestamps unavailable (CreateQueryHeap failed)");
        return false;
    }
    tsReadback_ = createBuffer({static_cast<std::uint64_t>(kTsPerSlot) * rhi::kFramesInFlight * 8, VK_BUFFER_USAGE_TRANSFER_DST_BIT, Memory::Readback, "rvk timestamps"});
    if (!tsReadback_) {
        tsHeap_.Reset();
        return false;
    }
    tsNsPerTick_ = 1e9 / static_cast<double>(freq);
    return true;
}

void D3D12DeviceImpl::timestampStart(std::uint32_t slot, bool details) {
    if (timestampsEnabled() && slot < rhi::kFramesInFlight) ts_[slot].details = details;
}

void D3D12DeviceImpl::timestampMark(CommandList& list, std::uint32_t slot, TimestampStream stream, const char* label) {
    if (!timestampsEnabled() || slot >= rhi::kFramesInFlight) return;
    timestampMark(list, slot, stream, ts_[slot].details && label != nullptr ? std::string(label) : std::string());
}

void D3D12DeviceImpl::timestampMark(CommandList& list, std::uint32_t slot, TimestampStream stream, std::string&& label) {
    if (!timestampsEnabled() || slot >= rhi::kFramesInFlight) return;
    TsSlot& s = ts_[slot];
    if (s.marks.size() >= kTsPerSlot) return;
    auto* raw = static_cast<D3DCommandList&>(list).raw();
    raw->EndQuery(tsHeap_.Get(), D3D12_QUERY_TYPE_TIMESTAMP, slot * kTsPerSlot + static_cast<UINT>(s.marks.size()));
    s.marks.push_back({static_cast<std::uint8_t>(stream), s.details ? std::move(label) : std::string()});
}

void D3D12DeviceImpl::resolveTimestamps(ID3D12GraphicsCommandList* list, std::uint32_t slot) {
    const std::uint32_t n = static_cast<std::uint32_t>(ts_[slot].marks.size());
    if (n == 0) return;
    list->ResolveQueryData(tsHeap_.Get(), D3D12_QUERY_TYPE_TIMESTAMP, slot * kTsPerSlot, n, static_cast<D3DBuffer&>(*tsReadback_).resource(),
                           static_cast<UINT64>(slot) * kTsPerSlot * 8);
}

TimestampResult D3D12DeviceImpl::timestampCollect(std::uint32_t slot) {
    TimestampResult r;
    if (!timestampsEnabled() || slot >= rhi::kFramesInFlight) return r;
    TsSlot& s = ts_[slot];
    const std::size_t n = s.marks.size();
    if (n >= 2) {
        const auto* t = static_cast<const std::uint64_t*>(tsReadback_->map()) + static_cast<std::size_t>(slot) * kTsPerSlot;
        r.valid = true;
        std::uint64_t first[2] = {0, 0}, last[2] = {0, 0}, prevMain = 0;
        bool seen[2] = {false, false};
        for (std::size_t i = 0; i < n; ++i) {
            const std::size_t k = s.marks[i].stream;
            if (!seen[k]) {
                first[k] = t[i];
                seen[k] = true;
            } else if (s.details && k == 0) {
                r.segments.push_back({std::move(s.marks[i].label), static_cast<double>(t[i] - prevMain) * tsNsPerTick_ * 1e-6});
            }
            last[k] = t[i];
            if (k == 0) prevMain = t[i];
        }
        r.mainMs = static_cast<double>(last[0] - first[0]) * tsNsPerTick_ * 1e-6;
        r.uploadMs = static_cast<double>(last[1] - first[1]) * tsNsPerTick_ * 1e-6;
    }
    s.marks.clear();
    s.details = false;
    return r;
}

// ---- debug regions (PIX / RenderDoc events on the main list) -----------------------------------------------------------------

namespace {
void beginEvent(ID3D12GraphicsCommandList* list, const char* text) {
    // PIX3 blob (header-only encoder of the winpixevent port, no runtime DLL); the debug layer and DRED's breadcrumb contexts understand it
    UINT64 blob[PIXEventsGraphicsRecordSpaceQwords];
    UINT64* end = PixEventsLegacy::EncodeBeginEventForContext(blob, static_cast<UINT64>(PIX_COLOR_DEFAULT), "%s", text != nullptr ? text : "");
    list->BeginEvent(D3D12_EVENT_METADATA, blob, static_cast<UINT>(reinterpret_cast<BYTE*>(end) - reinterpret_cast<BYTE*>(blob)));
}
}  // namespace

void D3D12DeviceImpl::labelBegin(const char* text) {
    if (curMain_ != nullptr) beginEvent(curMain_, text);
}
void D3D12DeviceImpl::labelEnd() {
    if (curMain_ != nullptr) curMain_->EndEvent();
}

// a label in order with a threaded scope's chunks: its own tiny list, appended where the chunk would be
void D3D12DeviceImpl::scopeLabel(bool begin, const char* text) {
    if (!scope_.secondary) {
        if (begin) labelBegin(text);
        else labelEnd();
        return;
    }
    ID3D12GraphicsCommandList4* l = acquireList(labelPool_, frameSlot_);
    if (l == nullptr) return;
    if (begin) beginEvent(l, text);
    else l->EndEvent();
    l->Close();
    if (chunksUsed_ == chunkPool_.size()) chunkPool_.push_back(std::make_unique<ChunkEntry>());
    ChunkEntry* e = chunkPool_[chunksUsed_++].get();
    e->list = l;
    e->binds = 0;
    e->done.store(true, std::memory_order_relaxed);
    scopeChunks_.push_back(e);
}

// ---- the overlay's UI renderer: Dear ImGui's DX12 backend (third_party/imgui_dx12) --------------------------------------------

namespace {

class D3DImGuiBackend final : public ImGuiBackend {
public:
    explicit D3DImGuiBackend(D3D12DeviceImpl& dev) : dev_(dev) {}
    ~D3DImGuiBackend() override { shutdown(); }

    bool init(const ImGuiBackendDesc& desc) override {
        if (initialised_) return true;
        ImGui_ImplDX12_InitInfo ii;
        ii.Device = dev_.device();
        ii.CommandQueue = dev_.queue();
        // (its vertex / index buffers cycle; the draws of 2 frames may be in flight; with AMD's frame generation swapchain its present callback draws the UI
        // on the generated and the real frame: two renders per frame, and the swapchain's presents trail the render thread by a few frames)
        ii.NumFramesInFlight = (static_cast<int>(rhi::kFramesInFlight) + 2) * 2 + 1;
        ii.RTVFormat = toDxgi(desc.colorFormat);
        ii.DSVFormat = DXGI_FORMAT_UNKNOWN;
        ii.UserData = &dev_;
        ii.SrvDescriptorHeap = dev_.srvHeap();
        ii.SrvDescriptorAllocFn = [](ImGui_ImplDX12_InitInfo* i, D3D12_CPU_DESCRIPTOR_HANDLE* c, D3D12_GPU_DESCRIPTOR_HANDLE* g) {
            auto* d = static_cast<D3D12DeviceImpl*>(i->UserData);
            const std::uint32_t slot = d->srvAlloc().alloc();
            *c = d->srvCpu(slot);
            *g = d->srvGpu(slot);
        };
        ii.SrvDescriptorFreeFn = [](ImGui_ImplDX12_InitInfo* i, D3D12_CPU_DESCRIPTOR_HANDLE c, D3D12_GPU_DESCRIPTOR_HANDLE) {
            auto* d = static_cast<D3D12DeviceImpl*>(i->UserData);
            const std::uint32_t slot = static_cast<std::uint32_t>((c.ptr - d->srvCpu(0).ptr) / (d->srvCpu(1).ptr - d->srvCpu(0).ptr));
            d->retire([d, slot] { d->srvAlloc().release(slot); });  // (a frame in flight may still sample the texture)
        };
        initialised_ = ImGui_ImplDX12_Init(&ii);
        return initialised_;
    }
    void shutdown() override {
        if (!initialised_) return;
        dev_.waitIdle();
        ImGui_ImplDX12_Shutdown();
        initialised_ = false;
    }
    void newFrame() override { ImGui_ImplDX12_NewFrame(); }
    void updateTexture(ImTextureData* texture) override { ImGui_ImplDX12_UpdateTexture(texture); }
    void renderDrawData(ImDrawData* data, CommandList& cmd) override {
        auto& c = static_cast<D3DCommandList&>(cmd);
        ImGui_ImplDX12_RenderDrawData(data, c.raw());
        c.resetBindings();  // (ImGui set its own root signature / pipeline / vertex input)
        if (!d3d12ffx::inPresentCallback()) dev_.invalidateMainRec();  // (the frame generation swapchain's callback records on AMD's thread: not the render thread's recording)
    }

private:
    D3D12DeviceImpl& dev_;
    bool initialised_ = false;
};

}  // namespace

std::unique_ptr<ImGuiBackend> D3D12DeviceImpl::createImGuiBackend() { return std::make_unique<D3DImGuiBackend>(*this); }

// ---- the vertex capture pool ------------------------------------------------------------------------------------------------

void D3D12DeviceImpl::setCaptureState(ID3D12GraphicsCommandList* list, std::uint32_t slot, D3D12_RESOURCE_STATES to) {
    D3D12_RESOURCE_BARRIER b[16]{};
    UINT n = 0;
    const auto flushBarriers = [&] {
        if (n > 0) list->ResourceBarrier(n, b);
        n = 0;
    };
    for (CaptureChunk& c : capture_[slot].chunks) {
        if (c.buf->state == to) continue;
        b[n].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b[n].Transition = {c.buf->resource(), D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, c.buf->state, to};
        c.buf->state = to;
        if (++n == 16) flushBarriers();
    }
    flushBarriers();
}

RingAlloc D3D12DeviceImpl::allocCapture(std::uint64_t size, std::uint64_t alignment) {
    if (!gameCaps_.vertexStores) return {};
    CapturePool& p = capture_[frameSlot_];
    size = std::max<std::uint64_t>(size, 16);
    for (;;) {
        if (p.cur < p.chunks.size()) {
            CaptureChunk& c = p.chunks[p.cur];
            const std::uint64_t off = (c.used + alignment - 1) & ~(alignment - 1);
            if (off + size <= c.size) {
                c.used = off + size;
                RingAlloc a;
                a.buffer = reinterpret_cast<std::uint64_t>(c.buf.get());
                a.offset = off;
                a.ptr = static_cast<std::uint8_t*>(c.buf->map()) + off;
                a.ref = GpuRef::fromWords(c.buf->heapIndex(), static_cast<std::uint32_t>(off));
                a.refRw = GpuRef::fromWords(c.buf->heapIndexRw(), static_cast<std::uint32_t>(off));
                a.gpuVa = c.buf->gpuAddress() + off;
                return a;
            }
            ++p.cur;
            continue;
        }
        CaptureChunk c;
        c.size = alignUp(std::max<std::uint64_t>(16ull << 20, size + alignment), 65536);
        c.buf.reset(new D3DBuffer(*this, {c.size, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT, Memory::Device, "rvk vertex captures"}));
        c.buf->gpuUpload = true;
        if (!c.buf->create()) {
            spdlog::error("d3d12: the capture pool could not grow ({} bytes)", c.size);
            return {};
        }
        // (the new chunk is written by the draw that asked for it: UAV before it, in the main list = before the chunks of a threaded scope)
        if (curMain_ != nullptr) {
            D3D12_RESOURCE_BARRIER rb{};
            rb.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            rb.Transition = {c.buf->resource(), D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, c.buf->state, D3D12_RESOURCE_STATE_UNORDERED_ACCESS};
            c.buf->state = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
            curMain_->ResourceBarrier(1, &rb);
        }
        captureAllRead_ = false;
        p.chunks.push_back(std::move(c));
    }
}

void D3D12DeviceImpl::captureAccess(CommandList& list, bool write) {
    if (!gameCaps_.vertexStores) return;
    auto* l = static_cast<D3DCommandList&>(list).raw();
    if (write) {
        setCaptureState(l, frameSlot_, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        captureHold_ = true;
        captureAllRead_ = false;
    } else {
        captureHold_ = false;
        captureForRead(l, true);
    }
}

void D3D12DeviceImpl::captureForRead(ID3D12GraphicsCommandList* list, bool force) {
    if (!gameCaps_.vertexStores || captureAllRead_ || (captureHold_ && !force)) return;
    for (std::uint32_t s = 0; s < rhi::kFramesInFlight; ++s) setCaptureState(list, s, D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE);
    captureAllRead_ = true;
    captureHold_ = false;
}

void D3D12DeviceImpl::runRaytracingSelfTest() {
    spdlog::info("rt selftest: {} (D3D12 DXR: the acceleration structure / ray query paths are covered by rvk_rhitest)", gameCaps_.rayTracing ? "available" : "ray queries are not enabled");
}

// ---- device removal diagnostics ---------------------------------------------------------------------------------------------

void D3D12DeviceImpl::reportRemoved(const char* where) {
    // the reason, DRED's breadcrumbs / page fault and a minidump in bin\rvk\dumps, once (markRemoved)
    markRemoved(where, device_->GetDeviceRemovedReason());
}
