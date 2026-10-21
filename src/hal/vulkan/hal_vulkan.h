#pragma once

// The engine's graphics HAL (eCAPIInstance, eCAPIDevice, eCAPITexture, ...) implemented on d3d9::Device (mode = "vulkan").
// Semantics follow the engine's only other implementation, eCDX9* (docs/research/hal-structs.md): every enum is a D3D9 value,
// master states guard the normal setters, effects bypass them inside a render-state block, render-target changes unbind the
// 16 texture stages. Slots without an override are the generated stubs (log once, neutral result).

#include <array>
#include <chrono>
#include <filesystem>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <vector>

#include "d3d9/device.h"
#include "hal/stub_generated.h"
#include "hal/types.h"

namespace rvk::hal::vulkan {

class HalDevice;

struct Options {
    bool validation = false;
    int adapter = -1;
    std::filesystem::path cacheDir;  // <bin>/rvk/cache
    std::vector<std::uint32_t> screenshotFrames;  // frames whose back buffer is saved as PNG
    std::filesystem::path screenshotDir;           // <bin>/rvk/screenshots
    std::uint32_t logDraws = 0;
    std::uint32_t logFrame = 0;  // log the HAL calls of this frame
};

// true while the frame selected by Options::logFrame is being recorded
bool logThisFrame();

class HalInstance final : public StubInstance {
public:
    explicit HalInstance(const Options& options);
    bEResult Init(eCGfxShared::eEAPIType a0) override;
    bEResult Finit() override;
    bEResult GetAdapterCount(unsigned long* a0) override;
    bEResult GetModeCount(unsigned long a0, unsigned long* a1) override;
    bEResult GetAdapterDesc(unsigned long a0, eCGfxShared::eSGfxAdapterDesc* a1) override;
    bEResult GetModeDesc(unsigned long a0, unsigned long a1, eCGfxShared::eSGfxModeDesc* a2) override;
    bEResult CreateDevice(unsigned long a0, eCAPIDevice::eSAPIStartParameters const& a1, eCAPIDevice** a2) override;

private:
    struct Mode {
        std::uint32_t width, height, refresh;
    };
    Options options_;
    std::vector<Mode> modes_;
};

class HalTexture final : public StubTexture {
public:
    HalTexture(HalDevice& dev, std::unique_ptr<d3d9::Texture> tex);
    ~HalTexture() override;
    d3d9::Texture* tex() const { return tex_.get(); }

    bEResult LockRect(unsigned int a0, eCGfxShared::eSGfxLockedRect* a1, tagRECT const* a2, unsigned long a3) override;
    bEResult UnlockRect(unsigned int a0) override;
    bEResult LockImageLayer(unsigned int a0, unsigned long a1, int* a2, void** a3) override;
    bEResult UnlockImageLayer(unsigned int a0) override;
    bEResult GetRenderTargetHandle(void** a0) override;
    bEResult SetTexture(unsigned long a0) override;
    bEResult CopyCurrentRTToImage(eCGfxShared::eETexFilter a0, bCRect* a1, bCRect* a2) override;
    bEResult CopyImageToCurrentRT(eCGfxShared::eETexFilter a0, bCRect* a1, bCRect* a2) override;
    bEResult CopySurface(eCAPITexture* a0) override;
    bEResult GetRenderTargetData(eCAPITexture* a0, int a1) override;
    bEResult GenerateTextureMipMaps(unsigned int a0, eCGfxShared::eECopyFilter a1) override;

private:
    HalDevice& dev_;
    std::unique_ptr<d3d9::Texture> tex_;
};

class HalCubeMap final : public StubCubeMap {
public:
    HalCubeMap(HalDevice& dev, std::unique_ptr<d3d9::Texture> tex);
    ~HalCubeMap() override;
    d3d9::Texture* tex() const { return tex_.get(); }

    bEResult LockRect(eCGfxShared::eECubeMapFace a0, unsigned int a1, eCGfxShared::eSGfxLockedRect* a2, tagRECT const* a3,
                      unsigned long a4) override;
    bEResult UnlockRect(eCGfxShared::eECubeMapFace a0, unsigned int a1) override;
    bEResult GetRenderTargetHandle(eCGfxShared::eECubeMapFace a0, void** a1) override;
    bEResult SetCubeMap(unsigned long a0) override;

private:
    HalDevice& dev_;
    std::unique_ptr<d3d9::Texture> tex_;
};

class HalVolume final : public StubVolume {
public:
    HalVolume(HalDevice& dev, std::unique_ptr<d3d9::Texture> tex);
    ~HalVolume() override;
    d3d9::Texture* tex() const { return tex_.get(); }

