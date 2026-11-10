// D3D9 SM 1.x-3.0 bytecode -> HLSL (SM 6.6) per the contract in shader/abi.h; the port of glsl.cpp (read it for the semantics: registers are
// module-scope variables so that D3D9 subroutines map to parameterless functions; the entry point applies the D3D9 -> Vulkan fix-ups
// (half-pixel offset, alpha test) and copies the output registers to the entry's output struct). Same reflection, same hooks.
//
// Differences that matter: every address of the ABI is a (low, high) pair read through rvkPtr (shaders/include/rvk_draw.hlsli); the stage's
// inputs / outputs are structs with explicit [[vk::location]] (D3D9 register / semantic location, abi.h varyingLocation); boolean vector
// selects are select(); GLSL's `mix(a, b, bvec)` is select(cond, b, a).

#include "shader/hlsl.h"

#include "shader/embedded.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdlib>
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <cstring>
#include <format>
#include <map>
#include <set>

namespace rvk::shader {

static std::atomic<bool> g_varyingFillers{false};
// the VARY<n> index of a variant's extra varying declaration (-1: none)
static std::int64_t extraVaryLocation(const std::string& text) {
    const auto p = text.find("VARY");
    return p == std::string::npos ? -1 : std::strtoll(text.c_str() + p + 4, nullptr, 10);
}
void setVaryingFillers(bool on) { g_varyingFillers.store(on); }
bool varyingFillers() { return g_varyingFillers.load(); }

namespace {

constexpr const char* kComp = "xyzw";

bool isIdentifier(const std::string& s) {
    return !s.empty() && std::all_of(s.begin(), s.end(), [](char c) {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '.';
    });
}

std::string paren(const std::string& s) { return isIdentifier(s) ? s : "(" + s + ")"; }

std::string maskSuffix(std::uint8_t mask) {
    std::string s;
    for (int c = 0; c < 4; ++c) {
        if ((mask & (1 << c)) != 0) {
            s += kComp[c];
        }
    }
    return s;
}

// Exact HLSL literal of a float given by its bits (shortest round-trip form); inf/NaN have no literal.
std::string floatLiteral(std::uint32_t bits, bool& isConstant) {
    float f;
    std::memcpy(&f, &bits, sizeof(f));
    if (!std::isfinite(f)) {
        isConstant = false;
        return std::format("asfloat(0x{:08x}u)", bits);
    }
    std::string s = std::format("{}", f);
    if (s.find_first_of(".en") == std::string::npos) {
        s += ".0";
    }
    return s;
}

const char* cmpOp(Cmp c) {
    switch (c) {
    case Cmp::Gt: return ">";
    case Cmp::Eq: return "==";
    case Cmp::Ge: return ">=";
    case Cmp::Lt: return "<";
    case Cmp::Ne: return "!=";
    case Cmp::Le: return "<=";
    default: return "==";
    }
}

// a float4 splat of a scalar expression
std::string splat(const std::string& f) { return "(float4)(" + f + ")"; }

class Generator {
public:
    Generator(const Program& p, const TranslateOptions& o)
        : p_(p), opt_(o), vs_(p.version.stage == Stage::Vertex), major_(p.version.major), consts_(vs_ ? "pc.vs" : "pc.ps") {}

    bool scene() const { return opt_.sceneRecord || opt_.sceneView; }

    HlslResult run() {
        HlslResult res;
        res.reflection.version = p_.version;
        scan();
        if (!error_.empty()) {
            res.error = error_;
            return res;
        }
        emitBody();
        if (error_.empty() && scene()) checkScene();
        if (error_.empty() && opt_.pullVertices && (!vs_ || !opt_.sceneView || !opt_.extraInputs.empty())) error_ = "vertex pulling: only for scene ABI v2 vertex shaders without extra inputs";
        if (!error_.empty()) {
            res.error = error_;
            return res;
        }
        res.hlsl = header() + (opt_.prelude.empty() ? std::string() : opt_.prelude + "\n") + globals() +
                   (opt_.extraGlobals.empty() ? std::string() : opt_.extraGlobals + "\n") + functions_ + entryPoint();
        res.reflection = reflection_;
        res.reflection.version = p_.version;
        res.reflection.intBoolConstants = readsIntBool_;
        res.reflection.splitConsts = !readsIntBool_;
        return res;
    }

private:
    // ---- pass 1: what the shader uses (identical to glsl.cpp) ------------------------------------------------------------

    std::uint32_t regLimit() const { return vs_ ? abi::kVsWindowFirst[abi::kConstWindows] : abi::kPsWindowFirst[abi::kConstWindows]; }

    void noteWindow(std::uint32_t reg, bool wholeWindow) {
        if (reg >= regLimit()) return;
        const std::uint32_t w = abi::constWindow(vs_, reg);
        const std::uint32_t end = wholeWindow ? (vs_ ? abi::kVsWindowFirst[w + 1] : abi::kPsWindowFirst[w + 1]) : reg + 1;
        reflection_.windowEnd[w] = static_cast<std::uint16_t>(std::max<std::uint32_t>(reflection_.windowEnd[w], end));
    }

    void noteRead(const Register& r, const RelAddr& rel) {
        if (r.type == RegType::Const) {
            noteWindow(r.index, rel.present);
            if (rel.present) {
                reflection_.relativeConstants = true;
                reflection_.floatConstCount = abi::kFloatConsts;
            } else {
                reflection_.floatConstCount = std::max(reflection_.floatConstCount, r.index + 1);
            }
        } else if (r.type == RegType::Temp || r.type == RegType::TempFloat16) {
            temps_.insert(r.index);
        } else if (r.type == RegType::MiscType && r.index == 1) {
            usesFace_ = true;
        }
    }

    void noteWrite(const Register& r) {
        switch (r.type) {
        case RegType::Temp:
        case RegType::TempFloat16: temps_.insert(r.index); break;
        case RegType::RastOut: rastOut_.insert(r.index); break;
        case RegType::AttrOut: attrOut_.insert(r.index); break;
        case RegType::Output: outputs_.insert(r.index); break;
        case RegType::ColorOut: reflection_.colorOutputs |= 1u << r.index; break;
        case RegType::DepthOut: reflection_.writesDepth = true; break;
        default: break;
        }
    }

