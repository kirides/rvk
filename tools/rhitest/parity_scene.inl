// Included by parity.cpp (inside its anonymous namespace): the scene variant (docs/research/frame-ownership.md 7.10, abi.h DrawRecord / ScenePush).
//
// N draws of one small mesh, drawn twice into two targets:
//   (a) today's path: one draw per push block, the push block's windows / DrawBlock / frame pointing at the draw's own register blocks;
//   (b) the scene path: scene variants of the same shader pair (TranslateOptions::sceneRecord / RVK_SCENE_RECORD), ONE drawIndexedIndirect over N argument
//       records (firstInstance = i) with the per-instance `uint2 ref` stream; the DrawRecord i of the record buffer holds the windows of draw i (every
//       window of a record = the draw's full register block, as the legacy windows of (a)), its DrawBlock and flags; the push block is a ScenePush.
// The images must be byte-equal on each backend (and the scene image is cross-checked between the backends like the other cases). The draws differ in
// their register contents and textures, and are blended additively: each draw must read its own record.

// the translated shader of D3D9 tokens (the real corpus shaders), compiled for both targets; the reflection of the plain translation on request
bool translatedTokens(const std::vector<std::uint32_t>& tokens, const shader::TranslateOptions& options, Blobs& out, const char* name, shader::Reflection* reflection = nullptr) {
    const shader::DecodeResult dec = shader::decode(tokens);
    if (!dec.ok()) {
        std::printf("  decoding %s failed: %s\n", name, dec.error.c_str());
        return false;
    }
    shader::setVaryingFillers(true);
    const shader::HlslResult r = shader::toHlsl(dec.program, options);
    shader::setVaryingFillers(false);
    if (!r.ok()) {
        std::printf("  translating %s failed: %s\n", name, r.error.c_str());
        return false;
    }
    if (reflection != nullptr) *reflection = r.reflection;
    const bool vertex = dec.program.version.stage == shader::Stage::Vertex;
    return compileBoth(std::string(name) + ".hlsl", r.hlsl, vertex ? "VSMain" : "PSMain", vertex ? "vs_6_6" : "ps_6_6", out);
}

// one pair of shaders + what its draws need
struct SceneCase {
    std::string label;
    Blobs vs, ps, svs, sps;                               // plain and scene variants
    std::vector<VkVertexInputAttributeDescription> attrs;  // the plain vertex layout (binding 0)
    std::uint32_t stride = 0;
    std::vector<std::uint8_t> vertices;                    // 6 vertices of `stride` bytes
    // register files and DrawBlock of draw i (the texture heap indices are the caller's)
    std::function<void(std::uint32_t draw, StageBlock& vs, StageBlock& ps, abi::DrawBlock& db)> fill;
    bool expectDistinct = false;                           // the draws are placed so that they do not overlap: the picture must show every one of them
};

