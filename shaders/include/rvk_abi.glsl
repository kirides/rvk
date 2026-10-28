// The renderer's shader ABI in GLSL: the mirror of src/shader/abi.h (change both together; the rvk_tests case
// abi_glsl_layout_matches_cpp compares every offset below with the C++ structs).
//
// Translated D3D9 shaders get this text verbatim after their #version line (embedded at build time, shader/glsl.cpp);
// hand-written shaders (shaders/**, compiled by rvk_glslc) #include it. Pass shaders (injected passes, compute) define
// RVK_PASS_SHADER before the include and declare their own push block starting with RVK_PASS_PUSH_HEADER.
// Declarations only (no functions): helpers for hand-written shaders are in rvk_common.glsl.

#extension GL_EXT_buffer_reference : require
#extension GL_EXT_nonuniform_qualifier : require

// D3D9 constant registers of one stage (StageConsts): c at 0, i at 4096, b at 4352; 4416 bytes
layout(buffer_reference, std430, buffer_reference_align = 16) readonly buffer RvkStage {
    vec4 c[256];
    ivec4 i[16];
    uint b[16];
};

// Per-draw state (DrawBlock, 208 bytes): bindless texture / sampler indices of D3D9 samplers s0..s15 (pixel) and
// 16..19 (vertex s0..s3), the half-pixel fix, the alpha test; pad0 = debug tint (RGBA8, 0 = none)
layout(buffer_reference, std430, buffer_reference_align = 16) readonly buffer RvkDraw {
    uint tex[20];
    uint smp[20];
    vec4 posFix;
    uint alphaFunc;
    float alphaRef;
    uint pad0;
    uint pad1;
    vec4 psC5;             // pixel shader c5 of the draw (split constants: the pixel window of register 5; abi.h)
};

// Per-frame block (FrameBlock, 976 bytes; Device::frameBlockAddress): the main view's camera (current + previous), depth
// conventions, target size, jitter, time, frame index, image roles, feature parameters. Matrices are the engine's
// row-vector D3D matrices stored as they are, which GLSL reads as their transpose: clip = viewProj * vec4(world, 1).
layout(buffer_reference, std430, buffer_reference_align = 16) readonly buffer RvkFrame {
    mat4 viewProj;         //   0
    mat4 invViewProj;      //  64
    mat4 view;             // 128
    mat4 proj;             // 192
    mat4 invView;          // 256
    mat4 invProj;          // 320
    mat4 prevViewProj;     // 384  the previous frame's (= viewProj when RVK_FRAME_PREV_VALID is clear)
    mat4 prevInvViewProj;  // 448
    vec4 cameraPos;        // 512  xyz world position, w = engine far (c11.w: the depth map stores view depth / w)
    vec4 prevCameraPos;    // 528  xyz, w unused
    vec4 depthParams;      // 544  x projection near, y projection far, z engine far, w 1 / engine far
    vec4 viewport;         // 560  scene target width, height, 1 / width, 1 / height (0 before the scene target is known)
    vec4 jitter;           // 576  xy this frame's sub-pixel jitter in pixels, zw the previous frame's (0: no TAA yet)
    vec4 time;             // 592  x seconds since the device was created, y seconds since the previous frame, zw 0
    uvec4 frameInfo;       // 608  x frame index (low 32 bits), y RVK_FRAME_* flags, z main views so far this frame, w 0
    uvec4 images;          // 624  bindless t2D slots: x scene HDR, y refraction copy, z depth map; w tCube slot of the
                           //      sky environment cube (0 = not known). Sample only after Device::readable() for passes
    vec4 params[16];       // 640  feature parameters (abi.h kFrameParam*)
    uvec4 rtScene;         // 896  the ray tracing scene (enh/rt_scene, rvk_rt.glsl): xy TLAS address, zw geometry records
    uvec4 rtInfo;          // 912  x TLAS instances, y records, z RVK_FRAME_RT_* flags (0: no scene this submission), w 0
    uvec4 gi;              // 928  the GI probe volume (rvk_gi.glsl): xy its header's address, z RVK_FRAME_GI_* flags, w 0
    uvec4 ao;              // 944  last frame's ambient occlusion (enh/gbuffer): x t2D slot, y sampler, z RVK_FRAME_AO_* flags,
                           //      w strength (float bits)
    uvec4 sgi;             // 960  last frame's screen probe GI ratio (enh/gi): x t2D slot, y sampler, z RVK_FRAME_SGI_* flags, w 0
};
const uint RVK_FRAME_RT_VALID = 1u;
const uint RVK_FRAME_GI_VALID = 1u;
const uint RVK_FRAME_AO_VALID = 1u;
const uint RVK_FRAME_SGI_VALID = 1u;

