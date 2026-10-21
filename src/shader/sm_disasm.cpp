#include <cstdio>
#include <cstring>
#include <format>

#include "shader/sm_ir.h"

namespace rvk::shader {
namespace {

std::string regName(const Register& r, Stage stage, int major) {
    const auto n = r.index;
    switch (r.type) {
    case RegType::Temp: return std::format("r{}", n);
    case RegType::Input: return std::format("v{}", n);
    case RegType::Const: return std::format("c{}", n);
    case RegType::Addr: return stage == Stage::Vertex ? std::format("a{}", n) : std::format("t{}", n);
    case RegType::RastOut: return n == 0 ? "oPos" : n == 1 ? "oFog" : "oPts";
    case RegType::AttrOut: return std::format("oD{}", n);
    case RegType::Output: return major >= 3 ? std::format("o{}", n) : std::format("oT{}", n);
    case RegType::ConstInt: return std::format("i{}", n);
    case RegType::ColorOut: return std::format("oC{}", n);
    case RegType::DepthOut: return "oDepth";
    case RegType::Sampler: return std::format("s{}", n);
    case RegType::ConstBool: return std::format("b{}", n);
    case RegType::Loop: return "aL";
    case RegType::MiscType: return n == 0 ? "vPos" : "vFace";
    case RegType::Label: return std::format("l{}", n);
    case RegType::Predicate: return std::format("p{}", n);
    default: return std::format("?{}:{}", static_cast<int>(r.type), n);
    }
}

std::string relString(const RelAddr& rel, Stage stage, int major) {
    if (!rel.present) {
        return {};
    }
    if (rel.reg.type == RegType::Loop) {
        return "[aL]";  // the loop counter is a scalar
    }
    return std::format("[{}.{}]", regName(rel.reg, stage, major), "xyzw"[rel.component]);
}

std::string maskString(std::uint8_t mask) {
    if (mask == 0xF) {
        return {};
    }
    std::string s = ".";
    for (int c = 0; c < 4; ++c) {
        if ((mask & (1 << c)) != 0) {
            s += "xyzw"[c];
        }
    }
    return s;
}

std::string swizzleString(const std::array<std::uint8_t, 4>& sw) {
    if (sw == std::array<std::uint8_t, 4>{0, 1, 2, 3}) {
        return {};
    }
    std::string s = ".";
    for (auto c : sw) {
        s += "xyzw"[c];
    }
    if (sw[0] == sw[1] && sw[1] == sw[2] && sw[2] == sw[3]) {
        s.resize(2);
    }
    return s;
}

// D3DX order: prefix, register, suffix modifier, swizzle ("-r1_abs.x")
std::string srcString(const SrcParam& p, Stage stage, int major) {
    const std::string r = regName(p.reg, stage, major) + relString(p.rel, stage, major);
    const std::string sw = swizzleString(p.swizzle);
    switch (p.mod) {
    case SrcMod::None: return r + sw;
    case SrcMod::Neg: return "-" + r + sw;
    case SrcMod::Bias: return r + "_bias" + sw;
    case SrcMod::BiasNeg: return "-" + r + "_bias" + sw;
    case SrcMod::Sign: return r + "_bx2" + sw;
    case SrcMod::SignNeg: return "-" + r + "_bx2" + sw;
    case SrcMod::Comp: return "1-" + r + sw;
    case SrcMod::X2: return r + "_x2" + sw;
    case SrcMod::X2Neg: return "-" + r + "_x2" + sw;
    case SrcMod::Dz: return r + "_dz" + sw;
    case SrcMod::Dw: return r + "_dw" + sw;
    case SrcMod::Abs: return r + "_abs" + sw;
    case SrcMod::AbsNeg: return "-" + r + "_abs" + sw;
    case SrcMod::Not: return "!" + r + sw;
    }
    return r + sw;
}

const char* usageName(Usage u) {
    switch (u) {
    case Usage::Position: return "position";
    case Usage::BlendWeight: return "blendweight";
    case Usage::BlendIndices: return "blendindices";
    case Usage::Normal: return "normal";
    case Usage::PSize: return "psize";
    case Usage::TexCoord: return "texcoord";
    case Usage::Tangent: return "tangent";
    case Usage::Binormal: return "binormal";
    case Usage::TessFactor: return "tessfactor";
    case Usage::PositionT: return "positiont";
    case Usage::Color: return "color";
    case Usage::Fog: return "fog";
    case Usage::Depth: return "depth";
    case Usage::Sample: return "sample";
    }
    return "usage?";
}

const char* cmpName(Cmp c) {
    switch (c) {
    case Cmp::Gt: return "_gt";
    case Cmp::Eq: return "_eq";
    case Cmp::Ge: return "_ge";
    case Cmp::Lt: return "_lt";
    case Cmp::Ne: return "_ne";
    case Cmp::Le: return "_le";
    default: return "";
    }
}

}  // namespace

std::string disassemble(const Program& p) {
    const Stage stage = p.version.stage;
    const int major = p.version.major;
    std::string out = std::format("{}_{}_{}\n", stage == Stage::Vertex ? "vs" : "ps", major, p.version.minor);
    for (const auto& d : p.inputs) {
        if (major < 3 && stage == Stage::Pixel) {  // ps_2_x inputs carry no semantic (D3DX prints a bare dcl)
            out += std::format("dcl {}{}\n", regName(d.reg, stage, major), maskString(d.mask));
            continue;
        }
        if (d.reg.type == RegType::MiscType) {
            out += std::format("dcl {}{}\n", regName(d.reg, stage, major), maskString(d.mask));
            continue;
        }
        out += std::format("dcl_{}{} {}{}\n", usageName(d.usage), d.usageIndex == 0 ? std::string() : std::to_string(d.usageIndex),
                           regName(d.reg, stage, major), maskString(d.mask));
    }
    for (const auto& d : p.outputs) {
        out += std::format("dcl_{}{} {}{}\n", usageName(d.usage), d.usageIndex == 0 ? std::string() : std::to_string(d.usageIndex),
                           regName(d.reg, stage, major), maskString(d.mask));
    }
    for (const auto& s : p.samplers) {
        const char* t = s.type == TextureType::Cube ? "cube" : s.type == TextureType::Volume ? "volume" : "2d";
        out += std::format("dcl_{} s{}\n", t, s.index);
    }
    for (const auto& d : p.defs) {
        const std::string r = regName(d.dst.reg, stage, major);
        if (d.op == Op::Def) {
            float f[4];
            std::memcpy(f, d.imm.data(), sizeof(f));
            // %.9g through the CRT, like D3DXDisassembleShader (golden comparisons: tools/compare_disasm.py)
            char buf[160];
            std::snprintf(buf, sizeof(buf), "def %s, %.9g, %.9g, %.9g, %.9g\n", r.c_str(), f[0], f[1], f[2], f[3]);
            out += buf;
        } else if (d.op == Op::DefI) {
            out += std::format("defi {}, {}, {}, {}, {}\n", r, static_cast<std::int32_t>(d.imm[0]),
                               static_cast<std::int32_t>(d.imm[1]), static_cast<std::int32_t>(d.imm[2]),
                               static_cast<std::int32_t>(d.imm[3]));
        } else {
            out += std::format("defb {}, {}\n", r, d.imm[0] != 0 ? "true" : "false");
        }
    }
    for (const auto& ins : p.instructions) {
        std::string line;
        if (ins.predicated) {
            line += "(" + srcString(ins.predicate, stage, major) + ") ";
        }
        line += opName(ins.op);
        if (ins.op == Op::Tex) {
            line += ins.texProject() ? "p" : ins.texBias() ? "b" : "";
        }
        if (ins.op == Op::Ifc || ins.op == Op::Breakc || ins.op == Op::Setp) {
            line += cmpName(ins.comparison());
        }
        if (ins.hasDst) {
            if (ins.dst.saturate) {
                line += "_sat";
            }
            if (ins.dst.partialPrecision) {
                line += "_pp";
            }
            if (ins.dst.centroid) {
                line += "_centroid";
            }
        }
        bool first = true;
        auto add = [&](const std::string& s) {
            line += first ? " " : ", ";
            line += s;
            first = false;
        };
        if (ins.hasDst) {
            add(regName(ins.dst.reg, stage, major) + relString(ins.dst.rel, stage, major) + maskString(ins.dst.writeMask));
        }
        for (const auto& s : ins.src) {
            add(srcString(s, stage, major));
        }
        out += line + "\n";
    }
    return out;
}

}  // namespace rvk::shader
