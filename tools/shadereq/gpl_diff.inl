// rvk_shadereq --gpl (included into main.cpp's anonymous namespace): the fast link, the link-time optimised link and the monolithic
// build of one pipeline description, rendered and compared (frame-ownership.md 16.27).
//
//   rvk_shadereq --gpl <list.txt of fragment shader .spv> --vs <gpl_varyings.spv> [--format f32|f16|u8|srgb] [--depth] [--trials N]
//                [--alpha-func F --alpha-ref R] [--no-lto] [--cross] [--verbose]
//
// Variants: A = fast link, B = a second fast link (control), C = LTO link, D = monolithic, E = a second monolithic (control).
// A trial = random constants, random 16x16 textures, one big triangle over 32x32 pixels whose varyings vary (shaders/test/gpl_varyings.vert).

float halfToFloat(std::uint16_t h) {
    const std::uint32_t sign = (h >> 15) & 1u, exp = (h >> 10) & 0x1fu, mant = h & 0x3ffu;
    const float v = exp == 0 ? std::ldexp(static_cast<float>(mant), -24)
                             : exp == 31 ? INFINITY : std::ldexp(static_cast<float>(mant | 0x400u), static_cast<int>(exp) - 25);
    return sign != 0 ? -v : v;
}

struct GplOptions {
    std::string vs, list, format = "f32";
    int trials = 4;
    std::uint32_t seed = 1;
    bool depth = false, validation = true, verbose = false, noLto = false, cross = false;
    std::uint32_t alphaFunc = 8;  // D3DCMP (8 = always); 5 = greater than alphaRef (0..255)
    float alphaRef = 100.0f;
};

