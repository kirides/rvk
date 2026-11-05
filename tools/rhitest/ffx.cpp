// rvk_rhitest --fsr / --fg: AMD FidelityFX on the D3D12 backend (WP7 of docs/research/dx12-game-port.md; src/rhi/d3d12/d3d12_ffx.*), the D3D12
// counterpart of rvk_vktest --fsr / --fg. SKIP without the DX12 DLLs (third_party/amd_fidelityfx/signedbin or the path given).
//
//   --fsr [dll]  FSR through the rhi::Upscaler of the D3D12 device: create the FSR 3 context (and the ML one when the API offers it), twelve dispatches
//                on a constant image (native AA): every output pixel must keep the input colour; the jitter sequence is FSR's own.
//   --fg [dll]   frame generation: the manual model (rhi::FrameGenerator without a swapchain: configure / prepare / dispatch on constant inputs: the
//                generated frame is that colour too) and AMD's frame generation swapchain on a hidden window (create, present 40 frames with the
//                UI composition hook, the present callback ran for each, destroy in the documented order).
// Failures of the ML provider are noted, not counted (AMD's own code); `--validation` turns the D3D12 debug layer on (its messages go to the log).

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <directx/d3d12.h>
#include <dxgi1_6.h>
#include <wrl/client.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <memory>
#include <string>
#include <thread>

#include "enh/upscale.h"
#include "rhi/d3d12/d3d12_ffx.h"
#include "rhi/game_device.h"
#include "rhi/modules.h"
#include "rhi/rhi.h"

using namespace rvk;
using Microsoft::WRL::ComPtr;