    void scan() {
        for (const auto& d : p_.defs) {
            if (d.op == Op::Def) {
                defF_[d.dst.reg.index] = d.imm;
            } else if (d.op == Op::DefI) {
                defI_[d.dst.reg.index] = d.imm;
            } else {
                defB_[d.dst.reg.index] = d.imm[0] != 0;
            }
        }
        for (const auto& s : p_.samplers) {
            const std::uint32_t slot = vs_ ? abi::kVertexSamplerBase + s.index : s.index;
            if (slot < abi::kSamplerSlots) {
                reflection_.samplers[slot] = s.type == TextureType::Unknown ? TextureType::Tex2D : s.type;
            }
        }
        for (const auto& ins : p_.instructions) {
            if (ins.op == Op::Label && !ins.src.empty()) {
                labels_.insert(ins.src[0].reg.index);
            }
            if (ins.op == Op::TexKill) {
                reflection_.discards = true;
            }
            if (ins.hasDst && ins.op != Op::TexKill) {
                noteWrite(ins.dst.reg);
            }
            if (ins.op == Op::TexKill) {
                noteRead(ins.dst.reg, ins.dst.rel);
            }
            for (const auto& s : ins.src) {
                noteRead(s.reg, s.rel);
            }
            const int rows = matrixRows(ins.op);
            if (rows > 0 && ins.src.size() >= 2 && ins.src[1].reg.type == RegType::Const && !ins.src[1].rel.present) {
                reflection_.floatConstCount = std::max(reflection_.floatConstCount, ins.src[1].reg.index + rows);
                for (int k = 0; k < rows; ++k) noteWindow(ins.src[1].reg.index + static_cast<std::uint32_t>(k), false);
            }
            if (rows > 0 && ins.src.size() >= 2 &&
                (ins.src[1].reg.type == RegType::Temp || ins.src[1].reg.type == RegType::TempFloat16)) {
                for (int k = 0; k < rows; ++k) {
                    temps_.insert(ins.src[1].reg.index + static_cast<std::uint32_t>(k));
                }
            }
            if (ins.op == Op::Tex || ins.op == Op::TexLdl || ins.op == Op::TexLdd) {
                if (ins.src.size() >= 2 && ins.src[1].reg.type == RegType::Sampler) {
                    const std::uint32_t slot = vs_ ? abi::kVertexSamplerBase + ins.src[1].reg.index : ins.src[1].reg.index;
                    if (slot < abi::kSamplerSlots && reflection_.samplers[slot] == TextureType::Unknown) {
                        reflection_.samplers[slot] = TextureType::Tex2D;
                    }
                }
            }
            if (ins.hasDst && ins.dst.rel.present) {
                error_ = "relative addressing of an output register is not supported";
            }
        }
        for (const auto& d : p_.inputs) {
            if (vs_) {
                if (d.reg.type == RegType::Input) {
                    reflection_.vertexInputs.push_back({d.reg.index, d.usage, d.usageIndex});
                }
            } else if (const std::uint32_t loc = psInputLocation(d); loc != abi::kInvalidLocation) {
                reflection_.varyingMask |= 1u << loc;
                reflection_.pixelInputs.push_back(
                    {loc, d.reg.type == RegType::Addr ? std::format("t{}", d.reg.index) : std::format("v{}", d.reg.index)});
            }
        }
        if (vs_) {
            if (major_ >= 3) {
                for (const auto& d : p_.outputs) {
                    if (const std::uint32_t loc = abi::varyingLocation(d.usage, d.usageIndex); loc != abi::kInvalidLocation) {
                        reflection_.varyingMask |= 1u << loc;
                    }
                    if (d.usage == Usage::PSize) {
                        reflection_.writesPointSize = true;
                    }
                }
            } else {
                for (auto i : attrOut_) {
                    reflection_.varyingMask |= 1u << abi::varyingLocation(Usage::Color, i);
                }
                for (auto i : outputs_) {
                    reflection_.varyingMask |= 1u << abi::varyingLocation(Usage::TexCoord, i);
                }
                if (rastOut_.contains(1)) {
                    reflection_.varyingMask |= 1u << abi::varyingLocation(Usage::Fog, 0);
                }
                reflection_.writesPointSize = rastOut_.contains(2);
            }
        }
    }

    static int matrixRows(Op op) {
        switch (op) {
        case Op::M4x4: return 4;
        case Op::M4x3: return 3;
        case Op::M3x4: return 4;
        case Op::M3x3: return 3;
        case Op::M3x2: return 2;
        default: return 0;
        }
    }

    std::uint32_t psInputLocation(const Declaration& d) const {
        if (d.reg.type == RegType::MiscType) {
            return abi::kInvalidLocation;
        }
        if (major_ >= 3) {
            return abi::varyingLocation(d.usage, d.usageIndex);
        }
        if (d.reg.type == RegType::Input) {
            return abi::varyingLocation(Usage::Color, d.reg.index);
        }
        if (d.reg.type == RegType::Addr) {  // t#
            return abi::varyingLocation(Usage::TexCoord, d.reg.index);
        }
        return abi::kInvalidLocation;
    }

    // ---- expressions ------------------------------------------------------------------------------------------------

    std::string relIndex(const RelAddr& rel) const {
        if (rel.reg.type == RegType::Loop) {
            return "aL";
        }
        return std::format("a0.{}", kComp[rel.component]);
    }

    // a float constant register read: through the window block that owns it (rvk_draw.hlsli RVK_VSC / RVK_PSC)
    std::string constRead(std::uint32_t n) const { return std::format("{}({})", vs_ ? "RVK_VSC" : "RVK_PSC", n); }

    std::string regExpr(const Register& r, const RelAddr& rel) {
        const auto n = r.index;
        switch (r.type) {
        case RegType::Temp:
        case RegType::TempFloat16: return std::format("r{}", n);
        case RegType::Input: return std::format("v{}", n);
        case RegType::Const:
            if (rel.present) {
                return std::format("rvk_cRel({} + {})", relIndex(rel), n);
            }
            if (defF_.contains(n)) {
                return std::format("c{}_def", n);
            }
            if (auto it = opt_.constOverrides.find(n); it != opt_.constOverrides.end()) {
                return overrideExpr(it->second, constRead(n));
            }
            return constRead(n);
        case RegType::Addr: return vs_ ? "(float4)a0" : std::format("t{}", n);
        case RegType::RastOut: return n == 0 ? "oPos" : n == 1 ? "oFog" : "oPts";
        case RegType::AttrOut: return std::format("oD{}", n);
        case RegType::Output: return major_ >= 3 ? std::format("o{}", n) : std::format("oT{}", n);
        case RegType::ConstInt: return std::format("(float4){}", intExpr(n));
        case RegType::ConstBool: return std::format("(float4)({} ? 1.0 : 0.0)", boolConst(n));
        case RegType::ColorOut: return std::format("oC{}", n);
        case RegType::DepthOut: return "oDepth";
        case RegType::Loop: return "(float4)float(aL)";
        case RegType::MiscType:
            return n == 0 ? "float4(rvk_fragCoord.xy - 0.5, 0.0, 0.0)" : "(float4)(rvk_front ? 1.0 : -1.0)";
        case RegType::Predicate: return "(float4)p0";
        default:
            error_ = std::format("unsupported source register type {}", static_cast<int>(r.type));
            return "(float4)0.0";
        }
    }

    // TranslateOptions::constOverrides / inputOverrides: the expression with `$c` (`$v`) replaced by the original read
    static std::string overrideExpr(const std::string& expr, const std::string& original, std::string_view placeholder = "$c") {
        std::string out = "(";
        for (std::size_t i = 0; i < expr.size(); ++i) {
            if (expr[i] == '$' && i + 1 < expr.size() && expr[i + 1] == placeholder[1]) {
                out += original;
                ++i;
            } else {
                out += expr[i];
            }
        }
        return out + ")";
    }

    std::string intExpr(std::uint32_t n) const {
        if (auto it = defI_.find(n); it != defI_.end()) {
            return std::format("int4({}, {}, {}, {})", static_cast<std::int32_t>(it->second[0]), static_cast<std::int32_t>(it->second[1]),
                               static_cast<std::int32_t>(it->second[2]), static_cast<std::int32_t>(it->second[3]));
        }
        readsIntBool_ = true;
        return std::format("rvkPtr<RvkStageData, 16>({}).Get().i[{}]", consts_, n);
    }

    std::string boolConst(std::uint32_t n) const {
        if (auto it = defB_.find(n); it != defB_.end()) {
            return it->second ? "true" : "false";
        }
        readsIntBool_ = true;
        return std::format("(rvkPtr<RvkStageData, 16>({}).Get().b[{}][{}] != 0u)", consts_, n >> 2, n & 3);
    }

    std::string swizzled(const std::string& base, const std::array<std::uint8_t, 4>& sw) {
        if (sw == std::array<std::uint8_t, 4>{0, 1, 2, 3}) {
            return base;
        }
        std::string s = paren(base) + ".";
        for (auto c : sw) {
            s += kComp[c];
        }
        return s;
    }

