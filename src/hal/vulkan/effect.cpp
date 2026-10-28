// eCAPIEffect on the fx_2_0 parser (src/fx): the engine's post-processing and sprite effects without D3DX at runtime
// (D3DX only compiles effects handed over as HLSL source, on the CPU). Semantics of eCDX9Effect (hal-structs.md 7):
// BeginTechnique opens the device's render-state block, EndTechnique restores the render states the passes set; pass
// states bypass the master flags.

#include <windows.h>

#include <cstring>
#include <mutex>
#include <unordered_map>

#include <spdlog/spdlog.h>

#include "core/exports.h"
#include "fx/effect_binary.h"
#include "hal/vulkan/hal_vulkan.h"

namespace rvk::hal::vulkan {

using namespace rvk::d3d9;

namespace {

// bCString::operator=(char const*) (SharedBase): engine strings in the description structs
void assignString(EngineString& s, const char* text) {
    using Fn = void* (*)(void* self, const char* text);
    static const Fn fn = exports::resolveAs<Fn>(L"SharedBase.dll", "??4bCString@@QEAAAEAV0@PEBD@Z");
    if (fn != nullptr) {
        fn(&s, text != nullptr ? text : "");
    }
}

struct D3DXBuffer {  // ID3DXBuffer
    virtual long __stdcall QueryInterface(const void*, void**) = 0;
    virtual unsigned long __stdcall AddRef() = 0;
    virtual unsigned long __stdcall Release() = 0;
    virtual void* __stdcall GetBufferPointer() = 0;
    virtual unsigned long __stdcall GetBufferSize() = 0;
};

// HLSL effect source -> fx_2_0 binary with d3dx9_39's CPU compiler (ID3DXEffectCompiler::CompileEffect = vtable slot 59)
std::vector<std::byte> compileEffectSource(const char* text, std::size_t size) {
    using CreateFn = long(__stdcall*)(const char*, UINT, const void*, void*, DWORD, void**, D3DXBuffer**);
    using CompileFn = long(__stdcall*)(void*, DWORD, D3DXBuffer**, D3DXBuffer**);
    HMODULE d3dx = GetModuleHandleW(L"d3dx9_39.dll");
    const auto create = d3dx != nullptr ? reinterpret_cast<CreateFn>(GetProcAddress(d3dx, "D3DXCreateEffectCompiler")) : nullptr;
    if (create == nullptr) {
        return {};
    }
    void* compiler = nullptr;
    D3DXBuffer* errors = nullptr;
    if (create(text, static_cast<UINT>(size), nullptr, nullptr, 0, &compiler, &errors) < 0 || compiler == nullptr) {
        if (errors != nullptr) {
            spdlog::warn("hal: effect source: {}", static_cast<const char*>(errors->GetBufferPointer()));
            errors->Release();
        }
        return {};
    }
    D3DXBuffer* out = nullptr;
    void** vtbl = *static_cast<void***>(compiler);
    const long hr = reinterpret_cast<CompileFn>(vtbl[59])(compiler, 0, &out, &errors);
    reinterpret_cast<unsigned long(__stdcall*)(void*)>(vtbl[2])(compiler);
    if (errors != nullptr) errors->Release();
    if (hr < 0 || out == nullptr) {
        return {};
    }
    std::vector<std::byte> bin(static_cast<const std::byte*>(out->GetBufferPointer()),
                               static_cast<const std::byte*>(out->GetBufferPointer()) + out->GetBufferSize());
    out->Release();
    return bin;
}

std::mutex g_effectsMutex;
std::unordered_map<const void*, const EffectInfo*> g_effects;  // live HalEffects by their eCAPIEffect pointer

}  // namespace

class HalEffect final : public StubEffect, public EffectInfo {
public:
    HalEffect(HalDevice& dev, fx::Effect effect) : dev_(dev), fx_(std::move(effect)) {
        {
            const std::lock_guard lock(g_effectsMutex);
            g_effects[static_cast<const eCAPIEffect*>(this)] = this;
        }
        values_ = fx::ParameterValues::defaults(fx_);
        params_.reserve(fx_.parameters.size() * 2);
        for (std::uint32_t i = 0; i < fx_.parameters.size(); ++i) {
            params_.push_back({i, kNoElement});
        }
        techniqueHandles_.resize(fx_.techniques.size());
        technique_ = fx_.techniques.empty() ? -1 : 0;  // D3DX starts with the first (valid) technique
        textures_.assign(fx_.parameters.size(), nullptr);
        for (const auto& s : fx_.shaders) {
            shaders_.push_back(dev_.dev().createShader(s.tokens));
        }
    }

