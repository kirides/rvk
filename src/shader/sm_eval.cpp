#include "shader/sm_eval.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <format>
#include <limits>
#include <map>
#include <set>
#include <unordered_map>

namespace rvk::shader::eval {
namespace {

constexpr float kNan = std::numeric_limits<float>::quiet_NaN();

float asFloat(std::uint32_t bits) {
    float f;
    std::memcpy(&f, &bits, 4);
    return f;
}

bool isFlow(Op op) {
    switch (op) {
    case Op::If: case Op::Ifc: case Op::Else: case Op::EndIf: case Op::Rep: case Op::EndRep: case Op::Loop: case Op::EndLoop:
    case Op::Break: case Op::Breakc: case Op::Breakp: case Op::Call: case Op::CallNz: case Op::Label: case Op::Ret:
        return true;
    default: return false;
    }
}

bool compare(Cmp c, float a, float b) {
    switch (c) {
    case Cmp::Gt: return a > b;
    case Cmp::Eq: return a == b;
    case Cmp::Ge: return a >= b;
    case Cmp::Lt: return a < b;
    case Cmp::Ne: return a != b;
    case Cmp::Le: return a <= b;
    default: return false;
    }
}

int matrixRows(Op op) {
    switch (op) {
    case Op::M4x4: return 4;
    case Op::M4x3: return 3;
    case Op::M3x4: return 4;
    case Op::M3x3: return 3;
    case Op::M3x2: return 2;
    default: return 0;
    }
}

bool isTex(Op op) { return op == Op::Tex || op == Op::TexLdl || op == Op::TexLdd; }

class Machine {
public:
    Machine(const Program& p, const Env& e) : p_(p), e_(e) {
        for (const Instruction& d : p.defs) {
            if (d.op == Op::Def) {
                defF_[d.dst.reg.index] = {asFloat(d.imm[0]), asFloat(d.imm[1]), asFloat(d.imm[2]), asFloat(d.imm[3])};
            } else if (d.op == Op::DefI) {
                defI_[d.dst.reg.index] = {static_cast<std::int32_t>(d.imm[0]), static_cast<std::int32_t>(d.imm[1]),
                                          static_cast<std::int32_t>(d.imm[2]), static_cast<std::int32_t>(d.imm[3])};
            } else if (d.op == Op::DefB) {
                defB_[d.dst.reg.index] = d.imm[0] != 0;
            }
        }
    }

