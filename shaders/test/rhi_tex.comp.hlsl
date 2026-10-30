// RHI backend test (tools/rhitest): texture kinds and subresources on both backends from one HLSL text.
// Samples a cube map (a direction per face), a 3D texture, the mip levels of a 2D texture (SampleLevel / Load) and the layers of a 2D array
// texture (one heap index per layer) with a point sampler; thread i writes its result to texel (i, 0) of the storage image u0.
// (rhi_texstore.comp.hlsl writes the subresources.)
#include "rvk_common.hlsli"

struct TexPush {
    uint cube;     // heap index of the cube map
    uint volume;   // ... of the 3D texture
    uint mips;     // ... of the 2D texture with a mip chain
    uint sampler;  // point sampler
    uint4 layers;  // heap indices of the layers of the array texture (xyz)
};
RVK_PUSH(TexPush, pc);

[[vk::image_format("rgba32f")]] RWTexture2D<float4> outImage : register(u0, space1);

[numthreads(16, 1, 1)]
void CSMain(uint3 id : SV_DispatchThreadID) {
    const uint i = id.x;
    float4 r = float4(-1.0, -1.0, -1.0, -1.0);
    if (i < 6u) {
        // +X -X +Y -Y +Z -Z
        const float3 dirs[6] = {float3(1, 0, 0), float3(-1, 0, 0), float3(0, 1, 0), float3(0, -1, 0), float3(0, 0, 1), float3(0, 0, -1)};
        r = RVK_TEXCUBE(pc.cube).SampleLevel(RVK_SAMP(pc.sampler), dirs[i], 0.0);
    } else if (i < 8u) {
        // voxels (1, 2, 3) and (3, 0, 2) of a 4x4x4 texture
        const float3 voxel = i == 6u ? float3(1, 2, 3) : float3(3, 0, 2);
        r = RVK_TEX3D(pc.volume).SampleLevel(RVK_SAMP(pc.sampler), (voxel + 0.5) / 4.0, 0.0);
    } else if (i < 11u) {
        // mip levels 1, 2, 3 of an 8x8 texture with 4 levels
        r = RVK_TEX2D(pc.mips).SampleLevel(RVK_SAMP(pc.sampler), float2(0.3, 0.6), float(i - 7u));
    } else if (i == 11u) {
        r = RVK_TEX2D(pc.mips).Load(int3(1, 1, 2));  // texel (1, 1) of mip 2
    } else if (i < 15u) {
        r = RVK_TEX2D(pc.layers[i - 12u]).SampleLevel(RVK_SAMP(pc.sampler), float2(0.5, 0.5), 0.0);
    }
    outImage[uint2(i, 0)] = r;
}