namespace {

float halfToFloat(std::uint16_t h) {
    const std::uint32_t sign = (h >> 15) & 1u, exp = (h >> 10) & 31u, man = h & 1023u;
    float v;
    if (exp == 0) v = std::ldexp(static_cast<float>(man), -24);
    else if (exp == 31) v = man != 0 ? NAN : INFINITY;
    else v = std::ldexp(static_cast<float>(man | 1024u), static_cast<int>(exp) - 25);
    return sign != 0 ? -v : v;
}

std::filesystem::path defaultDll(const std::string& arg) {
    if (!arg.empty()) return arg;
#ifdef RVK_FFX_DLL_DX12
    return RVK_FFX_DLL_DX12;
#else
    return "amd_fidelityfx_loader_dx12.dll";
#endif
}

struct Env {
    std::unique_ptr<rhi::Device> dev;
    rhi::GameDevice* game = nullptr;
    int failures = 0;
    bool soft = false;
    bool check(bool ok, const char* what) {
        if (!ok) {
            std::printf("%s: %s%s\n", soft ? "NOTE" : "FAIL", soft ? "(ML) " : "", what);
            if (!soft) ++failures;
        }
        return ok;
    }
};

bool open(Env& e, bool validation) {
    rhi::DeviceDesc dd;
    dd.backend = rhi::Backend::D3D12;
    dd.validation = validation;
    dd.name = "rvk_rhitest ffx";
    e.dev = rhi::createD3D12Device(dd);
    if (!e.dev) {
        std::printf("SKIP: no D3D12 device meeting the floor\n");
        return false;
    }
    e.game = dynamic_cast<rhi::GameDevice*>(e.dev.get());
    if (e.game == nullptr) {
        std::printf("FAIL: the D3D12 device is not a game device\n");
        e.failures = 1;
        return false;
    }
    std::printf("adapter '%s'\n", e.dev->caps().adapterName.c_str());
    return true;
}

constexpr VkImageUsageFlags kImg = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;

std::unique_ptr<rhi::Texture> image(Env& e, VkFormat f, std::uint32_t w, std::uint32_t h, const char* name) {
    return e.dev->createTexture({VK_IMAGE_TYPE_2D, f, {w, h, 1}, 1, 1, kImg, name});
}

// reads RGBA16F / RG... back: the first three channels of every pixel against `want`; returns the number of channels off
int compareHalf(Env& e, rhi::Texture& tex, std::uint32_t w, std::uint32_t h, const float want[3], float tol, float* worstOut) {
    const std::uint32_t pitch = rhi::bufferRowPitch(VK_FORMAT_R16G16B16A16_SFLOAT, w);
    auto rb = e.dev->createBuffer({static_cast<std::uint64_t>(pitch) * h, VK_BUFFER_USAGE_TRANSFER_DST_BIT, rhi::Memory::Readback, "ffx readback"});
    e.dev->submitAndWait([&](rhi::CommandList& cl) {
        cl.transition(tex, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
        cl.copyTextureToBuffer(*rb, 0, tex);
    });
    const auto* base = static_cast<const std::uint8_t*>(rb->map());
    int bad = 0;
    float worst = 0.0f;
    for (std::uint32_t y = 0; y < h; ++y) {
        const auto* px = reinterpret_cast<const std::uint16_t*>(base + static_cast<std::size_t>(y) * pitch);
        for (std::uint32_t x = 0; x < w; ++x) {
            for (int c = 0; c < 3; ++c) {
                const float got = halfToFloat(px[x * 4 + c]);
                const float err = std::fabs(got - want[c]);
                worst = std::max(worst, err);
                if (err > tol && bad++ < 4) std::printf("pixel (%u, %u) channel %d = %f (want %f)\n", x, y, c, got, want[c]);
            }
        }
    }
    if (worstOut != nullptr) *worstOut = worst;
    return bad;
}

void clearInputs(rhi::CommandList& cl, rhi::Texture& color, rhi::Texture& depth, rhi::Texture& motion) {
    for (rhi::Texture* t : {&color, &depth, &motion}) cl.transition(*t, VK_IMAGE_LAYOUT_GENERAL);
    VkClearColorValue c{};
    c.float32[0] = 0.25f;
    c.float32[1] = 0.5f;
    c.float32[2] = 0.75f;
    c.float32[3] = 1.0f;
    cl.clearTexture(color, c);
    VkClearColorValue d{};
    d.float32[0] = 0.5f;
    cl.clearTexture(depth, d);
    cl.clearTexture(motion, VkClearColorValue{});
}

int fsrTest(const std::string& dllArg, bool validation) {
    const std::filesystem::path dll = defaultDll(dllArg);
    if (!std::filesystem::exists(dll)) {
        std::printf("SKIP: %s not found\n", dll.string().c_str());
        return 0;
    }
    Env e;
    if (!open(e, validation)) return e.failures;
    constexpr std::uint32_t kW = 256, kH = 144;
    std::unique_ptr<rhi::Upscaler> fsr = e.game->createUpscaler();
    if (e.check(fsr && fsr->load(dll), "load the DLL")) {
        std::printf("versions: %s\n", fsr->versions().c_str());
        const std::vector<rhi::UpscalerVersion> offered = fsr->versionList();
        bool haveMl = false;
        for (const rhi::UpscalerVersion& v : offered) {
            std::printf("  version 0x%llx '%s' (%d.%d.%d%s)\n", static_cast<unsigned long long>(v.id), v.name.c_str(), v.major, v.minor, v.patch, v.fromDriver ? ", driver" : "");
            haveMl = haveMl || v.major >= 4;
        }
        for (int pass = 0; pass < 2; ++pass) {
            const bool ml = pass == 1;
            e.soft = ml;
            if (ml && !haveMl) {
                std::printf("ML (FSR 4) upscaler: not offered on this device\n");
                continue;
            }
            std::printf("--- %s ---\n", ml ? "ML upscaler (FSR 4)" : "FSR 3 upscaler");
            rhi::UpscalerCreateInfo ci;
            ci.ml = ml;
            ci.maxRenderSize = ci.maxUpscaleSize = {kW, kH};
            ci.hdr = true;
            ci.debugChecking = true;
            if (!e.check(fsr->createContext(ci), "ffxCreateContext")) continue;
            std::printf("provider: %s\n", fsr->providerName().c_str());
            const int phases = fsr->jitterPhaseCount(kW, kW);
            std::printf("jitter phases (native AA): %d\n", phases);
            e.check(phases > 0, "jitter phase count");
            VkExtent2D rs{};
            e.check(fsr->renderSizeForMode({1920, 1080}, 2, rs) && rs.width > 1000 && rs.width < 1920, "render size for quality mode (1920x1080, balanced-ish)");

            auto color = image(e, VK_FORMAT_R16G16B16A16_SFLOAT, kW, kH, "fsr color");
            auto depth = image(e, VK_FORMAT_R32_SFLOAT, kW, kH, "fsr depth");
            auto motion = image(e, VK_FORMAT_R16G16_SFLOAT, kW, kH, "fsr motion");
            auto output = image(e, VK_FORMAT_R16G16B16A16_SFLOAT, kW, kH, "fsr output");
            if (!e.check(color && depth && motion && output, "image creation")) continue;
            constexpr int kFrames = 12;
            for (int frame = 0; frame < kFrames; ++frame) {
                e.dev->submitAndWait([&](rhi::CommandList& cl) {
                    clearInputs(cl, *color, *depth, *motion);
                    rhi::UpscalerDispatch d;
                    d.cmd = &cl;
                    d.color = color.get();
                    d.depth = depth.get();
                    d.motion = motion.get();
                    d.output = output.get();
                    d.renderSize = d.upscaleSize = {kW, kH};
                    d.reset = frame == 0;
                    d.cameraNear = 1.0f;
                    d.cameraFar = 1000.0f;
                    d.cameraFovY = 1.0f;
                    fsr->jitterOffset(frame % std::max(phases, 1), phases, d.jitterX, d.jitterY);
                    // our own sequence (enh/upscale: what the game jitters with) must be the DLL's
                    const auto mine = enh::upscaleJitter(static_cast<std::uint64_t>(frame), phases);
                    e.check(std::fabs(mine[0] - d.jitterX) < 1e-5f && std::fabs(mine[1] - d.jitterY) < 1e-5f, "jitter sequence = the DLL's");
                    e.check(phases == enh::upscaleJitterPhases(kW, kW), "jitter phase count = the DLL's");
                    e.check(fsr->dispatch(d), "ffxDispatch");
                });
            }
            const float want[3] = {0.25f, 0.5f, 0.75f};
            float worst = 0.0f;
            const int bad = compareHalf(e, *output, kW, kH, want, 0.02f, &worst);
            std::printf("output: %d channel(s) off, worst error %f\n", bad, worst);
            e.check(bad == 0, "output keeps the constant input");
            // the automatic reactive mask (FSR 3 only): a pass over (opaque, colour) into an R8 image
            if (!ml) {
                auto reactive = e.dev->createTexture({VK_IMAGE_TYPE_2D, VK_FORMAT_R8_UNORM, {kW, kH, 1}, 1, 1, kImg, "fsr reactive"});
                if (e.check(reactive != nullptr, "reactive image")) {
                    e.dev->submitAndWait([&](rhi::CommandList& cl) {
                        clearInputs(cl, *color, *depth, *motion);
                        e.check(fsr->generateReactiveMask(cl, *color, *color, *reactive, {kW, kH}), "generateReactiveMask");
                    });
                }
            }
            fsr->destroyContext();
        }
        e.soft = false;
        e.dev->submitAndWait([](rhi::CommandList&) {});
        fsr->destroy();
    }
    const std::uint32_t verrors = e.dev->validationErrors();
    std::printf("%s: fsr (D3D12): %d failure(s), %u debug layer error(s)\n", e.failures == 0 ? "PASS" : "FAIL", e.failures, verrors);
    return e.failures == 0 ? 0 : 1;
}

// ---- frame generation ---------------------------------------------------------------------------------------------------------------

int fgManual(Env& e, const std::filesystem::path& dll) {
    constexpr std::uint32_t kW = 256, kH = 144;
    std::unique_ptr<rhi::FrameGenerator> fg = e.game->createFrameGenerator();
    if (!e.check(fg && fg->load(dll), "load the DLL")) return 0;
    for (int pass = 0; pass < 2; ++pass) {
        const bool ml = pass == 1;
        e.soft = ml;
        std::printf("--- manual model, %s ---\n", ml ? "ML (FSR 4)" : "FSR 3");
        rhi::FrameGeneratorCreateInfo ci;
        ci.ml = ml;
        ci.displaySize = ci.maxRenderSize = {kW, kH};
        ci.backBufferFormat = VK_FORMAT_R16G16B16A16_SFLOAT;
        ci.hudlessFormat = VK_FORMAT_R16G16B16A16_SFLOAT;
        ci.debugChecking = true;
        if (!fg->createContext(ci)) {
            if (ml) std::printf("ML (FSR 4) frame generation: not offered on this device\n");
            else e.check(false, "ffxCreateContext");
            continue;
        }
        std::printf("provider: %s, %llu MiB\n", fg->providerName().c_str(), static_cast<unsigned long long>(fg->memoryBytes() >> 20));
        auto color = image(e, VK_FORMAT_R16G16B16A16_SFLOAT, kW, kH, "fg color");
        auto depth = image(e, VK_FORMAT_R32_SFLOAT, kW, kH, "fg depth");
        auto motion = image(e, VK_FORMAT_R16G16_SFLOAT, kW, kH, "fg motion");
        auto output = image(e, VK_FORMAT_R16G16B16A16_SFLOAT, kW, kH, "fg output");
        if (!e.check(color && depth && motion && output, "image creation")) continue;
        constexpr int kFrames = 8;
        for (int frame = 0; frame < kFrames; ++frame) {
            e.dev->submitAndWait([&](rhi::CommandList& cl) {
                clearInputs(cl, *color, *depth, *motion);
                rhi::FrameGeneratorConfigure cfg;
                cfg.frameId = static_cast<std::uint64_t>(frame) + 1;
                cfg.hudless = color.get();
                cfg.reset = frame == 0;
                e.check(fg->configure(cfg), "ffxConfigure");
                rhi::FrameGeneratorPrepare pr;
                pr.cmd = &cl;
                pr.frameId = cfg.frameId;
                pr.renderSize = {kW, kH};
                pr.reset = frame == 0;
                pr.cameraNear = 1.0f;
                pr.cameraFar = 1000.0f;
                pr.cameraFovY = 1.0f;
                pr.depth = depth.get();
                pr.motion = motion.get();
                e.check(fg->prepare(pr), "ffxDispatch(prepare)");
                rhi::FrameGeneratorDispatch dp;
                dp.cmd = &cl;
                dp.frameId = cfg.frameId;
                dp.color = color.get();
                dp.output = output.get();
                dp.reset = frame == 0;
                e.check(fg->dispatch(dp), "ffxDispatch(frame generation)");
            });
        }
        const float want[3] = {0.25f, 0.5f, 0.75f};
        float worst = 0.0f;
        const int bad = compareHalf(e, *output, kW, kH, want, 0.03f, &worst);
        std::printf("generated frame: %d channel(s) off, worst error %f\n", bad, worst);
        e.check(bad == 0, "the generated frame of constant inputs keeps the colour");
        e.dev->submitAndWait([](rhi::CommandList&) {});
        fg->destroyContext();
    }
    e.soft = false;
    fg->destroy();
    return 0;
}

// AMD's swapchain on a hidden window: present 40 frames cleared to a colour (the clear goes through the raw D3D12 interfaces: the swapchain's buffers are
// not RHI textures), count the present callbacks, destroy in order.
int fgSwapchain(Env& e, const std::filesystem::path& dll) {
    constexpr std::uint32_t kW = 320, kH = 180, kBuffers = 3;
    std::printf("--- frame generation swapchain ---\n");
    const rhi::d3d12ffx::Native n = rhi::d3d12ffx::nativeOf(*e.game);
    RECT rc{0, 0, static_cast<LONG>(kW), static_cast<LONG>(kH)};
    AdjustWindowRect(&rc, WS_OVERLAPPEDWINDOW, FALSE);
    HWND hwnd = CreateWindowExW(0, L"STATIC", L"rvk_rhitest ffx", WS_OVERLAPPEDWINDOW, 0, 0, rc.right - rc.left, rc.bottom - rc.top, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    std::unique_ptr<rhi::d3d12ffx::FgSwapchain> sc = rhi::d3d12ffx::FgSwapchain::make(*e.game);
    std::string reason;
    if (!e.check(sc->load(dll) && sc->create(hwnd, kW, kH, kBuffers, false, &reason), reason.empty() ? "create the swapchain" : reason.c_str())) {
        DestroyWindow(hwnd);
        return 0;
    }
    std::printf("provider: %s\n", sc->providerName().c_str());
    ComPtr<IDXGISwapChain3> sc3;
    e.check(SUCCEEDED(sc->swapchain()->QueryInterface(IID_PPV_ARGS(&sc3))), "IDXGISwapChain3");
    ComPtr<ID3D12DescriptorHeap> rtvHeap;
    D3D12_DESCRIPTOR_HEAP_DESC hd{};
    hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    hd.NumDescriptors = kBuffers;
    ID3D12Device* dev = static_cast<ID3D12Device*>(n.device);
    dev->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&rtvHeap));
    const UINT step = dev->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    ComPtr<ID3D12Resource> buffers[kBuffers];
    for (UINT i = 0; i < kBuffers; ++i) {
        if (!e.check(sc3 && SUCCEEDED(sc3->GetBuffer(i, IID_PPV_ARGS(&buffers[i]))), "GetBuffer")) break;
        D3D12_CPU_DESCRIPTOR_HANDLE h = rtvHeap->GetCPUDescriptorHandleForHeapStart();
        h.ptr += static_cast<SIZE_T>(i) * step;
        dev->CreateRenderTargetView(buffers[i].Get(), nullptr, h);
    }
    ComPtr<ID3D12CommandAllocator> alloc;
    ComPtr<ID3D12GraphicsCommandList> list;
    ComPtr<ID3D12Fence> fence;
    dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&alloc));
    dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc.Get(), nullptr, IID_PPV_ARGS(&list));
    list->Close();
    dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence));
    std::atomic<int> uiCalls{0};
    rhi::PresentUi ui;
    ui.wants = [] { return true; };
    ui.record = [&](rhi::CommandList&, VkExtent2D extent) {
        if (extent.width == kW && extent.height == kH) ++uiCalls;
    };
    sc->setUi(std::move(ui));
    // the callback model: the present callback exists once a generator context is configured with the swapchain (frame generation itself stays off)
    std::unique_ptr<rhi::FrameGenerator> gen = e.game->createFrameGenerator();
    {
        rhi::FrameGeneratorCreateInfo ci;
        ci.displaySize = ci.maxRenderSize = {kW, kH};
        ci.backBufferFormat = VK_FORMAT_B8G8R8A8_UNORM;
        ci.swapchain = true;
        ci.debugChecking = true;
        const std::lock_guard lock(sc->contextMutex());
        if (e.check(gen && gen->load(dll) && gen->createContext(ci), "generator context (callback model)")) {
            rhi::FrameGeneratorConfigure cfg;
            cfg.enabled = false;
            cfg.frameId = 1;
            cfg.present = sc.get();
            e.check(gen->configure(cfg), "ffxConfigure with the swapchain");
        }
    }
    constexpr int kFrames = 40;
    std::uint64_t fenceValue = 0;
    const HANDLE evt = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    for (int frame = 0; frame < kFrames; ++frame) {
        const UINT idx = sc3->GetCurrentBackBufferIndex();
        alloc->Reset();
        list->Reset(alloc.Get(), nullptr);
        D3D12_RESOURCE_BARRIER b{};
        b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b.Transition = {buffers[idx].Get(), D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_RENDER_TARGET};
        list->ResourceBarrier(1, &b);
        D3D12_CPU_DESCRIPTOR_HANDLE h = rtvHeap->GetCPUDescriptorHandleForHeapStart();
        h.ptr += static_cast<SIZE_T>(idx) * step;
        const float c[4] = {0.2f, 0.5f, 0.8f, 1.0f};
        list->ClearRenderTargetView(h, c, 0, nullptr);
        std::swap(b.Transition.StateBefore, b.Transition.StateAfter);
        list->ResourceBarrier(1, &b);
        list->Close();
        ID3D12CommandList* lists[1] = {list.Get()};
        n.queue->ExecuteCommandLists(1, lists);
        {
            const std::lock_guard lock(sc->contextMutex());
            const HRESULT hr = sc3->Present(0, 0);
            if (FAILED(hr)) e.check(false, "Present");
        }
        // our own frame pacing: wait for the frame (the swapchain's work after it is its own)
        n.queue->Signal(fence.Get(), ++fenceValue);
        fence->SetEventOnCompletion(fenceValue, evt);
        WaitForSingleObject(evt, 5000);
    }
    sc->waitForPresents();
    const rhi::PresentWrapper::Counters counters = sc->takeCounters();
    std::printf("present callbacks %llu (generated %llu, with UI %llu), UI hook calls %d, real presents %llu\n", static_cast<unsigned long long>(counters.callbacks),
                static_cast<unsigned long long>(counters.generatedFrames), static_cast<unsigned long long>(counters.uiFrames), uiCalls.load(),
                static_cast<unsigned long long>(sc->realPresentCount()));
    e.check(counters.callbacks >= static_cast<std::uint64_t>(kFrames) - 2, "a present callback per presented frame");
    e.check(static_cast<std::uint64_t>(uiCalls.load()) == counters.uiFrames && counters.uiFrames == counters.callbacks, "the UI hook ran in every callback");
    CloseHandle(evt);
    {
        const std::lock_guard lock(sc->contextMutex());
        gen->destroy();  // the generator's context goes first
    }
    // the documented order: the owner's references first, then WaitForPresents + the context + the swapchain
    for (auto& b : buffers) b.Reset();
    sc3.Reset();
    sc->destroy();
    e.check(!sc->valid(), "destroyed");
    DestroyWindow(hwnd);
    return 0;
}

