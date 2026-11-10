// RHI backend test (S-6, rvk_rhitest "depth map derived from the hardware depth"): two triangles at known view depths drawn the way the main view's z pre-pass is under
// reverse-z: clip.w = view z, clip.z = the projection near, so z / w = near / view z in the depth buffer. Vertex ids 0..2 = a big tilted triangle (view z 100 .. 700),
// 3..5 = a smaller one in front of it (40 .. 60). Pixel shaders: depth_z_scene.frag.hlsl.

static const float kNear = 10.0;

struct VsOut {
    float4 position : SV_Position;
    float viewZ : TEXCOORD0;
};

VsOut VSMain(uint id : SV_VertexID) {
    static const float3 v[6] = {float3(-0.9, -0.9, 100.0), float3(0.3, 0.9, 700.0), float3(0.3, -0.9, 300.0),
                                float3(-0.7, -0.6, 40.0), float3(-0.1, -0.6, 40.0), float3(-0.4, 0.4, 60.0)};
    float3 p = v[id];
    VsOut o;
    o.position = float4(p.x * p.z, p.y * p.z, kNear, p.z);
    o.viewZ = p.z;
    return o;
}