// RvkFrame.frameInfo.y
const uint RVK_FRAME_CAMERA_VALID = 1u;      // the camera fields hold this frame's main view
const uint RVK_FRAME_CAMERA_PREDICTED = 2u;  // ... the previous frame's (no main view yet this frame, e.g. shadow maps)
const uint RVK_FRAME_PREV_VALID = 4u;        // the prev* fields hold the frame before
const uint RVK_FRAME_REVERSE_Z = 8u;         // the main view's hardware depth is reversed: near / view z, 0 = infinity (rvk_common.glsl)

#ifndef RVK_PASS_SHADER
// Push constants of draws (112 bytes, vertex + fragment). drawId: the draw's index in the frame (= its draw record's index);
// flags: RVK_DRAW_* below. prevVs: a vertex capture address (draw variants; 0 = none). vs / ps: the draw's one full
// StageConsts (legacy draws; 0 for split draws). vsw / psw: the float constants by register window (abi.h
// kVsWindowFirst / kPsWindowFirst): c[n] of the window that owns register n, biased so the register number stays the index
layout(push_constant, std430) uniform RvkPush {
    RvkStage vs;
    RvkStage ps;
    RvkDraw draw;
    RvkFrame frame;
    RvkStage prevVs;
    uint drawId;
    uint flags;
    RvkStage vsw[4];
    RvkStage psw[4];
} pc;
// float constant register n of the vertex / pixel stage (n: an int / uint expression; a constant folds to one window)
#define RVK_VS_WIN(n) ((uint(n)) < 12u ? 0 : (uint(n)) < 31u ? 1 : (uint(n)) < 232u ? 2 : 3)
#define RVK_PS_WIN(n) ((uint(n)) < 5u ? 0 : (uint(n)) < 6u ? 1 : (uint(n)) < 14u ? 2 : 3)
#define RVK_VSC(n) pc.vsw[RVK_VS_WIN(n)].c[n]
#define RVK_PSC(n) pc.psw[RVK_PS_WIN(n)].c[n]
#else
// Pass shaders: push block (<= 128 bytes) = this header (16 bytes) + the pass's own fields
#define RVK_PASS_PUSH_HEADER RvkFrame frame; uint passId; uint passFlags;
#endif

// RvkPush.flags: bits 0-7 the draw kind (d3d9/draws.h Kind), 8-11 the pass (scene.h Pass), 12-15 the view (scene.h View),
// 16.. feature bits
const uint RVK_DRAW_KIND_MASK = 0xffu;
const uint RVK_DRAW_PASS_SHIFT = 8u;
const uint RVK_DRAW_VIEW_SHIFT = 12u;
const uint RVK_DRAW_TINTED = 0x10000u;

// Set 0: the bindless heap (update-after-bind, partially bound). Index 0 of every texture array is a 1x1 (0,0,0,1) texture.
layout(set = 0, binding = 0) uniform texture2D t2D[];
layout(set = 0, binding = 1) uniform textureCube tCube[];
layout(set = 0, binding = 2) uniform texture3D t3D[];
layout(set = 0, binding = 3) uniform sampler samp[];
