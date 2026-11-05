// The traced reflections' composite (enh/material_rt.*): the filtered reflection image onto the scene HDR target, blend result = src.rgb + dst * src.a
// (rgb = the light the reflection adds, a = the factor the scene is multiplied by: puddles darken the surface under them).

#include "rvk_common.hlsli"

struct MaterialRtCompositePush {
    RVK_PASS_PUSH_HEADER
    uint image;    // bindless t2D slot of the filtered reflection image (the scene's size)
    uint sampler;  // bindless sampler index (point)
    uint pad0;
    uint pad1;
};
RVK_PUSH(MaterialRtCompositePush, pc);

float4 PSMain(float4 position : SV_Position, float2 uv : TEXCOORD0) : SV_Target0 {
    return rvkSample2DLod(pc.image, pc.sampler, uv, 0.0);
}
