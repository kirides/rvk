# fx_2_0 effect binaries (D3DX effects) as Risen uses them

Our `eCAPIEffect` runs the game's D3DX effects without D3DX. The engine compiles its post-processing `.fx` files
(`data/common/effects.pak`) through the HAL (`eCAPIDevice::CompileEffectFromFile`, D3DX effect compiler) and hands the
result to `eCAPIDevice::CreateEffect(data, size)`. This note documents the binary format as implemented and verified in
`src/fx/effect_binary.{h,cpp}` (namespace `rvk::fx`), the constant tables (CTAB) and preshaders (FXLC) inside it, and what
the game's effects actually use.

Sources of format knowledge: Wine's d3dx9 (`dlls/d3dx9_36/effect.c`, `preshader.c`, `shader.c`) and vkd3d-shader
(`libs/vkd3d-shader/fx.c`), read for the format only; the implementation is our own. Everything below marked
**[verified]** was checked against the 35 captured effects and against d3dx9_39 (see section 8).

Tools: `rvk_fxtool dump <file> [--listing]`, `rvk_fxtool corpus <dir>` (parse + decode + CTAB cross-check + statistics),
`rvk_fxtool compile <file.fx> <out.bin> [DEFINE=1 ...]` (D3DX effect compiler, CPU only). Tests: `tests/fx_tests.cpp`.

## 1. Container [verified]

All integers little-endian, 32 bit. Offsets inside the effect are relative to the byte after the 8-byte header ("base").

| offset | content |
|---|---|
| 0 | magic `0xFEFF0901` (fx_2_0) |
| 4 | `U` = size of the unstructured area |
| 8 | unstructured area (`U` bytes): names, type descriptions, default values; referenced by offset from base. Offset 0 holds a 0 dword |
| 8 + U | structured part, read sequentially (below) |

Structured part:

```
u32 parameterCount, techniqueCount, shaderCount, objectCount
Parameter[parameterCount]
Technique[techniqueCount]
u32 objectDataCount, resourceCount
ObjectData[objectDataCount]       u32 objectId, u32 size, bytes[size] padded to 4
Resource[resourceCount]           u32 technique, u32 index, u32 element, u32 state, u32 usage, u32 size, bytes padded to 4
```

`shaderCount` is not needed for parsing (Wine: shader variables + one slot per pass; wobble: 4). Objects are numbered
`0 .. objectCount-1`; the values of strings, textures and shaders are object ids (object 0 is unused in practice).

### Names (unstructured area)

`u32 length` (including the terminating NUL), then the characters padded to 4 bytes. Offset 0 / length 0 = no name.

### Type description ("typedef")

```
u32 type          D3DXPARAMETER_TYPE (0 void, 1 bool, 2 int, 3 float, 4 string, 5..9 texture/1D/2D/3D/CUBE,
                  10..14 sampler/1D/2D/3D/CUBE, 15 pixelshader, 16 vertexshader, ...)
u32 class         D3DXPARAMETER_CLASS (0 scalar, 1 vector, 2 matrix_rows, 3 matrix_columns, 4 object, 5 struct)
u32 nameOffset, semanticOffset
u32 elements      array size, 0 = no array
vector:           u32 columns, u32 rows        (note the order)
scalar / matrix:  u32 rows, u32 columns
struct:           u32 memberCount, then memberCount member typedefs (recursively, inline)
object:           nothing more
```

Arrays: every element has the parent's type; for arrays of structs each element re-reads the same member typedefs.
Effect parameters of `float4x4` are `MATRIX_ROWS` (data row-major) even when the shaders use them column-major.

### Value

Read sequentially at the value offset, in declaration order of the leaves:
numeric leaf `rows * columns` dwords (bool = 0/1, int, float bits); string / texture / shader leaf `u32 objectId`;
sampler leaf `u32 stateCount` + `stateCount` state assignments (below). Parameters without an initializer still have a
zero default. (For structs mixing numbers and objects Wine reads object ids from the start of the value; our parser uses
one running cursor. Not used by the game.)

### Parameter, annotation, technique, pass, state

```
Parameter   u32 typedefOffset, u32 valueOffset, u32 flags (1 shared, 2 literal), u32 annotationCount, Annotation[...]
Annotation  u32 typedefOffset, u32 valueOffset
Technique   u32 nameOffset, u32 annotationCount, u32 passCount, Annotation[...], Pass[passCount]
Pass        u32 nameOffset, u32 annotationCount, u32 stateCount, Annotation[...], State[stateCount]
State       u32 stateId (index into the state table, section 2), u32 index, u32 typedefOffset, u32 valueOffset
```

