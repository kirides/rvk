// The renderer's shader ABI in HLSL (docs/research/hlsl-rhi.md 4): the mirror of src/shader/abi.h, the successor of rvk_abi.glsl.
// Layout rule: every ABI struct is made of float4 / uint4 (and arrays of them) only, so the layout is the same under std140, std430,
// scalar and ByteAddressBuffer packing and the C++ structs mirror it field by field (rvk_tests abi_hlsl_layout_matches_cpp compares
// the offsets in the compiled SPIR-V). Matrices are stored the way the engine keeps them (row-major, row-vector: v' = v * M) as
// four float4 rows; RvkFrame (below) builds float4x4 from them, so `mul(float4(world, 1), f.viewProj)` is the D3D clip position.
//
// The one backend-specific part of the shader sources is this header's section 1 (how a pointer to a block is spelled and how push
// constants are declared); everything else is plain SM 6.6 HLSL, compiled to SPIR-V (Vulkan) or DXIL (DX12) from the same text.
#pragma once

// ---- 1. references and push constants ---------------------------------------------------------------------------------------
#ifdef __spirv__
// a pointer to a block in GPU memory (Vulkan: the buffer's device address; a push-constant member of 8 bytes)
#define RVK_REF(T) vk::BufferPointer<T, 16>
#define RVK_LOAD(T, ref) (ref).Get()
#define RVK_PUSH(T, name) [[vk::push_constant]] T name
// explicit Vulkan (binding, set) for the resources whose D3D register space differs from the set (the bindless heap below);
// every other resource uses register(<class><binding>, space<set>), which DXC maps onto the same binding and set
#define RVK_VK_BINDING(slot, space) [[vk::binding(slot, space)]]
#else
// DX12 (SM 6.6 bindless, docs/research/hlsl-rhi.md 6): a reference is (heap index of a ByteAddressBuffer SRV, byte offset) in the same 8 bytes the
// Vulkan side keeps a device address in (rhi::Buffer::shaderRef fills either); push constants are the root constants of the fixed root
// signature (32 DWORDs at b0, space0); pass resources keep their (register, space) = (binding, set) numbers (rhi/d3d12)
struct RvkBufRef {
    uint slot;
    uint offset;
};
#define RVK_REF(T) RvkBufRef
// (a heap access is assigned to a typed local first: a cast followed by a method call crashes DXC 1.9.2602's DXIL back end)
ByteAddressBuffer rvkHeapBuffer(uint slot) {
    ByteAddressBuffer b = ResourceDescriptorHeap[NonUniformResourceIndex(slot)];
    return b;
}
#define RVK_LOAD(T, ref) (rvkHeapBuffer((ref).slot).Load<T>((ref).offset))
#define RVK_PUSH(T, name) ConstantBuffer<T> name : register(b0, space0)
#define RVK_VK_BINDING(slot, space)
#endif

// ---- 2. the per-frame block (FrameBlock, 976 bytes; Device::frameBlockAddress) -----------------------------------------------
struct RvkFrameData {
    float4 viewProj[4];         //   0  matrices: four rows each
    float4 invViewProj[4];      //  64
    float4 view[4];             // 128
    float4 proj[4];             // 192
    float4 invView[4];          // 256
    float4 invProj[4];          // 320
    float4 prevViewProj[4];     // 384  the previous frame's (= viewProj when RVK_FRAME_PREV_VALID is clear)
    float4 prevInvViewProj[4];  // 448
    float4 cameraPos;           // 512  xyz world position, w = engine far (c11.w: the depth map stores view depth / w)
    float4 prevCameraPos;       // 528  xyz, w unused
    float4 depthParams;         // 544  x projection near, y projection far, z engine far, w 1 / engine far
    float4 viewport;            // 560  scene target width, height, 1 / width, 1 / height (0 before the scene target is known)
    float4 jitter;              // 576  xy this frame's sub-pixel jitter in pixels, zw the previous frame's
    float4 time;                // 592  x seconds since the device was created, y seconds since the previous frame
    uint4 frameInfo;            // 608  x frame index (low 32 bits), y RVK_FRAME_* flags, z main views so far this frame
    uint4 images;               // 624  bindless t2D slots: x scene HDR, y refraction copy, z depth map; w tCube slot of the sky cube
    float4 params[16];          // 640  feature parameters (abi.h kFrameParam*)
    uint4 rtScene;              // 896  the ray tracing scene: xy TLAS address, zw geometry records
    uint4 rtInfo;               // 912  x TLAS instances, y records, z RVK_FRAME_RT_* flags
    uint4 gi;                   // 928  the GI probe volume: xy its header's address, z RVK_FRAME_GI_* flags
    uint4 ao;                   // 944  last frame's ambient occlusion: x t2D slot, y sampler, z flags, w strength (float bits)
    uint4 sgi;                  // 960  last frame's screen probe GI ratio: x t2D slot, y sampler, z flags, w second image slot
};

