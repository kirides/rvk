// The D3D12 game device's presentation (rhi::GamePresenter): a DXGI flip model swapchain on the game window, presented from its own thread; the engine's
// back buffer is copied (same size: CopyTextureRegion) or drawn (scaled: blitDraw) into the acquired image. Frame generation / exclusive fullscreen / the
// overlay are later packages: they report themselves unavailable.

class D3D12Presenter final : public GamePresenter {
public:
    explicit D3D12Presenter(D3D12DeviceImpl& dev) : dev_(dev) {}
    ~D3D12Presenter() override { destroy(); }

    bool create(void* window, std::uint32_t width, std::uint32_t height, bool vsync) override {
        hwnd_ = static_cast<HWND>(window);
        vsync_ = vsync;
        DXGI_SWAP_CHAIN_DESC1 sd{};
        sd.Width = width;
        sd.Height = height;
        sd.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        sd.SampleDesc = {1, 0};
        sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        sd.BufferCount = kBuffers;
        sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
        sd.AlphaMode = DXGI_ALPHA_MODE_IGNORE;
        sd.Flags = dev_.tearing_ ? DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING : 0;
        ComPtr<IDXGISwapChain1> sc1;
        const HRESULT hr = dev_.factory()->CreateSwapChainForHwnd(dev_.queue(), hwnd_, &sd, nullptr, nullptr, &sc1);
        if (FAILED(hr) || FAILED(sc1.As(&sc_))) {
            spdlog::error("d3d12: CreateSwapChainForHwnd failed (0x{:08x})", static_cast<unsigned>(hr));
            return false;
        }
        dev_.factory()->MakeWindowAssociation(hwnd_, DXGI_MWA_NO_ALT_ENTER | DXGI_MWA_NO_WINDOW_CHANGES);
        width_ = width;
        height_ = height;
        if (!wrap()) return false;
        quit_ = false;
        thread_ = std::thread([this] { run(); });
        spdlog::info("d3d12: swapchain {}x{} flip model, {} buffers, {}", width, height, kBuffers, dev_.tearing_ ? "tearing allowed (vsync off)" : "no tearing support");
        return true;
    }
    void destroy() override {
        if (thread_.joinable()) {
            {
                const std::lock_guard lock(m_);
                quit_ = true;
            }
            cv_.notify_all();
            thread_.join();
        }
        images_.clear();
        sc_.Reset();
    }
    PresentInfo info() const override {
        PresentInfo i;
        i.valid = sc_ != nullptr;
        i.extent = {width_, height_};
        i.format = VK_FORMAT_B8G8R8A8_UNORM;
        i.minImageCount = 2;
        i.imageCount = kBuffers;
        i.presentMode = vsync_ ? 1 : 0;
        return i;
    }
    void setVsync(bool vsync) override { vsync_ = vsync; }  // a Present argument on D3D12: applies at once
    bool recreate(std::uint32_t width, std::uint32_t height) override {
        waitDone();
        dev_.waitIdle();
        for (auto& i : images_) i->release();
        images_.clear();
        const HRESULT hr = sc_->ResizeBuffers(kBuffers, width, height, DXGI_FORMAT_B8G8R8A8_UNORM, dev_.tearing_ ? DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING : 0);
        if (FAILED(hr)) {
            spdlog::error("d3d12: ResizeBuffers {}x{} failed (0x{:08x})", width, height, static_cast<unsigned>(hr));
            return false;
        }
        width_ = width;
        height_ = height;
        acquired_ = -1;
        spdlog::info("d3d12: swapchain resized to {}x{}", width, height);
        return wrap();
    }
    PresentStatus waitIdle(std::string* error) override {
        waitDone();
        const std::lock_guard lock(m_);
        const PresentStatus s = status_;
        if (error != nullptr) *error = errorText_;
        status_ = PresentStatus::Ok;
        errorText_.clear();
        return s;
    }
    void waitDone() override {
        std::unique_lock lock(m_);
        cv_.wait(lock, [this] { return !pending_; });
    }
    double takePresentMs() override {
        const std::lock_guard lock(m_);
        const double v = presentMs_;
        presentMs_ = 0.0;
        return v;
    }
    void setFrameGen(bool on, const std::filesystem::path&) override {
        if (on) reason_ = "frame generation is not available on the D3D12 backend yet";
    }
    bool frameGenWanted() const override { return false; }
    bool frameGenActive() const override { return false; }
    const std::string& frameGenReason() const override { return reason_; }
    void releaseFrameGen() override {}
    PresentWrapper* presentWrapper() override { return nullptr; }
    bool waitFrameGenPresent() override {
        waitDone();
        return true;
    }
    void setFullScreenExclusive(void*) override {}
    bool fullScreenExclusiveConfigured() const override { return false; }
    bool fullScreenExclusiveHeld() const override { return false; }
    bool acquireFullScreenExclusive(std::string* error) override {
        if (error != nullptr) *error = "exclusive fullscreen is not available on the D3D12 backend";
        return false;
    }
    void releaseFullScreenExclusive() override {}
    void noteExclusiveLost() override {}
    bool takeExclusiveLost() override { return false; }

