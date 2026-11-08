// Included by parity.cpp (anonymous namespace): S-7b's bloom chain (post_bright / post_glow / post_bloom.comp.hlsl) and DoF middle (post_dofmid.comp.hlsl, native::OwnPost),
// each run through the bindless heap and storage images on the backend and compared with a CPU port written as the engine's draws are (stage by stage, each stage a full
// image rounded to 8 bits, taps by a bilinear sampler), plus the cross-check of the two backends.

namespace postgroups {

struct BrightParams {
    std::uint32_t tex[4];
    float uv[4], tap[4], ps[4];
    std::uint32_t size[4];
};
struct GlowParams {
    std::uint32_t tex[4];
    float uvH[4], centerH[4], offH[6][4];
    float uvS[4], centerS[4], light[4], sw[4];
};
struct BloomParams {
    std::uint32_t tex[4];
    std::uint32_t size[4];
    float uv[4], center[4], off[6][4], blend[4];
};
struct DofParams {
    std::uint32_t tex[4];
    std::uint32_t size[4];
};
static_assert(sizeof(BrightParams) == 80 && sizeof(GlowParams) == 208 && sizeof(BloomParams) == 176 && sizeof(DofParams) == 32);

struct Push {
    std::uint64_t frame;
    std::uint32_t passId, passFlags;
    std::uint64_t params;
    std::uint32_t pad[2];
};

struct Img {
    int w = 0, h = 0;
    std::vector<float> px;  // rgba, 0..1
    float* at(int x, int y) { return &px[(static_cast<std::size_t>(y) * w + x) * 4]; }
    const float* at(int x, int y) const { return &px[(static_cast<std::size_t>(y) * w + x) * 4]; }
};

// a bilinear tap with clamp addressing (the engine's sampler states)
std::array<float, 4> bilerp(const Img& im, float u, float v) {
    const float x = u * static_cast<float>(im.w) - 0.5f, y = v * static_cast<float>(im.h) - 0.5f;
    const int x0 = static_cast<int>(std::floor(x)), y0 = static_cast<int>(std::floor(y));
    const float fx = x - static_cast<float>(x0), fy = y - static_cast<float>(y0);
    const auto t = [&](int xx, int yy) { return im.at(std::clamp(xx, 0, im.w - 1), std::clamp(yy, 0, im.h - 1)); };
    std::array<float, 4> r{};
    for (int c = 0; c < 4; ++c) {
        const float a = t(x0, y0)[c] * (1 - fx) + t(x0 + 1, y0)[c] * fx, b = t(x0, y0 + 1)[c] * (1 - fx) + t(x0 + 1, y0 + 1)[c] * fx;
        r[static_cast<std::size_t>(c)] = a * (1 - fy) + b * fy;
    }
    return r;
}
float q8(float v) { return std::floor(std::clamp(v, 0.0f, 1.0f) * 255.0f + 0.5f) / 255.0f; }

// uploads an RGBA8 image (bytes) into a texture; returns nothing, the caller records the copy
void fillRgba8(rhi::Buffer& staging, std::uint32_t pitch, const Img& im) {
    for (int y = 0; y < im.h; ++y) {
        for (int x = 0; x < im.w; ++x) {
            for (int c = 0; c < 4; ++c) static_cast<std::uint8_t*>(staging.map())[static_cast<std::size_t>(y) * pitch + static_cast<std::size_t>(x) * 4 + static_cast<std::size_t>(c)] = static_cast<std::uint8_t>(std::lround(im.at(x, y)[c] * 255.0f));
        }
    }
}

struct Stats {
    int maxStep = 0;
    double mean = 0;
};
Stats compareRgba8(const std::uint8_t* got, std::uint32_t pitch, const Img& want, std::vector<float>* all) {
    Stats s;
    double sum = 0;
    for (int y = 0; y < want.h; ++y) {
        for (int x = 0; x < want.w; ++x) {
            for (int c = 0; c < 4; ++c) {
                const int g = got[static_cast<std::size_t>(y) * pitch + static_cast<std::size_t>(x) * 4 + static_cast<std::size_t>(c)];
                const int w = static_cast<int>(std::lround(want.at(x, y)[c] * 255.0f));
                s.maxStep = std::max(s.maxStep, std::abs(g - w));
                sum += std::abs(g - w);
                if (all != nullptr) all->push_back(static_cast<float>(g));
            }
        }
    }
    s.mean = sum / (static_cast<double>(want.w) * want.h * 4);
    return s;
}

}  // namespace postgroups