    bEResult LockVolumeBox(unsigned int a0, unsigned long a1, int* a2, int* a3, void** a4) override;
    bEResult UnlockVolumeBox(unsigned int a0) override;
    bEResult SetVolume(unsigned long a0) override;

private:
    HalDevice& dev_;
    std::unique_ptr<d3d9::Texture> tex_;
};

class HalVertexData final : public StubVertexData {
public:
    HalVertexData(HalDevice& dev, const eCGfxShared::eSGfxVertexBufDesc& desc, std::unique_ptr<d3d9::Buffer> buffer);
    ~HalVertexData() override;
    d3d9::Buffer* buffer() const { return buffer_.get(); }
    std::uint32_t stride() const { return stride_; }

    bEResult LockBuffer(unsigned int a0, unsigned int a1, unsigned long a2, unsigned char** a3) override;
    bEResult UnlockBuffer() override;
    bEResult GetDesc(eCGfxShared::eSGfxVertexBufDesc* a0) override;
    bEResult SetStride(unsigned long a0) override;

private:
    HalDevice& dev_;
    eCGfxShared::eSGfxVertexBufDesc desc_{};
    std::unique_ptr<d3d9::Buffer> buffer_;
    std::uint32_t stride_ = 0;
};

class HalIndexData final : public StubIndexData {
public:
    HalIndexData(HalDevice& dev, const eCGfxShared::eSGfxIndexBufDesc& desc, std::unique_ptr<d3d9::Buffer> buffer);
    ~HalIndexData() override;
    d3d9::Buffer* buffer() const { return buffer_.get(); }

    bEResult LockBuffer(unsigned int a0, unsigned int a1, unsigned long a2, unsigned char** a3) override;
    bEResult UnlockBuffer() override;
    bEResult GetDesc(eCGfxShared::eSGfxIndexBufDesc* a0) override;

private:
    HalDevice& dev_;
    eCGfxShared::eSGfxIndexBufDesc desc_{};
    std::unique_ptr<d3d9::Buffer> buffer_;
};

// a draw-call record: 4 streams, indices, declaration, draw description (eCDX9Primitive, no reference count)
class HalPrimitive final : public StubPrimitive {
public:
    explicit HalPrimitive(HalDevice& dev);

    bool SetStream(unsigned long a0, eCAPIVertexData* a1) override;
    bool SetIndexData(eCAPIIndexData* a0) override;
    bool SetVertexDecl(void* a0) override;
    bool SetFVF(unsigned long a0) override;
    bool SetStreamSrcFrequency(unsigned long a0, unsigned long a1) override;
    unsigned long GetStreamSrcFrequency(unsigned long a0) const override;
    bool SetStreamOffset(unsigned long a0, unsigned long a1) override;
    unsigned long GetStreamOffset(unsigned long a0) const override;
    bool SetStreamStride(unsigned long a0, unsigned long a1) override;
    unsigned long GetStreamStride(unsigned long a0) const override;
    bool SetDrawCallDescription(eCGfxShared::eSGfxDrawCallDesc const& a0) override;
    eCGfxShared::eSGfxDrawCallDesc const& GetDrawCallDescription_const() const override;
    eCGfxShared::eSGfxDrawCallDesc& GetDrawCallDescription_12() override;
    void EnableStream(unsigned long a0, bool a1) override;
    bool IsStreamEnabled(unsigned long a0) const override;
    bool Execute() override;

private:
    struct Stream {
        HalVertexData* data = nullptr;
        bool enabled = true;
        std::uint32_t offset = 0;
        std::uint32_t stride = 0;
        std::uint32_t freq = 1;
    };
    HalDevice& dev_;
    Stream streams_[4];
    HalIndexData* indices_ = nullptr;
    d3d9::VertexDecl* decl_ = nullptr;
    eCGfxShared::eSGfxDrawCallDesc draw_{};
};

class HalHLSLShader final : public StubHLSLShader {
public:
    explicit HalHLSLShader(HalDevice& dev);

