#pragma once

// fx_2_0 effect binaries (compiled D3DX effects): parser, constant tables, preshader evaluator. See docs/research/fx2.md.
//
// The engine compiles its post-processing .fx files with D3DX and hands the binary (magic 0xFEFF0901) to
// eCAPIDevice::CreateEffect; our eCAPIEffect runs them without D3DX. This module turns the binary into a data model:
// parameters (types, defaults, annotations, sampler states), techniques -> passes -> state assignments, embedded SM2/SM3
// shaders with their constant tables (CTAB) and preshaders (FXLC programs that compute shader constants or state values
// from parameters on the CPU). Names and enum values follow the D3DX SDK (D3DXPARAMETER_CLASS/TYPE, D3DXREGISTER_SET).

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "shader/sm_tokens.h"

namespace rvk::fx {

inline constexpr std::uint32_t kEffectMagic = 0xFEFF0901u;  // fx_2_0
inline constexpr std::uint32_t kNone = 0xFFFFFFFFu;

// ------------------------------------------------------------------------------------------------------------------
// types (D3DXPARAMETER_CLASS / D3DXPARAMETER_TYPE)
// ------------------------------------------------------------------------------------------------------------------

enum class ParamClass : std::uint32_t { Scalar = 0, Vector, MatrixRows, MatrixColumns, Object, Struct };

enum class ParamType : std::uint32_t {
    Void = 0, Bool, Int, Float, String, Texture, Texture1D, Texture2D, Texture3D, TextureCube, Sampler, Sampler1D,
    Sampler2D, Sampler3D, SamplerCube, PixelShader, VertexShader, PixelFragment, VertexFragment, Unsupported,
};

const char* className(ParamClass c);
const char* typeName(ParamType t);
bool isSampler(ParamType t);
bool isTexture(ParamType t);
bool isShader(ParamType t);
bool isNumeric(ParamType t);  // Bool / Int / Float

// D3DX_PARAMETER_* flags of a parameter
inline constexpr std::uint32_t kParamShared = 1;      // `shared` (effect pools)
inline constexpr std::uint32_t kParamLiteral = 2;     // `literal`
inline constexpr std::uint32_t kParamAnnotation = 4;  // set on annotations

// ------------------------------------------------------------------------------------------------------------------
// state table: the effect's state ids (index into this table) -> D3D9 state (Wine d3dx9 effect.c order, 0x00..0xB2)
// ------------------------------------------------------------------------------------------------------------------

enum class StateClass : std::uint8_t {
    LightEnable, Fvf, Light, Material, NPatchMode, PixelShader, RenderState, SetSampler, SamplerState, Texture,
    TextureStage, Transform, VertexShader, ShaderConst,
};
enum class MaterialOp : std::uint32_t { Diffuse, Ambient, Specular, Emissive, Power };
enum class LightOp : std::uint32_t {
    Type, Diffuse, Specular, Ambient, Position, Direction, Range, FallOff, Attenuation0, Attenuation1, Attenuation2,
    Theta, Phi,
};
enum class ShaderConstOp : std::uint32_t { VsFloat, VsBool, VsInt, PsFloat, PsBool, PsInt };

struct StateInfo {
    StateClass cls;
    // RenderState: D3DRENDERSTATETYPE, SamplerState: D3DSAMPLERSTATETYPE, TextureStage: D3DTEXTURESTAGESTATETYPE,
    // Transform: D3DTRANSFORMSTATETYPE (D3DTS_WORLD = 256; plus State::index), Material/Light/ShaderConst: the ops above
    std::uint32_t op;
    const char* name;  // "D3DRS_ZENABLE", "AddressU", "Vertexshader", ...
};
std::span<const StateInfo> stateTable();
const StateInfo* stateInfo(std::uint32_t stateId);  // nullptr when out of range
// Symbolic name of a state value where the D3D9 enum is known ("D3DCULL_NONE", "D3DTEXF_LINEAR"), else empty.
std::string stateValueName(std::uint32_t stateId, std::uint32_t value);

// ------------------------------------------------------------------------------------------------------------------
// constant tables (CTAB comment of SM2/3 shaders and of preshaders)
// ------------------------------------------------------------------------------------------------------------------

enum class RegisterSet : std::uint16_t { Bool = 0, Int4 = 1, Float4 = 2, Sampler = 3 };
const char* registerSetName(RegisterSet s);

// One constant as D3DXCONSTANT_DESC describes it. Top-level constants come from the table; `members` are the array
// elements (elements > 1) or struct members with the register ranges D3DX derives for them (GetConstantElement /
// GetConstant(parent, i)).
struct Constant {
    std::string name;
    RegisterSet registerSet = RegisterSet::Float4;
    std::uint32_t registerIndex = 0;
    std::uint32_t registerCount = 0;  // top-level Int4: the raw table value (D3DX quirk), else registers
    std::uint16_t reserved = 0;       // D3DXSHADER_CONSTANTINFO::Reserved
    ParamClass cls = ParamClass::Scalar;
    ParamType type = ParamType::Float;
    std::uint32_t rows = 0, columns = 0;
    std::uint32_t elements = 1;  // 1 = not an array (D3DX reports 1)
    std::uint32_t structMembers = 0;
    std::uint32_t bytes = 0;     // 4 * elements * rows * columns (D3DX's Bytes)
    std::vector<std::uint32_t> defaultValue;  // register-layout default (registerCount * 4 dwords, Bool: count), or empty
    std::vector<Constant> members;
};

struct ConstantTable {
    bool present = false;  // the shader has a CTAB comment
    std::string creator;
    std::string target;    // "vs_3_0", "ps_2_0", "fx_2_0" (preshaders), ...
    std::uint32_t version = 0;
    std::uint32_t flags = 0;
    std::vector<Constant> constants;