// the frame block unpacked: matrices as float4x4 (rows as stored), everything else as in RvkFrameData
struct RvkFrame {
    float4x4 viewProj;
    float4x4 invViewProj;
    float4x4 view;
    float4x4 proj;
    float4x4 invView;
    float4x4 invProj;
    float4x4 prevViewProj;
    float4x4 prevInvViewProj;
    float4 cameraPos;
    float4 prevCameraPos;
    float4 depthParams;
    float4 viewport;
    float4 jitter;
    float4 time;
    uint4 frameInfo;
    uint4 images;
    float4 params[16];
    uint4 rtScene;
    uint4 rtInfo;
    uint4 gi;
    uint4 ao;
    uint4 sgi;
};

float4x4 rvkRows(float4 r[4]) { return float4x4(r[0], r[1], r[2], r[3]); }

// Unused fields cost nothing: the loads are plain and the compiler drops the dead ones
RvkFrame rvkLoadFrame(RVK_REF(RvkFrameData) p) {
    RvkFrameData d = RVK_LOAD(RvkFrameData, p);
    RvkFrame f;
    f.viewProj = rvkRows(d.viewProj);
    f.invViewProj = rvkRows(d.invViewProj);
    f.view = rvkRows(d.view);
    f.proj = rvkRows(d.proj);
    f.invView = rvkRows(d.invView);
    f.invProj = rvkRows(d.invProj);
    f.prevViewProj = rvkRows(d.prevViewProj);
    f.prevInvViewProj = rvkRows(d.prevInvViewProj);
    f.cameraPos = d.cameraPos;
    f.prevCameraPos = d.prevCameraPos;
    f.depthParams = d.depthParams;
    f.viewport = d.viewport;
    f.jitter = d.jitter;
    f.time = d.time;
    f.frameInfo = d.frameInfo;
    f.images = d.images;
    f.params = d.params;
    f.rtScene = d.rtScene;
    f.rtInfo = d.rtInfo;
    f.gi = d.gi;
    f.ao = d.ao;
    f.sgi = d.sgi;
    return f;
}

// ---- 1b. pointers from stored addresses (Vulkan: needs shaderInt64) -----------------------------------------------------------
// GPU addresses travel as (low, high) uint pairs in the frame block, in records and in push constants; rvkPtr<T, align>(pair) is a
// typed pointer to the data there (`.Get().member`, atomics through it: InterlockedAdd(rvkPtr<Counters, 4>(a).Get().n, 1)).
#ifdef __spirv__  // (device addresses do not exist in shaders on DX12: references are (heap slot, offset) pairs, RvkBufRef)
template<typename T, uint A>
vk::BufferPointer<T, A> rvkPtr(uint2 a) {
    return vk::BufferPointer<T, A>(uint64_t(a.x) | (uint64_t(a.y) << 32));
}
// the uint64 of such a pair (address arithmetic: vk::RawBufferLoad<T>(rvkAddr(a) + offset))
uint64_t rvkAddr(uint2 a) { return uint64_t(a.x) | (uint64_t(a.y) << 32); }
#endif