`State::index` is the stage / sampler / light / register index (`Sampler[1] = <s>` -> 1, `Texture[2]` -> 2). Assignments
inside `sampler_state { }` carry index **256** (`kSamplerStateIndex`) in every effect seen: the runtime applies them to
the sampler register that the shader's CTAB assigns to the sampler parameter. A state's value is constant unless a
resource (below) replaces it.

### Object data and resources

`ObjectData` fills objects with inline data: string contents (with NUL) and the shaders of shader *variables*
(`VertexShader g_vs[2] = { compile ... }`). `Resource` entries carry the "complex" state values; they address a state by
`technique, pass(index), state` or, with `technique = 0xFFFFFFFF`, by `parameter, element (0xFFFFFFFF = none), state`
inside a sampler:

| usage | meaning | blob |
|---|---|---|
| 0 | state value is a shader (`VertexShader = compile vs_3_0 VS()`): the blob is the bytecode of the state's object | SM2/3 bytecode |
| 0 | state value of type bool/int/float/string: an **expression** (`AlphaRef = (int)(g_a * 255)`, `AlphaTestEnable = <g_flag>`) | preshader, version `0x46580200` |
| 1 | parameter reference (`Texture = <SrcTexture>`) | parameter name, NUL-terminated |
| 2 | **array selector** (`VertexShader = (g_vs[g_index])`) | `u32 nameSize`, name, preshader computing the index |

## 2. State ids [verified for the ids the game uses]

The state id is an index into a fixed table (Wine's `state_table`, 179 entries, `rvk::fx::stateTable()` gives class + D3D9
enum value + name):

| ids | class | D3D9 enum |
|---|---|---|
| 0x00-0x66 | render states (ZENABLE .. BLENDOPALPHA, in Wine's order, not D3DRS order) | D3DRENDERSTATETYPE |
| 0x67-0x78 | texture stage states (COLOROP .. CONSTANT) | D3DTEXTURESTAGESTATETYPE |
| 0x79 NPatchMode, 0x7a FVF, 0x7b-0x7e transforms (PROJECTION, VIEW, WORLD, TEXTURE0; + index) | | D3DTS |
| 0x7f-0x83 material, 0x84-0x90 light (+ index), 0x91 LightEnable | | |
| 0x92 VertexShader, 0x93 PixelShader | | |
| 0x94-0xa3 shader constants (`VertexShaderConstantF[i]` ...) | | |
| 0xa4 Texture, 0xa5-0xb1 sampler states (AddressU .. DMAPOffset), 0xb2 Sampler | | D3DSAMPLERSTATETYPE |

Ids used by the 35 captured effects (assignments / effects; values seen):

| id | state | count | effects | values |
|---|---|---|---|---|
| 0x00 | ZENABLE | 209 | 30 | FALSE |
| 0x01 | FILLMODE | 273 | 34 | SOLID |
| 0x03 | ZWRITEENABLE | 128 | 24 | FALSE |
| 0x04 | ALPHATESTENABLE | 276 | 31 | FALSE |
| 0x06 / 0x07 | SRCBLEND / DESTBLEND | 40 | 13 | ONE, SRCALPHA, ZERO / INVSRCALPHA, ONE, SRCCOLOR, ZERO |
| 0x08 | CULLMODE | 205 | 29 | NONE |
| 0x0a / 0x0b | ALPHAREF / ALPHAFUNC | 1 | 1 | 0 / GREATEREQUAL (sprite effect) |
| 0x0d | ALPHABLENDENABLE | 280 | 34 | FALSE, TRUE |
| 0x0e | FOGENABLE | 274 | 35 | FALSE |
| 0x16-0x1d | STENCILENABLE, FAIL, ZFAIL, PASS, FUNC, REF, MASK, WRITEMASK | 20 each | 5 (post_SSM) | TRUE; INCR/ZERO; KEEP/ZERO; INCR/ZERO; EQUAL; 0; 0xFFFFFFFF; 0xFFFFFFFF |
| 0x49 | COLORWRITEENABLE | 279 | 33 | 0, A (0x8), RGBA |
| 0x92 / 0x93 | VertexShader / PixelShader | 284 each | 35 | always a constant shader (no NULL, no arrays) |
| 0xa4 | Texture (in sampler_state) | 133 | 35 | always `<parameter>` |
| 0xa5-0xa7 | AddressU/V/W | 133/133/132 | 35 | CLAMP, WRAP |
| 0xa9-0xab | MagFilter / MinFilter / MipFilter | 133 | 35 | POINT, LINEAR / NONE, LINEAR |

Not captured but in effects.pak (compiled with `rvk_fxtool compile`, all parse and cross-check): `pre_lightmap.fx` adds
BLENDOP (REVSUBTRACT), SEPARATEALPHABLENDENABLE, SRCBLENDALPHA, DESTBLENDALPHA and bool shader constants; `post_sky`,
`post_desaturate`, `post_distort` add nothing new. `editor_copy.fx` uses ps_1_x, which d3dx9_39 refuses without the legacy
compiler flag (editor only).

## 3. Shaders [verified]

Shader blobs are D3D9 SM2/SM3 bytecode (`0xFFFE0200` .. `0xFFFF0300`) ending with the END token; comments right after
the version token carry `CTAB` (constant table) and, when uniform-only expressions were hoisted, `PRES` (preshader).
`rvk::shader::decode` decodes all 568 corpus shaders.

### CTAB (`D3DXSHADER_CONSTANTTABLE`)

Offsets relative to the CTAB body (after the `'CTAB'` fourcc):

```
header (28)       u32 size = 28, creatorOffset, version (shader version token), constantCount, constantInfoOffset,
                  flags, targetOffset
constant (20)     u32 nameOffset, u16 registerSet (0 bool, 1 int4, 2 float4, 3 sampler), u16 registerIndex,
                  u16 registerCount, u16 reserved, u32 typeInfoOffset, u32 defaultValueOffset (0 = none)
typeinfo (16)     u16 class, type, rows, columns, elements, structMembers, u32 structMemberInfoOffset
member (8)        u32 nameOffset, u32 typeInfoOffset
```

D3DX derives descriptions for array elements and struct members (`GetConstantElement`, `GetConstant(parent, i)`);
`parseConstantTable` reproduces them, including the quirks found by the cross-check:

* registers per leaf in float4/int4: vector 1, scalar 1, matrix_rows `rows`, matrix_columns `columns`; bool:
  `rows * columns`; sampler 1. Element/member `i` starts at `parentIndex + untruncated size of 0..i-1` and its count is
  clipped to the parent's range (a preshader that reads only `g_arr[0]` gets `g_arr` = c2 count 1, elements at
  c2 (1), c3 (0), **c4** (0); Wine accumulates the clipped counts and gets c3 for the last one).