    Result run() {
        Result res;
        if (p_.version.major < 3) {
            res.error = "only SM 3.0 programs are evaluated";
            return res;
        }
        if (!match(res.error)) return res;
        const auto& ins = p_.instructions;
        std::size_t pc = 0, steps = 0;
        struct LoopState {
            std::size_t start;   // the rep / loop instruction
            std::int32_t left;
            std::int32_t step;   // loop: aL increment
            std::int32_t savedAl;
            bool isLoop;
        };
        std::vector<LoopState> loops;
        while (pc < ins.size()) {
            if (++steps > e_.maxSteps) {
                res.error = "step limit";
                return res;
            }
            const Instruction& in = ins[pc];
            switch (in.op) {
            case Op::If: {
                if (in.src.empty()) return fail(res, in);
                if (!boolOf(in.src[0])) {
                    pc = else_[pc] != 0 ? else_[pc] + 1 : end_[pc] + 1;
                    continue;
                }
                break;
            }
            case Op::Ifc: {
                if (in.src.size() < 2) return fail(res, in);
                if (!compare(in.comparison(), read(in.src[0])[0], read(in.src[1])[0])) {
                    pc = else_[pc] != 0 ? else_[pc] + 1 : end_[pc] + 1;
                    continue;
                }
                break;
            }
            case Op::Else:  // reached at the end of the taken if-branch
                pc = end_[pc] + 1;
                continue;
            case Op::EndIf: break;
            case Op::Rep: {
                if (in.src.empty()) return fail(res, in);
                const std::int32_t n = intOf(in.src[0])[0];
                if (n <= 0) {
                    pc = end_[pc] + 1;
                    continue;
                }
                loops.push_back({pc, std::min(n, 255), 0, aL_, false});
                break;
            }
            case Op::Loop: {
                if (in.src.size() < 2) return fail(res, in);
                const auto i = intOf(in.src[1]);
                if (i[0] <= 0) {
                    pc = end_[pc] + 1;
                    continue;
                }
                loops.push_back({pc, std::min(i[0], 255), i[2], aL_, true});
                aL_ = i[1];
                break;
            }
            case Op::EndRep:
            case Op::EndLoop: {
                if (loops.empty()) return fail(res, in);
                LoopState& l = loops.back();
                if (--l.left > 0) {
                    if (l.isLoop) aL_ += l.step;
                    pc = l.start + 1;
                    continue;
                }
                if (l.isLoop) aL_ = l.savedAl;
                loops.pop_back();
                break;
            }
            case Op::Break:
            case Op::Breakc:
            case Op::Breakp: {
                bool take = true;
                if (in.op == Op::Breakc) {
                    if (in.src.size() < 2) return fail(res, in);
                    take = compare(in.comparison(), read(in.src[0])[0], read(in.src[1])[0]);
                } else if (in.op == Op::Breakp) {
                    if (in.src.empty()) return fail(res, in);
                    take = boolOf(in.src[0]);
                }
                if (take) {
                    if (loops.empty()) return fail(res, in);
                    const LoopState l = loops.back();
                    loops.pop_back();
                    if (l.isLoop) aL_ = l.savedAl;
                    pc = end_[l.start] + 1;
                    continue;
                }
                break;
            }
            case Op::Call:
            case Op::CallNz:
            case Op::Label:
            case Op::Ret: return fail(res, in);
            default:
                if (!execute(in, pc, res)) return res;
                break;
            }
            ++pc;
        }
        for (std::size_t k = 0; k < res.outputs.size(); ++k) res.outputs[k] = p_.version.stage == Stage::Vertex ? o_[k] : oC_[k % 4];
        res.depth = oDepth_;
        res.killed = killed_;
        res.ok = true;
        return res;
    }

private:
    Result& fail(Result& r, const Instruction& in) {
        r.error = std::format("{} at token {} not supported here", opName(in.op), in.tokenOffset);
        return r;
    }

    // matching if / else / endif and rep / loop / endrep / endloop
    bool match(std::string& error) {
        const auto& ins = p_.instructions;
        else_.assign(ins.size(), 0);
        end_.assign(ins.size(), 0);
        std::vector<std::size_t> stack;
        for (std::size_t i = 0; i < ins.size(); ++i) {
            const Op op = ins[i].op;
            if (op == Op::If || op == Op::Ifc || op == Op::Rep || op == Op::Loop) {
                stack.push_back(i);
            } else if (op == Op::Else) {
                if (stack.empty()) return error = "else without if", false;
                else_[stack.back()] = i;
                end_[i] = stack.back();  // fixed below
            } else if (op == Op::EndIf || op == Op::EndRep || op == Op::EndLoop) {
                if (stack.empty()) return error = "unbalanced flow control", false;
                const std::size_t open = stack.back();
                stack.pop_back();
                end_[open] = i;
                if (else_[open] != 0) end_[else_[open]] = i;
            }
        }
        if (!stack.empty()) return error = "unbalanced flow control", false;
        return true;
    }

    std::int32_t relOffset(const RelAddr& rel) const {
        if (!rel.present) return 0;
        if (rel.reg.type == RegType::Loop) return aL_;
        return a0_[rel.component & 3];
    }

    Vec4 constant(std::int64_t index) const {
        if (index < 0) return {0, 0, 0, 0};
        if (const auto it = defF_.find(static_cast<std::uint32_t>(index)); it != defF_.end()) return it->second;
        if (e_.floats == nullptr || index >= e_.floatCount) return {0, 0, 0, 0};
        const float* c = e_.floats[index];
        return {c[0], c[1], c[2], c[3]};
    }

    std::array<std::int32_t, 4> intConst(std::uint32_t index) const {
        if (const auto it = defI_.find(index); it != defI_.end()) return it->second;
        if (e_.ints == nullptr || index >= 16) return {0, 0, 0, 0};
        return {e_.ints[index][0], e_.ints[index][1], e_.ints[index][2], e_.ints[index][3]};
    }