    std::string srcExpr(const SrcParam& s, std::uint32_t indexOffset = 0) {
        Register r = s.reg;
        r.index += indexOffset;
        const std::string v = swizzled(regExpr(r, s.rel), s.swizzle);
        switch (s.mod) {
        case SrcMod::None: return v;
        case SrcMod::Neg: return "-" + paren(v);
        case SrcMod::Bias: return std::format("({} - 0.5)", v);
        case SrcMod::BiasNeg: return std::format("(0.5 - {})", v);
        case SrcMod::Sign: return std::format("({} * 2.0 - 1.0)", v);
        case SrcMod::SignNeg: return std::format("(1.0 - {} * 2.0)", v);
        case SrcMod::Comp: return std::format("(1.0 - {})", v);
        case SrcMod::X2: return std::format("({} * 2.0)", v);
        case SrcMod::X2Neg: return std::format("({} * -2.0)", v);
        case SrcMod::Abs: return std::format("abs({})", v);
        case SrcMod::AbsNeg: return std::format("-abs({})", v);
        case SrcMod::Dz:
        case SrcMod::Dw:
        case SrcMod::Not:
            error_ = std::format("source modifier {} is not supported here", static_cast<int>(s.mod));
            return v;
        }
        return v;
    }

    std::string scalar(const SrcParam& s) { return paren(srcExpr(s)) + ".x"; }

    // b# or p0.<component> (if, callnz, breakp)
    std::string boolSrc(const SrcParam& s) {
        std::string b;
        if (s.reg.type == RegType::ConstBool) {
            b = boolConst(s.reg.index);
        } else if (s.reg.type == RegType::Predicate) {
            b = std::format("p0.{}", kComp[s.swizzle[0]]);
        } else {
            error_ = "boolean source must be b# or p0";
            return "false";
        }
        return s.mod == SrcMod::Not ? "!" + paren(b) : b;
    }

    // ---- statements -------------------------------------------------------------------------------------------------

    void line(const std::string& s) {
        body_.append(static_cast<std::size_t>(indent_) * 4, ' ');
        body_ += s;
        body_ += '\n';
    }

    std::string dstName(const DstParam& d) {
        const auto n = d.reg.index;
        switch (d.reg.type) {
        case RegType::Temp:
        case RegType::TempFloat16: return std::format("r{}", n);
        case RegType::RastOut: return n == 0 ? "oPos" : n == 1 ? "oFog" : "oPts";
        case RegType::AttrOut: return std::format("oD{}", n);
        case RegType::Output: return major_ >= 3 ? std::format("o{}", n) : std::format("oT{}", n);
        case RegType::ColorOut: return std::format("oC{}", n);
        case RegType::DepthOut: return "oDepth";
        default:
            error_ = std::format("unsupported destination register type {}", static_cast<int>(d.reg.type));
            return "rvk_dummy";
        }
    }

    void assign(const Instruction& ins, std::string value) {
        const DstParam& d = ins.dst;
        if (d.saturate) {
            value = std::format("clamp({}, 0.0, 1.0)", value);
        }
        const std::string target = dstName(d);
        const std::uint8_t mask = d.writeMask;
        if (mask == 0) {
            return;
        }
        if (!ins.predicated) {
            if (mask == 0xF) {
                line(std::format("{} = {};", target, value));
            } else {
                const std::string m = maskSuffix(mask);
                line(std::format("{}.{} = {}.{};", target, m, paren(value), m));
            }
            return;
        }
        line("{");
        ++indent_;
        line(std::format("float4 rvk_t = {};", value));
        for (int c = 0; c < 4; ++c) {
            if ((mask & (1 << c)) == 0) {
                continue;
            }
            const std::string cond = std::format("{}p0.{}", ins.predicate.mod == SrcMod::Not ? "!" : "",
                                                 kComp[ins.predicate.swizzle[static_cast<std::size_t>(c)]]);
            line(std::format("if ({}) {}.{} = rvk_t.{};", cond, target, kComp[c], kComp[c]));
        }
        --indent_;
        line("}");
    }

    // the texture object and the sampler of a D3D9 sampler slot of the draw (bindless: rvk_draw.hlsli)
    std::string textureObject(std::uint32_t slot) const {
        const TextureType t = reflection_.samplers[slot];
        const char* arr = t == TextureType::Cube ? "RVK_TEXCUBE_UNI" : t == TextureType::Volume ? "RVK_TEX3D_UNI" : "RVK_TEX2D_UNI";
        return std::format("{}(rvk_draw.tex[{}][{}])", arr, slot >> 2, slot & 3);
    }
    std::string samplerObject(std::uint32_t slot) const { return std::format("RVK_SAMP_UNI(rvk_draw.smp[{}][{}])", slot >> 2, slot & 3); }

    std::string coord(std::uint32_t samplerIndex, const std::string& v) {
        const std::uint32_t slot = vs_ ? abi::kVertexSamplerBase + samplerIndex : samplerIndex;
        const TextureType t = slot < abi::kSamplerSlots ? reflection_.samplers[slot] : TextureType::Tex2D;
        return paren(v) + (t == TextureType::Cube || t == TextureType::Volume ? ".xyz" : ".xy");
    }

    void emitTex(const Instruction& ins) {
        if (ins.src.size() < 2 || ins.src[1].reg.type != RegType::Sampler) {
            error_ = std::format("{}: expected a sampler operand", opName(ins.op));
            return;
        }
        const std::uint32_t s = ins.src[1].reg.index;
        const std::uint32_t slot = vs_ ? abi::kVertexSamplerBase + s : s;
        if (slot >= abi::kSamplerSlots) {
            error_ = std::format("sampler s{} out of range", s);
            return;
        }
        const std::string tex = textureObject(slot), smp = samplerObject(slot);
        const std::string c = srcExpr(ins.src[0]);
        std::string v;
        if (ins.op == Op::TexLdl) {
            v = std::format("{}.SampleLevel({}, {}, {}.w)", tex, smp, coord(s, c), paren(c));
        } else if (ins.op == Op::TexLdd) {
            if (ins.src.size() < 4) {
                error_ = "texldd needs 4 operands";
                return;
            }
            v = std::format("{}.SampleGrad({}, {}, {}, {})", tex, smp, coord(s, c), coord(s, srcExpr(ins.src[2])), coord(s, srcExpr(ins.src[3])));
        } else if (vs_) {
            v = std::format("{}.SampleLevel({}, {}, 0.0)", tex, smp, coord(s, c));
        } else if (ins.texProject()) {
            // texldp: the coordinate over its w (a cube: xyz / w)
            const bool cube = reflection_.samplers[slot] == TextureType::Cube;
            v = std::format("{}.Sample({}, {}.{} / {}.w)", tex, smp, paren(c), cube ? "xyz" : "xy", paren(c));
        } else if (ins.texBias()) {
            v = std::format("{}.SampleBias({}, {}, {}.w)", tex, smp, coord(s, c), paren(c));
        } else {
            v = std::format("{}.Sample({}, {})", tex, smp, coord(s, c));
        }
        for (std::size_t k = 0; k < opt_.texCaptures.size() && !vs_; ++k) {
            const auto& cap = opt_.texCaptures[k];
            if (cap.samplerReg != s || texCapturesDone_.contains(k)) continue;
            texCapturesDone_.insert(k);
            line("{");
            line(std::format("    float4 rvk_ts = {};", v));
            line(std::format("    {} = float4({}.xy, 0.0, 0.0); {} = rvk_ts; {} = true;", cap.uvTarget, paren(c), cap.colorTarget, cap.okTarget));
            assign(ins, "rvk_ts");
            line("}");
            return;
        }
        assign(ins, v);
    }

    void openFunction(const std::string& name) {
        body_.clear();
        indent_ = 1;
        currentFunction_ = name;
        functionOpen_ = true;
    }

    void closeFunction() {
        if (!functionOpen_) {
            return;
        }
        if (depth_ != 0) {
            error_ = std::format("unbalanced control flow in {}", currentFunction_);
        }
        functions_ += std::format("void {}() {{\n{}}}\n\n", currentFunction_, body_);
        functionOpen_ = false;
        depth_ = 0;
    }

