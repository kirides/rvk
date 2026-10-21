#include <format>

#include "shader/sm_ir.h"

namespace rvk::shader {
namespace {

RegType regTypeOf(std::uint32_t tok) {
    return static_cast<RegType>(((tok >> 28) & 0x7) | ((tok >> 8) & 0x18));
}

Register regOf(std::uint32_t tok) { return Register{regTypeOf(tok), tok & 0x7FF}; }

bool hasDestination(Op op) {
    switch (op) {
    case Op::Nop: case Op::Call: case Op::CallNz: case Op::Loop: case Op::Ret: case Op::EndLoop: case Op::Label:
    case Op::Rep: case Op::EndRep: case Op::If: case Op::Ifc: case Op::Else: case Op::EndIf: case Op::Break:
    case Op::Breakc: case Op::Breakp: case Op::Phase:
        return false;
    default:
        return true;
    }
}

class Reader {
public:
    Reader(std::span<const std::uint32_t> tokens, std::size_t begin, std::size_t end) : t_(tokens), i_(begin), end_(end) {}
    bool done() const { return i_ >= end_; }
    std::uint32_t next(std::string& error) {
        if (i_ >= end_) {
            if (error.empty()) {
                error = std::format("instruction operands run past the instruction length at token {}", i_);
            }
            return 0;
        }
        return t_[i_++];
    }

    RelAddr relative(std::uint32_t tok, int major, std::string& error) {
        RelAddr rel;
        if ((tok & (1u << 13)) == 0) {
            return rel;
        }
        rel.present = true;
        if (major < 2) {  // vs_1_1: implicit a0.x
            rel.reg = Register{RegType::Addr, 0};
            return rel;
        }
        const std::uint32_t a = next(error);
        rel.reg = regOf(a);
        rel.component = static_cast<std::uint8_t>((a >> 16) & 3);  // swizzle .x selects the component
        return rel;
    }

    SrcParam src(int major, std::string& error) {
        const std::uint32_t tok = next(error);
        SrcParam p;
        p.reg = regOf(tok);
        const std::uint32_t sw = (tok >> 16) & 0xFF;
        for (int c = 0; c < 4; ++c) {
            p.swizzle[static_cast<std::size_t>(c)] = static_cast<std::uint8_t>((sw >> (2 * c)) & 3);
        }
        p.mod = static_cast<SrcMod>((tok >> 24) & 0xF);
        p.rel = relative(tok, major, error);
        return p;
    }

