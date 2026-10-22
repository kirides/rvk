#include "enh/rt_material.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <format>

#include <spdlog/spdlog.h>

#include "engine/materials.h"
#include "shader/abi.h"
#include "vk/formats.h"

namespace rvk::enh {

namespace abi = shader::abi;
namespace ev = shader::eval;
namespace mats = engine::materials;

namespace {

constexpr std::uint32_t kMaterialBase = 9;   // g_SamplerData = s9..s15
constexpr std::uint32_t kMaterialSlots = 7;
constexpr std::uint32_t kCmpGreater = 5;     // D3DCMP_GREATER (the colour pass's alpha test of masked materials)

std::uint64_t mix(std::uint64_t h, std::uint64_t v) {
    h ^= v + 0x9E3779B97F4A7C15ull + (h << 6) + (h >> 2);
    return h;
}
std::uint64_t mixFloat(std::uint64_t h, float f) {
    std::uint32_t b;
    std::memcpy(&b, &f, 4);
    return mix(h, b);
}

ev::Vec4 vec(float x, float y, float z, float w) { return {x, y, z, w}; }

bool finite(const ev::Vec4& v) { return std::isfinite(v[0]) && std::isfinite(v[1]); }

}  // namespace

bool RtMaterial::operator==(const RtMaterial& o) const {
    return albedo == o.albedo && opacity == o.opacity && sampler == o.sampler && uvSet == o.uvSet &&
           std::memcmp(uvMap, o.uvMap, sizeof(uvMap)) == 0 && colour == o.colour && alpha == o.alpha && masked == o.masked;
}

std::uint64_t RtMaterials::signature(const d3d9::ReplaceQuery& q) {
    std::uint64_t h = 0x51ED27u;
    h = mix(h, q.engineVs != nullptr ? q.engineVs->hash : 0);
    h = mix(h, q.enginePs != nullptr ? q.enginePs->hash : 0);
    h = mix(h, reinterpret_cast<std::uintptr_t>(q.tag != nullptr ? q.tag->material : nullptr));
    // the textures of the material slots the pixel shader samples (the others are stale bindings of earlier draws)
    for (std::uint32_t s = kMaterialBase; s < kMaterialBase + kMaterialSlots; ++s) {
        const bool used = q.enginePs != nullptr && q.enginePs->reflection.samplers[s] != shader::TextureType::Unknown;
        h = mix(h, reinterpret_cast<std::uintptr_t>(used && q.textures != nullptr ? q.textures[s] : nullptr));
    }
    return h;
}

const RtMaterial* RtMaterials::get(std::uint32_t id) const {
    const auto it = entries_.find(id);
    return it != entries_.end() ? &it->second.m : nullptr;
}

void RtMaterials::addRef(std::uint32_t id) {
    if (const auto it = entries_.find(id); it != entries_.end()) ++it->second.refs;
}

void RtMaterials::release(std::uint32_t id) {
    const auto it = entries_.find(id);
    if (it == entries_.end() || --it->second.refs > 0) return;
    const Entry& e = it->second;
    if (const auto k = byKey_.find(e.key); k != byKey_.end() && k->second == id) byKey_.erase(k);
    for (const d3d9::Texture* t : {static_cast<const d3d9::Texture*>(e.m.albedo), static_cast<const d3d9::Texture*>(e.m.opacity)}) {
        if (t == nullptr) continue;
        const auto range = byTexture_.equal_range(t);
        for (auto r = range.first; r != range.second;) r = r->second == id ? byTexture_.erase(r) : std::next(r);
    }
    entries_.erase(it);
}

void RtMaterials::textureReleased(const d3d9::Texture* t) {
    const auto range = byTexture_.equal_range(t);
    if (range.first == range.second) return;
    for (auto r = range.first; r != range.second; ++r) {
        const auto it = entries_.find(r->second);
        if (it == entries_.end()) continue;
        Entry& e = it->second;
        if (e.m.albedo == t) e.m.albedo = nullptr;
        if (e.m.opacity == t) e.m.opacity = nullptr;
        e.m.dead = true;
        // no longer findable by content: a new texture at the same address must not match it
        if (const auto k = byKey_.find(e.key); k != byKey_.end() && k->second == it->first) byKey_.erase(k);
    }
    byTexture_.erase(t);
}

void RtMaterials::clear() {
    entries_.clear();
    byKey_.clear();
    byTexture_.clear();
    analyses_.clear();
    programs_.clear();
}

std::uint32_t RtMaterials::intern(const RtMaterial& m) {
    std::uint64_t key = mix(reinterpret_cast<std::uintptr_t>(m.albedo), reinterpret_cast<std::uintptr_t>(m.opacity));
    key = mix(key, m.sampler);
    key = mix(key, m.uvSet);
    for (const auto& row : m.uvMap)
        for (float f : row) key = mixFloat(key, f);
    key = mix(key, m.colour);
    key = mix(key, m.alpha | (m.masked ? 0x80000000u : 0u));
    if (const auto it = byKey_.find(key); it != byKey_.end()) {
        Entry& e = entries_[it->second];
        if (e.m == m && !e.m.dead) {
            ++e.refs;
            return it->second;
        }
    }
    const std::uint32_t id = nextId_++;
    if (nextId_ == 0) nextId_ = 1;
    Entry& e = entries_[id];
    e.m = m;
    e.key = key;
    e.refs = 1;
    byKey_[key] = id;
    if (m.albedo != nullptr) byTexture_.emplace(m.albedo, id);
    if (m.opacity != nullptr && m.opacity != m.albedo) byTexture_.emplace(m.opacity, id);
    return id;
}

const RtMaterials::Program* RtMaterials::program(const d3d9::Shader* s) {
    if (s == nullptr) return nullptr;
    auto [it, inserted] = programs_.try_emplace(s->hash);
    if (inserted) {
        shader::DecodeResult r = shader::decode(s->bytecode);
        it->second.ok = r.ok() && r.program.version.major >= 3;
        if (it->second.ok) it->second.program = std::move(r.program);
    }
    return it->second.ok ? &it->second : nullptr;
}

RtMaterials::Analysis* RtMaterials::analysis(const d3d9::Shader* vs, const d3d9::Shader* ps, std::uint32_t reg) {
    const std::uint64_t key = mix(mix(vs->hash, ps->hash), reg);
    auto [it, inserted] = analyses_.try_emplace(key);
    Analysis& a = it->second;
    if (!inserted) return &a;
    const Program* vp = program(vs);
    const Program* pp = program(ps);
    if (vp == nullptr || pp == nullptr) {
        a.error = "shader not decodable as SM 3.0";
        return &a;
    }
    a.texAt = ev::firstSample(pp->program, reg);
    if (a.texAt == SIZE_MAX) {
        a.error = std::format("the pixel shader never samples s{}", reg);
        return &a;
    }
    const shader::Instruction& tex = pp->program.instructions[a.texAt];
    const std::uint8_t comps = static_cast<std::uint8_t>(0x3 | (tex.texProject() || tex.texBias() ? 0x8 : 0));
    const ev::Slice psl = ev::sliceSource(pp->program, a.texAt, 0, comps);
    // the pixel shader inputs it reads -> the vertex shader outputs of the same semantics
    std::array<std::uint8_t, 16> outs{};
    for (const shader::Declaration& in : pp->program.inputs) {
        if (in.reg.type != shader::RegType::Input || in.reg.index >= 16 || psl.inputs[in.reg.index] == 0) continue;
        for (const shader::Declaration& o : vp->program.outputs) {
            if (o.usage == in.usage && o.usageIndex == in.usageIndex && o.reg.index < 16) outs[o.reg.index] |= psl.inputs[in.reg.index];
        }
    }
    const ev::Slice vsl = ev::sliceOutputs(vp->program, outs);
    a.psConsts = psl.floatConsts;
    a.vsConsts = vsl.floatConsts;
    a.usesInts = psl.usesInts || vsl.usesInts;
    a.ok = true;
    return &a;
}

bool RtMaterials::uvMap(d3d9::Device& dev, const d3d9::ReplaceQuery& q, std::uint32_t reg, std::array<float, 7>& out, std::string& why) {
    Analysis* a = analysis(q.engineVs, q.enginePs, reg);
    if (!a->ok) {
        why = a->error;
        return false;
    }
    // the values of the constants the coordinate depends on
    std::uint64_t h = 0xC0FFEEu;
    for (std::uint16_t c : a->vsConsts) {
        const float* v = dev.vsConstant(c);
        for (int k = 0; k < 4; ++k) h = mixFloat(h, v[k]);
    }
    h = mix(h, 0xABCDu);
    for (std::uint16_t c : a->psConsts) {
        const float* v = dev.psConstant(c);
        for (int k = 0; k < 4; ++k) h = mixFloat(h, v[k]);
    }
    if (a->usesInts) {
        for (int i = 0; i < 16; ++i) {
            h = mix(h, dev.vsBoolConstants()[i] | (dev.psBoolConstants()[i] << 1));
            for (int k = 0; k < 4; ++k) h = mix(h, static_cast<std::uint32_t>(dev.vsIntConstants()[i][k]) ^ (static_cast<std::uint32_t>(dev.psIntConstants()[i][k]) << 7));
        }
    }
    if (const auto it = a->maps.find(h); it != a->maps.end()) {
        out = it->second;
        return true;
    }
    ++iv_.evaluations;
    const Program* vp = program(q.engineVs);
    const Program* pp = program(q.enginePs);
    const shader::Program& vs = vp->program;
    const shader::Program& ps = pp->program;

    // one probe: the vertex inputs -> the vertex shader -> its outputs by semantic -> the pixel shader -> the coordinate
    const auto probe = [&](ev::Vec4 uv0, ev::Vec4 uv1, bool moved, ev::Vec4& coord) -> bool {
        ev::Env venv;
        venv.floats = dev.vsConstants();
        venv.floatCount = d3d9::kVsFloatConsts;
        venv.ints = dev.vsIntConstants();
        venv.bools = dev.vsBoolConstants();
        for (const shader::Declaration& in : vs.inputs) {
            if (in.reg.type != shader::RegType::Input || in.reg.index >= 16) continue;
            ev::Vec4 v{0, 0, 0, 1};
            switch (in.usage) {
            case shader::Usage::Position: v = moved ? vec(-40.0f, 15.0f, 70.0f, 1.0f) : vec(10.0f, 20.0f, 30.0f, 1.0f); break;
            case shader::Usage::Normal: v = moved ? vec(0.6f, 0.8f, 0.0f, 1.0f) : vec(0.0f, 0.0f, 1.0f, 1.0f); break;
            case shader::Usage::Tangent: v = moved ? vec(0.0f, 0.0f, 1.0f, 1.0f) : vec(1.0f, 0.0f, 0.0f, 1.0f); break;
            case shader::Usage::Color: v = vec(1.0f, 1.0f, 1.0f, 1.0f); break;
            case shader::Usage::TexCoord:
                v = in.usageIndex == 0 ? uv0 : in.usageIndex == 1 ? uv1 : in.usageIndex == 6 ? vec(0.5f, 0.5f, 0.0f, 1.0f) : vec(0, 0, 0, 1);
                break;
            case shader::Usage::BlendWeight:
            case shader::Usage::BlendIndices: v = vec(0, 0, 0, 0); break;
            default: break;
            }
            venv.inputs[in.reg.index] = v;
        }
        const ev::Result vr = ev::run(vs, venv);
        if (!vr.ok) {
            why = "vertex shader: " + vr.error;
            return false;
        }
        ev::Env penv;
        penv.floats = dev.psConstants();
        penv.floatCount = d3d9::kPsFloatConsts;
        penv.ints = dev.psIntConstants();
        penv.bools = dev.psBoolConstants();
        penv.position = vec(100.0f, 100.0f, 0.0f, 0.0f);
        for (const shader::Declaration& in : ps.inputs) {
            if (in.reg.type != shader::RegType::Input || in.reg.index >= 16) continue;
            for (const shader::Declaration& o : vs.outputs) {
                if (o.usage == in.usage && o.usageIndex == in.usageIndex && o.reg.index < 16) penv.inputs[in.reg.index] = vr.outputs[o.reg.index];
            }
        }
        bool seen = false;
        const std::size_t at = a->texAt;
        penv.sample = [&](std::uint32_t, const ev::Vec4& c, std::size_t i) {
            if (i == at && !seen) {
                coord = c;
                seen = true;
            }
            return ev::Vec4{0.5f, 0.5f, 0.5f, 1.0f};
        };
        const ev::Result pr = ev::run(ps, penv);
        if (!pr.ok) {
            why = "pixel shader: " + pr.error;
            return false;
        }
        if (!seen) {
            why = "the diffuse texld was not reached";
            return false;
        }
        const shader::Instruction& tex = ps.instructions[at];
        if (tex.texProject() && coord[3] != 0.0f) {
            coord[0] /= coord[3];
            coord[1] /= coord[3];
        }
        if (!finite(coord)) {
            why = "not finite";
            return false;
        }
        return true;
    };
    ev::Vec4 c[7];
    const ev::Vec4 z{0, 0, 0, 1};
    if (!probe(z, z, false, c[0]) || !probe(vec(1, 0, 0, 1), z, false, c[1]) || !probe(vec(0, 1, 0, 1), z, false, c[2]) ||
        !probe(z, vec(1, 0, 0, 1), false, c[3]) || !probe(z, vec(0, 1, 0, 1), false, c[4]) ||
        !probe(vec(0.3f, -0.7f, 0, 1), vec(0.6f, 0.2f, 0, 1), false, c[5]) || !probe(z, z, true, c[6])) {
        ++iv_.failed;
        return false;
    }
    // coord = b + A0 uv0 + A1 uv1
    float A0[2][2], A1[2][2], b[2];
    for (int r = 0; r < 2; ++r) {
        b[r] = c[0][r];
        A0[r][0] = c[1][r] - b[r];
        A0[r][1] = c[2][r] - b[r];
        A1[r][0] = c[3][r] - b[r];
        A1[r][1] = c[4][r] - b[r];
    }
    std::uint32_t bits = 0;
    bool affine = true, world = false;
    for (int r = 0; r < 2; ++r) {
        const float pred = b[r] + A0[r][0] * 0.3f + A0[r][1] * -0.7f + A1[r][0] * 0.6f + A1[r][1] * 0.2f;
        if (std::fabs(pred - c[5][r]) > 1e-3f * (1.0f + std::fabs(c[5][r]))) affine = false;
        if (std::fabs(c[6][r] - c[0][r]) > 1e-4f * (1.0f + std::fabs(c[0][r]))) world = true;
    }
    const float n0 = std::fabs(A0[0][0]) + std::fabs(A0[0][1]) + std::fabs(A0[1][0]) + std::fabs(A0[1][1]);
    const float n1 = std::fabs(A1[0][0]) + std::fabs(A1[0][1]) + std::fabs(A1[1][0]) + std::fabs(A1[1][1]);
    const bool set1 = n1 > n0;
    if (n0 > 0.0f && n1 > 0.0f) affine = false;  // both sets: the larger one, approximately
    if (!affine || world) bits |= abi::kRtUvApprox;
    if (set1) bits |= 1u;
    const auto& A = set1 ? A1 : A0;
    out = {A[0][0], A[0][1], b[0], A[1][0], A[1][1], b[1], static_cast<float>(bits)};
    if ((bits & abi::kRtUvApprox) != 0) ++iv_.approx;
    if (a->maps.size() > 256) a->maps.clear();  // animated maps: bounded
    a->maps.emplace(h, out);
    return true;
}

std::uint32_t RtMaterials::resolve(d3d9::Device& dev, const d3d9::ReplaceQuery& q) {
    if (q.engineVs == nullptr || q.enginePs == nullptr || q.textures == nullptr) return 0;
    const auto t0 = std::chrono::steady_clock::now();
    ++iv_.resolves;
    RtMaterial m;
    std::string detail;
    // the slot from the material graph of the executing shader
    // the draw tag's material when there is one (its shader = ms_pActiveShader in every main-view solid draw checked, and
    // right where the active shader is stale: z pre-pass, cascades), else the executing shader
    const void* tagMaterial = q.tag != nullptr ? q.tag->material : nullptr;
    mats::ShaderInfo info;
    const bool graph = mats::active(info, tagMaterial) && (info.cls == mats::ShaderClass::Default || info.cls == mats::ShaderClass::Leafs);
    if (tagMaterial != nullptr && q.phase.view == d3d9::scene::View::Main && q.phase.pass == d3d9::scene::Pass::Solid) {
        mats::ShaderInfo executing;
        if (mats::active(executing) && executing.shader != info.shader) ++iv_.shaderMismatch;
    }
    int slot = -1;
    const auto& samplers = q.enginePs->reflection.samplers;
    if (graph && info.diffuse.source == mats::Source::Sampler && info.diffuse.slot >= 0) {
        slot = info.samplerBase + info.diffuse.slot;
        ++iv_.graph;
    } else if (graph && info.diffuse.source != mats::Source::Sampler) {
        m.uvSet |= abi::kRtUvNoTexture;  // constant / vertex colour / cube-only diffuse: a neutral grey
        ++iv_.noTexture;
    } else if (samplers[kMaterialBase] == shader::TextureType::Tex2D) {
        slot = static_cast<int>(kMaterialBase);
        m.uvSet |= abi::kRtUvHeuristic;
        ++iv_.heuristic;
    } else {
        m.uvSet |= abi::kRtUvNoTexture;
        ++iv_.noTexture;
    }
    if (slot >= 0) {
        d3d9::Texture* t = slot < 16 ? q.textures[slot] : nullptr;
        const auto usable = [](const d3d9::Texture* x) { return x != nullptr && !x->isNull() && x->kind() == vk::TextureKind::Tex2D; };
        d3d9::Texture* pool = nullptr;
        if (!usable(t) || samplers[static_cast<std::size_t>(slot)] != shader::TextureType::Tex2D) {
            // the pixel shader does not sample the slot (a depth / shadow pass): the material's pool image, unmapped uv
            // (only with the tag's material: without it the executing shader may be stale, a depth pass runs no material)
            if (graph && tagMaterial != nullptr && info.diffuse.image != nullptr) pool = dev.engineTexture(mats::apiTextureOf(info.diffuse.image));
            if (!usable(pool)) pool = nullptr;
        }
        if (pool != nullptr) {
            m.albedo = pool;
            m.sampler = dev.samplerFor(true, false);
            m.uvSet |= (info.diffuse.uvSet & 1u) | (info.diffuse.uvSource ? abi::kRtUvApprox : 0u);
            ++iv_.pool;
            detail += std::format(" (s{} not sampled: the pool image, uv set {} unmapped)", slot, info.diffuse.uvSet);
        } else if (!usable(t) || samplers[static_cast<std::size_t>(slot)] != shader::TextureType::Tex2D) {
            m.uvSet |= abi::kRtUvNoTexture;
            detail += std::format(" (s{}: no 2D texture bound)", slot);
        } else {
            m.albedo = t;
            m.sampler = dev.samplerOfSlot(static_cast<std::uint32_t>(slot));
            std::array<float, 7> map{};
            std::string why;
            if (uvMap(dev, q, static_cast<std::uint32_t>(slot), map, why)) {
                m.uvMap[0][0] = map[0];
                m.uvMap[0][1] = map[1];
                m.uvMap[0][2] = map[2];
                m.uvMap[1][0] = map[3];
                m.uvMap[1][1] = map[4];
                m.uvMap[1][2] = map[5];
                m.uvSet |= static_cast<std::uint32_t>(map[6]);
            } else {
                m.uvSet |= abi::kRtUvApprox;  // identity on TEXCOORD0
                detail += " (uv: " + why + ")";
            }
        }
    }
    // opacity of masked materials: the Opacity tree's sampler (channel by the proxy's swizzle), else the diffuse alpha
    std::uint32_t channel = 3;
    if (graph) {
        m.masked = info.blendMode == 1;
        if (m.masked) m.alpha = kCmpGreater | (static_cast<std::uint32_t>(info.maskReference) << 8);
        const mats::Role& o = info.opacity;
        if (o.source == mats::Source::Sampler && o.slot >= 0) {
            const int os = info.samplerBase + o.slot;
            channel = o.swizzle >= 2 && o.swizzle <= 5 ? static_cast<std::uint32_t>(o.swizzle - 2) : 3u;
            if (os != slot) {
                d3d9::Texture* t = os < 16 ? q.textures[os] : nullptr;
                if (t != nullptr && !t->isNull() && t->kind() == vk::TextureKind::Tex2D) {
                    m.opacity = t;
                    m.uvSet |= abi::kRtUvOpacityTexture;
                } else {
                    m.uvSet |= abi::kRtUvOpacityOpaque;
                }
            }
        } else if (o.source != mats::Source::None) {
            m.uvSet |= abi::kRtUvOpacityOpaque;
        }
    }
    m.uvSet |= channel << abi::kRtUvOpacityShift;
    if (graph && info.diffuse.slot >= 0) m.name = mats::poolImageName(info, info.diffuse.slot);
    if (m.name.empty() && q.tag != nullptr && q.tag->material != nullptr) m.name = mats::resourceName(q.tag->material);
    const std::uint32_t id = intern(m);
    iv_.ms += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();

    // once per material x pixel shader: what was found (the verification of the slot method in rvk.log)
    const std::uint64_t logKey = mix(reinterpret_cast<std::uintptr_t>(q.tag != nullptr ? q.tag->material : nullptr), q.enginePs->hash);
    if (logged_.size() < 600 && logged_.insert(logKey).second) {
        std::string s;
        if (graph) {
            const auto role = [&](const char* what, const mats::Role& r) {
                return std::format(" {} {}{}", what, mats::name(r.source),
                                   r.slot >= 0 ? std::format(" k{} s{} '{}'{}", r.slot, info.samplerBase + r.slot, mats::poolImageName(info, r.slot),
                                                             r.uvSource ? " (tex-coord source)" : std::format(" (uv set {})", r.uvSet))
                                               : std::string());
            };
            s = std::format("shader {} {} blend {} ref {} pool {}:{}{}{}", info.shader, mats::name(info.cls), info.blendMode, info.maskReference,
                            info.samplerCount, role("diffuse", info.diffuse), role("opacity", info.opacity), role("normal", info.normal));
            if (info.opacity.source != mats::Source::None) s += std::format(" (opacity swizzle {})", info.opacity.swizzle);
        } else {
            s = "no material graph (bytecode fallback s9)";
        }
        std::string declared;
        for (std::uint32_t r = kMaterialBase; r < 16; ++r) {
            if (samplers[r] == shader::TextureType::Unknown) continue;
            const d3d9::Texture* t = q.textures[r];
            declared += std::format(" s{}={}", r, t == nullptr ? std::string("-") : std::format("{}x{} {}", t->desc().width, t->desc().height, vk::d3dFormatName(t->desc().format)));
        }
        const std::string matName = q.tag != nullptr && q.tag->material != nullptr ? mats::resourceName(q.tag->material) : std::string("?");
        spdlog::info("rt: material '{}' ({} {}, vs {:016x} ps {:016x}): {}; bound{}; albedo {} uv set {} map ({:.4g} {:.4g} {:.4g} / {:.4g} {:.4g} {:.4g}){}{}{}{}",
                     matName, d3d9::draws::name(q.kind), d3d9::scene::name(q.phase.view), q.engineVs->hash, q.enginePs->hash, s, declared,
                     slot >= 0 ? std::format("s{}", slot) : std::string("none"), m.uvSet & abi::kRtUvSetMask, m.uvMap[0][0], m.uvMap[0][1],
                     m.uvMap[0][2], m.uvMap[1][0], m.uvMap[1][1], m.uvMap[1][2], (m.uvSet & abi::kRtUvApprox) ? " approx" : "",
                     (m.uvSet & abi::kRtUvNoTexture) ? " no-texture" : "", (m.uvSet & abi::kRtUvHeuristic) ? " heuristic" : "", detail);
    }
    return id;
}

}  // namespace rvk::enh
