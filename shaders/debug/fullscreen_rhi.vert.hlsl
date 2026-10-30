// fullscreen.vert for passes recorded through the slim RHI (docs/research/hlsl-rhi.md 8): the RHI's viewport is D3D style (y down: the Vulkan
// backend flips it with a negative height, CommandList::setViewport), so clip y = +1 is the top row. A triangle covering the target (draw 3 vertices,
// no vertex buffer); uv 0,0 = top left, as fullscreen.vert.

struct VsOut {
    float4 position : SV_Position;
    float2 uv : TEXCOORD0;
};

VsOut VSMain(uint vertexId : SV_VertexID) {
    VsOut o;
    float2 uv = float2((vertexId << 1) & 2, vertexId & 2);
    o.uv = uv;
    o.position = float4(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0, 0.0, 1.0);
    return o;
}