    ~HalEffect() override {
        const std::lock_guard lock(g_effectsMutex);
        g_effects.erase(static_cast<const eCAPIEffect*>(this));
    }

    // EffectInfo (native/effect_ops)
    const fx::Effect& fxEffect() const override { return fx_; }
    const d3d9::Shader* fxShader(std::uint32_t k) const override { return k < shaders_.size() ? shaders_[k].get() : nullptr; }
    std::uint32_t parameterOf(const void* handle) const override {
        const Handle* h = handleOf(const_cast<void*>(handle));
        return h != nullptr && h->element == kNoElement ? h->param : fx::kNone;
    }
    const fx::ParameterValues& currentValues() const override { return values_; }
    const d3d9::Texture* currentTexture(std::uint32_t param) const override { return param < textures_.size() ? textures_[param] : nullptr; }
    int currentTechnique() const override { return technique_; }
    int techniqueOf(const void* handle) const override {
        for (std::size_t i = 0; i < techniqueHandles_.size(); ++i) {
            if (handle == &techniqueHandles_[i]) return static_cast<int>(i);
        }
        return -1;
    }

    bEResult OnResetDevice() override { return bEResult_Ok; }
    bEResult OnLostDevice() override { return bEResult_Ok; }

    bEResult SetTexture(void* const a0, eCAPITexture* a1) override {
        if (logThisFrame() && a1 != nullptr) {
            const d3d9::Texture* t = static_cast<HalTexture*>(a1)->tex();
            const auto& d = t->desc();
            const Handle* h = handleOf(a0);
            spdlog::info("hal> Effect::SetTexture {} ({}x{} {} tex {})", h != nullptr ? paramOf(*h).name : std::string("?"), d.width,
                         d.height, vk::d3dFormatName(d.format), static_cast<const void*>(t));
        }
        return setTex(a0, a1 != nullptr ? static_cast<HalTexture*>(a1)->tex() : nullptr);
    }
    bEResult SetCubeMap(void* const a0, eCAPICubeMap* a1) override {
        return setTex(a0, a1 != nullptr ? static_cast<HalCubeMap*>(a1)->tex() : nullptr);
    }
    bEResult SetVolume(void* const a0, eCAPIVolume* a1) override {
        return setTex(a0, a1 != nullptr ? static_cast<HalVolume*>(a1)->tex() : nullptr);
    }

