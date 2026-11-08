// Included by parity.cpp (anonymous namespace): S-7's luminance chain in one dispatch (shaders/post/post_lum.comp.hlsl, native::OwnPost). The scene is an RGBA16F
// texture read through the bindless heap (texel fetches), the parameter block is reached by reference, the two 1x1 R32F outputs are storage images. The
// result is compared with a CPU port of the engine's eight draws (LogLuminance, five DownSamples, AdaptiveLiminance, Copy) fed with the same stage
// parameters, and the two backends are cross-checked.

namespace postlum {

// mirror of native::PostLumParams (src/native/own_post.h)
struct Stage {
    float uv[4];
    float tap[4];
};
struct Params {
    std::uint32_t tex[4];
    Stage stage[7];
    float adapt[4];
};
static_assert(sizeof(Params) == 256);

struct Texel {
    int x, y;
};
Texel texelOf(float u, float v, int w, int h) {
    return {std::clamp(static_cast<int>(std::floor(u * static_cast<float>(w))), 0, w - 1), std::clamp(static_cast<int>(std::floor(v * static_cast<float>(h))), 0, h - 1)};
}

}  // namespace postlum

void testPostLum(Harness& h) {
    rhi::Device& dev = h.dev;
    const int before = failures;
    const char* label = "post luminance chain by compute";
    if (!selected(label)) return;
    using namespace postlum;
    Blobs cs;
    if (!compileBoth("post/post_lum.comp.hlsl", "", "CSMain", "cs_6_6", cs)) {
        check(false, "post_lum: compile");
        return;
    }
    constexpr std::uint32_t kW = 160, kH = 90;
    rhi::TextureDesc sd{VK_IMAGE_TYPE_2D, VK_FORMAT_R16G16B16A16_SFLOAT, {kW, kH, 1}, 1, 1, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, "post lum scene"};
    rhi::TextureDesc od{VK_IMAGE_TYPE_2D, VK_FORMAT_R32_SFLOAT, {1, 1, 1}, 1, 1, VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT, "post lum out"};
    auto scene = dev.createTexture(sd);
    auto prev = dev.createTexture(od);
    auto outA = dev.createTexture(od);
    auto outB = dev.createTexture(od);
    const std::uint32_t pitch = rhi::bufferRowPitch(sd.format, kW);
    auto staging = dev.createBuffer({static_cast<std::uint64_t>(pitch) * kH, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, rhi::Memory::Upload, "post lum staging"});
    auto prevStaging = dev.createBuffer({512, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, rhi::Memory::Upload, "post lum prev staging"});
    auto readback = dev.createBuffer({1024, VK_BUFFER_USAGE_TRANSFER_DST_BIT, rhi::Memory::Readback, "post lum readback"});
    Blobs cst;
    if (!compileBoth("post/post_lum_tile.comp.hlsl", "", "CSMain", "cs_6_6", cst)) {
        check(false, "post_lum_tile: compile");
        return;
    }
    auto pipeline = dev.createComputePipeline({cs.shader(), "post lum"});
    auto pipelineTile = dev.createComputePipeline({cst.shader(), "post lum tile"});
    rhi::TextureDesc td8{VK_IMAGE_TYPE_2D, VK_FORMAT_R32_SFLOAT, {8, 9, 1}, 1, 1, VK_IMAGE_USAGE_STORAGE_BIT, "post lum tile"};
    auto tileImg = dev.createTexture(td8);
    auto params = dev.createBuffer({sizeof(Params), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, rhi::Memory::Upload, "post lum params"});
    if (!scene || !prev || !outA || !outB || !staging || !prevStaging || !readback || !pipeline || !pipelineTile || !tileImg || !params) {
        check(false, "post_lum: resources / pipeline");
        return;
    }
    // an HDR-like scene (halves): a gradient, a bright patch and noise
    std::vector<float> img(static_cast<std::size_t>(kW) * kH * 4);
    std::uint32_t seed = 777u;
    for (std::uint32_t y = 0; y < kH; ++y) {
        for (std::uint32_t x = 0; x < kW; ++x) {
            for (int c = 0; c < 4; ++c) {
                seed = seed * 1664525u + 1013904223u;
                const float noise = static_cast<float>(seed >> 8) / 16777216.0f;
                const float v = 0.05f + 0.6f * static_cast<float>(x) / kW * (0.5f + 0.5f * static_cast<float>(c) / 3.0f) + (x > 100 && y < 30 ? 6.0f : 0.0f) + 0.3f * noise;
                const std::uint16_t hv = floatToHalf(v);
                std::memcpy(static_cast<std::uint8_t*>(staging->map()) + static_cast<std::size_t>(y) * pitch + (x * 4 + c) * 2, &hv, 2);
                img[(static_cast<std::size_t>(y) * kW + x) * 4 + c] = halfToF(hv);
            }
        }
    }
    const float prevValue = 0.9f;
    std::memcpy(prevStaging->map(), &prevValue, 4);
    // the stage parameters of an engine-like quad: uv at the pixel centres of a W x H target shifted by a quarter source texel (away from texel borders),
    // taps one source texel to the right / down
    Params p{};
    static constexpr int kDim[7] = {64, 32, 16, 8, 4, 2, 1};
    for (int s = 0; s < 7; ++s) {
        const int d = kDim[s];
        const float srcW = s == 0 ? static_cast<float>(kW) : static_cast<float>(2 * d), srcH = s == 0 ? static_cast<float>(kH) : static_cast<float>(2 * d);
        p.stage[s].uv[0] = 0.5f / static_cast<float>(d) - 0.25f / srcW;
        p.stage[s].uv[1] = 0.5f / static_cast<float>(d) - 0.25f / srcH;
        p.stage[s].uv[2] = 1.0f / static_cast<float>(d);
        p.stage[s].uv[3] = 1.0f / static_cast<float>(d);
        p.stage[s].tap[0] = 1.0f / srcW;
        p.stage[s].tap[1] = 1.0f / srcH;
    }
    p.adapt[0] = 0.3f;
    p.adapt[1] = 0.001f;
    p.adapt[2] = 1000.0f;
    p.tex[0] = scene->heapSampled();
    p.tex[1] = prev->heapSampled();
    std::memcpy(params->map(), &p, sizeof(p));
    struct Push {
        std::uint64_t frame;
        std::uint32_t passId, passFlags;
        std::uint64_t params;
        std::uint32_t pad[2];
    } push{};
    push.params = refOf(*params);
    dev.submitAndWait([&](rhi::CommandList& cl) {
        cl.transition(*scene, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        cl.copyBufferToTexture(*scene, *staging, 0);
        cl.transition(*scene, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        cl.transition(*prev, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        cl.copyBufferToTexture(*prev, *prevStaging, 0);
        cl.transition(*prev, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        cl.transition(*tileImg, VK_IMAGE_LAYOUT_GENERAL);
        cl.bindPipeline(*pipelineTile);
        rhi::Texture* const tileStorage[] = {tileImg.get()};
        cl.setStorageTextures(tileStorage);
        cl.setPushConstants(&push, sizeof(push));
        cl.dispatch(8, 8, 1);
        cl.computeBarrier();
        cl.transition(*outA, VK_IMAGE_LAYOUT_GENERAL);
        cl.transition(*outB, VK_IMAGE_LAYOUT_GENERAL);
        cl.bindPipeline(*pipeline);
        rhi::Texture* const storage[] = {outA.get(), outB.get(), tileImg.get()};
        cl.setStorageTextures(storage);
        cl.setPushConstants(&push, sizeof(push));
        cl.dispatch(1, 1, 1);
        cl.transition(*outA, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
        cl.transition(*outB, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
        cl.copyTextureToBuffer(*readback, 0, *outA);
        cl.copyTextureToBuffer(*readback, 512, *outB);
    });
    readback->invalidate();
    float gotA = 0, gotB = 0;
    std::memcpy(&gotA, readback->map(), 4);
    std::memcpy(&gotB, static_cast<std::uint8_t*>(readback->map()) + 512, 4);

    // CPU: the eight draws
    std::vector<float> cur(64 * 64);
    for (int y = 0; y < 64; ++y) {
        for (int x = 0; x < 64; ++x) {
            const float u = p.stage[0].uv[0] + p.stage[0].uv[2] * static_cast<float>(x), v = p.stage[0].uv[1] + p.stage[0].uv[3] * static_cast<float>(y);
            const float cx = p.stage[0].tap[0], cy = p.stage[0].tap[1];
            const float off[4][2] = {{cx, 0.0f}, {0.0f, 0.0f}, {cx, cy}, {0.0f, cy}};
            float a[3] = {0, 0, 0};
            for (const auto& o : off) {
                const Texel t = texelOf(u + o[0], v + o[1], static_cast<int>(kW), static_cast<int>(kH));
                for (int c = 0; c < 3; ++c) a[c] += img[(static_cast<std::size_t>(t.y) * kW + static_cast<std::size_t>(t.x)) * 4 + static_cast<std::size_t>(c)] * 0.25f;
            }
            const float lum = a[0] * 0.300000012f + a[1] * 0.589999974f + a[2] * 0.109999999f + 9.99999975e-06f;
            cur[static_cast<std::size_t>(y) * 64 + static_cast<std::size_t>(x)] = std::log2(lum) * 0.693147182f;
        }
    }
    int size = 64;
    for (int s = 1; s <= 5; ++s) {
        const int d = kDim[s];
        std::vector<float> next(static_cast<std::size_t>(d) * d);
        for (int y = 0; y < d; ++y) {
            for (int x = 0; x < d; ++x) {
                const float u = p.stage[s].uv[0] + p.stage[s].uv[2] * static_cast<float>(x), v = p.stage[s].uv[1] + p.stage[s].uv[3] * static_cast<float>(y);
                const float cx = p.stage[s].tap[0], cy = p.stage[s].tap[1];
                const float off[4][2] = {{cx, 0.0f}, {0.0f, 0.0f}, {cx, cy}, {0.0f, cy}};
                float a = 0;
                for (const auto& o : off) {
                    const Texel t = texelOf(u + o[0], v + o[1], size, size);
                    a += cur[static_cast<std::size_t>(t.y) * static_cast<std::size_t>(size) + static_cast<std::size_t>(t.x)] * 0.25f;
                }
                next[static_cast<std::size_t>(y) * d + x] = a;
            }
        }
        cur = std::move(next);
        size = d;
    }
    float want = 0;
    {
        const float u = p.stage[6].uv[0], v = p.stage[6].uv[1], cx = p.stage[6].tap[0], cy = p.stage[6].tap[1];
        const float off[4][2] = {{cx, 0.0f}, {0.0f, 0.0f}, {cx, cy}, {0.0f, cy}};
        float a = 0;
        for (const auto& o : off) {
            const Texel t = texelOf(u + o[0], v + o[1], 2, 2);
            a += cur[static_cast<std::size_t>(t.y) * 2 + static_cast<std::size_t>(t.x)] * 0.25f;
        }
        const float geo = std::exp2(a * 1.44269502f);
        const float c = p.adapt[0] * (geo - prevValue) + prevValue;
        want = std::min(p.adapt[2], std::max(c, p.adapt[1]));
    }
    const float relA = std::fabs(gotA - want) / std::max(std::fabs(want), 1e-9f), relB = std::fabs(gotB - want) / std::max(std::fabs(want), 1e-9f);
    check(relA < 2e-4f && relB < 2e-4f, std::format("post_lum: adapted luminance {} / {} against the CPU chain {}", gotA, gotB, want));
    std::printf("  post lum: adapt out %.9g, avg out %.9g, CPU %.9g (rel %.3g / %.3g)\n", gotA, gotB, want, relA, relB);
    crossCheck(dev, "post lum", {gotA, gotB}, 2e-5f);
    std::printf("[%s] %s: %s\n", rhi::backendName(dev.caps().backend), label, failures == before ? "PASS" : "FAIL");
}
