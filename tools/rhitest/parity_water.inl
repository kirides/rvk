// Included by parity.cpp (anonymous namespace): the enhanced water's pixel shader (shaders/water/water.frag.hlsl, the lake model: three normal maps, a sky cube
// masked by two textures, SSR through the depth map, the refraction copy, the engine's lighting from the WaterBlock) drawn on both backends. The water draws
// are the renderer's way of using the draw ABI with its own block in `pc.draw` instead of a DrawBlock, with the engine's translated vertex shader in front
// (here an assembled one writing the same interpolators: TEXCOORD0 1 3 4 5 6 7, COLOR0 = varying locations 0 1 3 4 5 6 7 16).

// vs: a quad of the water plane (y = 0) in front of the camera
const char* kWaterVsAsm = R"(vs_3_0
dcl_position v0
dcl_texcoord0 v1
dcl_position o0
dcl_texcoord0 o1
dcl_texcoord1 o2
dcl_texcoord3 o3
dcl_texcoord4 o4
dcl_texcoord5 o5
dcl_texcoord6 o6
dcl_texcoord7 o7
dcl_color0 o8
mul o0, v0, c7
mul o1, v1.xyxy, c8
mul o2, v1.yxyx, c8
mul r0, v0.xxyy, c0
add o3, r0, c1
mov o4, c2
mov o5, c3
mov o6, c4
mov o7, c5
mov o8, c6
)";

struct Mat4 {
    float m[4][4];
};
Mat4 inverse(const Mat4& a) {  // Gauss-Jordan with partial pivoting
    float w[4][8] = {};
    for (int r = 0; r < 4; ++r) {
        for (int c = 0; c < 4; ++c) w[r][c] = a.m[r][c];
        w[r][4 + r] = 1.0f;
    }
    for (int col = 0; col < 4; ++col) {
        int piv = col;
        for (int r = col + 1; r < 4; ++r) {
            if (std::fabs(w[r][col]) > std::fabs(w[piv][col])) piv = r;
        }
        for (int c = 0; c < 8; ++c) std::swap(w[col][c], w[piv][c]);
        const float d = w[col][col];
        for (int c = 0; c < 8; ++c) w[col][c] /= d;
        for (int r = 0; r < 4; ++r) {
            if (r == col) continue;
            const float k = w[r][col];
            for (int c = 0; c < 8; ++c) w[r][c] -= k * w[col][c];
        }
    }
    Mat4 out{};
    for (int r = 0; r < 4; ++r) {
        for (int c = 0; c < 4; ++c) out.m[r][c] = w[r][4 + c];
    }
    return out;
}

