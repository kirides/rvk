// ---- test: the opaque-first traces (rt-reflections.md 3.3.1) -------------------------------------------------------------------------------
// (included by parity.cpp inside its anonymous namespace)
//
// shaders/include/rvk_rt_trace.hlsli traces water.rt's reflection and shadow rays in two queries (rvkRtTraceClosestOpaqueFirst /
// rvkRtVisibilityOpaqueFirst: the instances without RVK_RT_MASK_ALPHA_TESTED first, then the cut-outs only, bounded by that hit). The claim: the
// same hit as one query (rvkRtTraceClosest / rvkRtVisibility) up to exact-t ties. A synthetic TLAS of one quad BLAS in nine instances: opaque and
// alpha-tested (a checker texture's alpha, GREATER 128 through the production rvkRtCandidatePasses), in front of / behind / between each other,
// tilted, coplanar (exact ties), a character (mask DYNAMIC) and two instances that break the renderer's mask contract (a non-opaque instance
// with the mask ALPHA_TESTED | DYNAMIC, a non-opaque one with the mask OPAQUE: both must still give the same answer). Rays: a grid along +z,
// the reverse grid along -z, oblique rays, sky rays, rays starting between the layers (tMin > 0) and rays ending between them (tMax).
// Per ray and flags (0, NO_DYNAMIC): hit / t bits / record / primitive / barycentrics compared exactly; a ray where both forms hit at the
// same t on different triangles is an exact tie (counted, listed by record pair, not a failure); the visibilities must be equal everywhere.

struct OfRay {
    float originMin[4];
    float dirMax[4];
};
static_assert(sizeof(OfRay) == 32);

