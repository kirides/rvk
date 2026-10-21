// HAL resource objects on d3d9::Device: textures, cube maps, volumes, vertex/index data, primitives, HLSL shaders.

#include <cstring>

#include <spdlog/spdlog.h>

#include "core/exports.h"
#include "hal/vulkan/hal_vulkan.h"
#include "vk/formats.h"

namespace rvk::hal::vulkan {

HalVertexData* asVertexData(eCAPIVertexData* p) { return static_cast<HalVertexData*>(p); }
HalIndexData* asIndexData(eCAPIIndexData* p) { return static_cast<HalIndexData*>(p); }
HalTexture* asTexture(eCAPITexture* p) { return static_cast<HalTexture*>(p); }

namespace {

bool lockLevel(d3d9::Texture* t, std::uint32_t level, std::uint32_t face, const tagRECT* rect, unsigned long flags,
               eCGfxShared::eSGfxLockedRect* out) {
    if (t == nullptr || out == nullptr) {
        return false;
    }
    d3d9::Texture::Locked lk;
    if (!t->lock(level, face, reinterpret_cast<const std::int32_t*>(rect), flags, lk)) {
        return false;
    }
    out->pitch = static_cast<std::int32_t>(lk.rowPitch);
    out->bits = lk.bits;
    return true;
}

// eETexFilter (D3DTEXF) -> linear blit?
bool linearFilter(eCGfxShared::eETexFilter f) { return static_cast<std::uint32_t>(f) >= d3d9::TEXF_LINEAR; }

}  // namespace

// ---- textures --------------------------------------------------------------------------------------------------------

HalTexture::HalTexture(HalDevice& dev, std::unique_ptr<d3d9::Texture> tex) : dev_(dev), tex_(std::move(tex)) {}
HalTexture::~HalTexture() { dev_.dev().release(std::move(tex_)); }

bEResult HalTexture::LockRect(unsigned int a0, eCGfxShared::eSGfxLockedRect* a1, tagRECT const* a2, unsigned long a3) {
    return lockLevel(tex_.get(), a0, 0, a2, a3, a1) ? bEResult_Ok : bEResult_False;
}

bEResult HalTexture::UnlockRect(unsigned int a0) {
    tex_->unlock(a0, 0);
    return bEResult_Ok;
}

bEResult HalTexture::LockImageLayer(unsigned int a0, unsigned long a1, int* a2, void** a3) {
    eCGfxShared::eSGfxLockedRect lr{};
    if (!lockLevel(tex_.get(), a0, 0, nullptr, a1, &lr)) {
        return bEResult_False;
    }
    if (a2 != nullptr) *a2 = lr.pitch;
    if (a3 != nullptr) *a3 = lr.bits;
    return bEResult_Ok;
}

bEResult HalTexture::UnlockImageLayer(unsigned int a0) { return UnlockRect(a0); }

bEResult HalTexture::GetRenderTargetHandle(void** a0) {
    if (a0 == nullptr) return bEResult_False;
    *a0 = tex_->surface(0, 0);
    return *a0 != nullptr ? bEResult_Ok : bEResult_False;
}

bEResult HalTexture::SetTexture(unsigned long a0) {
    if (logThisFrame()) spdlog::info("hal> Texture::SetTexture stage {} ({}x{} {})", a0, tex_->desc().width, tex_->desc().height,
                                     vk::d3dFormatName(tex_->desc().format));
    dev_.dev().setTexture(static_cast<std::uint32_t>(a0), tex_.get());
    return bEResult_Ok;
}

bEResult HalTexture::CopyCurrentRTToImage(eCGfxShared::eETexFilter a0, bCRect* a1, bCRect* a2) {
    dev_.dev().stretchRect(dev_.dev().renderTarget(0), reinterpret_cast<const std::int32_t*>(a1), tex_->surface(0, 0),
                           reinterpret_cast<const std::int32_t*>(a2), linearFilter(a0));
    return bEResult_Ok;
}

bEResult HalTexture::CopyImageToCurrentRT(eCGfxShared::eETexFilter a0, bCRect* a1, bCRect* a2) {
    dev_.dev().stretchRect(tex_->surface(0, 0), reinterpret_cast<const std::int32_t*>(a1), dev_.dev().renderTarget(0),
                           reinterpret_cast<const std::int32_t*>(a2), linearFilter(a0));
    return bEResult_Ok;
}

bEResult HalTexture::CopySurface(eCAPITexture* a0) {
    HalTexture* src = asTexture(a0);
    if (src == nullptr) return bEResult_False;
    dev_.dev().stretchRect(src->tex()->surface(0, 0), nullptr, tex_->surface(0, 0), nullptr, false);
    return bEResult_Ok;
}

bEResult HalTexture::GetRenderTargetData(eCAPITexture* a0, int a1) {
    HalTexture* dst = asTexture(a0);
    if (dst == nullptr) return bEResult_False;
    d3d9::Surface* src = tex_->surface(static_cast<std::uint32_t>(a1), 0);
    if (dst->tex()->isRenderTarget()) {
        dev_.dev().stretchRect(src, nullptr, dst->tex()->surface(0, 0), nullptr, false);
        return bEResult_Ok;
    }
    return dev_.dev().readRenderTarget(src, dst->tex(), 0) ? bEResult_Ok : bEResult_False;
}

bEResult HalTexture::GenerateTextureMipMaps(unsigned int a0, eCGfxShared::eECopyFilter a1) {
    (void)a1;
    // rebuild levels > a0 from level a0 by successive linear blits
    for (std::uint32_t l = a0 + 1; l < tex_->levels(); ++l) {
        dev_.dev().stretchRect(tex_->surface(l - 1, 0), nullptr, tex_->surface(l, 0), nullptr, true);
    }
    return bEResult_Ok;
}

HalCubeMap::HalCubeMap(HalDevice& dev, std::unique_ptr<d3d9::Texture> tex) : dev_(dev), tex_(std::move(tex)) {}
HalCubeMap::~HalCubeMap() { dev_.dev().release(std::move(tex_)); }

bEResult HalCubeMap::LockRect(eCGfxShared::eECubeMapFace a0, unsigned int a1, eCGfxShared::eSGfxLockedRect* a2, tagRECT const*,
                              unsigned long a4) {
    // eCDX9CubeMap::LockRect ignores the rect
    return lockLevel(tex_.get(), a1, static_cast<std::uint32_t>(a0), nullptr, a4, a2) ? bEResult_Ok : bEResult_False;
}

bEResult HalCubeMap::UnlockRect(eCGfxShared::eECubeMapFace a0, unsigned int a1) {
    tex_->unlock(a1, static_cast<std::uint32_t>(a0));
    return bEResult_Ok;
}

bEResult HalCubeMap::GetRenderTargetHandle(eCGfxShared::eECubeMapFace a0, void** a1) {
    if (a1 == nullptr) return bEResult_False;
    *a1 = tex_->surface(0, static_cast<std::uint32_t>(a0));
    return *a1 != nullptr ? bEResult_Ok : bEResult_False;
}

bEResult HalCubeMap::SetCubeMap(unsigned long a0) {
    dev_.dev().setTexture(static_cast<std::uint32_t>(a0), tex_.get());
    return bEResult_Ok;
}

HalVolume::HalVolume(HalDevice& dev, std::unique_ptr<d3d9::Texture> tex) : dev_(dev), tex_(std::move(tex)) {}
HalVolume::~HalVolume() { dev_.dev().release(std::move(tex_)); }

bEResult HalVolume::LockVolumeBox(unsigned int a0, unsigned long a1, int* a2, int* a3, void** a4) {
    d3d9::Texture::Locked lk;
    if (!tex_->lock(a0, 0, nullptr, a1, lk)) return bEResult_False;
    if (a2 != nullptr) *a2 = static_cast<int>(lk.rowPitch);
    if (a3 != nullptr) *a3 = static_cast<int>(lk.slicePitch);
    if (a4 != nullptr) *a4 = lk.bits;
    return bEResult_Ok;
}

bEResult HalVolume::UnlockVolumeBox(unsigned int a0) {
    tex_->unlock(a0, 0);
    return bEResult_Ok;
}

bEResult HalVolume::SetVolume(unsigned long a0) {
    dev_.dev().setTexture(static_cast<std::uint32_t>(a0), tex_.get());
    return bEResult_Ok;
}

// ---- buffers ---------------------------------------------------------------------------------------------------------

HalVertexData::HalVertexData(HalDevice& dev, const eCGfxShared::eSGfxVertexBufDesc& desc, std::unique_ptr<d3d9::Buffer> buffer)
    : dev_(dev), desc_(desc), buffer_(std::move(buffer)), stride_(desc.stride) {}
HalVertexData::~HalVertexData() { dev_.dev().release(std::move(buffer_)); }

bEResult HalVertexData::LockBuffer(unsigned int a0, unsigned int a1, unsigned long a2, unsigned char** a3) {
    if (a3 == nullptr) return bEResult_False;
    *a3 = static_cast<unsigned char*>(buffer_->lock(a0, a1, a2));
    return *a3 != nullptr ? bEResult_Ok : bEResult_False;
}

bEResult HalVertexData::UnlockBuffer() {
    buffer_->unlock();
    return bEResult_Ok;
}

bEResult HalVertexData::GetDesc(eCGfxShared::eSGfxVertexBufDesc* a0) {
    if (a0 == nullptr) return bEResult_False;
    a0->length = desc_.length;
    a0->usage = desc_.usage;
    a0->fvf = desc_.fvf;
    a0->pool = desc_.pool;  // eCDX9VertexData::GetDesc does not write the stride
    return bEResult_Ok;
}

bEResult HalVertexData::SetStride(unsigned long a0) {
    stride_ = static_cast<std::uint32_t>(a0);
    return bEResult_Ok;
}

HalIndexData::HalIndexData(HalDevice& dev, const eCGfxShared::eSGfxIndexBufDesc& desc, std::unique_ptr<d3d9::Buffer> buffer)
    : dev_(dev), desc_(desc), buffer_(std::move(buffer)) {}
HalIndexData::~HalIndexData() { dev_.dev().release(std::move(buffer_)); }

bEResult HalIndexData::LockBuffer(unsigned int a0, unsigned int a1, unsigned long a2, unsigned char** a3) {
    if (a3 == nullptr) return bEResult_False;
    *a3 = static_cast<unsigned char*>(buffer_->lock(a0, a1, a2));
    return *a3 != nullptr ? bEResult_Ok : bEResult_False;
}

bEResult HalIndexData::UnlockBuffer() {
    buffer_->unlock();
    return bEResult_Ok;
}

bEResult HalIndexData::GetDesc(eCGfxShared::eSGfxIndexBufDesc* a0) {
    if (a0 == nullptr) return bEResult_False;
    *a0 = desc_;
    return bEResult_Ok;
}

// ---- primitive -------------------------------------------------------------------------------------------------------

HalPrimitive::HalPrimitive(HalDevice& dev) : dev_(dev) {}

bool HalPrimitive::SetStream(unsigned long a0, eCAPIVertexData* a1) {
    if (a0 >= 4) return false;
    streams_[a0].data = asVertexData(a1);
    // the vertex data's stride at this moment (a later SetStride on the data does not change it)
    streams_[a0].stride = streams_[a0].data != nullptr ? streams_[a0].data->stride() : 0;
    return true;
}

bool HalPrimitive::SetIndexData(eCAPIIndexData* a0) {
    indices_ = asIndexData(a0);
    return true;
}

bool HalPrimitive::SetVertexDecl(void* a0) {
    decl_ = dev_.declFromHandle(a0);
    return true;
}

bool HalPrimitive::SetFVF(unsigned long a0) {
    static bool logged = false;
    if (!logged) {
        logged = true;
        spdlog::warn("hal: FVF draws (0x{:x}) are not supported", a0);
    }
    decl_ = nullptr;
    return true;
}

bool HalPrimitive::SetStreamSrcFrequency(unsigned long a0, unsigned long a1) {
    if (a0 >= 4) return false;
    streams_[a0].freq = static_cast<std::uint32_t>(a1);
    return true;
}

unsigned long HalPrimitive::GetStreamSrcFrequency(unsigned long a0) const { return a0 < 4 ? streams_[a0].freq : 0; }

bool HalPrimitive::SetStreamOffset(unsigned long a0, unsigned long a1) {
    if (a0 >= 4) return false;
    streams_[a0].offset = static_cast<std::uint32_t>(a1);
    return true;
}

unsigned long HalPrimitive::GetStreamOffset(unsigned long a0) const { return a0 < 4 ? streams_[a0].offset : 0; }

bool HalPrimitive::SetStreamStride(unsigned long a0, unsigned long a1) {
    if (a0 >= 4) return false;
    streams_[a0].stride = static_cast<std::uint32_t>(a1);
    return true;
}

unsigned long HalPrimitive::GetStreamStride(unsigned long a0) const { return a0 < 4 ? streams_[a0].stride : 0; }

bool HalPrimitive::SetDrawCallDescription(eCGfxShared::eSGfxDrawCallDesc const& a0) {
    draw_ = a0;
    return true;
}

eCGfxShared::eSGfxDrawCallDesc const& HalPrimitive::GetDrawCallDescription_const() const { return draw_; }
eCGfxShared::eSGfxDrawCallDesc& HalPrimitive::GetDrawCallDescription_12() { return draw_; }

void HalPrimitive::EnableStream(unsigned long a0, bool a1) {
    if (a0 < 4) streams_[a0].enabled = a1;
}

bool HalPrimitive::IsStreamEnabled(unsigned long a0) const { return a0 < 4 && streams_[a0].enabled; }

bool HalPrimitive::Execute() {
    if (logThisFrame()) spdlog::info("hal> Primitive::Execute decl {} stream0 {} stride {}", static_cast<void*>(decl_),
                                     static_cast<void*>(streams_[0].data), streams_[0].stride);
    d3d9::Device& d = dev_.dev();
    for (std::uint32_t i = 0; i < 4; ++i) {
        const Stream& s = streams_[i];
        if (s.data == nullptr || !s.enabled) {
            d.setStreamSource(i, nullptr, 0, 0);
            continue;
        }
        d.setStreamSource(i, s.data->buffer(), s.offset, s.stride);
        d.setStreamSourceFreq(i, s.freq);  // only for enabled streams (eCDX9Primitive::Execute)
    }
    d.setIndices(indices_ != nullptr ? indices_->buffer() : nullptr);
    d.setVertexDecl(decl_);
    return dev_.Draw(draw_) == bEResult_Ok;
}

// ---- HLSL shaders ----------------------------------------------------------------------------------------------------

HalHLSLShader::HalHLSLShader(HalDevice& dev) : dev_(dev) {}

bool HalHLSLShader::create(std::vector<std::uint32_t> bytecode, std::uint32_t type) {
    bytecode_ = std::move(bytecode);
    type_ = type;
    shader_.reset();
    if (bytecode_.empty()) {
        return false;
    }
    shader_ = dev_.dev().createShader(bytecode_);
    return shader_ != nullptr;
}

bEResult HalHLSLShader::Read(bCIStream& a0) {
    // record (eCDX9HLSLShader::Read 0x74a9a0): u32 size, size bytes of bytecode, u32 eEGfxHLSLShaderType; bCIStream's
    // virtual slot 1 is Read(void* dst, unsigned long bytes)
    using ReadFn = unsigned long (*)(void* stream, void* dst, unsigned long bytes);
    void* stream = &a0;
    const ReadFn read = (*reinterpret_cast<ReadFn**>(stream))[1];
    std::uint32_t size = 0;
    read(stream, &size, 4);
    if (size == 0) {
        bytecode_.clear();
        shader_.reset();
        return bEResult_Ok;
    }
    std::vector<std::uint32_t> code((size + 3) / 4);
    read(stream, code.data(), size);
    std::uint32_t type = 0;
    read(stream, &type, 4);
    if (!create(std::move(code), type)) {
        static int logged = 0;
        if (logged++ < 10) {
            spdlog::warn("hal: a pooled HLSL shader (type {}, {} bytes) could not be translated", type, size);
        }
    }
    return bEResult_Ok;
}

bEResult HalHLSLShader::Write(bCOStream&) {
    // the engine writes runtime-compiled shaders into Data/Compiled/Materials/ShaderMaterialPool_User.smp; in the test
    // copies that folder is a junction into the real install: refuse (docs/research/hal.md, CLAUDE.md rule 1)
    return bEResult_False;
}

namespace {

// eCDX9HLSLShader on a stand-in device: the engine's own Compile (D3DXCompileShader with the engine's include handler and
// macros) without a D3D9 device. Its CreateShader slot is patched to a no-op that marks the shader as created.
struct Dx9ShaderCompiler {
    using CtorFn = void* (*)(void* self, void* dx9Device);
    using DtorFn = void (*)(void* self);
    using CompileFn = bEResult (*)(void* self, const eCGfxShared::eSGfxHLSLShaderSrc* src);
    using DataFn = bEResult (*)(const void* self, void** data, unsigned long* size);