    bool boolConst(std::uint32_t index) const {
        if (const auto it = defB_.find(index); it != defB_.end()) return it->second;
        return e_.bools != nullptr && index < 16 && e_.bools[index] != 0;
    }

    Vec4 raw(const Register& r, const RelAddr& rel, std::uint32_t indexOffset) const {
        const std::uint32_t n = r.index + indexOffset;
        switch (r.type) {
        case RegType::Temp:
        case RegType::TempFloat16: return n < r_.size() ? r_[n] : Vec4{kNan, kNan, kNan, kNan};
        case RegType::Input: return n < e_.inputs.size() ? e_.inputs[n] : Vec4{0, 0, 0, 0};
        case RegType::Const: return constant(static_cast<std::int64_t>(n) + relOffset(rel));
        case RegType::ConstInt: {
            const auto i = intConst(n);
            return {static_cast<float>(i[0]), static_cast<float>(i[1]), static_cast<float>(i[2]), static_cast<float>(i[3])};
        }
        case RegType::ConstBool: {
            const float b = boolConst(n) ? 1.0f : 0.0f;
            return {b, b, b, b};
        }
        case RegType::Addr:
            return {static_cast<float>(a0_[0]), static_cast<float>(a0_[1]), static_cast<float>(a0_[2]), static_cast<float>(a0_[3])};
        case RegType::Loop: return {static_cast<float>(aL_), static_cast<float>(aL_), static_cast<float>(aL_), static_cast<float>(aL_)};
        case RegType::MiscType: return n == 0 ? e_.position : Vec4{e_.face, e_.face, e_.face, e_.face};
        case RegType::Predicate: return {p0_[0] ? 1.0f : 0.0f, p0_[1] ? 1.0f : 0.0f, p0_[2] ? 1.0f : 0.0f, p0_[3] ? 1.0f : 0.0f};
        case RegType::Output: return n < o_.size() ? o_[n] : Vec4{0, 0, 0, 0};
        case RegType::ColorOut: return oC_[n & 3];
        default: return {kNan, kNan, kNan, kNan};
        }
    }

    Vec4 read(const SrcParam& s, std::uint32_t indexOffset = 0) const {
        const Vec4 v = raw(s.reg, s.rel, indexOffset);
        Vec4 o{};
        for (int c = 0; c < 4; ++c) o[c] = v[s.swizzle[c] & 3];
        for (float& x : o) {
            switch (s.mod) {
            case SrcMod::None: break;
            case SrcMod::Neg: x = -x; break;
            case SrcMod::Bias: x = x - 0.5f; break;
            case SrcMod::BiasNeg: x = 0.5f - x; break;
            case SrcMod::Sign: x = x * 2.0f - 1.0f; break;
            case SrcMod::SignNeg: x = 1.0f - x * 2.0f; break;
            case SrcMod::Comp: x = 1.0f - x; break;
            case SrcMod::X2: x = x * 2.0f; break;
            case SrcMod::X2Neg: x = x * -2.0f; break;
            case SrcMod::Abs: x = std::fabs(x); break;
            case SrcMod::AbsNeg: x = -std::fabs(x); break;
            default: break;
            }
        }
        return o;
    }

    std::array<std::int32_t, 4> intOf(const SrcParam& s) const {
        if (s.reg.type == RegType::ConstInt) return intConst(s.reg.index);
        const Vec4 v = read(s);
        return {static_cast<std::int32_t>(v[0]), static_cast<std::int32_t>(v[1]), static_cast<std::int32_t>(v[2]),
                static_cast<std::int32_t>(v[3])};
    }

    bool boolOf(const SrcParam& s) const {
        bool b = false;
        if (s.reg.type == RegType::ConstBool) b = boolConst(s.reg.index);
        else if (s.reg.type == RegType::Predicate) b = p0_[s.swizzle[0] & 3];
        return s.mod == SrcMod::Not ? !b : b;
    }