void testWaterDraw(Harness& h) {
    rhi::Device& dev = h.dev;
    const int before = failures;
    const char* label = "water pixel shader (lake model)";
    if (!selected(label)) return;
    if (!devtools::D3DX::get().available()) {
        std::printf("[%s] %s: SKIP (d3dx9_39.dll not installed)\n", rhi::backendName(dev.caps().backend), label);
        return;
    }
    Blobs vs, ps;
    if (!translated(kWaterVsAsm, true, {}, vs, "parity_water_vs") || !compileBoth("water/water.frag.hlsl", {}, "PSMain", "ps_6_6", ps, 3)) {
        check(false, std::string(label) + ": shaders");
        return;
    }
    const auto smooth = [](std::uint32_t s, std::uint32_t x, std::uint32_t y, float k) {
        const float a = 0.5f + 0.45f * std::sin(0.9f * static_cast<float>(x) + 0.7f * static_cast<float>(s) + k);
        const float b = 0.5f + 0.45f * std::cos(0.8f * static_cast<float>(y) + 0.5f * static_cast<float>(s));
        const float c = 0.5f + 0.4f * std::sin(0.5f * static_cast<float>(x + y) + static_cast<float>(s));
        return std::array<std::uint8_t, 4>{static_cast<std::uint8_t>(a * 255), static_cast<std::uint8_t>(b * 255), static_cast<std::uint8_t>(c * 255), 255};
    };
    const rhi::SamplerIndex linearClamp = dev.createSampler({VK_FILTER_LINEAR, VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE});
    const rhi::SamplerIndex linearWrap = dev.createSampler({VK_FILTER_LINEAR, VK_SAMPLER_ADDRESS_MODE_REPEAT});
    const rhi::SamplerIndex pointClamp = dev.createSampler({VK_FILTER_NEAREST, VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE});
    const std::uint32_t cube = h.addCube(4, [&](std::uint32_t f, std::uint32_t x, std::uint32_t y) { return smooth(f + 20, x, y, 0.3f); }, "parity sky cube");
    std::uint32_t tex[20] = {};
    for (std::uint32_t slot : {10u, 11u, 12u, 13u, 14u}) {
        tex[slot] = h.addPattern(4, 4, [&](std::uint32_t x, std::uint32_t y) { return smooth(slot, x, y, 0.0f); }, "parity water texture");
    }
    const std::uint32_t refraction = h.addPattern(8, 8, [&](std::uint32_t x, std::uint32_t y) { return smooth(40, x, y, 0.1f); }, "parity refraction copy");
    // the depth map (view depth / far in .r): the ground a bit behind the water, a ridge across the middle for the SSR to hit
    const std::uint32_t depthMap = h.addPattern(8, 8,
                                                [&](std::uint32_t x, std::uint32_t y) {
                                                    const float d = (y == 3 || y == 4) ? 0.07f + 0.01f * static_cast<float>(x) : 0.11f + 0.004f * static_cast<float>(y);
                                                    return std::array<std::uint8_t, 4>{static_cast<std::uint8_t>(d * 255.0f), 0, 0, 255};
                                                },
                                                "parity depth map");
    if (cube == 0 || refraction == 0 || depthMap == 0 || linearClamp == 0 || linearWrap == 0 || pointClamp == 0) {
        check(false, std::string(label) + ": textures / samplers");
        return;
    }
    // the WaterBlock (enh/water.h: 192 bytes of DrawBlock + 71 float4 = 83 float4), written by float4 index (shaders/water/water_common.hlsli)
    std::vector<float> wb(83 * 4, 0.0f);
    const auto setF = [&](std::size_t i, float x, float y, float z, float w) {
        wb[i * 4] = x;
        wb[i * 4 + 1] = y;
        wb[i * 4 + 2] = z;
        wb[i * 4 + 3] = w;
    };
    const auto setU = [&](std::size_t i, std::uint32_t x, std::uint32_t y, std::uint32_t z, std::uint32_t w) {
        const std::uint32_t v[4] = {x, y, z, w};
        std::memcpy(&wb[i * 4], v, 16);
    };
    std::uint32_t slotTex[20] = {}, slotSmp[20] = {};
    slotTex[9] = cube;
    for (std::uint32_t slot : {10u, 11u, 12u, 13u, 14u}) slotTex[slot] = tex[slot];
    for (std::uint32_t slot = 9; slot < 15; ++slot) slotSmp[slot] = linearWrap;
    for (std::uint32_t i = 0; i < 5; ++i) {
        setU(i, slotTex[i * 4], slotTex[i * 4 + 1], slotTex[i * 4 + 2], slotTex[i * 4 + 3]);
        setU(5 + i, slotSmp[i * 4], slotSmp[i * 4 + 1], slotSmp[i * 4 + 2], slotSmp[i * 4 + 3]);
    }
    setF(12, 0.0f, 100.0f, -300.0f, 5000.0f);  // camFar
    setF(13, 0.5f, 0.6f, 0.7f, 1.0f);           // fogColor
    setF(14, 0.0f, 0.00005f, 0.6f, 0.0f);       // fogParams
    setF(15, 0.10f, 0.25f, 0.30f, 0.60f);       // c6 (water colour, distortion)
    setF(16, 0.90f, 0.70f, 0.80f, 0.35f);       // c7
    setF(17, 0.04f, 0.90f, 4.00f, 0.00f);       // c8 (fresnel)
    setF(18, 0.0006f, 0.10f, 1.20f, 0.00f);     // c9 (depth fade)
    setF(19, 0.30f, 0.70f, 3.00f, 0.00f);       // c10 (alpha)
    setF(20, 0.20f, 0.30f, 0.50f, 0.50f);       // c11
    setF(21, 0.30f, 0.20f, 0.10f, 0.40f);       // c12
    setF(23, 0.20f, 0.22f, 0.25f, 1.0f);        // indoor
    for (std::size_t k = 0; k < 7; ++k) setF(24 + k, 0.05f * static_cast<float>(k), 0.03f + 0.02f * static_cast<float>(k), 0.04f, 0.2f + 0.02f * static_cast<float>(k));
    setF(31, 2.0f, 1.9f, 1.6f, 1.0f);   // sun colour
    setF(32, 1.0f, 0.95f, 0.8f, 1.0f);  // sun specular
    setF(33, 0.42f, 0.78f, 0.46f, 0.0f);  // sun direction
    setF(47, 1.0f, 0.5f, 0.3f, 0.5f);            // look
    setF(48, 100.0f, 400.0f, 2500.0f, 1.0f);     // misc: near cap, coast band, SSR distance, murkiness
    setU(49, 0, 0, 0, 0);                        // res: flags 0 = not the grid, not the sea
    setU(50, 2u, 12u, refraction, depthMap);     // res2: WATER_MODEL_LAKE, SSR steps, refraction copy, depth map
    setU(51, linearClamp, linearWrap, pointClamp, 0);  // smpOurs
    abi::FrameBlock fb{};
    // a perspective camera at (0, 100, -300) looking along +z (the engine's row vectors: v' = v M, w = view z)
    const float fx = 1.2f, fy = 1.2f, zf = 5000.0f, zn = 10.0f, A = zf / (zf - zn), B = -zn * zf / (zf - zn);
    const Mat4 vp{{{fx, 0, 0, 0}, {0, fy, 0, 0}, {0, 0, A, 1}, {0, -100.0f * fy, 300.0f * A + B, 300.0f}}};
    const Mat4 ivp = inverse(vp);
    std::memcpy(fb.viewProj, vp.m, 64);
    std::memcpy(fb.invViewProj, ivp.m, 64);
    fb.cameraPos[1] = 100.0f;
    fb.cameraPos[2] = -300.0f;
    fb.cameraPos[3] = zf;
    fb.depthParams[0] = zn;
    fb.depthParams[1] = zf;
    fb.depthParams[2] = zf;
    fb.depthParams[3] = 1.0f / zf;
    fb.viewport[0] = fb.viewport[1] = static_cast<float>(kTarget);
    fb.viewport[2] = fb.viewport[3] = 1.0f / static_cast<float>(kTarget);
    fb.frameInfo[1] = abi::kFrameCameraValid;
    StageBlock vsc, psc;
    vsc.set(0, 500.0f, 0.0f, 500.0f, 0.0f);  // world x, z of the quad
    vsc.set(1, 0.0f, 0.0f, 400.0f, 0.85f);   // + offset (in front of the camera), global alpha
    vsc.set(2, 0.0f, 1.0f, 0.0f, 1.0f);      // the surface normal, outdoor factor
    vsc.set(3, 1.0f, 0.0f, 0.0f, 0.0f);      // tangent
    vsc.set(4, 0.0f, 0.0f, 1.0f, 0.0f);      // binormal
    vsc.set(5, 0.0f, 0.0f, 0.0f, 0.0f);
    vsc.set(6, 1.0f, 1.0f, 1.0f, 1.0f);
    vsc.set(7, 0.95f, 0.95f, 1.0f, 1.0f);
    vsc.set(8, 3.0f, 3.0f, 3.0f, 3.0f);  // the texture coordinates repeat 3x
    auto* bVs = h.makeBuffer(vsc.bytes.data(), vsc.bytes.size(), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, "parity vs consts");
    auto* bPs = h.makeBuffer(psc.bytes.data(), psc.bytes.size(), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, "parity ps consts");
    auto* bWater = h.makeBuffer(wb.data(), wb.size() * 4, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, "parity water block");
    auto* bFrame = h.makeBuffer(&fb, sizeof(fb), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, "parity frame block");
    if (!bVs || !bPs || !bWater || !bFrame) {
        check(false, std::string(label) + ": resources");
        return;
    }
    std::vector<Target> targets(1);
    if (!drawQuad(h, vs, ps, pushFor(*bVs, *bPs, *bWater, *bFrame), targets, "parity water")) {
        check(false, std::string(label) + ": pipeline / target");
        return;
    }
    const std::vector<std::uint8_t> px = pixelsOf(targets[0]);
    std::uint32_t lit = 0, changes = 0;
    for (std::size_t i = 0; i < px.size(); i += 4) lit += (px[i] != 0 || px[i + 1] != 0 || px[i + 2] != 0) ? 1u : 0u;
    for (std::size_t i = 4; i < px.size(); i += 4) changes += std::memcmp(&px[i], &px[i - 4], 3) != 0 ? 1u : 0u;
    const std::uint8_t* centre = px.data() + (static_cast<std::size_t>(kTarget / 2) * kTarget + kTarget / 2) * 4;
    check(lit > 1000 && changes > 300, std::format("{}: {} lit pixels, {} changes along the rows (centre ({}, {}, {}, {}))", label, lit, changes, centre[0], centre[1], centre[2], centre[3]));
    crossCheck(dev, label, toFloats(px.data(), px.size()), 4.0f, 0.004f);
    std::printf("[%s] %s: %s\n", rhi::backendName(dev.caps().backend), label, failures == before ? "PASS" : "FAIL");
}
