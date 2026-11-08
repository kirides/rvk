// S-7b (docs/research/frame-ownership.md 14.8b): the DoF graph's middle, post_dof.fx DoFBlurCocH -> DoFBlurCocV -> DoFNearCoc -> DoFSmallBlur (four draws over the
// 400x225 images) as ONE dispatch: an 8x8 output tile computes the stages over its halo in groupshared memory and writes the two images the final DoF pass samples,
// BlurV (the "large" blur) and SmallBlur (the "medium"). Input: the DoFDown image (colour + circle of confusion in alpha).
//
// Every tap of the four shaders sits at a texel boundary IN BOTH AXES (the quad's uv at pixel p is p / size: the engine adds no half texel to these units; a "horizontal"
// tap at k texels is also half a texel above the pixel), at an integer texel offset (native::OwnPost verifies the composed draws' offsets), so each bilinear tap is the exact
// mean of a 2 x 2 block of texels: the kernel does that arithmetic itself (clamped texel indices = the clamp sampler), in the shaders' summation order. Between stages the
// values are rounded to 8 bits, as the engine's A8R8G8B8 intermediates:
//   BlurH(x, y) = 0.25 ((t(+1) + t(-1)) + t(+3)) + t(-3)),  t(k) = mean of T[x + k - 1 .. x + k] x [y - 1 .. y]      (rgba)
//   BlurV likewise: t(k) = mean of H[x - 1 .. x] x [y + k - 1 .. y + k]
//   NearCoc: rgb = DoFDown rgb, a = 2 max(BlurV.a, DoFDown.a) - DoFDown.a
//   SmallBlur(p) = 0.25 (((t(x0,y0) + t(x1,y0)) + t(x0,y1)) + t(x1,y1)), x0 = p, x1 = p + 1: t = the 2 x 2 mean of NearCoc at the corner

#include "rvk_common.hlsli"

// mirror: native::PostDofMidParams
struct PostDofMidParams {
    uint4 tex;   // x DoFDown image (bindless t2D), y, z, w
    uint4 size;  // x, y: image size
};

struct PostDofMidPush {
    RVK_PASS_PUSH_HEADER
    uint2 params;
    uint2 pad;
};
RVK_PUSH(PostDofMidPush, pc);

[[vk::image_format("rgba8")]] RWTexture2D<float4> outBlurV : register(u0, space1);
[[vk::image_format("rgba8")]] RWTexture2D<float4> outSmall : register(u1, space1);

// halo layout: output tile origin t0; NearCoc / BlurV over columns / rows [t0 - 1, t0 + 8] (10), H over rows [t0 - 5, t0 + 11] (17) x columns [t0 - 2, t0 + 8] (11)
groupshared float4 gH[17 * 11];
groupshared float4 gV[10 * 10];
groupshared float4 gN[10 * 10];

float4 q8(float4 v) { return floor(saturate(v) * 255.0 + 0.5) / 255.0; }

// the DoFDown image's texel (clamped: the clamp sampler)
float4 texC(uint tex, int x, int y, int2 size) { return rvkFetch2D(tex, int2(clamp(x, 0, size.x - 1), clamp(y, 0, size.y - 1))); }
// a bilinear tap at a texel corner k texels right of pixel x (and half a texel above row y): the mean of the 2 x 2 block (x + k - 1 .. x + k) x (y - 1 .. y)
float4 meanX(uint tex, int x, int y, int k, int2 size) {
    return 0.25 * (texC(tex, x + k - 1, y - 1, size) + texC(tex, x + k, y - 1, size) + texC(tex, x + k - 1, y, size) + texC(tex, x + k, y, size));
}

// BlurH at a (halo) position; the position clamps to the image like the next stage's clamp sampler
float4 blurHAt(uint tex, int2 p, int2 size) {
    p = clamp(p, int2(0, 0), size - 1);
    float4 r = meanX(tex, p.x, p.y, 1, size) + meanX(tex, p.x, p.y, -1, size);
    r = r + meanX(tex, p.x, p.y, 3, size);
    r = r + meanX(tex, p.x, p.y, -3, size);
    return q8(r * 0.25);
}

