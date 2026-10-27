#include "native/static_compose.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <format>

namespace rvk::native::sc {

// ---- passes ---------------------------------------------------------------------------------------------------------------

std::size_t buildPasses(const PassInputs& in, std::vector<Pass>& out) {
    const std::size_t before = out.size();
    // (param_6 & 0xff ^ 1) << 9: bit 9 = not instanced
    const std::uint32_t base = (in.instanced ? 0u : 1u) << 9;
    std::uint32_t flags = base;
    if (in.zFill) {
        flags = base | 0x80u;  // kind 2, no lights, no lightmap, no sun; illumination null, first light 0
        out.push_back({flags, nullptr, 0, in.switchValue});
    }
    if (in.illum == nullptr || !in.hasList) {
        out.push_back({(in.combi & 3u) << 4 | (flags & 0xfffffe00u), nullptr, 0, in.switchValue});
        return out.size() - before;
    }
    const std::int32_t total = static_cast<std::int32_t>(in.dynamicLights) + static_cast<std::int32_t>(in.staticLights);
    bool first = true;
    std::int32_t next = 0;
    do {
        const std::int32_t start = next;
        if (first && in.listSun != 0 && in.illumNoSun == 0) {
            flags = (flags & 0xfffffffdu) | 1u;
        } else {
            flags &= 0xfffffffcu;
        }
        if (next < total) {
            const std::int32_t n = std::min(3, total - next);
            flags = (flags & ~0xcu) | (static_cast<std::uint32_t>(n) << 2 & 0xcu);
            next += n;
        } else {
            flags &= 0xfffffff3u;
        }
        if (first) {
            flags = (in.combi & 3u) << 4 | (flags & 0xfffffe0fu);
            first = false;
        } else {
            flags = (flags & 0xfffffe4fu) | 0x40u;
        }
        out.push_back({flags, in.illum, static_cast<std::uint8_t>(start), in.switchValue});
    } while (next < total);
    return out.size() - before;
}

// ---- states ---------------------------------------------------------------------------------------------------------------

bool isBlended(const BlendInputs& b) {
    if (b.tradeAlphaAsSolid) return false;
    if (b.mode == 0) return b.hasOpacity || b.refracted;
    return b.mode != 1;
}

void fillBlending(RenderStates& rs, const BlendInputs& b, bool additive, float fade) {
    const auto tail = [&] {
        rs.alphaTest = 1;
        rs.alphaFunc = 5;
        rs.blend = true;
        rs.alphaRef = 0;
    };
    switch (b.mode) {
        case 1:
            if (fade == 1.0f) {
                rs.blend = additive;
                rs.src = 2;
                rs.dst = additive ? 2u : 1u;
                rs.alphaTest = 2;
                rs.alphaFunc = 5;
                rs.depthFunc = 3;
                rs.alphaRef = b.maskRef;
                return;
            }
            rs.blend = true;
            rs.src = 5;
            rs.dst = additive ? 2u : 6u;
            rs.alphaTest = 2;
            rs.alphaFunc = 5;
            rs.alphaRef = static_cast<std::uint32_t>(static_cast<std::int32_t>(static_cast<float>(b.maskRef) * fade));
            return;
        case 3:
            rs.src = 9;
            rs.dst = 3;
            tail();
            return;
        case 4:
            rs.dst = 6;
            rs.src = 2;
            tail();
            return;
        case 5:
            rs.dst = 4;
            rs.src = 2;
            tail();
            return;
        case 6:
            rs.dst = 4;
            rs.src = 1;
            tail();
            return;
        case 7:
            rs.src = 5;
            rs.dst = 2;
            tail();
            return;
        case 8:
            rs.dst = 2;
            rs.src = 1;
            rs.alphaFunc = 1;
            rs.blend = true;
            rs.alphaTest = 2;
            rs.alphaRef = 0;
            return;
        case 2:
            break;
        default:
            if ((!b.hasOpacity || b.refracted) && fade == 1.0f) {
                rs.src = 2;
                rs.dst = additive ? 2u : 1u;
                rs.blend = additive;
                rs.alphaTest = 1;
                rs.alphaFunc = 5;
                rs.alphaRef = 0;
                return;
            }
            break;
    }
    // case 2 (and the default's blended fall-through): the alpha reference is not written
    rs.blend = true;
    rs.src = 5;
    rs.dst = additive ? 2u : 6u;
    rs.alphaTest = 1;
    rs.alphaFunc = 5;
}

RenderStates passStates(std::uint32_t flags, const BlendInputs& b, float fade) {
    RenderStates rs;
    rs.depthFunc = 4;
    rs.depthWriteMaster = isBlended(b) && !b.refracted ? 1u : 0u;
    const float f = (passNonInstanced(flags) && (flags & 0xc00u) == 0) ? fade : 1.0f;
    const std::uint32_t kind = passKind(flags);
    if (kind == 0 || kind == 1) {
        fillBlending(rs, b, kind == 1, f);
        // local_3c._1_2_ = 1: colour write on, alpha write off
        rs.colorWrite = true;
        rs.alphaWrite = false;
    } else if (kind == 2) {
        rs.alphaRef = 0;
        rs.alphaFunc = 5;
        rs.alphaTest = 1;
        rs.blend = rs.colorWrite = rs.alphaWrite = false;
        rs.src = 2;
        rs.dst = 1;
        rs.depthWriteMaster = 2;
    } else {
        rs.valid = false;
    }
    rs.renderMode = 0;
    return rs;
}

RenderStates zPassStates(bool zWrite, bool colorWrite) {
    RenderStates rs;
    rs.alphaRef = 0;
    rs.alphaFunc = 8;
    rs.alphaTest = 1;
    rs.blend = false;
    rs.colorWrite = rs.alphaWrite = colorWrite;
    rs.src = 2;
    rs.dst = 1;
    rs.depthWriteMaster = zWrite ? 2u : 1u;
    rs.renderMode = 0;
    rs.depthFunc = 4;
    return rs;
}

std::array<std::uint32_t, StOwnedCount> deviceStates(const RenderStates& rs, const Visual& v, std::uint32_t adminRenderMode) {
    std::array<std::uint32_t, StOwnedCount> s{};
    s[StZFunc] = rs.depthFunc;  // a master: the visual's SetDepthTesting is ignored
    s[StZWrite] = rs.depthWriteMaster == 0 ? (v.depthWrite == 2 ? 1u : 0u) : (rs.depthWriteMaster == 2 ? 1u : 0u);
    s[StBlend] = rs.blend ? 1u : 0u;
    s[StSrc] = rs.src;  // compared only while blending
    s[StDst] = rs.dst;
    s[StColorWrite] = (rs.colorWrite ? 7u : 0u) | (rs.alphaWrite ? 8u : 0u);
    s[StAlphaTest] = 0;  // SetMasterAlphaTesting(1): always off
    s[StCull] = v.cull;  // no master cull (released by BeginZPass / BeginMaterial)
    const std::uint32_t mode = rs.renderMode != 0 ? rs.renderMode : adminRenderMode;
    s[StFill] = mode != 0 ? mode : v.fill;
    s[StFog] = 0;  // SetMasterFogging(1)
    return s;
}

const char* stateName(std::uint32_t s) {
    static constexpr const char* kNames[StOwnedCount] = {"ZFUNC", "ZWRITEENABLE", "ALPHABLENDENABLE", "SRCBLEND", "DESTBLEND",
                                                         "COLORWRITEENABLE", "ALPHATESTENABLE", "CULLMODE", "FILLMODE", "FOGENABLE"};
    return s < StOwnedCount ? kNames[s] : "?";
}

// ---- geometry -------------------------------------------------------------------------------------------------------------

void planWholeMeshZ(std::span<const ZSub> subs, std::vector<ZDraw>& out) {
    ZDraw run;
    bool open = false;          // primitive count != 0
    std::uint32_t runShader = 0;  // r12: the shader of the last non-masked sub-mesh added
    const auto flush = [&] {
        if (open) {
            run.shaderOf = runShader;
            out.push_back(run);
            open = false;
        }
    };
    for (std::uint32_t i = 0; i < subs.size(); ++i) {
        const ZSub& s = subs[i];
        if (!s.ready) continue;
        if (s.blended || s.masked) flush();  // blended: flush with the previous shader; masked: the same, before it draws alone
        if (s.blended) continue;
        if (!s.masked) runShader = i;
        if (!open) {
            run = {};
            run.minIndex = s.firstVertex;
            run.startIndex = s.startIndex;
            run.numVertices = 0;
            run.primCount = 0;
        }
        run.numVertices += s.vertexCount;
        run.primCount += s.indexCount / 3;
        open = run.primCount != 0;
        if (s.masked) {
            if (open) {
                run.shaderOf = i;
                out.push_back(run);
            }
            open = false;
        }
    }
    flush();
}

void planBatches(std::span<const Instance> instances, std::uint32_t maxPerBatch, std::vector<Batch>& out) {
    const std::uint32_t n = static_cast<std::uint32_t>(instances.size());
    std::uint8_t count = 0;
    bool first = true, alone = false;
    Instance cur;
    for (std::uint32_t k = 0; k <= n; ++k) {
        bool flush = k >= n;
        if (!flush) {
            const Instance& in = instances[k];
            flush = cur.list != in.list || cur.switchValue != in.switchValue || cur.flags != in.flags ||
                    count == static_cast<std::uint8_t>(maxPerBatch) || cur.f != in.f || alone || first;
        }
        if (flush) {
            if (count != 0) {
                out.push_back({k - count, count});
                count = 0;
            }
            first = false;
        }
        if (k < n) {
            cur = instances[k];
            alone = instances[k].alone;
        }
        count = static_cast<std::uint8_t>(count + 1);
    }
}

std::int32_t constantSwitchIndex(std::int32_t s, std::int32_t count, std::int32_t repeat) {
    if (count <= 0) return -1;
    if (repeat == 0) return s % count;
    if (repeat == 1) {
        const std::int32_t last = count - 1;
        return std::min(s, last) < 0 ? 0 : std::min(s, last);
    }
    if (repeat == 2) {
        if (count < 2) return -1;  // the engine divides by count - 1
        const std::int32_t q = s / (count - 1), r = s % (count - 1);
        return (q & 1) != 0 ? count - r - 1 : r;
    }
    return 0;
}

std::int32_t samplerSwitchIndex(std::int32_t s, std::int32_t begin, std::int32_t count, std::int32_t repeat, std::int32_t type) {
    if (type != 2 || count <= 0) return -1;
    std::int32_t v = std::max(s - begin, 0);
    if (repeat == 0) {
        v = v % count;
    } else if (repeat == 1) {
        const std::int32_t last = count - 1;
        v = std::min(v, last) < 0 ? 0 : std::min(v, last);
    } else if (repeat == 2) {
        if (count < 2) return -1;
        const std::int32_t q = v / (count - 1), r = v % (count - 1);
        v = (q & 1) != 0 ? count - r - 1 : r;
    } else {
        return -1;  // the engine reads an uninitialised stack slot
    }
    const std::int32_t last = count - 1;
    return std::min(v, last) < 0 ? 0 : std::min(v, last);
}

// ---- constants ------------------------------------------------------------------------------------------------------------

void fogConstants(bool enabled, std::uint32_t colorArgb, float start, float end, float density, float c3[4], float c4[4]) {
    for (int i = 0; i < 4; ++i) c3[i] = c4[i] = 0.0f;
    if (!enabled) return;
    c3[0] = static_cast<float>(colorArgb >> 16 & 0xffu) / 255.0f;
    c3[1] = static_cast<float>(colorArgb >> 8 & 0xffu) / 255.0f;
    c3[2] = static_cast<float>(colorArgb & 0xffu) / 255.0f;
    if (0.0f < density) {
        const float k = 1.0f / ((end - start) / density);
        c4[0] = -(k * start);
        c4[1] = k;
        c4[2] = density;
    }
}

void viewToScreen(std::int32_t width, std::int32_t height, float c1[4]) {
    c1[0] = 0.5f;
    c1[1] = -0.5f;
    c1[2] = 0.5f / static_cast<float>(width) + 0.5f;
    c1[3] = 0.5f / static_cast<float>(height) + 0.5f;
}

void camObstacle(float d, float& x, float& y) {
    std::uint32_t bits = 0x3bda740eu;  // the value the engine stores (1 / 150)
    std::memcpy(&x, &bits, 4);
    std::uint32_t neg = 0xbbda740eu;
    float k = 0.0f;
    std::memcpy(&k, &neg, 4);
    y = (d - 50.0f) * k;
}

void ambientSh(const float list[7][4], const float indoor[3], float f, const float* staticSh, float out[7][4]) {
    for (int k = 0; k < 7; ++k) {
        for (int c = 0; c < 4; ++c) out[k][c] = list[k][c] * f;
    }
    const float g = 1.0f - f;
    for (int k = 0; k < 3; ++k) out[k][3] = g * indoor[k] + out[k][3];
    if (staticSh != nullptr) {
        for (int k = 0; k < 7; ++k) {
            for (int c = 0; c < 4; ++c) out[k][c] += staticSh[k * 4 + c];
        }
    }
}

// ---- comparison -----------------------------------------------------------------------------------------------------------

const char* name(DrawClass c) {
    static constexpr const char* kNames[kDrawClassCount] = {"z pre-pass", "base pass", "additive light pass", "z-fill pass", "overlay pass",
                                                            "non-op", "sky"};
    const auto i = static_cast<std::size_t>(c);
    return i < kDrawClassCount ? kNames[i] : "?";
}

DrawClass passClass(std::uint32_t flags, bool overlay) {
    if (overlay) return DrawClass::Overlay;
    switch (passKind(flags)) {
        case 1: return DrawClass::Additive;
        case 2: return DrawClass::ZFill;
        default: return DrawClass::Base;
    }
}

const char* name(Field f) {
    static constexpr const char* kNames[kFieldCount] = {
        "draw count", "draw tag", "vs", "ps", "declaration", "streams", "indices", "range", "vertex / index data", "zfunc", "zwrite", "blend", "colour write", "alpha test",
        "cull", "fill", "fog state", "texture", "sampler state", "vs bool", "vs int", "ps bool", "ps int",
        "vs camera c0-3", "vs inv proj c4-7", "vs inv view c8-10", "vs cam pos c11", "vs admin other", "vs fog c30", "vs instance c32+",
        "vs global other", "vs material c232+",
        "ps cam pos c0", "ps screen c1", "ps reserved", "ps fog colour c3", "ps fog c4", "ps obstacle c5.xy", "ps mask ref c5.w",
        "ps material c6-13", "ps indoor c14", "ps SH c15-21", "ps sun c22-24", "ps lights c25+", "unowned constant"};
    const auto i = static_cast<std::size_t>(f);
    return i < kFieldCount ? kNames[i] : "?";
}

Field vsField(std::uint32_t reg, std::uint32_t c) {
    if (reg <= 3) return Field::VsCamera;
    if (reg <= 7) return Field::VsInvProj;
    if (reg <= 10) return c == 3 ? Field::VsAdminOther : Field::VsInvView;
    if (reg == 11) return Field::VsCamPos;
    if (reg == 30) return Field::VsFog;
    if (reg < 31) return Field::VsAdminOther;
    if (reg == 31) return Field::VsGlobalOther;
    if (reg < 232) return Field::VsInstance;
    return Field::VsMaterial;
}

Field psField(std::uint32_t reg, std::uint32_t c) {
    switch (reg) {
        case 0: return Field::PsCamPos;
        case 1: return Field::PsScreen;
        case 2: return Field::PsReserved;
        case 3: return Field::PsFogColor;
        case 4: return Field::PsFog;
        case 5: return c < 2 ? Field::PsObstacle : (c == 3 ? Field::PsMaskRef : Field::PsReserved);
        default: break;
    }
    if (reg <= 13) return Field::PsMaterial;
    if (reg == 14) return Field::PsIndoor;
    if (reg <= 21) return Field::PsSh;
    if (reg <= 24) return Field::PsSun;
    return Field::PsLights;
}

namespace {

// 0 equal (bits, or both NaN), 1 close (relative 1e-5), 2 different
int floatCmp(float a, float b) {
    std::uint32_t ab = 0, bb = 0;
    std::memcpy(&ab, &a, 4);
    std::memcpy(&bb, &b, 4);
    if (ab == bb || (std::isnan(a) && std::isnan(b))) return 0;
    if (std::abs(a - b) <= 1e-5f * std::max(1.0f, std::abs(b))) return 1;
    return 2;
}

}  // namespace

Diff compare(const DrawDesc& a, const DrawDesc& e) {
    Diff d;
    const auto bad = [&](Field f, std::string text) {
        d.fields.set(static_cast<std::size_t>(f));
        if (d.first.empty()) d.first = std::move(text);
    };
    if (a.tagSubMesh != e.tagSubMesh || a.tagMaterial != e.tagMaterial || a.tagMeshResource != e.tagMeshResource ||
        a.tagWater != e.tagWater || a.tagMaterialName != e.tagMaterialName) {
        bad(Field::Tag, std::format("tag sub-mesh {} material {} mesh {} water {} name {} vs engine {} {} {} {} {}", a.tagSubMesh, a.tagMaterial,
                                    a.tagMeshResource, a.tagWater, static_cast<const void*>(a.tagMaterialName), e.tagSubMesh, e.tagMaterial,
                                    e.tagMeshResource, e.tagWater, static_cast<const void*>(e.tagMaterialName)));
    }
    if (a.vs != e.vs) bad(Field::Vs, std::format("vs {} vs engine {}", a.vs, e.vs));
    if (a.ps != e.ps) bad(Field::Ps, std::format("ps {} vs engine {}", a.ps, e.ps));
    if (a.decl != e.decl) bad(Field::Decl, std::format("declaration {} vs engine {}", a.decl, e.decl));
    for (std::uint32_t i = 0; i < 4; ++i) {
        const auto& x = a.streams[i];
        const auto& y = e.streams[i];
        if (x.buffer != y.buffer || (y.buffer != nullptr && (x.offset != y.offset || x.stride != y.stride || x.freq != y.freq))) {
            bad(Field::Streams, std::format("stream {}: {} +{} stride {} freq {:#x} vs engine {} +{} stride {} freq {:#x}", i, x.buffer,
                                            x.offset, x.stride, x.freq, y.buffer, y.offset, y.stride, y.freq));
        }
    }
    if (a.indices != e.indices) bad(Field::Indices, std::format("indices {} vs engine {}", a.indices, e.indices));
    if (a.primType != e.primType || a.baseVertex != e.baseVertex || a.minIndex != e.minIndex || a.numVertices != e.numVertices ||
        a.startIndex != e.startIndex || a.primCount != e.primCount) {
        bad(Field::Range, std::format("range type {} base {} min {} n {} start {} prims {} vs engine {} {} {} {} {} {}", a.primType,
                                      a.baseVertex, a.minIndex, a.numVertices, a.startIndex, a.primCount, e.primType, e.baseVertex,
                                      e.minIndex, e.numVertices, e.startIndex, e.primCount));
    }
    static constexpr Field kStateField[StOwnedCount] = {Field::ZFunc, Field::ZWrite, Field::Blend, Field::Blend, Field::Blend,
                                                        Field::ColorWrite, Field::AlphaTest, Field::Cull, Field::Fill, Field::Fog};
    for (std::uint32_t s = 0; s < StOwnedCount; ++s) {
        if ((s == StSrc || s == StDst) && e.states[StBlend] == 0) continue;
        if (a.states[s] != e.states[s]) bad(kStateField[s], std::format("{} {} vs engine {}", stateName(s), a.states[s], e.states[s]));
    }
    for (std::uint32_t st = 0; st < kStages; ++st) {
        if ((e.samplerMask >> st & 1u) == 0 || (a.inheritMask >> st & 1u) != 0) continue;
        ++d.stages;
        const auto& x = a.samplers[st];
        const auto& y = e.samplers[st];
        if (x.texture != y.texture) bad(Field::Texture, std::format("s{} texture {} vs engine {}", st, x.texture, y.texture));
        if (x.ss != y.ss) {
            bad(Field::SamplerState, std::format("s{} states {},{},{},{},{},{:#x},{} vs engine {},{},{},{},{},{:#x},{}", st, x.ss[0], x.ss[1],
                                                 x.ss[2], x.ss[3], x.ss[4], x.ss[5], x.ss[6], y.ss[0], y.ss[1], y.ss[2], y.ss[3], y.ss[4],
                                                 y.ss[5], y.ss[6]));
        }
    }
    const auto regs = [&](auto live, const float (*x)[4], const float (*y)[4], const Src* src, std::uint32_t count, bool vs) {
        for (std::uint32_t r = 0; r < count; ++r) {
            if (!live(r)) continue;
            ++d.regs;
            if (src[r] == Src::None) {
                bad(Field::UnownedConstant, std::format("{} c{} not composed (engine ({}, {}, {}, {}))", vs ? "vs" : "ps", r, y[r][0], y[r][1],
                                                        y[r][2], y[r][3]));
                continue;
            }
            for (std::uint32_t c = 0; c < 4; ++c) {
                const int k = floatCmp(x[r][c], y[r][c]);
                if (k == 1) ++d.closeFloats;
                if (k == 2) {
                    bad(vs ? vsField(r, c) : psField(r, c),
                        std::format("{} c{}.{} {} vs engine {} (composed ({}, {}, {}, {}), engine ({}, {}, {}, {}))", vs ? "vs" : "ps", r,
                                    "xyzw"[c], x[r][c], y[r][c], x[r][0], x[r][1], x[r][2], x[r][3], y[r][0], y[r][1], y[r][2], y[r][3]));
                }
            }
        }
    };
    regs([&](std::uint32_t r) { return e.vsLive.test(r); }, a.vsF, e.vsF, a.vsSrc.data(), kVsRegs, true);
    regs([&](std::uint32_t r) { return e.psLive.test(r); }, a.psF, e.psF, a.psSrc.data(), kPsRegs, false);
    // bool / int register files (16 + 16 per stage)
    for (std::uint32_t r = 0; r < 16; ++r) {
        d.biRegs += 4;
        if (a.vsB[r] != e.vsB[r]) bad(Field::VsBool, std::format("vs b{} {} vs engine {}", r, a.vsB[r], e.vsB[r]));
        if (a.psB[r] != e.psB[r]) bad(Field::PsBool, std::format("ps b{} {} vs engine {}", r, a.psB[r], e.psB[r]));
        if (std::memcmp(a.vsI[r], e.vsI[r], 16) != 0) {
            bad(Field::VsInt, std::format("vs i{} ({}, {}, {}, {}) vs engine ({}, {}, {}, {})", r, a.vsI[r][0], a.vsI[r][1], a.vsI[r][2], a.vsI[r][3],
                                          e.vsI[r][0], e.vsI[r][1], e.vsI[r][2], e.vsI[r][3]));
        }
        if (std::memcmp(a.psI[r], e.psI[r], 16) != 0) {
            bad(Field::PsInt, std::format("ps i{} ({}, {}, {}, {}) vs engine ({}, {}, {}, {})", r, a.psI[r][0], a.psI[r][1], a.psI[r][2], a.psI[r][3],
                                          e.psI[r][0], e.psI[r][1], e.psI[r][2], e.psI[r][3]));
        }
    }
    return d;
}

void CompareCounts::add(const CompareCounts& o) {
    ops += o.ops;
    draws += o.draws;
    cleanDraws += o.cleanDraws;
    opsComposed += o.opsComposed;
    opsNotComposed += o.opsNotComposed;
    for (std::size_t i = 0; i < kFieldCount; ++i) mismatches[i] += o.mismatches[i];
    closeFloats += o.closeFloats;
    regs += o.regs;
    stages += o.stages;
    biRegs += o.biRegs;
    biLive += o.biLive;
    for (std::size_t i = 0; i < kDrawClassCount; ++i) {
        classDraws[i] += o.classDraws[i];
        classClean[i] += o.classClean[i];
    }
    for (std::size_t i = 0; i < listDraws.size(); ++i) {
        listDraws[i] += o.listDraws[i];
        listClean[i] += o.listClean[i];
    }
}

std::string summary(const CompareCounts& c, const char* const* listNames, std::size_t listCount) {
    std::string s = std::format("{} ops ({} composed, {} left to the engine), {} draws compared, {} equal", c.ops, c.opsComposed,
                                c.opsNotComposed, c.draws, c.cleanDraws);
    std::string bad;
    for (std::size_t i = 0; i < kFieldCount; ++i) {
        if (c.mismatches[i] == 0) continue;
        bad += std::format("{}{} {}", bad.empty() ? "" : ", ", name(static_cast<Field>(i)), c.mismatches[i]);
    }
    s += bad.empty() ? "; mismatches: none" : "; mismatches: " + bad;
    if (c.closeFloats != 0) s += std::format("; {} floats equal within 1e-5 only", c.closeFloats);
    s += std::format("; {} float registers, {} bool / int registers ({} live in the CTABs) and {} sampler stages compared", c.regs, c.biRegs,
                     c.biLive, c.stages);
    std::string classes;
    for (std::size_t i = 0; i < kDrawClassCount; ++i) {
        if (c.classDraws[i] == 0) continue;
        classes += std::format("{}{} {}/{}", classes.empty() ? "" : ", ", name(static_cast<DrawClass>(i)), c.classClean[i], c.classDraws[i]);
    }
    if (!classes.empty()) s += "; equal per class: " + classes;
    if (listNames != nullptr) {
        std::string lists;
        for (std::size_t i = 0; i < std::min(listCount, c.listDraws.size()); ++i) {
            if (c.listDraws[i] == 0) continue;
            lists += std::format("{}{} {}/{}", lists.empty() ? "" : ", ", listNames[i], c.listClean[i], c.listDraws[i]);
        }
        if (!lists.empty()) s += "; per list: " + lists;
    }
    return s;
}

// ---- sky dome ---------------------------------------------------------------------------------------------------------------

void skyBillboardVertices(std::span<const std::uint8_t> billboards, std::uint32_t count, std::vector<std::uint8_t>& out) {
    out.assign(static_cast<std::size_t>(count) * 4 * kSkyVertexStride, 0);
    for (std::uint32_t i = 0; i < count && (i + 1) * kSkyBillboardSize <= billboards.size(); ++i) {
        const std::uint8_t* b = billboards.data() + static_cast<std::size_t>(i) * kSkyBillboardSize;
        float s[2], r[2];
        std::memcpy(s, b + 0xc, 8);
        std::memcpy(r, b + 0x14, 8);
        // GetInverted(s).GetTranslated(r), s + r, s - r, -s - r (0x6e1b70..: xorps with the sign mask, addss / subss)
        const float corner[4][2] = {{-s[0] + r[0], -s[1] + r[1]}, {s[0] + r[0], s[1] + r[1]}, {s[0] - r[0], s[1] - r[1]}, {-s[0] - r[0], -s[1] - r[1]}};
        // uv per corner: (u0, v0), (u1, v0), (u1, v1), (u0, v1) from the rect @0x20 (u0, v0, u1, v1)
        static constexpr std::uint32_t kU[4] = {0x20, 0x28, 0x28, 0x20}, kV[4] = {0x24, 0x24, 0x2c, 0x2c};
        for (std::uint32_t k = 0; k < 4; ++k) {
            std::uint8_t* v = out.data() + (static_cast<std::size_t>(i) * 4 + k) * kSkyVertexStride;
            std::memcpy(v, b, 12);
            std::memcpy(v + 0xc, b + 0x1c, 4);
            std::memcpy(v + 0x10, b + kU[k], 4);
            std::memcpy(v + 0x14, b + kV[k], 4);
            std::memcpy(v + 0x18, corner[k], 8);
        }
    }
}

void skyQuadIndices(std::uint32_t count, std::vector<std::uint8_t>& out) {
    out.resize(static_cast<std::size_t>(count) * 12);
    for (std::uint32_t k = 0; k < count; ++k) {
        const std::uint16_t b = static_cast<std::uint16_t>(4 * k);
        const std::uint16_t q[6] = {b, static_cast<std::uint16_t>(b + 1), static_cast<std::uint16_t>(b + 2), b, static_cast<std::uint16_t>(b + 2),
                                    static_cast<std::uint16_t>(b + 3)};
        std::memcpy(out.data() + static_cast<std::size_t>(k) * 12, q, 12);
    }
}

RenderStates skyDomeStates(const BlendInputs& b, float fade) {
    RenderStates rs;
    rs.depthFunc = 4;
    fillBlending(rs, b, false, fade);
    rs.colorWrite = true;
    rs.alphaWrite = false;
    rs.depthWriteMaster = isBlended(b) && !b.refracted ? 1u : 0u;
    rs.renderMode = 0;
    return rs;
}

// ---- the opaque billboards' queue -------------------------------------------------------------------------------------------

void planBillboards(std::span<const BillboardEntry> entries, std::uint32_t maxQuads, std::vector<BillboardRecord>& records,
                    std::vector<BillboardPlace>& places) {
    records.clear();
    places.assign(entries.size(), {});
    std::uint32_t cycle = 0, quad = 0;
    bool locked = false;
    std::size_t open = 0;  // records of the current cycle not drawn yet
    BillboardEntry prev;
    for (std::uint32_t i = 0; i < entries.size(); ++i) {
        const BillboardEntry& e = entries[i];
        // 0x4f068c: quads of the cycle + this one's > 1000 -> the open records are drawn first (the buffers unlocked)
        if (quad + e.quads > maxQuads && open > 0) {
            open = 0;
            locked = false;
            ++cycle;
        }
        if (e.quads == 0) continue;  // 0x4f08ae: nothing locked, written or recorded
        if (!locked) {
            locked = true;
            quad = 0;
        }
        if (open == 0 || e.material != prev.material || e.image != prev.image || e.blend != prev.blend || e.lightSet != prev.lightSet ||
            e.switchValue != prev.switchValue) {
            BillboardRecord r;
            r.cycle = cycle;
            r.firstQuad = quad;
            r.firstEntry = i;
            r.keys = e;
            records.push_back(r);
            ++open;
            prev = e;
        }
        BillboardRecord& r = records.back();
        r.quads += e.quads;
        r.entryCount = i + 1 - r.firstEntry;
        places[i] = {static_cast<std::int32_t>(cycle), quad};
        quad += e.quads;
    }
}

void billboardQuadIndices(std::uint32_t firstQuad, std::uint32_t quads, std::vector<std::uint8_t>& out) {
    out.resize(static_cast<std::size_t>(quads) * 12);
    for (std::uint32_t k = 0; k < quads; ++k) {
        const std::uint16_t b = static_cast<std::uint16_t>(4 * (firstQuad + k));
        const std::uint16_t q[6] = {b, static_cast<std::uint16_t>(b + 1), static_cast<std::uint16_t>(b + 3), static_cast<std::uint16_t>(b + 1),
                                    static_cast<std::uint16_t>(b + 2), static_cast<std::uint16_t>(b + 3)};
        std::memcpy(out.data() + static_cast<std::size_t>(k) * 12, q, 12);
    }
}

}  // namespace rvk::native::sc