    bEResult Compile(eCGfxShared::eSGfxHLSLShaderSrc const& a0) override;
    bEResult GetShaderByteCodeSize(long& a0) const override;
    bEResult GetFunctionData(void** a0, unsigned long& a1) const override;
    bEResult GetShaderType(eCGfxShared::eEGfxHLSLShaderType& a0) const override;
    bEResult Execute() override;
    bEResult DestroyShader() override;
    bEResult CreateShader() override;
    bEResult Read(bCIStream& a0) override;
    bEResult Write(bCOStream& a0) override;

private:
    bool create(std::vector<std::uint32_t> bytecode, std::uint32_t type);
    HalDevice& dev_;
    std::vector<std::uint32_t> bytecode_;
    std::uint32_t type_ = 0;  // eEGfxHLSLShaderType (bit 0: 1 = pixel)
    std::shared_ptr<d3d9::Shader> shader_;
};

class HalDevice final : public StubDevice {
public:
    HalDevice();
    ~HalDevice() override;
    bool create(const Options& options, const eCAPIDevice::eSAPIStartParameters& params);
    d3d9::Device& dev() { return dev_; }

    // render-state writes: `normal` setters respect the master flags; effects call setRenderState directly
    void setRenderState(std::uint32_t rs, std::uint32_t value);
    // 16-stage unbind done on every render-target change (eCDX9Device::SetRenderTarget)
    void unbindTextures();
    d3d9::VertexDecl* declFromHandle(void* handle);

