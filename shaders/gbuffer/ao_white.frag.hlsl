// ao.mode = replace (src/enh/gbuffer.cpp): the game's SSAO draws (post_SSM.fx SSAO / BlurSSAO into the half-size mask) write
// "unoccluded", so the shadow mask's red channel is 1 and only the GTAO (on the ambient light) shades.

float4 PSMain(float4 position : SV_Position, float2 uv : TEXCOORD0) : SV_Target0 { return float4(1.0, 1.0, 1.0, 1.0); }
