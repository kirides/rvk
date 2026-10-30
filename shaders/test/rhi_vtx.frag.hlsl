// RHI backend test (tools/rhitest): the fragment shader of the vertex input / indirect draw check (with test/rhi_vtx.vert): the interpolated colour.

float4 PSMain(float4 position : SV_Position, float4 col : TEXCOORD0) : SV_Target0 { return col; }