void testOpaqueFirst(Harness& h) {
    rhi::Device& dev = h.dev;
    const int before = failures;
    const char* name = "opaque-first traces";
    if (!dev.caps().rayQuery) {
        std::printf("  %s: SKIP (no ray query)\n", name);
        return;
    }
    Blobs cs;
    if (!compileBoth("test/rhi_rt_opaquefirst.comp.hlsl", {}, "CSMain", "cs_6_6", cs, 1)) {
        check(false, std::format("{}: shader", name));
        return;
    }
    constexpr VkBufferUsageFlags kSrv = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;

    // the quad: (-1, -1) .. (1, 1) at z = 0, uv = ((x + 1) / 2, (1 - y) / 2); position float3 + uv float2 (20 bytes)
    const float verts[4][5] = {{-1, -1, 0, 0, 1}, {1, -1, 0, 1, 1}, {1, 1, 0, 1, 0}, {-1, 1, 0, 0, 0}};
    const std::uint32_t idx[6] = {0, 1, 2, 0, 2, 3};
    rhi::Buffer* bVerts = h.makeBuffer(verts, sizeof(verts), VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | kSrv, "opaque-first vertices");
    rhi::Buffer* bIdx = h.makeBuffer(idx, sizeof(idx), VK_BUFFER_USAGE_INDEX_BUFFER_BIT | kSrv, "opaque-first indices");
    // the alpha: a checker of 2 x 2 texels (one mip: the ray cone's level does not matter), point sampler, repeat; four textures of different sizes
    // (8 .. 64 texels: the size query of a non-uniform bindless index inside the candidate loop is checked against the width each record carries)
    std::uint32_t texs[4]{};
    for (std::uint32_t k = 0; k < 4; ++k) {
        texs[k] = h.addPattern(8u << k, 8u << k, [](std::uint32_t x, std::uint32_t y) {
            return std::array<std::uint8_t, 4>{200, 150, 100, static_cast<std::uint8_t>((((x >> 1) + (y >> 1)) & 1u) != 0 ? 255 : 0)};
        }, "opaque-first checker");
    }
    const rhi::SamplerIndex sampler = dev.createSampler({VK_FILTER_NEAREST, VK_SAMPLER_ADDRESS_MODE_REPEAT});
    if (bVerts == nullptr || bIdx == nullptr || texs[0] == 0 || texs[1] == 0 || texs[2] == 0 || texs[3] == 0 || sampler == 0) {
        check(false, std::format("{}: resources", name));
        return;
    }
    rhi::TriangleGeometry tri;
    tri.vertices = bVerts;
    tri.vertexStride = 20;
    tri.vertexCount = 4;
    tri.indices = bIdx;
    tri.indexCount = 6;
    tri.indexType = VK_INDEX_TYPE_UINT32;
    tri.opaque = true;  // (as the renderer's BLASes: the instance flags decide)
    auto blas = dev.createBlas({&tri, 1});

    // the instances: scale, rotation about y (degrees), offset, mask, non-opaque, alpha-tested record
    struct Spec {
        const char* what;
        float scale, rotY, x, y, z;
        std::uint32_t mask;
        bool nonOpaque;
    };
    const Spec specs[] = {
        {"opaque back wall", 1.0f, 0.0f, 0.0f, 0.0f, 5.0f, abi::kRtMaskOpaque, false},
        {"cut-out in front", 0.8f, 0.0f, 0.1f, 0.0f, 2.0f, abi::kRtMaskAlphaTested, true},
        {"cut-out behind", 1.25f, 0.0f, 0.0f, 0.0f, 8.0f, abi::kRtMaskAlphaTested, true},
        {"opaque between", 0.4f, 0.0f, -0.5f, 0.5f, 3.0f, abi::kRtMaskOpaque, false},
        {"cut-out coplanar with the wall (ties)", 0.5f, 0.0f, 0.5f, -0.5f, 5.0f, abi::kRtMaskAlphaTested, true},
        {"cut-out tilted through the layers", 0.9f, 60.0f, -0.3f, -0.4f, 3.0f, abi::kRtMaskAlphaTested, true},
        {"character (opaque)", 0.3f, 0.0f, 0.6f, 0.6f, 4.0f, abi::kRtMaskDynamic, false},
        {"non-opaque with mask cut-out | dynamic", 0.35f, 0.0f, -0.6f, -0.6f, 4.5f, abi::kRtMaskAlphaTested | abi::kRtMaskDynamic, true},
        {"non-opaque with mask opaque", 0.35f, 0.0f, 0.1f, -0.7f, 2.5f, abi::kRtMaskOpaque, true},
    };
    constexpr std::uint32_t kInstances = static_cast<std::uint32_t>(std::size(specs));
    auto tlas = dev.createTlas(kInstances);
    rhi::Buffer* bInst = h.makeBuffer(nullptr, kInstances * sizeof(VkAccelerationStructureInstanceKHR), kSrv, "opaque-first instances");
    if (!blas || !tlas || bInst == nullptr) {
        check(false, std::format("{}: acceleration structures", name));
        return;
    }
    auto* inst = static_cast<VkAccelerationStructureInstanceKHR*>(bInst->map());
    std::vector<abi::RtGeometryRecord> recs(kInstances);
    const auto lo = [](std::uint64_t v) { return static_cast<std::uint32_t>(v); };
    const auto hi = [](std::uint64_t v) { return static_cast<std::uint32_t>(v >> 32); };
    const std::uint64_t vRef = refOf(*bVerts), iRef = refOf(*bIdx);
    for (std::uint32_t k = 0; k < kInstances; ++k) {
        const Spec& s = specs[k];
        const float a = s.rotY * 3.14159265f / 180.0f, c = std::cos(a) * s.scale, sn = std::sin(a) * s.scale;
        VkAccelerationStructureInstanceKHR& v = inst[k];
        v = {};
        v.transform = {{{c, 0, sn, s.x}, {0, s.scale, 0, s.y}, {-sn, 0, c, s.z}}};
        v.instanceCustomIndex = k;  // one record per instance (record = instance custom index + geometry 0)
        v.mask = s.mask;
        v.flags = VK_GEOMETRY_INSTANCE_TRIANGLE_FACING_CULL_DISABLE_BIT_KHR |
                  (s.nonOpaque ? VK_GEOMETRY_INSTANCE_FORCE_NO_OPAQUE_BIT_KHR : VK_GEOMETRY_INSTANCE_FORCE_OPAQUE_BIT_KHR);
        v.accelerationStructureReference = blas->address();
        abi::RtGeometryRecord& r = recs[k];
        r.vertices[0] = lo(vRef);
        r.vertices[1] = hi(vRef);
        r.indices[0] = lo(iRef);
        r.indices[1] = hi(iRef);
        r.stride = 20;
        r.flags = abi::kRtRecordIndex32 | (s.nonOpaque ? abi::kRtRecordAlphaTested : 0u);
        r.normal = abi::kRtAttribAbsent;
        r.texcoord0 = 12u | (1u << 16);  // FLOAT2
        r.texcoord1 = r.texcoord6 = r.color0 = abi::kRtAttribAbsent;
        r.instanceId = 8u << (k % 4);  // (the test shader checks its texture's width against it)
        r.albedoTex = texs[k % 4];
        r.albedoSampler = sampler;
        r.alpha = 5u | (128u << 8);                  // GREATER 128
        // the diffuse alpha; the texture's log2(area) x 8 as the RT scene writes it (rvkRtTexArea: the cone LOD's texel count)
        const std::uint32_t area8 = 8u * (2u * (3u + k % 4));
        r.uvSet = (3u << abi::kRtUvOpacityShift) | (area8 << abi::kRtUvAlbedoAreaShift) | (area8 << abi::kRtUvOpacityAreaShift);
        const float uvScale = 1.0f + 0.5f * static_cast<float>(k % 3);  // a different checker size per instance
        r.uvMap[0][0] = uvScale;
        r.uvMap[0][2] = 0.03f * static_cast<float>(k);
        r.uvMap[1][1] = uvScale;
        r.uvMap[1][2] = 0.05f * static_cast<float>(k);
    }
    rhi::Buffer* bRecs = h.makeBuffer(recs.data(), recs.size() * sizeof(abi::RtGeometryRecord), kSrv, "opaque-first records");

    // the rays
    std::vector<OfRay> rays;
    std::uint32_t lcg = 12345u;
    const auto rnd = [&]() {
        lcg = lcg * 1664525u + 1013904223u;
        return static_cast<float>(lcg >> 8) / 16777216.0f;
    };
    const auto add = [&](float ox, float oy, float oz, float dx, float dy, float dz, float tMin, float tMax) {
        const float l = std::sqrt(dx * dx + dy * dy + dz * dz);
        rays.push_back({{ox, oy, oz, tMin}, {dx / l, dy / l, dz / l, tMax}});
    };
    constexpr int kGrid = 64;
    for (int y = 0; y < kGrid; ++y) {
        for (int x = 0; x < kGrid; ++x) {
            const float px = -1.5f + 3.0f * (static_cast<float>(x) + 0.5f) / kGrid, py = -1.5f + 3.0f * (static_cast<float>(y) + 0.5f) / kGrid;
            add(px, py, -10.0f, 0, 0, 1, 0.0f, 100.0f);    // front to back
            add(px, py, 20.0f, 0, 0, -1, 0.0f, 100.0f);    // back to front (the cut-out behind the wall first)
        }
    }
    for (int i = 0; i < 2048; ++i) {  // oblique rays through the stack
        const float ox = -2.0f + 4.0f * rnd(), oy = -2.0f + 4.0f * rnd();
        add(ox, oy, -6.0f, (rnd() - 0.5f) * 0.8f, (rnd() - 0.5f) * 0.8f, 1.0f, 0.0f, 100.0f);
    }
    for (int i = 0; i < 512; ++i) {  // sky rays: away from everything / passing beside it
        const float a = 6.2831853f * rnd();
        add(-1.0f + 2.0f * rnd(), -1.0f + 2.0f * rnd(), -4.0f, std::cos(a), std::sin(a), -0.2f - rnd(), 0.0f, 100.0f);
        add(3.0f + rnd(), -1.0f + 2.0f * rnd(), 0.0f, 0.1f * rnd(), 0.2f * (rnd() - 0.5f), 1.0f, 0.0f, 100.0f);
    }
    for (int i = 0; i < 1024; ++i) {  // starting between the layers (tMin > 0), ending between them (tMax)
        const float ox = -1.3f + 2.6f * rnd(), oy = -1.3f + 2.6f * rnd();
        add(ox, oy, -1.0f, 0.1f * (rnd() - 0.5f), 0.1f * (rnd() - 0.5f), 1.0f, 3.2f + rnd(), 100.0f);
        add(ox, oy, -1.0f, 0.1f * (rnd() - 0.5f), 0.1f * (rnd() - 0.5f), 1.0f, 0.0f, 3.5f + 4.0f * rnd());
        add(ox, oy, -1.0f, 0, 0, 1, 0.0f, 6.0f);  // ends exactly at the wall (t 6): a cut-out / wall at tMax
    }
    const std::uint32_t count = static_cast<std::uint32_t>(rays.size());
    constexpr std::uint32_t kWords = 40;
    rhi::Buffer* bRays = h.makeBuffer(rays.data(), rays.size() * sizeof(OfRay), kSrv, "opaque-first rays");
    abi::FrameBlock fb{};
    const std::uint64_t recsRef = bRecs != nullptr ? refOf(*bRecs) : 0;
    fb.rtScene[0] = 1;  // (rvkRtValid: a non-zero TLAS address)
    fb.rtScene[2] = lo(recsRef);
    fb.rtScene[3] = hi(recsRef);
    fb.rtInfo[0] = kInstances;
    fb.rtInfo[1] = kInstances;
    fb.rtInfo[2] = abi::kFrameRtValid;
    fb.rtInfo[3] = tlas->tlasSlot();
    rhi::Buffer* bFrame = h.makeBuffer(&fb, sizeof(fb), kSrv, "opaque-first frame block");
    constexpr VkBufferUsageFlags kRw = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    auto out = dev.createBuffer({static_cast<std::uint64_t>(count) * kWords * 4, kRw, rhi::Memory::Device, "opaque-first results"});
    auto readback = dev.createBuffer({static_cast<std::uint64_t>(count) * kWords * 4, VK_BUFFER_USAGE_TRANSFER_DST_BIT, rhi::Memory::Readback, "opaque-first readback"});
    auto pipeline = dev.createComputePipeline({cs.shader(), "opaque-first traces"});
    if (bRecs == nullptr || bRays == nullptr || bFrame == nullptr || !out || !readback || !pipeline) {
        check(false, std::format("{}: resources / pipeline", name));
        return;
    }
    struct Push {
        std::uint32_t frame[2], passId, passFlags, rays[2], outBuf[2], count, pad[3];
    } push{};
    static_assert(sizeof(Push) == 48);
    const auto fr = bFrame->shaderRef(0), ra = bRays->shaderRef(0), ro = out->shaderRefRw(0);
    push.frame[0] = fr[0];
    push.frame[1] = fr[1];
    push.rays[0] = ra[0];
    push.rays[1] = ra[1];
    push.outBuf[0] = ro[0];
    push.outBuf[1] = ro[1];
    push.count = count;
    push.passFlags = std::getenv("RVK_OF_PLAINREAD") != nullptr ? 1u : 0u;  // (diagnostics: the plain query also reads CandidateTriangleRayT)
    dev.submitAndWait([&](rhi::CommandList& cl) {
        cl.bufferBarrier(*out, rhi::BufferAccess::ShaderWrite);
        cl.buildBlas(*blas);
        cl.buildTlas(*tlas, *bInst, 0, kInstances);
        cl.bindPipeline(*pipeline);
        cl.setPushConstants(&push, sizeof(push));
        cl.dispatch((count + 63) / 64, 1, 1);
        cl.bufferBarrier(*out);
        cl.copyBuffer(*readback, 0, *out, 0, static_cast<std::uint64_t>(count) * kWords * 4);
    });
    readback->invalidate();
    const auto* w = static_cast<const std::uint32_t*>(readback->map());

    // compare
    std::uint64_t differing = 0, ties = 0, hits = 0, misses = 0, visDiffer = 0, blocked = 0, open = 0;
    std::array<std::uint64_t, 16> hitsByRecord{};
    std::map<std::pair<std::uint32_t, std::uint32_t>, std::uint64_t> tiePairs;
    std::string first;
    std::vector<float> cross;  // the opaque-first answers for the backend cross-check (hit, t, visibility)
    cross.reserve(static_cast<std::size_t>(count) * 6);
    for (std::uint32_t i = 0; i < count; ++i) {
        const std::uint32_t* r = w + static_cast<std::size_t>(i) * kWords;
        for (std::uint32_t k = 0; k < 2; ++k) {
            const std::uint32_t* a = r + k * 12;  // one query
            const std::uint32_t* b = a + 6;       // opaque first
            float tb;
            std::memcpy(&tb, &b[1], 4);
            cross.push_back(static_cast<float>(b[0]));
            cross.push_back(b[0] != 0 ? tb : 0.0f);
            if (a[0] != 0) {
                ++hits;
                if (a[2] < hitsByRecord.size()) ++hitsByRecord[a[2]];
            } else {
                ++misses;
            }
            bool bad = a[0] != b[0] || a[1] != b[1];
            if (!bad && a[0] != 0 && (a[2] != b[2] || a[3] != b[3])) {
                ++ties;  // the same t on another triangle: an exact tie
                ++tiePairs[{std::min(a[2], b[2]), std::max(a[2], b[2])}];
            } else if (!bad && a[0] != 0 && (a[4] != b[4] || a[5] != b[5])) {
                bad = true;  // the same triangle must give the same barycentrics
            }
            if (bad) {
                if (differing < 12 && std::getenv("RVK_OF_VERBOSE") != nullptr) {
                    float ta, tb2;
                    std::memcpy(&ta, &a[1], 4);
                    std::memcpy(&tb2, &b[1], 4);
                    const OfRay& y = rays[i];
                    std::printf("    differing ray %u flags %u: o %g %g %g d %g %g %g t %g..%g: one %u t %g rec %d prim %d | of %u t %g rec %d prim %d\n", i, k, y.originMin[0], y.originMin[1],
                                y.originMin[2], y.dirMax[0], y.dirMax[1], y.dirMax[2], y.originMin[3], y.dirMax[3], a[0], ta, static_cast<int>(a[2]), static_cast<int>(a[3]), b[0], tb2,
                                static_cast<int>(b[2]), static_cast<int>(b[3]));
                }
                if (differing++ == 0) {
                    float ta, tb2;
                    std::memcpy(&ta, &a[1], 4);
                    std::memcpy(&tb2, &b[1], 4);
                    const OfRay& y = rays[i];
                    first = std::format("ray {} (flags {}; origin {} {} {}, dir {} {} {}, t {} .. {}): one query hit {} t {} record {} primitive {}, opaque-first hit {} t {} record {} primitive {}", i,
                                        k == 0 ? "0" : "NO_DYNAMIC", y.originMin[0], y.originMin[1], y.originMin[2], y.dirMax[0], y.dirMax[1], y.dirMax[2], y.originMin[3],
                                        y.dirMax[3], a[0], ta, a[2], a[3], b[0], tb2, b[2], b[3]);
                }
            }
        }
        for (std::uint32_t k = 0; k < 2; ++k) {
            float va, vb;
            std::memcpy(&va, &r[24 + 2 * k], 4);
            std::memcpy(&vb, &r[25 + 2 * k], 4);
            cross.push_back(vb);
            if (va != vb && visDiffer++ == 0 && first.empty()) first = std::format("ray {} (flags {}): visibility {} (one query) vs {} (opaque first)", i, k, va, vb);
            (va == 0.0f ? blocked : open)++;
        }
    }
    // the raw ray-t semantics (words 28..31): the closest hit forced opaque, forced non-opaque (candidate t, committed t) must agree, also with tMin > 0
    std::uint64_t rawBad[2] = {}, rawRays[2] = {}, plainRays = 0, plainBad = 0, plainPlusTMin = 0;
    std::string rawFirst;
    for (std::uint32_t i = 0; i < count; ++i) {
        const std::uint32_t* r = w + static_cast<std::size_t>(i) * kWords;
        float tO, tC, tK;
        std::memcpy(&tO, &r[28], 4);
        std::memcpy(&tC, &r[29], 4);
        std::memcpy(&tK, &r[30], 4);
        float tP;
        std::memcpy(&tP, &r[31], 4);
        const int cat = rays[i].originMin[3] > 0.0f ? 1 : 0;
        if (cat == 1 && tO >= 0.0f) {
            ++plainRays;
            if (std::fabs(tP - tO) > 1e-4f * (1.0f + tO)) {
                ++plainBad;
                if (std::fabs(tP - (tO + rays[i].originMin[3])) <= 1e-3f * (1.0f + tO)) ++plainPlusTMin;
            }
        }
        ++rawRays[cat];
        const bool ok = (tO < 0.0f && tK < 0.0f) || (std::fabs(tO - tK) <= 1e-4f * (1.0f + tO) && std::fabs(tO - tC) <= 1e-4f * (1.0f + tO));
        if (!ok && rawBad[cat]++ == 0 && rawFirst.empty()) {
            rawFirst = std::format("ray {} (tMin {}): forced opaque t {}, non-opaque candidate t {}, committed t {}", i, rays[i].originMin[3], tO, tC, tK);
        }
    }
    std::printf("  %s: ray t semantics: tMin = 0: %llu of %llu rays disagree; tMin > 0: %llu of %llu disagree%s%s\n", name, static_cast<unsigned long long>(rawBad[0]),
                static_cast<unsigned long long>(rawRays[0]), static_cast<unsigned long long>(rawBad[1]), static_cast<unsigned long long>(rawRays[1]),
                rawFirst.empty() ? "" : "; first: ", rawFirst.c_str());
    std::printf("  %s: NOTE the platform's committed t of a non-opaque hit with a plain RayDesc and tMin > 0: %llu of %llu rays wrong, %llu of them = t + tMin%s\n", name,
                static_cast<unsigned long long>(plainBad), static_cast<unsigned long long>(plainRays), static_cast<unsigned long long>(plainPlusTMin),
                plainBad != 0 ? " (rvkRay starts such rays at tMin on this backend)" : "");
    check(rawBad[0] == 0 && rawBad[1] == 0, std::format("{}: the ray query's t of non-opaque hits ({} / {} rays): {}", name, rawBad[0], rawBad[1], rawFirst));
    // the size of a non-uniform bindless texture (rvkSize2D = GetDimensions) inside the candidate loop and outside it
    std::uint64_t sizeBadIn = 0, sizeChecked = 0, sizeBadOut = 0;
    for (std::uint32_t i = 0; i < count; ++i) {
        const std::uint32_t* r = w + static_cast<std::size_t>(i) * kWords;
        sizeBadIn += r[32];
        sizeChecked += r[33];
        sizeBadOut += r[34];
    }
    // a NOTE, not a failure: the platform's behaviour (rvkSize2D is for wave-uniform indices; the RT records carry their sizes, rvkRtTexArea)
    std::printf("  %s: NOTE texture sizes of a lane-varying bindless index (GetDimensions): inside the candidate loop %llu of %llu wrong, outside it %llu of %u wrong%s\n",
                name, static_cast<unsigned long long>(sizeBadIn), static_cast<unsigned long long>(sizeChecked), static_cast<unsigned long long>(sizeBadOut), count,
                sizeBadIn + sizeBadOut != 0 ? " (the first lane's size: rvkRtTexArea reads the records' sizes instead)" : "");
    std::string byRecord, tieList;
    for (std::uint32_t k = 0; k < kInstances; ++k) byRecord += std::format("{}{} {}", k == 0 ? "" : ", ", k, hitsByRecord[k]);
    for (const auto& [p, n] : tiePairs) tieList += std::format("{}records {} / {}: {}", tieList.empty() ? "" : ", ", p.first, p.second, n);
    std::printf("  %s: %u rays x 2 flags: %llu hits (by record: %s), %llu misses, %llu exact-t ties (%s), %llu differing; visibility %llu blocked / %llu open, %llu differing\n", name,
                count, static_cast<unsigned long long>(hits), byRecord.c_str(), static_cast<unsigned long long>(misses), static_cast<unsigned long long>(ties),
                tieList.empty() ? "none" : tieList.c_str(), static_cast<unsigned long long>(differing), static_cast<unsigned long long>(blocked),
                static_cast<unsigned long long>(open), static_cast<unsigned long long>(visDiffer));
    check(differing == 0 && visDiffer == 0, std::format("{}: {} differing hit(s), {} differing visibilit(ies): {}", name, differing, visDiffer, first));
    // coverage: every instance hit somewhere, misses and both visibility answers present, the cut-outs really reject (rays through the front
    // cut-out reach the wall: the wall's hits outnumber what the opaque layers alone leave visible)
    bool allHit = true;
    for (std::uint32_t k = 0; k < kInstances; ++k) allHit = allHit && hitsByRecord[k] > 0;
    check(allHit && misses > 0 && blocked > 0 && open > 0, std::format("{}: coverage (hits by record {}; misses {}, blocked {}, open {})", name, byRecord, misses, blocked, open));
    crossCheck(dev, name, cross, 1e-4f, 0.002f);
    std::printf("  %s: %s\n", name, failures == before ? "PASS" : "FAIL");
}