    // ---- slots ----
    bEResult CreatePrimititve(eCAPIPrimitive** a0) override;
    bEResult CreateIndexData(eCGfxShared::eSGfxIndexBufDesc const& a0, eCAPIIndexData** a1) override;
    bEResult CreateVertexData(eCGfxShared::eSGfxVertexBufDesc const& a0, eCAPIVertexData** a1) override;
    bEResult CreateEffect(void const* a0, unsigned int a1, eCAPIEffect** a2) override;
    bEResult PreprocessShader(eCGfxShared::eSGfxHLSLShaderSrc const& a0, bCString& a1) override;
    bEResult CreateHLSLShader(eCAPIHLSLShader** a0) override;
    bEResult CreateTexture(eCGfxShared::eSGfxLayersDesc const& a0, eCAPITexture** a1) override;
    bEResult CreateImageFromMemory(eCGfxShared::eEPoolType a0, int a1, unsigned char a2, void const* a3, unsigned int a4,
                                   eCGfxShared::eSGfxLayersDesc& a5, void*& a6) override;
    bEResult CreateCubeMap(eCGfxShared::eSGfxLayersDesc const& a0, eCAPICubeMap** a1) override;
    bEResult CreateVolume(eCGfxShared::eSGfxLayersDesc const& a0, eCAPIVolume** a1) override;
    bEResult GetImageInfoFromMemory(void const* a0, unsigned int a1, eCGfxShared::eSGfxImageInfo& a2) override;
    bEResult CheckDeviceFormat(eCGfxShared::eSGfxLayersDesc const& a0, bool a1) override;
    bEResult ToggleNewMode(eCAPIDevice::eSAPIModeParameters const& a0) override;
    bEResult IsResetNecessary(bool* a0) override;
    bEResult IsResetNeeded(bool* a0) override;
    bEResult IsDeviceNotReset(bool* a0) override;
    bEResult Reset() override;
    bEResult EnableClipping(bool a0) override;
    bEResult GetClippingEnables(bool& a0) override;
    bEResult SetScissor(bCRect const& a0) override;
    bEResult GetScissor(bCRect& a0) override;
    bEResult EnableScissor(bool a0) override;
    bool IsScissorEnabled() override;
    bEResult SetViewport(eCGfxShared::eSGfxViewport const& a0) override;
    bEResult GetViewport(eCGfxShared::eSGfxViewport& a0) override;
    bEResult ClearViewport(eCGfxShared::eEViewportClear a0, unsigned long a1, float a2, unsigned long a3) override;
    bEResult SwapScreen(HWND__* a0, bCRect const* a1, bCRect const* a2) override;
    bEResult BeginScene() override;
    bEResult EndScene() override;
    bEResult SetMasterRenderMode(eCGfxShared::eEMasterRenderMode a0) override;
    bEResult GetMasterRenderMode(eCGfxShared::eEMasterRenderMode* a0) override;
    bEResult SetMasterShadeMode(eCGfxShared::eEMasterShadeMode a0) override;
    bEResult GetMasterShadeMode(eCGfxShared::eEMasterShadeMode* a0) override;
    bEResult SetMasterCullingMode(eCGfxShared::eEMasterCullingMode a0) override;
    bEResult GetMasterCullingMode(eCGfxShared::eEMasterCullingMode* a0) override;
    bEResult SetMasterDepthTesting(eCGfxShared::eEMasterCmpFunc a0) override;
    bEResult GetMasterDepthTesting(eCGfxShared::eEMasterCmpFunc* a0) override;
    bEResult SetMasterDepthWrite(eCGfxShared::eEMasterWrite a0) override;
    bEResult GetMasterDepthWrite(eCGfxShared::eEMasterWrite* a0) override;
    bEResult SetMasterDepthBias(bool a0, float a1, float a2) override;
    bEResult GetMasterDepthBias(bool* a0, float* a1, float* a2) override;
    bEResult SetMasterStencilTesting(eCGfxShared::eEMasterStencil a0, eCGfxShared::eSGfxStencilData const& a1) override;
    bEResult GetMasterStencilTesting(eCGfxShared::eEMasterStencil* a0, eCGfxShared::eSGfxStencilData* a1) override;
    bEResult SetMasterBlending(eCGfxShared::eEMasterBlending a0, eCGfxShared::eEMasterBlending a1) override;
    bEResult GetMasterBlending(eCGfxShared::eEMasterBlending* a0, eCGfxShared::eEMasterBlending* a1) override;
    bEResult SetMasterAlphaTesting(eCGfxShared::eEMasterAlphaTest a0, eCGfxShared::eSGfxAlphaTestData const& a1) override;
    bEResult GetMasterAlphaTesting(eCGfxShared::eEMasterAlphaTest* a0, eCGfxShared::eSGfxAlphaTestData* a1) override;
    bEResult SetMasterFogging(eCGfxShared::eEMasterFogging a0) override;
    bEResult GetMasterFogging(eCGfxShared::eEMasterFogging* a0) override;
    bEResult SetLighting(bool a0) override;
    bEResult GetLighting(bool* a0) override;
    bEResult SetAlphaBlending(bool a0) override;
    bEResult GetAlphaBlending(bool* a0) override;
    bEResult SetColorWriting(bool a0) override;
    bEResult GetColorWriting(bool* a0) override;
    bEResult SetAlphaWriting(bool a0) override;
    bEResult GetAlphaWriting(bool* a0) override;
    bEResult SetSpecularEffect(bool a0) override;
    bEResult GetSpecularEffect(bool* a0) override;
    bEResult SetGammaRamp(bool a0, eCGfxShared::eSGfxGammaRamp const* a1) override;
    bEResult GetGammaRamp(eCGfxShared::eSGfxGammaRamp* a0) override;
    bEResult SetRenderMode(eCGfxShared::eERenderMode a0) override;
    bEResult SetShadeMode(eCGfxShared::eEShadeMode a0) override;
    bEResult SetCullingMode(eCGfxShared::eECullingMode a0) override;
    bEResult SetDepthTesting(eCGfxShared::eECmpFunc a0) override;
    bEResult SetDepthWrite(eCGfxShared::eEWrite a0) override;
    bEResult SetDepthBias(float a0, float a1) override;
    bEResult SetStencilTesting(eCGfxShared::eEStencil a0, eCGfxShared::eSGfxStencilData const& a1) override;
    bEResult SetViewAndProjectionMatrices(bCMatrix const* a0, bCMatrix const* a1) override;
    bEResult SetWorldMatrix(bCMatrix const* a0) override;
    bEResult GetWorldMatrix(bCMatrix* a0) override;
    bEResult SetViewMatrix(bCMatrix const* a0) override;
    bEResult GetViewMatrix(bCMatrix* a0) override;
    bEResult Draw(eCGfxShared::eSGfxDrawCallDesc const& a0) override;
    bEResult DrawUP(eCGfxShared::eEPrimitiveType a0, unsigned int a1, void* const a2, unsigned int a3) override;
    bEResult DrawIndexedUP(eCGfxShared::eEPrimitiveType a0, unsigned int a1, unsigned int a2, unsigned int a3, void* const a4,
                           void* const a5, unsigned int a6) override;
    bEResult SetFog(eCGfxShared::eSGfxFogData& a0) override;
    bEResult SetColorFactor(unsigned long a0) override;
    bEResult SetBlending(eCGfxShared::eEBlending a0, eCGfxShared::eEBlending a1) override;
    bEResult SetAlphaTesting(eCGfxShared::eEAlphaTest a0, eCGfxShared::eSGfxAlphaTestData const& a1) override;
    bEResult CreateVertexDeclaration(eCGfxShared::eSGfxDclElement const* a0, void** a1) override;
    bEResult DestroyVertexDeclaration(void* a0) override;
    bEResult GetVertexDeclarationData(void* a0, eCGfxShared::eSGfxDclElement* a1, unsigned int* a2) override;
    bEResult SetVertexDeclaration(void* a0) override;
    bEResult SetVertexProgramRegisterF(unsigned long a0, float const* a1, unsigned long a2) override;
    bEResult SetVertexProgramRegisterI(unsigned long a0, int const* a1, unsigned long a2) override;
    bEResult SetVertexProgramRegisterB(unsigned long a0, int const* a1, unsigned long a2) override;
    bEResult GetVertexProgramRegisterF(unsigned long a0, float* a1, unsigned long a2) override;
    bEResult SetPixelProgramRegisterF(unsigned long a0, float const* a1, unsigned long a2) override;
    bEResult SetPixelProgramRegisterI(unsigned long a0, int const* a1, unsigned long a2) override;
    bEResult SetPixelProgramRegisterB(unsigned long a0, int const* a1, unsigned long a2) override;
    bEResult GetPixelProgramRegisterF(unsigned long a0, float* a1, unsigned long a2) override;
    bEResult CompileEffect(eCGfxShared::eSGfxEffectSource const& a0, void** a1, void** a2) override;
    bEResult CompileEffectFromFile(eCGfxShared::eSGfxEffectFileSource const& a0, void** a1, void** a2) override;
    bEResult CreateAuxBuffer(unsigned long a0, void** a1) override;
    bEResult DestroyAuxBuffer(void* a0) override;
    bEResult GetAuxBuffer(void* a0, void** a1, unsigned long* a2) override;
    bEResult SetTextureAdressing(unsigned long a0, eCGfxShared::eETexAddress a1, eCGfxShared::eETexAddress a2,
                                 eCGfxShared::eETexAddress a3) override;
    bEResult SetTextureFiltering(unsigned long a0, eCGfxShared::eETexFilter a1, eCGfxShared::eETexFilter a2,
                                 eCGfxShared::eETexFilter a3) override;
    bEResult SetSamplerState(unsigned long a0, eCGfxShared::eESamplerState a1, unsigned long a2) override;
    bEResult CreateDepthBuffer(unsigned long a0, unsigned long a1, eCGfxShared::eEDepthFormat a2, void** a3) override;
    bEResult DestroyDepthBuffer(void* a0) override;
    bEResult SetRenderTargetAt(void* a0, int a1) override;
    bEResult GetRenderTargetAt(void** a0, int a1) override;
    bEResult SetRenderTarget(void* a0, void* a1) override;
    bEResult GetRenderTarget(void** a0, void** a1) override;
    bEResult GetBackBuffer(void** a0, void** a1) override;
    bEResult SetClipPlane(unsigned long a0) override;
    bEResult SetClipPlaneData(unsigned long a0, eCGfxShared::eSGfxPlaneData& a1) override;
    bEResult GetClipPlaneData(unsigned long a0, eCGfxShared::eSGfxPlaneData& a1) override;
    bEResult SetTextureNULL(unsigned long a0) override;
    bEResult SetIndexDataNULL() override;
    bEResult SetVertexDataNULL(unsigned long a0) override;
    bEResult CreateOcclusionQuery(void*& a0) override;
    bEResult DestroyOcclusionQuery(void* const a0) override;
    bEResult BeginOcclusionTest(void* const a0) override;
    bEResult EndOcclusionTest(void* const a0) override;
    bEResult GetOcclusionTestInfo(void* const a0, unsigned long& a1, bool a2) override;
    void EnableOptimization() override {}
    void DisableOptimization() override {}
    void SetForceLowestMipMapLevel(bool a0) override { forceLowestMip_ = a0; }
    bool GetForceLowestMipMapLevel() override { return forceLowestMip_; }
    void BeginRenderStateBlock() override;
    void EndRenderStateBlock() override;
    // profiling callbacks: stored by eCDX9Device, never needed here
    void SetEffectCreateCallback(void (*)()) override {}
    void SetTextureCreateCallback(void (*)()) override {}
    void SetCubemapCreateCallback(void (*)()) override {}
    void SetVolumeCreateCallback(void (*)()) override {}
    void SetVertexBufferCreateCallback(void (*)()) override {}
    void SetIndexBufferCreateCallback(void (*)()) override {}
    void SetVertexShaderCreateCallback(void (*)()) override {}
    void SetPixelShaderCreateCallback(void (*)()) override {}
    void SetTextureLockCallback(void (*)()) override {}
    void SetCubemapLockCallback(void (*)()) override {}
    void SetVolumeLockCallback(void (*)()) override {}
    void SetVertexBufferLockCallback(void (*)()) override {}
    void SetIndexBufferLockCallback(void (*)()) override {}
    void SetRenderStateActCallback(void (*)()) override {}
    void SetTextureStageStateActCallback(void (*)()) override {}
    void SetSamplerStateActCallback(void (*)()) override {}
    void SetEffectActCallback(void (*)()) override {}
    void SetTextureActCallback(void (*)()) override {}
    void SetCubemapActCallback(void (*)()) override {}
    void SetVolumeActCallback(void (*)()) override {}
    void SetVertexShaderActCallback(void (*)()) override {}
    void SetPixelShaderActCallback(void (*)()) override {}
    void SetRenderTargetActCallback(void (*)()) override {}
    void SetDrawCallback(void (*)(unsigned long, unsigned long, eCGfxShared::eEPrimitiveType)) override {}
    void SetMemoryAllocCallback(void (*)(eCGfxShared::eEGraphicMemoryType, unsigned long, unsigned long, unsigned long,
                                         unsigned long, unsigned long, unsigned long, unsigned long, unsigned long, char*)) override {}
    void SetMemoryFreeCallback(void (*)(eCGfxShared::eEGraphicMemoryType, unsigned long)) override {}

private:
    enum Master { M_RENDERMODE, M_SHADE, M_CULL, M_DEPTHFUNC, M_DEPTHWRITE, M_DEPTHBIAS, M_STENCIL, M_BLEND, M_ALPHATEST,
                  M_FOG, M_COUNT };
    void normal(std::uint32_t rs, std::uint32_t value) { setRenderState(rs, value); }
    void applyStencil(bool enable, const eCGfxShared::eSGfxStencilData& d);
    void applyAlphaTest(bool enable, const eCGfxShared::eSGfxAlphaTestData& d);
    void logFrameStats();