[numthreads(8, 8, 1)]
void CSMain(uint3 gid : SV_GroupID, uint3 tid : SV_GroupThreadID) {
    PostDofMidParams p = rvkPtr<PostDofMidParams, 16>(pc.params).Get();
    const int2 size = int2(p.size.xy);
    const int2 t0 = int2(gid.xy) * 8;
    const uint t = tid.y * 8u + tid.x;
    // 1. BlurH over rows t0.y - 5 .. t0.y + 11 of the 11 columns t0.x - 2 .. t0.x + 8
    for (uint i = t; i < 187u; i += 64u) gH[i] = blurHAt(p.tex.x, int2(t0.x - 2 + int(i % 11u), t0.y - 5 + int(i / 11u)), size);
    GroupMemoryBarrierWithGroupSync();
    // 2. BlurV and 3. NearCoc over the 10 x 10 halo (positions clamp to the image)
    for (uint j = t; j < 100u; j += 64u) {
        const int2 q = int2(t0.x - 1 + int(j % 10u), t0.y - 1 + int(j / 10u));
        const int2 qc = clamp(q, int2(0, 0), size - 1);
        const int col = qc.x - (t0.x - 2);  // LDS column of image column qc.x (qc.x - 1 is the one before)
        const int row0 = qc.y - (t0.y - 5);  // LDS row of image row qc.y
        // taps k: the mean of the block (x - 1 .. x) x (y + k - 1 .. y + k); image cells beyond the border hold the clamped cell already
        const float4 a1 = 0.25 * (gH[(row0 + 0) * 11 + col - 1] + gH[(row0 + 0) * 11 + col] + gH[(row0 + 1) * 11 + col - 1] + gH[(row0 + 1) * 11 + col]);
        const float4 a2 = 0.25 * (gH[(row0 - 2) * 11 + col - 1] + gH[(row0 - 2) * 11 + col] + gH[(row0 - 1) * 11 + col - 1] + gH[(row0 - 1) * 11 + col]);
        const float4 a3 = 0.25 * (gH[(row0 + 2) * 11 + col - 1] + gH[(row0 + 2) * 11 + col] + gH[(row0 + 3) * 11 + col - 1] + gH[(row0 + 3) * 11 + col]);
        const float4 a4 = 0.25 * (gH[(row0 - 4) * 11 + col - 1] + gH[(row0 - 4) * 11 + col] + gH[(row0 - 3) * 11 + col - 1] + gH[(row0 - 3) * 11 + col]);
        float4 bv = a1 + a2;
        bv = bv + a3;
        bv = bv + a4;
        bv = q8(bv * 0.25);
        gV[j] = bv;
        const float4 d = texC(p.tex.x, qc.x, qc.y, size);
        float4 n;
        n.rgb = d.rgb;
        n.a = 2.0 * max(bv.a, d.a) - d.a;
        gN[j] = q8(n);
    }
    GroupMemoryBarrierWithGroupSync();
    const int2 px = t0 + int2(tid.xy);
    if (px.x >= size.x || px.y >= size.y) return;
    const int c = (int(tid.y) + 1) * 10 + int(tid.x) + 1;
    outBlurV[px] = gV[c];
    // 4. SmallBlur: the four corner taps, each the mean of a 2 x 2 block of NearCoc
    const float4 t00 = 0.25 * (gN[c - 11] + gN[c - 10] + gN[c - 1] + gN[c]);
    const float4 t10 = 0.25 * (gN[c - 10] + gN[c - 9] + gN[c] + gN[c + 1]);
    const float4 t01 = 0.25 * (gN[c - 1] + gN[c] + gN[c + 9] + gN[c + 10]);
    const float4 t11 = 0.25 * (gN[c] + gN[c + 1] + gN[c + 10] + gN[c + 11]);
    float4 s = t00 + t10;
    s = s + t01;
    s = s + t11;
    outSmall[px] = s * 0.25;
}