    void emitBody() {
        openFunction("rvk_main");
        int loopId = 0;
        for (const auto& ins : p_.instructions) {
            if (!error_.empty()) {
                return;
            }
            if (ins.op == Op::Label) {
                closeFunction();
                openFunction(std::format("l{}", ins.src.empty() ? 0 : ins.src[0].reg.index));
                continue;
            }
            if (!functionOpen_) {
                error_ = std::format("instruction {} outside of a function", opName(ins.op));
                return;
            }
            if (ins.op == Op::Ret) {
                if (depth_ == 0) {
                    closeFunction();
                } else {
                    line("return;");
                }
                continue;
            }
            emitCaptures(ins);
            emitInstruction(ins, loopId);
        }
        closeFunction();
    }

    // TranslateOptions::dotCaptures (first match each)
    void emitCaptures(const Instruction& ins) {
        if (opt_.dotCaptures.empty() || (ins.op != Op::Dp3 && ins.op != Op::Dp4) || ins.src.size() < 2) return;
        for (std::size_t c = 0; c < opt_.dotCaptures.size(); ++c) {
            if (capturesDone_.contains(c)) continue;
            const auto& cap = opt_.dotCaptures[c];
            for (int k = 0; k < 2; ++k) {
                const SrcParam& a = ins.src[static_cast<std::size_t>(k)];
                if (a.reg.type != RegType::Const || a.rel.present || a.reg.index != cap.reg) continue;
                line(std::format("{} = {};", cap.target, srcExpr(ins.src[static_cast<std::size_t>(1 - k)])));
                capturesDone_.insert(c);
                break;
            }
        }
    }

    void emitInstruction(const Instruction& ins, int& loopId) {
        const auto& s = ins.src;
        auto need = [&](std::size_t n) {
            if (s.size() < n) {
                error_ = std::format("{} at token {}: expected {} source operands", opName(ins.op), ins.tokenOffset, n);
                return false;
            }
            return true;
        };
        switch (ins.op) {
        case Op::Nop: break;
        case Op::Mov:
            if (need(1)) assign(ins, srcExpr(s[0]));
            break;
        case Op::Mova: {
            if (!need(1)) break;
            const std::string m = maskSuffix(ins.dst.writeMask);
            line(std::format("a0.{} = int4(floor({} + 0.5)).{};", m, srcExpr(s[0]), m));
            break;
        }
        case Op::Add:
            if (need(2)) assign(ins, std::format("{} + {}", srcExpr(s[0]), srcExpr(s[1])));
            break;
        case Op::Sub:
            if (need(2)) assign(ins, std::format("{} - {}", srcExpr(s[0]), srcExpr(s[1])));
            break;
        case Op::Mul:
            if (need(2)) assign(ins, std::format("{} * {}", paren(srcExpr(s[0])), paren(srcExpr(s[1]))));
            break;
        case Op::Mad:
            if (need(3)) assign(ins, std::format("{} * {} + {}", paren(srcExpr(s[0])), paren(srcExpr(s[1])), srcExpr(s[2])));
            break;
        case Op::Rcp:
            // clamped to FLT_MAX (DXVK d3d9.floatEmulation): D3D9's 0 * rcp(0) is 0 (legacy multiply), IEEE 0 * inf is NaN
            if (need(1)) assign(ins, splat(std::format("min(1.0 / {}, 3.402823466e+38)", scalar(s[0]))));
            break;
        case Op::Rsq:
            if (need(1)) assign(ins, splat(std::format("min(rsqrt(abs({})), 3.402823466e+38)", scalar(s[0]))));
            break;
        case Op::Dp3:
            if (need(2)) assign(ins, splat(std::format("dot({}.xyz, {}.xyz)", paren(srcExpr(s[0])), paren(srcExpr(s[1])))));
            break;
        case Op::Dp4:
            if (need(2)) assign(ins, splat(std::format("dot({}, {})", srcExpr(s[0]), srcExpr(s[1]))));
            break;
        case Op::Min:
            if (need(2)) assign(ins, std::format("min({}, {})", srcExpr(s[0]), srcExpr(s[1])));
            break;
        case Op::Max:
            if (need(2)) assign(ins, std::format("max({}, {})", srcExpr(s[0]), srcExpr(s[1])));
            break;
        case Op::Slt:
            if (need(2)) assign(ins, std::format("(float4)({} < {})", srcExpr(s[0]), srcExpr(s[1])));
            break;
        case Op::Sge:
            if (need(2)) assign(ins, std::format("(float4)({} >= {})", srcExpr(s[0]), srcExpr(s[1])));
            break;
        case Op::Exp:
        case Op::ExpP:
            if (need(1)) assign(ins, splat(std::format("min(exp2({}), 3.402823466e+38)", scalar(s[0]))));
            break;
        case Op::Log:
        case Op::LogP:
            // log(0) = -FLT_MAX, not -inf: FXC's expanded pow (log, mul, exp) of pow(0, 0) is exp(0 * -FLT_MAX) = 1, as on D3D9
            if (need(1)) assign(ins, splat(std::format("max(log2(abs({})), -3.402823466e+38)", scalar(s[0]))));
            break;
        case Op::Lit:
            if (need(1)) assign(ins, std::format("rvk_lit({})", srcExpr(s[0])));
            break;
        case Op::Dst:
            if (need(2)) {
                const std::string a = paren(srcExpr(s[0]));
                const std::string b = paren(srcExpr(s[1]));
                assign(ins, std::format("float4(1.0, {}.y * {}.y, {}.z, {}.w)", a, b, a, b));
            }
            break;
        case Op::Lrp:
            if (need(3)) assign(ins, std::format("lerp({}, {}, {})", srcExpr(s[2]), srcExpr(s[1]), srcExpr(s[0])));
            break;
        case Op::Frc:
            if (need(1)) assign(ins, std::format("frac({})", srcExpr(s[0])));
            break;
        case Op::M4x4:
        case Op::M4x3:
        case Op::M3x4:
        case Op::M3x3:
        case Op::M3x2: {
            if (!need(2)) break;
            const int rows = matrixRows(ins.op);
            const bool dot3 = ins.op == Op::M3x4 || ins.op == Op::M3x3 || ins.op == Op::M3x2;
            const std::string a = paren(srcExpr(s[0]));
            std::string v = "float4(";
            for (int k = 0; k < 4; ++k) {
                if (k > 0) v += ", ";
                if (k < rows) {
                    const std::string row = paren(srcExpr(s[1], static_cast<std::uint32_t>(k)));
                    v += dot3 ? std::format("dot({}.xyz, {}.xyz)", a, row) : std::format("dot({}, {})", a, row);
                } else {
                    v += "0.0";
                }
            }
            assign(ins, v + ")");
            break;
        }
        case Op::Pow:
            if (need(2)) assign(ins, splat(std::format("rvk_pow(abs({}), {})", scalar(s[0]), scalar(s[1]))));
            break;
        case Op::Crs:
            if (need(2)) assign(ins, std::format("float4(cross({}.xyz, {}.xyz), 0.0)", paren(srcExpr(s[0])), paren(srcExpr(s[1]))));
            break;
        case Op::Sgn:
            if (need(1)) assign(ins, std::format("(float4)sign({})", srcExpr(s[0])));
            break;
        case Op::Abs:
            if (need(1)) assign(ins, std::format("abs({})", srcExpr(s[0])));
            break;
        case Op::Nrm:
            if (need(1)) assign(ins, std::format("rvk_nrm({})", srcExpr(s[0])));
            break;
        case Op::SinCos:
            if (need(1)) {
                const std::string x = scalar(s[0]);
                assign(ins, std::format("float4(cos({}), sin({}), 0.0, 0.0)", x, x));
            }
            break;
        case Op::Cmp:
            if (need(3)) assign(ins, std::format("select({} >= 0.0, {}, {})", srcExpr(s[0]), srcExpr(s[1]), srcExpr(s[2])));
            break;
        case Op::Cnd:
            if (need(3)) assign(ins, std::format("select({} > 0.5, {}, {})", srcExpr(s[0]), srcExpr(s[1]), srcExpr(s[2])));
            break;
        case Op::Dp2Add:
            if (need(3)) {
                assign(ins, splat(std::format("dot({}.xy, {}.xy) + {}", paren(srcExpr(s[0])), paren(srcExpr(s[1])), scalar(s[2]))));
            }
            break;
        case Op::Dsx:
            if (need(1)) assign(ins, std::format("ddx({})", srcExpr(s[0])));
            break;
        case Op::Dsy:
            if (need(1)) assign(ins, std::format("ddy({})", srcExpr(s[0])));
            break;
        case Op::Tex:
        case Op::TexLdl:
        case Op::TexLdd: emitTex(ins); break;
        case Op::TexKill: {
            // rvk_kill: x < 0, or the hashed cut-out of jittered draws (header); every component is evaluated (its fwidth)
            const std::string r = paren(regExpr(ins.dst.reg, ins.dst.rel));
            std::string cond;
            int n = 0;
            for (int c = 0; c < 4; ++c) {
                if ((ins.dst.writeMask & (1 << c)) != 0) {
                    cond += std::format("{}rvk_kill({}.{})", cond.empty() ? "" : ", ", r, kComp[c]);
                    ++n;
                }
            }
            if (n == 1) {
                line(std::format("if ({}) discard;", cond));
            } else if (n > 1) {
                line(std::format("if (any(bool{}({}))) discard;", n, cond));
            }
            break;
        }
        case Op::Setp: {
            if (!need(2)) break;
            const std::string m = maskSuffix(ins.dst.writeMask);
            line(std::format("p0.{} = ({} {} {}).{};", m, srcExpr(s[0]), cmpOp(ins.comparison()), srcExpr(s[1]), m));
            break;
        }
        case Op::If:
            if (need(1)) {
                line(std::format("if ({}) {{", boolSrc(s[0])));
                ++indent_;
                ++depth_;
            }
            break;
        case Op::Ifc:
            if (need(2)) {
                line(std::format("if ({} {} {}) {{", scalar(s[0]), cmpOp(ins.comparison()), scalar(s[1])));
                ++indent_;
                ++depth_;
            }
            break;
        case Op::Else:
            --indent_;
            line("} else {");
            ++indent_;
            break;
        case Op::EndIf:
        case Op::EndLoop:
        case Op::EndRep: {
            --indent_;
            --depth_;
            line("}");
            if (ins.op == Op::EndLoop && !loopSaves_.empty()) {
                line(std::format("aL = {};", loopSaves_.back()));
                loopSaves_.pop_back();
            }
            break;
        }
        case Op::Rep:
            if (need(1)) {
                const int id = loopId++;
                line(std::format("for (int rvk_k{} = 0; rvk_k{} < min({}.x, 255); ++rvk_k{}) {{", id, id, intExpr(s[0].reg.index), id));
                ++indent_;
                ++depth_;
            }
            break;
        case Op::Loop:
            if (need(2)) {
                const int id = loopId++;
                const std::string i = intExpr(s[1].reg.index);
                const std::string save = std::format("rvk_aL{}", id);
                line(std::format("int {} = aL;", save));
                loopSaves_.push_back(save);
                line(std::format("aL = {}.y;", paren(i)));
                line(std::format("for (int rvk_k{} = 0; rvk_k{} < min({}.x, 255); ++rvk_k{}, aL += {}.z) {{", id, id, paren(i), id,
                                 paren(i)));
                ++indent_;
                ++depth_;
            }
            break;
        case Op::Break: line("break;"); break;
        case Op::Breakc:
            if (need(2)) line(std::format("if ({} {} {}) break;", scalar(s[0]), cmpOp(ins.comparison()), scalar(s[1])));
            break;
        case Op::Breakp:
            if (need(1)) line(std::format("if ({}) break;", boolSrc(s[0])));
            break;
        case Op::Call:
            if (need(1)) line(std::format("l{}();", s[0].reg.index));
            break;
        case Op::CallNz:
            if (need(2)) line(std::format("if ({}) l{}();", boolSrc(s[1]), s[0].reg.index));
            break;
        default:
            error_ = std::format("instruction {} (opcode {}) is not supported", opName(ins.op), static_cast<int>(ins.op));
            break;
        }
    }

