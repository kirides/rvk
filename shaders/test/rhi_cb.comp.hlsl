// RHI backend test (tools/rhitest, docs/research/hlsl-rhi.md 5): every binding kind of the RHI in one compute shader, compiled once to SPIR-V and
// once to DXIL from this text. Reads a constant buffer (b8 space1), a texture + sampler by bindless index (the indices travel in the constant
// buffer), a RVK_REF to a block (the frame block: rvkLoadFrame), the push constants, and writes a storage image (u0 space1).
#include "rvk_common.hlsli"

struct CbData {
    float4 scale;
    uint4 ids;  // x sampled texture (bindless index), y sampler index
};
ConstantBuffer<CbData> g_cb : register(b8, space1);

struct RhiPush {
    RVK_PASS_PUSH_HEADER
    uint width;
    uint height;
    uint pad0;
    uint pad1;
};
RVK_PUSH(RhiPush, pc);

[[vk::image_format("rgba32f")]] RWTexture2D<float4> outImage : register(u0, space1);

[numthreads(8, 8, 1)]
void CSMain(uint3 id : SV_DispatchThreadID) {
    if (id.x >= pc.width || id.y >= pc.height) return;
    RvkFrame f = rvkLoadFrame(pc.frame);
    float2 uv = (float2(id.xy) + 0.5) / float2(pc.width, pc.height);
    float4 t = rvkSample2DLod(g_cb.ids.x, g_cb.ids.y, uv, 0.0);
    outImage[id.xy] = t * g_cb.scale + float4(f.time.x, f.params[1].x, float(pc.passId), float(id.x + id.y));
}
