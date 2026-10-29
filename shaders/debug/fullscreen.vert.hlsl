// A triangle covering the target (draw 3 vertices, no vertex buffer); uv 0,0 = top left.

struct VsOut {
    float4 position : SV_Position;
    float2 uv : TEXCOORD0;
};

VsOut VSMain(uint vertexId : SV_VertexID) {
    VsOut o;
    float2 uv = float2((vertexId << 1) & 2, vertexId & 2);
    o.uv = uv;
    o.position = float4(uv * 2.0 - 1.0, 0.0, 1.0);
    return o;
}