// GLSL's mod(): x - y * floor(x / y) (HLSL's fmod truncates, so differs for negative x)
float rvkMod(float x, float y) { return x - y * floor(x / y); }
float2 rvkMod(float2 x, float2 y) { return x - y * floor(x / y); }
float3 rvkMod(float3 x, float3 y) { return x - y * floor(x / y); }
float4 rvkMod(float4 x, float4 y) { return x - y * floor(x / y); }

static const uint RVK_FRAME_RT_VALID = 1u;
static const uint RVK_FRAME_GI_VALID = 1u;
static const uint RVK_FRAME_AO_VALID = 1u;
static const uint RVK_FRAME_SGI_VALID = 1u;

// RvkFrame.frameInfo.y
static const uint RVK_FRAME_CAMERA_VALID = 1u;      // the camera fields hold this frame's main view
static const uint RVK_FRAME_CAMERA_PREDICTED = 2u;  // ... the previous frame's (no main view yet this frame, e.g. shadow maps)
static const uint RVK_FRAME_PREV_VALID = 4u;        // the prev* fields hold the frame before
static const uint RVK_FRAME_REVERSE_Z = 8u;         // the main view's hardware depth is reversed: near / view z, 0 = infinity

// ---- 3. pass shaders: the push block starts with this header (16 bytes); a pass adds its own fields after it ----------------
// (translated D3D9 shaders and material shaders have their own push block, RvkPush, with the draw pointers: added with their port)
#define RVK_PASS_PUSH_HEADER RVK_REF(RvkFrameData) frame; uint passId; uint passFlags;

// ---- 4. the bindless heap: set 0 on Vulkan ------------------------------------------------------------------------------------
// One unbounded array per resource type, each in its own register space (a D3D12 root signature needs that); the Vulkan build binds
// them to set 0's bindings 0..3 (abi.h kBinding*). Index 0 of every texture array is a 1x1 (0,0,0,1) texture.
#ifdef __spirv__
RVK_VK_BINDING(0, 0) Texture2D<float4> g_t2D[] : register(t0, space100);
RVK_VK_BINDING(1, 0) TextureCube<float4> g_tCube[] : register(t0, space101);
RVK_VK_BINDING(2, 0) Texture3D<float4> g_t3D[] : register(t0, space102);
RVK_VK_BINDING(3, 0) SamplerState g_samp[] : register(s0, space103);
#define RVK_TEX2D(i) g_t2D[NonUniformResourceIndex(i)]
#define RVK_TEXCUBE(i) g_tCube[NonUniformResourceIndex(i)]
#define RVK_TEX3D(i) g_t3D[NonUniformResourceIndex(i)]
#define RVK_SAMP(i) g_samp[NonUniformResourceIndex(i)]
#else
// DX12: one shader-visible heap for textures / buffers (the index space is the heap's: rhi::Texture::heapSampled), one for samplers
Texture2D<float4> rvkHeapTex2D(uint i) {
    Texture2D<float4> t = ResourceDescriptorHeap[NonUniformResourceIndex(i)];
    return t;
}
TextureCube<float4> rvkHeapTexCube(uint i) {
    TextureCube<float4> t = ResourceDescriptorHeap[NonUniformResourceIndex(i)];
    return t;
}
Texture3D<float4> rvkHeapTex3D(uint i) {
    Texture3D<float4> t = ResourceDescriptorHeap[NonUniformResourceIndex(i)];
    return t;
}
SamplerState rvkHeapSampler(uint i) {
    SamplerState s = SamplerDescriptorHeap[NonUniformResourceIndex(i)];
    return s;
}
#define RVK_TEX2D(i) rvkHeapTex2D(i)
#define RVK_TEXCUBE(i) rvkHeapTexCube(i)
#define RVK_TEX3D(i) rvkHeapTex3D(i)
#define RVK_SAMP(i) rvkHeapSampler(i)
#endif