    bEResult SetParameter(void* const a0, void* const a1, unsigned int a2) override {
        auto [data, size] = paramBytes(a0);
        if (data == nullptr || a1 == nullptr) return bEResult_False;
        std::memcpy(data, a1, std::min<std::size_t>(size, a2));
        return bEResult_Ok;
    }
    bEResult GetParameter(void* const a0, void* const a1, unsigned int a2) override {
        auto [data, size] = paramBytes(a0);
        if (data == nullptr || a1 == nullptr) return bEResult_False;
        std::memcpy(a1, data, std::min<std::size_t>(size, a2));
        return bEResult_Ok;
    }
    // the typed array setters: count = number of floats / ints / bools / vectors / matrices
    bEResult SetBoolArray(void* const a0, void* const a1, unsigned int a2) override { return SetParameter(a0, a1, a2 * 4); }
    bEResult GetBoolArray(void* const a0, void* const a1, unsigned int a2) override { return GetParameter(a0, a1, a2 * 4); }
    bEResult SetFloatArray(void* const a0, void* const a1, unsigned int a2) override { return SetParameter(a0, a1, a2 * 4); }
    bEResult GetFloatArray(void* const a0, void* const a1, unsigned int a2) override { return GetParameter(a0, a1, a2 * 4); }
    bEResult SetIntArray(void* const a0, void* const a1, unsigned int a2) override { return SetParameter(a0, a1, a2 * 4); }
    bEResult GetIntArray(void* const a0, void* const a1, unsigned int a2) override { return GetParameter(a0, a1, a2 * 4); }
    bEResult SetVectorArray(void* const a0, void* const a1, unsigned int a2) override { return SetParameter(a0, a1, a2 * 16); }
    bEResult GetVectorArray(void* const a0, void* const a1, unsigned int a2) override { return GetParameter(a0, a1, a2 * 16); }
    bEResult SetMatrixArray(void* const a0, void* const a1, unsigned int a2) override { return setMatrices(a0, a1, a2, false); }
    bEResult GetMatrixArray(void* const a0, void* const a1, unsigned int a2) override { return GetParameter(a0, a1, a2 * 64); }
    bEResult SetMatrixTransposeArray(void* const a0, void* const a1, unsigned int a2) override { return setMatrices(a0, a1, a2, true); }
    bEResult GetMatrixTransposeArray(void* const a0, void* const a1, unsigned int a2) override { return GetParameter(a0, a1, a2 * 64); }

    void* GetParameterByName(char const* a0) const override {
        if (a0 == nullptr) return nullptr;
        const std::uint32_t i = fx_.parameterIndex(a0);
        if (i == fx::kNone) spdlog::warn("hal: effect GetParameterByName('{}'): unknown", a0);
        return i == fx::kNone ? nullptr : const_cast<Handle*>(&params_[i]);
    }

    bEResult GetEffectDescription(eCGfxShared::eSGfxEffectDesc& a0) override {
        a0.parameters = static_cast<std::uint32_t>(fx_.parameters.size());
        a0.techniques = static_cast<std::uint32_t>(fx_.techniques.size());
        return bEResult_Ok;
    }

    bEResult GetParameterDescription(unsigned int a0, eCGfxShared::eSGfxParameterDesc& a1, void* a2) override {
        Handle* h = nullptr;
        if (a2 == nullptr) {
            if (a0 >= fx_.parameters.size()) return bEResult_False;
            h = &params_[a0];
        } else {
            const Handle* parent = handleOf(a2);
            if (parent == nullptr || parent->element != kNoElement) return bEResult_False;
            const fx::Parameter& pp = fx_.parameters[parent->param];
            if (a0 >= pp.members.size()) return bEResult_False;
            h = elementHandle(parent->param, a0);
        }
        const fx::Parameter& p = paramOf(*h);
        assignString(a1.name, p.name.c_str());
        assignString(a1.semantic, p.semantic.c_str());
        a1.handle = h;
        a1.parameterClass = static_cast<std::uint32_t>(p.cls);
        a1.parameterType = static_cast<std::uint32_t>(p.type);
        a1.rows = p.rows;
        a1.columns = p.columns;
        a1.elements = p.elements;
        a1.annotations = static_cast<std::uint32_t>(p.annotations.size());
        a1.structMembers = p.cls == fx::ParamClass::Struct ? static_cast<std::uint32_t>(p.members.size()) : 0u;
        a1.flags = p.flags;
        a1.bytes = fx::isNumeric(p.type) ? 4 * p.rows * p.columns * std::max(1u, p.elements) : 0u;
        return bEResult_Ok;
    }

    bEResult GetTechniqueDescription(unsigned int a0, eCGfxShared::eSGfxTechniqueDesc& a1) override {
        if (a0 >= fx_.techniques.size()) return bEResult_False;
        const fx::Technique& t = fx_.techniques[a0];
        assignString(a1.name, t.name.c_str());
        a1.handle = &techniqueHandles_[a0];
        a1.passes = static_cast<std::uint32_t>(t.passes.size());
        a1.annotations = static_cast<std::uint32_t>(t.annotations.size());
        return bEResult_Ok;
    }