    // the scene variant (abi.h kSceneVaryingLocation): the record index travels in a varying no game shader may use, and the stage's full constant
    // block does not exist (the push block has no vs / ps pair)
    void checkScene() {
        constexpr std::uint32_t kLoc = abi::kSceneVaryingLocation;
        if (readsIntBool_) {
            error_ = "scene variant: the shader reads i# / b# constants (no full stage block in a scene draw)";
            return;
        }
        if (vs_) {
            for (const auto& d : p_.outputs) {
                if (abi::varyingLocation(d.usage, d.usageIndex) == kLoc) error_ = std::format("scene variant: the shader writes varying location {}", kLoc);
            }
            for (auto i : outputs_) {
                if (major_ < 3 && abi::varyingLocation(Usage::TexCoord, i) == kLoc) error_ = std::format("scene variant: the shader writes varying location {}", kLoc);
            }
        } else {
            for (const auto& d : p_.inputs) {
                if (psInputLocation(d) == kLoc) error_ = std::format("scene variant: the shader reads varying location {}", kLoc);
            }
        }
    }

    // ---- assembly -----------------------------------------------------------------------------------------------------

    std::string header() const {
        std::string h = "// translated from ";
        h += vs_ ? "vs" : "ps";
        h += std::format("_{}_{} by risen-enhanced-graphics\n\n", major_, p_.version.minor);
        // the ABI declarations (push constants, the pointers, the bindless heap): shaders/include/rvk_draw.hlsli
        if (scene()) h += "#define RVK_SCENE_RECORD 1\n";  // (the push block becomes a ScenePush, the draw's data comes from a DrawRecord)
        if (opt_.sceneView) h += "#define RVK_SCENE_VIEW 1\n";    // (ABI v2: the view's windows / posFix / c5.xyz come from the view table)
        h += opt_.defines;
        h += "#include \"rvk_draw.hlsli\"\n\n";
        if (opt_.pullVertices) h += "#include \"rvk_pull.hlsli\"\n\n";  // (S-4: after the ABI header; uberTranslatedPart keeps it inside rvkTr's text)
        return h;
    }

    const TranslateOptions::InputOverride* inputOverride(const Declaration& d) const {
        for (const auto& o : opt_.inputOverrides) {
            if (o.usage == d.usage && o.usageIndex == d.usageIndex) return &o;
        }
        return nullptr;
    }