    const Constant* find(std::string_view name) const;
};

struct ConstantTableResult {
    ConstantTable table;
    std::string error;  // empty on success (also when there is no CTAB: table.present = false)
    bool ok() const { return error.empty(); }
};

// Parses the CTAB comment of any SM1-3 shader (token 0 = version token) or of a preshader blob.
ConstantTableResult parseConstantTable(std::span<const std::uint32_t> shaderTokens);
// The CTAB comment body (after the 'CTAB' fourcc), e.g. for tools.
ConstantTableResult parseConstantTableData(std::span<const std::byte> ctab);

// Finds a comment block with the given fourcc ('CTAB' = 0x42415443, 'PRES', 'CLIT', 'FXLC', 'PRSI') in the comment
// tokens directly following token 0. Returns the body after the fourcc (empty span if not found).
std::span<const std::uint32_t> findComment(std::span<const std::uint32_t> tokens, std::uint32_t fourcc);

inline constexpr std::uint32_t fourcc(char a, char b, char c, char d) {
    return std::uint32_t(std::uint8_t(a)) | std::uint32_t(std::uint8_t(b)) << 8 | std::uint32_t(std::uint8_t(c)) << 16 |
           std::uint32_t(std::uint8_t(d)) << 24;
}

// ------------------------------------------------------------------------------------------------------------------
// preshaders (FXLC): CPU programs that compute shader constants ("PRES" comment in a shader) or a state value / array
// index (expression blobs of state assignments)
// ------------------------------------------------------------------------------------------------------------------

enum class PresOp : std::uint16_t {
    Nop = 0x000, Mov = 0x100, Neg = 0x101, Rcp = 0x103, Frc = 0x104, Exp = 0x105, Log = 0x106, Rsq = 0x107,
    Sin = 0x108, Cos = 0x109, Asin = 0x10a, Acos = 0x10b, Atan = 0x10c, Sqrt = 0x112, Ineg = 0x120, Not = 0x121,
    Itof = 0x130, Utof = 0x131, Ftou = 0x133, Ftob = 0x137, Floor = 0x139, Ceil = 0x13a, Min = 0x200, Max = 0x201,
    Lt = 0x202, Ge = 0x203, Add = 0x204, Mul = 0x205, Atan2 = 0x206, Div = 0x208, Cmp = 0x300, Movc = 0x301,
    Dot = 0x500, Noise = 0x502, DotSwiz = 0x70e,
};
const char* presOpName(PresOp op);

// Register tables of the preshader VM; offsets count components (c3.y = 3 * 4 + 1).
enum class PresTable : std::uint8_t {
    None = 0,
    Literal = 1,   // CLIT (doubles)
    Input = 2,     // c#: parameters, mapped by the preshader's CTAB
    VInput = 3,    // v#: tx_1_0 texture-shader inputs (unused by effects)
    Output = 4,    // shader float constants (c#) / the expression result
    OutBool = 5,   // shader bool constants (offsets count registers, not components)
    OutInt = 6,    // shader int constants
    Temp = 7,      // r#
};
const char* presTableName(PresTable t);

struct PresRegister {
    PresTable table = PresTable::None;
    std::uint32_t offset = 0;  // component offset (OutBool: register)
};

struct PresOperand {
    PresRegister reg;
    bool relative = false;
    PresRegister index;  // relative addressing: the register holding the index (in registers)
};

struct PresInstruction {
    PresOp op = PresOp::Nop;
    std::uint32_t raw = 0;          // instruction token
    std::uint32_t components = 1;   // 1..4
    bool scalar = false;            // first input is a scalar, broadcast to all components
    std::vector<PresOperand> inputs;
    PresOperand output;
};

struct Preshader {
    std::vector<std::uint32_t> tokens;  // the raw blob (version token + comments)
    std::uint32_t version = 0;      // 0x46580200 ('FX' 2.0) or 0x54580100 (tx_1_0)
    std::vector<double> literals;   // CLIT
    std::vector<PresInstruction> code;
    ConstantTable inputs;           // CTAB: which parameter feeds which c# register
    std::vector<std::uint32_t> prsi;  // PRSI comment body (raw; output register mapping, see fx2.md)
    std::uint32_t tempRegisters = 0;
    std::uint32_t outputRegisters = 0;  // float4 output registers written (max index + 1)
    std::uint32_t outBoolRegisters = 0, outIntRegisters = 0;
    std::uint32_t inputRegisters = 0;   // c# registers read (max index + 1)
};

struct PreshaderResult {
    Preshader preshader;
    std::string error;
    bool ok() const { return error.empty(); }
};

// Parses a preshader blob: version token followed by the CLIT / FXLC / CTAB / PRSI comments.
PreshaderResult parsePreshader(std::span<const std::uint32_t> tokens);

// Register file of the VM (inputs and results). Float4 registers are 4 floats each.
struct PresRegisters {
    std::vector<float> input;     // c#   (fill with fillConstants() from the preshader's CTAB)
    std::vector<float> output;    // oc#  (shader float constants / expression value)
    std::vector<std::int32_t> outInt;   // 4 per register
    std::vector<std::uint32_t> outBool; // 1 per register
    std::vector<float> vinput;    // v# (tx_1_0 only)
};
// Runs the program. Missing registers read as 0; the vectors grow as needed.
// Computes in double precision like D3DX (temps stored as float).
void runPreshader(const Preshader& p, PresRegisters& regs);

// Disassembly ("mul r0.x, c1.x, (2)") for dumps.
std::string disassemblePreshader(const Preshader& p);

// ------------------------------------------------------------------------------------------------------------------
// effect
// ------------------------------------------------------------------------------------------------------------------

struct State;

// Parameters, annotations and state values share this description (Wine's d3dx_parameter).
struct Parameter {
    std::string name;
    std::string semantic;
    ParamClass cls = ParamClass::Scalar;
    ParamType type = ParamType::Void;
    std::uint32_t rows = 0, columns = 0;
    std::uint32_t elements = 0;  // array size, 0 = not an array
    std::uint32_t flags = 0;     // kParam*
    std::vector<Parameter> members;      // array elements (elements > 0) or struct members
    std::vector<Parameter> annotations;  // top-level parameters, techniques, passes