    void write(const Instruction& in, const Vec4& value) {
        const DstParam& d = in.dst;
        Vec4* target = nullptr;
        const std::uint32_t n = d.reg.index + static_cast<std::uint32_t>(relOffset(d.rel));
        switch (d.reg.type) {
        case RegType::Temp:
        case RegType::TempFloat16: target = n < r_.size() ? &r_[n] : nullptr; break;
        case RegType::Output: target = n < o_.size() ? &o_[n] : nullptr; break;
        case RegType::ColorOut: target = &oC_[n & 3]; break;
        case RegType::DepthOut:
            if (!in.predicated || predicate(in, 0)) oDepth_ = d.saturate ? std::clamp(value[0], 0.0f, 1.0f) : value[0];
            return;
        default: return;
        }
        if (target == nullptr) return;
        for (int c = 0; c < 4; ++c) {
            if ((d.writeMask & (1 << c)) == 0) continue;
            if (in.predicated && !predicate(in, c)) continue;
            float v = value[c];
            if (d.saturate) v = std::isnan(v) ? 0.0f : std::clamp(v, 0.0f, 1.0f);
            (*target)[c] = v;
        }
    }

    bool predicate(const Instruction& in, int c) const {
        const bool b = p0_[in.predicate.swizzle[c] & 3];
        return in.predicate.mod == SrcMod::Not ? !b : b;
    }

    static Vec4 splat(float x) { return {x, x, x, x}; }