void testPostBloom(Harness& h) {
    rhi::Device& dev = h.dev;
    const int before = failures;
    const char* label = "post bloom chain by compute";
    if (!selected(label)) return;
    using namespace postgroups;
    Blobs cb, cg, cl, cf;
    if (!compileBoth("post/post_bright.comp.hlsl", "", "CSMain", "cs_6_6", cb) || !compileBoth("post/post_glow.comp.hlsl", "", "CSMain", "cs_6_6", cg) ||
        !compileBoth("post/post_bloom.comp.hlsl", "", "CSMain", "cs_6_6", cl)) {
        check(false, "post bloom: compile");
        return;
    }
    (void)cf;
    constexpr int kSW = 160, kSH = 90, kW = 64, kH = 48;
    constexpr VkImageUsageFlags kStore = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    auto mk = [&](VkFormat f, int w, int hh, VkImageUsageFlags u, const char* n) { return dev.createTexture({VK_IMAGE_TYPE_2D, f, {static_cast<std::uint32_t>(w), static_cast<std::uint32_t>(hh), 1}, 1, 1, u, n}); };
    auto scene = mk(VK_FORMAT_R16G16B16A16_SFLOAT, kSW, kSH, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, "bloom scene");
    auto depth = mk(VK_FORMAT_R32_SFLOAT, kSW, kSH, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, "bloom depth");
    auto avg = mk(VK_FORMAT_R32_SFLOAT, 1, 1, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, "bloom avg");
    auto bp = mk(VK_FORMAT_R8G8B8A8_UNORM, kW, kH, kStore, "bloom bp");
    auto gh = mk(VK_FORMAT_R8G8B8A8_UNORM, kW, kH, kStore, "bloom glowH");
    auto st = mk(VK_FORMAT_R8G8B8A8_UNORM, kW, kH, kStore, "bloom streak");
    auto out = mk(VK_FORMAT_R8G8B8A8_UNORM, kW, kH, kStore, "bloom out");
    const std::uint32_t pitchS = rhi::bufferRowPitch(VK_FORMAT_R16G16B16A16_SFLOAT, kSW), pitchD = rhi::bufferRowPitch(VK_FORMAT_R32_SFLOAT, kSW);
    const std::uint32_t pitch8 = rhi::bufferRowPitch(VK_FORMAT_R8G8B8A8_UNORM, kW);
    auto stS = dev.createBuffer({static_cast<std::uint64_t>(pitchS) * kSH, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, rhi::Memory::Upload, "bloom st scene"});
    auto stD = dev.createBuffer({static_cast<std::uint64_t>(pitchD) * kSH, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, rhi::Memory::Upload, "bloom st depth"});
    auto stA = dev.createBuffer({512, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, rhi::Memory::Upload, "bloom st avg"});
    auto stO = dev.createBuffer({static_cast<std::uint64_t>(pitch8) * kH, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, rhi::Memory::Upload, "bloom st out"});
    auto rb = dev.createBuffer({static_cast<std::uint64_t>(pitch8) * kH, VK_BUFFER_USAGE_TRANSFER_DST_BIT, rhi::Memory::Readback, "bloom readback"});
    auto pb = dev.createBuffer({sizeof(BrightParams), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, rhi::Memory::Upload, "bloom p1"});
    auto pg = dev.createBuffer({sizeof(GlowParams), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, rhi::Memory::Upload, "bloom p2"});
    auto pl = dev.createBuffer({sizeof(BloomParams), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, rhi::Memory::Upload, "bloom p3"});
    auto k1 = dev.createComputePipeline({cb.shader(), "post bright"});
    auto k2 = dev.createComputePipeline({cg.shader(), "post glow"});
    auto k3 = dev.createComputePipeline({cl.shader(), "post bloom"});
    if (!scene || !depth || !avg || !bp || !gh || !st || !out || !stS || !stD || !stA || !stO || !rb || !pb || !pg || !pl || !k1 || !k2 || !k3) {
        check(false, "post bloom: resources / pipelines");
        return;
    }
    const rhi::SamplerIndex linear = dev.createSampler({VK_FILTER_LINEAR, VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE});
    // inputs: an HDR-like scene (halves), a depth map with a sky (1.0) band, the adapted luminance, a previous bloom image
    std::vector<float> sc(static_cast<std::size_t>(kSW) * kSH * 4), dp(static_cast<std::size_t>(kSW) * kSH);
    std::uint32_t seed = 4242u;
    const auto rnd = [&]() {
        seed = seed * 1664525u + 1013904223u;
        return static_cast<float>(seed >> 8) / 16777216.0f;
    };
    for (int y = 0; y < kSH; ++y) {
        for (int x = 0; x < kSW; ++x) {
            for (int c = 0; c < 4; ++c) {
                const float v = 0.2f + 2.5f * static_cast<float>(x) / kSW * (0.4f + 0.2f * static_cast<float>(c)) + (x > 90 && y < 25 ? 9.0f : 0.0f) + 1.2f * rnd();
                const std::uint16_t hv = floatToHalf(v);
                std::memcpy(static_cast<std::uint8_t*>(stS->map()) + static_cast<std::size_t>(y) * pitchS + (x * 4 + c) * 2, &hv, 2);
                sc[(static_cast<std::size_t>(y) * kSW + x) * 4 + c] = halfToF(hv);
            }
            const float d = (y < 30 && (x / 9) % 3 != 1) ? 1.0f : 0.4f + 0.3f * rnd();
            std::memcpy(static_cast<std::uint8_t*>(stD->map()) + static_cast<std::size_t>(y) * pitchD + x * 4, &d, 4);
            dp[static_cast<std::size_t>(y) * kSW + x] = d;
        }
    }
    const float avgLum = 0.45f;
    std::memcpy(stA->map(), &avgLum, 4);
    Img prevBloom;
    prevBloom.w = kW;
    prevBloom.h = kH;
    prevBloom.px.resize(static_cast<std::size_t>(kW) * kH * 4);
    for (std::size_t i = 0; i < prevBloom.px.size(); ++i) prevBloom.px[i] = std::round(rnd() * 255.0f) / 255.0f;
    fillRgba8(*stO, pitch8, prevBloom);
    // parameters (an engine-like quad: uv = p / size + a small shift away from the source's texel borders)
    BrightParams b{};
    b.uv[0] = 0.31f / kSW;
    b.uv[1] = 0.37f / kSH;
    b.uv[2] = 1.0f / kW;
    b.uv[3] = 1.0f / kH;
    b.tap[0] = 1.0f / kSW;
    b.tap[1] = 1.0f / kSH;
    b.tap[2] = 0.5f / kSW;
    b.tap[3] = 0.5f / kSH;
    b.ps[0] = 0.6f;
    b.ps[1] = 0.3f;
    b.ps[2] = 0.25f;
    b.ps[3] = 1.0f / 0.8f;
    b.size[0] = kW;
    b.size[1] = kH;
    b.tex[0] = scene->heapSampled();
    b.tex[1] = avg->heapSampled();
    b.tex[2] = depth->heapSampled();
    GlowParams g{};
    g.tex[0] = bp->heapSampled();
    g.tex[1] = linear;
    g.tex[2] = kW;
    g.tex[3] = kH;
    for (int k = 0; k < 2; ++k) {
        g.uvH[k] = 0.0f;
        g.uvS[k] = 0.0f;
    }
    g.uvH[2] = g.uvS[2] = 1.0f / kW;
    g.uvH[3] = g.uvS[3] = 1.0f / kH;
    g.centerH[0] = g.centerS[0] = 0.5f / kW;
    g.centerH[1] = g.centerS[1] = 0.5f / kH;
    for (int i = 0; i < 6; ++i) g.offH[i][0] = (0.5f + 2.0f * static_cast<float>(i)) / kW;
    g.light[0] = 0.3f;
    g.light[1] = 0.4f;
    g.light[2] = 0.7f / 64.0f;
    g.light[3] = 1.1f;
    g.sw[0] = 0.2f;
    g.sw[1] = 0.98f;
    BloomParams l{};
    l.tex[0] = gh->heapSampled();
    l.tex[1] = st->heapSampled();
    l.tex[2] = linear;
    l.tex[3] = 0;
    l.size[0] = kW;
    l.size[1] = kH;
    l.uv[2] = 1.0f / kW;
    l.uv[3] = 1.0f / kH;
    l.center[0] = 0.5f / kW;
    l.center[1] = 0.5f / kH;
    for (int i = 0; i < 6; ++i) l.off[i][1] = (0.5f + 2.0f * static_cast<float>(i)) / kH;
    l.blend[0] = 0.35f;
    std::memcpy(pb->map(), &b, sizeof(b));
    std::memcpy(pg->map(), &g, sizeof(g));
    std::memcpy(pl->map(), &l, sizeof(l));
    Push p1{}, p2{}, p3{};
    p1.params = refOf(*pb);
    p2.params = refOf(*pg);
    p3.params = refOf(*pl);
    dev.submitAndWait([&](rhi::CommandList& cmd) {
        cmd.transition(*scene, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        cmd.copyBufferToTexture(*scene, *stS, 0);
        cmd.transition(*scene, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        cmd.transition(*depth, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        cmd.copyBufferToTexture(*depth, *stD, 0);
        cmd.transition(*depth, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        cmd.transition(*avg, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        cmd.copyBufferToTexture(*avg, *stA, 0);
        cmd.transition(*avg, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        cmd.transition(*out, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        cmd.copyBufferToTexture(*out, *stO, 0);
        cmd.transition(*bp, VK_IMAGE_LAYOUT_GENERAL);
        cmd.bindPipeline(*k1);
        rhi::Texture* const s1[] = {bp.get()};
        cmd.setStorageTextures(s1);
        cmd.setPushConstants(&p1, sizeof(p1));
        cmd.dispatch((kW + 7) / 8, (kH + 7) / 8, 1);
        cmd.transition(*bp, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        cmd.transition(*gh, VK_IMAGE_LAYOUT_GENERAL);
        cmd.transition(*st, VK_IMAGE_LAYOUT_GENERAL);
        cmd.bindPipeline(*k2);
        rhi::Texture* const s2[] = {gh.get(), st.get()};
        cmd.setStorageTextures(s2);
        cmd.setPushConstants(&p2, sizeof(p2));
        cmd.dispatch((kW + 7) / 8, (kH + 7) / 8, 1);
        cmd.transition(*gh, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        cmd.transition(*st, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        cmd.transition(*out, VK_IMAGE_LAYOUT_GENERAL);
        cmd.bindPipeline(*k3);
        rhi::Texture* const s3[] = {out.get()};
        cmd.setStorageTextures(s3);
        cmd.setPushConstants(&p3, sizeof(p3));
        cmd.dispatch((kW + 7) / 8, (kH + 7) / 8, 1);
        cmd.transition(*out, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
        cmd.copyTextureToBuffer(*rb, 0, *out);
    });
    rb->invalidate();

    // ---- CPU: the four draws ----
    const auto tex = [&](float u, float v, int w, int hh) {
        return std::pair<int, int>{std::clamp(static_cast<int>(std::floor(u * static_cast<float>(w))), 0, w - 1), std::clamp(static_cast<int>(std::floor(v * static_cast<float>(hh))), 0, hh - 1)};
    };
    Img BP{kW, kH, std::vector<float>(static_cast<std::size_t>(kW) * kH * 4)};
    for (int y = 0; y < kH; ++y) {
        for (int x = 0; x < kW; ++x) {
            const float u = b.uv[0] + b.uv[2] * static_cast<float>(x), v = b.uv[1] + b.uv[3] * static_cast<float>(y);
            const float off[4][2] = {{b.tap[0], 0.0f}, {0.0f, 0.0f}, {b.tap[0], b.tap[1]}, {0.0f, b.tap[1]}};
            float a[3] = {0, 0, 0};
            for (const auto& o : off) {
                const auto t = tex(u + o[0], v + o[1], kSW, kSH);
                for (int c = 0; c < 3; ++c) a[c] = sc[(static_cast<std::size_t>(t.second) * kSW + t.first) * 4 + c] * 0.25f + a[c];
            }
            const float w = (1.0f / avgLum) * b.ps[0];
            float* o = BP.at(x, y);
            for (int c = 0; c < 3; ++c) {
                const float cc = a[c] * w;
                const float m = std::max(cc * (cc * b.ps[3] + 1.0f) - b.ps[1], 0.0f);
                o[c] = q8(m * (1.0f / (m + b.ps[2])));
            }
            const auto td = tex(u + b.tap[2], v + b.tap[3], kSW, kSH);
            o[3] = q8(dp[static_cast<std::size_t>(td.second) * kSW + td.first] - 1.0f >= 0.0f ? 1.0f : 0.0f);
        }
    }
    static constexpr float kWt[7] = {0.265961498f, 0.251588792f, 0.21296531f, 0.161313802f, 0.109340027f, 0.0663180873f, 0.0359939747f};
    Img H{kW, kH, std::vector<float>(static_cast<std::size_t>(kW) * kH * 4)}, S = H, R = prevBloom;
    for (int y = 0; y < kH; ++y) {
        for (int x = 0; x < kW; ++x) {
            const float cu = g.uvH[2] * static_cast<float>(x) + g.centerH[0], cv = g.uvH[3] * static_cast<float>(y) + g.centerH[1];
            float v[7][2] = {};
            for (int i = 0; i < 6; ++i) v[i][0] = g.offH[i][0];
            std::array<float, 4> r{};
            const auto add = [&](float du, float dv, float w) {
                const auto t = bilerp(BP, cu + du, cv + dv);
                for (std::size_t c = 0; c < 4; ++c) r[c] = t[c] * w + r[c];
            };
            add(v[1][0], v[1][1], kWt[1]);
            add(v[0][0], v[0][1], kWt[0]);
            for (int k = 2; k < 7; ++k) add(v[k][0], v[k][1], kWt[k]);
            for (int m = 0; m < 7; ++m) add(-v[m][0], -v[m][1], kWt[m]);
            for (int c = 0; c < 4; ++c) H.at(x, y)[c] = q8(r[static_cast<std::size_t>(c)]);
            // streak
            const float v0u = g.uvS[2] * static_cast<float>(x) + g.centerS[0], v0v = g.uvS[3] * static_cast<float>(y) + g.centerS[1];
            const float du = v0u - g.light[0], dv = v0v - g.light[1];
            auto t0 = bilerp(BP, v0u, v0v);
            for (int c = 0; c < 3; ++c) t0[static_cast<std::size_t>(c)] *= t0[3];
            std::array<float, 4> acc = t0;
            float pu = v0u, pv = v0v, atten = 1.0f;
            for (int i = 0; i < 64; ++i) {
                pu = du * (-g.light[2]) + pu;
                pv = dv * (-g.light[2]) + pv;
                auto t = bilerp(BP, pu, pv);
                for (int c = 0; c < 3; ++c) t[static_cast<std::size_t>(c)] *= t[3];
                for (std::size_t c = 0; c < 4; ++c) acc[c] = t[c] * atten * g.sw[0] + acc[c];
                atten *= g.sw[1];
            }
            for (int c = 0; c < 3; ++c) S.at(x, y)[c] = q8(acc[static_cast<std::size_t>(c)] * g.light[3]);
            S.at(x, y)[3] = q8(acc[3]);
        }
    }
    for (int y = 0; y < kH; ++y) {
        for (int x = 0; x < kW; ++x) {
            const float cu = l.uv[2] * static_cast<float>(x) + l.center[0], cv = l.uv[3] * static_cast<float>(y) + l.center[1];
            float v[7][2] = {};
            for (int i = 0; i < 6; ++i) v[i][1] = l.off[i][1];
            std::array<float, 3> r{};
            const auto add = [&](float du, float dv, float w) {
                const auto t = bilerp(H, cu + du, cv + dv);
                for (std::size_t c = 0; c < 3; ++c) r[c] = t[c] * w + r[c];
            };
            add(v[1][0], v[1][1], kWt[1]);
            add(v[0][0], v[0][1], kWt[0]);
            for (int k = 2; k < 7; ++k) add(v[k][0], v[k][1], kWt[k]);
            for (int m = 0; m < 7; ++m) add(-v[m][0], -v[m][1], kWt[m]);
            const auto s = bilerp(S, cu, cv);
            const float a = l.blend[0];
            const float src[4] = {std::clamp(r[0] + s[0], 0.0f, 1.0f), std::clamp(r[1] + s[1], 0.0f, 1.0f), std::clamp(r[2] + s[2], 0.0f, 1.0f), a};
            for (int c = 0; c < 4; ++c) R.at(x, y)[c] = q8(src[c] * a + prevBloom.at(x, y)[c] * (1.0f - a));
        }
    }
    std::vector<float> all;
    const Stats s = compareRgba8(static_cast<const std::uint8_t*>(rb->map()), pitch8, R, &all);
    // (the streak's 64 bilinear taps at arbitrary positions amplify the sampler's fixed-point weights: a few steps against a float CPU port)
    check(s.maxStep <= 8 && s.mean < 0.8, std::format("post bloom: bloom image against the CPU chain: max {} steps, mean {:.3f}", s.maxStep, s.mean));
    std::printf("  post bloom: max %d steps, mean %.3f steps against the CPU chain\n", s.maxStep, s.mean);
    crossCheck(dev, "post bloom", all, 2.5f, 0.001f);
    std::printf("[%s] %s: %s\n", rhi::backendName(dev.caps().backend), label, failures == before ? "PASS" : "FAIL");
}

void testPostDofMid(Harness& h) {
    rhi::Device& dev = h.dev;
    const int before = failures;
    const char* label = "post DoF middle by compute";
    if (!selected(label)) return;
    using namespace postgroups;
    Blobs cd;
    if (!compileBoth("post/post_dofmid.comp.hlsl", "", "CSMain", "cs_6_6", cd)) {
        check(false, "post dofmid: compile");
        return;
    }
    constexpr int kW = 37, kH = 29;  // not multiples of the 8 x 8 tile: the borders
    constexpr VkImageUsageFlags kStore = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    auto mk = [&](VkFormat f, VkImageUsageFlags u, const char* n) { return dev.createTexture({VK_IMAGE_TYPE_2D, f, {kW, kH, 1}, 1, 1, u, n}); };
    auto dsc = mk(VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, "dof dsc");
    auto outV = mk(VK_FORMAT_R8G8B8A8_UNORM, kStore, "dof outV");
    auto outS = mk(VK_FORMAT_R8G8B8A8_UNORM, kStore, "dof outS");
    const std::uint32_t pitch = rhi::bufferRowPitch(VK_FORMAT_R8G8B8A8_UNORM, kW);
    const std::uint64_t imgBytes = (static_cast<std::uint64_t>(pitch) * kH + 511) & ~std::uint64_t{511};
    auto st = dev.createBuffer({imgBytes, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, rhi::Memory::Upload, "dof staging"});
    auto rb = dev.createBuffer({imgBytes * 2, VK_BUFFER_USAGE_TRANSFER_DST_BIT, rhi::Memory::Readback, "dof readback"});
    auto pb = dev.createBuffer({sizeof(DofParams), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, rhi::Memory::Upload, "dof params"});
    auto k = dev.createComputePipeline({cd.shader(), "post dofmid"});
    if (!dsc || !outV || !outS || !st || !rb || !pb || !k) {
        check(false, "post dofmid: resources / pipeline");
        return;
    }
    Img D{kW, kH, std::vector<float>(static_cast<std::size_t>(kW) * kH * 4)};
    std::uint32_t seed = 99u;
    for (int y = 0; y < kH; ++y) {
        for (int x = 0; x < kW; ++x) {
            for (int c = 0; c < 4; ++c) {
                seed = seed * 1664525u + 1013904223u;
                const float v = c == 3 ? ((x / 6 + y / 5) % 2 == 0 ? 0.9f * static_cast<float>(x) / kW : 0.1f + 0.05f * static_cast<float>((seed >> 12) & 7)) : static_cast<float>(seed >> 8) / 16777216.0f;
                D.at(x, y)[c] = std::round(v * 255.0f) / 255.0f;
            }
        }
    }
    fillRgba8(*st, pitch, D);
    DofParams p{};
    p.tex[0] = dsc->heapSampled();
    p.size[0] = kW;
    p.size[1] = kH;
    std::memcpy(pb->map(), &p, sizeof(p));
    Push push{};
    push.params = refOf(*pb);
    dev.submitAndWait([&](rhi::CommandList& cmd) {
        cmd.transition(*dsc, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        cmd.copyBufferToTexture(*dsc, *st, 0);
        cmd.transition(*dsc, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        cmd.transition(*outV, VK_IMAGE_LAYOUT_GENERAL);
        cmd.transition(*outS, VK_IMAGE_LAYOUT_GENERAL);
        cmd.bindPipeline(*k);
        rhi::Texture* const s[] = {outV.get(), outS.get()};
        cmd.setStorageTextures(s);
        cmd.setPushConstants(&push, sizeof(push));
        cmd.dispatch((kW + 7) / 8, (kH + 7) / 8, 1);
        cmd.transition(*outV, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
        cmd.transition(*outS, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
        cmd.copyTextureToBuffer(*rb, 0, *outV);
        cmd.copyTextureToBuffer(*rb, imgBytes, *outS);
    });
    rb->invalidate();
    // ---- CPU: the four draws, one full image each, taps by the bilinear sampler (uv = p / size: the engine's quad, no half texel) ----
    const float dx = 1.0f / kW, dy = 1.0f / kH;
    const auto stage = [&](const Img& src, const std::function<void(const Img&, int, int, float*)>& f) {
        Img o{kW, kH, std::vector<float>(static_cast<std::size_t>(kW) * kH * 4)};
        for (int y = 0; y < kH; ++y) {
            for (int x = 0; x < kW; ++x) f(src, x, y, o.at(x, y));
        }
        return o;
    };
    const Img Hh = stage(D, [&](const Img& s, int x, int y, float* o) {
        const float u = static_cast<float>(x) * dx, v = static_cast<float>(y) * dy;
        auto a = bilerp(s, u + dx, v);
        const auto b = bilerp(s, u - dx, v);
        for (std::size_t c = 0; c < 4; ++c) a[c] += b[c];
        const auto c3 = bilerp(s, u + 3 * dx, v);
        for (std::size_t c = 0; c < 4; ++c) a[c] += c3[c];
        const auto d3 = bilerp(s, u - 3 * dx, v);
        for (std::size_t c = 0; c < 4; ++c) o[c] = q8((a[c] + d3[c]) * 0.25f);
    });
    const Img V = stage(Hh, [&](const Img& s, int x, int y, float* o) {
        const float u = static_cast<float>(x) * dx, v = static_cast<float>(y) * dy;
        auto a = bilerp(s, u, v + dy);
        const auto b = bilerp(s, u, v - dy);
        for (std::size_t c = 0; c < 4; ++c) a[c] += b[c];
        const auto c3 = bilerp(s, u, v + 3 * dy);
        for (std::size_t c = 0; c < 4; ++c) a[c] += c3[c];
        const auto d3 = bilerp(s, u, v - 3 * dy);
        for (std::size_t c = 0; c < 4; ++c) o[c] = q8((a[c] + d3[c]) * 0.25f);
    });
    const Img N = stage(V, [&](const Img& s, int x, int y, float* o) {
        const float* bv = s.at(x, y);
        const float* d = D.at(x, y);
        o[0] = q8(d[0]);
        o[1] = q8(d[1]);
        o[2] = q8(d[2]);
        o[3] = q8(2.0f * std::max(bv[3], d[3]) - d[3]);
    });
    const Img Sm = stage(N, [&](const Img& s, int x, int y, float* o) {
        const float u = static_cast<float>(x) * dx, v = static_cast<float>(y) * dy;
        auto a = bilerp(s, u, v);
        const auto b = bilerp(s, u + dx, v);
        for (std::size_t c = 0; c < 4; ++c) a[c] += b[c];
        const auto c1 = bilerp(s, u, v + dy);
        for (std::size_t c = 0; c < 4; ++c) a[c] += c1[c];
        const auto d1 = bilerp(s, u + dx, v + dy);
        for (std::size_t c = 0; c < 4; ++c) o[c] = q8((a[c] + d1[c]) * 0.25f);
    });
    std::vector<float> all;
    const Stats sv = compareRgba8(static_cast<const std::uint8_t*>(rb->map()), pitch, V, &all);
    const Stats ss = compareRgba8(static_cast<const std::uint8_t*>(rb->map()) + imgBytes, pitch, Sm, &all);
    // (ties of the 8-bit rounding may go either way between the CPU port and the kernel's own rounding of the intermediates: a step or two)
    check(sv.maxStep <= 2 && ss.maxStep <= 3 && sv.mean < 0.1 && ss.mean < 0.15, std::format("post dofmid: BlurV max {} mean {:.3f}, SmallBlur max {} mean {:.3f} steps against the CPU chain", sv.maxStep, sv.mean, ss.maxStep, ss.mean));
    std::printf("  post dofmid: BlurV max %d mean %.3f, SmallBlur max %d mean %.3f steps against the CPU chain\n", sv.maxStep, sv.mean, ss.maxStep, ss.mean);
    crossCheck(dev, "post dofmid", all, 1.5f, 0.0005f);
    std::printf("[%s] %s: %s\n", rhi::backendName(dev.caps().backend), label, failures == before ? "PASS" : "FAIL");
}
