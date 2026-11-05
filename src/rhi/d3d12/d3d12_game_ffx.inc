// Included at the end of d3d12_device.cpp (namespace rvk::rhi, after the anonymous namespace): the seam of the FidelityFX modules (d3d12_ffx.h / .cpp)
// to this device, and the device's factory functions for them. FSR upscaling, frame generation and AMD's frame generation swapchain: WP7 of
// docs/research/dx12-game-port.md.

namespace d3d12ffx {

Native nativeOf(GameDevice& device) {
    auto& dev = static_cast<D3D12DeviceImpl&>(device);
    Native n;
    n.device = dev.device();
    n.queue = dev.queue();
    n.factory = dev.factory();
    n.tearing = dev.tearing_;
    return n;
}

ID3D12Resource* resourceOf(Texture& texture) { return static_cast<D3DTexture&>(texture).resource(); }

ID3D12GraphicsCommandList* listOf(CommandList& list) { return static_cast<D3DCommandList&>(list).raw(); }

std::unique_ptr<CommandList> wrapList(GameDevice& device, ID3D12GraphicsCommandList* list) {
    auto& dev = static_cast<D3D12DeviceImpl&>(device);
    ID3D12DescriptorHeap* heaps[2] = {dev.srvHeap(), dev.samplerHeap()};  // (a list of the vendor's has none set; the vendored ImGui backend relies on the caller's)
    list->SetDescriptorHeaps(2, heaps);
    return std::make_unique<D3DCommandList>(dev, list);
}

void listAfterForeign(GameDevice& device, CommandList& list) {
    auto& dev = static_cast<D3D12DeviceImpl&>(device);
    auto& c = static_cast<D3DCommandList&>(list);
    ID3D12DescriptorHeap* heaps[2] = {dev.srvHeap(), dev.samplerHeap()};
    c.raw()->SetDescriptorHeaps(2, heaps);  // (the DLL's backend set its own)
    c.resetBindings();
    dev.invalidateMainRec();
}

std::uint32_t dxgiFormatOf(VkFormat format) { return static_cast<std::uint32_t>(toDxgi(format)); }

}  // namespace d3d12ffx

std::unique_ptr<Upscaler> D3D12DeviceImpl::createUpscaler() { return d3d12ffx::createUpscaler(*this); }
std::unique_ptr<FrameGenerator> D3D12DeviceImpl::createFrameGenerator() { return d3d12ffx::createFrameGenerator(*this); }
void D3D12DeviceImpl::probeFrameGeneration(const std::filesystem::path& loaderDll) { fgProbe_ = d3d12ffx::probeFrameGeneration(*this, loaderDll); }