    // value
    std::uint32_t dataOffset = 0;  // numeric data of this node: bytes [dataOffset, dataOffset + dataSize) of root `data`
    std::uint32_t dataSize = 0;
    std::uint32_t objectId = kNone;    // string / texture / shader leaf: index into Effect::objects
    std::vector<State> samplerStates;  // sampler leaf: the sampler_state { ... } assignments
    std::vector<std::byte> data;       // root (top-level parameter, annotation, state value): numeric default value

    bool isArray() const { return elements > 0; }
    std::uint32_t leafCount() const;  // rows * columns of one element
    // numeric helpers on the root data
    float floatAt(std::size_t i) const;  // i-th 32-bit component converted from the parameter's type
    std::uint32_t dwordAt(std::size_t i) const;
};

enum class StateKind : std::uint8_t {
    Constant,       // value holds the constant (or object id: shader / texture / string)
    ParamRef,       // `State = <param>` / `State = param`: reference by name
    Expression,     // FXLC program computes the value (`AlphaRef = g_f * 255`)
    ArraySelector,  // `VertexShader = g_vs[g_index]`: expression computes the index into an array parameter
};
const char* stateKindName(StateKind k);

// State::index of the assignments inside sampler_state { } (D3DX writes 256 there; the runtime applies them to the
// sampler register the shader's CTAB assigns to the sampler parameter).
inline constexpr std::uint32_t kSamplerStateIndex = 256;

struct State {
    std::uint32_t id = 0;     // index into stateTable()
    std::uint32_t index = 0;  // sampler / stage / light / transform / register index (kSamplerStateIndex in samplers)
    Parameter value;          // the value's type, and its constant value (Constant)
    StateKind kind = StateKind::Constant;
    std::string reference;           // ParamRef / ArraySelector: referenced parameter (full name, e.g. "tex" or "a[1]")
    std::uint32_t referenceParam = kNone;  // index of the referenced top-level parameter
    std::uint32_t expression = kNone;      // Expression / ArraySelector: index into Effect::preshaders
    std::uint32_t shader = kNone;          // Vertex/PixelShader constant: index into Effect::shaders