    d3d9::Device dev_;
    std::array<bool, M_COUNT> master_{};
    bool blockRecording_ = false;
    std::array<bool, d3d9::RS_COUNT> blockSaved_{};
    std::array<std::uint32_t, d3d9::RS_COUNT> blockValues_{};
    int sceneDepth_ = 0;
    bool forceLowestMip_ = false;
    bCMatrix world_{};
    bCMatrix view_{};
    bCMatrix projection_{};
    eCGfxShared::eSGfxGammaRamp gamma_{};
    float clipPlanes_[6][4]{};
    // handles
    std::mutex handleMutex_;
    std::unordered_map<void*, std::unique_ptr<d3d9::VertexDecl>> decls_;
    std::unordered_map<void*, std::unique_ptr<d3d9::Texture>> depthBuffers_;
    std::uint32_t nextQuery_ = 1;
    // stand-in "this" for the engine's own CPU-only DX9 functions (PreprocessShader, CompileEffect*, aux buffers)
    std::uint64_t dx9Standin_[8]{};
    std::uint64_t lastStatsFrame_ = 0;
    std::chrono::steady_clock::time_point lastStatsTime_ = std::chrono::steady_clock::now();
    std::uint64_t halDraws_ = 0;
    void updateLogFrame();
    Options options_;
};

// the vertex/index data / texture behind a HAL object (nullptr for foreign objects)
HalVertexData* asVertexData(eCAPIVertexData* p);
HalIndexData* asIndexData(eCAPIIndexData* p);
HalTexture* asTexture(eCAPITexture* p);

}  // namespace rvk::hal::vulkan
