// RHI backend test: the fragment shader of the graphics check (with debug/fullscreen.vert): writes the constant buffer's colour.

struct CbData {
    float4 scale;
    uint4 ids;
};
ConstantBuffer<CbData> g_cb : register(b8, space1);

float4 PSMain(float4 position : SV_Position, float2 uv : TEXCOORD0) : SV_Target0 { return g_cb.scale; }
