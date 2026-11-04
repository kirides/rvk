// rvk_shadertool dxil-check: included into main.cpp's anonymous namespace.
// DXIL parity (docs/research/hlsl-rhi.md, "DXIL parity (S1)"): one HLSL text, two targets. Every shader must compile to valid
// SPIR-V (spirv-val) AND to DXIL SM 6.6 that dxil.dll validated and signed (the validator runs inside the compile; a missing dxil.dll
// yields an unsigned container, which counts as a failure).

struct Both {
    bool spirvOk = false, dxilOk = false, dxilSigned = false;
    std::uint32_t pushBytes = 0;  // the DXIL's push constant block
    std::string spirvMsg, dxilMsg;
    bool ok() const { return spirvOk && dxilOk && dxilSigned; }
};

// the first error line of a compiler log (path, line and message), or its first line
std::string firstError(const std::string& log) {
    std::size_t p = 0;
    while (p < log.size()) {
        const std::size_t e = std::min(log.find('\n', p), log.size());
        const std::string line = log.substr(p, e - p);
        if (line.find("error") != std::string::npos) return line.substr(0, 400);
        p = e + 1;
    }
    return log.substr(0, std::min(log.find('\n'), log.size())).substr(0, 400);
}

// the message without the file position, for grouping equal failures
std::string errorKey(const std::string& line) {
    std::string k = line;
    const auto at = k.find("error:");
    if (at != std::string::npos) k = k.substr(at + 6);
    while (!k.empty() && k.front() == ' ') k.erase(k.begin());
    return k.substr(0, 150);
}

Both compileBoth(rvk::shader::dxc::Request r, spvtools::SpirvTools& tools, bool checkRootSignature = true) {
    Both b;
    r.cache = false;
    r.target = rvk::shader::dxc::Target::Spirv;
    const auto sp = rvk::shader::dxc::compile(r);
    if (!sp.ok()) {
        b.spirvMsg = "compile: " + firstError(sp.log);
    } else {
        std::string vlog;
        tools.SetMessageConsumer([&](spv_message_level_t, const char*, const spv_position_t&, const char* m) { vlog += m; vlog += '\n'; });
        if (tools.Validate(sp.words)) b.spirvOk = true;
        else b.spirvMsg = "spirv-val: " + vlog.substr(0, vlog.find('\n'));
    }
    r.target = rvk::shader::dxc::Target::Dxil;
    const auto dx = rvk::shader::dxc::compile(r);
    if (!dx.ok()) {
        b.dxilMsg = "compile: " + firstError(dx.log);
    } else {
        b.dxilOk = true;
        b.dxilSigned = dx.dxilSigned;
        b.pushBytes = rvk::shader::dxc::dxilPushBytes(dx.bytes);
        if (!dx.dxilSigned) b.dxilMsg = "DXIL is not signed (dxil.dll missing?)";
        // the fixed root signature: only the push constants (<= 128 bytes), the root CBV and the storage image table are bound
        if (const std::string v = rvk::shader::dxc::dxilContractViolation(dx.bytes); !v.empty() && checkRootSignature) {
            b.dxilOk = false;
            b.dxilMsg = "root signature: " + v;
        }
    }
    return b;
}

template <typename Fn>
void parallelFor(std::size_t n, int jobs, Fn&& fn) {
    std::atomic<std::size_t> next{0};
    std::vector<std::thread> pool;
    const int count = std::max(1, jobs);
    for (int t = 0; t < count; ++t) {
        pool.emplace_back([&] {
            for (std::size_t i; (i = next.fetch_add(1)) < n;) fn(i);
        });
    }
    for (auto& t : pool) t.join();
}

int defaultJobs() {
    const unsigned hw = std::thread::hardware_concurrency();
    return static_cast<int>(hw > 2 ? hw - 2 : 1u);
}

