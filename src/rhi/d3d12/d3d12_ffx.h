#pragma once

// AMD FidelityFX on the D3D12 backend (docs/research/dx12-game-port.md WP7): FSR upscaling (rhi::Upscaler), frame generation (rhi::FrameGenerator) and
// AMD's frame generation swapchain (rhi::PresentWrapper) through the signed FFX API DLLs for D3D12 (third_party/amd_fidelityfx/signedbin:
// amd_fidelityfx_loader_dx12.dll loads amd_fidelityfx_upscaler_dx12.dll / amd_fidelityfx_framegeneration_dx12.dll from its own folder, bin\rvk).
// The same contracts as the Vulkan wrappers (vk/fsr.h, vk/framegen.h, vk/fg_swapchain.h); the textures are RHI textures whose D3D12 resources are
// reached through the seam below (defined in d3d12_device.cpp), the vendor's calls are D3D12 only (this file's .cpp includes the Windows SDK's d3d12.h,
// never the Agility SDK headers of the device TU: only pointers cross).
//
// Resource states: the DLL's D3D12 backend transitions each resource from the state given in the call and back to it, so every texture is first moved to
// the stable state the RHI knows (SHADER_READ_ONLY = ALL_SHADER_RESOURCE = FFX PIXEL_COMPUTE_READ, GENERAL = UNORDERED_ACCESS) with CommandList::transition.
// After a recording of the DLL the command list's descriptor heaps / root bindings are the DLL's: listAfterForeign restores the device's.

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>

#include "rhi/game_device.h"
#include "rhi/modules.h"

struct ID3D12Device;
struct ID3D12CommandQueue;
struct ID3D12Resource;
struct ID3D12GraphicsCommandList;
struct IDXGIFactory;
struct IDXGISwapChain4;

namespace rvk::rhi::d3d12ffx {

// ---- the seam to the device (d3d12_device.cpp, d3d12_game_ffx.inc) ----------------------------------------------------------------------

struct Native {
    ID3D12Device* device = nullptr;
    ID3D12CommandQueue* queue = nullptr;  // the game queue (direct)
    IDXGIFactory* factory = nullptr;
    bool tearing = false;                 // DXGI_FEATURE_PRESENT_ALLOW_TEARING
};
Native nativeOf(GameDevice& device);
ID3D12Resource* resourceOf(Texture& texture);
ID3D12GraphicsCommandList* listOf(CommandList& list);
// a CommandList over a list of the vendor's (the present callback's), for the UI renderer: raw() is that list, no state of ours is touched
std::unique_ptr<CommandList> wrapList(GameDevice& device, ID3D12GraphicsCommandList* list);
// after the DLL recorded into `list`: the descriptor heaps and root bindings of the device are set again, the recorded draw state is dropped
void listAfterForeign(GameDevice& device, CommandList& list);
std::uint32_t dxgiFormatOf(VkFormat format);  // DXGI_FORMAT
// true on an AMD thread inside the frame generation swapchain's present callback (the UI record): the render thread's command recording is not to be touched
bool inPresentCallback();

// ---- providers / probe -------------------------------------------------------------------------------------------------------------

// the startup probe of frame generation (caps lines): loads the loader + the effect DLL, lists the providers for this device, unloads again
struct FrameGenProbe {
    bool probed = false;
    bool dllPresent = false;
    bool fsr3 = false;
    bool ml = false;
    std::string versions;   // "id: name, ..." of the frame generation providers
    std::string swapchain;  // the swapchain providers and the pinned one
    std::string reason;     // why frame generation is unavailable (empty when fsr3)
};
FrameGenProbe probeFrameGeneration(GameDevice& device, const std::filesystem::path& loaderDll);

// ---- modules -----------------------------------------------------------------------------------------------------------------------

std::unique_ptr<Upscaler> createUpscaler(GameDevice& device);
std::unique_ptr<FrameGenerator> createFrameGenerator(GameDevice& device);

// AMD's frame generation swapchain (the presentation of the D3D12 presenter while frame generation is wanted)
class FgSwapchain : public PresentWrapper {
public:
    static std::unique_ptr<FgSwapchain> make(GameDevice& device);
    ~FgSwapchain() override = default;
    // loads the FidelityFX loader (and, through it, the frame generation DLL next to it); false (logged) when it cannot
    virtual bool load(const std::filesystem::path& loaderDll) = 0;
    virtual bool loaded() const = 0;
    // creates the swapchain context and its swapchain (flip model, `bufferCount` app-facing buffers); `reason` says why not
    virtual bool create(void* hwnd, std::uint32_t width, std::uint32_t height, std::uint32_t bufferCount, bool tearing, std::string* reason) = 0;
    // WaitForPresents, then the swapchain context (the frame generation context must be gone; the swapchain interface returned by swapchain() is released
    // by the owner after this: the owner's buffers first)
    virtual void destroy() = 0;
    virtual bool valid() const = 0;
    // the wrapped swapchain: one reference belongs to the caller of create()
    virtual IDXGISwapChain4* swapchain() const = 0;
    // the frame generation context's configure takes these (FrameGenerator::configure through PresentWrapper)
    virtual const void* presentCallback() const = 0;
    virtual void* presentUser() = 0;
};

}  // namespace rvk::rhi::d3d12ffx
