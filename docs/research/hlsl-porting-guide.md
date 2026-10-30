# Porting a GLSL shader of this repo to HLSL

Read `hlsl-rhi.md` 2-4 first (the compile scheme and the ABI). Worked examples: `shaders/gbuffer/*.hlsl*`, `shaders/upscale/*.hlsl`,
`shaders/debug/*.hlsl*`. The GLSL original stays in the tree until the whole port is done (`RVK_FORCE_GLSL=1` runs the originals in the game for A/B);
the port is `<name>.hlsl` next to it (`water/water.frag` -> `water/water.frag.hlsl`), includes are `*.hlsli` next to / in `include/`.

## Files and entry points
- `.comp.hlsl` -> `[numthreads(X,Y,Z)] void CSMain(uint3 id : SV_DispatchThreadID, ...)`; `.vert.hlsl` -> `VSMain`; `.frag.hlsl` -> `PSMain`
  (`float4 PSMain(float4 position : SV_Position, float2 uv : TEXCOORD0) : SV_Target0`). Vertex outputs / fragment inputs are matched by
  semantic **in declaration order** (DXC numbers the locations in order): keep the same order on both sides, and the same set as the GLSL
  `layout(location = n)` (location n = the n-th user varying).
- `gl_GlobalInvocationID`=`SV_DispatchThreadID`, `gl_LocalInvocationID`=`SV_GroupThreadID`, `gl_WorkGroupID`=`SV_GroupID`,
  `gl_LocalInvocationIndex`=`SV_GroupIndex`, `gl_VertexIndex`=`SV_VertexID`, `gl_InstanceIndex`=`SV_InstanceID`, `gl_FragCoord`=`SV_Position`
  (same pixel-centre convention), `gl_FrontFacing`=`SV_IsFrontFace`, `discard`=`discard`, `gl_FragDepth`=`SV_Depth`.
- `layout(local_size_x = 8, local_size_y = 8)` -> `[numthreads(8, 8, 1)]`; a specialization constant (`constant_id`) ->
  `[[vk::constant_id(n)]] const uint NAME = default;`.

## ABI (shaders/include/rvk_abi.hlsli, rvk_common.hlsli)
- `#include "rvk_common.hlsli"` (it includes the ABI). Pass push block: `struct XPush { RVK_PASS_PUSH_HEADER <fields> }; RVK_PUSH(XPush, pc);` with the
  field order / types of the GLSL block (a C++ struct mirrors it; offsets are compared by the checker and tests). `RvkFrame f = rvkLoadFrame(pc.frame);`.
- Matrices are the engine's row-vector D3D matrices: GLSL `f.viewProj * vec4(w, 1.0)` is HLSL `mul(float4(w, 1.0), f.viewProj)`; GLSL
  `M[c][r]` is HLSL `M[c][r]` (same spelling, row = first index in HLSL: the stored rows). Matrices of your own (constructed in the shader) follow the
  HLSL rules: `float3x3(r0, r1, r2)` takes rows, `mul(v, M)` row-vector, `mul(M, v)` column-vector: translate each product deliberately.
- Pass resources: `[[vk::image_format("rgba16f")]] RWTexture2D<float4> name : register(u<binding>, space<set>);` with the set / binding of the GLSL
  `layout(set = S, binding = B)` (format = the GLSL format qualifier: r32f, rg16f, rgba8, r32ui ...). Buffers: `RWByteAddressBuffer` / `RWStructuredBuffer<T>` /
  `StructuredBuffer<T>` with `register(u<B>/t<B>, space<S>)` (storage buffer on Vulkan). Binding numbers must stay unique within a set.
- Bindless textures / samplers: only through the helpers (`rvkSample2D / rvkSample2DLod / rvkFetch2D / rvkSize2D` in rvk_common.hlsli; add cube / 3D / gather
  / compare variants there when you need them, using `g_tCube[NonUniformResourceIndex(i)]`, `g_t3D[...]`, `g_samp[...]`). `texelFetch(t, p, lod)` = `.Load(int3(p, lod))`,
  `textureSize` = `GetDimensions`, `textureLod` = `SampleLevel`, `textureGrad` = `SampleGrad`, `textureGather` = `Gather`.