    // the stage's input / output structs
    std::string structs() const {
        std::string g;
        if (vs_) {
            g += "struct RvkVsIn {\n";
            for (const auto& d : p_.inputs) {
                if (d.reg.type == RegType::Input) {
                    g += std::format("    [[vk::location({})]] float4 v{} : ATTR{};\n", d.reg.index, d.reg.index, d.reg.index);
                }
            }
            g += "    uint rvk_vertexId : SV_VertexID;\n    uint rvk_instanceId : SV_InstanceID;\n";
            if (scene()) g += std::format("    [[vk::location({})]] uint2 rvk_sceneRef : ATTR{};\n", abi::kSceneRefLocation, abi::kSceneRefLocation);
            g += opt_.extraInputs;
            if (!opt_.extraInputs.empty()) g += "\n";
            g += "};\n";
            g += "struct RvkVsOut {\n    float4 position : SV_Position;\n";
            std::set<std::uint32_t> locs;
            if (major_ >= 3) {
                for (const auto& d : p_.outputs) {
                    if (const std::uint32_t loc = abi::varyingLocation(d.usage, d.usageIndex); loc != abi::kInvalidLocation) locs.insert(loc);
                    if (d.usage == Usage::PSize && !varyingFillers()) g += "    [[vk::builtin(\"PointSize\")]] float rvk_psize : PSIZE;\n";
                }
            } else {
                for (auto i : attrOut_) locs.insert(abi::varyingLocation(Usage::Color, i));
                for (auto i : outputs_) locs.insert(abi::varyingLocation(Usage::TexCoord, i));
                if (rastOut_.contains(1)) locs.insert(abi::varyingLocation(Usage::Fog, 0));
                if (rastOut_.contains(2) && !varyingFillers()) g += "    [[vk::builtin(\"PointSize\")]] float rvk_psize : PSIZE;\n";
            }
            if (scene()) locs.insert(abi::kSceneVaryingLocation);
            // D3D12 links the stages by register: the signature is dense (0..max) on both sides, so a pixel shader that reads fewer / other varyings
            // than the vertex shader writes still lands on the same registers (zero-initialised fillers)
            if (varyingFillers() && (!locs.empty() || extraVaryLocation(opt_.extraOutputs) > 0)) {
                // (a variant's extra varying sits after them: it must land on the same register on both sides)
                const std::uint32_t top = std::max<std::int64_t>(locs.empty() ? -1 : static_cast<std::int64_t>(*locs.rbegin()), extraVaryLocation(opt_.extraOutputs) - 1);
                for (std::uint32_t l = 0; static_cast<std::int64_t>(l) <= static_cast<std::int64_t>(top); ++l) locs.insert(l);
            }
            bool extraDone = false;
            for (const std::uint32_t loc : locs) {
                if (varyingFillers() && static_cast<std::int64_t>(loc) == extraVaryLocation(opt_.extraOutputs)) {
                    g += opt_.extraOutputs + "\n";  // (in its register: a plain pixel shader pads this one)
                    extraDone = true;
                    continue;
                }
                if (scene() && loc == abi::kSceneVaryingLocation) {
                    g += std::format("    [[vk::location({})]] nointerpolation uint rvk_sceneIdx : VARY{};\n", loc, loc);
                    continue;
                }
                g += std::format("    [[vk::location({})]] float4 rvk_out{} : VARY{};\n", loc, loc, loc);
            }
            if (!extraDone) {
                g += opt_.extraOutputs;
                if (!opt_.extraOutputs.empty()) g += "\n";
            }
            g += "};\n";
        } else {
            g += "struct RvkPsIn {\n    float4 rvk_fragCoord : SV_Position;\n";
            std::map<std::uint32_t, std::string> lines;  // by location: the register order of a D3D12 signature is the declaration order
            for (const auto& d : p_.inputs) {
                const std::uint32_t loc = psInputLocation(d);
                if (loc == abi::kInvalidLocation) continue;
                const std::string name = d.reg.type == RegType::Addr ? std::format("t{}", d.reg.index) : std::format("v{}", d.reg.index);
                lines[loc] = std::format("    [[vk::location({})]] {}float4 {} : VARY{};\n", loc, d.centroid ? "centroid " : "", name, loc);
            }
            if (scene()) {
                lines[abi::kSceneVaryingLocation] = std::format("    [[vk::location({})]] nointerpolation uint rvk_sceneIdx : VARY{};\n", abi::kSceneVaryingLocation, abi::kSceneVaryingLocation);
            }
            if (varyingFillers() && (!lines.empty() || extraVaryLocation(opt_.extraInputs) > 0)) {
                const std::int64_t top = std::max<std::int64_t>(lines.empty() ? 0 : static_cast<std::int64_t>(lines.rbegin()->first), extraVaryLocation(opt_.extraInputs));
                for (std::int64_t l = 0; l < top; ++l) {
                    if (!lines.contains(l) && l != extraVaryLocation(opt_.extraInputs)) lines[l] = std::format("    [[vk::location({})]] float4 rvk_pad{} : VARY{};\n", l, l, l);
                }
            }
            // a variant's extra varying keeps its register: below the highest input it goes in between (the plain vertex shader's twin pads it)
            const std::int64_t exIn = varyingFillers() ? extraVaryLocation(opt_.extraInputs) : -1;
            bool extraInDone = false;
            if (exIn >= 0 && !lines.empty() && exIn < static_cast<std::int64_t>(lines.rbegin()->first)) {
                lines[static_cast<std::uint32_t>(exIn)] = opt_.extraInputs + "\n";
                extraInDone = true;
            }
            for (const auto& [loc, text] : lines) g += text;
            if (!extraInDone) {
                g += opt_.extraInputs;
                if (!opt_.extraInputs.empty()) g += "\n";
            }
            if (usesFace_) g += "    bool rvk_front : SV_IsFrontFace;\n";  // (a system value: after every register the VS also writes)
            g += "};\nstruct RvkPsOut {\n";
            for (std::uint32_t i = 0; i < 4; ++i) {
                if ((reflection_.colorOutputs & (1u << i)) != 0) g += std::format("    float4 oC{} : SV_Target{};\n", i, i);
            }
            if (reflection_.writesDepth) g += "    float rvk_depth : SV_Depth;\n";
            g += opt_.extraOutputs;
            if (!opt_.extraOutputs.empty()) g += "\n";
            g += "};\n";
        }
        return g + "\n";
    }