// Shaders that compile to DXIL but cannot be used with the fixed root signature, and why (documented exceptions, docs/research/hlsl-rhi.md "DXIL parity (S1)"):
// rt_query.comp is the Vulkan floor's ray query self-test (src/vk/raytracing.cpp): it binds its TLAS as a push descriptor of its own layout; the DX12 backend's
// check of the same scene is rhi_rt.comp (rvk_rhitest).
bool rootSignatureExemption(const std::string& label) { return label.starts_with("test/rt_query.comp"); }

// per-item result of a dxil-check run: label (name + variant), family, outcome
struct Item {
    std::string label, family;
    Both r;
};

// prints the summary: counts per family (SPIR-V ok / DXIL ok), failures grouped by first error
int reportDxil(const std::vector<Item>& items, int maxPrinted, bool quiet) {
    std::map<std::string, std::array<int, 4>> fam;  // total, spirv ok, dxil ok (signed), both
    std::map<std::string, std::vector<std::string>> byError;
    int printed = 0, failed = 0;
    for (const Item& it : items) {
        auto& f = fam[it.family];
        ++f[0];
        f[1] += it.r.spirvOk;
        f[2] += it.r.dxilOk && it.r.dxilSigned;
        f[3] += it.r.ok();
        if (it.r.ok()) {
            if (!quiet) std::printf("PASS %s\n", it.label.c_str());
            continue;
        }
        ++failed;
        const std::string msg = !it.r.spirvOk ? "SPIR-V " + it.r.spirvMsg : "DXIL " + it.r.dxilMsg;
        byError[errorKey(msg)].push_back(it.label);
        if (printed < maxPrinted) {
            ++printed;
            std::printf("FAIL %s: %s\n", it.label.c_str(), msg.c_str());
        }
    }
    std::printf("\n%-22s %7s %9s %9s %9s\n", "family", "total", "spirv ok", "dxil ok", "both");
    std::uint32_t largestPush = 0;
    std::string largestPushName;
    for (const Item& it : items) {
        if (it.r.pushBytes > largestPush) {
            largestPush = it.r.pushBytes;
            largestPushName = it.label;
        }
    }
    int total = 0, both = 0;
    for (const auto& [k, f] : fam) {
        std::printf("%-22s %7d %9d %9d %9d\n", k.c_str(), f[0], f[1], f[2], f[3]);
        total += f[0];
        both += f[3];
    }
    std::printf("%-22s %7d %29d\n", "TOTAL", total, both);
    std::printf("largest push constant block: %u bytes (%s; the root signature holds 128)\n", largestPush, largestPushName.c_str());
    if (!byError.empty()) {
        std::printf("\nfailure reasons (%d failing):\n", failed);
        for (const auto& [m, l] : byError) std::printf("%6zu  %s   e.g. %s\n", l.size(), m.c_str(), l.front().c_str());
    }
    return failed == 0 ? 0 : 1;
}

bool requireDxc() {
    if (rvk::shader::dxc::available()) return true;
    std::printf("DXC unavailable: %s\n", std::string(rvk::shader::dxc::unavailableReason()).c_str());
    return false;
}