* top-level int4 constants report the raw table count.
* default values are in register layout (each element/row padded to 4 components); D3DX hands out a DefaultValue pointer
  for struct members even when the constant has no default (running offset from 0 into the CTAB): we report none.

### Constant upload rules (`setConstant`, `fillConstants`, `evaluateShaderConstants`)

Effect parameter -> shader registers like D3DX: matrices are transposed when parameter and constant majorness differ
(effect `float4x4` = MATRIX_ROWS, shader default = MATRIX_COLUMNS -> register `i` = column `i`); one register per array
element / matrix row or column; bool/int parameters converted to float (1.0 / value) for float4 registers, floats truncated
for int4; the register count from the CTAB truncates. Samplers have no register data: sampler `s#` from the CTAB + the
parameter's sampler states + the texture parameter its `Texture` state references.

## 4. Preshaders (FXLC) [verified]

A preshader blob is a token stream: version token (`0x46580201` in shader `PRES` comments, `0x46580200` for state
expressions, `0x54580100` = tx_1_0 texture shaders), comments `CLIT`, `FXLC`, `PRSI`, `CTAB`, then END.

* `CLIT`: `u32 count`, `count` **doubles** (literal table, count is a multiple of 4).
* `CTAB`: inputs; maps parameter names to the preshader's `c#` input registers (float4; same layout as above).
* `FXLC`: `u32 instructionCount`, then per instruction:
  `u32 token` = `scalar << 31 | opcode << 20 | componentCount (1..4)`, `u32 inputCount`, then `inputCount + 1` operands
  (inputs, then the output). Operand: `u32 indexCount (0/1)`, [`u32 table, u32 offset` of the index register], `u32 table,
  u32 offset`. Offsets count components (`c3.y` = 13); for the bool output table they count registers (stored * 4).
  Tables: 1 literal (CLIT), 2 `c#` inputs, 3 `v#` (tx only), 4 `oc#` float outputs = the shader's float constants
  (same register number), 5 bool outputs, 6 int outputs, 7 `r#` temps.
  `scalar` = the first input is one component broadcast to all.
* `PRSI` (not needed to run; Wine ignores it): 11 dwords in every corpus preshader, e.g. `5 0 0 1 0 0 1 5 1 0 0` for a
  preshader writing oc5: `[0]` first output register, `[3]` output register count, `[6]` number of ranges, then
  (`shader register`, `count`) per range, two trailing zeros. Inferred: output register == shader register in all
  342 corpus preshaders (identity mapping, as Wine assumes).