    DstParam dst(int major, std::string& error) {
        const std::uint32_t tok = next(error);
        DstParam d;
        d.reg = regOf(tok);
        d.writeMask = static_cast<std::uint8_t>((tok >> 16) & 0xF);
        const std::uint32_t mod = (tok >> 20) & 0xF;
        d.saturate = (mod & 1) != 0;
        d.partialPrecision = (mod & 2) != 0;
        d.centroid = (mod & 4) != 0;
        const auto shift = static_cast<std::int8_t>((tok >> 24) & 0xF);
        d.shift = static_cast<std::int8_t>(shift >= 8 ? shift - 16 : shift);
        d.rel = relative(tok, major, error);
        return d;
    }

private:
    std::span<const std::uint32_t> t_;
    std::size_t i_;
    std::size_t end_;
};

}  // namespace

DecodeResult decode(std::span<const std::uint32_t> tokens) {
    DecodeResult result;
    Program& prog = result.program;
    if (tokens.empty()) {
        result.error = "empty token stream";
        return result;
    }
    const auto version = parseVersion(tokens[0]);
    if (!version) {
        result.error = std::format("bad version token 0x{:08x}", tokens[0]);
        return result;
    }
    prog.version = *version;
    if (version->major < 2) {
        result.error = std::format("shader model {}.{} is not supported", version->major, version->minor);
        return result;
    }
    const int major = version->major;

    std::size_t i = 1;
    while (i < tokens.size()) {
        const std::uint32_t tok = tokens[i];
        const auto opcode = static_cast<Op>(tok & 0xFFFF);
        if (opcode == Op::End) {
            return result;
        }
        if (opcode == Op::Comment) {
            i += 1 + ((tok >> 16) & 0x7FFF);
            continue;
        }
        const std::size_t len = (tok >> 24) & 0xF;
        const std::size_t begin = i + 1;
        const std::size_t end = begin + len;
        if (end > tokens.size()) {
            result.error = std::format("instruction at token {} runs past the end", i);
            return result;
        }
        Reader r(tokens, begin, end);
        std::string& err = result.error;

        Instruction ins;
        ins.op = opcode;
        ins.control = (tok >> 16) & 0xFF;
        ins.predicated = (tok & (1u << 28)) != 0;
        ins.coissue = (tok & (1u << 30)) != 0;
        ins.tokenOffset = i;

        if (opcode == Op::Dcl) {
            const std::uint32_t d = r.next(err);
            const DstParam dst = r.dst(major, err);
            if (dst.reg.type == RegType::Sampler) {
                prog.samplers.push_back({dst.reg.index, static_cast<TextureType>((d >> 27) & 0xF)});
            } else {
                Declaration decl;
                decl.reg = dst.reg;
                decl.usage = static_cast<Usage>(d & 0x1F);
                decl.usageIndex = static_cast<std::uint8_t>((d >> 16) & 0xF);
                decl.mask = dst.writeMask;
                decl.centroid = dst.centroid;
                if (dst.reg.type == RegType::Output) {
                    prog.outputs.push_back(decl);
                } else {
                    prog.inputs.push_back(decl);
                }
            }
        } else if (opcode == Op::Def || opcode == Op::DefI || opcode == Op::DefB) {
            ins.hasDst = true;
            ins.dst = r.dst(major, err);
            const int n = opcode == Op::DefB ? 1 : 4;
            for (int k = 0; k < n; ++k) {
                ins.imm[static_cast<std::size_t>(k)] = r.next(err);
            }
            prog.defs.push_back(ins);
        } else {
            if (hasDestination(opcode)) {
                ins.hasDst = true;
                ins.dst = r.dst(major, err);
            }
            if (ins.predicated) {
                ins.predicate = r.src(major, err);
            }
            while (!r.done() && err.empty()) {
                ins.src.push_back(r.src(major, err));
            }
            prog.instructions.push_back(std::move(ins));
        }
        if (!err.empty()) {
            return result;
        }
        if (!r.done()) {
            result.error = std::format("instruction at token {} ({}) has unread operand tokens", i, opName(opcode));
            return result;
        }
        i = end;
    }
    result.error = "no END token";
    return result;
}

const char* opName(Op op) {
    switch (op) {
    case Op::Nop: return "nop";
    case Op::Mov: return "mov";
    case Op::Add: return "add";
    case Op::Sub: return "sub";
    case Op::Mad: return "mad";
    case Op::Mul: return "mul";
    case Op::Rcp: return "rcp";
    case Op::Rsq: return "rsq";
    case Op::Dp3: return "dp3";
    case Op::Dp4: return "dp4";
    case Op::Min: return "min";
    case Op::Max: return "max";
    case Op::Slt: return "slt";
    case Op::Sge: return "sge";
    case Op::Exp: return "exp";
    case Op::Log: return "log";
    case Op::Lit: return "lit";
    case Op::Dst: return "dst";
    case Op::Lrp: return "lrp";
    case Op::Frc: return "frc";
    case Op::M4x4: return "m4x4";
    case Op::M4x3: return "m4x3";
    case Op::M3x4: return "m3x4";
    case Op::M3x3: return "m3x3";
    case Op::M3x2: return "m3x2";
    case Op::Call: return "call";
    case Op::CallNz: return "callnz";
    case Op::Loop: return "loop";
    case Op::Ret: return "ret";
    case Op::EndLoop: return "endloop";
    case Op::Label: return "label";
    case Op::Dcl: return "dcl";
    case Op::Pow: return "pow";
    case Op::Crs: return "crs";
    case Op::Sgn: return "sgn";
    case Op::Abs: return "abs";
    case Op::Nrm: return "nrm";
    case Op::SinCos: return "sincos";
    case Op::Rep: return "rep";
    case Op::EndRep: return "endrep";
    case Op::If: return "if";
    case Op::Ifc: return "if";  // D3DX spells ifc as if_<cmp>
    case Op::Else: return "else";
    case Op::EndIf: return "endif";
    case Op::Break: return "break";
    case Op::Breakc: return "break";  // D3DX spells breakc as break_<cmp>
    case Op::Mova: return "mova";
    case Op::DefB: return "defb";
    case Op::DefI: return "defi";
    case Op::TexCoord: return "texcoord";
    case Op::TexKill: return "texkill";
    case Op::Tex: return "texld";
    case Op::ExpP: return "expp";
    case Op::LogP: return "logp";
    case Op::Cnd: return "cnd";
    case Op::Def: return "def";
    case Op::Cmp: return "cmp";
    case Op::Bem: return "bem";
    case Op::Dp2Add: return "dp2add";
    case Op::Dsx: return "dsx";
    case Op::Dsy: return "dsy";
    case Op::TexLdd: return "texldd";
    case Op::Setp: return "setp";
    case Op::TexLdl: return "texldl";
    case Op::Breakp: return "breakp";
    case Op::Phase: return "phase";
    case Op::Comment: return "comment";
    case Op::End: return "end";
    default: return "op?";
    }
}

}  // namespace rvk::shader