int gplDiff(const GplOptions& o) {
    constexpr std::uint32_t kN = 32;
    vk::Context ctx;
    if (!ctx.create({o.validation, -1, "rvk_shadereq"})) return 2;
    vk::Frames frames;
    vk::Bindless bindless;
    vk::PipelineCache pipes;
    if (!frames.create(ctx) || !bindless.create(ctx, frames) || !pipes.create(ctx, bindless.pipelineLayout(), {}, true)) return 2;
    if (!pipes.usesLibraries()) {
        std::printf("pipeline libraries are not used on this device\n");
        return 2;
    }
    VkFormat fmt = VK_FORMAT_R32G32B32A32_SFLOAT;
    std::uint32_t bpp = 16;
    if (o.format == "f16") {
        fmt = VK_FORMAT_R16G16B16A16_SFLOAT;
        bpp = 8;
    } else if (o.format == "u8") {
        fmt = VK_FORMAT_R8G8B8A8_UNORM;
        bpp = 4;
    } else if (o.format == "srgb") {
        fmt = VK_FORMAT_B8G8R8A8_SRGB;
        bpp = 4;
    }
    const VkShaderModule vs = vk::createShaderModule(ctx, readSpv(o.vs));
    if (!vs) return 2;

    constexpr std::uint32_t kTex = 8, kTexSz = 16;
    vk::Image tex[kTex];
    for (auto& t : tex) {
        t.create(ctx, {VK_IMAGE_TYPE_2D, VK_FORMAT_R8G8B8A8_UNORM, {kTexSz, kTexSz, 1}, 1, 1, false,
                       VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, {}, "gpl texture"});
    }
    VkSampler sampler = VK_NULL_HANDLE;
    VkSamplerCreateInfo sci{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    sci.magFilter = sci.minFilter = VK_FILTER_LINEAR;
    sci.addressModeU = sci.addressModeV = sci.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    vkCreateSampler(ctx.device(), &sci, nullptr, &sampler);
    const std::uint32_t samplerSlot = bindless.addSampler(sampler);
    std::uint32_t texSlot[kTex];
    for (std::uint32_t i = 0; i < kTex; ++i) texSlot[i] = bindless.addTexture(vk::TextureKind::Tex2D, tex[i].view());

    constexpr int kVariants = 5;
    vk::Image rt[kVariants];
    vk::Image depth;
    VkBuffer readback[kVariants] = {};
    VmaAllocation readbackAlloc[kVariants] = {};
    VmaAllocationInfo readbackInfo[kVariants] = {};
    for (int i = 0; i < kVariants; ++i) {
        rt[i].create(ctx, {VK_IMAGE_TYPE_2D, fmt, {kN, kN, 1}, 1, 1, false,
                           VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT, {}, "gpl target"});
        VkBufferCreateInfo bci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
        bci.size = kN * kN * bpp;
        bci.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
        VmaAllocationCreateInfo aci{};
        aci.priority = 0.5f;
        aci.usage = VMA_MEMORY_USAGE_AUTO;
        aci.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;
        vmaCreateBuffer(ctx.allocator(), &bci, &aci, &readback[i], &readbackAlloc[i], &readbackInfo[i]);
    }
    if (o.depth) {
        depth.create(ctx, {VK_IMAGE_TYPE_2D, VK_FORMAT_D32_SFLOAT, {kN, kN, 1}, 1, 1, false, VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT, {}, "gpl depth"});
    }

    std::vector<std::string> names;
    {
        std::ifstream in(o.list);
        for (std::string l; std::getline(in, l);) {
            while (!l.empty() && (l.back() == '\r' || l.back() == ' ')) l.pop_back();
            if (!l.empty()) names.push_back(l);
        }
    }
    std::mt19937 rng(o.seed);
    std::uniform_real_distribution<float> uni(0.05f, 1.0f);
    std::vector<VkShaderModule> modules;
    int shadersDiff = 0, shadersRun = 0, skipped = 0;
    bool firstFrame = true;
    for (const std::string& name : names) {
        VkShaderModule fs = vk::createShaderModule(ctx, readSpv(name));
        if (!fs) {
            ++skipped;
            continue;
        }
        vk::PipelineDesc pd{};
        pd.vs = vk::rhiModule(vs);
        pd.fs = vk::rhiModule(fs);
        pd.colorCount = 1;
        pd.colorFormats[0] = fmt;
        if (o.depth) pd.depthFormat = VK_FORMAT_D32_SFLOAT;
        vk::PipelineDesc pdDemote = pd;  // --cross: the monolithic builds use the module as DXC wrote it (demote), the links the one the renderer makes (vk::g_demoteAsKill)
        if (o.cross) {
            const bool was = vk::g_demoteAsKill.exchange(false);
            VkShaderModule fsd = vk::createShaderModule(ctx, readSpv(name));
            vk::g_demoteAsKill.store(was);
            modules.push_back(fsd);
            pdDemote.fs = vk::rhiModule(fsd);
        }
        VkPipeline pl[kVariants] = {};
        const int kinds[kVariants] = {0, 0, 1, 2, 2};
        bool ok = true;
        for (int i = 0; i < kVariants; ++i) {
            if (i == 2 && o.noLto) continue;
            pl[i] = pipes.buildVariantForTest(o.cross && kinds[i] == 2 ? pdDemote : pd, kinds[i]);
            if (pl[i] == VK_NULL_HANDLE) ok = false;
        }
        if (!ok) {
            std::printf("%s: pipeline failed (interface mismatch with the vertex shader?)\n", name.c_str());
            for (VkPipeline p : pl) {
                if (p != VK_NULL_HANDLE) vkDestroyPipeline(ctx.device(), p, nullptr);
            }
            modules.push_back(fs);
            ++skipped;
            continue;
        }
        const int pairs[5][2] = {{0, 3}, {2, 3}, {0, 1}, {3, 4}, {0, 2}};
        const char* pairNames[5] = {"A~D", "C~D", "A~B", "D~E", "A~C"};
        std::uint32_t diffPx[5] = {};
        float maxd[5] = {};
        std::uint32_t covered = 0;
        for (int trial = 0; trial < o.trials; ++trial) {
            frames.begin();
            VkCommandBuffer cmd = frames.cmd();
            if (firstFrame) {
                bindless.recordInit(cmd);
                for (auto& t : tex) {
                    rhi::RingAlloc staging = frames.allocate(kTexSz * kTexSz * 4, 16);
                    auto* px = static_cast<std::uint8_t*>(staging.ptr);
                    for (std::uint32_t i = 0; i < kTexSz * kTexSz * 4; ++i) px[i] = static_cast<std::uint8_t>(rng() & 0xff);
                    t.transition(cmd, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
                    VkBufferImageCopy region{};
                    region.bufferOffset = staging.offset;
                    region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
                    region.imageExtent = {kTexSz, kTexSz, 1};
                    vkCmdCopyBufferToImage(cmd, vk::vkBuffer(staging), t.image(), VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
                    t.transition(cmd, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
                }
                firstFrame = false;
            }
            rhi::RingAlloc vsC = frames.allocate(shader::abi::kStageConstsSize, 16);
            rhi::RingAlloc psC = frames.allocate(shader::abi::kStageConstsSize, 16);
            std::memset(vsC.ptr, 0, shader::abi::kStageConstsSize);
            std::memset(psC.ptr, 0, shader::abi::kStageConstsSize);
            auto* vc = static_cast<float*>(vsC.ptr);
            auto* pc = static_cast<float*>(psC.ptr);
            for (int i = 0; i < 256 * 4; ++i) {
                vc[i] = uni(rng);
                pc[i] = uni(rng);
            }
            rhi::RingAlloc db = frames.allocate(sizeof(shader::abi::DrawBlock), 16);
            shader::abi::DrawBlock block{};
            for (std::uint32_t s = 0; s < shader::abi::kSamplerSlots; ++s) {
                block.tex[s] = texSlot[s % kTex];
                block.smp[s] = samplerSlot;
            }
            block.posFix[0] = 1.0f / kN;
            block.posFix[1] = -1.0f / kN;
            block.alphaFunc = o.alphaFunc;
            block.alphaRef = o.alphaRef;
            std::memcpy(db.ptr, &block, sizeof(block));
            if (o.verbose) std::printf("  trial %d: PS c5 = %.6f %.6f %.6f %.6f\n", trial, pc[20], pc[21], pc[22], pc[23]);
            shader::abi::PushConstants push{vsC.ref.bits, psC.ref.bits, db.ref.bits, 0, 0, 0, 0};
            push.legacyWindows();
            for (int i = 0; i < kVariants; ++i) {
                if (pl[i] == VK_NULL_HANDLE) continue;
                rt[i].transition(cmd, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
                if (o.depth) depth.transition(cmd, VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL);
                VkRenderingAttachmentInfo color{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
                color.imageView = rt[i].view();
                color.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
                color.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
                color.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
                color.clearValue.color = {{0.0f, 0.0f, 0.0f, 0.0f}};
                VkRenderingAttachmentInfo dep{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
                dep.imageView = o.depth ? depth.view() : VK_NULL_HANDLE;
                dep.imageLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
                dep.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
                dep.storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
                dep.clearValue.depthStencil = {0.0f, 0};
                VkRenderingInfo ri{VK_STRUCTURE_TYPE_RENDERING_INFO};
                ri.renderArea = {{0, 0}, {kN, kN}};
                ri.layerCount = 1;
                ri.colorAttachmentCount = 1;
                ri.pColorAttachments = &color;
                ri.pDepthAttachment = o.depth ? &dep : nullptr;
                vkCmdBeginRendering(cmd, &ri);
                vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pl[i]);
                const VkDescriptorSet set = bindless.set();
                vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, bindless.pipelineLayout(), 0, 1, &set, 0, nullptr);
                vkCmdPushConstants(cmd, bindless.pipelineLayout(), VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(push), &push);
                setDynamicState(cmd, ctx, kN, kN);
                if (o.depth) {
                    vkCmdSetDepthTestEnable(cmd, VK_TRUE);
                    vkCmdSetDepthWriteEnable(cmd, VK_TRUE);
                    vkCmdSetDepthCompareOp(cmd, VK_COMPARE_OP_GREATER_OR_EQUAL);
                }
                if (ctx.features().vertexInputDynamicState) vkCmdSetVertexInputEXT(cmd, 0, nullptr, 0, nullptr);
                vkCmdDraw(cmd, 3, 1, 0, 0);
                vkCmdEndRendering(cmd);
                rt[i].transition(cmd, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
                VkBufferImageCopy back{};
                back.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
                back.imageExtent = {kN, kN, 1};
                vkCmdCopyImageToBuffer(cmd, rt[i].image(), VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, readback[i], 1, &back);
            }
            frames.submit(VK_NULL_HANDLE, VK_NULL_HANDLE);
            frames.waitIdle();
            auto decode = [&](int v, std::uint32_t px, std::uint32_t c) -> float {
                const auto* b = static_cast<const std::uint8_t*>(readbackInfo[v].pMappedData) + px * bpp;
                if (bpp == 16) {
                    float f;
                    std::memcpy(&f, b + c * 4, 4);
                    return f;
                }
                if (bpp == 8) {
                    std::uint16_t h;
                    std::memcpy(&h, b + c * 2, 2);
                    return halfToFloat(h);
                }
                return static_cast<float>(b[c]) / 255.0f;
            };
            for (std::uint32_t px = 0; px < kN * kN; ++px) {
                if (decode(3, px, 0) != 0.0f || decode(3, px, 1) != 0.0f || decode(3, px, 2) != 0.0f) ++covered;
                for (int pr = 0; pr < 5; ++pr) {
                    const int a = pairs[pr][0], b = pairs[pr][1];
                    if (pl[a] == VK_NULL_HANDLE || pl[b] == VK_NULL_HANDLE) continue;
                    if (std::memcmp(static_cast<const std::uint8_t*>(readbackInfo[a].pMappedData) + px * bpp,
                                    static_cast<const std::uint8_t*>(readbackInfo[b].pMappedData) + px * bpp, bpp) == 0) {
                        continue;
                    }
                    ++diffPx[pr];
                    float d = 0.0f;
                    for (std::uint32_t c = 0; c < 4; ++c) {
                        const float x = decode(a, px, c), y = decode(b, px, c);
                        if (std::isnan(x) != std::isnan(y)) {
                            d = INFINITY;
                        } else if (!std::isnan(x)) {
                            d = std::fmax(d, std::fabs(x - y));
                        }
                    }
                    maxd[pr] = std::fmax(maxd[pr], d);
                    if (o.verbose && diffPx[pr] <= 2) {
                        std::printf("    %s px %u: ", pairNames[pr], px);
                        for (std::uint32_t c = 0; c < 4; ++c) std::printf("%.6f/%.6f ", decode(a, px, c), decode(b, px, c));
                        std::putchar(10);
                    }
                }
            }
        }
        ++shadersRun;
        const bool differs = diffPx[0] != 0 || diffPx[1] != 0;
        if (differs) ++shadersDiff;
        std::printf("%s%s: covered %u/%u px", differs ? "DIFF " : "ok   ", name.c_str(), covered, kN * kN * o.trials);
        for (int pr = 0; pr < 5; ++pr) std::printf("  %s %u (max %.4g)", pairNames[pr], diffPx[pr], maxd[pr]);
        std::putchar(10);
        std::fflush(stdout);
        frames.retire([&] {});
        for (VkPipeline p : pl) {
            if (p != VK_NULL_HANDLE) vkDestroyPipeline(ctx.device(), p, nullptr);
        }
        modules.push_back(fs);  // (kept: the library cache is keyed by the module handle, a destroyed module's handle is reused)
    }
    for (int i = 0; i < kVariants; ++i) {
        vmaDestroyBuffer(ctx.allocator(), readback[i], readbackAlloc[i]);
        rt[i].destroy(ctx);
    }
    if (o.depth) depth.destroy(ctx);
    frames.retire([&] {});
    for (auto& t : tex) t.destroy(ctx);
    vkDestroySampler(ctx.device(), sampler, nullptr);
    vkDestroyShaderModule(ctx.device(), vs, nullptr);
    pipes.destroy();
    for (VkShaderModule m : modules) vkDestroyShaderModule(ctx.device(), m, nullptr);
    bindless.destroy();
    frames.destroy();
    const std::uint32_t verrors = ctx.validationErrors();
    ctx.destroy();
    std::printf("gpl: %d shaders run, %d differ (fast link or LTO link vs monolithic), %d skipped, %u validation error(s)\n", shadersRun, shadersDiff, skipped, verrors);
    return shadersDiff == 0 && verrors == 0 ? 0 : 1;
}
