// A scaled copy of one texture subresource into a render target (the D3D12 backend's StretchRect / mip chain / present blit): a triangle over the viewport,
// uv from the push block's rectangle.

#include "rvk_common.hlsli"

struct BlitPush {
    uint srcTex;
    uint smp;
    uint pad0;
    uint pad1;
    float4 uvRect;  // u0, v0, du, dv
};
RVK_PUSH(BlitPush, pc);

struct VsOut {
    float4 position : SV_Position;
    float2 uv : TEXCOORD0;
};

VsOut VSMain(uint vertexId : SV_VertexID) {
    VsOut o;
    const float2 t = float2((vertexId << 1) & 2, vertexId & 2);
    o.position = float4(t.x * 2.0 - 1.0, 1.0 - t.y * 2.0, 0.0, 1.0);
    o.uv = pc.uvRect.xy + t * pc.uvRect.zw;
    return o;
}