// every embedded shader with a stage extension (ocean_fft once per size)
int cmdDxilEmbedded(int argc, char** argv) {
    std::string filter;
    int jobs = defaultJobs();
    bool quiet = false;
    for (int i = 3; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--filter" && i + 1 < argc) filter = argv[++i];
        else if (a == "--jobs" && i + 1 < argc) jobs = std::atoi(argv[++i]);
        else if (a == "--quiet") quiet = true;
    }
    if (!requireDxc()) return 2;
    struct Job {
        std::string name, defines, family;
    };
    std::vector<Job> jobList;
    for (const auto& f : embedded::hlslSources()) {
        std::string name(f.name);
        if (!name.ends_with(".hlsl")) continue;
        name.resize(name.size() - 5);
        if (!(name.ends_with(".comp") || name.ends_with(".vert") || name.ends_with(".frag"))) continue;
        if (!filter.empty() && name.find(filter) == std::string::npos) continue;
        const std::string family = name.substr(0, name.find('/'));
        if (name == "water/ocean_fft.comp") {
            for (int n = 128; n <= 512; n *= 2) {
                int l = 0;
                while ((1 << l) < n) ++l;
                jobList.push_back({name, std::format("OCEAN_N={};OCEAN_LOG2N={}", n, l), family});
            }
        } else {
            jobList.push_back({name, {}, family});
        }
    }
    std::vector<Item> items(jobList.size());
    parallelFor(jobList.size(), jobs, [&](std::size_t i) {
        const Job& j = jobList[i];
        spvtools::SpirvTools tools(SPV_ENV_VULKAN_1_3);
        rvk::shader::dxc::Request r;
        r.file = j.name + ".hlsl";
        const std::string ext = j.name.substr(j.name.rfind('.'));
        r.entry = ext == ".comp" ? "CSMain" : ext == ".vert" ? "VSMain" : "PSMain";
        r.profile = ext == ".comp" ? "cs_6_6" : ext == ".vert" ? "vs_6_6" : "ps_6_6";
        for (std::size_t pos = 0; pos < j.defines.size();) {
            const auto end = std::min(j.defines.find(';', pos), j.defines.size());
            const std::string d = j.defines.substr(pos, end - pos);
            const auto eq = d.find('=');
            if (!d.empty()) r.defines.push_back({d.substr(0, eq), eq == std::string::npos ? std::string() : d.substr(eq + 1)});
            pos = end + 1;
        }
        items[i].label = j.defines.empty() ? j.name : j.name + " [" + j.defines + "]";
        items[i].family = j.family;
        items[i].r = compileBoth(r, tools, !rootSignatureExemption(items[i].label));
    });
    return reportDxil(items, 200, quiet);
}

// the translated D3D9 shaders of a corpus (and one enhancement variant): each translated as `corpus` does, compiled to SPIR-V and DXIL
int cmdDxilCorpus(const fs::path& dir, int jobs, int maxErrors, const TranslateOptions& baseOptions, VariantFn variant) {
    if (!requireDxc()) return 2;
    std::vector<fs::path> files;
    for (const auto& e : fs::directory_iterator(dir)) {
        const auto name = e.path().filename().string();
        if (e.is_regular_file() && (name.starts_with("vs") || name.starts_with("ps"))) files.push_back(e.path());
    }
    std::ranges::sort(files);
    std::vector<Item> items(files.size());
    std::vector<char> skip(files.size(), 0);
    const auto t0 = std::chrono::steady_clock::now();
    const int opt = std::getenv("RVK_DXC_OPT") != nullptr ? std::atoi(std::getenv("RVK_DXC_OPT")) : 1;
    parallelFor(files.size(), jobs, [&](std::size_t i) {
        const fs::path& f = files[i];
        spvtools::SpirvTools tools(SPV_ENV_VULKAN_1_3);
        const std::string fname = f.filename().string();
        items[i].label = fname;
        items[i].family = fname.substr(0, fname.find('_'));
        const std::vector<std::uint32_t> tokens = readTokens(f);
        TranslateOptions options = baseOptions;
        const DecodeResult dec = decode(tokens);
        if (variant != nullptr) {
            const HlslResult plain = dec.ok() ? toHlsl(dec.program) : HlslResult{};
            rvk::d3d9::Shader sh;
            sh.stage = dec.ok() ? dec.program.version.stage : Stage::Vertex;
            sh.reflection = plain.reflection;
            if (dec.ok() && plain.ok()) sh.semantics = rvk::d3d9::shaderSemantics(tokens, plain.reflection);
            if (!dec.ok() || !plain.ok() || !variant(sh, options)) {
                skip[i] = 1;
                return;
            }
        }
        if (!dec.ok()) {
            items[i].r.spirvMsg = "decode: " + dec.error;
            return;
        }
        const HlslResult h = toHlsl(dec.program, options);
        if (!h.ok()) {
            items[i].r.spirvMsg = "hlsl: " + h.error;
            return;
        }
        rvk::shader::dxc::Request r;
        r.file = "translated.hlsl";
        r.source = h.hlsl;
        const bool vs = dec.program.version.stage == Stage::Vertex;
        r.entry = vs ? "VSMain" : "PSMain";
        r.profile = vs ? "vs_6_6" : "ps_6_6";
        r.optLevel = opt;
        items[i].r = compileBoth(r, tools);
    });
    std::vector<Item> kept;
    int skipped = 0;
    for (std::size_t i = 0; i < items.size(); ++i) {
        if (skip[i]) ++skipped;
        else kept.push_back(std::move(items[i]));
    }
    const int rc = reportDxil(kept, maxErrors, true);
    const double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    std::printf("\n%zu shaders in %.1f s (%d without the variant: skipped)\n", kept.size(), secs, skipped);
    return rc;
}