- GPU addresses stored as `(low, high)` uint pairs (GLSL `buffer_reference` of a `uvec2`/`uint64`): `rvkPtr<StructT, alignment>(uint2 addr)` gives a typed pointer
  (`.Get().member`, `.Get().arr[i] = v`, `InterlockedAdd(rvkPtr<S,4>(a).Get().n, 1)`), `vk::RawBufferLoad<T>(rvkAddr(a) + byteOffset)` / `vk::RawBufferStore`
  for raw reads (T: scalar / vector, `alignment` argument defaults to 4). Needs `shaderInt64` (enabled). Define ABI structs of `float4` / `uint4` members only
  (see rvk_abi.hlsli 4.1) so the layout is independent of the packing rule; a GLSL std430 struct with scalars needs explicit padding or `vk::RawBufferLoad` of
  each field.
- Ray queries: the TLAS is `g_tlas[NonUniformResourceIndex(f.rtInfo.w)]` (declared in rvk_rt.hlsli once it exists; Vulkan binding 4 of set 0).
  `rayQueryEXT` -> `RayQuery<flags>`; `rayQueryInitializeEXT` = `q.TraceRayInline(tlas, 0, mask, ray)`; `rayQueryProceedEXT` = `q.Proceed()`; candidate / committed
  accessors are the HLSL ones (`q.CandidateType()`, `q.CandidateTriangleBarycentrics()`, `q.CommittedRayT()`, ...).

## GLSL -> HLSL pitfalls (each one has bitten somebody)
- `mix` -> `lerp` (a `bool` mix selector -> `select` / ternary); `fract` -> `frac`; `mod(x, y)` -> `rvkMod` (fmod truncates); `inversesqrt` -> `rsqrt`;
  `atan(y, x)` -> `atan2(y, x)`; `clamp(x, 0, 1)` -> `saturate`; `dFdx/dFdy` -> `ddx/ddy` (`ddx_fine` for fine); `floatBitsToUint` -> `asuint`, `uintBitsToFloat` -> `asfloat`,
  `floatBitsToInt` -> `asint`; `packHalf2x16` -> `f32tof16` pairs; `bitfieldExtract` -> shifts and masks; `findMSB` -> `firstbithigh`; `bitCount` -> `countbits`.
- Vector constructors: `vec3(1.0)` is **not** valid HLSL: write `float3(1.0, 1.0, 1.0)` (or `(float3)1.0`). `vec4(v3, 1.0)` is fine. `mat3(m4)` -> `(float3x3)m4`.
- Vector comparisons: GLSL `lessThan(a, b)` -> `a < b` (returns a bool vector), `any` / `all` work; `bvec` arithmetic: `float3(b)` casts.
  The ternary operator on **vectors** with a vector condition needs `select(cond, a, b)`; with a scalar bool it is fine.
- Integer / float: `int(x)` truncates in both. Integer division and `%` of negatives behave like C in both. `uint` shifts by >= 32 are undefined in both. `float` -> `uint`
  of a negative is undefined in HLSL: clamp first if the GLSL relied on wrapping.
- Swizzle assignment, `out` / `inout` parameters: same keywords; arrays are copied by value in function arguments (also GLSL); array-of-arrays `float a[2][3]` ok.
- `const` globals -> `static const`; non-const global variables -> `static`; `shared` -> `groupshared`; `barrier()` -> `GroupMemoryBarrierWithGroupSync()`;
  `memoryBarrierShared()` -> `GroupMemoryBarrier()`; `atomicAdd(shared, v)` -> `InterlockedAdd(sharedVar, v, orig)`.
- No `#define` with arguments named like attribute names; no recursion; no `switch` fallthrough surprises (HLSL switch needs `break`/`[branch]` is a hint only).
- HLSL integer `uint2 / uint` etc. promote like C. `max(uint, int)` is ambiguous: cast.
- Unused-variable / uninitialized warnings are fine; errors are not. DXC runs with `-HV 2021` (templates, no implicit truncation warnings off).
- Divergence: textures sampled inside non-uniform control flow with implicit derivatives (`Sample`) behave like GLSL `texture`; keep the original's choice (`SampleLevel` where it used `textureLod`).
- Do not "improve" the algorithm. A port is line for line; behaviour must stay the same (A/B in the game decides).

## Checking a port (no build needed; the tools are built)
```
$env:RVK_SHADER_DIR = '<repo>\shaders'
build\RelWithDebInfo\rvk_shadertool.exe hlsl water/water.frag            # compile like the renderer, spirv-val; prints DXC errors / warnings
python tools\port_check.py water/water.frag [more names]                   # GLSL vs HLSL interface: resources (set, binding, kind, format), push offsets, local size
```
`port_check` exit 0 = same interface. A difference line `GLSL only` / `HLSL only` is a bug in the port unless you can explain it (e.g. an unused resource).