    std::string globals() const {
        std::string g = structs();
        g += "static RvkDrawData rvk_draw;\n";
        if (!vs_) {
            g += "static float4 rvk_fragCoord;\n";
            if (usesFace_) g += "static bool rvk_front;\n";
        }
        // inputs
        for (const auto& d : p_.inputs) {
            if (vs_) {
                if (d.reg.type == RegType::Input) g += std::format("static float4 v{};\n", d.reg.index);
                continue;
            }
            const std::uint32_t loc = psInputLocation(d);
            if (loc == abi::kInvalidLocation) continue;
            g += std::format("static float4 {};\n", d.reg.type == RegType::Addr ? std::format("t{}", d.reg.index) : std::format("v{}", d.reg.index));
        }
        // outputs
        if (vs_) {
            if (major_ >= 3) {
                std::set<std::uint32_t> regs(outputs_.begin(), outputs_.end());
                for (const auto& d : p_.outputs) regs.insert(d.reg.index);
                std::set<std::uint32_t> posRegs;
                for (const auto& d : p_.outputs) {
                    if (d.usage == Usage::Position) posRegs.insert(d.reg.index);
                }
                for (auto r : regs) g += std::format("static {}float4 o{} = float4(0.0, 0.0, 0.0, 0.0);\n", varyingFillers() && posRegs.contains(r) ? "precise " : "", r);
            } else {
                g += "static float4 oPos = float4(0.0, 0.0, 0.0, 0.0);\nstatic float4 oFog = float4(0.0, 0.0, 0.0, 0.0);\n"
                     "static float4 oPts = float4(0.0, 0.0, 0.0, 0.0);\n";
                for (auto i : attrOut_) g += std::format("static float4 oD{} = float4(0.0, 0.0, 0.0, 0.0);\n", i);
                for (auto i : outputs_) g += std::format("static float4 oT{} = float4(0.0, 0.0, 0.0, 0.0);\n", i);
            }
        } else {
            for (std::uint32_t i = 0; i < 4; ++i) {
                if ((reflection_.colorOutputs & (1u << i)) != 0) g += std::format("static float4 oC{} = float4(0.0, 0.0, 0.0, 0.0);\n", i);
            }
            if (reflection_.writesDepth) g += "static float4 oDepth = float4(0.0, 0.0, 0.0, 0.0);\n";
        }
        g += "static float4 rvk_dummy;\n";
        // registers
        for (auto t : temps_) {
            g += std::format("static float4 r{} = float4(0.0, 0.0, 0.0, 0.0);\n", t);
        }
        g += "static int4 a0 = int4(0, 0, 0, 0);\nstatic int aL = 0;\nstatic bool4 p0 = bool4(false, false, false, false);\n";
        for (const auto& [n, imm] : defF_) {
            bool isConstant = true;
            const std::string v = std::format("float4({}, {}, {}, {})", floatLiteral(imm[0], isConstant), floatLiteral(imm[1], isConstant),
                                              floatLiteral(imm[2], isConstant), floatLiteral(imm[3], isConstant));
            g += std::format("static {}float4 c{}_def = {};\n", isConstant ? "const " : "", n, v);
        }
        // helpers
        {
            const std::string rel = vs_ ? "RVK_VSC(i)" : "RVK_PSC(i)";
            if (opt_.constOverrides.empty()) {
                g += std::format("\nfloat4 rvk_cRel(int i) {{ return uint(i) < {}u ? {} : float4(0.0, 0.0, 0.0, 0.0); }}\n", regLimit(), rel);
            } else {
                g += "\nfloat4 rvk_cRel(int i) {\n    switch (i) {\n";
                for (const auto& [n, expr] : opt_.constOverrides) {
                    if (n < regLimit()) {
                        g += std::format("    case {}: return {};\n", n, overrideExpr(expr, constRead(n)));
                    }
                }
                g += std::format("    default: return uint(i) < {}u ? {} : float4(0.0, 0.0, 0.0, 0.0);\n    }}\n}}\n", regLimit(), rel);
            }
        }
        // D3D9's multiplication is "legacy" (0 * anything = 0, also 0 * inf), so pow(0, 0) = exp2(0 * log2(0)) = 1 there (and on AMD's Vulkan compiler);
        // the D3D12 path's IEEE exp2(0 * -inf) is NaN, which poisoned the scene colour (refracted crystals) and the engine's eye adaptation. SPIR-V's
        // Pow(0, 0) is undefined too (NVIDIA / Intel may give NaN): both backends use it (DXVK selects 1 for a zero exponent the same way)
        g += "float rvk_pow(float x, float y) { return y == 0.0 ? 1.0 : pow(x, y); }\n";
        g += "float4 rvk_lit(float4 s) {\n"
             "    float y = max(s.x, 0.0);\n"
             "    float z = (s.x > 0.0 && s.y > 0.0) ? pow(s.y, clamp(s.w, -128.0, 128.0)) : 0.0;\n"
             "    return float4(1.0, y, z, 1.0);\n}\n";
        g += "float4 rvk_nrm(float4 s) {\n    float l = dot(s.xyz, s.xyz);\n    return l > 0.0 ? s * rsqrt(l) : float4(0.0, 0.0, 0.0, 0.0);\n}\n";
        if (!vs_) {
            // the cut-outs of jittered draws (abi.h alphaFunc flags; docs/research/upscaling.md 17 / 19). The engine's materials
            // cut out in the z pre-pass (texkill of alpha - c5.w; the colour pass then matches its depth), a few draws with the
            // render-state alpha test (GREATER / GREATEREQUAL). kAlphaThicken: where the value changes fast across pixels
            // (strands finer than a render pixel) the threshold drops by strength x min(fwidth, 1), so they stay about a pixel
            // wide; kAlphaHashed: the coverage around the threshold against per-pixel noise that changes every frame
            g += "float rvk_alphaNoise() {\n"
                 "    float2 p = rvk_fragCoord.xy + 5.588238 * float((rvk_draw.alpha.x >> 16u) & 63u);\n"
                 "    return frac(52.9829189 * frac(dot(p, float2(0.06711056, 0.00583715))));\n}\n"
                 "bool rvk_kill(float x) {\n"
                 "    float w = max(fwidth(x), 1.0 / 255.0);\n"
                 "    uint f = rvk_draw.alpha.x;\n"
                 "    if ((f & 0x400u) != 0u) x += float(f >> 24u) * (1.0 / 255.0) * min(w, 1.0);\n"
                 "    if ((f & 0x100u) == 0u) return x < 0.0;\n"
                 "    return clamp(x / w + 0.5, 0.0, 1.0) <= rvk_alphaNoise();\n}\n"
                 "bool rvk_alphaTest(float a) {\n"
                 "    float q = round(clamp(a, 0.0, 1.0) * 255.0);\n"
                 "    float r = asfloat(rvk_draw.alpha.y);\n"
                 "    uint f = rvk_draw.alpha.x & 0xffu;\n"
                 "    if ((f == 5u || f == 7u) && (rvk_draw.alpha.x & 0x500u) != 0u) {\n"
                 "        return !rvk_kill(a - (f == 5u ? r + 0.5 : r - 0.5) / 255.0);\n"
                 "    }\n"
                 "    switch (f) {\n"
                 "    case 1u: return false;\n    case 2u: return q < r;\n    case 3u: return q == r;\n"
                 "    case 4u: return q <= r;\n    case 5u: return q > r;\n    case 6u: return q != r;\n"
                 "    case 7u: return q >= r;\n    default: return true;\n    }\n}\n";
        }
        g += "\nvoid rvk_main();\n";
        for (auto l : labels_) {
            g += std::format("void l{}();\n", l);
        }
        g += "\n";
        return g;
    }

