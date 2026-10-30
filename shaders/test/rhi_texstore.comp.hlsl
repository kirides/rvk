// RHI backend test (tools/rhitest): writes into subresources through the views setStorageViews(mip, layer) binds, one HLSL text for both backends:
// one mip level of a 2D texture (u0, RWTexture2D), one layer of a 2D array texture (u1, RWTexture2DArray) and one mip level of a 3D texture
// (u2, RWTexture3D).

[[vk::image_format("rgba32f")]] RWTexture2D<float4> mipImage : register(u0, space1);
[[vk::image_format("rgba32f")]] RWTexture2DArray<float4> layerImage : register(u1, space1);
[[vk::image_format("rgba32f")]] RWTexture3D<float4> volumeImage : register(u2, space1);

[numthreads(8, 8, 1)]
void CSMain(uint3 id : SV_DispatchThreadID) {
    if (id.x < 4u && id.y < 4u) {
        mipImage[id.xy] = float4(float(id.x) / 4.0, float(id.y) / 4.0, 0.5, 1.0);
        layerImage[uint3(id.xy, 0u)] = float4(0.25, float(id.x), float(id.y), 1.0);
    }
    if (id.x < 2u && id.y < 2u) {
        for (uint z = 0u; z < 2u; ++z) volumeImage[uint3(id.xy, z)] = float4(float(id.x), float(id.y), float(z), 0.5);
    }
}
