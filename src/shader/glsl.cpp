// D3D9 SM 2.0/3.0 bytecode -> GLSL 4.60 (Vulkan) per the contract in shader/abi.h.
//
// Registers become module-scope variables (r#, a0, aL, p0, outputs) so D3D9 subroutines (label/call/ret) map to parameterless
// GLSL functions. The epilogue in main() applies the D3D9 -> Vulkan fix-ups (half-pixel offset, alpha test) and copies the
// output registers to the real outputs. Semantics follow the D3D9 reference (and DXVK's dxso where the reference is vague):
// scalar instructions read component .x of the swizzled source; rsq/log/pow use |x|; texkill tests the masked components.

#include "shader/glsl.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <format>
#include <map>
#include <set>

#include "shader/embedded.h"

namespace rvk::shader {
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

// Exact GLSL literal of a float given by its bits (shortest round-trip form); inf/NaN have no literal.
std::string floatLiteral(std::uint32_t bits, bool& isConstant) {
    float f;
    std::memcpy(&f, &bits, sizeof(f));
    if (!std::isfinite(f)) {
        isConstant = false;
        return std::format("uintBitsToFloat(0x{:08x}u)", bits);
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

const char* cmpFunc(Cmp c) {
    switch (c) {
    case Cmp::Gt: return "greaterThan";
    case Cmp::Eq: return "equal";
    case Cmp::Ge: return "greaterThanEqual";
    case Cmp::Lt: return "lessThan";
    case Cmp::Ne: return "notEqual";
    case Cmp::Le: return "lessThanEqual";
    default: return "equal";
    }
}

class Generator {
public:
    Generator(const Program& p, const GlslOptions& o)
        : p_(p), opt_(o), vs_(p.version.stage == Stage::Vertex), major_(p.version.major), consts_(vs_ ? "pc.vs" : "pc.ps") {}

    GlslResult run() {
        GlslResult res;
        res.reflection.version = p_.version;
        scan();
        if (!error_.empty()) {
            res.error = error_;
            return res;
        }
        emitBody();
        if (!error_.empty()) {
            res.error = error_;
            return res;
        }
        res.glsl = header() + globals() + (opt_.extraGlobals.empty() ? std::string() : opt_.extraGlobals + "\n") + functions_ +
                   mainFunction();
        res.reflection = reflection_;
        res.reflection.version = p_.version;
        return res;
    }

private:
    // ---- pass 1: what the shader uses -------------------------------------------------------------------------------

    void noteRead(const Register& r, const RelAddr& rel) {
        if (r.type == RegType::Const) {
            if (rel.present) {
                reflection_.relativeConstants = true;
                reflection_.floatConstCount = abi::kFloatConsts;
            } else {
                reflection_.floatConstCount = std::max(reflection_.floatConstCount, r.index + 1);
            }
        } else if (r.type == RegType::Temp || r.type == RegType::TempFloat16) {
            temps_.insert(r.index);
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
            // matrix ops read consecutive registers
            const int rows = matrixRows(ins.op);
            if (rows > 0 && ins.src.size() >= 2 && ins.src[1].reg.type == RegType::Const && !ins.src[1].rel.present) {
                reflection_.floatConstCount = std::max(reflection_.floatConstCount, ins.src[1].reg.index + rows);
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
        // inputs
        for (const auto& d : p_.inputs) {
            if (vs_) {
                if (d.reg.type == RegType::Input) {
                    reflection_.vertexInputs.push_back({d.reg.index, d.usage, d.usageIndex});
                }
            } else if (const std::uint32_t loc = psInputLocation(d); loc != abi::kInvalidLocation) {
                reflection_.varyingMask |= 1u << loc;
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
                return overrideExpr(it->second, std::format("{}.c[{}]", consts_, n));
            }
            return std::format("{}.c[{}]", consts_, n);
        case RegType::Addr: return vs_ ? "vec4(a0)" : std::format("t{}", n);
        case RegType::RastOut: return n == 0 ? "oPos" : n == 1 ? "oFog" : "oPts";
        case RegType::AttrOut: return std::format("oD{}", n);
        case RegType::Output: return major_ >= 3 ? std::format("o{}", n) : std::format("oT{}", n);
        case RegType::ConstInt: return std::format("vec4({})", intExpr(n));
        case RegType::ConstBool: return std::format("vec4({} ? 1.0 : 0.0)", boolConst(n));
        case RegType::ColorOut: return std::format("oC{}", n);
        case RegType::DepthOut: return "oDepth";
        case RegType::Loop: return "vec4(float(aL))";
        case RegType::MiscType:
            return n == 0 ? "vec4(gl_FragCoord.xy - vec2(0.5), 0.0, 0.0)" : "vec4(gl_FrontFacing ? 1.0 : -1.0)";
        case RegType::Predicate: return "vec4(p0)";
        default:
            error_ = std::format("unsupported source register type {}", static_cast<int>(r.type));
            return "vec4(0.0)";
        }
    }

    // GlslOptions::constOverrides: the expression with `$c` replaced by the original read
    static std::string overrideExpr(const std::string& expr, const std::string& original) {
        std::string out = "(";
        for (std::size_t i = 0; i < expr.size(); ++i) {
            if (expr[i] == '$' && i + 1 < expr.size() && expr[i + 1] == 'c') {
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
            return std::format("ivec4({}, {}, {}, {})", static_cast<std::int32_t>(it->second[0]),
                               static_cast<std::int32_t>(it->second[1]), static_cast<std::int32_t>(it->second[2]),
                               static_cast<std::int32_t>(it->second[3]));
        }
        return std::format("{}.i[{}]", consts_, n);
    }

    std::string boolConst(std::uint32_t n) const {
        if (auto it = defB_.find(n); it != defB_.end()) {
            return it->second ? "true" : "false";
        }
        return std::format("({}.b[{}] != 0u)", consts_, n);
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
        case SrcMod::Bias: return std::format("({} - vec4(0.5))", v);
        case SrcMod::BiasNeg: return std::format("(vec4(0.5) - {})", v);
        case SrcMod::Sign: return std::format("({} * 2.0 - vec4(1.0))", v);
        case SrcMod::SignNeg: return std::format("(vec4(1.0) - {} * 2.0)", v);
        case SrcMod::Comp: return std::format("(vec4(1.0) - {})", v);
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
            value = std::format("clamp({}, vec4(0.0), vec4(1.0))", value);
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
        line(std::format("vec4 rvk_t = {};", value));
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

    std::string samplerExpr(std::uint32_t index) {
        const std::uint32_t slot = vs_ ? abi::kVertexSamplerBase + index : index;
        if (slot >= abi::kSamplerSlots) {
            error_ = std::format("sampler s{} out of range", index);
            return "";
        }
        const TextureType t = reflection_.samplers[slot];
        const std::string tex = std::format("pc.draw.tex[{}]", slot);
        const std::string smp = std::format("samp[pc.draw.smp[{}]]", slot);
        switch (t) {
        case TextureType::Cube: return std::format("samplerCube(tCube[{}], {})", tex, smp);
        case TextureType::Volume: return std::format("sampler3D(t3D[{}], {})", tex, smp);
        default: return std::format("sampler2D(t2D[{}], {})", tex, smp);
        }
    }

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
        const std::string se = samplerExpr(s);
        const std::string c = srcExpr(ins.src[0]);
        std::string v;
        if (ins.op == Op::TexLdl) {
            v = std::format("textureLod({}, {}, {}.w)", se, coord(s, c), paren(c));
        } else if (ins.op == Op::TexLdd) {
            if (ins.src.size() < 4) {
                error_ = "texldd needs 4 operands";
                return;
            }
            v = std::format("textureGrad({}, {}, {}, {})", se, coord(s, c), coord(s, srcExpr(ins.src[2])), coord(s, srcExpr(ins.src[3])));
        } else if (vs_) {
            v = std::format("textureLod({}, {}, 0.0)", se, coord(s, c));
        } else if (ins.texProject()) {
            const std::uint32_t slot = s;
            v = reflection_.samplers[slot] == TextureType::Cube
                    ? std::format("texture({}, {}.xyz / {}.w)", se, paren(c), paren(c))
                    : std::format("textureProj({}, {})", se, c);
        } else if (ins.texBias()) {
            v = std::format("texture({}, {}, {}.w)", se, coord(s, c), paren(c));
        } else {
            v = std::format("texture({}, {})", se, coord(s, c));
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
            emitInstruction(ins, loopId);
        }
        closeFunction();
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
        auto vec = [&](const std::string& f) { return std::format("vec4({})", f); };
        switch (ins.op) {
        case Op::Nop: break;
        case Op::Mov:
            if (need(1)) assign(ins, srcExpr(s[0]));
            break;
        case Op::Mova: {
            if (!need(1)) break;
            const std::string m = maskSuffix(ins.dst.writeMask);
            line(std::format("a0.{} = ivec4(floor({} + vec4(0.5))).{};", m, srcExpr(s[0]), m));
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
            if (need(1)) assign(ins, vec("1.0 / " + scalar(s[0])));
            break;
        case Op::Rsq:
            if (need(1)) assign(ins, vec(std::format("inversesqrt(abs({}))", scalar(s[0]))));
            break;
        case Op::Dp3:
            if (need(2)) assign(ins, vec(std::format("dot({}.xyz, {}.xyz)", paren(srcExpr(s[0])), paren(srcExpr(s[1])))));
            break;
        case Op::Dp4:
            if (need(2)) assign(ins, vec(std::format("dot({}, {})", srcExpr(s[0]), srcExpr(s[1]))));
            break;
        case Op::Min:
            if (need(2)) assign(ins, std::format("min({}, {})", srcExpr(s[0]), srcExpr(s[1])));
            break;
        case Op::Max:
            if (need(2)) assign(ins, std::format("max({}, {})", srcExpr(s[0]), srcExpr(s[1])));
            break;
        case Op::Slt:
            if (need(2)) assign(ins, std::format("vec4(lessThan({}, {}))", srcExpr(s[0]), srcExpr(s[1])));
            break;
        case Op::Sge:
            if (need(2)) assign(ins, std::format("vec4(greaterThanEqual({}, {}))", srcExpr(s[0]), srcExpr(s[1])));
            break;
        case Op::Exp:
        case Op::ExpP:
            if (need(1)) assign(ins, vec("exp2(" + scalar(s[0]) + ")"));
            break;
        case Op::Log:
        case Op::LogP:
            if (need(1)) assign(ins, vec(std::format("log2(abs({}))", scalar(s[0]))));
            break;
        case Op::Lit:
            if (need(1)) assign(ins, std::format("rvk_lit({})", srcExpr(s[0])));
            break;
        case Op::Dst:
            if (need(2)) {
                const std::string a = paren(srcExpr(s[0]));
                const std::string b = paren(srcExpr(s[1]));
                assign(ins, std::format("vec4(1.0, {}.y * {}.y, {}.z, {}.w)", a, b, a, b));
            }
            break;
        case Op::Lrp:
            if (need(3)) assign(ins, std::format("mix({}, {}, {})", srcExpr(s[2]), srcExpr(s[1]), srcExpr(s[0])));
            break;
        case Op::Frc:
            if (need(1)) assign(ins, std::format("fract({})", srcExpr(s[0])));
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
            std::string v = "vec4(";
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
            if (need(2)) assign(ins, vec(std::format("pow(abs({}), {})", scalar(s[0]), scalar(s[1]))));
            break;
        case Op::Crs:
            if (need(2)) assign(ins, std::format("vec4(cross({}.xyz, {}.xyz), 0.0)", paren(srcExpr(s[0])), paren(srcExpr(s[1]))));
            break;
        case Op::Sgn:
            if (need(1)) assign(ins, std::format("sign({})", srcExpr(s[0])));
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
                assign(ins, std::format("vec4(cos({}), sin({}), 0.0, 0.0)", x, x));
            }
            break;
        case Op::Cmp:
            if (need(3)) {
                assign(ins, std::format("mix({}, {}, greaterThanEqual({}, vec4(0.0)))", srcExpr(s[2]), srcExpr(s[1]), srcExpr(s[0])));
            }
            break;
        case Op::Cnd:
            if (need(3)) {
                assign(ins, std::format("mix({}, {}, greaterThan({}, vec4(0.5)))", srcExpr(s[2]), srcExpr(s[1]), srcExpr(s[0])));
            }
            break;
        case Op::Dp2Add:
            if (need(3)) {
                assign(ins, vec(std::format("dot({}.xy, {}.xy) + {}", paren(srcExpr(s[0])), paren(srcExpr(s[1])), scalar(s[2]))));
            }
            break;
        case Op::Dsx:
            if (need(1)) assign(ins, std::format("dFdx({})", srcExpr(s[0])));
            break;
        case Op::Dsy:
            if (need(1)) assign(ins, std::format("dFdy({})", srcExpr(s[0])));
            break;
        case Op::Tex:
        case Op::TexLdl:
        case Op::TexLdd: emitTex(ins); break;
        case Op::TexKill: {
            const std::string r = paren(regExpr(ins.dst.reg, ins.dst.rel));
            std::string cond;
            for (int c = 0; c < 4; ++c) {
                if ((ins.dst.writeMask & (1 << c)) != 0) {
                    cond += std::format("{}{}.{} < 0.0", cond.empty() ? "" : " || ", r, kComp[c]);
                }
            }
            if (!cond.empty()) {
                line(std::format("if ({}) discard;", cond));
            }
            break;
        }
        case Op::Setp: {
            if (!need(2)) break;
            const std::string m = maskSuffix(ins.dst.writeMask);
            line(std::format("p0.{} = {}({}, {}).{};", m, cmpFunc(ins.comparison()), srcExpr(s[0]), srcExpr(s[1]), m));
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

    // ---- assembly -----------------------------------------------------------------------------------------------------

    std::string header() const {
        std::string h = "#version 460\n";
        h += "// translated from ";
        h += vs_ ? "vs" : "ps";
        h += std::format("_{}_{} by risen-enhanced-graphics\n\n", major_, p_.version.minor);
        // the ABI declarations (push constants, buffer references, the bindless set): shaders/include/rvk_abi.glsl
        h += embedded::abiGlsl();
        h += "\n";
        return h;
    }

    std::string globals() const {
        std::string g;
        // inputs
        for (const auto& d : p_.inputs) {
            if (vs_) {
                if (d.reg.type == RegType::Input) {
                    g += std::format("layout(location = {}) in vec4 v{};\n", d.reg.index, d.reg.index);
                }
                continue;
            }
            const std::uint32_t loc = psInputLocation(d);
            if (loc == abi::kInvalidLocation) {
                continue;
            }
            const std::string name = d.reg.type == RegType::Addr ? std::format("t{}", d.reg.index) : std::format("v{}", d.reg.index);
            g += std::format("layout(location = {}) {}in vec4 {};\n", loc, d.centroid ? "centroid " : "", name);
        }
        // outputs
        if (vs_) {
            if (major_ >= 3) {
                for (const auto& d : p_.outputs) {
                    if (const std::uint32_t loc = abi::varyingLocation(d.usage, d.usageIndex); loc != abi::kInvalidLocation) {
                        g += std::format("layout(location = {}) out vec4 rvk_out{};\n", loc, loc);
                    }
                }
                std::set<std::uint32_t> regs(outputs_.begin(), outputs_.end());
                for (const auto& d : p_.outputs) {
                    regs.insert(d.reg.index);
                }
                for (auto r : regs) {
                    g += std::format("vec4 o{} = vec4(0.0);\n", r);
                }
            } else {
                g += "vec4 oPos = vec4(0.0);\nvec4 oFog = vec4(0.0);\nvec4 oPts = vec4(0.0);\n";
                for (auto i : attrOut_) {
                    g += std::format("layout(location = {}) out vec4 rvk_out{};\nvec4 oD{} = vec4(0.0);\n",
                                     abi::varyingLocation(Usage::Color, i), abi::varyingLocation(Usage::Color, i), i);
                }
                for (auto i : outputs_) {
                    g += std::format("layout(location = {}) out vec4 rvk_out{};\nvec4 oT{} = vec4(0.0);\n",
                                     abi::varyingLocation(Usage::TexCoord, i), abi::varyingLocation(Usage::TexCoord, i), i);
                }
                if (rastOut_.contains(1)) {
                    g += std::format("layout(location = {}) out vec4 rvk_out{};\n", abi::varyingLocation(Usage::Fog, 0),
                                     abi::varyingLocation(Usage::Fog, 0));
                }
            }
        } else {
            for (std::uint32_t i = 0; i < 4; ++i) {
                if ((reflection_.colorOutputs & (1u << i)) != 0) {
                    g += std::format("layout(location = {}) out vec4 rvk_oC{};\nvec4 oC{} = vec4(0.0);\n", i, i, i);
                }
            }
            if (reflection_.writesDepth) {
                g += "vec4 oDepth = vec4(0.0);\n";
            }
        }
        // registers
        for (auto t : temps_) {
            g += std::format("vec4 r{} = vec4(0.0);\n", t);
        }
        g += "ivec4 a0 = ivec4(0);\nint aL = 0;\nbvec4 p0 = bvec4(false);\n";
        for (const auto& [n, imm] : defF_) {
            bool isConstant = true;
            const std::string v = std::format("vec4({}, {}, {}, {})", floatLiteral(imm[0], isConstant), floatLiteral(imm[1], isConstant),
                                              floatLiteral(imm[2], isConstant), floatLiteral(imm[3], isConstant));
            g += std::format("{}vec4 c{}_def = {};\n", isConstant ? "const " : "", n, v);
        }
        // helpers
        if (opt_.constOverrides.empty()) {
            g += std::format("\nvec4 rvk_cRel(int i) {{ return uint(i) < 256u ? {}.c[i] : vec4(0.0); }}\n", consts_);
        } else {
            g += "\nvec4 rvk_cRel(int i) {\n    switch (i) {\n";
            for (const auto& [n, expr] : opt_.constOverrides) {
                if (n < abi::kFloatConsts) {
                    g += std::format("    case {}: return {};\n", n, overrideExpr(expr, std::format("{}.c[{}]", consts_, n)));
                }
            }
            g += std::format("    default: return uint(i) < 256u ? {}.c[i] : vec4(0.0);\n    }}\n}}\n", consts_);
        }
        g += "vec4 rvk_lit(vec4 s) {\n"
             "    float y = max(s.x, 0.0);\n"
             "    float z = (s.x > 0.0 && s.y > 0.0) ? pow(s.y, clamp(s.w, -128.0, 128.0)) : 0.0;\n"
             "    return vec4(1.0, y, z, 1.0);\n}\n";
        g += "vec4 rvk_nrm(vec4 s) {\n    float l = dot(s.xyz, s.xyz);\n    return l > 0.0 ? s * inversesqrt(l) : vec4(0.0);\n}\n";
        if (!vs_) {
            g += "bool rvk_alphaTest(float a) {\n"
                 "    float q = round(clamp(a, 0.0, 1.0) * 255.0);\n"
                 "    float r = pc.draw.alphaRef;\n"
                 "    switch (pc.draw.alphaFunc) {\n"
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

    std::string mainFunction() const {
        std::string m = "void main() {\n    rvk_main();\n";
        if (vs_) {
            std::string pos = "oPos";
            if (major_ >= 3) {
                pos = "vec4(0.0)";
                for (const auto& d : p_.outputs) {
                    if (d.usage == Usage::Position && d.usageIndex == 0) {
                        pos = std::format("o{}", d.reg.index);
                    } else if (d.usage == Usage::PSize) {
                        m += std::format("    gl_PointSize = o{}.x;\n", d.reg.index);
                    } else if (const std::uint32_t loc = abi::varyingLocation(d.usage, d.usageIndex); loc != abi::kInvalidLocation) {
                        m += std::format("    rvk_out{} = o{};\n", loc, d.reg.index);
                    }
                }
            } else {
                for (auto i : attrOut_) {
                    m += std::format("    rvk_out{} = oD{};\n", abi::varyingLocation(Usage::Color, i), i);
                }
                for (auto i : outputs_) {
                    m += std::format("    rvk_out{} = oT{};\n", abi::varyingLocation(Usage::TexCoord, i), i);
                }
                if (rastOut_.contains(1)) {
                    m += std::format("    rvk_out{} = vec4(oFog.x);\n", abi::varyingLocation(Usage::Fog, 0));
                }
                if (rastOut_.contains(2)) {
                    m += "    gl_PointSize = oPts.x;\n";
                }
            }
            m += std::format("    vec4 rvk_pos = {};\n", pos);
            if (!opt_.epilogue.empty()) {
                m += opt_.epilogue;
                m += "\n";
            }
            m += "    rvk_pos.xy += pc.draw.posFix.xy * rvk_pos.w;\n    gl_Position = rvk_pos;\n";
        } else {
            if ((reflection_.colorOutputs & 1u) != 0) {
                m += "    if (!rvk_alphaTest(oC0.a)) discard;\n";
                if (opt_.tintEpilogue) {
                    // debug view "tint draws" (abi.h DrawBlock pad[0], 0 = off): oC0 towards the tint, keeping its brightness
                    // (a floor lifts dark surfaces; black stays black, so additive / premultiplied draws keep their shape)
                    m += "    if (pc.draw.pad0 != 0u) {\n"
                         "        vec4 rvk_tint = unpackUnorm4x8(pc.draw.pad0);\n"
                         "        float rvk_l = dot(max(oC0.rgb, vec3(0.0)), vec3(0.299, 0.587, 0.114));\n"
                         "        float rvk_k = 0.25 * clamp(rvk_l * 16.0, 0.0, 1.0) + 0.75 * clamp(rvk_l * 1.5, 0.0, 1.0);\n"
                         "        oC0.rgb = mix(oC0.rgb, rvk_tint.rgb * rvk_k, rvk_tint.a);\n"
                         "    }\n";
                }
            }
            if (!opt_.epilogue.empty()) {
                m += opt_.epilogue;
                m += "\n";
            }
            for (std::uint32_t i = 0; i < 4; ++i) {
                if ((reflection_.colorOutputs & (1u << i)) != 0) {
                    m += std::format("    rvk_oC{} = oC{};\n", i, i);
                }
            }
            if (reflection_.writesDepth) {
                m += "    gl_FragDepth = clamp(oDepth.x, 0.0, 1.0);\n";
            }
        }
        m += "}\n";
        return m;
    }

    const Program& p_;
    const GlslOptions opt_;
    const bool vs_;
    const int major_;
    const std::string consts_;
    Reflection reflection_;
    std::string error_;

    std::set<std::uint32_t> temps_, rastOut_, attrOut_, outputs_, labels_;
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

GlslResult toGlsl(const Program& program, const GlslOptions& options) { return Generator(program, options).run(); }

}  // namespace rvk::shader
