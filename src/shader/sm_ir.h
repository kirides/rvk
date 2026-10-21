#pragma once

// Decoded D3D9 shader bytecode (SM 2.0 - 3.0; SM 1.x is decoded but not translated). Names follow the D3D9 DDK
// (D3DSIO_*, D3DSPR_*, D3DSPSM_*, D3DDECLUSAGE_*).

#include <array>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include "shader/sm_tokens.h"

namespace rvk::shader {

enum class Op : std::uint16_t {
    Nop = 0, Mov, Add, Sub, Mad, Mul, Rcp, Rsq, Dp3, Dp4, Min, Max, Slt, Sge, Exp, Log, Lit, Dst, Lrp, Frc,
    M4x4, M4x3, M3x4, M3x3, M3x2, Call, CallNz, Loop, Ret, EndLoop, Label, Dcl, Pow, Crs, Sgn, Abs, Nrm, SinCos,
    Rep, EndRep, If, Ifc, Else, EndIf, Break, Breakc, Mova, DefB, DefI,
    TexCoord = 64, TexKill, Tex, TexBem, TexBemL, TexReg2AR, TexReg2GB, TexM3x2Pad, TexM3x2Tex, TexM3x3Pad, TexM3x3Tex,
    Reserved0, TexM3x3Spec, TexM3x3VSpec, ExpP, LogP, Cnd, Def, TexReg2RGB, TexDp3Tex, TexM3x2Depth, TexDp3, TexM3x3,
    TexDepth, Cmp, Bem, Dp2Add, Dsx, Dsy, TexLdd, Setp, TexLdl, Breakp,
    Phase = 0xFFFD, Comment = 0xFFFE, End = 0xFFFF,
};

enum class RegType : std::uint8_t {
    Temp = 0, Input = 1, Const = 2, Addr = 3 /* vs: a0, ps: t# (Texture) */, RastOut = 4, AttrOut = 5,
    Output = 6 /* vs < 3: oT#, vs 3: o# */, ConstInt = 7, ColorOut = 8, DepthOut = 9, Sampler = 10, Const2 = 11,
    Const3 = 12, Const4 = 13, ConstBool = 14, Loop = 15, TempFloat16 = 16, MiscType = 17, Label = 18, Predicate = 19,
};

enum class SrcMod : std::uint8_t {
    None = 0, Neg, Bias, BiasNeg, Sign, SignNeg, Comp, X2, X2Neg, Dz, Dw, Abs, AbsNeg, Not,
};

enum class Usage : std::uint8_t {
    Position = 0, BlendWeight, BlendIndices, Normal, PSize, TexCoord, Tangent, Binormal, TessFactor, PositionT, Color,
    Fog, Depth, Sample,
};

enum class TextureType : std::uint8_t { Unknown = 0, Tex2D = 2, Cube = 3, Volume = 4 };

// ifc / breakc / setp comparison (instruction token bits 16..18)
enum class Cmp : std::uint8_t { None = 0, Gt = 1, Eq = 2, Ge = 3, Lt = 4, Ne = 5, Le = 6 };

struct Register {
    RegType type = RegType::Temp;
    std::uint32_t index = 0;
};

struct RelAddr {
    bool present = false;
    Register reg;              // a0 or aL
    std::uint8_t component = 0;  // which component of the address register (0..3)
};

struct SrcParam {
    Register reg;
    std::array<std::uint8_t, 4> swizzle{0, 1, 2, 3};
    SrcMod mod = SrcMod::None;
    RelAddr rel;
};

struct DstParam {
    Register reg;
    std::uint8_t writeMask = 0xF;  // bit 0 = x
    bool saturate = false;
    bool partialPrecision = false;
    bool centroid = false;
    std::int8_t shift = 0;  // SM1 result shift (-8..7 encoded in 4 bits)
    RelAddr rel;            // vs_3_0 output o[aL + n]
};

struct Instruction {
    Op op = Op::Nop;
    std::uint32_t control = 0;  // bits 16..23 of the instruction token (comparison, texld flags)
    bool predicated = false;
    bool coissue = false;
    SrcParam predicate;          // when predicated: p0 with swizzle (and Not modifier)
    bool hasDst = false;
    DstParam dst;
    std::vector<SrcParam> src;
    std::array<std::uint32_t, 4> imm{};  // def / defi / defb immediates (raw bits)
    std::size_t tokenOffset = 0;         // position in the token stream (for diagnostics)

    // texld flavours (Op::Tex in SM2+): control bit 0 = project (texldp), bit 1 = bias (texldb)
    bool texProject() const { return (control & 1) != 0; }
    bool texBias() const { return (control & 2) != 0; }
    Cmp comparison() const { return static_cast<Cmp>(control & 7); }
};

// dcl_* for an input/output register (SM3 semantics; SM2 inputs too)
struct Declaration {
    Register reg;
    Usage usage = Usage::Position;
    std::uint8_t usageIndex = 0;
    std::uint8_t mask = 0xF;
    bool centroid = false;
};

struct SamplerDecl {
    std::uint32_t index = 0;
    TextureType type = TextureType::Unknown;
};

struct Program {
    Version version;
    std::vector<Instruction> instructions;  // without dcl/def/comment/end
    std::vector<Declaration> inputs;
    std::vector<Declaration> outputs;       // vs_3_0 o# declarations
    std::vector<SamplerDecl> samplers;
    std::vector<Instruction> defs;          // def / defi / defb
};

struct DecodeResult {
    Program program;
    std::string error;  // empty on success
    bool ok() const { return error.empty(); }
};

DecodeResult decode(std::span<const std::uint32_t> tokens);

// D3D-assembler-like listing (for debugging and golden tests)
std::string disassemble(const Program& program);
const char* opName(Op op);

}  // namespace rvk::shader