    bool execute(const Instruction& in, std::size_t pc, Result& res) {
        const auto& s = in.src;
        const auto need = [&](std::size_t k) {
            if (s.size() >= k) return true;
            res.error = std::format("{} at token {}: {} operands expected", opName(in.op), in.tokenOffset, k);
            return false;
        };
        const auto each = [](const Vec4& a, auto f) {
            Vec4 o;
            for (int c = 0; c < 4; ++c) o[c] = f(a[c], c);
            return o;
        };
        Vec4 v{};
        switch (in.op) {
        case Op::Nop: return true;
        case Op::Mov:
            if (!need(1)) return false;
            v = read(s[0]);
            break;
        case Op::Mova: {
            if (!need(1)) return false;
            const Vec4 a = read(s[0]);
            for (int c = 0; c < 4; ++c) {
                if ((in.dst.writeMask & (1 << c)) != 0) a0_[c] = static_cast<std::int32_t>(std::floor(a[c] + 0.5f));
            }
            return true;
        }
        case Op::Add:
        case Op::Sub:
        case Op::Mul:
        case Op::Min:
        case Op::Max:
        case Op::Slt:
        case Op::Sge: {
            if (!need(2)) return false;
            const Vec4 a = read(s[0]), b = read(s[1]);
            const Op op = in.op;
            v = each(a, [&](float x, int c) {
                const float y = b[c];
                switch (op) {
                case Op::Add: return x + y;
                case Op::Sub: return x - y;
                case Op::Mul: return x * y;
                case Op::Min: return std::fmin(x, y);
                case Op::Max: return std::fmax(x, y);
                case Op::Slt: return x < y ? 1.0f : 0.0f;
                default: return x >= y ? 1.0f : 0.0f;
                }
            });
            break;
        }
        case Op::Mad: {
            if (!need(3)) return false;
            const Vec4 a = read(s[0]), b = read(s[1]), c3 = read(s[2]);
            v = each(a, [&](float x, int c) { return x * b[c] + c3[c]; });
            break;
        }
        case Op::Rcp:
            if (!need(1)) return false;
            v = splat(1.0f / read(s[0])[0]);
            break;
        case Op::Rsq:
            if (!need(1)) return false;
            v = splat(1.0f / std::sqrt(std::fabs(read(s[0])[0])));
            break;
        case Op::Dp3:
        case Op::Dp4: {
            if (!need(2)) return false;
            const Vec4 a = read(s[0]), b = read(s[1]);
            float d = a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
            if (in.op == Op::Dp4) d += a[3] * b[3];
            v = splat(d);
            break;
        }
        case Op::Exp:
        case Op::ExpP:
            if (!need(1)) return false;
            v = splat(std::exp2(read(s[0])[0]));
            break;
        case Op::Log:
        case Op::LogP:
            if (!need(1)) return false;
            v = splat(std::log2(std::fabs(read(s[0])[0])));
            break;
        case Op::Lit: {
            if (!need(1)) return false;
            const Vec4 a = read(s[0]);
            const float y = std::fmax(a[0], 0.0f);
            const float z = (a[0] > 0.0f && a[1] > 0.0f) ? std::pow(a[1], std::clamp(a[3], -128.0f, 128.0f)) : 0.0f;
            v = {1.0f, y, z, 1.0f};
            break;
        }
        case Op::Dst: {
            if (!need(2)) return false;
            const Vec4 a = read(s[0]), b = read(s[1]);
            v = {1.0f, a[1] * b[1], a[2], b[3]};
            break;
        }
        case Op::Lrp: {
            if (!need(3)) return false;
            const Vec4 t = read(s[0]), a = read(s[1]), b = read(s[2]);
            v = each(t, [&](float x, int c) { return b[c] + x * (a[c] - b[c]); });
            break;
        }
        case Op::Frc:
            if (!need(1)) return false;
            v = each(read(s[0]), [](float x, int) { return x - std::floor(x); });
            break;
        case Op::M4x4:
        case Op::M4x3:
        case Op::M3x4:
        case Op::M3x3:
        case Op::M3x2: {
            if (!need(2)) return false;
            const int rows = matrixRows(in.op);
            const bool dot3 = in.op == Op::M3x4 || in.op == Op::M3x3 || in.op == Op::M3x2;
            const Vec4 a = read(s[0]);
            v = {0, 0, 0, 0};
            for (int k = 0; k < rows; ++k) {
                const Vec4 row = read(s[1], static_cast<std::uint32_t>(k));
                v[k] = a[0] * row[0] + a[1] * row[1] + a[2] * row[2] + (dot3 ? 0.0f : a[3] * row[3]);
            }
            break;
        }
        case Op::Pow:
            if (!need(2)) return false;
            v = splat(std::pow(std::fabs(read(s[0])[0]), read(s[1])[0]));
            break;
        case Op::Crs: {
            if (!need(2)) return false;
            const Vec4 a = read(s[0]), b = read(s[1]);
            v = {a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0], 0.0f};
            break;
        }
        case Op::Sgn:
            if (!need(1)) return false;
            v = each(read(s[0]), [](float x, int) { return x > 0.0f ? 1.0f : x < 0.0f ? -1.0f : 0.0f; });
            break;
        case Op::Abs:
            if (!need(1)) return false;
            v = each(read(s[0]), [](float x, int) { return std::fabs(x); });
            break;
        case Op::Nrm: {
            if (!need(1)) return false;
            const Vec4 a = read(s[0]);
            const float l = a[0] * a[0] + a[1] * a[1] + a[2] * a[2];
            const float k = l > 0.0f ? 1.0f / std::sqrt(l) : 0.0f;
            v = each(a, [&](float x, int) { return x * k; });
            break;
        }
        case Op::SinCos: {
            if (!need(1)) return false;
            const float x = read(s[0])[0];
            v = {std::cos(x), std::sin(x), 0.0f, 0.0f};
            break;
        }
        case Op::Cmp:
        case Op::Cnd: {
            if (!need(3)) return false;
            const Vec4 a = read(s[0]), b = read(s[1]), c3 = read(s[2]);
            const bool cnd = in.op == Op::Cnd;
            v = each(a, [&](float x, int c) { return (cnd ? x > 0.5f : x >= 0.0f) ? b[c] : c3[c]; });
            break;
        }
        case Op::Dp2Add: {
            if (!need(3)) return false;
            const Vec4 a = read(s[0]), b = read(s[1]);
            v = splat(a[0] * b[0] + a[1] * b[1] + read(s[2])[0]);
            break;
        }
        case Op::Dsx:
        case Op::Dsy: v = {0, 0, 0, 0}; break;
        case Op::Tex:
        case Op::TexLdl:
        case Op::TexLdd: {
            if (!need(2)) return false;
            const Vec4 coord = read(s[0]);
            const std::uint32_t sampler = s[1].reg.index;
            const Vec4 t = e_.sample ? e_.sample(sampler, coord, pc) : Vec4{0.5f, 0.5f, 0.5f, 1.0f};
            for (int c = 0; c < 4; ++c) v[c] = t[s[1].swizzle[c] & 3];
            break;
        }
        case Op::TexKill: {
            const Vec4 a = raw(in.dst.reg, in.dst.rel, 0);
            for (int c = 0; c < 4; ++c) {
                if ((in.dst.writeMask & (1 << c)) != 0 && a[c] < 0.0f) killed_ = true;
            }
            return true;
        }
        case Op::Setp: {
            if (!need(2)) return false;
            const Vec4 a = read(s[0]), b = read(s[1]);
            for (int c = 0; c < 4; ++c) {
                if ((in.dst.writeMask & (1 << c)) != 0 && (!in.predicated || predicate(in, c))) p0_[c] = compare(in.comparison(), a[c], b[c]);
            }
            return true;
        }
        default:
            res.error = std::format("{} at token {} not evaluated", opName(in.op), in.tokenOffset);
            return false;
        }
        if (in.hasDst) write(in, v);
        return true;
    }