// ---- ubershader requests (JSON lines of debug.material_sources) ----------------------------------------------------------------------
// a minimal reader for the fields the checker needs: "file", "entry", "hash" and the "macros" array of [name, value] string pairs
struct UberRequest {
    std::string hash, file, entry;
    std::vector<std::pair<std::string, std::string>> macros;
};

bool jsonString(const std::string& s, std::size_t& p, std::string& out) {
    if (p >= s.size() || s[p] != '"') return false;
    ++p;
    out.clear();
    while (p < s.size() && s[p] != '"') {
        if (s[p] == '\\' && p + 1 < s.size()) {
            ++p;
            switch (s[p]) {
                case 'n': out += '\n'; break;
                case 't': out += '\t'; break;
                case 'r': out += '\r'; break;
                case 'u':
                    if (p + 4 < s.size()) out += static_cast<char>(std::stoi(s.substr(p + 1, 4), nullptr, 16));
                    p += 4;
                    break;
                default: out += s[p]; break;
            }
        } else {
            out += s[p];
        }
        ++p;
    }
    ++p;
    return true;
}

bool parseUberLine(const std::string& line, UberRequest& r) {
    r = {};
    auto field = [&](const char* key, std::string& out) {
        const std::string k = std::string("\"") + key + "\":\"";
        auto at = line.find(k);
        if (at == std::string::npos) return false;
        at += k.size() - 1;
        return jsonString(line, at, out);
    };
    if (!field("file", r.file) || !field("entry", r.entry)) return false;
    field("hash", r.hash);
    auto at = line.find("\"macros\":[");
    if (at == std::string::npos) return false;
    at += 10;
    while (at < line.size() && line[at] != ']') {
        if (line[at] == '[') {
            ++at;
            std::string name, value;
            if (!jsonString(line, at, name)) return false;
            while (at < line.size() && line[at] != '"') ++at;
            if (!jsonString(line, at, value)) return false;
            while (at < line.size() && line[at] != ']') ++at;
            ++at;
            r.macros.emplace_back(name, value);
        } else {
            ++at;
        }
    }
    return true;
}

