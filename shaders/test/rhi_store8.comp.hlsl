// RHI backend test (tools/rhitest): eight storage images (u0..u7 space1, CommandList::setStorageViews) written in one dispatch, one HLSL text for
// both backends: image i gets the value (i + 1) in the texel (id.x, 0) and (i + 1) * 10 in the texel (id.x, 1).

[[vk::image_format("r32f")]] RWTexture2D<float> img0 : register(u0, space1);
[[vk::image_format("r32f")]] RWTexture2D<float> img1 : register(u1, space1);
[[vk::image_format("r32f")]] RWTexture2D<float> img2 : register(u2, space1);
[[vk::image_format("r32f")]] RWTexture2D<float> img3 : register(u3, space1);
[[vk::image_format("r32f")]] RWTexture2D<float> img4 : register(u4, space1);
[[vk::image_format("r32f")]] RWTexture2D<float> img5 : register(u5, space1);
[[vk::image_format("r32f")]] RWTexture2D<float> img6 : register(u6, space1);
[[vk::image_format("r32f")]] RWTexture2D<float> img7 : register(u7, space1);

[numthreads(4, 1, 1)]
void CSMain(uint3 id : SV_DispatchThreadID) {
    const uint2 a = uint2(id.x, 0u);
    const uint2 b = uint2(id.x, 1u);
    img0[a] = 1.0; img0[b] = 10.0;
    img1[a] = 2.0; img1[b] = 20.0;
    img2[a] = 3.0; img2[b] = 30.0;
    img3[a] = 4.0; img3[b] = 40.0;
    img4[a] = 5.0; img4[b] = 50.0;
    img5[a] = 6.0; img5[b] = 60.0;
    img6[a] = 7.0; img6[b] = 70.0;
    img7[a] = 8.0; img7[b] = 80.0;
}