    CtorFn ctor = nullptr;
    DtorFn dtor = nullptr;
    std::array<void*, 26> vtable{};
    bool ready = false;
    std::mutex mutex;

    static bEResult createShaderStub(void* self) {
        *reinterpret_cast<std::uint64_t*>(static_cast<std::uint8_t*>(self) + 0x30) = 1;
        return bEResult_Ok;
    }

    bool init() {
        if (ready) return true;
        ctor = exports::resolveAs<CtorFn>(L"Engine.dll", "??0eCDX9HLSLShader@@IEAA@PEAVeCDX9Device@@@Z");
        dtor = exports::resolveAs<DtorFn>(L"Engine.dll", "??1eCDX9HLSLShader@@UEAA@XZ");
        void* const* vt = exports::resolveAs<void* const*>(L"Engine.dll", "??_7eCDX9HLSLShader@@6B@");
        if (ctor == nullptr || dtor == nullptr || vt == nullptr) {
            return false;
        }
        for (std::size_t i = 0; i < vtable.size(); ++i) vtable[i] = vt[i];
        vtable[22] = reinterpret_cast<void*>(&createShaderStub);  // CreateShader
        ready = true;
        return true;
    }

    bool compile(const eCGfxShared::eSGfxHLSLShaderSrc& src, std::vector<std::uint32_t>& out, std::uint32_t& type) {
        const std::lock_guard lock(mutex);
        if (!init()) return false;
        alignas(16) std::uint8_t standinDevice[0x40]{};  // +0x10 (IDirect3DDevice9*) = null
        alignas(16) std::uint8_t obj[0x40]{};
        ctor(obj, standinDevice);
        *reinterpret_cast<void**>(obj) = vtable.data();
        const bEResult r = reinterpret_cast<CompileFn>(vtable[0])(obj, &src);
        bool ok = false;
        if (r == bEResult_Ok) {
            void* data = nullptr;
            unsigned long size = 0;
            reinterpret_cast<DataFn>(vtable[4])(obj, &data, &size);
            if (data != nullptr && size >= 4) {
                out.assign(static_cast<const std::uint32_t*>(data), static_cast<const std::uint32_t*>(data) + size / 4);
                type = *reinterpret_cast<std::uint32_t*>(obj + 0x28);
                ok = true;
            }
        }
        *reinterpret_cast<std::uint64_t*>(obj + 0x30) = 0;  // the fake "shader object" must not be released
        dtor(obj);
        return ok;
    }
};

Dx9ShaderCompiler g_compiler;

}  // namespace

bEResult HalHLSLShader::Compile(eCGfxShared::eSGfxHLSLShaderSrc const& a0) {
    std::vector<std::uint32_t> code;
    std::uint32_t type = 0;
    if (!g_compiler.compile(a0, code, type)) {
        spdlog::warn("hal: HLSL compile failed");
        return bEResult_False;
    }
    return create(std::move(code), type) ? bEResult_Ok : bEResult_False;
}

bEResult HalHLSLShader::GetShaderByteCodeSize(long& a0) const {
    a0 = static_cast<long>(bytecode_.size() * 4);
    return bEResult_Ok;
}

bEResult HalHLSLShader::GetFunctionData(void** a0, unsigned long& a1) const {
    if (bytecode_.empty()) return bEResult_False;
    if (a0 != nullptr) *a0 = const_cast<std::uint32_t*>(bytecode_.data());
    a1 = static_cast<unsigned long>(bytecode_.size() * 4);
    return bEResult_Ok;
}

bEResult HalHLSLShader::GetShaderType(eCGfxShared::eEGfxHLSLShaderType& a0) const {
    if (bytecode_.empty()) return bEResult_False;
    a0 = static_cast<eCGfxShared::eEGfxHLSLShaderType>(type_ & 1);  // 0 vertex, 1 pixel
    return bEResult_Ok;
}

bEResult HalHLSLShader::Execute() {
    if (logThisFrame()) spdlog::info("hal> HLSLShader::Execute {} {:016x}", (type_ & 1) ? "ps" : "vs", shader_ ? shader_->hash : 0);
    if ((type_ & 1) != 0) {
        dev_.dev().setPixelShader(shader_);
    } else {
        dev_.dev().setVertexShader(shader_);
    }
    return shader_ != nullptr ? bEResult_Ok : bEResult_False;
}

bEResult HalHLSLShader::DestroyShader() { return bEResult_Ok; }
bEResult HalHLSLShader::CreateShader() { return shader_ != nullptr ? bEResult_Ok : bEResult_False; }

}  // namespace rvk::hal::vulkan