    bEResult IsTechniqueValid(unsigned int a0, bool& a1) override {
        a1 = a0 < fx_.techniques.size();
        return bEResult_Ok;
    }
    bEResult IsParameterUsed(void* const, void* const, bool& a2) override {
        a2 = true;  // conservative: the engine then sets it (only a little extra work)
        return bEResult_Ok;
    }
    bEResult SetTechnique(void* const a0) override {
        if (a0 == nullptr) {
            return bEResult_False;  // the engine passes NULL before it knows its technique; D3DX rejects it too
        }
        for (std::size_t i = 0; i < techniqueHandles_.size(); ++i) {
            if (a0 == &techniqueHandles_[i]) {
                technique_ = static_cast<int>(i);
                return bEResult_Ok;
            }
        }
        static int logged = 0;
        if (logged++ < 10) {
            spdlog::warn("hal: effect SetTechnique({}) with an unknown handle ({} techniques)", a0, techniqueHandles_.size());
        }
        return bEResult_False;
    }
    bEResult BeginTechnique() override {
        if (logThisFrame()) spdlog::info("hal> Effect::BeginTechnique {}", technique_ >= 0 ? fx_.techniques[technique_].name : "-");
        dev_.BeginRenderStateBlock();
        pass_ = -1;
        return technique_ >= 0 ? bEResult_Ok : bEResult_False;
    }
    bEResult SetPassNumber(unsigned int a0) override {
        if (technique_ < 0 || a0 >= fx_.techniques[static_cast<std::size_t>(technique_)].passes.size()) {
            static int logged = 0;
            if (logged++ < 10) spdlog::warn("hal: effect SetPassNumber({}) without a valid technique ({})", a0, technique_);
            return bEResult_False;
        }
        if (logThisFrame()) spdlog::info("hal> Effect::SetPassNumber {}", a0);
        pass_ = static_cast<int>(a0);  // the same pass again = CommitChanges: re-apply (constants may have changed)
        applyPass(fx_.techniques[static_cast<std::size_t>(technique_)].passes[a0]);
        return bEResult_Ok;
    }
    bEResult EndTechnique() override {
        pass_ = -1;
        dev_.EndRenderStateBlock();
        return bEResult_Ok;
    }

private:
    static constexpr std::uint32_t kNoElement = 0xFFFFFFFFu;
    struct Handle {
        std::uint32_t param;
        std::uint32_t element;
    };

    const Handle* handleOf(void* h) const {
        const auto* p = static_cast<const Handle*>(h);
        for (const auto& x : params_) {
            if (&x == p) return p;
        }
        return nullptr;
    }
    Handle* elementHandle(std::uint32_t param, std::uint32_t element) {
        for (auto& x : params_) {
            if (x.param == param && x.element == element) return &x;
        }
        if (params_.size() == params_.capacity()) {
            spdlog::warn("hal: effect handle table full");
            return &params_[param];
        }
        params_.push_back({param, element});  // capacity reserved up front: existing handles stay valid
        return &params_.back();
    }
    const fx::Parameter& paramOf(const Handle& h) const {
        const fx::Parameter& p = fx_.parameters[h.param];
        return h.element == kNoElement ? p : p.members[h.element];
    }
    std::pair<std::byte*, std::size_t> paramBytes(void* handle) {
        const Handle* h = handleOf(handle);
        if (h == nullptr) return {nullptr, 0};
        auto& data = values_.data[h->param];
        const fx::Parameter& p = paramOf(*h);
        const std::size_t offset = h->element == kNoElement ? 0 : p.dataOffset;
        const std::size_t size = h->element == kNoElement ? data.size() : p.dataSize;
        if (offset + size > data.size()) return {nullptr, 0};
        return {data.data() + offset, size};
    }
    bEResult setTex(void* handle, d3d9::Texture* tex) {
        const Handle* h = handleOf(handle);
        if (h == nullptr) return bEResult_False;
        textures_[h->param] = tex;
        return bEResult_Ok;
    }
    bEResult setMatrices(void* handle, void* data, unsigned int count, bool transpose) {
        const Handle* h = handleOf(handle);
        if (h == nullptr || data == nullptr) return bEResult_False;
        // the value is kept in the parameter's own layout: column-major parameters store the transpose of SetMatrix
        const bool columns = fx_.parameters[h->param].cls == fx::ParamClass::MatrixColumns;
        std::vector<float> m(static_cast<const float*>(data), static_cast<const float*>(data) + count * 16);
        if (columns != transpose) {
            for (unsigned int k = 0; k < count; ++k) {
                float* a = m.data() + k * 16;
                for (int r = 0; r < 4; ++r)
                    for (int c = r + 1; c < 4; ++c) std::swap(a[r * 4 + c], a[c * 4 + r]);
            }
        }
        return SetParameter(handle, m.data(), count * 64);
    }