int fgTest(const std::string& dllArg, bool validation) {
    const std::filesystem::path dll = defaultDll(dllArg);
    if (!std::filesystem::exists(dll) || !std::filesystem::exists(dll.parent_path() / "amd_fidelityfx_framegeneration_dx12.dll")) {
        std::printf("SKIP: %s (and amd_fidelityfx_framegeneration_dx12.dll next to it) not found\n", dll.string().c_str());
        return 0;
    }
    Env e;
    if (!open(e, validation)) return e.failures;
    const rhi::d3d12ffx::FrameGenProbe probe = rhi::d3d12ffx::probeFrameGeneration(*e.game, dll);
    std::printf("probe: fsr3 %d ml %d, providers [%s], swapchain [%s] %s\n", probe.fsr3, probe.ml, probe.versions.c_str(), probe.swapchain.c_str(), probe.reason.c_str());
    e.check(probe.fsr3, "the API offers an FSR 3 frame generation provider");
    fgManual(e, dll);
    fgSwapchain(e, dll);
    e.dev->submitAndWait([](rhi::CommandList&) {});
    const std::uint32_t verrors = e.dev->validationErrors();
    std::printf("%s: fg (D3D12): %d failure(s), %u debug layer error(s)\n", e.failures == 0 ? "PASS" : "FAIL", e.failures, verrors);
    return e.failures == 0 ? 0 : 1;
}

}  // namespace

namespace rvk::ffxtest {
int run(bool fsr, bool fg, const std::string& dll, bool validation) {
    std::setvbuf(stdout, nullptr, _IONBF, 0);  // a crash inside the DLL must not swallow the output
    int r = 0;
    if (fsr) r |= fsrTest(dll, validation);
    if (fg) r |= fgTest(dll, validation);
    return r;
}
}  // namespace rvk::ffxtest