    const Program& p_;
    const Env& e_;
    std::unordered_map<std::uint32_t, Vec4> defF_;
    std::unordered_map<std::uint32_t, std::array<std::int32_t, 4>> defI_;
    std::unordered_map<std::uint32_t, bool> defB_;
    std::vector<std::size_t> else_, end_;
    std::array<Vec4, 32> r_{};
    std::array<Vec4, 16> o_{};
    std::array<Vec4, 4> oC_{};
    float oDepth_ = 0.0f;
    std::array<std::int32_t, 4> a0_{};
    std::int32_t aL_ = 0;
    std::array<bool, 4> p0_{};
    bool killed_ = false;
};

// ---- slices ------------------------------------------------------------------------------------------------------------

std::uint32_t regKey(RegType t, std::uint32_t index) { return (static_cast<std::uint32_t>(t) << 16) | (index & 0xFFFF); }

// the post-swizzle components of source `k` that instruction `in` reads to produce the written components `written`
std::uint8_t sourceComponents(const Instruction& in, std::size_t k, std::uint8_t written) {
    switch (in.op) {
    case Op::Mov: case Op::Add: case Op::Sub: case Op::Mul: case Op::Mad: case Op::Min: case Op::Max: case Op::Slt:
    case Op::Sge: case Op::Frc: case Op::Lrp: case Op::Cmp: case Op::Cnd: case Op::Abs: case Op::Sgn: case Op::Mova:
    case Op::Dsx: case Op::Dsy: case Op::Setp:
        return written;
    case Op::Rcp: case Op::Rsq: case Op::Exp: case Op::ExpP: case Op::Log: case Op::LogP: case Op::SinCos: case Op::Pow:
        return 0x1;
    case Op::Dp3: case Op::Crs: return 0x7;
    case Op::Nrm: return static_cast<std::uint8_t>(0x7 | written);
    case Op::Dp4: return 0xF;
    case Op::Dp2Add: return k == 2 ? 0x1 : 0x3;
    case Op::Lit: return 0xB;
    case Op::Dst: return k == 0 ? 0x6 : 0xA;
    case Op::M3x4: case Op::M3x3: case Op::M3x2: return 0x7;
    default: return 0xF;  // texld (coordinate and all), matrices' rows, the rest: conservative
    }
}

class Slicer {
public:
    explicit Slicer(const Program& p) : p_(p) {
        for (const Instruction& d : p.defs) {
            if (d.op == Op::Def) defF_.insert(d.dst.reg.index);
        }
    }

    // `src` read with post-swizzle components `components` (rows: matrix registers index .. index + rows - 1)
    void need(const SrcParam& src, std::uint8_t components, std::uint32_t rows = 1) {
        std::uint8_t regMask = 0;
        for (int c = 0; c < 4; ++c) {
            if ((components & (1 << c)) != 0) regMask |= static_cast<std::uint8_t>(1 << (src.swizzle[c] & 3));
        }
        if (regMask == 0) return;
        if (src.rel.present) {
            slice_.relative = true;
            if (src.rel.reg.type == RegType::Addr) live_[regKey(RegType::Addr, 0)] |= static_cast<std::uint8_t>(1 << (src.rel.component & 3));
            else slice_.flow = true;  // aL: inside a loop
        }
        for (std::uint32_t k = 0; k < rows; ++k) {
            const std::uint32_t n = src.reg.index + k;
            switch (src.reg.type) {
            case RegType::Temp:
            case RegType::TempFloat16: live_[regKey(RegType::Temp, n)] |= regMask; break;
            case RegType::Addr: live_[regKey(RegType::Addr, 0)] |= regMask; break;
            case RegType::Input:
                if (n < 16) slice_.inputs[n] |= regMask;
                break;
            case RegType::Const:
                if (src.rel.present || !defF_.contains(n)) consts_.insert(static_cast<std::uint16_t>(n));
                break;
            case RegType::ConstInt:
            case RegType::ConstBool: slice_.usesInts = true; break;
            default: break;
            }
        }
    }