    std::uint32_t stateDword(const fx::State& s) const {
        switch (s.kind) {
        case fx::StateKind::Constant: return s.value.dwordAt(0);
        case fx::StateKind::ParamRef:
            if (s.referenceParam != fx::kNone && !values_.data[s.referenceParam].empty()) {
                std::uint32_t v = 0;
                std::memcpy(&v, values_.data[s.referenceParam].data(), 4);
                return v;
            }
            return 0;
        case fx::StateKind::Expression: {
            const auto v = fx::evaluateExpression(fx_, fx_.preshaders[s.expression], values_, s.value.type, 1);
            return v.empty() ? 0u : v[0];
        }
        default: return 0;
        }
    }

    // the texture parameter a `Texture = <x>` sampler state names
    d3d9::Texture* samplerTexture(const fx::State& s) const {
        if (s.kind == fx::StateKind::ParamRef && s.referenceParam != fx::kNone) return textures_[s.referenceParam];
        return nullptr;
    }

    void applySamplers(const fx::Shader& shader, std::uint32_t samplerBase) {
        d3d9::Device& d = dev_.dev();
        for (const auto& c : shader.constants.constants) {
            if (c.registerSet != fx::RegisterSet::Sampler) continue;
            const std::uint32_t pi = fx_.parameterIndex(c.name);
            if (pi == fx::kNone) continue;
            const fx::Parameter& sp = fx_.parameters[pi];
            const std::uint32_t reg = samplerBase + c.registerIndex;
            for (const auto& st : sp.samplerStates) {
                const fx::StateInfo* info = st.info();
                if (info == nullptr) continue;
                if (info->cls == fx::StateClass::Texture) {
                    d.setTexture(reg, samplerTexture(st));
                } else if (info->cls == fx::StateClass::SamplerState) {
                    d.setSamplerState(reg, info->op, stateDword(st));
                }
            }
        }
    }

    void applyConstants(const fx::Shader& shader, bool vertex) {
        const fx::ConstantRegisters regs = fx::evaluateShaderConstants(fx_, shader, values_);
        d3d9::Device& d = dev_.dev();
        const auto nf = static_cast<std::uint32_t>(regs.f.size() / 4);
        const auto ni = static_cast<std::uint32_t>(regs.i.size() / 4);
        const auto nb = static_cast<std::uint32_t>(regs.b.size());
        std::vector<std::int32_t> b(regs.b.begin(), regs.b.end());
        if (logThisFrame()) {
            // the pass's float registers (-LogFrame): what the engine set for this pass (e.g. the shadow mask's cascades)
            for (std::uint32_t r = 0; r < std::min(nf, 16u); ++r) {
                spdlog::info("hal>   {} c{} = {:.6g} {:.6g} {:.6g} {:.6g}", vertex ? "vs" : "ps", r, regs.f[r * 4], regs.f[r * 4 + 1],
                             regs.f[r * 4 + 2], regs.f[r * 4 + 3]);
            }
        }
        if (vertex) {
            if (nf) d.setVertexShaderConstantF(0, regs.f.data(), nf);
            if (ni) d.setVertexShaderConstantI(0, regs.i.data(), ni);
            if (nb) d.setVertexShaderConstantB(0, b.data(), nb);
        } else {
            if (nf) d.setPixelShaderConstantF(0, regs.f.data(), nf);
            if (ni) d.setPixelShaderConstantI(0, regs.i.data(), ni);
            if (nb) d.setPixelShaderConstantB(0, b.data(), nb);
        }
    }