// runs one case with `n` draws; prints PASS / FAIL. `mode` 1 = scene ABI v2 (frame-ownership.md 7.19): the scene shaders are the sceneView variants, the draws' view
// registers (VS c0..c30, PS c0..c4, posFix, c5.xyz) come from a ViewBlock2 table slot chosen by the ref stream's y (draw i: slot 3 / 9 by i & 1; the plain reference
// draws use the same registers), the records carry no view window bases; mode 2 = the same with every draw pointing at the OTHER slot (a mutation: the images
// MUST differ, which proves the draws read their slot)
void runSceneCase(Harness& h, SceneCase& c, std::uint32_t n, int mode = 0) {
    rhi::Device& dev = h.dev;
    const bool v2 = mode != 0;
    constexpr std::uint32_t kSlotOf[2] = {3, 9};
    StageBlock slotVs[2], slotPs[2];
    abi::DrawBlock slotDb[2]{};
    std::vector<abi::ViewBlock2> views(16);
    std::memset(views.data(), 0, views.size() * sizeof(abi::ViewBlock2));
    if (v2) {
        for (std::uint32_t j = 0; j < 2; ++j) {
            c.fill(j, slotVs[j], slotPs[j], slotDb[j]);
            abi::ViewBlock2& vb = views[kSlotOf[j]];
            std::memcpy(vb.vs0, slotVs[j].bytes.data(), sizeof(vb.vs0));
            std::memcpy(vb.vs1, slotVs[j].bytes.data() + 12 * 16, sizeof(vb.vs1));
            std::memcpy(vb.ps0, slotPs[j].bytes.data(), sizeof(vb.ps0));
            std::memcpy(vb.posFix, slotDb[j].posFix, sizeof(vb.posFix));
            std::memcpy(vb.c5, slotPs[j].bytes.data() + 5 * 16, 3 * sizeof(float));
        }
    }
    const int before = failures;
    const char* backend = rhi::backendName(dev.caps().backend);
    if (!c.vs.ok || !c.ps.ok || !c.svs.ok || !c.sps.ok) {
        check(false, c.label + ": shaders");
        return;
    }
    if (!dev.caps().drawIndirectFirstInstance || !dev.caps().multiDrawIndirect) {
        std::printf("[%s] %s: SKIP (no drawIndirectFirstInstance / multiDrawIndirect)\n", backend, c.label.c_str());
        return;
    }
    abi::FrameBlock fb{};
    fb.time[0] = 0.5f;
    auto* bFrame = h.makeBuffer(&fb, sizeof(fb), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, "scene frame block");
    auto* bVerts = h.makeBuffer(c.vertices.data(), c.vertices.size(), VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, "scene vertices");
    const std::uint32_t indices[6] = {0, 1, 2, 3, 4, 5};
    auto* bIndex = h.makeBuffer(indices, sizeof(indices), VK_BUFFER_USAGE_INDEX_BUFFER_BIT, "scene indices");
    std::vector<abi::DrawRecord> records(n);
    std::vector<std::uint32_t> args(5 * n), refs(2 * n);
    std::vector<abi::PushConstants> pushes(n);
    for (std::uint32_t i = 0; i < n; ++i) {
        StageBlock vsc, psc;
        abi::DrawBlock db{};
        c.fill(i, vsc, psc, db);
        if (v2) {
            // the plain reference draw sees the view registers of its slot (and its own c5.w: the record's c5w)
            const std::uint32_t j = i & 1;
            std::memcpy(vsc.bytes.data(), slotVs[j].bytes.data(), 31 * 16);
            std::memcpy(psc.bytes.data(), slotPs[j].bytes.data(), 5 * 16);
            std::memcpy(psc.bytes.data() + 5 * 16, slotPs[j].bytes.data() + 5 * 16, 3 * sizeof(float));
            std::memcpy(db.posFix, slotDb[j].posFix, sizeof(db.posFix));
        }
        auto* bVs = h.makeBuffer(vsc.bytes.data(), vsc.bytes.size(), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, "scene vs consts");
        auto* bPs = h.makeBuffer(psc.bytes.data(), psc.bytes.size(), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, "scene ps consts");
        auto* bDraw = h.makeBuffer(&db, sizeof(db), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, "scene draw block");
        if (bVs == nullptr || bPs == nullptr || bDraw == nullptr || bFrame == nullptr) {
            check(false, c.label + ": resources");
            return;
        }
        pushes[i] = pushFor(*bVs, *bPs, *bDraw, *bFrame);
        pushes[i].drawId = i;
        pushes[i].flags = abi::drawFlags(1, 2, 3);
        abi::DrawRecord& r = records[i];
        for (std::uint32_t w = 0; w < abi::kConstWindows; ++w) {
            r.vsWin[w] = pushes[i].vsWin[w];
            r.psWin[w] = pushes[i].psWin[w];
        }
        r.draw = pushes[i].draw;
        if (v2) {
            r.vsWin[0] = r.vsWin[1] = r.psWin[0] = r.psWin[1] = 0;  // (the view windows are the table slot's)
            std::memcpy(&r.c5w, psc.bytes.data() + 5 * 16 + 12, 4);
        }
        r.drawId = i;
        r.flags = pushes[i].flags;
        r.instanceSlot = 100 + i;
        args[5 * i + 0] = 6;  // indexCount
        args[5 * i + 1] = 1;  // instanceCount
        args[5 * i + 2] = 0;  // firstIndex
        args[5 * i + 3] = 0;  // vertexOffset (0: D3D12's SV_VertexID ignores it)
        args[5 * i + 4] = i;  // firstInstance: the ref stream's element
        refs[2 * i + 0] = i;  // record index
        refs[2 * i + 1] = v2 ? kSlotOf[mode == 2 ? 1 - (i & 1) : (i & 1)] : 7 * i + 1;
    }
    auto* bRecords = h.makeBuffer(records.data(), records.size() * sizeof(abi::DrawRecord), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, "scene draw records");
    auto* bArgs = h.makeBuffer(args.data(), args.size() * 4, VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, "scene indirect args");
    auto* bRefs = h.makeBuffer(refs.data(), refs.size() * 4, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, "scene ref stream");
    if (!bVerts || !bIndex || !bRecords || !bArgs || !bRefs) {
        check(false, c.label + ": buffers");
        return;
    }
    abi::ScenePush sp{};
    sp.records = refOf(*bRecords);
    sp.frame = refOf(*bFrame);
    sp.recordBase = 0;
    if (v2) {
        auto* bViews = h.makeBuffer(views.data(), views.size() * sizeof(abi::ViewBlock2), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, "scene view table");
        if (bViews == nullptr) {
            check(false, c.label + ": view table");
            return;
        }
        sp.views = refOf(*bViews);
    }

    const VkVertexInputBindingDescription bindings[2] = {{0, c.stride, VK_VERTEX_INPUT_RATE_VERTEX}, {1, 8, VK_VERTEX_INPUT_RATE_INSTANCE}};
    std::vector<VkVertexInputAttributeDescription> sceneAttrs = c.attrs;
    sceneAttrs.push_back({abi::kSceneRefLocation, 1, VK_FORMAT_R32G32_UINT, 0});
    const auto makePipeline = [&](const Blobs& vs, const Blobs& ps, bool scene, const char* name) {
        rhi::GraphicsPipelineDesc gd;
        gd.vs = vs.shader();
        gd.ps = ps.shader();
        gd.colorFormats[0] = VK_FORMAT_R8G8B8A8_UNORM;
        gd.blend.blendEnable = VK_TRUE;  // every draw adds its own picture
        gd.blend.srcColorBlendFactor = gd.blend.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
        gd.blend.dstColorBlendFactor = gd.blend.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
        gd.blend.colorBlendOp = gd.blend.alphaBlendOp = VK_BLEND_OP_ADD;
        gd.blend.colorWriteMask = 0xF;
        gd.vertexBindings = std::span<const VkVertexInputBindingDescription>(bindings, scene ? 2 : 1);
        gd.vertexAttributes = scene ? std::span<const VkVertexInputAttributeDescription>(sceneAttrs) : std::span<const VkVertexInputAttributeDescription>(c.attrs);
        gd.name = name;
        return dev.createGraphicsPipeline(gd);
    };
    auto plain = makePipeline(c.vs, c.ps, false, "scene parity plain");
    auto scene = makePipeline(c.svs, c.sps, true, "scene parity scene");
    Target targets[2];
    for (Target& t : targets) {
        t.tex = dev.createTexture({VK_IMAGE_TYPE_2D, t.format, {kTarget, kTarget, 1}, 1, 1, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT, "scene parity target"});
        t.pitch = rhi::bufferRowPitch(t.format, kTarget);
        t.readback = dev.createBuffer({static_cast<std::uint64_t>(t.pitch) * kTarget, VK_BUFFER_USAGE_TRANSFER_DST_BIT, rhi::Memory::Readback, "scene parity readback"});
    }
    if (!plain || !scene || !targets[0].tex || !targets[1].tex || !targets[0].readback || !targets[1].readback) {
        check(false, c.label + ": pipelines / targets");
        return;
    }
    dev.submitAndWait([&](rhi::CommandList& cl) {
        for (int k = 0; k < 2; ++k) {
            cl.transition(*targets[k].tex, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
            rhi::RenderingInfo ri;
            ri.colorCount = 1;
            ri.color[0].texture = targets[k].tex.get();
            ri.color[0].load = VK_ATTACHMENT_LOAD_OP_CLEAR;
            ri.color[0].clear.float32[3] = 1.0f;
            ri.width = ri.height = kTarget;
            cl.beginRendering(ri);
            if (k == 0) {
                cl.bindPipeline(*plain);
                rhi::Buffer* vb[1] = {bVerts};
                const std::uint64_t off[1] = {0};
                const std::uint32_t str[1] = {c.stride};
                cl.setVertexBuffers(0, vb, off, str);
                for (std::uint32_t i = 0; i < n; ++i) {
                    cl.setPushConstants(&pushes[i], sizeof(abi::PushConstants));
                    cl.draw(6, 1, 0, 0);
                }
            } else {
                cl.bindPipeline(*scene);
                cl.setPushConstants(&sp, sizeof(sp));
                rhi::Buffer* vb[2] = {bVerts, bRefs};
                const std::uint64_t off[2] = {0, 0};
                const std::uint32_t str[2] = {c.stride, 8};
                cl.setVertexBuffers(0, vb, off, str);
                cl.setIndexBuffer(*bIndex, 0, VK_INDEX_TYPE_UINT32);
                cl.drawIndexedIndirect(*bArgs, 0, n, 20);
            }
            cl.endRendering();
            cl.transition(*targets[k].tex, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
            cl.copyTextureToBuffer(*targets[k].readback, 0, *targets[k].tex);
        }
    });
    const std::vector<std::uint8_t> a = pixelsOf(targets[0]), b = pixelsOf(targets[1]);
    std::size_t differ = 0, lit = 0, saturated = 0;
    for (std::size_t i = 0; i < a.size(); i += 4) {
        if (std::memcmp(&a[i], &b[i], 4) != 0) ++differ;
        if (a[i] != 0 || a[i + 1] != 0 || a[i + 2] != 0) ++lit;
        if (a[i] == 255 && a[i + 1] == 255 && a[i + 2] == 255) ++saturated;
    }
    if (mode == 2) {
        check(differ != 0, std::format("{}: the draws pointing at the wrong view slot drew the same picture (the view slot is not read)", c.label));
        std::printf("[%s] %s: %s (wrong-view-slot mutation: %zu pixels differ, must be > 0)\n", backend, c.label.c_str(), failures == before ? "PASS" : "FAIL", differ);
        return;
    }
    check(differ == 0, std::format("{}: {} pixels differ between the per-draw push path and the scene path", c.label, differ));
    if (c.expectDistinct) {
        // the draws sit in separate quadrants with their own colours: every quadrant must be lit
        std::uint32_t quadrants = 0;
        for (int q = 0; q < 4; ++q) {
            const std::uint32_t x = 16 + 32 * (q & 1), y = 16 + 32 * (q >> 1);
            const std::uint8_t* p = b.data() + (static_cast<std::size_t>(y) * kTarget + x) * 4;
            quadrants += (p[0] != 0 || p[1] != 0 || p[2] != 0) ? 1u : 0u;
        }
        check(quadrants == 4, std::format("{}: only {} of 4 draws are visible in the scene image", c.label, quadrants));
    }
    crossCheck(dev, (c.label + " scene").c_str(), toFloats(b.data(), b.size()), 2.0f, 0.002f);
    std::printf("[%s] %s: %s (%u draws, %zu lit pixels, %zu saturated, %zu differ)\n", backend, c.label.c_str(), failures == before ? "PASS" : "FAIL", n, lit, saturated, differ);
}

// a quad as 6 vertices: position float3 + uv float2 (the layout of kQuad), attributes 0 and 1
void quadLayout(SceneCase& c) {
    c.stride = 20;
    c.vertices.resize(sizeof(kQuad));
    std::memcpy(c.vertices.data(), kQuad, sizeof(kQuad));
    c.attrs = {{0, 0, VK_FORMAT_R32G32B32_SFLOAT, 0}, {1, 0, VK_FORMAT_R32G32_SFLOAT, 12}};
}

// the asm pair of testTranslatedDraw: four draws, one per quadrant (c3 offset, c4 colour, ps c0), two textures (DrawBlock tex[0])
void testSceneAsm(Harness& h, bool v2 = false, bool mutate = false) {
    rhi::Device& dev = h.dev;
    if (!devtools::D3DX::get().available()) {
        std::printf("[%s] scene variant (asm pair): SKIP (d3dx9_39.dll not installed)\n", rhi::backendName(dev.caps().backend));
        return;
    }
    SceneCase c;
    c.label = std::string("scene variant (asm pair") + (v2 ? ", ABI v2" : "") + (mutate ? ", wrong view slot" : "") + ")";
    if (!selected(c.label.c_str())) return;
    shader::TranslateOptions plainOpt, sceneOpt;
    sceneOpt.sceneRecord = true;
    sceneOpt.sceneView = v2;
    quadLayout(c);
    c.expectDistinct = !v2;  // (ABI v2: the draws of a view slot share its registers, so only two placements exist)
    if (!translated(kVsAsm, true, plainOpt, c.vs, "scene_vs") || !translated(kPsAsm, false, plainOpt, c.ps, "scene_ps") || !translated(kVsAsm, true, sceneOpt, c.svs, "scene_vs_s") ||
        !translated(kPsAsm, false, sceneOpt, c.sps, "scene_ps_s")) {
        check(false, c.label + ": translation");
        return;
    }
    const std::uint32_t tex[2] = {
        h.addPattern(4, 4, [](std::uint32_t x, std::uint32_t y) { return std::array<std::uint8_t, 4>{static_cast<std::uint8_t>(64 * x + 32), static_cast<std::uint8_t>(64 * y + 32), 128, 255}; }, "scene texture 0"),
        h.addPattern(4, 4, [](std::uint32_t x, std::uint32_t y) { return std::array<std::uint8_t, 4>{static_cast<std::uint8_t>(255 - 64 * y), static_cast<std::uint8_t>(64 * x + 32), 40, 255}; }, "scene texture 1")};
    const rhi::SamplerIndex smp = dev.createSampler({VK_FILTER_NEAREST, VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE});
    c.fill = [&](std::uint32_t i, StageBlock& vs, StageBlock& ps, abi::DrawBlock& db) {
        const float ox = (i & 1) != 0 ? 0.5f : -0.5f, oy = (i & 2) != 0 ? -0.5f : 0.5f;
        vs.set(0, 0.4f, 0, 0, 0);
        vs.set(1, 0, 0.4f, 0, 0);
        vs.set(2, 0, 0, 1, 0);
        vs.set(3, ox, oy, 0, 1);
        vs.set(4, 0.4f + 0.2f * static_cast<float>(i), 1.0f - 0.2f * static_cast<float>(i), 0.5f, 1.0f);
        ps.set(0, 1.0f, 0.9f - 0.1f * static_cast<float>(i), 0.8f, 1.0f);
        db.tex[0] = tex[i & 1];
        db.smp[0] = smp;
    };
    runSceneCase(h, c, 4, !v2 ? 0 : mutate ? 2 : 1);
}

// the ubershader (a captured request) with the translated vertex shader of the uber test: the pixel shader reads window 2 / 3 and the DrawBlock by record
void testSceneUber(Harness& h, const char* name, const shader::MacroList& macros, const std::string& hooks, bool heavy, bool v2 = false, const char* entry = "PS_Default") {
    rhi::Device& dev = h.dev;
    SceneCase c;
    c.label = std::string("scene variant (ubershader, ") + (std::string(entry) == "PS_Default" ? "" : "zpass, ") + name + (v2 ? ", ABI v2" : "") + ")";
    if (!selected(c.label.c_str())) return;
    if (!devtools::D3DX::get().available()) {
        std::printf("[%s] %s: SKIP (d3dx9_39.dll not installed)\n", rhi::backendName(dev.caps().backend), c.label.c_str());
        return;
    }
    shader::TranslateOptions plainOpt, sceneOpt;
    sceneOpt.sceneRecord = true;
    sceneOpt.sceneView = v2;
    quadLayout(c);
    if (!translated(kUberVsAsm, true, plainOpt, c.vs, "scene_uber_vs") || !translated(kUberVsAsm, true, sceneOpt, c.svs, "scene_uber_vs_s") ||
        !compileBoth("scene_uber.hlsl", shader::uberPixelHlsl(entry, withExtra(macros), hooks), "PSMain", "ps_6_6", c.ps) ||
        !compileBoth("scene_uber_s.hlsl", shader::uberPixelHlsl(entry, withExtra(macros), hooks, true, v2), "PSMain", "ps_6_6", c.sps)) {
        check(false, c.label + ": shaders");
        return;
    }
    const rhi::SamplerIndex linear = dev.createSampler({VK_FILTER_LINEAR, VK_SAMPLER_ADDRESS_MODE_REPEAT});
    // two sets of material textures; draw i uses set i & 1
    std::uint32_t texSets[2][16] = {};
    std::uint32_t seed = 1;
    for (int set = 0; set < 2; ++set) {
        for (std::uint32_t slot : {0u, 1u, 2u, 4u, 5u, 9u, 10u, 11u, 12u, 13u, 14u, 15u}) {
            if (!heavy && slot != 9u) continue;
            const std::uint32_t s = seed++;
            texSets[set][slot] = h.addPattern(4, 4,
                                              [s](std::uint32_t x, std::uint32_t y) {
                                                  const float a = 0.5f + 0.45f * std::sin(0.9f * static_cast<float>(x) + 0.7f * static_cast<float>(s));
                                                  const float b = 0.5f + 0.45f * std::cos(0.8f * static_cast<float>(y) + 0.5f * static_cast<float>(s));
                                                  const float cc = 0.5f + 0.4f * std::sin(0.5f * static_cast<float>(x + y) + static_cast<float>(s));
                                                  return std::array<std::uint8_t, 4>{static_cast<std::uint8_t>(a * 255), static_cast<std::uint8_t>(b * 255), static_cast<std::uint8_t>(cc * 255), 255};
                                              },
                                              "scene material texture");
        }
    }
    c.fill = [&](std::uint32_t i, StageBlock& vs, StageBlock& ps, abi::DrawBlock& db) {
        vs.set(1, 0.5f, 0.5f, 0.3f, 0.3f);
        vs.set(2, 0.1f, 0.2f, 0.3f, 0.4f);
        vs.set(3, 0.2f, 0.3f, 0.5f, 1.0f);
        vs.set(4, 0.2f, 0.2f, 0.2f, 0.2f);
        vs.set(5, 0.4f, 0.5f, 0.6f, 1.0f);
        vs.set(6, 0.1f, 0.1f, 0.1f, 0.1f);
        vs.set(7, 0.3f, 0.8f, 0.3f, 0.9f);
        vs.set(8, 0.7f, 0.2f, 0.3f, 0.0f);
        vs.set(9, 0.3f, 0.3f, 0.9f, 0.0f);
        vs.set(10, 0.3f, 0.4f, 0.5f, 0.6f);
        vs.set(11, 0.4f, 0.5f, 0.6f, 0.8f);
        for (std::uint32_t r = 0; r < 224; ++r) {
            const auto f = [&](std::uint32_t k) { return 0.15f + 0.7f * (0.5f + 0.5f * std::sin(0.37f * static_cast<float>(r) + 1.3f * static_cast<float>(k) + 0.9f * static_cast<float>(i))); };
            ps.set(r, f(0), f(1), f(2), f(3));
        }
        ps.set(24, 0.3f, 0.8f, 0.5f, 0.0f);
        for (std::uint32_t slot = 0; slot < 16; ++slot) {
            db.tex[slot] = texSets[i & 1][slot];
            db.smp[slot] = texSets[i & 1][slot] != 0 ? linear : 0;
        }
    };
    runSceneCase(h, c, 4, v2 ? 1 : 0);
}

// ---- real corpus shaders: a lit material pair with textures and a skinned pair -----------------------------------------------------------------

std::filesystem::path findCorpus() {
    if (const char* env = std::getenv("RVK_CORPUS")) return env;
    std::filesystem::path p = std::filesystem::path(__FILE__).parent_path();
    for (int up = 0; up < 8; ++up, p = p.parent_path()) {
        if (std::filesystem::is_directory(p / "corpus")) return p / "corpus";
        if (!p.has_parent_path() || p.parent_path() == p) break;
    }
    return {};
}

std::vector<std::uint32_t> readTokensFile(const std::filesystem::path& p) {
    std::ifstream in(p, std::ios::binary);
    std::vector<char> bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    std::vector<std::uint32_t> t(bytes.size() / 4);
    std::memcpy(t.data(), bytes.data(), t.size() * 4);
    return t;
}

struct CorpusShader {
    std::filesystem::path file;
    shader::Reflection reflection;
    bool vertex = false;
};

// the first suitable vertex shaders and the pixel shader with the most textures whose varyings a vertex shader writes
void testSceneCorpus(Harness& h, bool v2 = false) {
    rhi::Device& dev = h.dev;
    const std::filesystem::path dir = findCorpus();
    if (dir.empty()) {
        std::printf("[%s] scene variant (corpus pairs): SKIP (no corpus directory; RVK_CORPUS)\n", rhi::backendName(dev.caps().backend));
        return;
    }
    std::vector<std::filesystem::path> vsFiles, psFiles;
    for (const auto& e : std::filesystem::directory_iterator(dir)) {
        const std::string n = e.path().filename().string();
        if (n.starts_with("vs30_")) vsFiles.push_back(e.path());
        else if (n.starts_with("ps30_")) psFiles.push_back(e.path());
    }
    std::sort(vsFiles.begin(), vsFiles.end());
    std::sort(psFiles.begin(), psFiles.end());
    const auto reflect = [](const std::filesystem::path& f, shader::Reflection& out) {
        const shader::DecodeResult dec = shader::decode(readTokensFile(f));
        if (!dec.ok()) return false;
        const shader::HlslResult r = shader::toHlsl(dec.program);
        out = r.reflection;
        return r.ok();
    };
    const auto hasInput = [](const shader::Reflection& r, shader::Usage u) {
        return std::any_of(r.vertexInputs.begin(), r.vertexInputs.end(), [u](const auto& v) { return v.usage == u; });
    };
    // vertex shaders: window-readable, no vertex textures, no relative addressing beyond the windows (floatConstCount <= 256 holds always), not writing varying 14
    struct Vs {
        std::filesystem::path file;
        shader::Reflection r;
    };
    std::vector<Vs> skinned, rigid;
    for (const auto& f : vsFiles) {
        shader::Reflection r;
        if (!reflect(f, r) || !r.splitConsts || (r.varyingMask & (1u << abi::kSceneVaryingLocation)) != 0) continue;
        bool vtex = false;
        for (std::uint32_t s = abi::kVertexSamplerBase; s < abi::kSamplerSlots; ++s) vtex = vtex || r.samplers[s] != shader::TextureType::Unknown;
        if (vtex) continue;
        const bool bones = hasInput(r, shader::Usage::BlendIndices) && hasInput(r, shader::Usage::BlendWeight);
        if (!hasInput(r, shader::Usage::Position)) continue;
        if (bones && skinned.size() < 1) skinned.push_back({f, r});
        if (!bones && hasInput(r, shader::Usage::Normal) && hasInput(r, shader::Usage::TexCoord) && rigid.size() < 1) rigid.push_back({f, r});
        if (!skinned.empty() && !rigid.empty()) break;
    }
    // pixel shaders: the most 2D / cube textures among those whose varyings a vertex shader covers
    const auto pickPs = [&](const Vs& v, int minSamplers, std::filesystem::path& file, shader::Reflection& refl) {
        int best = -1;
        for (const auto& f : psFiles) {
            shader::Reflection r;
            if (!reflect(f, r) || !r.splitConsts || r.writesDepth || r.colorOutputs != 1u) continue;
            if ((r.varyingMask & ~v.r.varyingMask) != 0 || (r.varyingMask & (1u << abi::kSceneVaryingLocation)) != 0) continue;
            int count = 0;
            bool ok = true;
            for (std::uint32_t s = 0; s < abi::kPixelSamplers; ++s) {
                if (r.samplers[s] == shader::TextureType::Volume) ok = false;
                if (r.samplers[s] != shader::TextureType::Unknown) ++count;
            }
            if (ok && count >= minSamplers && count > best) {
                best = count;
                file = f;
                refl = r;
            }
        }
        return best >= 0;
    };
    const std::string suffix = v2 ? ", ABI v2)" : ")";
    const std::string labels[2] = {"scene variant (corpus: rigid mesh + lit material" + suffix, "scene variant (corpus: skinned mesh + material" + suffix};
    const struct {
        const char* label;
        const Vs* vs;
        int minSamplers;
    } cases[2] = {{labels[0].c_str(), rigid.empty() ? nullptr : &rigid[0], 4}, {labels[1].c_str(), skinned.empty() ? nullptr : &skinned[0], 2}};
    shader::Reflection ignored;
    for (const auto& k : cases) {
        if (k.vs == nullptr || !selected(k.label)) {
            if (k.vs == nullptr) std::printf("[%s] %s: SKIP (no suitable vertex shader in the corpus)\n", rhi::backendName(dev.caps().backend), k.label);
            continue;
        }
        std::filesystem::path psFile;
        shader::Reflection psRefl;
        if (!pickPs(*k.vs, k.minSamplers, psFile, psRefl)) {
            std::printf("[%s] %s: SKIP (no pixel shader for %s)\n", rhi::backendName(dev.caps().backend), k.label, k.vs->file.filename().string().c_str());
            continue;
        }
        SceneCase c;
        c.label = k.label;
        shader::TranslateOptions plainOpt, sceneOpt;
        sceneOpt.sceneRecord = true;
        sceneOpt.sceneView = v2;
        const std::vector<std::uint32_t> vsTokens = readTokensFile(k.vs->file), psTokens = readTokensFile(psFile);
        shader::Reflection vsRefl;
        if (!translatedTokens(vsTokens, plainOpt, c.vs, "scene_corpus_vs", &vsRefl) || !translatedTokens(psTokens, plainOpt, c.ps, "scene_corpus_ps") ||
            !translatedTokens(vsTokens, sceneOpt, c.svs, "scene_corpus_vs_s") || !translatedTokens(psTokens, sceneOpt, c.sps, "scene_corpus_ps_s")) {
            check(false, c.label + ": translation");
            continue;
        }
        std::printf("  %s: vs %s + ps %s (%d textures)\n", k.label, k.vs->file.filename().string().c_str(), psFile.filename().string().c_str(),
                    static_cast<int>(std::count_if(psRefl.samplers.begin(), psRefl.samplers.begin() + abi::kPixelSamplers, [](auto t) { return t != shader::TextureType::Unknown; })));
        // vertex layout: one float4 per declared input (location = the v# register), values by usage
        c.stride = 16 * static_cast<std::uint32_t>(vsRefl.vertexInputs.size());
        c.vertices.assign(static_cast<std::size_t>(c.stride) * 6, 0);
        for (std::uint32_t v = 0; v < 6; ++v) {
            for (std::size_t a = 0; a < vsRefl.vertexInputs.size(); ++a) {
                const auto& in = vsRefl.vertexInputs[a];
                float val[4] = {0.5f, 0.5f, 0.5f, 1.0f};
                switch (in.usage) {
                case shader::Usage::Position: val[0] = kQuad[v][0]; val[1] = kQuad[v][1]; val[2] = 0.5f * kQuad[v][2]; val[3] = 1.0f; break;
                case shader::Usage::TexCoord: val[0] = kQuad[v][3]; val[1] = kQuad[v][4]; val[2] = 0.25f * static_cast<float>(in.usageIndex); val[3] = 0.5f; break;
                case shader::Usage::Normal: val[0] = 0.0f; val[1] = 0.0f; val[2] = 1.0f; val[3] = 0.0f; break;
                case shader::Usage::Tangent: val[0] = 1.0f; val[1] = 0.0f; val[2] = 0.0f; val[3] = 0.0f; break;
                case shader::Usage::Binormal: val[0] = 0.0f; val[1] = 1.0f; val[2] = 0.0f; val[3] = 0.0f; break;
                case shader::Usage::BlendIndices: val[0] = 0.0f; val[1] = 1.0f; val[2] = 2.0f; val[3] = 3.0f; break;
                case shader::Usage::BlendWeight: val[0] = 0.5f; val[1] = 0.5f; val[2] = 0.0f; val[3] = 0.0f; break;
                case shader::Usage::Color: val[0] = 0.8f; val[1] = 0.6f; val[2] = 0.4f; val[3] = 1.0f; break;
                default: break;
                }
                std::memcpy(c.vertices.data() + static_cast<std::size_t>(v) * c.stride + a * 16, val, 16);
            }
        }
        for (std::size_t a = 0; a < vsRefl.vertexInputs.size(); ++a) c.attrs.push_back({vsRefl.vertexInputs[a].location, 0, VK_FORMAT_R32G32B32A32_SFLOAT, static_cast<std::uint32_t>(a) * 16});
        // textures by the pixel shader's sampler types; draw i uses the set i & 1
        const rhi::SamplerIndex linear = dev.createSampler({VK_FILTER_LINEAR, VK_SAMPLER_ADDRESS_MODE_REPEAT});
        std::uint32_t texSets[2][16] = {};
        for (int set = 0; set < 2; ++set) {
            for (std::uint32_t s = 0; s < abi::kPixelSamplers; ++s) {
                const std::uint32_t seed = static_cast<std::uint32_t>(set) * 16 + s + 1;
                if (psRefl.samplers[s] == shader::TextureType::Tex2D) {
                    texSets[set][s] = h.addPattern(4, 4,
                                                   [seed](std::uint32_t x, std::uint32_t y) {
                                                       const float a = 0.5f + 0.45f * std::sin(0.9f * static_cast<float>(x) + 0.7f * static_cast<float>(seed));
                                                       const float b = 0.5f + 0.45f * std::cos(0.8f * static_cast<float>(y) + 0.5f * static_cast<float>(seed));
                                                       return std::array<std::uint8_t, 4>{static_cast<std::uint8_t>(a * 255), static_cast<std::uint8_t>(b * 255), static_cast<std::uint8_t>(128 + 60 * std::sin(static_cast<float>(seed))), 255};
                                                   },
                                                   "scene corpus texture");
                } else if (psRefl.samplers[s] == shader::TextureType::Cube) {
                    texSets[set][s] = h.addCube(4,
                                                [seed](std::uint32_t f, std::uint32_t x, std::uint32_t y) {
                                                    return std::array<std::uint8_t, 4>{static_cast<std::uint8_t>(40 * f + 8 * x), static_cast<std::uint8_t>(30 * y + seed), static_cast<std::uint8_t>(200 - 20 * f), 255};
                                                },
                                                "scene corpus cube");
                }
            }
        }
        c.fill = [&](std::uint32_t i, StageBlock& vs, StageBlock& ps, abi::DrawBlock& db) {
            // every register of both stages: a deterministic file in 0.15 .. 0.85, a different phase per draw (dim: the draws add up)
            for (std::uint32_t r = 0; r < abi::kFloatConsts; ++r) {
                const auto f = [&](std::uint32_t kk, float phase) { return 0.15f + 0.7f * (0.5f + 0.5f * std::sin(0.37f * static_cast<float>(r) + 1.3f * static_cast<float>(kk) + phase * static_cast<float>(i))); };
                vs.set(r, f(0, 0.4f), f(1, 0.6f), f(2, 0.8f), f(3, 1.0f));
                ps.set(r, 0.5f * f(0, 0.9f), 0.5f * f(1, 0.7f), 0.5f * f(2, 0.5f), f(3, 0.3f));
            }
            for (std::uint32_t slot = 0; slot < 16; ++slot) {
                db.tex[slot] = texSets[i & 1][slot];
                db.smp[slot] = texSets[i & 1][slot] != 0 ? linear : 0;
            }
        };
        runSceneCase(h, c, 4, v2 ? 1 : 0);
    }
}

shader::MacroList maskedZRequest() {
    shader::MacroList m = simpleRequest();
    m.emplace_back("XFX_MASKED", "true");
    return m;
}

void testScene(Harness& h) {
    testSceneAsm(h);
    testSceneUber(h, "diffuse * vertex colour", simpleRequest(), {}, false);
    testSceneUber(h, "normal map, specular, lightmap, lights", heavyRequest(), {}, true);
    // the hooks of the G-buffer + GI variants without the extra render targets: window 3 read through rvkPsWin
    testSceneUber(h, "gi hooks", heavyRequest(), "#define RVK_MV_AMBIENT 1\n#define RVK_MV_AMB_REG 15\n#define RVK_MV_HAS_IND 1\n#define RVK_MV_IND_REG 14\n#define RVK_MV_KIND_SKINNED 3\n", true);
    // S-1a: the z pass entry (z pre-pass, cascades: the largest class of owned draws), masked, and the G-buffer + GI hooks of the owned path
    testSceneUber(h, "masked", maskedZRequest(), {}, false, false, "PS_DefaultZPass");
    testSceneUber(h, "gbuffer + gi hooks", heavyRequest(), kHookDefines, true);
    testSceneCorpus(h);
    // scene ABI v2 (frame-ownership.md 7.19): the same draws with the view registers from a view table slot; the wrong-slot mutation must differ
    testSceneAsm(h, true);
    testSceneAsm(h, true, true);
    testSceneUber(h, "diffuse * vertex colour", simpleRequest(), {}, false, true);
    testSceneUber(h, "normal map, specular, lightmap, lights", heavyRequest(), {}, true, true);
    testSceneUber(h, "gi hooks", heavyRequest(), "#define RVK_MV_AMBIENT 1\n#define RVK_MV_AMB_REG 15\n#define RVK_MV_HAS_IND 1\n#define RVK_MV_IND_REG 14\n#define RVK_MV_KIND_SKINNED 3\n", true, true);
    testSceneUber(h, "masked", maskedZRequest(), {}, false, true, "PS_DefaultZPass");
    testSceneUber(h, "gbuffer + gi hooks", heavyRequest(), kHookDefines, true, true);
    testSceneCorpus(h, true);
}