    void needLive(RegType type, std::uint32_t index, std::uint8_t mask) { live_[regKey(type, index)] |= mask; }

    // walks instructions [0, end) backwards
    Slice run(std::size_t end) {
        const auto& ins = p_.instructions;
        bool flow = false;
        for (std::size_t j = 0; j < std::min(end, ins.size()); ++j) {
            if (isFlow(ins[j].op) || ins[j].predicated) flow = true;
        }
        if (flow) {
            // conservative: every constant read before `end`, every input
            slice_.flow = true;
            slice_.usesInts = true;
            for (std::size_t j = 0; j < std::min(end, ins.size()); ++j) {
                for (const SrcParam& s : ins[j].src) need(s, 0xF, std::max<std::uint32_t>(1, static_cast<std::uint32_t>(matrixRows(ins[j].op))));
            }
            for (auto& m : slice_.inputs) m = 0xF;
        } else {
            for (std::size_t j = std::min(end, ins.size()); j-- > 0;) {
                const Instruction& in = ins[j];
                if (!in.hasDst || in.op == Op::TexKill) continue;
                RegType t = in.dst.reg.type;
                if (t == RegType::TempFloat16) t = RegType::Temp;
                const auto it = live_.find(regKey(t, in.dst.reg.index));
                if (it == live_.end()) continue;
                // the live components this instruction produces (sourceComponents maps them onto what it reads)
                const std::uint8_t written = static_cast<std::uint8_t>(in.dst.writeMask & it->second);
                if (written == 0) continue;
                it->second = static_cast<std::uint8_t>(it->second & ~written);
                if (it->second == 0) live_.erase(it);
                for (std::size_t k = 0; k < in.src.size(); ++k) {
                    if (in.src[k].reg.type == RegType::Sampler) continue;
                    const std::uint32_t rows = k == 1 ? static_cast<std::uint32_t>(std::max(1, matrixRows(in.op))) : 1u;
                    need(in.src[k], sourceComponents(in, k, written), rows);
                }
            }
        }
        slice_.floatConsts.assign(consts_.begin(), consts_.end());
        return slice_;
    }

private:
    const Program& p_;
    std::set<std::uint32_t> defF_;
    std::map<std::uint32_t, std::uint8_t> live_;
    std::set<std::uint16_t> consts_;
    Slice slice_;
};

}  // namespace

Result run(const Program& program, const Env& env) {
    Machine m(program, env);
    return m.run();
}

Slice sliceSource(const Program& program, std::size_t at, std::size_t src, std::uint8_t components) {
    Slicer s(program);
    if (at < program.instructions.size() && src < program.instructions[at].src.size()) s.need(program.instructions[at].src[src], components);
    return s.run(at);
}

Slice sliceOutputs(const Program& program, const std::array<std::uint8_t, 16>& outputs) {
    Slicer s(program);
    for (std::uint32_t i = 0; i < outputs.size(); ++i) {
        if (outputs[i] != 0) s.needLive(RegType::Output, i, outputs[i]);
    }
    return s.run(program.instructions.size());
}

std::size_t firstSample(const Program& program, std::uint32_t sampler) {
    for (std::size_t i = 0; i < program.instructions.size(); ++i) {
        const Instruction& in = program.instructions[i];
        if (isTex(in.op) && in.src.size() >= 2 && in.src[1].reg.type == RegType::Sampler && in.src[1].reg.index == sampler) return i;
    }
    return SIZE_MAX;
}

}  // namespace rvk::shader::eval