int cmdDxilUber(int argc, char** argv) {
    int jobs = defaultJobs(), limit = 1 << 30, maxErrors = 30;
    std::vector<UberRequest> reqs;
    for (int i = 3; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--jobs" && i + 1 < argc) { jobs = std::atoi(argv[++i]); continue; }
        if (a == "--limit" && i + 1 < argc) { limit = std::atoi(argv[++i]); continue; }
        if (a == "--max-errors" && i + 1 < argc) { maxErrors = std::atoi(argv[++i]); continue; }
        std::ifstream in(a);
        std::string line;
        while (std::getline(in, line)) {
            UberRequest r;
            if (parseUberLine(line, r) && rvk::shader::uberSupports(r.file, r.entry)) reqs.push_back(std::move(r));
        }
    }
    if (static_cast<int>(reqs.size()) > limit) reqs.resize(static_cast<std::size_t>(limit));
    if (!requireDxc()) return 2;
    // the hook variants of enh/material_variant.cpp, with fixed registers (any c14..c223 register is valid)
    struct Variant {
        const char* name;
        const char* defines;
    };
    static const Variant kVariants[] = {
        {"plain", ""},
        {"gbuffer", "#define RVK_MV_AMBIENT 1\n#define RVK_MV_AMB_REG 14\n#define RVK_MV_HAS_IND 1\n#define RVK_MV_IND_REG 21\n#define RVK_MV_GBUFFER 1\n#define RVK_MV_KIND_SKINNED 3\n#define RVK_MV_VNORMAL 1\n"},
        {"gbuffer+motion", "#define RVK_MV_AMBIENT 1\n#define RVK_MV_AMB_REG 14\n#define RVK_MV_HAS_IND 1\n#define RVK_MV_IND_REG 21\n#define RVK_MV_GBUFFER 1\n#define RVK_MV_MOTION 1\n#define RVK_MV_KIND_SKINNED 3\n#define RVK_MV_VNORMAL 1\n"},
        {"gi", "#define RVK_MV_AMBIENT 1\n#define RVK_MV_AMB_REG 14\n#define RVK_MV_HAS_IND 1\n#define RVK_MV_IND_REG 21\n#define RVK_MV_KIND_SKINNED 3\n"},
    };
    constexpr std::size_t kN = std::size(kVariants);
    std::vector<Item> items(reqs.size() * kN);
    parallelFor(items.size(), jobs, [&](std::size_t i) {
        const UberRequest& q = reqs[i / kN];
        const Variant& v = kVariants[i % kN];
        spvtools::SpirvTools tools(SPV_ENV_VULKAN_1_3);
        items[i].label = std::format("{} {} {}", q.hash, q.entry, v.name);
        items[i].family = std::string("uber ") + v.name;
        rvk::shader::dxc::Request r;
        r.file = "uber.hlsl";
        r.source = rvk::shader::uberPixelHlsl(q.entry, q.macros, v.defines);
        if (r.source.empty()) {
            items[i].r.spirvMsg = "uber: invalid macro name";
            return;
        }
        r.entry = "PSMain";
        r.profile = "ps_6_6";
        r.optLevel = 1;
        items[i].r = compileBoth(r, tools);
    });
    std::printf("%zu captured requests x %zu variants\n", reqs.size(), kN);
    return reportDxil(items, maxErrors, true);
}

int cmdDxilCheck(int argc, char** argv) {
    const std::string what = argv[2];
    if (what == "embedded") return cmdDxilEmbedded(argc, argv);
    if (what == "uber") return cmdDxilUber(argc, argv);
    if (what == "corpus" && argc >= 4) {
        TranslateOptions options;
        VariantFn variant = nullptr;
        int jobs = defaultJobs(), maxErrors = 30;
        for (int i = 4; i < argc; ++i) {
            const std::string a = argv[i];
            if (a == "--cluster") { rvk::enh::setClusterVariants(true); continue; }
            if (a == "--pbr") rvk::enh::setPbrVariants(true);
            else if (a == "--gi") variant = &rvk::enh::Gi::variantOptions;
            else if (a == "--gbuf") variant = [](const rvk::d3d9::Shader& sh, TranslateOptions& o) { return rvk::enh::materialVariantOptions(sh, rvk::enh::MaterialVariant::Gbuffer, o); };
            else if (a == "--gbuf-motion") variant = [](const rvk::d3d9::Shader& sh, TranslateOptions& o) { return rvk::enh::materialVariantOptions(sh, rvk::enh::MaterialVariant::GbufferMotion, o); };
            else if (a == "--capture") variant = &rvk::enh::RtScene::captureOptions;
            else if (a == "--tint") options.tintEpilogue = true;
            else if (a == "--jobs" && i + 1 < argc) jobs = std::atoi(argv[++i]);
            else if (a == "--max-errors" && i + 1 < argc) maxErrors = std::atoi(argv[++i]);
        }
        return cmdDxilCorpus(argv[3], jobs, maxErrors, options, variant);
    }
    std::printf("usage: rvk_shadertool dxil-check embedded [--filter S] | corpus <dir> [variant] | uber <jsonl>...\n");
    return 2;
}