    void applyPass(const fx::Pass& pass) {
        d3d9::Device& d = dev_.dev();
        const fx::Shader* vs = nullptr;
        const fx::Shader* ps = nullptr;
        for (const auto& s : pass.states) {
            const fx::StateInfo* info = s.info();
            if (info == nullptr) continue;
            switch (info->cls) {
            case fx::StateClass::RenderState: dev_.setRenderState(info->op, stateDword(s)); break;
            case fx::StateClass::VertexShader:
            case fx::StateClass::PixelShader: {
                const bool vertex = info->cls == fx::StateClass::VertexShader;
                std::shared_ptr<d3d9::Shader> sh;
                const fx::Shader* fs = nullptr;
                if (s.shader != fx::kNone && s.shader < shaders_.size()) {
                    sh = shaders_[s.shader];
                    fs = &fx_.shaders[s.shader];
                }
                if (vertex) {
                    d.setVertexShader(sh);
                    vs = fs;
                } else {
                    d.setPixelShader(sh);
                    ps = fs;
                }
                break;
            }
            case fx::StateClass::SamplerState: d.setSamplerState(s.index, info->op, stateDword(s)); break;
            case fx::StateClass::Texture: d.setTexture(s.index, samplerTexture(s)); break;
            default: break;  // fixed-function states (transforms, lights, material, FVF, stages) are not used by the game
            }
        }
        if (vs != nullptr) {
            applyConstants(*vs, true);
            applySamplers(*vs, kVertexSamplerD3D);
        }
        if (ps != nullptr) {
            applyConstants(*ps, false);
            applySamplers(*ps, 0);
        }
    }

    HalDevice& dev_;
    fx::Effect fx_;
    fx::ParameterValues values_;
    std::vector<Handle> params_;
    std::vector<std::uint8_t> techniqueHandles_;
    std::vector<d3d9::Texture*> textures_;
    std::vector<std::shared_ptr<d3d9::Shader>> shaders_;
    int technique_ = -1;
    int pass_ = -1;
};

const EffectInfo* EffectInfo::fromApi(const void* apiEffect) {
    if (apiEffect == nullptr) return nullptr;
    const std::lock_guard lock(g_effectsMutex);
    const auto it = g_effects.find(apiEffect);
    return it != g_effects.end() ? it->second : nullptr;
}

bEResult HalDevice::CreateEffect(void const* a0, unsigned int a1, eCAPIEffect** a2) {
    if (a0 == nullptr || a2 == nullptr) return bEResult_False;
    *a2 = nullptr;
    std::span<const std::byte> data(static_cast<const std::byte*>(a0), a1);
    std::vector<std::byte> compiled;
    if (!fx::isEffectBinary(data)) {
        // the engine's sprite effect arrives as HLSL source (D3DXCreateEffect accepts both)
        compiled = compileEffectSource(static_cast<const char*>(a0), a1);
        if (compiled.empty()) {
            spdlog::warn("hal: CreateEffect: source does not compile");
            return bEResult_False;
        }
        data = compiled;
    }
    auto parsed = fx::parseEffect(data);
    if (!parsed.ok()) {
        spdlog::warn("hal: CreateEffect: {}", parsed.error);
        return bEResult_False;
    }
    spdlog::info("hal: effect created ({} parameters, {} techniques, {} shaders{})", parsed.effect.parameters.size(),
                 parsed.effect.techniques.size(), parsed.effect.shaders.size(), compiled.empty() ? "" : ", compiled from source");
    *a2 = new HalEffect(*this, std::move(parsed.effect));
    return bEResult_Ok;
}

}  // namespace rvk::hal::vulkan