    std::string entryPoint() const {
        std::string m;
        if (vs_) {
            // (S-4: the pulled form keeps the same body under another name; its entry, after it, builds rvk_in from the fetch block)
            m += opt_.pullVertices ? "RvkVsOut rvkVsMain(RvkVsIn rvk_in) {\n" : "RvkVsOut VSMain(RvkVsIn rvk_in) {\n";
            if (scene()) m += "    rvkSceneBeginVs(rvk_in.rvk_sceneRef);\n";
            m += "    rvk_draw = rvkLoadDraw();\n    RvkVsOut rvk_out = (RvkVsOut)0;\n";
            if (scene()) m += "    rvk_out.rvk_sceneIdx = rvk_sceneIndex;\n";
            for (const auto& d : p_.inputs) {
                if (d.reg.type != RegType::Input) continue;
                if (const auto* o = inputOverride(d)) {
                    m += std::format("    v{} = {};\n", d.reg.index, overrideExpr(o->expr, std::format("rvk_in.v{}", d.reg.index), "$v"));
                } else {
                    if (varyingFillers() && d.reg.index < 15) {
                        // D3D12: a D3D9 UBYTE4 input is read as R8G8B8A8_UNORM (no scaled formats in DXGI) and scaled back by 255: draws flag such inputs in their push flags
                        m += std::format("    v{} = (({} >> {}u) & 1u) != 0u ? round(rvk_in.v{} * 255.0) : rvk_in.v{};\n", d.reg.index, scene() ? "rvkDrawFlags()" : "pc.flags", 17 + d.reg.index, d.reg.index, d.reg.index);
                    } else {
                        m += std::format("    v{} = rvk_in.v{};\n", d.reg.index, d.reg.index);
                    }
                }
            }
        } else {
            m += "RvkPsOut PSMain(RvkPsIn rvk_in) {\n";
            if (scene()) m += "    rvkSceneBeginPs(rvk_in.rvk_sceneIdx);\n";
            m += "    rvk_draw = rvkLoadDraw();\n    RvkPsOut rvk_out = (RvkPsOut)0;\n";
            m += "    rvk_fragCoord = rvk_in.rvk_fragCoord;\n";
            if (usesFace_) m += "    rvk_front = rvk_in.rvk_front;\n";
            for (const auto& d : p_.inputs) {
                const std::uint32_t loc = psInputLocation(d);
                if (loc == abi::kInvalidLocation) continue;
                const std::string name = d.reg.type == RegType::Addr ? std::format("t{}", d.reg.index) : std::format("v{}", d.reg.index);
                m += std::format("    {} = rvk_in.{};\n", name, name);
            }
        }
        if (!opt_.prologue.empty()) {
            m += opt_.prologue;
            m += "\n";
        }
        m += "    rvk_main();\n";
        if (vs_) {
            std::string pos = "oPos";
            if (major_ >= 3) {
                pos = "float4(0.0, 0.0, 0.0, 0.0)";
                for (const auto& d : p_.outputs) {
                    if (d.usage == Usage::Position && d.usageIndex == 0) {
                        pos = std::format("o{}", d.reg.index);
                    } else if (d.usage == Usage::PSize) {
                        if (!varyingFillers()) m += std::format("    rvk_out.rvk_psize = o{}.x;\n", d.reg.index);
                    } else if (const std::uint32_t loc = abi::varyingLocation(d.usage, d.usageIndex); loc != abi::kInvalidLocation) {
                        m += std::format("    rvk_out.rvk_out{} = o{};\n", loc, d.reg.index);
                    }
                }
            } else {
                for (auto i : attrOut_) {
                    m += std::format("    rvk_out.rvk_out{} = oD{};\n", abi::varyingLocation(Usage::Color, i), i);
                }
                for (auto i : outputs_) {
                    m += std::format("    rvk_out.rvk_out{} = oT{};\n", abi::varyingLocation(Usage::TexCoord, i), i);
                }
                if (rastOut_.contains(1)) {
                    m += std::format("    rvk_out.rvk_out{} = (float4)oFog.x;\n", abi::varyingLocation(Usage::Fog, 0));
                }
                if (rastOut_.contains(2)) {
                    if (!varyingFillers()) m += "    rvk_out.rvk_psize = oPts.x;\n";
                }
            }
            // the depth pre-pass and the solid pass compare positions of two different shaders for equality (ZFUNC_EQUAL): `precise` keeps the
            // driver from fusing the position's multiply-adds differently in each. Both backends: AMD's Vulkan compiler happens to give equal
            // results without it, other drivers need not (DXVK decorates the position Invariant for the same reason)
            m += std::format("    precise float4 rvk_pos = {};\n", pos);
            if (!opt_.epilogue.empty()) {
                std::string ep = opt_.epilogue;  // (`$pos`: the position output register, TranslateOptions::epilogue)
                for (std::size_t at = ep.find("$pos"); at != std::string::npos; at = ep.find("$pos", at + pos.size())) ep.replace(at, 4, pos);
                m += ep;
                m += "\n";
            }
            // the half-pixel fix, then the reversed depth remap (abi.h posFix: xy fix, zw z' = z (1 + zw.x) + zw.y w; 0 = none)
            m += "    rvk_pos.xy += rvk_draw.posFix.xy * rvk_pos.w;\n";
            m += "    rvk_pos.z += rvk_draw.posFix.z * rvk_pos.z + rvk_draw.posFix.w * rvk_pos.w;\n    rvk_out.position = rvk_pos;\n";
        } else {
            if ((reflection_.colorOutputs & 1u) != 0) {
                m += "    if (!rvk_alphaTest(oC0.a)) discard;\n";
                if (opt_.tintEpilogue) {
                    // debug view "tint draws" (abi.h DrawBlock pad[0], 0 = off): oC0 towards the tint, keeping its brightness
                    // (a floor lifts dark surfaces; black stays black, so additive / premultiplied draws keep their shape)
                    m += "    if (rvk_draw.alpha.z != 0u) {\n"
                         "        float4 rvk_tint = rvkUnpackUnorm4x8(rvk_draw.alpha.z);\n"
                         "        float rvk_l = dot(max(oC0.rgb, float3(0.0, 0.0, 0.0)), float3(0.299, 0.587, 0.114));\n"
                         "        float rvk_k = 0.25 * clamp(rvk_l * 16.0, 0.0, 1.0) + 0.75 * clamp(rvk_l * 1.5, 0.0, 1.0);\n"
                         "        oC0.rgb = lerp(oC0.rgb, rvk_tint.rgb * rvk_k, rvk_tint.a);\n"
                         "    }\n";
                }
            }
            if (!opt_.epilogue.empty()) {
                m += opt_.epilogue;
                m += "\n";
            }
            for (std::uint32_t i = 0; i < 4; ++i) {
                if ((reflection_.colorOutputs & (1u << i)) != 0) {
                    m += std::format("    rvk_out.oC{} = oC{};\n", i, i);
                }
            }
            if (reflection_.writesDepth) {
                m += "    rvk_out.rvk_depth = clamp(oDepth.x, 0.0, 1.0) * (1.0 + rvk_draw.posFix.z) + rvk_draw.posFix.w;\n";
            }
        }
        m += "    return rvk_out;\n}\n";
        if (vs_ && opt_.pullVertices) m += pullEntry();
        return m;
    }

    // S-4: the pulled entry (rvk_pull.hlsli): SV_VertexID + the ref stream in, every declared input read from the draw's VertexFetch block in the
    // input assembler's conversion, then the translated program unchanged
    std::string pullEntry() const {
        std::string m = "\nstruct RvkVsPullIn {\n    uint rvk_vertexId : SV_VertexID;\n    uint rvk_instanceId : SV_InstanceID;\n";
        m += std::format("    [[vk::location({})]] uint2 rvk_sceneRef : ATTR{};\n}};\n", abi::kSceneRefLocation, abi::kSceneRefLocation);
        m += "RvkVsIn rvkPullInputs(RvkVsPullIn p) {\n    RvkVsIn r = (RvkVsIn)0;\n    r.rvk_vertexId = p.rvk_vertexId;\n    r.rvk_instanceId = p.rvk_instanceId;\n"
             "    r.rvk_sceneRef = p.rvk_sceneRef;\n    const RvkPullCtx ctx = rvkPullBegin(rvkPullBlock(p.rvk_sceneRef), p.rvk_vertexId);\n";
        for (const auto& d : p_.inputs) {
            if (d.reg.type == RegType::Input) m += std::format("    r.v{} = rvkPullAttr(ctx, {}u);\n", d.reg.index, d.reg.index);
        }
        m += "    return r;\n}\nRvkVsOut VSMain(RvkVsPullIn rvk_pin) { return rvkVsMain(rvkPullInputs(rvk_pin)); }\n";
        return m;
    }

    const Program& p_;
    const TranslateOptions opt_;
    const bool vs_;
    const int major_;
    const std::string consts_;
    mutable bool readsIntBool_ = false;  // intExpr / boolConst emitted a stage-block read (Reflection::intBoolConstants)
    bool usesFace_ = false;              // the shader reads vFace (misc register 1)
    Reflection reflection_;
    std::string error_;

    std::set<std::uint32_t> temps_, rastOut_, attrOut_, outputs_, labels_;
    std::set<std::size_t> capturesDone_;
    std::set<std::size_t> texCapturesDone_;
    std::map<std::uint32_t, std::array<std::uint32_t, 4>> defF_, defI_;
    std::map<std::uint32_t, bool> defB_;

    std::string functions_;
    std::string body_;
    std::string currentFunction_;
    bool functionOpen_ = false;
    int indent_ = 1;
    int depth_ = 0;
    std::vector<std::string> loopSaves_;
};

}  // namespace

HlslResult toHlsl(const Program& program, const TranslateOptions& options) { return Generator(program, options).run(); }

std::uint64_t hlslIncludesHash() {
    std::uint64_t h = 1469598103934665603ull;
    for (const char* name : {"include/rvk_draw.hlsli", "include/rvk_abi.hlsli", "include/rvk_common.hlsli", "include/rvk_oct.hlsli"}) {
        for (const char c : embedded::hlslSource(name)) {
            h ^= static_cast<std::uint8_t>(c);
            h *= 1099511628211ull;
        }
    }
    return h;
}

std::uint32_t hlslPullHash() {
    std::uint64_t h = 1469598103934665603ull;
    for (const char c : embedded::hlslSource("include/rvk_pull.hlsli")) {
        h ^= static_cast<std::uint8_t>(c);
        h *= 1099511628211ull;
    }
    return static_cast<std::uint32_t>(h ^ (h >> 32));
}

}  // namespace rvk::shader
