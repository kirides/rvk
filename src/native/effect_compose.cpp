#include "native/effect_compose.h"

#include <algorithm>
#include <cstring>
#include <format>

#include "d3d9/defs.h"

namespace rvk::native::fxc {

namespace {

std::uint32_t stateDword(const fx::Effect& e, const fx::State& s, const fx::ParameterValues& values) {
    switch (s.kind) {
    case fx::StateKind::Constant: return s.value.dwordAt(0);
    case fx::StateKind::ParamRef:
        if (s.referenceParam != fx::kNone && s.referenceParam < values.data.size() && values.data[s.referenceParam].size() >= 4) {
            std::uint32_t v = 0;
            std::memcpy(&v, values.data[s.referenceParam].data(), 4);
            return v;
        }
        return 0;
    case fx::StateKind::Expression: {
        if (s.expression >= e.preshaders.size()) return 0;
        const auto v = fx::evaluateExpression(e, e.preshaders[s.expression], values, s.value.type, 1);
        return v.empty() ? 0u : v[0];
    }
    default: return 0;
    }
}

const void* samplerTexture(const fx::State& s, std::span<const void* const> paramTextures) {
    if (s.kind == fx::StateKind::ParamRef && s.referenceParam != fx::kNone && s.referenceParam < paramTextures.size()) {
        return paramTextures[s.referenceParam];
    }
    return nullptr;
}

void setSampler(EffectDraw& d, std::uint32_t reg, std::uint32_t state, std::uint32_t value) {
    if (reg >= kStages || state >= kSamplerStates) {
        d.unsupported = true;
        return;
    }
    d.ss[reg][state] = value;
}

void setTexture(EffectDraw& d, std::uint32_t reg, const void* t) {
    if (reg >= kStages) {
        d.unsupported = true;
        return;
    }
    d.textures[reg] = t;
}

// HalEffect::applySamplers: the shader's sampler registers get their sampler parameter's sampler_state assignments
void applySamplers(const fx::Effect& e, const fx::Shader& shader, std::uint32_t base, const fx::ParameterValues& values,
                   std::span<const void* const> paramTextures, EffectDraw& d) {
    for (const auto& c : shader.constants.constants) {
        if (c.registerSet != fx::RegisterSet::Sampler) continue;
        const std::uint32_t pi = e.parameterIndex(c.name);
        if (pi == fx::kNone) continue;
        const fx::Parameter& sp = e.parameters[pi];
        const std::uint32_t reg = base + c.registerIndex;
        for (const auto& st : sp.samplerStates) {
            const fx::StateInfo* info = st.info();
            if (info == nullptr) continue;
            if (info->cls == fx::StateClass::Texture) {
                setTexture(d, reg, samplerTexture(st, paramTextures));
            } else if (info->cls == fx::StateClass::SamplerState) {
                setSampler(d, reg, info->op, stateDword(e, st, values));
            }
        }
    }
}

// HalEffect::applyConstants: the registers from c0 (float, int, bool)
void applyConstants(const fx::Effect& e, const fx::Shader& shader, bool vertex, const fx::ParameterValues& values, EffectDraw& d) {
    const fx::ConstantRegisters regs = fx::evaluateShaderConstants(e, shader, values);
    const auto nf = static_cast<std::uint32_t>(regs.f.size() / 4);
    const auto ni = static_cast<std::uint32_t>(regs.i.size() / 4);
    const auto nb = static_cast<std::uint32_t>(regs.b.size());
    const std::uint32_t maxF = vertex ? kVsRegs : kPsRegs;
    float (*f)[4] = vertex ? d.vsF : d.psF;
    for (std::uint32_t r = 0; r < nf && r < maxF; ++r) std::memcpy(f[r], &regs.f[r * 4], 16);
    std::int32_t (*iv)[4] = vertex ? d.vsI : d.psI;
    for (std::uint32_t r = 0; r < ni && r < 16; ++r) std::memcpy(iv[r], &regs.i[r * 4], 16);
    auto& b = vertex ? d.vsB : d.psB;
    for (std::uint32_t r = 0; r < nb && r < 16; ++r) b[r] = regs.b[r] != 0 ? 1u : 0u;
    (vertex ? d.vsRegsWritten : d.psRegsWritten) = std::max(vertex ? d.vsRegsWritten : d.psRegsWritten, nf);
}

void setState(EffectDraw& d, std::uint32_t rs, std::uint32_t v) {
    if (rs >= kRenderStates) return;
    d.rs[rs] = v;
    d.rsWritten.set(rs);
}

}  // namespace

void applyPass(const fx::Effect& e, const fx::Pass& pass, const fx::ParameterValues& values, std::span<const void* const> paramTextures,
               std::span<const void* const> shaders, EffectDraw& d) {
    const fx::Shader* vs = nullptr;
    const fx::Shader* ps = nullptr;
    for (const auto& s : pass.states) {
        const fx::StateInfo* info = s.info();
        if (info == nullptr) continue;
        switch (info->cls) {
        case fx::StateClass::RenderState: setState(d, info->op, stateDword(e, s, values)); break;
        case fx::StateClass::VertexShader:
        case fx::StateClass::PixelShader: {
            const bool vertex = info->cls == fx::StateClass::VertexShader;
            const void* sh = nullptr;
            const fx::Shader* fs = nullptr;
            if (s.shader != fx::kNone && s.shader < e.shaders.size()) {
                sh = s.shader < shaders.size() ? shaders[s.shader] : nullptr;
                fs = &e.shaders[s.shader];
            }
            if (vertex) {
                d.vs = sh;
                vs = fs;
            } else {
                d.ps = sh;
                ps = fs;
            }
            break;
        }
        case fx::StateClass::SamplerState: setSampler(d, s.index, info->op, stateDword(e, s, values)); break;
        case fx::StateClass::Texture: setTexture(d, s.index, samplerTexture(s, paramTextures)); break;
        case fx::StateClass::Transform:
        case fx::StateClass::ShaderConst:
        case fx::StateClass::TextureStage:
        case fx::StateClass::Light:
        case fx::StateClass::LightEnable:
        case fx::StateClass::Material:
        case fx::StateClass::Fvf:
        case fx::StateClass::NPatchMode:
            d.unsupported = true;  // the HAL's effect ignores them (not used by the game's effects)
            break;
        default: break;
        }
    }
    if (vs != nullptr) {
        applyConstants(e, *vs, true, values, d);
        applySamplers(e, *vs, d3d9::kVertexSamplerD3D, values, paramTextures, d);
    }
    if (ps != nullptr) {
        applyConstants(e, *ps, false, values, d);
        applySamplers(e, *ps, 0, values, paramTextures, d);
    }
}

void applyVisual(EffectDraw& d, std::uint32_t fill, std::uint32_t cull, std::uint32_t depthFunc, std::uint32_t depthWrite, const Masters& m) {
    if (!m.fill) setState(d, d3d9::RS_FILLMODE, fill);
    if (!m.cull) setState(d, d3d9::RS_CULLMODE, cull);
    if (!m.depthFunc) setState(d, d3d9::RS_ZFUNC, depthFunc);
    if (!m.depthWrite) setState(d, d3d9::RS_ZWRITEENABLE, depthWrite == 2 ? 1u : 0u);
}

PssmState pssmPass(int p, bool cameraInside, const PssmState& prev) {
    PssmState s = prev;
    if (p == 0) s.fail = s.zfail = s.pass = 1, s.func = 8;
    if (p == 0 || p == 1) {
        (cameraInside ? s.zfail : s.pass) = p == 0 ? 7u : 8u;
        s.cull = p == 0 ? 2u : 3u;
        s.depthFunc = 4;
    } else if (p == 2) {
        s.fail = s.zfail = s.pass = 2;
        s.func = 6;
        s.cull = 1;
        s.depthFunc = 8;
    }
    return s;
}

void applyPssmStencil(EffectDraw& d, const PssmState& s) {
    setState(d, d3d9::RS_STENCILENABLE, 1);
    setState(d, d3d9::RS_STENCILFAIL, s.fail);
    setState(d, d3d9::RS_STENCILZFAIL, s.zfail);
    setState(d, d3d9::RS_STENCILPASS, s.pass);
    setState(d, d3d9::RS_STENCILFUNC, s.func);
    setState(d, d3d9::RS_STENCILREF, 0);
    setState(d, d3d9::RS_STENCILMASK, 0xffffffffu);
    setState(d, d3d9::RS_STENCILWRITEMASK, 0xffffffffu);
}

void transpose(const float in[16], float out[16]) {
    for (int r = 0; r < 4; ++r)
        for (int c = 0; c < 4; ++c) out[c * 4 + r] = in[r * 4 + c];
}

void cascadeFilter(const MaskConstants& k, std::int32_t mapSize, float& kernel, float offset[4], float scale[2]) {
    const auto s = static_cast<float>(mapSize);
    kernel = k.kernel / s;
    offset[0] = offset[1] = k.negHalf / s;
    offset[2] = offset[3] = k.half / s;
    scale[0] = scale[1] = s;
}

void fadeOut(const MaskConstants& k, float maxDistance, float cameraFar, float out[2]) {
    const float f = maxDistance * k.fadeFraction;
    out[0] = cameraFar / f;
    out[1] = -((maxDistance - f) / f);
}

float zDistance(float f, float p33, float p43) { return (f * p33 + p43) / f; }

void shadowProjection(const float box[6], float scale[3], float translation[3]) {
    scale[0] = 2.0f / (box[3] - box[0]);
    scale[1] = 2.0f / (box[4] - box[1]);
    scale[2] = 1.0f / (box[5] - box[2]);
    translation[0] = (box[0] + box[3]) * -0.5f * scale[0];
    translation[1] = (box[1] + box[4]) * -0.5f * scale[1];
    translation[2] = -(scale[2] * box[2]);
}

void shadowTextureBias(std::int32_t size, float depthBias, float out[16]) {
    for (int i = 0; i < 16; ++i) out[i] = i % 5 == 0 ? 1.0f : 0.0f;
    out[0] = 0.5f;
    out[5] = -0.5f;
    const float e = 0.5f / static_cast<float>(size) + 0.5f;
    out[12] = e;
    out[13] = e;
    out[14] = -depthBias;
}

// ---- N5.7 post chain ----------------------------------------------------------------------------------------------------------

void colorMatrix(float exposure, float saturation, const float tint[3], float one, const float weights[3], float out[9]) {
    // 0x758980 in its instruction order: f = 1 - s; t_i = E tint_i; a / b / c = f w_R / w_G / w_B
    const float f = one - saturation;
    const float t0 = exposure * tint[0], t1 = exposure * tint[1], t2 = exposure * tint[2];
    const float a = f * weights[0], b = f * weights[1], c = f * weights[2];
    out[0] = (a + saturation) * t0;
    out[1] = b * t0;
    out[2] = c * t0;
    out[3] = a * t1;
    out[4] = (b + saturation) * t1;
    out[5] = c * t1;
    out[6] = a * t2;
    out[7] = b * t2;
    out[8] = (c + saturation) * t2;
}

void focalRangeNear(float nearRange, float cameraFar, float epsilon, float minusOne, float out[2]) {
    if (nearRange < epsilon || nearRange == epsilon) {
        out[0] = out[1] = 0.0f;
        return;
    }
    const float n = nearRange / cameraFar;
    const float r = minusOne / n;
    out[0] = r;
    out[1] = -(r * n);
}

void focalRangeFar(float farStart, float farEnd, float cameraFar, float one, float out[2]) {
    float e = farEnd;
    if (farEnd <= farStart) e = cameraFar;
    const float r = (e - farStart) / cameraFar;
    if (one <= r) {
        out[0] = out[1] = 0.0f;
        return;
    }
    const float k = one / r;
    out[0] = k;
    out[1] = -((farStart / cameraFar) * k);
}

void clampScreenVector(float v[3], float one, float minusOne) {
    float f = one;
    if (v[0] <= one) f = v[0];
    v[0] = f;
    if (f < minusOne) v[0] = minusOne;
    f = one;
    if (v[1] <= one) f = v[1];
    v[1] = minusOne;
    if (minusOne <= f) v[1] = f;
}

float sunFlare(float viewZ) {
    float v = -viewZ;
    if (v <= 0.0f) v = 0.0f;
    return v * v;
}

// ---- N5.7 sprite batcher ------------------------------------------------------------------------------------------------------

SpritePlan planSprites(std::span<const SpriteKey> records, std::uint32_t capacity, std::uint32_t start, std::uint32_t count) {
    SpritePlan p;
    const std::size_t n = records.size();
    p.quadOffset.assign(n, 0);
    p.discard.assign(n, false);
    if (n == 0) {
        p.endStart = start;
        p.endCount = count;
        return p;
    }
    const void* bound = nullptr;              // +0x88
    const void* pending = records[0].image;   // +0x90
    std::uint32_t current = 0;                // +0x98
    std::uint32_t wanted = records[0].blend;  // +0x9c
    std::uint32_t firstRecord = 0;
    const auto drawBatch = [&](SpriteBatch::Why why) {
        SpriteBatch b;
        b.why = why;
        if (current != wanted) {
            b.setBlend = true;
            b.blend = wanted;
            current = wanted;
        }
        if (bound != pending) {
            b.setImage = true;
            b.image = pending;
            bound = pending;
        }
        if (count != 0) {
            b.start = start;
            b.count = count;
            b.firstRecord = firstRecord;
            start += count;
            count = 0;
        }
        p.batches.push_back(b);
    };
    for (std::size_t i = 0; i < n;) {
        if (records[i].blend != wanted) {
            drawBatch(SpriteBatch::Why::Blend);
            wanted = records[i].blend;
            continue;
        }
        if (records[i].image != pending) {
            drawBatch(SpriteBatch::Why::Texture);
            pending = records[i].image;
            continue;
        }
        if (capacity - (start + count) == 0) {  // `cmp (capacity - (start + count)), 1; jb`: full
            drawBatch(SpriteBatch::Why::Full);
            start = count = 0;
            continue;
        }
        if (count == 0) firstRecord = static_cast<std::uint32_t>(i);
        p.discard[i] = start + count == 0;
        p.quadOffset[i] = (start + count) * kSpriteQuad;
        ++count;
        ++i;
    }
    drawBatch(SpriteBatch::Why::End);
    p.endStart = start;
    p.endCount = count;
    return p;
}

void spriteQuad(const std::uint8_t* r, float w, std::uint8_t out[kSpriteQuad]) {
    // RenderBatches 0x7572b5..0x75741a: x / y of the corner, z +0x30, the w lane, colour +0x40, u, v
    static constexpr std::uint32_t kCorner[4] = {0x00, 0x08, 0x18, 0x10};
    static constexpr std::uint32_t kU[4] = {0x20, 0x28, 0x28, 0x20};
    static constexpr std::uint32_t kV[4] = {0x24, 0x24, 0x2c, 0x2c};
    for (int k = 0; k < 4; ++k) {
        std::uint8_t* v = out + k * kSpriteVertex;
        std::memcpy(v + 0x00, r + kCorner[k], 4);
        std::memcpy(v + 0x04, r + kCorner[k] + 4, 4);
        std::memcpy(v + 0x08, r + 0x30, 4);
        std::memcpy(v + 0x0c, &w, 4);
        std::memcpy(v + 0x10, r + 0x40, 4);
        std::memcpy(v + 0x14, r + kU[k], 4);
        std::memcpy(v + 0x18, r + kV[k], 4);
    }
}

void spriteQuadIndices(std::uint32_t q, std::uint16_t out[6]) {
    static constexpr std::uint16_t kQuad[6] = {0, 2, 3, 0, 1, 2};
    for (int i = 0; i < 6; ++i) out[i] = static_cast<std::uint16_t>(4u * q + kQuad[i]);
}

bool spriteBlend(std::uint32_t flags, std::uint32_t& enable, std::uint32_t& src, std::uint32_t& dst) {
    // D3DBLEND: 1 ZERO, 2 ONE, 3 SRCCOLOR, 5 SRCALPHA, 6 INVSRCALPHA, 9 DESTCOLOR
    switch (flags & 0x7000u) {
    case 0x0000: enable = 1, src = 5, dst = 6; return true;
    case 0x1000: enable = 1, src = 2, dst = 2; return true;
    case 0x2000: enable = 1, src = 5, dst = 2; return true;
    case 0x3000: enable = 1, src = 9, dst = 1; return true;
    case 0x4000: enable = 1, src = 9, dst = 3; return true;
    case 0x5000: enable = 0, src = 2, dst = 1; return true;
    default: return false;
    }
}

bool spriteBytesEqual(std::span<const std::uint8_t> a, std::span<const std::uint8_t> b, std::size_t& firstDiff, std::uint64_t& wLanesDiffer) {
    firstDiff = 0;
    if (a.size() != b.size()) return false;
    for (std::size_t v = 0; v < a.size(); v += kSpriteVertex) {
        const std::size_t n = std::min<std::size_t>(kSpriteVertex, a.size() - v);
        for (std::size_t i = 0; i < n; ++i) {
            if (a[v + i] == b[v + i]) continue;
            if (i >= 0xc && i < 0x10) {
                ++wLanesDiffer;
                i = 0xf;  // one count per vertex
                continue;
            }
            firstDiff = v + i;
            return false;
        }
    }
    return true;
}

// ---- comparison -------------------------------------------------------------------------------------------------------------

const char* name(Field f) {
    static constexpr const char* kNames[] = {"draw count", "vs", "ps", "declaration", "streams", "indices", "range", "depth states",
                                             "stencil states", "blend states", "colour write", "raster states", "other states",
                                             "texture", "sampler state", "vs constants", "ps constants", "bool / int", "target",
                                             "viewport", "vertex / index data"};
    static_assert(std::size(kNames) == kFieldCount);
    return kNames[static_cast<std::size_t>(f)];
}

Field stateField(std::uint32_t rs) {
    using namespace d3d9;
    switch (rs) {
    case RS_ZENABLE:
    case RS_ZWRITEENABLE:
    case RS_ZFUNC:
    case RS_DEPTHBIAS:
    case RS_SLOPESCALEDEPTHBIAS: return Field::Depth;
    case RS_STENCILENABLE:
    case RS_STENCILFAIL:
    case RS_STENCILZFAIL:
    case RS_STENCILPASS:
    case RS_STENCILFUNC:
    case RS_STENCILREF:
    case RS_STENCILMASK:
    case RS_STENCILWRITEMASK:
    case RS_TWOSIDEDSTENCILMODE:
    case RS_CCW_STENCILFAIL:
    case RS_CCW_STENCILZFAIL:
    case RS_CCW_STENCILPASS:
    case RS_CCW_STENCILFUNC: return Field::Stencil;
    case RS_ALPHABLENDENABLE:
    case RS_SRCBLEND:
    case RS_DESTBLEND:
    case RS_BLENDOP:
    case RS_SEPARATEALPHABLENDENABLE:
    case RS_SRCBLENDALPHA:
    case RS_DESTBLENDALPHA:
    case RS_BLENDOPALPHA:
    case RS_BLENDFACTOR:
    case RS_ALPHATESTENABLE:
    case RS_ALPHAREF:
    case RS_ALPHAFUNC: return Field::Blend;
    case RS_COLORWRITEENABLE:
    case RS_COLORWRITEENABLE1:
    case RS_COLORWRITEENABLE2:
    case RS_COLORWRITEENABLE3:
    case RS_SRGBWRITEENABLE: return Field::ColorWrite;
    case RS_FILLMODE:
    case RS_CULLMODE:
    case RS_SCISSORTESTENABLE:
    case RS_CLIPPLANEENABLE:
    case RS_SHADEMODE: return Field::Raster;
    default: return Field::OtherState;
    }
}

namespace {
bool regsEqual(const float a[4], const float b[4]) { return std::memcmp(a, b, 16) == 0; }
}  // namespace

Diff compare(const EffectDraw& c, const EffectDraw& e, const Live& live) {
    Diff d;
    const auto miss = [&](Field f, std::string what) {
        if (!d.fields.test(static_cast<std::size_t>(f)) && d.first.empty()) d.first = std::move(what);
        d.fields.set(static_cast<std::size_t>(f));
    };
    if (c.vs != e.vs) miss(Field::Vs, std::format("vs {} vs engine {}", c.vs, e.vs));
    if (c.ps != e.ps) miss(Field::Ps, std::format("ps {} vs engine {}", c.ps, e.ps));
    if (c.decl != e.decl) miss(Field::Decl, "declaration");
    for (std::uint32_t i = 0; i < 4; ++i) {
        if (!(c.streams[i] == e.streams[i])) {
            miss(Field::Streams, std::format("stream {}: {} +{} /{} f{} vs engine {} +{} /{} f{}", i, c.streams[i].buffer, c.streams[i].offset,
                                             c.streams[i].stride, c.streams[i].freq, e.streams[i].buffer, e.streams[i].offset, e.streams[i].stride,
                                             e.streams[i].freq));
        }
    }
    if (c.indexed && c.indices != e.indices) miss(Field::Indices, "index buffer");
    if (c.indexed != e.indexed || c.primType != e.primType || c.primCount != e.primCount ||
        (c.indexed ? (c.baseVertex != e.baseVertex || c.minIndex != e.minIndex || c.numVertices != e.numVertices || c.startIndex != e.startIndex)
                   : c.startVertex != e.startVertex)) {
        miss(Field::Range, std::format("range: type {} prims {} base {} min {} n {} start {} vs engine type {} prims {} base {} min {} n {} start {}",
                                       c.primType, c.primCount, c.baseVertex, c.minIndex, c.numVertices, c.startIndex, e.primType, e.primCount,
                                       e.baseVertex, e.minIndex, e.numVertices, e.startIndex));
    }
    for (std::uint32_t rs = 0; rs < kRenderStates; ++rs) {
        ++d.states;
        if (c.rs[rs] != e.rs[rs]) miss(stateField(rs), std::format("render state {}: {:#x} vs engine {:#x}{}", rs, c.rs[rs], e.rs[rs],
                                                                   c.rsWritten.test(rs) ? " (composed)" : " (inherited)"));
    }
    for (std::uint32_t st = 0; st < kStages; ++st) {
        if ((live.stages >> st & 1u) == 0) continue;
        ++d.stages;
        if (c.textures[st] != e.textures[st]) miss(Field::Texture, std::format("stage {} texture {} vs engine {}", st, c.textures[st], e.textures[st]));
        for (std::uint32_t s = 1; s < kSamplerStates; ++s) {
            if (c.ss[st][s] != e.ss[st][s]) miss(Field::SamplerState, std::format("stage {} sampler state {}: {:#x} vs engine {:#x}", st, s, c.ss[st][s], e.ss[st][s]));
        }
    }
    for (std::uint32_t r = 0; r < kVsRegs; ++r) {
        if (!live.vs.test(r)) continue;
        ++d.regs;
        if (!regsEqual(c.vsF[r], e.vsF[r])) {
            miss(Field::VsConst, std::format("vs c{} ({}, {}, {}, {}) vs engine ({}, {}, {}, {})", r, c.vsF[r][0], c.vsF[r][1], c.vsF[r][2], c.vsF[r][3],
                                             e.vsF[r][0], e.vsF[r][1], e.vsF[r][2], e.vsF[r][3]));
        }
    }
    for (std::uint32_t r = 0; r < kPsRegs; ++r) {
        if (!live.ps.test(r)) continue;
        ++d.regs;
        if (!regsEqual(c.psF[r], e.psF[r])) {
            miss(Field::PsConst, std::format("ps c{} ({}, {}, {}, {}) vs engine ({}, {}, {}, {})", r, c.psF[r][0], c.psF[r][1], c.psF[r][2], c.psF[r][3],
                                             e.psF[r][0], e.psF[r][1], e.psF[r][2], e.psF[r][3]));
        }
    }
    if (c.vsB != e.vsB || c.psB != e.psB || std::memcmp(c.vsI, e.vsI, sizeof(c.vsI)) != 0 || std::memcmp(c.psI, e.psI, sizeof(c.psI)) != 0) {
        miss(Field::BoolInt, "bool / int registers");
    }
    if (c.rt0 != e.rt0 || c.depth != e.depth) miss(Field::Target, std::format("target {} / depth {} vs engine {} / {}", c.rt0, c.depth, e.rt0, e.depth));
    if (!(c.viewport == e.viewport)) {
        miss(Field::Viewport, std::format("viewport {} {} {}x{} z {}..{} vs engine {} {} {}x{} z {}..{}", c.viewport.x, c.viewport.y, c.viewport.width,
                                          c.viewport.height, c.viewport.minZ, c.viewport.maxZ, e.viewport.x, e.viewport.y, e.viewport.width,
                                          e.viewport.height, e.viewport.minZ, e.viewport.maxZ));
    }
    return d;
}

void Counts::add(const Counts& o) {
    units += o.units;
    unitsComposed += o.unitsComposed;
    draws += o.draws;
    cleanDraws += o.cleanDraws;
    for (std::size_t i = 0; i < kFieldCount; ++i) mismatches[i] += o.mismatches[i];
    regs += o.regs;
    stages += o.stages;
    states += o.states;
}

void Counts::add(const Diff& d) {
    ++draws;
    if (d.fields.none()) ++cleanDraws;
    for (std::size_t i = 0; i < kFieldCount; ++i) {
        if (d.fields.test(i)) ++mismatches[i];
    }
    regs += d.regs;
    stages += d.stages;
    states += d.states;
}

std::string summary(const Counts& c) {
    std::string out = std::format("{} units ({} composed), draws {} / {} equal ({} registers, {} stages, {} render states compared)", c.units,
                                  c.unitsComposed, c.cleanDraws, c.draws, c.regs, c.stages, c.states);
    std::string m;
    for (std::size_t i = 0; i < kFieldCount; ++i) {
        if (c.mismatches[i] != 0) m += std::format("{}{} {}", m.empty() ? "" : ", ", name(static_cast<Field>(i)), c.mismatches[i]);
    }
    return out + (m.empty() ? std::string(", 0 mismatches") : "; mismatches: " + m);
}

}  // namespace rvk::native::fxc