    const StateInfo* info() const { return stateInfo(id); }
};

struct Pass {
    std::string name;
    std::vector<Parameter> annotations;
    std::vector<State> states;
};

struct Technique {
    std::string name;
    std::vector<Parameter> annotations;
    std::vector<Pass> passes;
};

struct Shader {
    shader::Stage stage = shader::Stage::Vertex;
    int major = 0, minor = 0;
    std::uint32_t objectId = kNone;
    std::vector<std::uint32_t> tokens;  // whole bytecode (version token .. END)
    ConstantTable constants;            // CTAB
    std::uint32_t preshader = kNone;    // index into Effect::preshaders ("PRES" comment)
};

struct Object {
    ParamType type = ParamType::Void;  // type of the parameter / state that owns the id
    std::vector<std::byte> data;       // string bytes (with NUL), shader bytecode, expression blob
    std::uint32_t shader = kNone;      // index into Effect::shaders
    std::string_view string() const;   // ParamType::String
};

struct Effect {
    std::vector<Parameter> parameters;
    std::vector<Technique> techniques;
    std::vector<Object> objects;
    std::vector<Shader> shaders;
    std::vector<Preshader> preshaders;
    std::uint32_t shaderCount = 0;  // header field after the technique count (number of shader slots)

    // "name", "name[2]", "name.member", "name@annotation" (D3DX full names); nullptr if not found
    const Parameter* findParameter(std::string_view fullName) const;
    std::uint32_t parameterIndex(std::string_view name) const;  // top-level, kNone if not found
    const Technique* findTechnique(std::string_view name) const;
};

struct EffectResult {
    Effect effect;
    std::string error;  // empty on success
    bool ok() const { return error.empty(); }
};

// True if the data starts with the fx_2_0 magic. (The engine also passes HLSL source text to CreateEffect, which
// D3DXCreateEffect accepts as well: compile it with the D3DX effect compiler first.)
bool isEffectBinary(std::span<const std::byte> data);
EffectResult parseEffect(std::span<const std::byte> data);

// Human-readable dump (parameters, techniques, passes, states, shaders with CTAB, preshaders).
struct DumpOptions {
    bool shaderListings = false;  // include the decoded shader listings
    bool preshaderCode = true;
};
std::string dumpEffect(const Effect& e, const DumpOptions& options = {});
std::string dumpConstantTable(const ConstantTable& t);

// ------------------------------------------------------------------------------------------------------------------
// runtime helpers: parameter values -> constant registers
// ------------------------------------------------------------------------------------------------------------------

// Current values of the top-level numeric parameters (index = Effect::parameters index), initialised from defaults.
struct ParameterValues {
    std::vector<std::vector<std::byte>> data;
    static ParameterValues defaults(const Effect& e);
    void setFloats(const Effect& e, std::string_view name, std::span<const float> v);  // converts to the param's type
};

// Register file of one shader stage.
struct ConstantRegisters {
    std::vector<float> f;            // 4 per float4 register
    std::vector<std::int32_t> i;     // 4 per int4 register
    std::vector<std::uint32_t> b;    // 1 per bool register
};

// Writes `param`'s value into the registers that `c` describes (D3DX rules: matrix transpose between row- and
// column-major, one register per element / matrix row or column, bool/int/float conversion, register count
// truncation). `value` is the data of the top-level parameter (ParameterValues::data[i]); `param` is that parameter or
// one of its elements/members (its dataOffset indexes `value`). Returns false if the shapes are incompatible.
bool setConstant(const Constant& c, const Parameter& param, std::span<const std::byte> value, ConstantRegisters& regs);

// Fills every constant of `table` whose name matches a top-level parameter. Unknown names are skipped.
void fillConstants(const ConstantTable& table, const Effect& e, const ParameterValues& values, ConstantRegisters& regs);

// All constants of a shader: CTAB constants from the parameters, then its preshader (if any) evaluated into the
// float/int/bool registers it writes.
ConstantRegisters evaluateShaderConstants(const Effect& e, const Shader& s, const ParameterValues& values);

// Value of an Expression state (or the index of an ArraySelector) as 32-bit components of the state value's type
// (float bits / int / BOOL), D3DX conversion rules.
std::vector<std::uint32_t> evaluateExpression(const Effect& e, const Preshader& p, const ParameterValues& values,
                                              ParamType resultType, std::uint32_t resultComponents);

}  // namespace rvk::fx