    bool acquire() override {
        acquired_ = -1;
        if (sc_ == nullptr || IsIconic(hwnd_)) return false;
        RECT rc{};
        GetClientRect(hwnd_, &rc);
        if (rc.right > 0 && rc.bottom > 0 && (static_cast<std::uint32_t>(rc.right) != width_ || static_cast<std::uint32_t>(rc.bottom) != height_)) return false;  // "out of date"
        acquired_ = static_cast<int>(sc_->GetCurrentBackBufferIndex());
        return true;
    }
    bool exclusiveLost() const override { return false; }
    bool acquireNotReady() const override { return false; }
    void blitFrame(Texture& srcTex, VkExtent3D srcExtent) override {
        if (acquired_ < 0) return;
        D3DTexture& back = *images_[static_cast<std::size_t>(acquired_)];
        D3DTexture& src = static_cast<D3DTexture&>(srcTex);
        CommandList& cl = dev_.frameCommands();
        const bool same = srcExtent.width == width_ && srcExtent.height == height_ && src.desc().format == VK_FORMAT_B8G8R8A8_UNORM && src.desc().extent.width == width_ &&
                          src.desc().extent.height == height_;
        if (same) {
            cl.transition(src, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
            cl.transition(back, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
            const TextureCopy c{};
            cl.copyTexture(back, src, std::span<const TextureCopy>(&c, 1));
            return;
        }
        // a scaled / converting copy: the source sampled, the destination drawn (blitDraw moves the subresources itself)
        const float uv[4] = {0.0f, 0.0f, static_cast<float>(srcExtent.width) / static_cast<float>(src.desc().extent.width),
                             static_cast<float>(srcExtent.height) / static_cast<float>(src.desc().extent.height)};
        const D3D12_RECT dr{0, 0, static_cast<LONG>(width_), static_cast<LONG>(height_)};
        dev_.blitDraw(dev_.curMain_, back, {}, dr, src, {}, uv, true);
        dev_.invalidateMainRec();
    }
    void drawOverlay(PresentOverlay&) override {}  // (no UI backend on D3D12 yet: the overlay does not initialise)
    void finishFrame() override {
        if (acquired_ < 0) return;
        dev_.frameCommands().transition(*images_[static_cast<std::size_t>(acquired_)], VK_IMAGE_LAYOUT_PRESENT_SRC_KHR);
    }
    void queuePresent() override {
        if (acquired_ < 0 || sc_ == nullptr) return;
        {
            const std::lock_guard lock(m_);
            pending_ = true;
            interval_ = vsync_ ? 1u : 0u;
            flags_ = !vsync_ && dev_.tearing_ ? DXGI_PRESENT_ALLOW_TEARING : 0u;
        }
        cv_.notify_all();
        acquired_ = -1;
    }

private:
    static constexpr UINT kBuffers = 3;
    bool wrap() {
        TextureDesc td;
        td.format = VK_FORMAT_B8G8R8A8_UNORM;
        td.extent = {width_, height_, 1};
        td.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        td.name = "swapchain image";
        for (UINT i = 0; i < kBuffers; ++i) {
            ComPtr<ID3D12Resource> r;
            if (FAILED(sc_->GetBuffer(i, IID_PPV_ARGS(&r)))) return false;
            auto t = std::make_unique<D3DTexture>(dev_, td);
            t->adopt(std::move(r));
            t->state = D3D12_RESOURCE_STATE_PRESENT;
            t->lastLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
            images_.push_back(std::move(t));
        }
        return true;
    }
    void run() {
        SetThreadDescription(GetCurrentThread(), L"rvk present");
        for (;;) {
            UINT interval = 0, flags = 0;
            {
                std::unique_lock lock(m_);
                cv_.wait(lock, [this] { return quit_ || pending_; });
                if (quit_ && !pending_) return;
                interval = interval_;
                flags = flags_;
            }
            const auto t0 = std::chrono::steady_clock::now();
            const HRESULT hr = sc_->Present(interval, flags);
            const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
            {
                const std::lock_guard lock(m_);
                presentMs_ += ms;
                if (FAILED(hr)) {
                    status_ = PresentStatus::Error;
                    errorText_ = std::format("IDXGISwapChain::Present failed (0x{:08x})", static_cast<unsigned>(hr));
                }
                pending_ = false;
            }
            cv_.notify_all();
        }
    }

    D3D12DeviceImpl& dev_;
    HWND hwnd_ = nullptr;
    ComPtr<IDXGISwapChain3> sc_;
    std::vector<std::unique_ptr<D3DTexture>> images_;
    std::uint32_t width_ = 0, height_ = 0;
    std::atomic<bool> vsync_{true};
    int acquired_ = -1;
    std::string reason_ = "frame generation is not available on the D3D12 backend yet";
    std::thread thread_;
    std::mutex m_;
    std::condition_variable cv_;
    bool quit_ = false, pending_ = false;
    UINT interval_ = 1, flags_ = 0;
    PresentStatus status_ = PresentStatus::Ok;
    std::string errorText_;
    double presentMs_ = 0.0;
};