Execution (`runPreshader`): in order, double precision, temps stored as float, outputs as float/int (lrint)/bool.
Opcodes (component-wise unless noted; native semantics as established by Wine's tests):

| op | code | semantics |
|---|---|---|
| mov, neg | 0x100, 0x101 | |
| rcp | 0x103 | 1/x |
| frc | 0x104 | x - floor(x) |
| exp, log | 0x105, 0x106 | 2^x; log2(abs(x)), log(0) = 0 |
| rsq | 0x107 | 1/sqrt(abs(x)), rsq(0) = +inf |
| sin, cos, asin, acos, atan | 0x108-0x10c | (asin/acos NaN sign-flipped like native) |
| sqrt, floor, ceil | 0x112, 0x139, 0x13a | (fx_4 era, accepted) |
| min, max, lt, ge, add, mul, atan2 | 0x200-0x206 | lt/ge give 1/0 |
| div | 0x208 | always 0 in native d3dx9 (the compiler emits rcp + mul) |
| cmp | 0x300 | a >= 0 ? b : c |
| movc | 0x301 | a != 0 ? b : c |
| dot | 0x500 | one result from all components |
| d3ds_dotswiz | 0x70e | 6 or 8 scalar inputs: dot of the two halves |

Integer opcodes (0x120-0x137, 0x210-0x236) and noise are rejected by the parser (fx_4 only / not used).

Relative addressing: index = lrint(index register); out-of-range `c#` wraps at the next power of two of the table size
(native behaviour per Wine), other tables at their size.

Corpus preshaders: **342**, all in shaders (`PRES`), none in states; 1909 instructions, at most 29 per preshader, at
most 3 temps, only float4 outputs, no relative addressing. Opcodes used: rcp 747, mul 391, mul (scalar) 374, mov 293,
add (scalar) 59, neg 32, exp 11, cmp 2 (plus rsq in pre_lightmap.fx). Typical content: `0.5 / TextureSize` texel offsets,
`1 - pow(0.98, EyeAdapt * FrameTime)` (exp of a scaled log), `min(FrameRedundancy * FrameTime, MaxOpacity)` (cmp).

## 5. The captured effects

35 captures (`corpus/fx_*.bin`) = 34 compiled binaries + **one HLSL source text**: the engine's sprite effect
(`technique Main`, `g_mWorldViewProj`, `g_texDiffuse : DIFFUSE`) is passed to `CreateEffect` as source, not compiled, so
our `CreateEffect` must accept source too (compile with d3dx9_39's effect compiler on the CPU, then parse;
`isEffectBinary` tells them apart). Binaries are variants of 10 effects.pak files (compiled with different defines):

| source | techniques | binaries |
|---|---|---|
| post_hdr.fx | LogLuminance BrightPass BrightPassAlphaMask DownSample AdaptiveLiminance Copy ToneMap ExpToneMap HDRI ExpHDRI ColorOutput | 11 |
| post_dof.fx | DoFDown DoFBlurCocH DoFBlurCocV DoFNearCoc DoFSmallBlur DoF DebugAlphaChanel | 6 |
| post_SSM.fx | DSSM VSSM SSM SSM_Low PSSM PSSM_Low SSAO SSAO_Low BlurSSAO UpscaleSSAO Blur | 5 |
| overlay_color.fx | TOverlayColor OverlayRadialColor OverlayRadialBlur OverlayImage OverlayMovieStrips OverlayMovieStripsAbs | 4 |
| post_bloom.fx | DownSample GlowH GlowV GlowVAddBloom2 | 2 |
| post_wobble.fx | TWobble | 2 |
| post_blur.fx, post_edgesmooth.fx, post_radialBlur.fx, post_streaks.fx | Blur7H Blur7V Blur7 / EdgeSmooth EdgeSmoothDebug / RadialBlur... / Streak | 1 each |
| (engine, source text) | Main | 1 |

Totals: 263 techniques, 284 passes (1 pass per technique except a few), 568 shaders (vs_2_0 37, ps_2_0 37, vs_3_0 247,
ps_3_0 247), 342 with a preshader. Parameters: 195 float, 163 float2-4, 21 float4x4 (MATRIX_ROWS), 123 textures,
133 samplers (132 `sampler`, 1 `sampler2D`), 18 arrays (`float3 ColMatrix[3] : COLORMATRIX`, `float weights7[7]`); no
structs, no shared parameters, no annotations. Shader CTABs use only float4 (445) and sampler (501) registers.
Semantics the engine binds by: TEXTURE0-3, TEXTURESIZE0-3, FRAMETIME, TIME, SCREENSIZE, NEARFAR, BLENDVALUE,
BLOOMINTENSITY/OFFSET/THRESHOLD, COLORMATRIX, FOCALRANGENEAR/FAR, SUNLIGHTSCREENVECTOR, SUNFLAREINTENSITY, DIFFUSE
(+ IMAGE, INVPROJ, INVVIEW in pre_lightmap.fx).

## 6. What the runtime must support for this game

| feature | needed | count in corpus |
|---|---|---|
| fx_2_0 parsing, parameters by name and semantic, techniques by name, pass index | yes | 35 effects |
| HLSL source passed to CreateEffect (compile on CPU with d3dx9_39, then parse) | yes | 1 (sprite) |
| vs/ps 2_0 and 3_0 shaders per pass (constant, never NULL / array-selected) | yes | 568 |
| preshaders in shaders (float4 outputs, ops rcp mul mov add neg exp cmp; rsq for pre_lightmap) | yes | 342 |
| parameter -> constant upload: float/float2-4/float4x4 (transpose), float arrays | yes | |
| samplers: Texture = <param>, AddressU/V/W, Min/Mag/MipFilter | yes | 133 |
| render states: Z enable/write, fill mode, cull, alpha test/ref/func, blend enable + src/dest (+ blendop, separate alpha for pre_lightmap), fog enable (always FALSE), color write mask, stencil (post_SSM) | yes | see section 2 |
| state expressions, array selectors, parameter-referenced render states | no (supported by the parser) | 0 |
| bool/int shader constants, structs, annotations, shared parameters, fixed-function states | no (pre_lightmap: bool constants) | 0 |

FOGENABLE is always FALSE (post effects disable fixed-function fog). ALPHATESTENABLE is always FALSE except via
the sprite effect where it is FALSE too (ALPHAREF/ALPHAFUNC set but unused).

## 7. API (src/fx/effect_binary.h)

* `parseEffect(bytes) -> EffectResult {Effect, error}`: `Effect{parameters, techniques, objects, shaders, preshaders}`;
  `State{id, index, value, kind (Constant / ParamRef / Expression / ArraySelector), reference, referenceParam,
  expression, shader}`; `findParameter("a[1].b")`, `findTechnique`.
* `parseConstantTable(tokens)` for any SM1-3 shader (also the material shaders); `findComment`.
* `parsePreshader`, `runPreshader`, `disassemblePreshader`.
* `ParameterValues` (current values, from defaults), `setConstant`, `fillConstants`, `evaluateShaderConstants(effect,
  shader, values)` (CTAB uploads + preshader outputs), `evaluateExpression` (state expressions / array indices).
* `stateTable()`, `stateInfo(id)`, `stateValueName(id, value)`; `dumpEffect`, `dumpConstantTable`.

## 8. Verification

* All 35 captures parse (the source capture after compiling it with d3dx9_39); all 14 compilable effects.pak sources
  (compiled without defines) parse as well.
* All 568 embedded shaders decode with `rvk::shader::decode`.
* CTAB: for every embedded shader and every preshader (wrapped into a vs_3_0 token stream) the result of
  `parseConstantTable` was compared field by field with `D3DXGetShaderConstantTable` (d3dx9_39): name, register set,
  index, count, class, type, rows, columns, elements, struct members, bytes, default value bytes, recursively for all
  elements and members via `GetConstantElement` / `GetConstant(h, i)`: 910 tables, 1681 constants, 0 differences
  (`rvk_fxtool corpus corpus`; also in the `fx_corpus` test). `fx_ctab_compile_shader` does the same for a ps_3_0
  compiled in-test with structs, matrix arrays, row/column-major matrices, int, bool and sampler arrays.
* Preshaders: `fx_preshader_ops` runs every implemented opcode (incl. scalar broadcast, relative addressing, int/bool
  outputs) against hand computation; `fx_d3dx_compiled_effect` compiles an effect with sin/cos/exp/log/rsqrt/frac/min/
  max/comparisons/dot/division and checks the preshader outputs against the same HLSL expressions evaluated in C++ for
  two parameter sets, the `AlphaRef = (int)(g_alpha * 255)` expression and the array selector index; `fx_corpus`
  spot-checks the game's preshaders against their HLSL source: wobble `0.5 / SrcTextureSize`, `Speed * FrameTime`
  (post_wobble.fx), HDR eye adaptation `1 - pow(0.98, EyeAdapt * FrameTime)` (11 binaries), bloom
  `min(FrameRedundancy * FrameTime, MaxOpacity)` for both branches.
